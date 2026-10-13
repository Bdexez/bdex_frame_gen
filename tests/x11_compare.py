#!/usr/bin/env python3
"""x11_compare.py GAME_TITLE OVERLAY_TITLE: reads the two windows' pixels
through Xlib (ctypes, no module needed) — the game window's own content (it
is redirected by bdex-capture, so XGetImage gives its pixmap, not the overlay
above it) and what the overlay shows — and compares them: PSNR over the
shared area, after a moment. Prints "same scene (PSNR N dB)" or why not."""
import ctypes
import ctypes.util
import sys
import time

x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                         ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
x.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
x.XGetImage.restype = ctypes.c_void_p
x.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                        ctypes.c_ulong, ctypes.c_int]
x.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                           ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                           ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
x.XFree.argtypes = [ctypes.c_void_p]


class XImage(ctypes.Structure):  # the leading fields of Xlib's XImage
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int),
                ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]


def windows(dpy, w, depth=0):
    root, parent = ctypes.c_ulong(), ctypes.c_ulong()
    children, n = ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
    if not x.XQueryTree(dpy, w, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(children), ctypes.byref(n)):
        return
    kids = [children[i] for i in range(n.value)]
    if children:
        x.XFree(children)
    for k in kids:
        yield k
        if depth < 3:
            yield from windows(dpy, k, depth + 1)


def find(dpy, title):
    for w in windows(dpy, x.XDefaultRootWindow(dpy)):
        name = ctypes.c_char_p()
        if x.XFetchName(dpy, w, ctypes.byref(name)) and name.value:
            if title.encode() in name.value:
                return w
    return None


def grab(dpy, w):
    root = ctypes.c_ulong()
    gx, gy = ctypes.c_int(), ctypes.c_int()
    gw, gh, b, d = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
    x.XGetGeometry(dpy, w, ctypes.byref(root), ctypes.byref(gx), ctypes.byref(gy), ctypes.byref(gw), ctypes.byref(gh),
                   ctypes.byref(b), ctypes.byref(d))
    p = x.XGetImage(dpy, w, 0, 0, gw.value, gh.value, 0xFFFFFFFF, 2)  # ZPixmap
    if not p:
        return None
    img = ctypes.cast(p, ctypes.POINTER(XImage)).contents
    raw = ctypes.string_at(img.data, img.bytes_per_line * img.height)
    return img.width, img.height, img.bytes_per_line, raw


def main():
    dpy = x.XOpenDisplay(None)
    if not dpy:
        print("no display")
        return 1
    game, ov = find(dpy, sys.argv[1]), find(dpy, sys.argv[2])
    if not game or not ov:
        print(f"window not found (game {game}, overlay {ov})")
        return 1
    a, b = grab(dpy, game), grab(dpy, ov)
    if not a or not b:
        print("XGetImage failed")
        return 1
    w, h = min(a[0], b[0]), min(a[1], b[1])
    se, n, lit = 0.0, 0, 0
    for yy in range(0, h, 4):
        ra, rb = yy * a[2], yy * b[2]
        for xx in range(0, w, 4):
            for c in range(3):
                va, vb = a[3][ra + xx * 4 + c], b[3][rb + xx * 4 + c]
                se += (va - vb) ** 2
                n += 1
                lit += vb > 16
    if lit < n * 0.05:
        print("overlay black")
        return 1
    import math
    psnr = 99.0 if se == 0 else 10 * math.log10(255 * 255 / (se / n))
    # Real or generated frame of a moving scene, some frames apart: the same
    # scene is ~15 dB and up; another picture (black, garbage) well under 10.
    print(("same scene" if psnr >= 12 else "different picture") + f" (PSNR {psnr:.1f} dB)")
    return 0


sys.exit(main())
