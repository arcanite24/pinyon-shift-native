#!/usr/bin/env python3
"""Compile and validate libs/rhi/shaders/* into libs/rhi/src/rhi_modern_shaders_spv.h."""
from __future__ import annotations

import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SH_DIR = ROOT / "libs" / "rhi" / "shaders"
PRE_DIR = SH_DIR / "prebuilt"
OUT_HDR = ROOT / "libs" / "rhi" / "src" / "rhi_modern_shaders_spv.h"

SHADERS = [
    ("light_cluster.comp",     "kLightClusterCompSpv"),
    ("vis_cull.comp",          "kVisCullCompSpv"),
    ("vis_cull_hiz.comp",      "kVisCullHizCompSpv"),
    ("vis_raster.vert",        "kVisRasterVertSpv"),
    ("vis_raster.frag",        "kVisRasterFragSpv"),
    ("vis_classify.comp",      "kVisClassifyCompSpv"),
    ("vis_shade.comp",         "kVisShadeCompSpv"),
    ("xenos_detile.comp",      "kXenosDetileCompSpv"),
    ("hiz_spd.comp",           "kHizSpdCompSpv"),
    ("post_bloom_spd.comp",    "kPostBloomSpdCompSpv"),
    ("post_exposure_hist.comp","kPostExposureHistCompSpv"),
    ("post_mblur_tile.comp",   "kPostMblurTileCompSpv"),
    ("post_taa_hdr.comp",      "kPostTaaHdrCompSpv"),
]


def find_shader_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    ndk_base = pathlib.Path.home() / "AppData" / "Local" / "Android" / "Sdk" / "ndk"
    if ndk_base.is_dir():
        for cand in sorted(ndk_base.glob(f"*/shader-tools/windows-x86_64/{name}.exe"), reverse=True):
            return str(cand)
    raise RuntimeError(f"Could not find {name} on PATH or Android NDK shader-tools")


def main() -> int:
    glslc = find_shader_tool("glslc")
    spirv_val = find_shader_tool("spirv-val")
    PRE_DIR.mkdir(parents=True, exist_ok=True)

    lines = [
        "// Auto-generated validated Vulkan 1.3 SPIR-V binaries for libs/rhi modern pipelines.",
        "// Regenerate with: python tools/regenerate-rhi-shaders.py",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace shift::rhi::shaders {",
        "",
    ]

    for fname, sym in SHADERS:
        src = SH_DIR / fname
        spv = PRE_DIR / (fname + ".spv")
        res = subprocess.run(
            [glslc, "-O", "--target-env=vulkan1.3", "-o", str(spv), str(src)],
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            print(f"FAIL glslc {fname}:\n{res.stderr}", file=sys.stderr)
            return 1
        vres = subprocess.run(
            [spirv_val, "--target-env", "vulkan1.3", str(spv)],
            capture_output=True,
            text=True,
        )
        if vres.returncode != 0:
            print(f"FAIL spirv-val {fname}:\n{vres.stderr}", file=sys.stderr)
            return 1
        data = spv.read_bytes()
        words = struct.unpack(f"<{len(data) // 4}I", data)
        print(f"[ok] {fname}: {len(words)} words ({len(data)} B)")
        lines.append(f"inline constexpr uint32_t {sym}[{len(words)}] = {{")
        for i in range(0, len(words), 8):
            chunk = ", ".join(f"0x{w:08x}u" for w in words[i : i + 8])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")

    lines.append("}  // namespace shift::rhi::shaders")
    OUT_HDR.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {OUT_HDR.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
