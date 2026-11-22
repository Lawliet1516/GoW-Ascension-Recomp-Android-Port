// planes 4/5 in use: the decompiler adds `float2 clipd1 : SV_ClipDistance1`; glslang merges both into one float[6]. Only plane 5 carries the test value.
struct VSOutput { float4 pos : SV_Position; float4 clipd0 : SV_ClipDistance0; float2 clipd1 : SV_ClipDistance1; };
VSOutput main([[vk::location(0)]] float2 p : ATTR0, [[vk::location(1)]] float d : ATTR1)
{
    VSOutput Out; Out.pos = float4(p, 0.0, 1.0); Out.clipd0 = float4(0.5, 0.5, 0.5, 0.5); Out.clipd1 = float2(0.5, d); return Out;
}
