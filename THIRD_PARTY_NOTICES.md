# Third-party notices

bdex-framegen itself is proprietary (see `LICENSE`). It contains or ships
the following third-party components, under their own licenses:

## AMD FidelityFX Contrast Adaptive Sharpening (CAS)

`layer/shaders/cas.comp` is an adaptation of the CAS algorithm.

Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Khronos Vulkan headers

`layer/src/vk_compat.h` copies a few definitions from the Khronos Vulkan
headers (1.3.260), licensed Apache-2.0 OR MIT.

Copyright 2015-2023 The Khronos Group Inc.

## DXVK (Windows package)

The Windows package ships DXVK's `d3d9.dll`, `d3d10core.dll`, `d3d11.dll`
and `dxgi.dll` (folder `dxvk/`, version in `dxvk/VERSION.txt`, license in
`dxvk/LICENSE.txt`), unmodified, copied next to a game's executable when the
launcher injects into a Direct3D 9/10/11 game.

Copyright (c) 2017 Philip Rebohle, Copyright (c) 2019 Joshua Ashton

zlib/libpng license:

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## VKD3D-Proton (Windows package)

The Windows package ships VKD3D-Proton's `d3d12.dll` and `d3d12core.dll`
(folder `vkd3d-proton/`, version in `vkd3d-proton/VERSION.txt`), unmodified,
copied next to a game's executable when the launcher injects into a
Direct3D 12 game.

Copyright (c) the VKD3D-Proton authors (HansKristian-Work/vkd3d-proton, a
fork of Wine's vkd3d, Copyright (c) the Wine project authors).

Licensed under the GNU Lesser General Public License, version 2.1 or later:
its full text is in `vkd3d-proton/LICENSE.txt`, the copyright notices in
`vkd3d-proton/COPYING.txt` and `vkd3d-proton/AUTHORS.txt`. They are separate,
dynamically loaded libraries, not linked with bdex-framegen: you may replace
them with your own build of VKD3D-Proton. Their complete corresponding
source code (tag v3.0.1, submodules included) is published on the same
release page as the Windows package, `vkd3d-proton-3.0.1-source.tar.gz`, and
upstream at https://github.com/HansKristian-Work/vkd3d-proton.

## GLFW (demo program only)

`bdex_demo`, the test program, links against GLFW (zlib/libpng license,
Copyright (c) 2002-2006 Marcus Geelnard, Copyright (c) 2006-2019 Camilla
Löwy). It is not part of the distributed packages.
