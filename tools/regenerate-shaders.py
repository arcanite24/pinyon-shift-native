#!/usr/bin/env python3
"""Recompile carview's GLSL into the committed SPIR-V fallback.

The build compiles the shaders itself when it finds glslangValidator or
glslc; otherwise it uses apps/carview/shaders/prebuilt/*.spv. Run this
after every shader change so the fallback matches the source:

  python tools/regenerate-shaders.py [--compiler path/to/glslangValidator]
                                      [--target-env vulkan1.2]

The compiler is searched on PATH and in $VULKAN_SDK/Bin (or bin) when
--compiler is not given.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "apps" / "carview" / "shaders"


def find_compiler() -> str | None:
    for name in ("glslangValidator", "glslc"):
        found = shutil.which(name)
        if found:
            return found
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for sub in ("Bin", "bin"):
            for name in ("glslangValidator", "glslc"):
                for suffix in ("", ".exe"):
                    candidate = Path(sdk) / sub / (name + suffix)
                    if candidate.is_file():
                        return str(candidate)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--compiler")
    parser.add_argument("--target-env", default="vulkan1.3",
                        help="vulkan1.2 for compilers older than Vulkan 1.3 (the SPIR-V runs on 1.3)")
    args = parser.parse_args()
    compiler = args.compiler or find_compiler()
    if not compiler:
        print("no glslangValidator or glslc found; pass --compiler", file=sys.stderr)
        return 1
    is_glslc = Path(compiler).stem.lower() == "glslc"
    shader_exts = ("*.vert", "*.frag", "*.comp", "*.rgen", "*.rchit", "*.rahit", "*.rmiss")
    sources = []
    for ext in shader_exts:
        sources.extend(sorted(SHADERS.glob(ext)))
    for source in sources:
        out = SHADERS / "prebuilt" / (source.name + ".spv")
        if is_glslc:
            command = [compiler, f"--target-env={args.target_env}", "-o", str(out), str(source)]
        else:
            command = [compiler, "-V", "--target-env", args.target_env, "-o", str(out), str(source)]
        subprocess.run(command, check=True)
        print(f"{source.name} -> {out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
