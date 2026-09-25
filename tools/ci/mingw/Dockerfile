FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    cmake make g++ glslc libvulkan-dev pkg-config \
    g++-mingw-w64-x86-64-posix g++-mingw-w64-i686-posix && rm -rf /var/lib/apt/lists/*
