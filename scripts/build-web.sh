#!/bin/bash
# Desktop-mode renderer → WebAssembly (raylib / WebGL), no OpenXR.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
WS="$(CDPATH= cd -- "$ROOT/.." && pwd)"
OUT="$WS/public/balloon"
EMSDK_ENV="${EMSDK_ENV:-/tmp/srcbuild/emsdk/emsdk_env.sh}"
RAYLIB_SRC="${RAYLIB_SRC:-/tmp/srcbuild/raylib/src}"

if [ ! -f "$EMSDK_ENV" ]; then
  echo "emsdk not found at $EMSDK_ENV" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$EMSDK_ENV"

if [ ! -f "$RAYLIB_SRC/libraylib.web.a" ]; then
  make -C "$RAYLIB_SRC" PLATFORM=PLATFORM_WEB \
    RAYLIB_LIBTYPE=STATIC RAYLIB_BUILD_MODE=RELEASE -j"$(nproc)"
  cp "$RAYLIB_SRC/libraylib.a" "$RAYLIB_SRC/libraylib.web.a"
fi

mkdir -p "$OUT"
emcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  -DPLATFORM_WEB \
  -I"$RAYLIB_SRC" \
  -o "$OUT/balloon.js" \
  "$ROOT/src/main.c" \
  "$ROOT/src/wind.c" \
  "$ROOT/src/balloon.c" \
  "$ROOT/src/handles.c" \
  "$ROOT/src/terrain.c" \
  "$ROOT/src/terrain_mesh.c" \
  "$ROOT/src/newgen.c" \
  "$ROOT/src/scene.c" \
  "$ROOT/src/watch.c" \
  "$ROOT/src/xr_stub.c" \
  "$RAYLIB_SRC/libraylib.web.a" \
  -s USE_GLFW=3 \
  -s ASYNCIFY \
  -s ASYNCIFY_STACK_SIZE=65536 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=100663296 \
  -s ENVIRONMENT=web \
  -s EXIT_RUNTIME=0 \
  -s FILESYSTEM=0 \
  --no-heap-copy

echo "wrote $OUT/balloon.js"
