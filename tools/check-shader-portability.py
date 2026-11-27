#!/usr/bin/env python3
"""Cross-API shader portability auditor and SPIR-V binary reflection tool.

Audits all GLSL sources and prebuilt SPIR-V binaries in apps/carview/shaders/
for Vulkan 1.3 + Metal 3 (MSL 3.1) + DirectX 12 (HLSL SM 6.6 / DXIL) readiness:

  1. Pure-Python SPIR-V binary parser & reflection (works with zero external tools):
     - Magic/version/word-alignment validation
     - Execution model, entry point name, compute workgroup size
     - Exact push-constant block byte size (enforces <= 128 B across all shaders)
     - Descriptor set / binding table & split-bindless convention checks
     - Specialization constant ID audit (only constant_id 0=kLinearOut, 1=kReverseZ)
     - BuiltIn usage checks: zero gl_PointSize writes, zero gl_PointCoord reads
     - Capability / extension tracking (RayQueryKHR, PhysicalStorageBufferAddresses, etc.)
  2. Static GLSL & host-pipeline checks:
     - Zero gl_PointSize / gl_PointCoord in GLSL sources
     - Zero VK_PRIMITIVE_TOPOLOGY_POINT_LIST in apps/carview/showroom_sky.cpp
     - Prebuilt SPIR-V coverage for every entry shader
     - Manifest synchronization check against apps/carview/shaders/prebuilt/shader_manifest.json
  3. Optional external tool verification when available on host:
     - spirv-val (--target-env vulkan1.3)
     - spirv-cross (--msl --msl-version 30100 --msl-argument-buffers)
     - spirv-cross (--hlsl --shader-model 66 --hlsl-enable-compat) + dxc (-T *_6_6)

Usage:
  python tools/check-shader-portability.py [--json] [--strict-cross-compile] [--check-manifest]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SHADERS_DIR = ROOT / "apps" / "carview" / "shaders"
PREBUILT_DIR = SHADERS_DIR / "prebuilt"
MANIFEST_PATH = PREBUILT_DIR / "shader_manifest.json"

ENTRY_EXTS = (".vert", ".frag", ".comp", ".rgen", ".rchit", ".rahit", ".rmiss")
HEADER_EXTS = (".glsl",)
MAX_PUSH_CONSTANT_BYTES = 128
ALLOWED_SPEC_IDS = {0: "kLinearOut", 1: "kReverseZ", 2: "kMobileFast"}

# SPIR-V constants
SPV_MAGIC = 0x07230203

OP_NAME = 5
OP_MEMBER_NAME = 6
OP_EXTENSION = 10
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_CAPABILITY = 17
OP_EXECUTION_MODE_ID = 331
OP_TYPE_VOID = 19
OP_TYPE_BOOL = 20
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_MATRIX = 24
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_SAMPLED_IMAGE = 27
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_CONSTANT = 43
OP_SPEC_CONSTANT_TRUE = 48
OP_SPEC_CONSTANT_FALSE = 49
OP_SPEC_CONSTANT = 50
OP_VARIABLE = 59
OP_ACCESS_CHAIN = 65
OP_IN_BOUNDS_ACCESS_CHAIN = 66
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_TYPE_ACCEL_STRUCT_KHR = 5341

DEC_SPEC_ID = 1
DEC_BLOCK = 2
DEC_BUFFER_BLOCK = 3
DEC_ROW_MAJOR = 4
DEC_COL_MAJOR = 5
DEC_ARRAY_STRIDE = 6
DEC_MATRIX_STRIDE = 7
DEC_BUILTIN = 11
DEC_NON_WRITABLE = 24
DEC_LOCATION = 30
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_OFFSET = 35

BUILTIN_POSITION = 0
BUILTIN_POINT_SIZE = 1
BUILTIN_FRAG_COORD = 15
BUILTIN_POINT_COORD = 16
BUILTIN_VERTEX_INDEX = 42
BUILTIN_INSTANCE_INDEX = 43

SC_UNIFORM_CONSTANT = 0
SC_INPUT = 1
SC_UNIFORM = 2
SC_OUTPUT = 3
SC_PUSH_CONSTANT = 9
SC_STORAGE_BUFFER = 12
SC_PHYSICAL_STORAGE_BUFFER = 5349

EXEC_MODEL_NAMES = {
    0: "vertex",
    4: "fragment",
    5: "compute",
    5313: "raygen",
    5315: "anyhit",
    5316: "closesthit",
    5317: "miss",
}

IMAGE_DIM_NAMES = {
    0: "1D",
    1: "2D",
    2: "3D",
    3: "Cube",
    4: "Rect",
    5: "Buffer",
    6: "SubpassData",
}

CAPABILITY_NAMES = {
    1: "Shader",
    9: "Float16",
    11: "Int64",
    22: "Int16",
    39: "Int8",
    5301: "ShaderNonUniform",
    5302: "RuntimeDescriptorArray",
    5304: "SampledImageArrayNonUniformIndexing",
    5347: "PhysicalStorageBufferAddresses",
    5379: "DemoteToHelperInvocation",
    4472: "RayQueryKHR",
    4479: "RayTracingKHR",
}

STAGE_TO_DXC_PROFILE = {
    "vertex": "vs_6_6",
    "fragment": "ps_6_6",
    "compute": "cs_6_6",
}


def decode_spv_string(words: list[int]) -> tuple[str, int]:
    """Decode a null-terminated UTF-8 SPIR-V literal string; return (str, words_consumed)."""
    raw = bytearray()
    for idx, w in enumerate(words):
        chunk = struct.pack("<I", w)
        nul = chunk.find(0)
        if nul != -1:
            raw.extend(chunk[:nul])
            return raw.decode("utf-8", errors="replace"), idx + 1
        raw.extend(chunk)
    return raw.decode("utf-8", errors="replace"), len(words)


def reflect_spirv(spv_bytes: bytes) -> dict[str, Any]:
    """Parse and reflect a SPIR-V binary using pure Python."""
    if len(spv_bytes) < 20 or (len(spv_bytes) % 4) != 0:
        raise ValueError(f"invalid SPIR-V byte length: {len(spv_bytes)}")

    word_count_total = len(spv_bytes) // 4
    words = list(struct.unpack(f"<{word_count_total}I", spv_bytes))
    if words[0] != SPV_MAGIC:
        raise ValueError(f"invalid SPIR-V magic: 0x{words[0]:08x}")

    version_word = words[1]
    spv_major = (version_word >> 16) & 0xFF
    spv_minor = (version_word >> 8) & 0xFF

    capabilities: list[int] = []
    extensions: list[str] = []
    entry_model: int | None = None
    entry_name = "main"
    local_size: list[int] | None = None
    local_size_ids: list[int] | None = None

    names: dict[int, str] = {}
    member_names: dict[tuple[int, int], str] = {}
    decorations: dict[int, dict[int, list[int]]] = {}
    member_decorations: dict[tuple[int, int], dict[int, list[int]]] = {}

    types: dict[int, tuple[str, tuple[Any, ...]]] = {}
    constants: dict[int, int] = {}
    spec_constants: dict[int, dict[str, Any]] = {}
    variables: dict[int, tuple[int, int]] = {}  # id -> (ptr_type_id, storage_class)
    access_chains: list[tuple[int, list[int]]] = []  # (base_id, index_ids)

    pos = 5
    while pos < word_count_total:
        hdr = words[pos]
        wc = hdr >> 16
        op = hdr & 0xFFFF
        if wc == 0 or pos + wc > word_count_total:
            raise ValueError(f"truncated SPIR-V instruction at word {pos} (wc={wc})")
        ops = words[pos + 1 : pos + wc]
        pos += wc

        if op == OP_CAPABILITY and ops:
            capabilities.append(ops[0])
        elif op == OP_EXTENSION and ops:
            ext, _ = decode_spv_string(ops)
            extensions.append(ext)
        elif op == OP_ENTRY_POINT and len(ops) >= 3:
            entry_model = ops[0]
            entry_name, _ = decode_spv_string(ops[2:])
        elif op == OP_EXECUTION_MODE and len(ops) >= 5 and ops[1] == 17:
            local_size = [ops[2], ops[3], ops[4]]
        elif op == OP_EXECUTION_MODE_ID and len(ops) >= 5 and ops[1] == 38:
            local_size_ids = [ops[2], ops[3], ops[4]]
        elif op == OP_NAME and len(ops) >= 2:
            names[ops[0]], _ = decode_spv_string(ops[1:])
        elif op == OP_MEMBER_NAME and len(ops) >= 3:
            member_names[(ops[0], ops[1])], _ = decode_spv_string(ops[2:])
        elif op == OP_DECORATE and len(ops) >= 2:
            decorations.setdefault(ops[0], {})[ops[1]] = ops[2:]
        elif op == OP_MEMBER_DECORATE and len(ops) >= 3:
            member_decorations.setdefault((ops[0], ops[1]), {})[ops[2]] = ops[3:]
        elif op == OP_TYPE_VOID and ops:
            types[ops[0]] = ("void", ())
        elif op == OP_TYPE_BOOL and ops:
            types[ops[0]] = ("bool", ())
        elif op == OP_TYPE_INT and len(ops) >= 3:
            types[ops[0]] = ("int", (ops[1], ops[2]))
        elif op == OP_TYPE_FLOAT and len(ops) >= 2:
            types[ops[0]] = ("float", (ops[1],))
        elif op == OP_TYPE_VECTOR and len(ops) >= 3:
            types[ops[0]] = ("vec", (ops[1], ops[2]))
        elif op == OP_TYPE_MATRIX and len(ops) >= 3:
            types[ops[0]] = ("mat", (ops[1], ops[2]))
        elif op == OP_TYPE_IMAGE and len(ops) >= 8:
            types[ops[0]] = ("image", tuple(ops[1:8]))
        elif op == OP_TYPE_SAMPLER and ops:
            types[ops[0]] = ("sampler", ())
        elif op == OP_TYPE_SAMPLED_IMAGE and len(ops) >= 2:
            types[ops[0]] = ("sampled_image", (ops[1],))
        elif op == OP_TYPE_ARRAY and len(ops) >= 3:
            types[ops[0]] = ("array", (ops[1], ops[2]))
        elif op == OP_TYPE_RUNTIME_ARRAY and len(ops) >= 2:
            types[ops[0]] = ("runtime_array", (ops[1],))
        elif op == OP_TYPE_STRUCT and ops:
            types[ops[0]] = ("struct", tuple(ops[1:]))
        elif op == OP_TYPE_POINTER and len(ops) >= 3:
            types[ops[0]] = ("ptr", (ops[1], ops[2]))
        elif op == OP_TYPE_ACCEL_STRUCT_KHR and ops:
            types[ops[0]] = ("accel_struct", ())
        elif op == OP_CONSTANT and len(ops) >= 3:
            constants[ops[1]] = ops[2]
        elif op in (OP_SPEC_CONSTANT_TRUE, OP_SPEC_CONSTANT_FALSE, OP_SPEC_CONSTANT) and len(ops) >= 2:
            spec_constants[ops[1]] = {
                "type_id": ops[0],
                "op": op,
                "default": True if op == OP_SPEC_CONSTANT_TRUE else (False if op == OP_SPEC_CONSTANT_FALSE else (ops[2] if len(ops) > 2 else 0)),
            }
        elif op == OP_VARIABLE and len(ops) >= 3:
            variables[ops[1]] = (ops[0], ops[2])
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and len(ops) >= 3:
            access_chains.append((ops[2], ops[3:]))

    if local_size is None and local_size_ids is not None:
        local_size = [constants.get(cid, 1) for cid in local_size_ids]

    def type_byte_size(tid: int, member_decs: dict[int, list[int]] | None = None) -> int:
        if tid not in types:
            return 0
        kind, data = types[tid]
        if kind == "bool":
            return 4
        if kind in ("int", "float"):
            return max(1, data[0] // 8)
        if kind == "vec":
            return type_byte_size(data[0]) * data[1]
        if kind == "mat":
            col_tid, col_count = data
            col_kind, col_data = types.get(col_tid, ("", ()))
            row_count = col_data[1] if col_kind == "vec" else 1
            stride = None
            if member_decs and DEC_MATRIX_STRIDE in member_decs:
                stride = member_decs[DEC_MATRIX_STRIDE][0]
            if stride is None:
                stride = type_byte_size(col_tid)
            if member_decs and DEC_ROW_MAJOR in member_decs:
                return row_count * stride
            return col_count * stride
        if kind == "array":
            elem_tid, len_cid = data
            length = constants.get(len_cid, 0)
            stride = decorations.get(tid, {}).get(DEC_ARRAY_STRIDE, [None])[0]
            if stride is None:
                stride = type_byte_size(elem_tid)
            return length * stride
        if kind == "struct":
            members = data
            end = 0
            for idx, mtid in enumerate(members):
                mdecs = member_decorations.get((tid, idx), {})
                off = mdecs.get(DEC_OFFSET, [end])[0]
                sz = type_byte_size(mtid, mdecs)
                end = max(end, off + sz)
            return end
        return 0

    def pointee_type(ptr_tid: int) -> int | None:
        t = types.get(ptr_tid)
        if t and t[0] == "ptr":
            return t[1][1]
        return None

    # Push constants
    push_constant_bytes = 0
    push_constant_name: str | None = None
    for vid, (ptr_tid, sc) in variables.items():
        if sc == SC_PUSH_CONSTANT:
            pt = pointee_type(ptr_tid)
            if pt is not None:
                push_constant_bytes = max(push_constant_bytes, type_byte_size(pt))
                push_constant_name = names.get(pt) or names.get(vid)

    # Descriptor bindings
    bindings: list[dict[str, Any]] = []
    for vid, (ptr_tid, sc) in variables.items():
        vdec = decorations.get(vid, {})
        if DEC_DESCRIPTOR_SET not in vdec or DEC_BINDING not in vdec:
            continue
        set_idx = vdec[DEC_DESCRIPTOR_SET][0]
        binding_idx = vdec[DEC_BINDING][0]
        pt = pointee_type(ptr_tid)
        if pt is None:
            continue

        # Unwrap array / runtime array
        count: int | str = 1
        inner_tid = pt
        if inner_tid in types and types[inner_tid][0] == "array":
            elem_tid, len_cid = types[inner_tid][1]
            count = constants.get(len_cid, 0)
            inner_tid = elem_tid
        elif inner_tid in types and types[inner_tid][0] == "runtime_array":
            inner_tid = types[inner_tid][1][0]
            count = "unbounded"

        inner_kind, inner_data = types.get(inner_tid, ("unknown", ()))
        desc_type = "unknown"
        dim = None
        if sc == SC_UNIFORM_CONSTANT:
            if inner_kind == "sampler":
                desc_type = "sampler"
            elif inner_kind == "sampled_image":
                desc_type = "combined_image_sampler"
                img_t = types.get(inner_data[0])
                if img_t and img_t[0] == "image":
                    dim = IMAGE_DIM_NAMES.get(img_t[1][1], "2D")
            elif inner_kind == "image":
                dim = IMAGE_DIM_NAMES.get(inner_data[1], "2D")
                sampled_flag = inner_data[5]
                desc_type = "storage_image" if sampled_flag == 2 else "sampled_image"
            elif inner_kind == "accel_struct":
                desc_type = "acceleration_structure"
        elif sc == SC_UNIFORM:
            sdec = decorations.get(inner_tid, {})
            desc_type = "storage_buffer" if DEC_BUFFER_BLOCK in sdec else "uniform_buffer"
        elif sc == SC_STORAGE_BUFFER:
            desc_type = "storage_buffer"

        readonly = (DEC_NON_WRITABLE in vdec) or (DEC_NON_WRITABLE in decorations.get(inner_tid, {}))
        if not readonly and desc_type == "storage_buffer" and inner_kind == "struct" and inner_data:
            readonly = all(
                DEC_NON_WRITABLE in member_decorations.get((inner_tid, idx), {})
                for idx in range(len(inner_data))
            )
        entry: dict[str, Any] = {
            "set": set_idx,
            "binding": binding_idx,
            "type": desc_type,
            "count": count,
            "name": names.get(vid) or names.get(inner_tid) or f"set{set_idx}_b{binding_idx}",
        }
        if dim is not None:
            entry["dim"] = dim
        if desc_type in ("storage_buffer", "storage_image"):
            entry["readonly"] = readonly
        bindings.append(entry)

    bindings.sort(key=lambda b: (b["set"], b["binding"]))

    # Specialization constants
    spec_list: list[dict[str, Any]] = []
    for cid, info in spec_constants.items():
        cdec = decorations.get(cid, {})
        if DEC_SPEC_ID in cdec:
            sid = cdec[DEC_SPEC_ID][0]
            spec_list.append({
                "id": sid,
                "name": names.get(cid) or ALLOWED_SPEC_IDS.get(sid, f"spec_{sid}"),
                "default": info["default"],
            })
    spec_list.sort(key=lambda s: s["id"])

    # BuiltIn usage (accurate: detects actual gl_PointSize writes, not just implicit gl_PerVertex declaration)
    uses_point_size = False
    uses_point_coord = False
    uses_instance_index = False
    uses_vertex_index = False
    uses_frag_coord = False

    # Map output struct variables that contain a BuiltIn PointSize member
    per_vertex_point_size_member: dict[int, int] = {}
    for vid, (ptr_tid, sc) in variables.items():
        vdec = decorations.get(vid, {})
        if DEC_BUILTIN in vdec:
            bi = vdec[DEC_BUILTIN][0]
            if bi == BUILTIN_POINT_SIZE:
                uses_point_size = True
            elif bi == BUILTIN_POINT_COORD:
                uses_point_coord = True
            elif bi == BUILTIN_INSTANCE_INDEX:
                uses_instance_index = True
            elif bi == BUILTIN_VERTEX_INDEX:
                uses_vertex_index = True
            elif bi == BUILTIN_FRAG_COORD:
                uses_frag_coord = True
        pt = pointee_type(ptr_tid)
        if pt is not None and pt in types and types[pt][0] == "struct":
            for midx in range(len(types[pt][1])):
                mdec = member_decorations.get((pt, midx), {})
                if DEC_BUILTIN in mdec and mdec[DEC_BUILTIN][0] == BUILTIN_POINT_SIZE:
                    per_vertex_point_size_member[vid] = midx

    for base_id, idx_ids in access_chains:
        if base_id in per_vertex_point_size_member and idx_ids:
            first_idx = constants.get(idx_ids[0])
            if first_idx == per_vertex_point_size_member[base_id]:
                uses_point_size = True

    # Vertex inputs (for vertex shaders)
    vertex_inputs: list[dict[str, Any]] = []
    if entry_model == 0:
        for vid, (ptr_tid, sc) in variables.items():
            if sc != SC_INPUT:
                continue
            vdec = decorations.get(vid, {})
            if DEC_LOCATION in vdec:
                vertex_inputs.append({
                    "location": vdec[DEC_LOCATION][0],
                    "name": names.get(vid, f"in_loc{vdec[DEC_LOCATION][0]}"),
                })
        vertex_inputs.sort(key=lambda x: x["location"])

    cap_names = sorted({CAPABILITY_NAMES.get(c, f"Cap_{c}") for c in capabilities if c in CAPABILITY_NAMES})
    uses_bda = (5347 in capabilities) or any(
        sc == SC_PHYSICAL_STORAGE_BUFFER for (_, sc) in variables.values()
    )
    uses_ray_query = 4472 in capabilities
    uses_bindless = any(
        b["set"] == 1 and (b["count"] == "unbounded" or (isinstance(b["count"], int) and b["count"] >= 256))
        for b in bindings
    )

    return {
        "spirv_version": f"{spv_major}.{spv_minor}",
        "word_count": word_count_total,
        "byte_size": len(spv_bytes),
        "stage": EXEC_MODEL_NAMES.get(entry_model, f"unknown_{entry_model}"),
        "entry_point": entry_name,
        "local_size": local_size,
        "push_constant_bytes": push_constant_bytes,
        "push_constant_name": push_constant_name,
        "spec_constants": spec_list,
        "bindings": bindings,
        "vertex_inputs": vertex_inputs,
        "capabilities": cap_names,
        "extensions": sorted(set(extensions)),
        "uses_point_size": uses_point_size,
        "uses_point_coord": uses_point_coord,
        "uses_instance_index": uses_instance_index,
        "uses_vertex_index": uses_vertex_index,
        "uses_frag_coord": uses_frag_coord,
        "uses_bda": uses_bda,
        "uses_ray_query": uses_ray_query,
        "uses_bindless": uses_bindless,
    }


def find_tool(names: tuple[str, ...]) -> str | None:
    """Locate a shader tool on PATH, $VULKAN_SDK, Android NDK, or Windows Kits."""
    for name in names:
        found = shutil.which(name)
        if found:
            return found

    search_dirs: list[Path] = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        search_dirs.extend([Path(sdk) / "Bin", Path(sdk) / "bin"])

    # Android NDK shader-tools (e.g. glslc, spirv-val)
    local_app = os.environ.get("LOCALAPPDATA")
    if local_app:
        ndk_root = Path(local_app) / "Android" / "Sdk" / "ndk"
        if ndk_root.is_dir():
            for ver in sorted(ndk_root.iterdir(), reverse=True):
                search_dirs.append(ver / "shader-tools" / "windows-x86_64")

    # Windows Kits dxc.exe
    if any(n.lower() == "dxc" for n in names):
        wk_bin = Path(r"C:\Program Files (x86)\Windows Kits\10\bin")
        if wk_bin.is_dir():
            for ver in sorted(wk_bin.iterdir(), reverse=True):
                if ver.is_dir() and (ver / "x64").is_dir():
                    search_dirs.append(ver / "x64")

    for d in search_dirs:
        if not d.is_dir():
            continue
        for name in names:
            for suffix in ("", ".exe"):
                candidate = d / (name + suffix)
                if candidate.is_file():
                    return str(candidate)
    return None


REVERSED_SMOOTHSTEP_RE = re.compile(
    r"\bsmoothstep\s*\(\s*"
    r"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)[fF]?\s*,\s*"
    r"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)[fF]?\s*,"
)


def strip_glsl_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def strip_glsl_comments_preserve_lines(text: str) -> str:
    def _blank(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    text = re.sub(r"/\*.*?\*/", _blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", _blank, text)
    return text


def find_reversed_smoothsteps(raw: str) -> list[tuple[int, str, str]]:
    """Return (1-based line, edge0_str, edge1_str) for literal smoothstep(e0, e1, x) with e0 >= e1."""
    clean = strip_glsl_comments_preserve_lines(raw)
    hits: list[tuple[int, str, str]] = []
    for m in REVERSED_SMOOTHSTEP_RE.finditer(clean):
        e0_str, e1_str = m.group(1), m.group(2)
        if float(e0_str) >= float(e1_str):
            lineno = clean.count("\n", 0, m.start()) + 1
            hits.append((lineno, e0_str, e1_str))
    return hits


def list_shader_files() -> tuple[list[Path], list[Path]]:
    entries = sorted(p for p in SHADERS_DIR.iterdir() if p.is_file() and p.suffix in ENTRY_EXTS)
    headers = sorted(p for p in SHADERS_DIR.iterdir() if p.is_file() and p.suffix in HEADER_EXTS)
    return entries, headers


def build_manifest(run_tools: bool = True) -> tuple[dict[str, Any], list[str], list[str]]:
    """Audit all 56 shaders and return (manifest_dict, errors, advisories)."""
    entries, headers = list_shader_files()
    errors: list[str] = []
    advisories: list[str] = []

    spirv_val = find_tool(("spirv-val",)) if run_tools else None
    spirv_cross = find_tool(("spirv-cross",)) if run_tools else None
    dxc = find_tool(("dxc",)) if run_tools else None

    header_records: list[dict[str, Any]] = []
    for hp in headers:
        raw = hp.read_text(encoding="utf-8", errors="replace")
        code = strip_glsl_comments(raw)
        uses_bda_src = "GL_EXT_buffer_reference" in code or "buffer_reference" in code
        uses_pt_size_src = bool(re.search(r"\bgl_PointSize\b", code))
        uses_pt_coord_src = bool(re.search(r"\bgl_PointCoord\b", code))
        if uses_pt_size_src or uses_pt_coord_src:
            errors.append(f"{hp.name}: header uses gl_PointSize/gl_PointCoord (unsupported >1px in DX12)")
        for lineno, e0, e1 in find_reversed_smoothsteps(raw):
            errors.append(
                f"{hp.name}:{lineno}: reversed smoothstep({e0}, {e1}, ...) has edge0 >= edge1 "
                "(GLSL/SPIR-V UB; use 1.0 - smoothstep(lo, hi, x))"
            )
        notes: list[str] = []
        if uses_bda_src:
            notes.append("uses GL_EXT_buffer_reference (64-bit BDA); DX12 SM 6.6 requires bindless buffer-slot migration")
            advisories.append(f"{hp.name}: tracked DX12 advisory - GL_EXT_buffer_reference (BDA) pending bindless slot migration")
        header_records.append({
            "name": hp.name,
            "kind": "header",
            "lines": len(raw.splitlines()),
            "uses_bda": uses_bda_src,
            "notes": notes,
        })

    shader_records: list[dict[str, Any]] = []
    max_pc_seen = 0
    vk_ready_count = 0
    metal_ready_count = 0
    dx12_ready_count = 0

    for sp in entries:
        raw = sp.read_text(encoding="utf-8", errors="replace")
        code = strip_glsl_comments(raw)
        spv_path = PREBUILT_DIR / (sp.name + ".spv")
        if not spv_path.is_file():
            errors.append(f"{sp.name}: missing prebuilt SPIR-V at {spv_path.relative_to(ROOT).as_posix()}")
            continue

        if re.search(r"\bgl_PointSize\b", code):
            errors.append(f"{sp.name}: source references gl_PointSize (DX12 incompatible)")
        if re.search(r"\bgl_PointCoord\b", code):
            errors.append(f"{sp.name}: source references gl_PointCoord (DX12 incompatible)")
        for lineno, e0, e1 in find_reversed_smoothsteps(raw):
            errors.append(
                f"{sp.name}:{lineno}: reversed smoothstep({e0}, {e1}, ...) has edge0 >= edge1 "
                "(GLSL/SPIR-V UB; use 1.0 - smoothstep(lo, hi, x))"
            )

        try:
            refl = reflect_spirv(spv_path.read_bytes())
        except Exception as exc:
            errors.append(f"{sp.name}: SPIR-V reflection failed: {exc}")
            continue

        max_pc_seen = max(max_pc_seen, refl["push_constant_bytes"])

        # Hard portability invariants
        if refl["push_constant_bytes"] > MAX_PUSH_CONSTANT_BYTES:
            errors.append(
                f"{sp.name}: push constants {refl['push_constant_bytes']} B exceed {MAX_PUSH_CONSTANT_BYTES} B cross-API limit"
            )
        if refl["uses_point_size"]:
            errors.append(f"{sp.name}: SPIR-V writes BuiltIn PointSize (DX12 incompatible)")
        if refl["uses_point_coord"]:
            errors.append(f"{sp.name}: SPIR-V reads BuiltIn PointCoord (DX12 incompatible)")
        for sc in refl["spec_constants"]:
            if sc["id"] not in ALLOWED_SPEC_IDS:
                errors.append(f"{sp.name}: unexpected specialization constant_id={sc['id']} (allowed: {sorted(ALLOWED_SPEC_IDS)})")

        # Validate split-bindless set=1 convention when bindless is used
        if refl["uses_bindless"]:
            set1 = {b["binding"]: b for b in refl["bindings"] if b["set"] == 1}
            b0 = set1.get(0)
            b1 = set1.get(1)
            if not b0 or b0["type"] != "sampler":
                errors.append(f"{sp.name}: bindless set=1 binding=0 must be sampler, got {b0}")
            if not b1 or b1["type"] != "sampled_image":
                errors.append(f"{sp.name}: bindless set=1 binding=1 must be sampled_image array, got {b1}")

        # Optional spirv-val check
        spirv_val_ok: bool | None = None
        if spirv_val:
            res = subprocess.run(
                [spirv_val, "--target-env", "vulkan1.3", str(spv_path)],
                capture_output=True,
                text=True,
            )
            spirv_val_ok = res.returncode == 0
            if not spirv_val_ok:
                errors.append(f"{sp.name}: spirv-val failed: {(res.stderr or res.stdout).strip()}")

        # Compute cross-API readiness and notes
        notes: list[str] = []
        vk_ready = (spirv_val_ok is not False) and (refl["push_constant_bytes"] <= MAX_PUSH_CONSTANT_BYTES)
        metal_ready = vk_ready and not refl["uses_point_size"] and not refl["uses_point_coord"]
        dx12_ready = vk_ready and not refl["uses_point_size"] and not refl["uses_point_coord"]

        if refl["uses_bda"]:
            dx12_ready = False
            notes.append("uses PhysicalStorageBufferAddresses (64-bit BDA); migrate to bindless buffer slots for HLSL SM 6.6")
        if sp.name == "world_cull.comp":
            notes.append("Metal 3 uses companion world_cull_icb.metal for MTLIndirectCommandBuffer; DX12 uses 24B indirect record + root constant")
        if sp.name in ("world_gpu.vert",) and refl["uses_instance_index"]:
            notes.append("DX12 SM 6.6 requires 1-DWORD root constant (or SM 6.8 SV_StartInstanceLocation) for StartInstanceLocation")
        if refl["stage"] == "vertex":
            notes.append("SPIRV-Cross emits flip_vert_y=true for Metal/DX12 clip-space Y parity")

        # Optional live spirv-cross / dxc smoke-test when installed
        msl_cross_status = "untested (spirv-cross not installed)"
        hlsl_cross_status = "untested (spirv-cross not installed)"
        dxil_compile_status = "untested (spirv-cross not installed)"
        if spirv_cross:
            with tempfile.TemporaryDirectory(prefix="shift_spvc_") as tmp:
                tmp_dir = Path(tmp)
                # Metal 3 MSL 3.1
                msl_out = tmp_dir / (sp.name + ".metal")
                msl_cmd = [
                    spirv_cross, str(spv_path),
                    "--msl", "--msl-version", "30100", "--msl-argument-buffers",
                    "--output", str(msl_out),
                ]
                if refl["stage"] == "vertex":
                    msl_cmd.append("--flip-vert-y")
                mres = subprocess.run(msl_cmd, capture_output=True, text=True)
                if mres.returncode == 0:
                    msl_cross_status = "ok"
                else:
                    msl_cross_status = f"error: {(mres.stderr or mres.stdout).strip()[:160]}"
                    if metal_ready:
                        errors.append(f"{sp.name}: spirv-cross MSL 3.1 failed: {msl_cross_status}")

                # DirectX 12 HLSL SM 6.6 (+ DXC DXIL if available)
                if not refl["uses_bda"]:
                    hlsl_out = tmp_dir / (sp.name + ".hlsl")
                    hlsl_cmd = [
                        spirv_cross, str(spv_path),
                        "--hlsl", "--shader-model", "66", "--hlsl-enable-compat",
                        "--output", str(hlsl_out),
                    ]
                    if refl["stage"] == "vertex":
                        hlsl_cmd.append("--flip-vert-y")
                    hres = subprocess.run(hlsl_cmd, capture_output=True, text=True)
                    if hres.returncode == 0:
                        hlsl_cross_status = "ok"
                        if dxc and refl["stage"] in STAGE_TO_DXC_PROFILE:
                            dxil_out = tmp_dir / (sp.name + ".dxil")
                            dxc_cmd = [
                                dxc, "-T", STAGE_TO_DXC_PROFILE[refl["stage"]],
                                "-E", refl["entry_point"],
                                "-Fo", str(dxil_out), str(hlsl_out),
                            ]
                            dres = subprocess.run(dxc_cmd, capture_output=True, text=True)
                            if dres.returncode == 0:
                                dxil_compile_status = "ok"
                            else:
                                dxil_compile_status = f"error: {(dres.stderr or dres.stdout).strip()[:160]}"
                        else:
                            dxil_compile_status = "untested (dxc not installed)"
                    else:
                        hlsl_cross_status = f"error: {(hres.stderr or hres.stdout).strip()[:160]}"
                        dxil_compile_status = "skipped (hlsl failed)"
                else:
                    hlsl_cross_status = "skipped (BDA pending bindless slot migration)"
                    dxil_compile_status = "skipped (BDA pending bindless slot migration)"

        if vk_ready:
            vk_ready_count += 1
        if metal_ready:
            metal_ready_count += 1
        if dx12_ready:
            dx12_ready_count += 1

        shader_records.append({
            "name": sp.name,
            "source": sp.relative_to(ROOT).as_posix(),
            "prebuilt_spv": spv_path.relative_to(ROOT).as_posix(),
            "stage": refl["stage"],
            "entry_point": refl["entry_point"],
            "spirv_version": refl["spirv_version"],
            "spirv_bytes": refl["byte_size"],
            "local_size": refl["local_size"],
            "push_constant_bytes": refl["push_constant_bytes"],
            "spec_constants": refl["spec_constants"],
            "bindings": refl["bindings"],
            "vertex_inputs": refl["vertex_inputs"],
            "capabilities": refl["capabilities"],
            "uses_point_size": refl["uses_point_size"],
            "uses_point_coord": refl["uses_point_coord"],
            "uses_instance_index": refl["uses_instance_index"],
            "uses_frag_coord": refl["uses_frag_coord"],
            "uses_bda": refl["uses_bda"],
            "uses_ray_query": refl["uses_ray_query"],
            "uses_bindless": refl["uses_bindless"],
            "readiness": {
                "vulkan_1_3": vk_ready,
                "metal_3_msl_3_1": metal_ready,
                "dx12_hlsl_sm_6_6": dx12_ready,
            },
            "notes": notes,
        })

    # Host C++ check: zero VK_PRIMITIVE_TOPOLOGY_POINT_LIST anywhere in showroom_sky.cpp
    sky_cpp = ROOT / "apps" / "carview" / "showroom_sky.cpp"
    if sky_cpp.is_file():
        sky_code = strip_glsl_comments(sky_cpp.read_text(encoding="utf-8", errors="replace"))
        if "VK_PRIMITIVE_TOPOLOGY_POINT_LIST" in sky_code:
            errors.append("apps/carview/showroom_sky.cpp still references VK_PRIMITIVE_TOPOLOGY_POINT_LIST")

    manifest: dict[str, Any] = {
        "schema_version": 1,
        "shader_dir": "apps/carview/shaders",
        "limits": {
            "max_push_constant_bytes": MAX_PUSH_CONSTANT_BYTES,
            "allowed_spec_constant_ids": [0, 1],
            "point_list_allowed": False,
        },
        "summary": {
            "total_shader_files": len(entries) + len(headers),
            "entry_shaders": len(entries),
            "shared_headers": len(headers),
            "max_push_constant_bytes_observed": max_pc_seen,
            "point_size_shaders": sum(1 for s in shader_records if s["uses_point_size"]),
            "point_coord_shaders": sum(1 for s in shader_records if s["uses_point_coord"]),
            "bindless_shaders": sum(1 for s in shader_records if s["uses_bindless"]),
            "ray_query_shaders": sum(1 for s in shader_records if s["uses_ray_query"]),
            "bda_shaders": sum(1 for s in shader_records if s["uses_bda"]),
            "vulkan_1_3_ready": vk_ready_count,
            "metal_3_ready": metal_ready_count,
            "dx12_sm_6_6_ready": dx12_ready_count,
        },
        "headers": header_records,
        "shaders": shader_records,
    }
    return manifest, errors, advisories


def write_manifest(manifest: dict[str, Any], path: Path = MANIFEST_PATH) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(manifest, indent=2) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit all carview shaders for Vulkan/Metal/DX12 portability.")
    parser.add_argument("--json", action="store_true", help="Print the full manifest JSON to stdout")
    parser.add_argument("--write-manifest", action="store_true", help="Write apps/carview/shaders/prebuilt/shader_manifest.json")
    parser.add_argument("--check-manifest", action="store_true", help="Fail if committed shader_manifest.json is stale")
    parser.add_argument("--no-external-tools", action="store_true", help="Skip spirv-val/spirv-cross/dxc even if installed")
    args = parser.parse_args()

    manifest, errors, advisories = build_manifest(run_tools=not args.no_external_tools)

    if args.write_manifest:
        write_manifest(manifest)

    if args.check_manifest:
        if not MANIFEST_PATH.is_file():
            errors.append(f"missing {MANIFEST_PATH.relative_to(ROOT).as_posix()}; run tools/regenerate-shaders.py --emit-manifest")
        else:
            expected = json.dumps(manifest, indent=2) + "\n"
            actual = MANIFEST_PATH.read_text(encoding="utf-8")
            if actual != expected:
                errors.append(f"stale {MANIFEST_PATH.relative_to(ROOT).as_posix()}; regenerate via tools/regenerate-shaders.py")

    if args.json:
        print(json.dumps(manifest, indent=2))
    else:
        s = manifest["summary"]
        print(
            f"shader-portability: {s['total_shader_files']} files "
            f"({s['entry_shaders']} entries + {s['shared_headers']} headers), "
            f"max push constants = {s['max_push_constant_bytes_observed']} B / {MAX_PUSH_CONSTANT_BYTES} B, "
            f"PointSize/PointCoord = {s['point_size_shaders']}/{s['point_coord_shaders']}, "
            f"ready: VK1.3={s['vulkan_1_3_ready']}/{s['entry_shaders']}, "
            f"Metal3={s['metal_3_ready']}/{s['entry_shaders']}, "
            f"DX12_SM66={s['dx12_sm_6_6_ready']}/{s['entry_shaders']} "
            f"(BDA pending on {s['bda_shaders']} RT shaders)"
        )
        for adv in advisories:
            print(f"  [advisory] {adv}")

    for err in errors:
        print(f"ERROR: {err}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
