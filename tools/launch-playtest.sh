#!/usr/bin/env bash
# Launches carview in Playtest Session & Bug Recorder mode on macOS and Linux.
#
# Usage:
#   ./tools/launch-playtest.sh [options] [extra carview args...]
#   ./tools/launch-playtest.sh --play "/path/to/play" --car NIS_SkylineGTRVSpecII_02 --hour 18
#   ./tools/launch-playtest.sh --bin "/Applications/Shift Engine.app"
#   ./tools/launch-playtest.sh --launcher --bin "/Applications/Shift Engine.app"
#
# Options:
#   --play <dir>        Play folder (alias: --play-dir; default: ~/Library/Application Support/shift-engine/play on macOS,
#                       $XDG_DATA_HOME/shift-engine/play or ~/.local/share/shift-engine/play on Linux)
#   --bin <path>        Path to carview binary or macOS Shift Engine.app bundle (alias: --exe)
#   --out-root <dir>    Root folder for playtest sessions (default: <repo>/out/playtests)
#   --launcher          Launch shift-launcher with SHIFT_RECORD_SESSION instead of carview directly
#   -h, --help          Show this help
#   --                  Stop parsing script options; forward everything remaining to carview
#
# Controls during playtest:
#   macOS : Cmd + Shift + B, F11 (if Show Desktop is unbound), or Back + RB (gamepad)
#   Linux : F11 (keyboard) or Back + RB (gamepad)
#
# Outputs under out/playtests/YYYYMMDD-HHMMSS/:
#   session.log        stdout + stderr (missing textures, warnings, errors)
#   telemetry.jsonl    4 Hz periodic + every hitch (>25 ms), ground anomaly, audio glitch, and user mark
#   bug_00N.png/.json  user-marked bug screenshots + state snapshots + copy-pasteable repro CLI
#   summary.json       auto-generated exit summary (p50/p95/p99/max ms, top 20 hitches, anomalies, bugs)

set -uo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(dirname -- "$SCRIPT_DIR")"
OS_NAME="$(uname -s)"

if [ "$OS_NAME" = "Darwin" ]; then
  PLAY_DIR="$HOME/Library/Application Support/shift-engine/play"
else
  PLAY_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/shift-engine/play"
fi

BIN_PATH=""
OUT_ROOT="$ROOT_DIR/out/playtests"
USE_LAUNCHER=0
EXTRA_ARGS=()

usage() {
  sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --play|--play-dir)
      if [ $# -lt 2 ]; then echo "error: $1 requires a value" >&2; exit 2; fi
      PLAY_DIR="$2"
      shift 2
      ;;
    --bin|--exe)
      if [ $# -lt 2 ]; then echo "error: $1 requires a value" >&2; exit 2; fi
      BIN_PATH="$2"
      shift 2
      ;;
    --out-root)
      if [ $# -lt 2 ]; then echo "error: --out-root requires a value" >&2; exit 2; fi
      OUT_ROOT="$2"
      shift 2
      ;;
    --launcher)
      USE_LAUNCHER=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [ $# -gt 0 ]; do
        EXTRA_ARGS+=("$1")
        shift
      done
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

# Resolve .app bundle paths if given (e.g. /Applications/Shift Engine.app).
resolve_target() {
  local target="$1"
  local want="$2"
  if [ -d "$target" ] && [ -x "$target/Contents/MacOS/$want" ]; then
    printf '%s\n' "$target/Contents/MacOS/$want"
    return 0
  fi
  if [ -x "$target" ] && [ ! -d "$target" ]; then
    printf '%s\n' "$target"
    return 0
  fi
  return 1
}

WANT_BIN="carview"
if [ "$USE_LAUNCHER" -eq 1 ]; then
  WANT_BIN="shift-launcher"
fi

RESOLVED_BIN=""
if [ -n "$BIN_PATH" ]; then
  if ! RESOLVED_BIN="$(resolve_target "$BIN_PATH" "$WANT_BIN")"; then
    echo "error: $WANT_BIN not found or not executable at: $BIN_PATH" >&2
    exit 1
  fi
else
  CANDIDATES=(
    "$ROOT_DIR/build/apps/$WANT_BIN/$WANT_BIN"
    "$ROOT_DIR/build/apps/$WANT_BIN/Release/$WANT_BIN"
  )
  if [ "$WANT_BIN" = "shift-launcher" ]; then
    CANDIDATES=(
      "$ROOT_DIR/build/apps/launcher/shift-launcher"
      "$ROOT_DIR/build/apps/launcher/Release/shift-launcher"
    )
  fi
  if [ "$OS_NAME" = "Darwin" ]; then
    CANDIDATES+=(
      "$ROOT_DIR/out/release/Shift Engine.app"
      "/Applications/Shift Engine.app"
      "$HOME/Applications/Shift Engine.app"
    )
  fi
  for c in "${CANDIDATES[@]}"; do
    if RESOLVED_BIN="$(resolve_target "$c" "$WANT_BIN")"; then
      break
    fi
  done
  if [ -z "$RESOLVED_BIN" ]; then
    echo "error: $WANT_BIN not found. Build first with:" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    echo "or pass --bin <path-to-carview-or-Shift-Engine.app>." >&2
    exit 1
  fi
fi

# On macOS, ensure SDL3/volk can find libvulkan.1.dylib or libMoltenVK.dylib
# both for packaged .app bundles (mirrors tools/package.py's macos_launcher_wrapper)
# and for raw build/apps/carview/carview binaries.
MVK_LIB_DIR=""
if [ "$OS_NAME" = "Darwin" ]; then
  BIN_DIR="$(CDPATH='' cd -- "$(dirname -- "$RESOLVED_BIN")" && pwd)"
  MVK_CANDIDATES=()
  ICD_CANDIDATES=()

  if [ "$(basename -- "$BIN_DIR")" = "MacOS" ]; then
    CONTENTS_DIR="$(dirname -- "$BIN_DIR")"
    if [ "$(basename -- "$CONTENTS_DIR")" = "Contents" ]; then
      MVK_CANDIDATES+=("$CONTENTS_DIR/Frameworks")
      ICD_CANDIDATES+=(
        "$CONTENTS_DIR/Frameworks/MoltenVK_icd.json"
        "$CONTENTS_DIR/Resources/vulkan/icd.d/MoltenVK_icd.json"
      )
    fi
  fi

  MVK_CANDIDATES+=(
    "$BIN_DIR/../Frameworks"
    "$BIN_DIR"
    "$ROOT_DIR/out/release/Shift Engine.app/Contents/Frameworks"
  )
  # Check any staged macOS release bundle under out/release/shift-engine-macos-*
  if [ -d "$ROOT_DIR/out/release" ]; then
    while IFS= read -r stage_fw; do
      [ -n "$stage_fw" ] && MVK_CANDIDATES+=("$stage_fw")
    done < <(find "$ROOT_DIR/out/release" -maxdepth 4 -path '*/Shift Engine.app/Contents/Frameworks' -type d 2>/dev/null | sort -r)
  fi
  MVK_CANDIDATES+=(
    "/Applications/Shift Engine.app/Contents/Frameworks"
    "$HOME/Applications/Shift Engine.app/Contents/Frameworks"
    "$HOME/Applications/Shift Engine Integration.app/Contents/Frameworks"
    "/opt/homebrew/opt/vulkan-loader/lib"
    "/opt/homebrew/opt/molten-vk/lib"
    "/opt/homebrew/lib"
    "/usr/local/lib"
  )

  for d in "${MVK_CANDIDATES[@]}"; do
    if [ -f "$d/libvulkan.1.dylib" ] || [ -f "$d/libMoltenVK.dylib" ]; then
      MVK_LIB_DIR="$(CDPATH='' cd -- "$d" && pwd)"
      export DYLD_FALLBACK_LIBRARY_PATH="$MVK_LIB_DIR${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
      if [ -f "$MVK_LIB_DIR/MoltenVK_icd.json" ]; then
        if [ ${#ICD_CANDIDATES[@]} -gt 0 ]; then
          ICD_CANDIDATES=("$MVK_LIB_DIR/MoltenVK_icd.json" "${ICD_CANDIDATES[@]}")
        else
          ICD_CANDIDATES=("$MVK_LIB_DIR/MoltenVK_icd.json")
        fi
      fi
      break
    fi
  done

  ICD_CANDIDATES+=(
    "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json"
    "/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json"
    "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json"
  )
  for icd in "${ICD_CANDIDATES[@]}"; do
    if [ -f "$icd" ]; then
      export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$icd}"
      export VK_DRIVER_FILES="${VK_DRIVER_FILES:-$icd}"
      break
    fi
  done
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
SESSION_DIR="$OUT_ROOT/$STAMP"
if [ -e "$SESSION_DIR" ]; then
  SUFFIX=1
  while [ -e "${SESSION_DIR}_${SUFFIX}" ]; do
    SUFFIX=$((SUFFIX + 1))
  done
  SESSION_DIR="${SESSION_DIR}_${SUFFIX}"
fi
mkdir -p "$SESSION_DIR"

LOG_PATH="$SESSION_DIR/session.log"

if [ "$OS_NAME" = "Darwin" ]; then
  BUG_MARK_HINT="Cmd + Shift + B, F11, or Back + RB (gamepad)"
else
  BUG_MARK_HINT="F11 (keyboard) or Back + RB (gamepad)"
fi

echo "============================================================"
echo "Shift Engine Playtest Recorder"
echo "  Executable : $RESOLVED_BIN"
if [ -n "$MVK_LIB_DIR" ]; then
  echo "  Vulkan lib : $MVK_LIB_DIR"
fi
if [ "$USE_LAUNCHER" -eq 0 ]; then
  echo "  Play dir   : $PLAY_DIR"
else
  echo "  Mode       : shift-launcher (SHIFT_RECORD_SESSION inherited by carview)"
fi
echo "  Session dir: $SESSION_DIR"
echo "  Log file   : $LOG_PATH"
echo "  Bug mark   : $BUG_MARK_HINT"
echo "============================================================"

EXIT_CODE=0
if [ "$USE_LAUNCHER" -eq 1 ]; then
  if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
    SHIFT_RECORD_SESSION="$SESSION_DIR" "$RESOLVED_BIN" "${EXTRA_ARGS[@]}" 2>&1 | tee "$LOG_PATH"
  else
    SHIFT_RECORD_SESSION="$SESSION_DIR" "$RESOLVED_BIN" 2>&1 | tee "$LOG_PATH"
  fi
  EXIT_CODE="${PIPESTATUS[0]}"
else
  CMD_ARGS=(--drive --play "$PLAY_DIR" --record-session "$SESSION_DIR")
  if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
    CMD_ARGS+=("${EXTRA_ARGS[@]}")
  fi
  "$RESOLVED_BIN" "${CMD_ARGS[@]}" 2>&1 | tee "$LOG_PATH"
  EXIT_CODE="${PIPESTATUS[0]}"
fi

echo ""
echo "============================================================"
echo "Playtest session saved to:"
echo "  $SESSION_DIR"
echo "Files:"
echo "  Log      : $LOG_PATH"

TELEMETRY_PATH="$SESSION_DIR/telemetry.jsonl"
if [ -f "$TELEMETRY_PATH" ]; then
  echo "  Telemetry: $TELEMETRY_PATH"
fi

SUMMARY_PATH="$SESSION_DIR/summary.json"
if [ -f "$SUMMARY_PATH" ]; then
  echo "  Summary  : $SUMMARY_PATH"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$SUMMARY_PATH" <<'PY'
import json, sys
try:
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        s = json.load(f)
    print("  Metrics  : {} frames, {:.1f}s | p50={:.2f}ms p95={:.2f}ms p99={:.2f}ms max={:.2f}ms".format(
        s.get("total_frames", 0),
        float(s.get("duration_s", 0.0)),
        float(s.get("p50_ms", 0.0)),
        float(s.get("p95_ms", 0.0)),
        float(s.get("p99_ms", 0.0)),
        float(s.get("max_ms", 0.0)),
    ))
    print("  Issues   : hitches(>25ms)={}, ground_anomalies={}, audio_glitches={}, bugs_marked={}".format(
        s.get("hitch_count", 0),
        s.get("ground_anomaly_count", 0),
        s.get("audio_glitch_count", 0),
        len(s.get("bugs", [])),
    ))
except Exception as exc:
    print(f"  (could not parse summary.json preview: {exc})")
PY
  fi
fi

BUG_FILES=()
while IFS= read -r bf; do
  [ -n "$bf" ] && BUG_FILES+=("$bf")
done < <(find "$SESSION_DIR" -maxdepth 1 -name 'bug_*' -type f 2>/dev/null | sort)

if [ ${#BUG_FILES[@]} -gt 0 ]; then
  echo "  Bookmarks:"
  for bf in "${BUG_FILES[@]}"; do
    echo "    $bf"
  done
fi
echo "============================================================"

exit "$EXIT_CODE"
