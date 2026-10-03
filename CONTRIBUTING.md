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
2. **Your environment**: `bdex-framegen --report` writes system, session,
   GPU/driver, installed manifests, relevant environment variables, the
   config file and the end of the log into `bdex-framegen-report.txt` —
   attach it (read it first for anything private).
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
VK_IMPLICIT_LAYER_PATH=$PWD/build/layer BDEX_FG=1 ./build/demo/bdex_demo --fps 30
```

### Before opening a pull request

- `tools/ci/run.sh` (and `tools/ci/run.sh asan`, `tsan`) replays the Linux
  CI in Docker with the runner's Vulkan loader, when a change touches the CI,
  the tests or how they find the layer.
- `ctest` passes (unit tests, evaluator checks, headless engine test, frame-generation and acquire-timeout GPU tests).
- The layer stays clean under the Khronos validation layer, synchronization
  validation included (`ctest -R validation` checks both modes with every
  optional pass enabled; by hand:
  ```sh
  VK_LOADER_LAYERS_ENABLE='*validation' BDEX_FG=1 build/demo/bdex_demo --fps 30 --frames 90
  ```
  with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` for synchronization
  validation).
- For any change to the shaders or the interpolation, attach the before/after
  numbers from `tools/run_eval.sh build 40`, as well as
  `BDEX_EVAL_ARGS="--speed 2.5" tools/run_eval.sh build 40` for fast motion.
  Include the moving-edge and changing-region scores; for multiplier changes,
  also run with `BDEX_FG_MULTIPLIER=3` and `4`. A PSNR improvement on the demo
  is not absolute proof, but a regression is a good signal. The runner ignores
  the local config file; set experimental options with `BDEX_FG_*` variables.
- The demo is one scene. For quality changes, also replay recorded game
  sequences offline (no window, no compositor, identical inputs for every
  variant). Record the real frames of a game through the layer, then score
  the defaults against your option on those frames:
  ```sh
  BDEX_FG=1 BDEX_FG_MULTIPLIER=1 BDEX_FG_DUMP=/tmp/seq BDEX_FG_DUMP_FRAMES=240 %command%   # Steam launch options
  tools/eval_sequence.py /tmp/seq --variant OCCLUSION_STRENGTH=1 [--multiplier 3] [--exclude x0,y0,x1,y1]
  ```
  Every `multiplier`-th frame is an input and the ones in between are the
  ground truth. The report gives whole-frame, moving-region and moving-edge
  PSNR, SSIM, a flicker score (how much the error changes between successive
  generated frames), and what plain frame repetition (`dup`) and a
  cross-fade (`blend`) score on the same frames. `ctest -R corpus` replays a synthetic corpus
  (`tools/make_corpus.py`) plus any sequence directories under
  `$BDEX_CORPUS_DIR` against `tests/corpus_reference.json`; after an intended
  quality change, refresh it with `python3 tests/test_corpus.py build --update`
  and commit the diff with the numbers. Dumping slows the game down, which only makes
  the motion between frames larger; move the camera while recording.
- For any change affecting performance, attach a statistics line with
  `BDEX_FG_PROFILE=1` before and after.
- One PR = one topic. Commit messages are written in the imperative, with a
  body explaining *why* when it is not obvious.
- Add an entry to `CHANGELOG.md` (*Unreleased* section).

### Adding a game to the evaluation corpus

Measurements on real games beat the synthetic scenes. To add one:

1. Record its real frames (generation off):
   `bdex-framegen --record ~/rec/mygame 240 -- %command%` (Steam launch
   options) or with any command. Play a few seconds of representative
   motion — camera pans, characters, the HUD.
2. Pack a window of it, downscaled for the CI's software Vulkan:
   `tools/corpus_pack.py ~/rec/mygame mygame-town.tar.gz --name mygame-town --start 60 --count 121 --note "…"`
3. Check it locally: `tools/corpus_fetch.py /tmp/real mygame-town.tar.gz`,
   then `BDEX_CORPUS_DIR=/tmp/real tests/test_corpus.py build --update`
   writes its references into `tests/corpus_reference.json`.
4. Attach the archive to the repository's GitHub release named `corpus`
   (`gh release upload corpus mygame-town.tar.gz`) and commit the
   references: the Linux CI scores every archive of that release on each
   push. Mind the game's rights before publishing frames of it.

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

The project is proprietary (see `LICENSE`). Contributions are accepted only
from people the author invited; by contributing you assign the copyright of
your contribution to the project's author, so the project can stay under a
single owner.
