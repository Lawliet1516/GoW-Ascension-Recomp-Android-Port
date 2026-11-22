// the full output struct the RSX decompiler emits (14 varyings, then clipd0 last), written by hand with trivial values; clipd0 = (0.5, d, 0.5, 0.5)
struct VSOutput {
    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;
    float4 fog:FOG;
    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;
    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;
    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;
    float4 clipd0:SV_ClipDistance0;
};
VSOutput main([[vk::location(0)]] float2 p : ATTR0, [[vk::location(1)]] float d : ATTR1)
{
    VSOutput Out;
    Out.pos = float4(p, 0.0, 1.0);
    Out.col0 = float4(0,0,1,1); Out.col1 = (float4)0; Out.fog = (float4)0;
    Out.t0=(float4)0; Out.t1=(float4)0; Out.t2=(float4)0; Out.t3=(float4)0; Out.t4=(float4)0; Out.t5=(float4)0; Out.t6=(float4)0; Out.t7=(float4)0; Out.t8=(float4)0; Out.t9=(float4)0;
    Out.clipd0 = float4(0.5, d, 0.5, 0.5);
    return Out;
}
