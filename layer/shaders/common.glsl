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
// A small "<game fps> > <output fps>" readout in the top-left corner, drawn
// with a 3x5 bitmap font onto whatever frame is being produced, so both the
// real and the generated frames carry it and it never flickers.

// 3x5 digit glyphs (row-major, top row first), one bit per pixel.
const uint HUD_FONT[10] = uint[10](0x7B6Fu, 0x2492u, 0x73E7u, 0x73CFu, 0x5BC9u,
                                   0x79CFu, 0x79EFu, 0x7249u, 0x7BEFu, 0x7BCFu);

bool hudGlyphBit(int glyph, int row, int col) {
    if (row < 0 || row > 4 || col < 0 || col > 2) return false;
    if (glyph >= 0 && glyph <= 9) return ((HUD_FONT[glyph] >> (3 * (4 - row) + (2 - col))) & 1u) != 0u;
    if (glyph == 10) return (row <= 2 && col == row) || (row > 2 && col == 4 - row);  // ">" separator
    return false;
}

int hudDigits(int v) { return v < 10 ? 1 : v < 100 ? 2 : v < 1000 ? 3 : 4; }
int hudPow10(int n) { int p = 1; for (int i = 0; i < n; ++i) p *= 10; return p; }

// If pixel p lands on the HUD, returns true and writes its colour.
bool hudPixel(ivec2 p, int gameFps, int outFps, out vec3 col) {
    const int S = 4;              // pixel scale of the 3x5 font
    const int CELL = 4 * S;       // glyph cell width (3 glyph columns + 1 gap)
    const ivec2 origin = ivec2(14, 14);
    gameFps = clamp(gameFps, 0, 9999);
    outFps = clamp(outFps, 0, 9999);
    int gc = hudDigits(gameFps), oc = hudDigits(outFps);
    int cells = gc + 1 + oc;      // game digits, ">" separator, output digits
    int boxW = cells * CELL + S, boxH = 5 * S + 2 * S;

    ivec2 rel = p - origin + ivec2(S, S);  // include padding
    if (rel.x < 0 || rel.y < 0 || rel.x >= boxW || rel.y >= boxH) return false;

    col = vec3(0.03);             // dark background box (opaque, always readable)
    ivec2 t = rel - ivec2(S, S);  // text-area coordinate
    if (t.x < 0 || t.y < 0 || t.y >= 5 * S) return true;
    int cell = t.x / CELL, gx = t.x - cell * CELL;
    if (cell >= cells || gx >= 3 * S) return true;
    int glyph;
    if (cell < gc) glyph = (gameFps / hudPow10(gc - 1 - cell)) % 10;
    else if (cell == gc) glyph = 10;                                     // separator
    else glyph = (outFps / hudPow10(oc - 1 - (cell - gc - 1))) % 10;
    if (hudGlyphBit(glyph, t.y / S, gx / S)) col = vec3(0.15, 1.0, 0.35);  // bright green digits
    return true;
}
