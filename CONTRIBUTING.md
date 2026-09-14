# Contributing to bdex-framegen

Thanks for your interest in the project. Bug reports, test reports from real
games, ideas and pull requests are all welcome.

## Reporting a bug or a game that does not work

The most useful thing is a test report from a real game: so far the layer has
only been validated with the demo, `vkcube`, `vkgears` and the Steam Linux
Runtime container. Open an issue with:

1. **The layer log** at verbosity 2 (or 3 if the problem happens while
   loading):
   ```
   BDEX_FG=1 BDEX_FG_LOG=2 BDEX_FG_LOG_FILE=/tmp/bdex.log %command%
   ```
   The first lines show the configuration, the GPU, the queue in use and the
   swapchain format; that is often enough to understand what is going on.
2. **Your environment**: distribution, GPU and driver (`vulkaninfo --summary`),
   compositor (Wayland/X11), Vulkan loader version.
3. **The game** and how it is launched (native, DXVK, VKD3D-Proton, Proton
   version), including the launch options.
4. **The symptom**: no effect, crash, black screen, artefacts, stutter…
   For artefacts, one screenshot with `BDEX_FG_DEBUG=flow` and one with
   `BDEX_FG_DEBUG=split` help a lot.

If the game crashes, first check that it works with
`BDEX_FG_DEBUG=passthrough` (layer loaded, swapchain virtualised, but no
generation): this separates a Vulkan integration problem from a problem in
the shaders.

## Proposing a change

### Setup

```sh
git clone git@github.com:Bdexez/bdex_frame_gen.git
cd bdex_frame_gen
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The layer can be tested without installing it:

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer BDEX_FG=1 ./build/demo/bdex_demo --fps 30
```

### Before opening a pull request

- `ctest` passes (unit tests + end-to-end test).
- The layer stays clean under the Khronos validation layer, synchronization
  validation included:
  ```sh
  VK_LOADER_LAYERS_ENABLE='*validation' BDEX_FG=1 build/demo/bdex_demo --fps 30 --frames 90
  ```
  (to enable synchronization validation, see `khronos_validation.validate_sync`
  in a `vk_layer_settings.txt` file).
- For any change to the shaders or the interpolation, attach the before/after
  numbers from `tools/run_eval.sh build 40`, as well as
  `BDEX_EVAL_ARGS="--speed 2.5" tools/run_eval.sh build 40` for fast motion.
  A PSNR improvement on the demo is not absolute proof, but a regression is a
  good one.
- For any change affecting performance, attach a statistics line with
  `BDEX_FG_PROFILE=1` before and after.
- One PR = one topic. Commit messages are written in the imperative, with a
  body explaining *why* when it is not obvious.
- Add an entry to `CHANGELOG.md` (*Unreleased* section).

### Style

- C++20, `-Wall -Wextra` with no warnings. No external dependency beyond the
  Vulkan headers (the layer is loaded into the game's process: it must stay
  light and pull in nothing surprising).
- Vulkan functions are always called through the dispatch tables
  (`dev.vt.*`, `inst->vt.*`), never through the loader.
- Every dispatchable object created by the layer (queue, command buffer) goes
  through `adoptDispatch()`; every object created must be destroyed in
  `destroyAll()`.
- Shaders are compiled for SPIR-V 1.0 (`--target-env=vulkan1.0`) so that they
  work with Vulkan 1.0 applications; do not use a SPIR-V extension without a
  fallback.
- Configuration options are declared in `config.h`, parsed in
  `Config::apply()`, tested in `tests/test_config.cpp` and documented in the
  README table.

## Ideas for contributions

- Test reports from DXVK / VKD3D-Proton games and from NVIDIA / Intel GPUs.
- Finer flow estimation (dense per-pixel flow, occlusion handling).
- On-screen indicator (real / displayed fps) independent of the swapchain format.
- Support for more formats (10-bit HDR with the PQ colour space).
- Distribution packages (PKGBUILD, Flatpak).

## License

By contributing, you agree that your code is released under the project's MIT
license.
