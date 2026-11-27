#!/usr/bin/env bash
# Reproducible cross-build of Mesa Freedreno Turnip (libvulkan_freedreno.so)
# for Android arm64 (Qualcomm Adreno 7xx — Odin 2 / Odin 2 Portal, SD 8 Gen 2).
#
# Output:
#   assets/drivers/turnip/libvulkan_freedreno.so
#   assets/drivers/turnip/meta.json
#
# Prerequisites (Linux or WSL2 Ubuntu 22.04/24.04):
#   sudo apt-get update && sudo apt-get install -y \
#       git meson ninja-build python3-mako pkg-config bison flex \
#       glslang-tools patchelf libdrm-dev zlib1g-dev
#   export ANDROID_NDK_HOME=/path/to/android-ndk-r27c  # API >= 29
#
# Usage:
#   ./tools/build-turnip-android.sh [--mesa-ref mesa-25.2.0] [--hybrid-patch] [--patch path/to.patch]
#
# Notes on Shift Engine runtime tuning:
#   - Shift Engine loads assets/drivers/turnip/libvulkan_freedreno.so through
#     libadrenotools before vkCreateInstance() on Qualcomm Adreno devices.
#   - On Adreno 740 (Odin 2 Portal), TU_DEBUG=sysmem is set by default when
#     Turnip is active (+60% FPS on shadow+post-chain heavy frames because
#     gmem tile binning overhead exceeds sysmem bandwidth on our render graph).
#   - Custom Turnip forks can further optimize this by applying a per-render-pass
#     heuristic in src/freedreno/vulkan/tu_cmd_buffer.cc (use_sysmem_rendering)
#     and enabling UBWC on B10G11R11_UFLOAT_PACK32 targets.

set -euo pipefail

MESA_REPO="${MESA_REPO:-https://gitlab.freedesktop.org/mesa/mesa.git}"
MESA_REF="${MESA_REF:-main}"
API_LEVEL="${API_LEVEL:-29}"
PATCH_FILE=""
WORK_DIR="${WORK_DIR:-$(pwd)/.local/turnip-build}"
OUT_DIR="$(pwd)/assets/drivers/turnip"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mesa-ref) MESA_REF="$2"; shift 2 ;;
    --mesa-repo) MESA_REPO="$2"; shift 2 ;;
    --hybrid-patch) PATCH_FILE="$(pwd)/tools/patches/turnip-adreno7xx-hybrid-sysmem.patch"; shift 1 ;;
    --patch) PATCH_FILE="$2"; shift 2 ;;
    --api) API_LEVEL="$2"; shift 2 ;;
    --work-dir) WORK_DIR="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,30p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  echo "ERROR: ANDROID_NDK_HOME must point to an Android NDK (r26+ / r27+)." >&2
  exit 1
fi

HOST_TAG="linux-x86_64"
TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
if [[ ! -d "${TOOLCHAIN}/bin" ]]; then
  echo "ERROR: NDK LLVM toolchain not found at ${TOOLCHAIN}/bin" >&2
  exit 1
fi

for tool in git meson ninja python3 patchelf; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: missing required tool '$tool'" >&2
    exit 1
  fi
done

mkdir -p "${WORK_DIR}"
MESA_SRC="${WORK_DIR}/mesa"
BUILD_DIR="${WORK_DIR}/build-arm64"
CROSS_FILE="${WORK_DIR}/android-aarch64.ini"

if [[ ! -d "${MESA_SRC}/.git" ]]; then
  git clone --depth 1 --branch "${MESA_REF}" "${MESA_REPO}" "${MESA_SRC}"
else
  git -C "${MESA_SRC}" fetch --depth 1 origin "${MESA_REF}"
  git -C "${MESA_SRC}" checkout --force FETCH_HEAD
fi

if [[ -n "${PATCH_FILE}" ]]; then
  echo "Applying patch ${PATCH_FILE}..."
  git -C "${MESA_SRC}" apply --check "${PATCH_FILE}"
  git -C "${MESA_SRC}" apply "${PATCH_FILE}"
fi

cat > "${CROSS_FILE}" <<EOF
[binaries]
ar = '${TOOLCHAIN}/bin/llvm-ar'
c = ['${TOOLCHAIN}/bin/aarch64-linux-android${API_LEVEL}-clang', '-O3', '-fPIC']
cpp = ['${TOOLCHAIN}/bin/aarch64-linux-android${API_LEVEL}-clang++', '-O3', '-fPIC']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '${TOOLCHAIN}/bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=${TOOLCHAIN}/sysroot/usr/lib/aarch64-linux-android', 'pkg-config']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

rm -rf "${BUILD_DIR}"
meson setup "${BUILD_DIR}" "${MESA_SRC}" \
  --cross-file "${CROSS_FILE}" \
  --buildtype=release \
  -Dplatforms=android \
  -Dplatform-sdk-version="${API_LEVEL}" \
  -Dandroid-stub=true \
  -Dgallium-drivers= \
  -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=kgsl \
  -Dglx=disabled \
  -Degl=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dopengl=false \
  -Dshared-glapi=disabled \
  -Dllvm=disabled \
  -Dzstd=disabled \
  -Dbuild-tests=false \
  -Dcpp_rtti=false

ninja -C "${BUILD_DIR}" src/freedreno/vulkan/libvulkan_freedreno.so

BUILT_SO="${BUILD_DIR}/src/freedreno/vulkan/libvulkan_freedreno.so"
mkdir -p "${OUT_DIR}"
cp -f "${BUILT_SO}" "${OUT_DIR}/libvulkan_freedreno.so"
"${TOOLCHAIN}/bin/llvm-strip" --strip-unneeded "${OUT_DIR}/libvulkan_freedreno.so"
patchelf --set-soname vulkan.adreno.so "${OUT_DIR}/libvulkan_freedreno.so"

MESA_SHA="$(git -C "${MESA_SRC}" rev-parse --short HEAD)"
MESA_VER="$(cat "${MESA_SRC}/VERSION" 2>/dev/null || echo "${MESA_REF}")"
cat > "${OUT_DIR}/meta.json" <<EOF
{
  "schemaVersion": 1,
  "name": "Mesa Turnip (Shift Engine custom build)",
  "description": "Custom-built Mesa Freedreno Turnip (${MESA_VER}, ${MESA_SHA}) for Qualcomm Adreno 7xx (KGSL)",
  "author": "Mesa / Freedreno contributors",
  "packageVersion": "${MESA_VER}-${MESA_SHA}",
  "vendor": "Mesa",
  "driverVersion": "Vulkan 1.4 (${MESA_VER})",
  "minApi": ${API_LEVEL},
  "libraryName": "libvulkan_freedreno.so"
}
EOF

echo "Built ${OUT_DIR}/libvulkan_freedreno.so ($(du -h "${OUT_DIR}/libvulkan_freedreno.so" | cut -f1))"
