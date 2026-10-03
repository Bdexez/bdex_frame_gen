FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    cmake make gcc g++ pkg-config glslc libvulkan-dev vulkan-tools mesa-vulkan-drivers libglfw3-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxext-dev xvfb xauth openbox picom \
    python3-numpy python3-pil python3-yaml vulkan-validationlayers git ca-certificates && rm -rf /var/lib/apt/lists/*
