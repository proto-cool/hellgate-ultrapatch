// Volumetric fog: light scattered towards the camera by the air between it
// and the scene (src/postfx.c, after the opaque scene, before anything
// transparent; the engine's own distance fog stays as it is).
//
//   Scatter  half resolution. For each pixel the view ray, out to the scene
//            or gvFogParams.y units, whichever is nearer:
//            sun      marched in 24 steps through the sun's shadow maps (the
//                     near one, then the fine 80-unit one; lit beyond both),
//                     so the air in a building's or a character's shadow
//                     stays dark and the lit air between reads as shafts;
//                     Henyey-Greenstein phase, strongest looking into the sun
//            lights   the engine's point lights near the camera, integrated
//                     in closed form along the ray (a halo in the fog); the
//                     one light with a cube shadow map (src/plshadow.c) is
//                     marched through it instead, so it casts shafts too
//            haze     far geometry fades towards the engine's fog colour
//   Temporal blended into last frame's result, reprojected and clamped
//   Blur     two depth-aware 9-tap passes over the half-resolution result
//   Apply    scene x transmittance + scattered light (colour only: the back
//            buffer's alpha is the glow)
//
// Depth is D3D post-projection z; view-space z = P43 / (d - P33).

float4   gvFogMetrics;          // full resolution: 1/w, 1/h, w, h
float4   gvFogProj;             // projection _11, _22, _33, _43
float4x4 gmFogInvView;          // view -> world
float4   gvFogEye;              // camera in the world; .w the frame (noise)
float4   gvFogParams;           // x density (per unit), y march distance, z light glow, w lights in use
float4   gvFogSun;              // xyz towards the sun; w (> 0) the maps are valid
float4   gvFogSunCol;           // rgb sun colour x strength; w phase asymmetry g
float4x4 gmFogNear;             // world -> near map (uv, depth)
float4x4 gmFogFine;             // world -> fine map
float4   gvFogLights[12];       // xyz position, w reach
float4   gvFogLightCol[12];     // rgb colour x weight; w how much it is the shadowing light
float4   gvFogPLS;              // the cube's projection: x f/(f-n), y fn/(f-n), z bias
float4   gvFogPass;             // blur: this target's texel xy, step zw (uv)
float4   gvFogSky;              // x the sun's share on the sky and on anything well beyond
                                // the march distance (a whole column of lit air)
float4   gvFogHaze;             // x haze density (per unit), y near fade (units), z the frame's noise offset
float4   gvFogColor;            // the engine's fog colour (the haze fades to it)
float4x4 gmFogPrevView;         // last frame's view (world -> view)
float4   gvFogPrevProj;         // last frame's projection _11, _22; z history weight (0: none)

texture2D   depthTex2D;
texture2D   nearTex2D;
texture2D   fineTex2D;
textureCUBE plsTexCube;
texture2D   fogTex2D;
texture2D   histTex2D;

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
samplerCUBE plsTex {
    Texture = <plsTexCube>;
    AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D fogTex {
    Texture = <fogTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D fogPointTex {
    Texture = <fogTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};

sampler2D histTex {
    Texture = <histTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
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

// the centre of the full-resolution depth texel under uv (see ao.fx)
float2 snap(float2 uv)
{
    return (floor(uv * gvFogMetrics.zw - 0.25) + 0.5) * gvFogMetrics.xy;
}

float lin_z(float2 uv)
{
    float d = tex2Dlod(depthTex, float4(snap(uv), 0, 0)).r;
    return gvFogProj.w / (d - gvFogProj.z);
}

float ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// lit (1) or not (0) in one shadow map; -1 outside its square
float map_vis(sampler2D smp, float4x4 M, float3 P)
{
    float4 sp = mul(float4(P, 1.0), M);
    float2 uv = sp.xy / sp.w;
    if (any(uv < 0.0 || uv > 1.0)) return -1.0;
    // half a unit of depth: the fog does not need contact accuracy
    float bias = 0.5 * length(float3(M._13, M._23, M._33));
    return sp.z / sp.w - bias <= tex2Dlod(smp, float4(uv, 0, 0)).x ? 1.0 : 0.0;
}

float sun_vis(float3 P)
{
    float v = map_vis(nearTex, gmFogNear, P);
    if (v < 0) v = map_vis(fineTex, gmFogFine, P);
    return v < 0 ? 1.0 : v;
}

// Henyey-Greenstein, scaled to 1 looking straight into the sun, over a
// quarter-strength floor so the shafts do not vanish looking away from it.
// Unscaled (x 2.1 into the sun at g 0.5) it pushed the lit air to white.
float phase(float c, float g)
{
    float g2 = g * g;
    float hg = pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), -1.5) * pow(1.0 - g, 3.0);
    return 0.25 + 0.75 * hg;
}

// the point light's falloff in the fog: R^2 / (R^2 + 8 d^2), the same hot
// core as the surfaces' smooth curve, windowed to zero at the reach
float pl_fall(float d2, float R)
{
    float q2 = d2 / (R * R);
    float w = saturate(1.0 - q2 * q2);
    return w * w / (1.0 + 8.0 * q2);
}

float pls_vis(float3 P, float3 L)
{
    float3 v = P - L;
    float3 a = abs(v);
    float m = max(a.x, max(a.y, a.z));
    float s = texCUBElod(plsTex, float4(v, 0)).x;
    float zs = gvFogPLS.y / max(gvFogPLS.x - s, 1e-6);
    return m - gvFogPLS.z <= zs ? 1.0 : 0.0;
}

// the scene under uv in the world; d its depth (1: sky)
float3 world_at(float2 uv, out float d)
{
    d = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    float z = gvFogProj.w / (d - gvFogProj.z);
    float3 Pv = float3((uv.x * 2.0 - 1.0) / gvFogProj.x * z, (1.0 - uv.y * 2.0) / gvFogProj.y * z, z);
    return mul(float4(Pv, 1.0), gmFogInvView).xyz;
}

float4 ScatterPS(float2 uv : TEXCOORD0, float2 vp : VPOS) : COLOR
{
    uv = snap(uv);
    float d;
    float3 Pw = world_at(uv, d);
    float3 E = gvFogEye.xyz;
    float3 ray = Pw - E;
    float len = length(ray);
    float3 dir = ray / max(len, 1e-4);
    float tmax = d >= 0.99999 ? gvFogParams.y : min(len, gvFogParams.y);
    float sigma = gvFogParams.x;
    // a new offset each frame: the temporal pass averages them into smooth
    // shafts (one fixed pattern shimmered as the camera moved)
    float jit = frac(ign(vp) + gvFogHaze.z);
    float3 acc = 0;

    // the sun, through its shadow maps
    [branch] if (gvFogSun.w > 0) {
        const int N = 24;
        float dt = tmax / N, s = 0;
        [loop] for (int i = 0; i < N; i++) {
            float t = (i + jit) * dt;
            // none right at the camera: the first units of fog sat on the
            // player like a veil
            s += sun_vis(E + dir * t) * exp(-sigma * t) * smoothstep(0.0, gvFogHaze.y, t);
        }
        // the lit air's share of the view, at most 1 - exp(-sigma tmax); the
        // sky share fades in with distance, so far buildings and the sky
        // behind them get the same (a hard switch at the sky made distant
        // walls whiter than the sky, first dense run)
        float far = d >= 0.99999 ? 1.0 : saturate((len - tmax) / tmax);
        float share = lerp(1.0, gvFogSky.x, far);
        acc += gvFogSunCol.rgb * (phase(dot(dir, gvFogSun.xyz), gvFogSunCol.w) * s * sigma * dt * share);
    }

    // point lights
    [loop] for (int k = 0; k < (int)gvFogParams.w; k++) {
        float3 L = gvFogLights[k].xyz;
        float R = gvFogLights[k].w;
        float3 o = E - L;
        float tc = -dot(o, dir);                        // nearest approach along the ray
        float h2 = max(dot(o, o) - tc * tc, 0.0);
        if (h2 >= R * R) continue;
        float half_ = sqrt(R * R - h2);
        float a = max(0.0, tc - half_), b = min(tmax, tc + half_);
        if (b <= a) continue;
        // closed form of the integral of R^2 / (R^2 + 8 (h^2 + s^2)) ds,
        // less its value at the reach (1/9) and rescaled, so the halo falls
        // to zero at its edge (cut off at 1/9, each light was a hard-edged
        // disc, like a particle sprite)
        float H = sqrt(R * R / 8.0 + h2);
        float g = R * R / 8.0 / H * (atan((b - tc) / H) - atan((a - tc) / H));
        g = max(g - (b - a) / 9.0, 0.0) * 9.0 / 8.0;
        [branch] if (gvFogLightCol[k].w > 0.01) {
            // the shadowing light: the same halo times the lit share along
            // the ray, marched through its cube; eased in and out by .w, so
            // the shadow moving to another fire changes no brightness
            const int M = 16;
            float dt = (b - a) / M, lit = 0, all = 0;
            [loop] for (int j = 0; j < M; j++) {
                float3 P = E + dir * (a + (j + jit) * dt);
                float3 v = P - L;
                float fw = pl_fall(dot(v, v), R);
                lit += fw * pls_vis(P, L);
                all += fw;
            }
            g *= lerp(1.0, all > 1e-5 ? lit / all : 1.0, gvFogLightCol[k].w);
        }
        acc += gvFogLightCol[k].rgb * (g * gvFogParams.z * sigma);
    }

    // haze: far geometry fades towards the fog colour (alpha: what is left
    // of the scene); the sky is left as drawn
    float T = d >= 0.99999 ? 1.0 : exp(-gvFogHaze.x * max(len - gvFogHaze.y, 0.0));
    acc += gvFogColor.rgb * (1.0 - T);
    return float4(acc, T);
}

// Temporal accumulation: this frame's scatter blended into the history,
// read where this pixel's world position was on screen last frame, clamped
// to this frame's 3x3 neighbourhood (so what moved does not smear).
float4 TemporalPS(float2 uv : TEXCOORD0) : COLOR
{
    float4 cur = tex2Dlod(fogPointTex, float4(uv, 0, 0));
    [branch] if (gvFogPrevProj.z <= 0) return cur;
    float4 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float4 c = tex2Dlod(fogPointTex, float4(uv + float2(x, y) * gvFogPass.xy, 0, 0));
            mn = min(mn, c); mx = max(mx, c);
        }
    float d;
    float3 Pw = world_at(snap(uv), d);
    if (d >= 0.99999) Pw = gvFogEye.xyz + normalize(Pw - gvFogEye.xyz) * 1000.0;
    float3 pv = mul(float4(Pw, 1.0), gmFogPrevView).xyz;
    float2 pu = float2(pv.x * gvFogPrevProj.x / pv.z * 0.5 + 0.5, 0.5 - pv.y * gvFogPrevProj.y / pv.z * 0.5);
    if (pv.z <= 0 || any(pu < 0.0 || pu > 1.0)) return cur;
    float4 h = clamp(tex2Dlod(histTex, float4(pu, 0, 0)), mn, mx);
    return lerp(h, cur, gvFogPrevProj.z);
}

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR
{
    float z0 = lin_z(uv);
    float4 s = 0;
    float w = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 u = uv + gvFogPass.zw * i;
        float wi = exp(-i * i / 8.0) * saturate(1.0 - abs(lin_z(u) - z0) * 10.0 / z0);
        s += tex2Dlod(fogPointTex, float4(u, 0, 0)) * wi;
        w += wi;
    }
    return s / max(w, 1e-4);
}

// Depth-aware upsample: the four half-resolution texels around this pixel,
// bilinear weights times depth agreement, so a building's edge against the
// sky stays sharp (plain bilinear drew it in 2-pixel steps).
float4 ApplyPS(float2 uv : TEXCOORD0) : COLOR
{
    float z0 = lin_z(uv);
    float2 p = uv / gvFogPass.xy - 0.5;
    float2 b = floor(p), f = p - b;
    float4 s = 0;
    float w = 0;
    [unroll] for (int j = 0; j < 2; j++)
        [unroll] for (int i = 0; i < 2; i++) {
            float2 c = (b + float2(i, j) + 0.5) * gvFogPass.xy;
            float bw = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
            float wi = (bw + 1e-3) / (1e-3 + abs(lin_z(c) - z0) / z0);
            s += tex2Dlod(fogPointTex, float4(c, 0, 0)) * wi;
            w += wi;
        }
    return saturate(s / w);
}

#define FULLSCREEN ZEnable = false; ZWriteEnable = false; StencilEnable = false; \
    AlphaTestEnable = false; CullMode = None; FogEnable = false; SRGBWriteEnable = false

technique Scatter {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ScatterPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

technique Temporal {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 TemporalPS();
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

technique Apply {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ApplyPS();
        AlphaBlendEnable = true; SrcBlend = One; DestBlend = SrcAlpha; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

// debug: the scattered light alone
technique Show {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ApplyPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}
