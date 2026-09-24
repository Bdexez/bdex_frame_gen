# Sourced by the test and evaluation scripts: make the Vulkan loader load the
# layer of one given build, and only that one.
#
#   . tools/layer_env.sh; bdex_use_layer <build>/layer
#
# VK_IMPLICIT_LAYER_PATH (which replaces every search path) only exists in
# recent loaders; older ones — Ubuntu 24.04 ships 1.3.275 — ignore it, as
# they may ignore VK_ADD_IMPLICIT_LAYER_PATH. Every loader searches
# $XDG_DATA_HOME/vulkan/implicit_layer.d, so the build's manifest (which holds
# an absolute library_path) is also copied into a private XDG_DATA_HOME.
# That hides a layer installed in ~/.local/share, but not one installed
# system-wide (/usr/share, /etc) on an old loader: CI installs nothing there.
bdex_use_layer() {
  local dir="$1" m
  BDEX_LAYER_XDG="$(mktemp -d)"
  mkdir -p "$BDEX_LAYER_XDG/vulkan/implicit_layer.d"
  for m in "$dir"/*.json; do
    [ -f "$m" ] && cp "$m" "$BDEX_LAYER_XDG/vulkan/implicit_layer.d/"
  done
  export XDG_DATA_HOME="$BDEX_LAYER_XDG" VK_IMPLICIT_LAYER_PATH="$dir"
  unset VK_ADD_IMPLICIT_LAYER_PATH
}
