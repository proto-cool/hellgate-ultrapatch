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
//   Bounce     the same samples read a half-size copy of the lit frame:
//              the surfaces that block the ambient light reflect their own
//              colour back (one bounce, after SSDO). Occlusion goes in
//              alpha, the bounced light in rgb.
//   Blur       two depth-aware 9-tap passes (horizontal, vertical)
//   Apply      depth-aware upsample, then frame x (occlusion + bounce)
//              (blend DESTCOLOR, SRCALPHA): the bounce only adds to a
//              surface that is lit, never to the sky or to black; the debug
//              techniques write occlusion + bounce, or the bounce x4, instead
//
// Light spill (src/postfx.c spill(), HDR, indoors, after the transparent
// half of the scene, before the fog): the engine's nearby lights (lamps,
// fires, portals, spells; src/plshadow.c's list) light the surfaces around
// them wider and softer than the engine's own lighting does. In world
// space, so it holds still as the camera moves (the first cut read glowing
// pixels off the screen: it only worked close up and at some angles).
//
//   Gather     half resolution: every light by its view-space position,
//              reach (its radius x a factor), N.L with a little wrap;
//              unshadowed but for the light with the point-light shadow
//              cube, as the engine's own point lights are
//   Blur       the AO's two depth-aware passes
//   Apply      frame x (1 + light), at most 2.5x: the frame stands in
//              for the albedo, as the AO's bounce (blend DESTCOLOR, ONE)
//
// Contact shadows (src/postfx.c contact(), indoors, after the AO, before the
// transparent half): half resolution, blurred and upsampled as the AO, a
// march of about a unit from each
// surface towards its light through the depth buffer; what lies just in
// front of the ray (a thin band of depth, so the camera-side foreground
// casts nothing) darkens it, more the nearer the blocker. Feet and props
// that floated over the coarse indoor shadow maps meet the floor. The light
// direction is the nearby lights' (gvSpillLights) weighted by how much each
// lights the surface, plus some from straight above. No per-pixel noise:
// the light spill's showed that it swims with the camera.
//
// Rigid16 colour (src/postfx.c postfx_rigid16): background meshes in the
// Rigid16 vertex format (position and one uv, no normals: the barbed wire
// on the character select) have no colour technique in any 2018 background
// effect, so the engine drew their depth (_zbuffer RigidShader16) and never
// their colour: invisible, in everyone's game. Drawn here right after that
// depth draw, with its matrix and texture: cut out at alpha 0.5, lit by the
// level's ambient and a fixed share for the sun (no normals to do better),
// fogged as the level is.
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
float4 gvAoBleed;       // x bounce strength (0: none, the frame copy is not read)
float4 gvAoFog;         // the engine's distance fog: x start (with the LOOK shift), y end; z > 0 on
float4 gvSpill;         // x strength (0: off), y reach (x the light's radius), z lights in use
float4 gvSpillLights[12];   // view space: xyz position, w radius
float4 gvSpillCol[12];      // rgb colour (faded in and out), w > 0: the shadow cube's light
float4x4 gmSpillInvView;    // view -> world (the cube is in world space)
float4 gvContact;           // x strength (0: off), y reach (units), z lights in use
float4 gvContactUp;         // the world's up in view space
float4x4 gmWireWVP;         // Rigid16: the draw's WorldViewProjection (from _zbuffer)
float4 gvWireLight;         // rgb the light on it
float4 gvWireFog;           // x start, y end (view depth), z > 0 on
float4 gvWireFogCol;        // rgb the level's fog colour
float4 gvSpillPLS;          // the cube's projection: x f/(f-n), y fn/(f-n), z bias

texture2D depthTex2D;
texture2D aoTex2D;
texture2D nearTex2D;
texture2D fineTex2D;
texture2D wireTex2D;    // Rigid16: the mesh's texture (bound for its depth draw)
texture2D colTex2D;     // the lit frame so far, half size
textureCUBE plsTexCube; // the point-light shadow cube (src/plshadow.c)

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
sampler2D colTex {
    Texture = <colTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D aoTex {
    Texture = <aoTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
sampler2D wireTex {
    Texture = <wireTex2D>;
    AddressU = Wrap; AddressV = Wrap;
    MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
    SRGBTexture = false;
};
samplerCUBE plsTex {
    Texture = <plsTexCube>;
    AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
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

// normal from the nearer neighbour on each axis (no bleeding over edges),
// towards the camera
float3 view_normal(float2 uv, float3 P)
{
    float2 t = gvAoMetrics.xy;
    float3 pr = view_at(uv + float2(t.x, 0)), pl = view_at(uv - float2(t.x, 0));
    float3 pd = view_at(uv + float2(0, t.y)), pu = view_at(uv - float2(0, t.y));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dy, dx));
    return dot(N, P) > 0 ? -N : N;
}

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
    if (d >= 0.99999) return float4(0, 0, 0, 1);        // sky: no occlusion, no bounce
    float z = gvAoProj.w / (d - gvAoProj.z);
    float3 P = view_pos(uv, z);
    float3 N = view_normal(uv, P);
    float2 t = gvAoMetrics.xy;

    // two scales in one pass: contact (radius R) and the wide one (4R) that
    // outdoor geometry needs; at R alone a street averaged 0.97 (the log's
    // frame trace), everything larger than a curb was out of reach
    float R = gvAoParams.x;
    float pxu = gvAoProj.x * 0.5 * gvAoMetrics.z / z;     // full-res pixels per world unit here
    if (R * pxu < 1.0) return float4(0, 0, 0, 1);

    // interleaved gradient noise rotates the spirals per pixel; the blur hides it
    float phi = 6.2831853 * frac(52.9829189 * frac(dot(vp, float2(0.06711056, 0.00583715))));
    float occ[2];
    float3 bnc[2];
    [unroll] for (int sc = 0; sc < 2; sc++) {
        float Rs = sc == 0 ? R : 4.0 * R;
        float rpx = min(Rs * pxu, 0.2 * gvAoMetrics.z);
        float sum = 0;
        float3 bsum = 0;
        [unroll] for (int i = 0; i < 8; i++) {
            float a = (i + 0.5) / 8.0;
            float ang = a * 6.2831853 * 5.0 + phi + sc * 1.3;
            float2 u = uv + float2(cos(ang), sin(ang)) * (a * rpx) * t;
            float3 v = view_at(u) - P;
            float vv = dot(v, v);
            float f = saturate(1.0 - vv / (Rs * Rs));
            float o = f * saturate(dot(v, N) * rsqrt(vv + 1e-4) - 0.15);
            sum += o;
            [branch] if (gvAoBleed.x > 0 && o > 0)
                bsum += tex2Dlod(colTex, float4(u, 0, 0)).rgb * o;
        }
        occ[sc] = sum / 8.0;
        bnc[sc] = bsum / 8.0;
    }
    float sum = occ[0] + 0.7 * occ[1];
    float3 bounce = gvAoBleed.x * (bnc[0] + 0.7 * bnc[1]);
    float ao = saturate(1.0 - gvAoParams.y * sum);
    [branch] if (gvAoSun.w > 0)
        ao = lerp(ao, 1.0, gvAoSun.w * sun_share(P, N));
    float fade = saturate((z - gvAoParams.z) / max(gvAoParams.w - gvAoParams.z, 1e-3));
    // the engine fogged the materials before this pass: only the share of
    // the surface the fog left may be darkened (at full strength it drew
    // dark creases through the fog)
    [branch] if (gvAoFog.z > 0)
        fade = max(fade, 1.0 - saturate((gvAoFog.y - length(P)) / max(gvAoFog.y - gvAoFog.x, 1e-3)));
    ao = lerp(ao, 1.0, fade);
    bounce *= 1.0 - fade;
    return float4(bounce, ao);
}

float4 BlurPS(float2 uv : TEXCOORD0) : COLOR
{
    float z0 = lin_z(uv);
    float4 s = 0;
    float w = 0;
    [unroll] for (int i = -4; i <= 4; i++) {
        float2 u = uv + gvAoPass.zw * i;
        float wi = exp(-i * i / 8.0) * saturate(1.0 - abs(lin_z(u) - z0) * 20.0 / z0);
        s += tex2Dlod(aoPointTex, float4(u, 0, 0)) * wi;
        w += wi;
    }
    return s / max(w, 1e-4);
}

// linear view depth at half resolution, for soft particles (sky: the far plane)
float4 LinearDepthPS(float2 uv : TEXCOORD0) : COLOR
{
    return float4(lin_z(uv), 0, 0, 0);
}

// the four half-resolution texels around uv, weighted by how well their
// depth agrees with this pixel's (bilinear alone haloed edges); as fog.fx
float4 upsample_raw(float2 uv)
{
    float z0 = lin_z(uv);
    float2 p = uv / gvAoPass.xy - 0.5;
    float2 b = floor(p), f = p - b;
    float4 s = 0;
    float w = 0;
    [unroll] for (int j = 0; j < 2; j++)
        [unroll] for (int i = 0; i < 2; i++) {
            float2 c = (b + float2(i, j) + 0.5) * gvAoPass.xy;
            float bw = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
            float wi = (bw + 1e-3) / (1e-3 + abs(lin_z(c) - z0) / z0);
            s += tex2Dlod(aoPointTex, float4(c, 0, 0)) * wi;
            w += wi;
        }
    return s / w;
}

float4 upsample(float2 uv) { return saturate(upsample_raw(uv)); }

// rgb the bounce, a the occlusion: blended as frame x (a + rgb)
float4 ApplyPS(float2 uv : TEXCOORD0) : COLOR
{
    return upsample(uv);
}

float4 ShowPS(float2 uv : TEXCOORD0) : COLOR
{
    float4 o = upsample(uv);
    return float4(o.a + o.rgb, 1);
}

// debug: the bounced light alone, x4 on black
float4 ShowBouncePS(float2 uv : TEXCOORD0) : COLOR
{
    return float4(upsample(uv).rgb * 4.0, 1);
}

// ---- light spill ----

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float spill_cube_vis(float3 Pv, float3 Lv)
{
    float3 Pw = mul(float4(Pv, 1.0), gmSpillInvView).xyz;
    float3 Lw = mul(float4(Lv, 1.0), gmSpillInvView).xyz;
    float3 v = Pw - Lw;
    float3 a = abs(v);
    float m = max(a.x, max(a.y, a.z));
    float s = texCUBElod(plsTex, float4(v, 0)).x;
    float zs = gvSpillPLS.y / max(gvSpillPLS.x - s, 1e-6);
    return m - gvSpillPLS.z <= zs ? 1.0 : 0.0;
}

float4 SpillGatherPS(float2 uv : TEXCOORD0) : COLOR
{
    uv = snap(uv);
    float own = luma(tex2Dlod(colTex, float4(uv, 0, 0)).rgb);
    float d = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    if (d >= 0.99999) return float4(0, 0, 0, own);
    float z = gvAoProj.w / (d - gvAoProj.z);
    float3 P = view_pos(uv, z);
    float3 N = view_normal(uv, P);
    float3 sum = 0;
    [loop] for (int k = 0; k < (int)gvSpill.z; k++) {
        float3 L = gvSpillLights[k].xyz;
        float Re = gvSpillLights[k].w;                  // the engine's own radius
        float R = Re * gvSpill.y;                       // the spill's reach
        float3 v = L - P;
        float d2 = dot(v, v);
        [branch] if (d2 < R * R) {
            // Only what the engine's light leaves out: it falls off linearly
            // to nothing at its radius; light falls off more softly and goes
            // on past it. The spill is the difference, nothing where the
            // engine already lights (at full strength next to a lamp it was
            // the same light twice: a character by the lamps came out 20-35%
            // brighter and tinted, 2026-09-24), the most about the radius.
            float qe = sqrt(d2) / Re;
            float soft = 1.0 / (1.0 + 4.0 * qe * qe);
            float win = saturate(1.0 - d2 / (R * R));
            float tail = max(soft * win * win - saturate(1.0 - qe), 0.0);
            // the wrap widens with distance: normals rebuilt from depth
            // shimmer on far geometry as the camera moves, and so did the
            // light (2026-09-24); from 40 units it is half N.L, half flat
            float wrap = 0.3 + 0.7 * saturate(z / 40.0);
            float w = tail * saturate((dot(N, v) * rsqrt(d2 + 1e-4) + wrap) / (1.0 + wrap));
            [branch] if (w > 1e-3) {
                // unshadowed but for the light that has the cube, as the
                // engine's own point lights are. A march through the depth
                // buffer drew false shadows in the shapes of whatever stood
                // in front on screen (the player, a pillar) across the
                // walls (2026-09-24)
                float vis = 1.0;
                [branch] if (gvSpillCol[k].w > 0.01)
                    vis = lerp(1.0, spill_cube_vis(P + N * 0.1, L), gvSpillCol[k].w);
                sum += gvSpillCol[k].rgb * (w * vis);
            }
        }
    }
    float3 S = sum * gvSpill.x;
    float fade = saturate((z - gvAoParams.z) / max(gvAoParams.w - gvAoParams.z, 1e-3));
    [branch] if (gvAoFog.z > 0)
        fade = max(fade, 1.0 - saturate((gvAoFog.y - length(P)) / max(gvAoFog.y - gvAoFog.x, 1e-3)));
    return float4(min(S * (1.0 - fade), 16.0), own);
}

// the gain on the frame, blended DESTCOLOR x gain + frame: the frame stands
// in for the albedo, as the AO's bounce does. Divided by the local
// brightness (to light dark surfaces as much as lit ones) it drew bright
// outlines along every depth edge, where that average straddled both
// sides (2026-09-24)
float3 spill_gain(float2 uv)
{
    return min(max(upsample_raw(uv).rgb, 0), 1.5);
}

float4 SpillApplyPS(float2 uv : TEXCOORD0) : COLOR
{
    return float4(spill_gain(uv), 1);
}

// ---- contact shadows ----

float2 to_uv(float3 V)
{
    return float2(V.x * gvAoProj.x / V.z * 0.5 + 0.5, 0.5 - V.y * gvAoProj.y / V.z * 0.5);
}

// half resolution: the occlusion, 0..1 (blurred and upsampled after, as
// the AO: at full resolution with a level per step it drew hard, banded
// "zebra" shadows, 2026-09-24)
float4 ContactPS(float2 uv : TEXCOORD0) : COLOR
{
    uv = snap(uv);
    float d = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    if (d >= 0.99999) return 0.0;
    float z = gvAoProj.w / (d - gvAoProj.z);
    if (z > 40.0) return 0.0;                        // a step is under a pixel out there
    float3 P = view_pos(uv, z);
    float3 N = view_normal(uv, P);

    // where the light comes from: the lights by how much each lights P,
    // and a share from above so a room without lights still grounds things
    float3 Ld = gvContactUp.xyz * 0.25;
    [loop] for (int k = 0; k < (int)gvContact.z; k++) {
        float3 v = gvSpillLights[k].xyz - P;
        float R = gvSpillLights[k].w;
        float d2 = dot(v, v);
        [branch] if (d2 < R * R && d2 > 1e-4) {
            float q = sqrt(d2) / R;
            Ld += v * rsqrt(d2) * ((1.0 - q) * (1.0 - q) * dot(gvSpillCol[k].rgb, float3(0.3, 0.59, 0.11)));
        }
    }
    float ll = length(Ld);
    if (ll < 1e-4) return 0.0;
    Ld /= ll;
    if (dot(N, Ld) < -0.1) return 0.0;               // facing away: already in its own shade

    float len = gvContact.y;
    float3 O = P + N * (0.02 + 0.002 * z);
    float occ = 0.0;
    // Evenly spaced now (quadratic spacing left the far steps wider than
    // an ankle: patchy shadows under the feet), and a blocker counts less
    // the further along the ray it is, to nothing at the reach: a hand held
    // a unit above the floor shadowed it like a foot on it (2026-09-24)
    [loop] for (int j = 1; j <= 16; j++) {
        float a = j / 16.0;
        float t = a * len;
        float3 S = O + Ld * t;
        if (S.z < 0.1) break;
        float2 u = to_uv(S);
        if (any(u < 0.0 || u > 1.0)) break;
        float dz = S.z - lin_z(u);
        float bias = 0.01 + 0.003 * S.z;
        float thick = 0.1 + 0.5 * t;
        float o = saturate((dz - bias) * 20.0) * saturate((thick - dz) * 10.0);
        occ = max(occ, o * (1.0 - a) * (1.0 - a));
    }
    return occ * saturate((40.0 - z) / 15.0);
}

// full resolution: the blurred occlusion, upsampled; blended DESTCOLOR x it
float4 ContactApplyPS(float2 uv : TEXCOORD0) : COLOR
{
    return 1.0 - gvContact.x * upsample(uv).r;
}

// debug: the added light alone, on black (no albedo)
float4 SpillShowLightPS(float2 uv : TEXCOORD0) : COLOR
{
    return float4(max(upsample_raw(uv).rgb, 0), 1);
}

// ---- Rigid16 colour ----

struct WVO { float4 pos : POSITION; float2 uv : TEXCOORD0; float z : TEXCOORD1; };

WVO WireVS(float4 p : POSITION, float4 uv : TEXCOORD0)
{
    WVO o;
    o.pos = mul(float4(p.xyz, 1.0), gmWireWVP);    // as _zbuffer RigidShader16
    o.uv = uv.xy;
    o.z = o.pos.w;
    return o;
}

float4 WirePS(WVO i) : COLOR
{
    float4 t = tex2D(wireTex, i.uv);
    clip(t.a - 0.5);
    float3 c = t.rgb * gvWireLight.rgb;
    [branch] if (gvWireFog.z > 0)
        c = lerp(c, gvWireFogCol.rgb, saturate((i.z - gvWireFog.x) / max(gvWireFog.y - gvWireFog.x, 1e-3)));
    return float4(c, 0.004);                    // the engine's glow floor: no glow
}

// ---- backdrop mask (src/postfx.c backdrop_mask) ----
// a pass with only a pixel shader: the engine's own vertex shader stays
// bound, so the backdrop lands where it was drawn; ps_2_0 so it pairs with
// the engine's vs_1_1/2_0
float4 MaskPS() : COLOR { return float4(1, 1, 1, 1); }

// debug: shadow-only meshes (src/postfx.c shadow_only_*): the shadow pass's
// rigid casters again with the main camera, flat magenta, passing only where
// nothing nearer or equal is in the depth buffer
float4 ShadowOnlyPS() : COLOR { return float4(1, 0, 1, 1); }

// debug: depth without colour (src/postfx.c dnc_*): each main-view draw
// marks a mask, the depth pre-pass one and the colour draws another, with
// the engine's own vertex shader (ps_2_0 for its vs_1/2 shaders, ps_3_0 for
// the materials' vs_3_0); the view paints magenta where only depth landed
float4 DncMarkPS() : COLOR { return 1.0; }
texture2D dncZTex2D;
texture2D dncCTex2D;
sampler2D dncZTex {
    Texture = <dncZTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
sampler2D dncCTex {
    Texture = <dncCTex2D>;
    AddressU = Clamp; AddressV = Clamp;
    MipFilter = None; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
};
float4 gvDnc;       // x mode: 1 depth without colour, 2 the depth mask, 3 the colour mask
float4 DncShowPS(float2 uv : TEXCOORD0) : COLOR
{
    float z = tex2Dlod(dncZTex, float4(uv, 0, 0)).r, c = tex2Dlod(dncCTex, float4(uv, 0, 0)).r;
    float a = gvDnc.x > 2.5 ? c : gvDnc.x > 1.5 ? z : (z > 0.5 && c < 0.5 ? 1.0 : 0.0);
    return float4(1, 0, 1, a > 0.5 ? 0.85 : 0.0);
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
        AlphaBlendEnable = true; SrcBlend = DestColor; DestBlend = SrcAlpha; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique ShowBounce {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ShowBouncePS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique Show {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ShowPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique SpillGather {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 SpillGatherPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

// frame x (1 + gain)
technique SpillApply {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 SpillApplyPS();
        AlphaBlendEnable = true; SrcBlend = DestColor; DestBlend = One; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

// debug: the added light alone, frame x gain
technique SpillShow {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 SpillApplyPS();
        AlphaBlendEnable = true; SrcBlend = DestColor; DestBlend = Zero; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique SpillShowLight {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 SpillShowLightPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique Contact {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ContactPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0xf;
        FULLSCREEN;
    }
}

technique ContactApply {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ContactApplyPS();
        AlphaBlendEnable = true; SrcBlend = DestColor; DestBlend = Zero; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

// debug: the contact shadows alone, on white
technique ContactShow {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 ContactApplyPS();
        AlphaBlendEnable = false; ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}

technique Wire {
    pass p0 {
        VertexShader = compile vs_3_0 WireVS();
        PixelShader = compile ps_3_0 WirePS();
        AlphaBlendEnable = false; AlphaTestEnable = false; ColorWriteEnable = 0xf;
        ZEnable = true; ZWriteEnable = true; ZFunc = LessEqual; StencilEnable = false;
        CullMode = None; FogEnable = false; SRGBWriteEnable = false;
    }
}

technique BackdropMask {
    pass p0 {
        PixelShader = compile ps_2_0 MaskPS();
        AlphaBlendEnable = false; AlphaTestEnable = false; ColorWriteEnable = 0xf;
        ZEnable = true; ZWriteEnable = false; ZFunc = LessEqual; StencilEnable = false;
        FogEnable = false; SRGBWriteEnable = false;
    }
}

technique ShadowOnly {
    pass p0 {
        PixelShader = compile ps_2_0 ShadowOnlyPS();
        AlphaBlendEnable = false; AlphaTestEnable = false; ColorWriteEnable = 0x7;
        ZEnable = true; ZWriteEnable = false; ZFunc = Less; StencilEnable = false;
        CullMode = None; FogEnable = false; SRGBWriteEnable = false;
    }
}

technique DncMark2 {
    pass p0 {
        PixelShader = compile ps_2_0 DncMarkPS();
        AlphaBlendEnable = false; AlphaTestEnable = false; ColorWriteEnable = 0xf;
        ZEnable = true; ZWriteEnable = false; ZFunc = LessEqual; StencilEnable = false;
        FogEnable = false; SRGBWriteEnable = false;
    }
}

technique DncMark3 {
    pass p0 {
        PixelShader = compile ps_3_0 DncMarkPS();
        AlphaBlendEnable = false; AlphaTestEnable = false; ColorWriteEnable = 0xf;
        ZEnable = true; ZWriteEnable = false; ZFunc = LessEqual; StencilEnable = false;
        FogEnable = false; SRGBWriteEnable = false;
    }
}

technique DncShow {
    pass p0 {
        VertexShader = compile vs_3_0 QuadVS();
        PixelShader = compile ps_3_0 DncShowPS();
        AlphaBlendEnable = true; SrcBlend = SrcAlpha; DestBlend = InvSrcAlpha; BlendOp = Add;
        ColorWriteEnable = 0x7;
        FULLSCREEN;
    }
}
