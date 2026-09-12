#version 450
layout(location = 0) out vec2 vUV;
void main() {
    // Single triangle covering the viewport.
    vec2 pos = vec2((gl_VertexIndex & 1) * 4.0 - 1.0, (gl_VertexIndex & 2) * 2.0 - 1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
