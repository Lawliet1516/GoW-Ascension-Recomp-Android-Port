#version 450
// four clip distances, all equal to the attribute
layout(location = 0) in vec2 pos;
layout(location = 1) in float dist;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };
void main() { gl_Position = vec4(pos, 0.0, 1.0); gl_ClipDistance[0] = dist; gl_ClipDistance[1] = dist; gl_ClipDistance[2] = dist; gl_ClipDistance[3] = dist; }
