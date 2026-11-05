# Linux build/test environment for Folia (portable baseline: Qt 6 Widgets,
# vendored Lua + SQLite). Debian 13 "trixie" ships Qt 6.8.
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build qt6-base-dev libgl-dev libxkbcommon-dev \
      fonts-dejavu-core fonts-noto-cjk xvfb xauth ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
