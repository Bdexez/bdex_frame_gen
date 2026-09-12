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
