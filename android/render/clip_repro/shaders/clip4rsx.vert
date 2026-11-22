#version 450
// the shape the RSX decompiler emits (float4 SV_ClipDistance0 = (0.5, d, 0.5, 0.5)): only plane 1 carries the test value
layout(location = 0) in vec2 pos;
layout(location = 1) in float dist;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };
void main() { gl_Position = vec4(pos, 0.0, 1.0); gl_ClipDistance[0] = 0.5; gl_ClipDistance[1] = dist; gl_ClipDistance[2] = 0.5; gl_ClipDistance[3] = 0.5; }
