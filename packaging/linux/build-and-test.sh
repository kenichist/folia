#!/bin/sh
# Usage (from the repo root):
#   docker build -t folia-linux packaging/linux
#   docker run --rm -v "$PWD":/src:ro folia-linux sh /src/packaging/linux/build-and-test.sh
set -eu
cmake -S /src -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /work/build
cd /work/build
QT_QPA_PLATFORM=offscreen ctest --output-on-failure
ls -la /work/build/src/Folia
