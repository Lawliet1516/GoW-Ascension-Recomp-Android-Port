// minimal HLSL vertex shader: position + one float4 SV_ClipDistance0, as the RSX decompiler declares it (no RSX code, no cbuffer)
struct VSOutput { float4 pos : SV_Position; float4 clipd0 : SV_ClipDistance0; };
VSOutput main([[vk::location(0)]] float2 p : ATTR0, [[vk::location(1)]] float d : ATTR1)
{
    VSOutput Out; Out.pos = float4(p, 0.0, 1.0); Out.clipd0 = float4(0.5, d, 0.5, 0.5); return Out;
}
