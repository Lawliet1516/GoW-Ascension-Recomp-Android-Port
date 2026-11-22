#!/usr/bin/env bash
set -euo pipefail
cmake -S /root/work/colab-station/src/SDL3-3.4.12 -B /root/work/colab-station/runs/T3a-sdl3/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/root/work/colab-station/toolchain/android-ndk-r29/build/cmake/android.toolchain.cmake   -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF   -DCMAKE_INSTALL_PREFIX=/root/work/colab-station/toolchain/sdl3-android-arm64
cmake --build /root/work/colab-station/runs/T3a-sdl3/build -j2
cmake --install /root/work/colab-station/runs/T3a-sdl3/build
