#version 450
// Procedural test scene: scrolling backdrop, several objects moving at
// different speeds, a rotating textured square (pure rotation), a static
// "HUD" and a frame counter so that generated frames are easy to tell apart
// from real ones.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec2 resolution;
    float time;
    uint frame;
    uint variant;  // scene variant: 1 = different backdrop/objects (scene cut test)
} pc;

float sdBox(vec2 p, vec2 b) { vec2 d = abs(p) - b; return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0); }

// 3x5 pixel font for digits, one bit per pixel (row-major, top row first).
uint digitRows(uint d, uint row) {
    const uint font[10] = uint[10](
        0x7B6Fu, // 0: 111 101 101 101 111
        0x2492u, // 1: 010 010 010 010 010
        0x73E7u, // 2: 111 001 111 100 111
        0x73CFu, // 3: 111 001 111 001 111
        0x5BC9u, // 4: 101 101 111 001 001
        0x79CFu, // 5: 111 100 111 001 111
        0x79EFu, // 6: 111 100 111 101 111
        0x7249u, // 7: 111 001 010 010 010
        0x7BEFu, // 8: 111 101 111 101 111
        0x7BCFu  // 9: 111 101 111 001 111
    );
    return (font[d] >> (3u * (4u - row))) & 7u;
}

bool digitPixel(uint d, ivec2 p) {
    if (p.x < 0 || p.x > 2 || p.y < 0 || p.y > 4) return false;
    return ((digitRows(d, uint(p.y)) >> uint(2 - p.x)) & 1u) != 0u;
}

void main() {
    vec2 res = pc.resolution;
    vec2 px = vUV * res;
    float aspect = res.x / res.y;
    vec2 uv = vec2(vUV.x * aspect, vUV.y);
    float t = pc.time;

    // Scrolling checkerboard backdrop (slow horizontal pan).
    vec2 g = floor((uv + vec2(t * 0.15, 0.0)) * 8.0);
    float chk = mod(g.x + g.y, 2.0);
    vec3 col = mix(vec3(0.12, 0.13, 0.18), vec3(0.18, 0.2, 0.26), chk);
    if (pc.variant == 1u) {
        // Completely different scene: warm vertical stripes scrolling the other way.
        float st = step(0.5, fract((uv.x - t * 0.3) * 6.0));
        col = mix(vec3(0.45, 0.2, 0.1), vec3(0.7, 0.5, 0.2), st);
        t = -t * 1.7 + 100.0;
    }

    // Fast horizontal bar bouncing across the screen.
    float bx = 0.5 * aspect + 0.45 * aspect * sin(t * 2.2);
    float bar = 1.0 - smoothstep(0.02, 0.025, abs(uv.x - bx));
    col = mix(col, vec3(0.95, 0.85, 0.2), bar * step(0.25, vUV.y) * step(vUV.y, 0.75));

    // Circles orbiting at different speeds and sizes.
    for (int i = 0; i < 5; ++i) {
        float fi = float(i);
        float speed = 0.6 + fi * 0.5;
        vec2 c = vec2(0.5 * aspect + (0.25 + 0.08 * fi) * cos(t * speed + fi), 0.5 + (0.2 + 0.05 * fi) * sin(t * speed * 1.3 + fi));
        float r = 0.03 + 0.012 * fi;
        float d = length(uv - c) - r;
        vec3 cc = 0.5 + 0.5 * cos(fi * 1.7 + vec3(0.0, 2.1, 4.2));
        col = mix(col, cc, 1.0 - smoothstep(0.0, 0.004, d));
        col = mix(col, vec3(0.0), (1.0 - smoothstep(0.0, 0.004, abs(d))) * 0.7);
    }

    // Diagonal moving box with texture (tests rotation-free translation).
    vec2 bc = vec2(mod(t * 0.35, aspect + 0.4) - 0.2, 0.75 + 0.1 * sin(t * 3.0));
    float bd = sdBox(uv - bc, vec2(0.12, 0.07));
    if (bd < 0.0) {
        vec2 lp = (uv - bc) * 40.0;
        float stripes = step(0.5, fract(lp.x + lp.y));
        col = mix(vec3(0.9, 0.3, 0.3), vec3(0.3, 0.3, 0.9), stripes);
    }

    // Rotating textured square at a fixed position: pure rotation, which
    // block-matching optical flow (translation only) can approximate at best
    // piecewise. The internal checkerboard makes the angular motion visible.
    {
        vec2 rc = vec2(0.24 * aspect, 0.32);
        float a = t * 1.4;
        float ca = cos(a), sa = sin(a);
        vec2 p = uv - rc;
        vec2 pr = vec2(ca * p.x + sa * p.y, -sa * p.x + ca * p.y);
        if (sdBox(pr, vec2(0.1, 0.1)) < 0.0) {
            vec2 cell = floor(pr * 22.0);
            float chk2 = mod(cell.x + cell.y, 2.0);
            col = mix(vec3(0.15, 0.65, 0.72), vec3(0.95, 0.9, 0.35), chk2);
        }
    }

    // Static HUD: bar at the bottom with a slowly moving marker.
    if (vUV.y < 0.08) {
        col = vec3(0.05, 0.05, 0.07);
        float m = fract(t * 0.1) * aspect;
        col = mix(col, vec3(0.2, 0.9, 0.4), 1.0 - smoothstep(0.004, 0.006, abs(uv.x - m)) );
        if (abs(vUV.y - 0.04) < 0.004) col = mix(col, vec3(0.6), 0.5);
    }

    // Frame counter (top-left), 6 digits, 8x scaled pixel font.
    {
        const int scale = 6;
        ivec2 ip = ivec2(px.x, res.y - px.y) - ivec2(16, 16);
        if (ip.x >= 0 && ip.y >= 0 && ip.x < 6 * 4 * scale && ip.y < 5 * scale) {
            int digit = ip.x / (4 * scale);
            ivec2 lp = ivec2((ip.x - digit * 4 * scale) / scale, ip.y / scale);
            const uint pow10[6] = uint[6](100000u, 10000u, 1000u, 100u, 10u, 1u);
            uint dv = (pc.frame / pow10[digit]) % 10u;
            col = mix(col * 0.3, vec3(1.0), digitPixel(dv, lp) ? 1.0 : 0.0);
        }
    }

    outColor = vec4(col, 1.0);
}
