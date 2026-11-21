#version 450
layout(push_constant) uniform PC { vec4 rect; vec4 color; } pc;   // rect = x, y, w, h in NDC
layout(location = 0) out vec4 vColor;
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(pc.rect.xy + p * pc.rect.zw, 0.0, 1.0);
    vColor = pc.color;
}
