struct VSInput {
    float4 a0:ATTR0;
    float4 a3:ATTR3;
};
struct VSOutput {
    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;
    float4 fog:FOG;
    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;
    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;
    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;
    float4 clipd0:SV_ClipDistance0;
#ifdef RSX_POINT_SIZE_BUILTIN
    [[vk::builtin("PointSize")]] float psz:PSIZE;
#endif
};
cbuffer VPConst : register(b0) {
    float4 vp_c[512];
    float4 vp_posscale;
    float4 vp_posoffset;
#ifdef RSX_POINT_SIZE_BUILTIN
    float4 vp_pointsize;
#endif
};
#ifdef RSX_POINT_SIZE_BUILTIN
float vp_point_size(float from_vp) {
    float s = (vp_pointsize.y != 0.0) ? from_vp : vp_pointsize.x;
    s = (s == s) ? s : 1.0;
    return min(max(s, 1.0), max(vp_pointsize.z, 1.0));
}
#endif
float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 vp_mul(float4 a, float4 b) { return float4(vp_mul1(a.x, b.x), vp_mul1(a.y, b.y), vp_mul1(a.z, b.z), vp_mul1(a.w, b.w)); }
float vp_dot3(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z; }
float vp_dot4(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z + m.w; }
VSOutput main(VSInput input) {
    float4 v[16];
    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);
    v[0]=input.a0;
    v[3]=input.a3;
    float4 r[32]; float4 o[16];
    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;
    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);
    int4 a0 = (int4)0; int4 a1 = (int4)0;
    float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;
    { float4 _v = (float4)((v[0]).xyzw);
        o[0].xyzw = _v.xyzw;
    }
    { float4 _v = (float4)((v[3]).xyzw);
        o[1].xyzw = _v.xyzw;
    }
    { float4 _v = (float4)((v[3]).xyzw);
        o[5].xyzw = _v.xyzw;
    }
    VSOutput Out;
    float4 _p = o[0];
    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);
    Out.col0 = o[1]; Out.col1 = o[2]; Out.fog = o[5].xxxx;
    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];
    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];
    Out.t8=o[15]; Out.t9=o[6];
#ifdef RSX_POINT_SIZE_BUILTIN
    Out.psz = vp_point_size(o[6].x);
#endif
    Out.clipd0 = float4(0.5, -(o[5].z), 0.5, 0.5);
    return Out;
}
