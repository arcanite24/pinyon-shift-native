#!/usr/bin/env python3
"""Recompile carview's GLSL into prebuilt SPIR-V and optional Metal/DX12 targets.

The build compiles the shaders itself when it finds glslangValidator or
glslc; otherwise it uses apps/carview/shaders/prebuilt/*.spv. Run this
after every shader change so the prebuilt SPIR-V and shader_manifest.json
match the GLSL source:

  python tools/regenerate-shaders.py [--compiler path/to/glslc]
                                      [--target-env vulkan1.3]
                                      [--emit-msl]
                                      [--emit-hlsl]
                                      [--emit-dxil]
                                      [--check-portability]

Compilers and cross-compilers (glslangValidator, glslc, spirv-val,
spirv-cross, dxc) are searched on PATH, $VULKAN_SDK, Android NDK
shader-tools, and Windows Kits when explicit paths are not given.
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "apps" / "carview" / "shaders"
PREBUILT = SHADERS / "prebuilt"
MANIFEST_PATH = PREBUILT / "shader_manifest.json"
CHECKER_PATH = Path(__file__).resolve().parent / "check-shader-portability.py"

SHADER_EXTS = ("*.vert", "*.frag", "*.comp", "*.rgen", "*.rchit", "*.rahit", "*.rmiss")


def load_portability_module() -> Any:
    spec = importlib.util.spec_from_file_location("check_shader_portability", CHECKER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load {CHECKER_PATH}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def emit_msl_targets(
    spv_files: list[Path],
    spirv_cross: str,
    out_dir: Path,
    checker: Any,
) -> tuple[int, int]:
    """Cross-compile SPIR-V files to Metal 3 (MSL 3.1 + argument buffers)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    ok_count = 0
    fail_count = 0
    for spv in spv_files:
        refl = checker.reflect_spirv(spv.read_bytes())
        shader_name = spv.name[:-4] if spv.name.endswith(".spv") else spv.name
        out = out_dir / (shader_name + ".metal")
        cmd = [
            spirv_cross,
            str(spv),
            "--msl",
            "--msl-version",
            "30100",
            "--msl-argument-buffers",
            "--output",
            str(out),
        ]
        if refl["stage"] == "vertex":
            cmd.append("--flip-vert-y")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            ok_count += 1
            print(f"[msl]  {shader_name} -> {out.relative_to(ROOT)}")
        else:
            fail_count += 1
            msg = (res.stderr or res.stdout).strip().splitlines()[:2]
            print(f"[msl]  FAIL {shader_name}: {' | '.join(msg)}", file=sys.stderr)
    return ok_count, fail_count


def emit_hlsl_and_dxil_targets(
    spv_files: list[Path],
    spirv_cross: str,
    dxc: str | None,
    hlsl_dir: Path,
    dxil_dir: Path | None,
    checker: Any,
) -> tuple[int, int, int, int]:
    """Cross-compile SPIR-V files to HLSL SM 6.6 and optionally DXIL via dxc."""
    hlsl_dir.mkdir(parents=True, exist_ok=True)
    if dxil_dir is not None:
        dxil_dir.mkdir(parents=True, exist_ok=True)
    hlsl_ok = 0
    hlsl_skip = 0
    dxil_ok = 0
    fail_count = 0
    for spv in spv_files:
        refl = checker.reflect_spirv(spv.read_bytes())
        shader_name = spv.name[:-4] if spv.name.endswith(".spv") else spv.name
        if refl["uses_bda"]:
            hlsl_skip += 1
            print(f"[hlsl] SKIP {shader_name} (uses 64-bit BDA; pending bindless slot migration)")
            continue
        hlsl_out = hlsl_dir / (shader_name + ".hlsl")
        cmd = [
            spirv_cross,
            str(spv),
            "--hlsl",
            "--shader-model",
            "66",
            "--hlsl-enable-compat",
            "--output",
            str(hlsl_out),
        ]
        if refl["stage"] == "vertex":
            cmd.append("--flip-vert-y")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            fail_count += 1
            msg = (res.stderr or res.stdout).strip().splitlines()[:2]
            print(f"[hlsl] FAIL {shader_name}: {' | '.join(msg)}", file=sys.stderr)
            continue
        hlsl_ok += 1
        print(f"[hlsl] {shader_name} -> {hlsl_out.relative_to(ROOT)}")

        if dxil_dir is not None and dxc is not None:
            profile = checker.STAGE_TO_DXC_PROFILE.get(refl["stage"])
            if not profile:
                continue
            dxil_out = dxil_dir / (shader_name + ".dxil")
            dxc_cmd = [
                dxc,
                "-T",
                profile,
                "-E",
                refl["entry_point"],
                "-Fo",
                str(dxil_out),
                str(hlsl_out),
            ]
            dres = subprocess.run(dxc_cmd, capture_output=True, text=True)
            if dres.returncode == 0:
                dxil_ok += 1
                print(f"[dxil] {shader_name} -> {dxil_out.relative_to(ROOT)}")
            else:
                fail_count += 1
                msg = (dres.stderr or dres.stdout).strip().splitlines()[:2]
                print(f"[dxil] FAIL {shader_name}: {' | '.join(msg)}", file=sys.stderr)
    return hlsl_ok, hlsl_skip, dxil_ok, fail_count


def main() -> int:
    checker = load_portability_module()

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--compiler", help="Path to glslangValidator or glslc")
    parser.add_argument(
        "--target-env",
        default="vulkan1.3",
        help="vulkan1.2 for compilers older than Vulkan 1.3 (the SPIR-V runs on 1.3)",
    )
    parser.add_argument("--spirv-cross", help="Path to spirv-cross (optional, for --emit-msl/--emit-hlsl/--emit-dxil)")
    parser.add_argument("--dxc", help="Path to dxc (optional, for --emit-dxil)")
    parser.add_argument("--emit-msl", action="store_true", help="Cross-compile prebuilt SPIR-V to MSL 3.1 (.metal)")
    parser.add_argument("--emit-hlsl", action="store_true", help="Cross-compile prebuilt SPIR-V to HLSL SM 6.6 (.hlsl)")
    parser.add_argument("--emit-dxil", action="store_true", help="Compile generated HLSL SM 6.6 to DXIL via dxc")
    parser.add_argument("--msl-out-dir", type=Path, default=PREBUILT / "msl", help="Output directory for --emit-msl")
    parser.add_argument("--hlsl-out-dir", type=Path, default=PREBUILT / "hlsl", help="Output directory for --emit-hlsl")
    parser.add_argument("--dxil-out-dir", type=Path, default=PREBUILT / "dxil", help="Output directory for --emit-dxil")
    parser.add_argument("--check-portability", action="store_true", help="Audit all shaders for Vulkan/Metal/DX12 portability")
    parser.add_argument("--skip-spv", action="store_true", help="Skip GLSL -> SPIR-V compilation and use existing prebuilt/*.spv")
    parser.add_argument("--manifest-only", action="store_true", help="Only regenerate prebuilt/shader_manifest.json from existing SPIR-V")
    parser.add_argument("--no-manifest", action="store_true", help="Do not write prebuilt/shader_manifest.json")
    parser.add_argument("--strict-tools", action="store_true", help="Fail if optional spirv-cross/dxc tools are missing when requested")
    args = parser.parse_args()

    sources: list[Path] = []
    for ext in SHADER_EXTS:
        sources.extend(sorted(SHADERS.glob(ext)))
    sources.sort()

    spv_files: list[Path] = [PREBUILT / (s.name + ".spv") for s in sources]

    # 1. Compile GLSL -> SPIR-V unless skipped
    if not args.skip_spv and not args.manifest_only:
        # Prefer glslc so -O SPIR-V optimization runs (keeps large uber-shaders like
        # showroom.frag compact and avoids Apple M4 AGX MCObjectStreamer codegen faults).
        compiler = args.compiler or checker.find_tool(("glslc", "glslangValidator"))
        if not compiler:
            print("no glslangValidator or glslc found; pass --compiler or --skip-spv", file=sys.stderr)
            return 1
        is_glslc = Path(compiler).stem.lower() == "glslc"
        PREBUILT.mkdir(parents=True, exist_ok=True)
        for source, out in zip(sources, spv_files):
            if is_glslc:
                command = [compiler, "-O", f"--target-env={args.target_env}", "-o", str(out), str(source)]
            else:
                command = [compiler, "-V", "--target-env", args.target_env, "-o", str(out), str(source)]
            subprocess.run(command, check=True)
            print(f"[spv]  {source.name} -> {out.relative_to(ROOT)}")

    # 2. Optional Metal 3 (MSL 3.1) emission
    exit_code = 0
    spirv_cross = args.spirv_cross or checker.find_tool(("spirv-cross",))
    dxc = args.dxc or checker.find_tool(("dxc",))

    if args.emit_msl:
        if not spirv_cross:
            msg = "spirv-cross not found on PATH or $VULKAN_SDK; skipping --emit-msl"
            if args.strict_tools:
                print(f"ERROR: {msg}", file=sys.stderr)
                return 1
            print(f"[msl]  WARN: {msg}")
        else:
            ok, fail = emit_msl_targets(spv_files, spirv_cross, args.msl_out_dir, checker)
            print(f"[msl]  summary: {ok} emitted, {fail} failed")
            if fail:
                exit_code = 1

    # 3. Optional DX12 (HLSL SM 6.6 / DXIL) emission
    if args.emit_hlsl or args.emit_dxil:
        if not spirv_cross:
            msg = "spirv-cross not found on PATH or $VULKAN_SDK; skipping --emit-hlsl/--emit-dxil"
            if args.strict_tools:
                print(f"ERROR: {msg}", file=sys.stderr)
                return 1
            print(f"[hlsl] WARN: {msg}")
        else:
            dxil_dir = args.dxil_out_dir if args.emit_dxil else None
            if args.emit_dxil and not dxc:
                msg = "dxc not found on PATH or Windows Kits; emitting HLSL only"
                if args.strict_tools:
                    print(f"ERROR: {msg}", file=sys.stderr)
                    return 1
                print(f"[dxil] WARN: {msg}")
                dxil_dir = None
            hok, hskip, dok, fail = emit_hlsl_and_dxil_targets(
                spv_files, spirv_cross, dxc, args.hlsl_out_dir, dxil_dir, checker
            )
            print(f"[hlsl] summary: {hok} emitted, {hskip} skipped (BDA), {dok} dxil, {fail} failed")
            if fail:
                exit_code = 1

    # 4. Reflect & emit shader_manifest.json + optional portability audit
    if not args.no_manifest or args.check_portability or args.manifest_only:
        manifest, errors, advisories = checker.build_manifest(run_tools=args.check_portability)
        if not args.no_manifest:
            checker.write_manifest(manifest, MANIFEST_PATH)
            print(f"[manifest] wrote {MANIFEST_PATH.relative_to(ROOT)} ({manifest['summary']['entry_shaders']} entries, {manifest['summary']['shared_headers']} headers)")
        if args.check_portability:
            s = manifest["summary"]
            print(
                f"[portability] {s['total_shader_files']} files, "
                f"max push constants = {s['max_push_constant_bytes_observed']} B / {checker.MAX_PUSH_CONSTANT_BYTES} B, "
                f"PointSize/PointCoord = {s['point_size_shaders']}/{s['point_coord_shaders']}, "
                f"VK1.3={s['vulkan_1_3_ready']}/{s['entry_shaders']}, "
                f"Metal3={s['metal_3_ready']}/{s['entry_shaders']}, "
                f"DX12_SM66={s['dx12_sm_6_6_ready']}/{s['entry_shaders']}"
            )
            for adv in advisories:
                print(f"  [advisory] {adv}")
            for err in errors:
                print(f"ERROR: {err}", file=sys.stderr)
            if errors:
                exit_code = 1

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
