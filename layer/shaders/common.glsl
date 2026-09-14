// Shared helpers for the frame generation compute shaders.

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Cheap perceptual encoding used for matching so that dark regions are not
// crushed when the swapchain hands us linear (sRGB-decoded) values.
float perceptual(float l) { return sqrt(max(l, 0.0)); }

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(0.0031308, c));
}

vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

// --- On-screen frame-rate overlay (HUD) -----------------------------------
// Up to three stacked lines in the top-left corner, drawn with a 3x5 bitmap
// font onto whatever frame is being produced, so both the real and the
// generated frames carry it and it never flickers. Detail grows with `mode`:
//   1  output fps only
//   2  <game fps> ">" <output fps>
//   3  ... plus a "1%" line: the 1% low
//   4  ... plus a ".1%" line: the 0.1% low
// The low figures are the game fps at the 99th / 99.9th percentile of the
// game's frame time (source-side lows: frame generation paces the output
// evenly, so its own lows would be meaningless).

// 3x5 digit glyphs (row-major, top row first), one bit per pixel.
const uint HUD_FONT[10] = uint[10](0x7B6Fu, 0x2492u, 0x73E7u, 0x73CFu, 0x5BC9u,
                                   0x79CFu, 0x79EFu, 0x7249u, 0x7BEFu, 0x7BCFu);

// Glyphs: 0-9 digits, 10 = ">" separator, 11 = "%", 12 = ".".
bool hudGlyphBit(int glyph, int row, int col) {
    if (row < 0 || row > 4 || col < 0 || col > 2) return false;
    if (glyph >= 0 && glyph <= 9) return ((HUD_FONT[glyph] >> (3 * (4 - row) + (2 - col))) & 1u) != 0u;
    if (glyph == 10) return (row <= 2 && col == row) || (row > 2 && col == 4 - row);   // ">"
    if (glyph == 11) return (row == 0 && col != 1) || (row == 4 && col != 1) ||        // "%"
                            (row == 1 && col == 2) || (row == 2 && col == 1) || (row == 3 && col == 0);
    if (glyph == 12) return row == 4 && col == 1;                                       // "."
    return false;
}

int hudDigits(int v) { return v < 10 ? 1 : v < 100 ? 2 : v < 1000 ? 3 : 4; }
int hudPow10(int n) { int p = 1; for (int i = 0; i < n; ++i) p *= 10; return p; }
int hudLineCount(int mode) { return mode <= 2 ? 1 : mode - 1; }  // 1,1,2,3 for modes 1..4

// Append the decimal digits of v to the glyph list.
void hudPushNum(int v, inout int gl[10], inout int n) {
    v = clamp(v, 0, 9999);
    int d = hudDigits(v);
    for (int i = 0; i < d; ++i) gl[n++] = (v / hudPow10(d - 1 - i)) % 10;
}

// Fill `gl`/`n` with the glyphs of one HUD line; kind 1 = a "low" line (amber).
void hudBuildLine(int mode, int line, int g, int o, int l1, int l01,
                  out int gl[10], out int n, out int kind) {
    n = 0; kind = 0;
    for (int i = 0; i < 10; ++i) gl[i] = 0;
    if (line == 0) {
        if (mode == 1) { hudPushNum(o, gl, n); }
        else { hudPushNum(g, gl, n); gl[n++] = 10; hudPushNum(o, gl, n); }   // g > o
    } else if (line == 1) {
        kind = 1; gl[n++] = 1; gl[n++] = 11; hudPushNum(l1, gl, n);          // "1%" + value
    } else {
        kind = 1; gl[n++] = 12; gl[n++] = 1; gl[n++] = 11; hudPushNum(l01, gl, n);  // ".1%" + value
    }
}

// If pixel p lands on the HUD, returns true and writes its colour.
bool hudPixel(ivec2 p, int mode, int gameFps, int outFps, int low1, int low01, out vec3 col) {
    if (mode <= 0) return false;
    const int S = 4;              // pixel scale of the 3x5 font
    const int CELL = 4 * S;       // glyph cell width (3 glyph columns + 1 gap)
    const int LINEH = 6 * S;      // 5 font rows + 1 gap
    const ivec2 origin = ivec2(14, 14);
    int lines = hudLineCount(mode);

    int gl[10]; int n; int kind;
    int maxCells = 0;
    for (int L = 0; L < lines; ++L) {
        hudBuildLine(mode, L, gameFps, outFps, low1, low01, gl, n, kind);
        maxCells = max(maxCells, n);
    }
    int boxW = maxCells * CELL + S, boxH = lines * LINEH + S;

    ivec2 rel = p - origin + ivec2(S, S);  // include padding
    if (rel.x < 0 || rel.y < 0 || rel.x >= boxW || rel.y >= boxH) return false;

    col = vec3(0.03);             // dark background box (opaque, always readable)
    ivec2 t = rel - ivec2(S, S);  // text-area coordinate
    if (t.x < 0 || t.y < 0) return true;
    int line = t.y / LINEH;
    int ly = t.y - line * LINEH;
    if (line >= lines || ly >= 5 * S) return true;   // inter-line gap
    hudBuildLine(mode, line, gameFps, outFps, low1, low01, gl, n, kind);
    int cell = t.x / CELL, gx = t.x - cell * CELL;
    if (cell >= n || gx >= 3 * S) return true;
    if (hudGlyphBit(gl[cell], ly / S, gx / S))
        col = kind == 1 ? vec3(1.0, 0.75, 0.1) : vec3(0.15, 1.0, 0.35);   // amber lows / green
    return true;
}
