#!/usr/bin/env bash
# Runs OxCity with no GPU and no display: Mesa's lavapipe (software Vulkan) + SDL's offscreen video driver.
# Extra arguments go to the game, e.g.
#   tools/run_headless.sh --autoplay --frames 3000 --fixed-dt 0.0333 --screenshots captures
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../env.sh
. "$ROOT/env.sh"

BIN_DIR="$ROOT/engine/build/linux/x86_64/release"
if [ ! -x "$BIN_DIR/OxCity" ]; then
  echo "build first: . ./env.sh && (cd engine && xmake b OxCity)" >&2
  exit 1
fi

# llvmpipe advertises mesh shaders and ray tracing, the engine uses both when present, and Mesa 25.2's JIT
# crashes compiling them. the engine reads these toggles from context_config.toml in the working directory
if [ ! -f "$BIN_DIR/context_config.toml" ]; then
  printf '[display]\nvsync = false\n\n[render]\nmesh_shaders = false\nray_tracing = false\n' > "$BIN_DIR/context_config.toml"
fi

export SDL_VIDEO_DRIVER=offscreen

# --validation: khronos validation layers from the sandbox (see bootstrap.sh), passed on as the engine's own flag
validation=()
for a in "$@"; do
  if [ "$a" = "--validation" ]; then
    VVL="$OXCITY_SANDBOX/sysroot-vvl"
    export VK_LAYER_PATH="$VVL/usr/share/vulkan/explicit_layer.d"
    export LD_LIBRARY_PATH="$VVL/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
    validation=(--vulkan-validation)
  fi
done
cd "$BIN_DIR"
# relative screenshot dirs are relative to where the script was called from
args=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--validation" ]; then
    shift
    continue
  fi
  if [ "$1" = "--screenshots" ] && [ $# -gt 1 ]; then
    case "$2" in /*) args+=("$1" "$2") ;; *) args+=("$1" "$OLDPWD/$2") ;; esac
    shift 2
  else
    args+=("$1")
    shift
  fi
done
# OXCITY_WIDTH/OXCITY_HEIGHT: lavapipe's cost goes with the pixel count, the multiplayer test runs several at once
exec ./OxCity --width "${OXCITY_WIDTH:-960}" --height "${OXCITY_HEIGHT:-540}" "${validation[@]}" "${args[@]}"
