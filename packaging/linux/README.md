# Linux build

Verified in a container (see `Dockerfile`): Debian 13 "trixie", aarch64
(Docker Desktop VM on the M1), GCC (Debian default), Qt 6.8 from Debian
packages, headless tests with `QT_QPA_PLATFORM=offscreen`.

```sh
docker build -t folia-linux packaging/linux
docker run --rm -v "$PWD":/src:ro folia-linux sh /src/packaging/linux/build-and-test.sh
```

Native build on a Linux desktop (x86-64 or arm64):

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev libgl-dev libxkbcommon-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
./build/src/Folia
```

The container run is a build/test verification, **not** a reference-machine
measurement: no Linux hardware benchmarks have been run (unmeasured). The
footprint counter on Linux is VmRSS from /proc/self/status.
