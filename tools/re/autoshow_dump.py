"""Frame-dump side of the Autoshow references (tools/re/autoshow-refs.py).

Finds a car's draws in an Autoshow frame dump by the bytes of its vertex
buffers, reads the car's clip matrix and the scene's, converts them to
the reference schema's convention (docs/formats/AUTOSHOW_REFERENCE.md,
"Reference files"), and lists the draws to leave out of the two replays
that make the reference image (UI left out) and the mask (UI and car left
out). Our own tooling; nothing here contains game data.
"""
from __future__ import annotations

import importlib.util
import math
import sqlite3
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CARS = ROOT / ".local/re/cars"
RB_MODECONTROL = 0x2208
PA_SC_VIZ_QUERY = 0x2293
KEY = 56            # bytes of a vertex pool compared with the draw's buffer


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


wg = _load("world_geometry", ROOT / "tools/re/world-geometry.py")
cb = _load("fh1_carbin", ROOT / "tools/fh1_carbin.py")


# --- the car's files --------------------------------------------------------

def car_folder(media: str) -> Path:
    """The extracted folder of a car: the disc's first, then the packages'
    (every duplicated copy is identical; AUTOSHOW_REFERENCE.md)."""
    base = CARS / "base" / media
    if base.is_dir():
        return base
    for folder in sorted(CARS.glob("dlc/*/*/media/cars/*")):
        if folder.name.lower() == media.lower():
            return folder
    raise FileNotFoundError(f"no extracted folder for {media}")


def wheel_folder(car_id: int) -> Path | None:
    con = sqlite3.connect(CARS / "db/merged.slt")
    row = con.execute("SELECT w.MediaName FROM Data_Car d JOIN List_Wheels w "
                      "ON w.Id = d.StockWheelID WHERE d.Id = ?", (car_id,)).fetchone()
    if not row:
        return None
    for folder in [CARS / "wheels" / row[0], *CARS.glob("dlc/*/*/media/wheels/*")]:
        if folder.is_dir() and folder.name.lower() == row[0].lower():
            return folder
    return None


def paint(car_id: int) -> tuple[list[int], int]:
    """The first colour row (lowest sequence) of the car: sRGB bytes and
    the metallic flag."""
    con = sqlite3.connect(CARS / "db/merged.slt")
    rgb, metallic = con.execute("SELECT RGB, Metallic FROM Combo_Colors WHERE Ordinal = ? "
                                "ORDER BY Sequence, ID LIMIT 1", (car_id,)).fetchone()
    if isinstance(rgb, str):
        value = int(rgb.lstrip("#"), 16)
    else:
        value = int(rgb)
    return [(value >> 16) & 255, (value >> 8) & 255, value & 255], int(metallic)


def pool_bytes(pool) -> bytes:
    if isinstance(pool.vertices, bytes):
        return pool.vertices
    return b"".join(cb.VERTEX28.pack(*v) for v in pool.vertices)


def models(folder: Path):
    for path in sorted(folder.glob("*.carbin")):
        if path.name.lower().startswith("stripped"):
            continue
        try:
            yield path, cb.parse(path.read_bytes())
        except cb.CarbinError:
            continue


def model_keys(folder: Path, kind: str) -> dict[bytes, tuple[str, str, str]]:
    """The first bytes of every vertex pool of the folder's models, to
    (kind, file, part)."""
    keys = {}
    for path, model in models(folder):
        for section in model.sections:
            for pool in (section.lod0_pool, section.lod_pool):
                if pool.count and pool.stride == 28:
                    keys[pool_bytes(pool)[:KEY]] = (kind, path.name, section.name)
    return keys


_TYRES: dict | None = None


def tyre_keys() -> dict:
    global _TYRES
    if _TYRES is None:
        _TYRES = {}
        for folder in sorted((CARS / "wheels").iterdir()):
            if folder.name.lower().startswith("tire"):
                _TYRES.update(model_keys(folder, "tyre"))
    return _TYRES


def part_label(kind: str, file: str) -> str:
    if kind != "car":
        return kind
    name = file.lower()
    for label in ("cockpit", "caliper", "rotor"):
        if f"_{label}" in name:
            return label
    return "lod0" if name.endswith("_lod0.carbin") else "exterior"


# --- the dump -----------------------------------------------------------------

def draw_modes(registers, packets) -> list[tuple[int, bool]]:
    """RB_MODECONTROL's EDRAM mode at each draw and whether the draw is a
    visibility query that kills its pixels, walking the stream as
    world-geometry.py's walk_draws does with predication."""
    regs = list(registers)
    modes = []
    bin_mask = bin_select = (1 << 64) - 1
    i = 0
    while i < len(packets):
        header = packets[i]
        kind = header >> 30
        if kind == 0:
            base, count = header & 0x7FFF, ((header >> 16) & 0x3FFF) + 1
            single = (header >> 15) & 1
            for k in range(count):
                reg = base if single else base + k
                if reg < len(regs):
                    regs[reg] = packets[i + 1 + k]
            i += 1 + count
        elif kind == 1:
            regs[header & 0x7FF] = packets[i + 1]
            regs[(header >> 11) & 0x7FF] = packets[i + 2]
            i += 3
        elif kind == 2:
            i += 1
        else:
            opcode, count = (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1
            body = packets[i + 1:i + 1 + count]
            if opcode in (0x50, 0x51):
                value = (body[0] << 32) | body[1]
                if opcode == 0x50:
                    bin_mask = value
                else:
                    bin_select = value
            elif opcode in (0x60, 0x61, 0x62, 0x63):
                shift = 32 if opcode & 1 else 0
                keep = ((1 << 64) - 1) ^ (0xFFFFFFFF << shift)
                if opcode < 0x62:
                    bin_mask = (bin_mask & keep) | (body[0] << shift)
                else:
                    bin_select = (bin_select & keep) | (body[0] << shift)
            if header & 1 and not bin_mask & bin_select:
                i += 1 + count
                continue
            if opcode in (0x22, 0x36):
                viz = regs[PA_SC_VIZ_QUERY]
                modes.append((regs[RB_MODECONTROL] & 7, bool(viz & 1 and viz & 0x80)))
            i += 1 + count
    return modes


def ui_draws(draws: list[dict]) -> list[int]:
    """The UI: the frame ends with a resolve (EDRAM copy mode 6) of the UI
    surface to the front buffer, and the UI is the triangle lists drawn
    after the resolve before it (price, colour bar, prompts, HUD)."""
    copies = [n for n, d in enumerate(draws) if d["mode"] == 6]
    if len(copies) < 2:
        return []
    return [draws[n]["executor"] for n in range(copies[-2] + 1, copies[-1])
            if draws[n]["primitive"] == 4 and draws[n]["executor"] is not None]


def analyse(dump: Path, car: dict) -> dict:
    registers, packets, blocks = wg.read_frame_dump(str(dump))
    memory = wg.GuestMemory(blocks)
    draws = wg.walk_draws(registers, packets, memory, predicate=True, constants=True)
    modes = draw_modes(registers, packets)
    if len(modes) != len(draws):
        raise RuntimeError(f"{len(modes)} modes for {len(draws)} draws")
    # The executor numbers the draws it runs (fh1_debug_skip_draws and
    # fh1_debug_log_draws count them): every walked draw but the
    # visibility queries that kill their pixels.
    number = 0
    for d, (mode, query) in zip(draws, modes):
        d["mode"] = mode
        d["executor"] = None if query else number
        number += not query
    keys = model_keys(car_folder(car["media_name"]), "car")
    wheels = wheel_folder(car["id"])
    if wheels:
        keys.update(model_keys(wheels, "rim"))
    keys.update(tyre_keys())
    tagged = {}
    for n, d in enumerate(draws):
        for _, address, _ in d["fetch"]:
            if (head := memory.read(address, KEY)) in keys:
                tagged[n] = keys[head]
                break
    body = [n for n, k in tagged.items() if part_label(k[0], k[1]) == "lod0"]
    if not body:
        raise RuntimeError(f"no draw reads {car['media_name']}'s LOD0 model")
    matrix = draws[body[0]]["mvp"]
    # The opaque parts are drawn with one matrix; the transparent list
    # (glass, lamp covers, some body and wheel sub-sections) with a matrix
    # that differs by about a pixel. Both are the car's draws.
    def near(m):
        return max(abs(a - b) for a, b in zip(m, matrix)) < 0.05
    car_draws = sorted(n for n, k in tagged.items()
                       if near(draws[n]["mvp"]) and draws[n]["primitive"] in (4, 6))
    others = Counter(draws[n]["mvp"] for n in car_draws if draws[n]["mvp"] != matrix)
    second = others.most_common(1)[0][0] if others else None
    model = model_to_body(draws[body[0]]["vs_constants"])
    kinds = Counter(part_label(*tagged[n][:2]) for n in car_draws)
    car_numbers = [draws[n]["executor"] for n in car_draws if draws[n]["executor"] is not None]
    files = sorted({tagged[n][1] for n in car_draws})
    return {"draws": draws, "tagged": tagged, "car_draws": car_draws, "car_numbers": car_numbers,
            "ui": ui_draws(draws), "executed": number,
            "matrix": matrix, "second_matrix": second, "model_to_body": model,
            "second_draws": sum(others.values()), "kinds": dict(sorted(kinds.items())), "files": files,
            "other_buffer_draws": len(set(tagged) - set(car_draws)),
            "zviewport": draws[body[0]]["zviewport"], "viewport": draws[body[0]]["viewport"],
            "vtx_cntl": draws[body[0]]["vtx_cntl"]}


def stage_numbers(analysis: dict) -> list[int]:
    """Executor numbers of the stage: every other triangle draw (lists,
    strips, quads) of the main colour pass (EDRAM colour mode, the car's
    render target), so a replay without them leaves the car on the pass's
    clear colour. Post-processing (rectangles), resolves and the UI stay."""
    draws, car = analysis["draws"], set(analysis["car_draws"])
    surface = draws[analysis["car_draws"][0]]["surface_info"]
    return [d["executor"] for n, d in enumerate(draws)
            if n not in car and d["executor"] is not None and d["surface_info"] == surface
            and d["primitive"] in (4, 5, 6, 13) and d["mode"] == 4]


# --- matrices -----------------------------------------------------------------

MODEL_TO_BODY = 8   # vertex constants c8-c11 of the car's draws


def model_to_body(words) -> list[list[float]]:
    """The model-to-body transform the car's vertex shader applies before
    the clip matrix (constants c8-c11 as the columns of a 4x4): it mirrors
    z (the model's +Z rear to the body frame's +Z forward), lifts the car
    by its ride height and tilts it by its rake. Checked: rotation part
    orthonormal with determinant -1, last row (0, 0, 0, 1)."""
    import struct
    f = struct.unpack("<16f", struct.pack("<16I", *words[4 * MODEL_TO_BODY:4 * MODEL_TO_BODY + 16]))
    m = [[f[4 * c + r] for c in range(4)] for r in range(4)]
    rot = [row[:3] for row in m[:3]]
    if not (abs(det3(rot) + 1) < 1e-2 and all(abs(v) < 1e-4 for v in m[3][:3])
            and abs(m[3][3] - 1) < 1e-4):
        raise RuntimeError(f"c8-c11 is not a mirrored rigid transform: {m}")
    return m

def rows(game) -> list[list[float]]:
    return [list(game[4 * r:4 * r + 4]) for r in range(4)]


def to_std(game, width: int, height: int) -> list[list[float]]:
    """The game's clip matrix (rows clip x, y, z, w) in the schema's
    convention: x and y negated (the game draws the view rotated by a half
    turn) and the Direct3D 9 half pixel removed (clip x - w / width, clip
    y + w / height); z kept (z/w is already 0 near, 1 far)."""
    g = rows(game)
    w = g[3]
    return [[-a - b / width for a, b in zip(g[0], w)],
            [-a + b / height for a, b in zip(g[1], w)], g[2], w]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def column_major(m) -> list[float]:
    return [round(float(m[r][c]), 9) for c in range(4) for r in range(4)]


def dot3(a, b) -> float:
    return sum(x * y for x, y in zip(a[:3], b[:3]))


def det3(m) -> float:
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def decompose(clip):
    """proj x view of a world-to-clip matrix: view with rows right, up and
    forward (forward is the w row, normalised); proj with w = view z,
    z = view z - near, and x, y scales and centre terms. Returns (proj,
    view, near, largest difference of proj x view from clip)."""
    x, y, z, w = clip
    norm = math.sqrt(dot3(w, w))
    forward = [v / norm for v in w]
    view = []
    proj = [[0.0] * 4 for _ in range(4)]
    for k, row in enumerate((x, y)):
        c = dot3(row, forward)
        rest = [a - c * b for a, b in zip(row, forward)]
        s = math.sqrt(dot3(rest, rest))
        view.append([v / s for v in rest])
        proj[k][k] = s
        proj[k][2] = c
    near = (w[3] - z[3]) / norm
    proj[2][2], proj[2][3], proj[3][2] = 1.0, -near, 1.0
    view += [forward, [0.0, 0.0, 0.0, 1.0]]
    rebuilt = mat_mul(proj, view)
    residual = max(abs(rebuilt[r][c] - clip[r][c]) for r in range(4) for c in range(4))
    return proj, view, near, residual


def scene_matrix(draws, matrix):
    """The world-to-clip matrix of the stage: the most frequent other draw
    matrix that equals the car's times a proper rigid transform (the
    z-mirrored file-frame matrix of WORLD_GEOMETRY.md is rejected by the
    determinant). Returns (scene, car_to_world rows) or (None, None)."""
    counts = Counter(d["mvp"] for d in draws if d["mvp"] != matrix)
    for scene, _ in counts.most_common(60):
        if not any(abs(scene[4 * r + 3]) > 1e-3 for r in range(4)):
            continue        # camera-relative (sky and cube) matrices
        try:
            rotation, translation = wg.placement_xyw(scene, matrix)
        except ZeroDivisionError:
            continue
        if all(abs(dot3(rotation[i], rotation[j]) - (i == j)) < 2e-3
               for i in range(3) for j in range(3)) and det3(rotation) > 0:
            car_to_world = [rotation[r] + [translation[r]] for r in range(3)]
            return scene, car_to_world + [[0.0, 0.0, 0.0, 1.0]]
    return None, None


def matrices(analysis: dict, width: int, height: int) -> dict:
    """Matrices of the schema. The game's clip matrix maps the body frame;
    the model-to-body transform is folded in, so clip_from_car takes
    FORMAT-CARS car space."""
    game = analysis["matrix"]
    model = analysis["model_to_body"]
    clip = mat_mul(to_std(game, width, height), model)
    scene, body_to_world = scene_matrix(analysis["draws"], game)
    if scene is None:
        body_to_world = [[float(r == c) for c in range(4)] for r in range(4)]
        world = to_std(game, width, height)
    else:
        world = to_std(scene, width, height)
    car_to_world = mat_mul(body_to_world, model)
    proj, view, near, residual = decompose(world)
    rebuilt = mat_mul(mat_mul(proj, view), car_to_world)
    chain = max(abs(rebuilt[r][c] - clip[r][c]) for r in range(4) for c in range(4))
    car_near = (clip[3][3] - clip[2][3])
    z_is_w_minus_near = max(abs(clip[2][c] - clip[3][c]) for c in range(3))
    second = analysis.get("second_matrix")
    shift = None
    if second:
        # largest screen offset between the two matrices over the car's box
        a, b = to_std(game, width, height), to_std(second, width, height)
        shift = 0.0
        for x in (-1.0, 1.0):
            for y in (0.0, 1.5):
                for z in (-2.5, 2.5):
                    p = [sum(model[r][c] * v for c, v in enumerate((x, y, z, 1.0))) for r in range(4)]
                    pa = [sum(a[r][c] * p[c] for c in range(4)) for r in range(4)]
                    pb = [sum(b[r][c] * p[c] for c in range(4)) for r in range(4)]
                    dx = (pa[0] / pa[3] - pb[0] / pb[3]) * width / 2
                    dy = (pa[1] / pa[3] - pb[1] / pb[3]) * height / 2
                    shift = max(shift, math.hypot(dx, dy))
    return {
        "clip_from_car": column_major(clip),
        "view": column_major(view),
        "proj": column_major(proj),
        "car_to_world": column_major(car_to_world),
        "near": round(near, 6),
        "game_clip_from_car": [round(v, 9) for v in game],
        "checks": {"world_matrix_found": scene is not None,
                   "decompose_residual": residual, "chain_residual": chain,
                   "z_row_minus_w_row_xyz": z_is_w_minus_near,
                   "near_from_car_matrix": car_near,
                   "model_to_body": column_major(model),
                   "transparent_pass_matrix_shift_px": shift,
                   "transparent_clip_from_car": (column_major(mat_mul(to_std(second, width, height), model))
                                                 if second else None),
                   "view_determinant": det3(view),
                   "car_to_world_determinant": det3(car_to_world),
                   "car_to_world_translation": [car_to_world[r][3] for r in range(3)]},
    }


# --- projection check ---------------------------------------------------------

def triangles(model, lod: int = 0):
    """Car-space triangles of a model's sub-sections at one LOD."""
    for section in model.sections:
        pool = section.lod0_pool if lod == 0 else section.lod_pool
        if not pool.count or pool.stride != 28:
            continue
        points = cb.positions(section, pool)
        for sub in section.subsections:
            if sub.lod != lod:
                continue
            index = sub.indices
            if sub.index_type == 4:
                for k in range(0, len(index) - 2, 3):
                    yield points[index[k]], points[index[k + 1]], points[index[k + 2]]
                continue
            restart = 0xFFFF if sub.index_size == 2 else 0x00FFFFFF
            run: list[int] = []
            for i in index + [restart]:
                if i == restart or i >= len(points):
                    for k in range(len(run) - 2):
                        yield points[run[k]], points[run[k + 1]], points[run[k + 2]]
                    run = []
                else:
                    run.append(i)


def rasterise(tris, clip, width: int, height: int) -> bytearray:
    """Coverage at pixel centres of the projected triangles (both
    windings), with the schema's pixel mapping."""
    mask = bytearray(width * height)
    m = clip
    for tri in tris:
        pts = []
        for x, y, z in tri:
            cw = m[3][0] * x + m[3][1] * y + m[3][2] * z + m[3][3]
            if cw <= 1e-4:
                pts = None
                break
            cx = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3]
            cy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3]
            pts.append(((cx / cw + 1) * 0.5 * width, (1 - cy / cw) * 0.5 * height))
        if not pts:
            continue
        (x0, y0), (x1, y1), (x2, y2) = pts
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        lo_x = max(0, math.floor(min(x0, x1, x2) - 0.5))
        hi_x = min(width - 1, math.ceil(max(x0, x1, x2) - 0.5))
        lo_y = max(0, math.floor(min(y0, y1, y2) - 0.5))
        hi_y = min(height - 1, math.ceil(max(y0, y1, y2) - 0.5))
        for py in range(lo_y, hi_y + 1):
            sy = py + 0.5
            for px in range(lo_x, hi_x + 1):
                sx = px + 0.5
                w0 = (x1 - sx) * (y2 - sy) - (x2 - sx) * (y1 - sy)
                w1 = (x2 - sx) * (y0 - sy) - (x0 - sx) * (y2 - sy)
                w2 = (x0 - sx) * (y1 - sy) - (x1 - sx) * (y0 - sy)
                if (w0 >= 0 and w1 >= 0 and w2 >= 0) or (w0 <= 0 and w1 <= 0 and w2 <= 0):
                    mask[py * width + px] = 255
    return mask


def iou(a: bytes, b: bytes) -> float:
    inter = sum(1 for x, y in zip(a, b) if x and y)
    union = sum(1 for x, y in zip(a, b) if x or y)
    return inter / union if union else 0.0
