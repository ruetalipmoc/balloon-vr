#!/bin/sh
# Download and build raylib 5.5 (static, X11) and the OpenXR loader.
# Installs the loader into third_party/install. Raylib stays in
# third_party/raylib/src, which is where the Makefile looks.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p third_party
cd third_party

if [ ! -f raylib/src/libraylib.a ]; then
    echo "fetching raylib 5.5"
    curl -fL --retry 3 -o raylib-5.5.tar.gz https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    tar -xzf raylib-5.5.tar.gz
    rm -f raylib-5.5.tar.gz
    mv raylib-5.5 raylib
    make -C raylib/src PLATFORM=PLATFORM_DESKTOP \
        GLFW_LINUX_ENABLE_X11=TRUE GLFW_LINUX_ENABLE_WAYLAND=FALSE -j"$(nproc)"
fi

if [ ! -f install/include/openxr/openxr.h ]; then
    echo "fetching OpenXR SDK 1.1.65"
    curl -fL --retry 3 -o openxr.tar.gz https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.65.tar.gz
    tar -xzf openxr.tar.gz
    rm -f openxr.tar.gz
    cmake -S OpenXR-SDK-release-1.1.65 -B openxr-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$ROOT/third_party/install" \
        -DBUILD_LOADER=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_API_LAYERS=OFF
    cmake --build openxr-build -j"$(nproc)" --target openxr_loader
    cmake --install openxr-build
fi

echo "dependencies ready"
