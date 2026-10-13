#!/usr/bin/env python3
"""DXVK and VKD3D-Proton for the Windows package (injection into Direct3D
games, capture/src/injection.h): downloads the pinned releases, checks their
SHA256 and extracts the DLLs the launcher copies next to a game:

    OUT/dxvk/x64|x32/{d3d9,d3d10core,d3d11,dxgi}.dll
    OUT/vkd3d-proton/x64|x86/{d3d12,d3d12core}.dll

with their license texts (the release archives have none; the LGPL requires
its text next to the library): OUT/dxvk/LICENSE.txt,
OUT/vkd3d-proton/{LICENSE,COPYING,AUTHORS}.txt.

The same versions as tools/ci/proton-check.sh (tested under Wine through the
layer). VKD3D-Proton's .tar.zst needs the `zstandard` module or the `zstd`
program.

    tools/fetch_translation_layers.py OUT
    tools/fetch_translation_layers.py --source OUT

--source writes OUT/vkd3d-proton-<version>-source.tar.gz instead: the
complete source of the VKD3D-Proton we ship, submodules included, attached
to each release next to the Windows package (LGPL-2.1 section 4: the source
offered from the same place as the binaries).
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

DXVK = ("2.5.3", "d8e6ef7d1168095165e1f8a98c7d5a4485b080467bb573d2a9ef3e3d79ea1eb8")
VKD3D = ("3.0.1", "3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c")
VKD3D_COMMIT = "3b10bd7a7ec6a7347e616cf8bea59333afec2255"  # tag v3.0.1 (its submodules pinned by it)
DXVK_DLLS = ["d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"]
VKD3D_DLLS = ["d3d12.dll", "d3d12core.dll"]
# (repository, file, SHA256) at the pinned tags.
LICENSES = [
    ("dxvk", "doitsujin/dxvk", "LICENSE", "a5cb1a6ded7d2d7e92d550ba28edd21be2d1d4044662b399887351023e30ce64"),
    ("vkd3d-proton", "HansKristian-Work/vkd3d-proton", "LICENSE",
     "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"),
    ("vkd3d-proton", "HansKristian-Work/vkd3d-proton", "COPYING",
     "f51ad4bf5a3bd2b5f383fe28150b23d01e6f5d7b620b194150e7c6a620c43c1f"),
    ("vkd3d-proton", "HansKristian-Work/vkd3d-proton", "AUTHORS",
     "825ecce7c240aa4824cd80f27640cba19c74f7187b1ecdf8c1a2645ddb4d30c3"),
]


def fetch(url, sha):
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        sys.exit(f"{url}: SHA256 {got}, expected {sha}")
    return data


def unzstd(data):
    try:
        import zstandard
        return zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)).read()
    except ImportError:
        if not shutil.which("zstd"):
            sys.exit("VKD3D-Proton is a .tar.zst: install the zstandard module (pip) or zstd")
        return subprocess.run(["zstd", "-dc"], input=data, capture_output=True, check=True).stdout


def extract(tar_bytes, top, dirs, names, out):
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
        for src_dir, dst_dir in dirs:
            os.makedirs(os.path.join(out, dst_dir), exist_ok=True)
            for n in names:
                member = t.getmember(f"{top}/{src_dir}/{n}")
                with t.extractfile(member) as f, open(os.path.join(out, dst_dir, n), "wb") as o:
                    o.write(f.read())


def source(out):
    v = VKD3D[0]
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, f"vkd3d-proton-{v}")
        subprocess.run(["git", "clone", "--quiet", "--depth", "1", "--branch", f"v{v}", "--recurse-submodules",
                        "--shallow-submodules", "https://github.com/HansKristian-Work/vkd3d-proton", src], check=True)
        head = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
        if head != VKD3D_COMMIT:
            sys.exit(f"vkd3d-proton v{v} is {head}, expected {VKD3D_COMMIT}")
        os.makedirs(out, exist_ok=True)
        path = os.path.join(out, f"vkd3d-proton-{v}-source.tar.gz")
        with tarfile.open(path, "w:gz") as t:
            t.add(src, arcname=os.path.basename(src), filter=lambda m: None if
                  os.path.basename(m.name) == ".git" else m)
    print(f"{path} ({os.path.getsize(path) // 1024} KiB)")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--source":
        return source(sys.argv[2])
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    v, sha = DXVK
    data = fetch(f"https://github.com/doitsujin/dxvk/releases/download/v{v}/dxvk-{v}.tar.gz", sha)
    extract(data, f"dxvk-{v}", [("x64", "dxvk/x64"), ("x32", "dxvk/x32")], DXVK_DLLS, out)
    v, sha = VKD3D
    data = fetch(f"https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v{v}/vkd3d-proton-{v}.tar.zst", sha)
    extract(unzstd(data), f"vkd3d-proton-{v}", [("x64", "vkd3d-proton/x64"), ("x86", "vkd3d-proton/x86")], VKD3D_DLLS, out)
    for folder, repo, name, sha in LICENSES:
        tag = DXVK[0] if folder == "dxvk" else VKD3D[0]
        with open(os.path.join(out, folder, name + ".txt"), "wb") as f:
            f.write(fetch(f"https://raw.githubusercontent.com/{repo}/v{tag}/{name}", sha))
    with open(os.path.join(out, "dxvk", "VERSION.txt"), "w") as f:
        f.write(f"DXVK {DXVK[0]} (zlib license, https://github.com/doitsujin/dxvk)\n")
    with open(os.path.join(out, "vkd3d-proton", "VERSION.txt"), "w") as f:
        f.write(f"VKD3D-Proton {VKD3D[0]} (LGPL-2.1, LICENSE.txt; complete source: "
                f"vkd3d-proton-{VKD3D[0]}-source.tar.gz on the release page this package comes from, "
                f"and https://github.com/HansKristian-Work/vkd3d-proton/tree/v{VKD3D[0]})\n")
    print(f"DXVK {DXVK[0]} and VKD3D-Proton {VKD3D[0]} in {out}")


if __name__ == "__main__":
    main()
