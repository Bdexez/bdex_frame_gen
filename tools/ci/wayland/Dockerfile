# bdex-capture --wayland, end to end without a screen: sway headless (two
# outputs), PipeWire + WirePlumber, xdg-desktop-portal with the wlr backend
# choosing the first output itself, Xwayland, lavapipe.
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed base-devel cmake ninja git python python-numpy python-pillow \
      vulkan-headers vulkan-icd-loader vulkan-swrast shaderc glslang glfw \
      libx11 libxcomposite libxdamage libxext libxfixes libxrandr \
      sway xorg-xwayland xdg-desktop-portal xdg-desktop-portal-wlr pipewire wireplumber dbus \
      && pacman -Scc --noconfirm
