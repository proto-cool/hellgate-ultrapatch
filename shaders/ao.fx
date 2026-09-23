// Screen-space ambient occlusion (src/postfx.c), drawn between the opaque
// and the transparent halves of the scene's draw list, from the scene depth
// the device now keeps in an INTZ texture (src/device.c).
//
//   Occlusion  half resolution: normals rebuilt from depth, two 8-tap
//              spirals around each pixel (after McGuire et al.'s SAO), at
//              the radius and at 4x it, faded out with distance
//              Occlusion darkens only the ambient light, and the frame
//              it multiplies has the direct sun in it too: where the sun
//              reaches a surface (its angle, times the sun's shadow maps)
//              the occlusion is eased off by gvAoSun.w
//   Blur       two depth-aware 9-tap passes (horizontal, vertical)
//   Apply      depth-aware upsample, multiplied into the back buffer (blend
//              ZERO, SRCCOLOR); the debug technique writes the occlusion
//              itself instead
//
// Depth is D3D post-projection z; view-space z = P43 / (d - P33).

float4 gvAoMetrics;     // full resolution: 1/w, 1/h, w, h
float4 gvAoProj;        // projection _11, _22, _33, _43
float4 gvAoParams;      // radius (world units), strength, fade start, fade end
float4 gvAoPass;        // this target's texel size xy; blur step zw (uv)
float4x4 gmAoInvView;   // view -> world
float4 gvAoSun;         // xyz towards the sun (world); w how much of the
                        // occlusion full sun takes away (0: none, no maps read)
float4x4 gmAoNear;      // world -> near sun map (uv, depth)
float4x4 gmAoFine;      // world -> fine sun map

texture2D depthTex2D;
texture2D aoTex2D;
texture2D nearTex2D;
texture2D fineTex2D;

sampler2D depthTex {
    Texture = <depthTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D nearTex {
    Texture = <nearTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D fineTex {
    Texture = <fineTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D aoTex {
    Texture = <aoTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D aoPointTex {
    Texture = <aoTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};

struct VO { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VO QuadVS(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VO o;
    o.pos = pos;
    o.uv = uv;
    return o;
}

// The centre of the full-resolution depth texel under uv. The coordinates
// read here are texel corners (a half-resolution pixel's centre) and texel
// centres (+-1 texel from a snapped one); a plain floor() sits exactly on a
// step for one of the two, and a point sample there picks a texel by
// rounding, so the rebuilt position and its depth came apart in bands
// (stripes in the first in-game runs). A quarter-texel shift puts both
// kinds well inside a step: corners go to the texel up and left.
float2 snap(float2 uv)
{
    return (floor(uv * gvAoMetrics.zw - 0.25) + 0.5) * gvAoMetrics.xy;
}

float lin_z(float2 uv)
{
    float d = tex2Dlod(depthTex, float4(snap(uv), 0, 0)).r;
    return gvAoProj.w / (d - gvAoProj.z);
}

float3 view_pos(float2 uv, float z)
{
    return float3((uv.x * 2.0 - 1.0) / gvAoProj.x * z, (1.0 - uv.y * 2.0) / gvAoProj.y * z, z);
}

float3 view_at(float2 uv) { uv = snap(uv); return view_pos(uv, lin_z(uv)); }

// lit (1) or not (0) in one sun map; -1 outside its square (as fog.fx)
float map_vis(sampler2D smp, float4x4 M, float3 P)
{
    float4 sp = mul(float4(P, 1.0), M);
    float2 uv = sp.xy / sp.w;
    if (any(uv < 0.0 || uv > 1.0)) return -1.0;
    float bias = 0.2 * length(float3(M._13, M._23, M._33));
    return sp.z / sp.w - bias <= tex2Dlod(smp, float4(uv, 0, 0)).x ? 1.0 : 0.0;
}

// how much the sun lights this surface directly, 0..1
float sun_share(float3 P, float3 N)
{
    float3 Pw = mul(float4(P, 1.0), gmAoInvView).xyz;
    float3 Nw = normalize(mul(float4(N, 0.0), gmAoInvView).xyz);
    float ndl = saturate(dot(Nw, gvAoSun.xyz) * 2.0);   // normals from depth are rough
    if (ndl <= 0) return 0;
    Pw += Nw * 0.1;                                     // off the surface, against acne
    float v = map_vis(nearTex, gmAoNear, Pw);
    if (v < 0) v = map_vis(fineTex, gmAoFine, Pw);
    return ndl * (v < 0 ? 1.0 : v);
}

float4 OcclusionPS(float2 uv : TEXCOORD0, float2 vp : VPOS) : COLOR
{
    uv = snap(uv);
    float d = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    if (d >= 0.99999) return 1.0;                       // sky
    float z = gvAoProj.w / (d - gvAoProj.z);
    float3 P = view_pos(uv, z);

    // normal from the nearer neighbour on each axis (no bleeding over edges)
    float2 t = gvAoMetrics.xy;
    float3 pr = view_at(uv + float2(t.x, 0)), pl = view_at(uv - float2(t.x, 0));
    float3 pd = view_at(uv + float2(0, t.y)), pu = view_at(uv - float2(0, t.y));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dy, dx));
    N = dot(N, P) > 0 ? -N : N;                         // towards the camera

    // two scales in one pass: contact (radius R) and the wide one (4R) that
    // outdoor geometry needs; at R alone a street averaged 0.97 (the log's
    // frame trace), everything larger than a curb was out of reach
    float R = gvAoParams.x;
    float pxu = gvAoProj.x * 0.5 * gvAoMetrics.z / z;     // full-res pixels per world unit here
    if (R * pxu < 1.0) return 1.0;

    // interleaved gradient noise rotates the spirals per pixel; the blur hides it
    float phi = 6.2831853 * frac(52.9829189 * frac(dot(vp, float2(0.06711056, 0.00583715))));
    float occ[2];
    [unroll] for (int sc = 0; sc < 2; sc++) {
        float Rs = sc == 0 ? R : 4.0 * R;
        float rpx = min(Rs * pxu, 0.2 * gvAoMetrics.z);
        float sum = 0;
        [unroll] for (int i = 0; i < 8; i++) {
            float a = (i + 0.5) / 8.0;
            float ang = a * 6.2831853 * 5.0 + phi + sc * 1.3;
            float2 u = uv + float2(cos(ang), sin(ang)) * (a * rpx) * t;
            float3 v = view_at(u) - P;
            float vv = dot(v, v);
            float f = saturate(1.0 - vv / (Rs * Rs));
            sum += f * saturate(dot(v, N) * rsqrt(vv + 1e-4) - 0.15);
        }
        occ[sc] = sum / 8.0;
    }
    float sum = occ[0] + 0.7 * occ[1];
    float ao = saturate(1.0 - gvAoParams.y * sum);
    [branch] if (gvAoSun.w > 0)
        ao = lerp(ao, 1.0, gvAoSun.w * sun_share(P, N));
    ao = lerp(ao, 1.0, saturate((z - gvAoParams.z) / max(gvAoParams.w - gvAoParams.z, 1e-3)));
    return float4(ao, ao, ao, 1);
}

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR
{
    float z0 = lin_z(uv);
    float s = 0, w = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 u = uv + gvAoPass.zw * i;
        float wi = exp(-i * i / 8.0) * saturate(1.0 - abs(lin_z(u) - z0) * 20.0 / z0);
        s += tex2Dlod(aoPointTex, float4(u, 0, 0)).r * wi;
        w += wi;
    }
    float ao = s / max(w, 1e-4);
    return float4(ao, ao, ao, 1);
}

// linear view depth at half resolution, for soft particles (sky: the far plane)
float4 LinearDepthPS(float2 uv : TEXCOORD0) : COLOR
{
    return float4(lin_z(uv), 0, 0, 0);
}

// the four half-resolution texels around uv, weighted by how well their
// depth agrees with this pixel's (bilinear alone haloed edges); as fog.fx
float4 ApplyPS(float2 uv : TEXCOORD0) : COLOR
{
    float z0 = lin_z(uv);
    float2 p = uv / gvAoPass.xy - 0.5;
    float2 b = floor(p), f = p - b;
    float s = 0, w = 0;
    [unroll] for (int j = 0; j < 2; j++)
        [unroll] for (int i = 0; i < 2; i++) {
            float2 c = (b + float2(i, j) + 0.5) * gvAoPass.xy;
            float bw = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
            float wi = (bw + 1e-3) / (1e-3 + abs(lin_z(c) - z0) / z0);
            s += tex2Dlod(aoPointTex, float4(c, 0, 0)).r * wi;
            w += wi;
        }
    float ao = saturate(s / w);
    return float4(ao, ao, ao, 1);
}

#define FULLSCREEN ZEnable = false; ZWriteEnable = false; StencilEnable = false; \
    AlphaTestEnable = false; CullMode = None; FogEnable = false; SRGBWriteEnable = false

technique Occlusion {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 OcclusionPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

technique Blur {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 BlurPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

technique LinearDepth {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 LinearDepthPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

technique Apply {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ApplyPS();
        AlphaBlendEnable = true; SrcBlend = Zero; DestBlend = SrcColor; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique Show {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ApplyPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}
