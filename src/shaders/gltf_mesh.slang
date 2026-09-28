// Full PBR forward shader for all standard geometry.
//
// Vertex data is fetched via buffer device address (BDA) stored in
// DrawInstanceData.vertexBufferAddr.  No vertex input bindings.
//
// Push constants (48 bytes, matches aether::DrawContracts::PushConstants):
//   offset  0 : uint64_t  frameAddr         BDA -> FrameConstantsData
//   offset  8 : uint64_t  instanceDataAddr  BDA -> DrawInstanceData[]
//   offset 16 : uint64_t  skinPaletteAddr   BDA -> float4x4[] skin matrices
//   offset 24 : uint64_t  lightDataAddr      BDA -> LocalLight[]
//   offset 32 : uint64_t  tileHeadersAddr    BDA -> uint2[]
//   offset 40 : uint64_t  tileLightIndices   BDA -> uint[]

// ── Bindless resources (descriptor set 0) ────────────────────────────────
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/Sky.slangh"
#include "include/EnvBrdf.slangh"
#include "include/AmbientOcclusion.slangh"
#include "include/HeightFog.slangh"
#include "include/ShadowSampling.slangh"
#include "include/DirectionalShadowCSM.slangh"
#include "include/ContactShadow.slangh"
#include "include/LocalShadow.slangh"
#include "include/PBRUtilities.slangh"
#include "include/MeshVertex.slangh"
#include "include/Water.slangh"
#include "include/Foliage.slangh"
#define AETHER_VS_SKY_DEPTH 1
#define AETHER_VS_FOLIAGE_WIND 1
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// ── Fragment shader ───────────────────────────────────────────────────────

// Contact blob shadows (see FrameConstants.blobShadows): the soft dark disc the PS2 laid on
// the ground under every object it lit with its light records. Each blob is projected
// straight down, so it lands on whatever ground is under the actor and fades out as the
// actor rises off it. Overlapping blobs take the darkest, never stack.
// Darkness is the rig's: ground under Crash reads 0.81 of the ground beside him on the PS2's
// gamma-space output, 0.63 in linear. ponytail: kBlobFadeHeight (how far above the ground a
// blob survives) is not measured - the game's shadow-scale-by-height table is not extracted.
static const float kBlobDarkness = 0.37;
static const float kBlobFadeHeight = 2.0;
// CrashLook: character actors (the first blobShadowInfo.y blobs: Crash, Aku Aku) also lay a tight,
// darker core right under the feet while the feet are within kCharContactHeight of the ground - the
// contact the soft rig-matched blob is too wide to show. ponytail: a look choice, not measured.
static const float kCharContactDarkness = 0.35;
static const float kCharContactHeight = 0.35;
float BlobShadowVisibility(FrameConstantsData* fc, float3 worldPos, float upFacing)
{
    float shade = 0.0;
    float contact = 0.0;
    const uint count = min(fc->blobShadowInfo.x, 64u);
    for (uint i = 0u; i < count; ++i)
    {
        const float4 blob = fc->blobShadows[i];
        // A negative radius marks a pickup (see WorldRenderer::GatherBlobShadows): its blob stays
        // at full strength across its hover height and only fades past that, over a real drop.
        const bool pickup = blob.w < 0.0;
        const float r = abs(blob.w);
        // Below the actor's bounds bottom: fade with the drop. Above it: only up to the
        // actor's own middle, so the top of a neighbouring crate is not darkened.
        const float drop = blob.y - worldPos.y;
        const float dropFade = pickup ? smoothstep(kBlobFadeHeight, 2.0 * kBlobFadeHeight, drop) : smoothstep(0.0, kBlobFadeHeight, drop);
        const float vertical = (1.0 - dropFade) * (1.0 - smoothstep(1.5 * r, 2.0 * r, -drop));
        const float horizontal = 1.0 - smoothstep(r, 2.0 * r, length(worldPos.xz - blob.xz));
        shade = max(shade, horizontal * vertical);
        if (i < fc->blobShadowInfo.y)
        {
            const float core = (1.0 - smoothstep(0.3 * r, 1.1 * r, length(worldPos.xz - blob.xz)))
                * (1.0 - smoothstep(0.0, kCharContactHeight, drop)) * (1.0 - smoothstep(0.75 * r, 1.5 * r, -drop));
            contact = max(contact, core);
        }
    }
    return (1.0 - kBlobDarkness * shade * upFacing) * (1.0 - kCharContactDarkness * contact * upFacing);
}

// Twinsanity's god rays are nested additive sheets (four per beam, each shorter than the last)
// carrying a stripe texture the disc slides sideways at 0.2 U/s. Drawn as is that read as
// bars sliding across hard-edged cards. A light shaft instead keeps its rays in place and
// makes them breathe, and fades out wherever the card would show itself: its sheet's own
// edges (TEXCOORD_1 is the sheet's normalised 0..1 extent, x across, y from its top), seen
// edge-on, right in front of the camera, and where it cuts into the ground or a wall.
// Returned as a coverage multiplier: the blend is additive, so coverage is brightness.
// ponytail: every rate and width here is a look choice, not measured - the PS2 only slid.
float LightShaftFade(FrameConstantsData* fc, float2 uv, float2 sheet, float3 worldPos, float3 geomNormal)
{
    // Across the sheet a wide soft shoulder; along it, the top is already dark in the texture
    // and the bottom fades over half the sheet, so the staggered sheet ends merge into one
    // gradient down the beam instead of four steps.
    const float edges = smoothstep(0.0, 0.3, sheet.x) * smoothstep(1.0, 0.7, sheet.x)
        * smoothstep(0.0, 0.15, sheet.y) * smoothstep(1.0, 0.5, sheet.y);

    const float3 toEye = fc->cameraWorldPos.xyz - worldPos;
    const float eyeDist = length(toEye);
    const float3 V = toEye / max(eyeDist, 1e-4);
    const float nLen2 = dot(geomNormal, geomNormal);
    const float facing = nLen2 > 1e-12 ? abs(dot(geomNormal * rsqrt(nLen2), V)) : 1.0;
    // A card seen edge-on is a thin bright line; a card the camera is inside is a flat wall.
    const float view = smoothstep(0.08, 0.45, facing) * smoothstep(2.0, 9.0, eyeDist);

    // Soft intersection: fade over the last 1.5 m in front of the opaque scene behind.
    float soft = 1.0;
    const ResourceEntry depthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
    if (depthEntry.type == kResourceTypeBindlessTexture && depthEntry.address != 0xFFFFFFFFu && depthEntry.width > 0u && depthEntry.height > 0u)
    {
        const float4 clip = mul(fc->viewProj, float4(worldPos, 1.0));
        if (clip.w > 0.0)
        {
            const float3 ndc = clip.xyz / clip.w;
            const int2 texel = int2(saturate(ndc.xy * 0.5 + 0.5) * float2(depthEntry.width - 1u, depthEntry.height - 1u));
            const float sceneDepth = g_textures[uint(depthEntry.address)].Load(int3(texel, 0)).r;
            float4 sceneWorld = mul(fc->invViewProj, float4(ndc.xy, sceneDepth, 1.0));
            sceneWorld.xyz /= max(sceneWorld.w, 1e-6);
            soft = saturate((distance(fc->cameraWorldPos.xyz, sceneWorld.xyz) - eyeDist) / 1.5);
        }
    }

    // Shimmer: standing waves across the rays (no travelling term, so nothing slides), each
    // pulsing on its own slow clock, plus a gentle breath of the whole beam. The phase comes
    // from where the beam stands so neighbouring beams do not pulse in step.
    const float t = fc->elapsedTime;
    const float phase = (worldPos.x + worldPos.z) * 0.04;
    const float shimmer = 0.78
        + 0.14 * sin(uv.x * 18.85 + 1.3) * sin(t * 0.9 + phase)
        + 0.08 * sin(uv.x * 43.98 + 4.1) * sin(t * 1.37 + phase * 1.7);
    const float breath = 0.85 + 0.15 * sin(t * 0.45 + phase * 2.3);

    return edges * view * soft * shimmer * breath;
}

// Shared collectible highlight lobe: an energy-normalised Blinn lobe (narrower = brighter peak,
// same total), capped so a mirror-sharp lobe cannot blow out to a single white pixel, times NdotL.
// roughness 0.05 = a pin-sharp glint, 0.3 = a broad sheen.
float PickupHighlight(float3 N, float3 V, float3 L, float roughness)
{
    const float3 H = normalize(V + L);
    const float NdotH = saturate(dot(N, H));
    const float NdotL = saturate(dot(N, L));
    const float a = max(roughness, 0.03);
    const float power = 2.0 / (a * a);
    return min(pow(NdotH, power) * (power + 8.0) / (8.0 * kPI), 24.0) * NdotL;
}

// CrateLook: one light's GGX specular for the object-lit ORM path (the lobe times N.L,
// already cancelled against the BRDF's 1/N.L). 0 when the light is behind the surface.
float3 ObjectLitSpecular(float3 N, float3 V, float3 L, float roughness, float3 F0)
{
    const float NdotL = saturate(dot(N, L));
    const float NdotV = max(dot(N, V), 1e-4f);
    const float3 Hraw = V + L;
    const float3 H = dot(Hraw, Hraw) > 1e-12f ? normalize(Hraw) : N;
    return D_GGX(saturate(dot(N, H)), roughness) * G_SmithSchlick(NdotV, NdotL, roughness)
        * F_Schlick(saturate(dot(V, H)), F0) / (4.0f * NdotV);
}


// Character look (kFlagCharacter: Crash, Aku Aku - logs/look/CrashLook/README.md). Added on top
// of the PS2 object lighting, which stays the base so the characters keep their original
// colours; everything is tinted by the surface's own hue so orange fur stays orange.
// - Relief: a bump from the texture's own luminance (screen-derivative surface gradient, no
//   extra fetch) - wood grain on Aku Aku, denim and laces on Crash. Faded out once a pixel is
//   wider than kCharReliefFade so a far character does not sparkle.
// - Wrap: the key and fill wrap past the terminator and come back in the surface's saturated
//   hue - the soft, subsurface-like falloff of fur and skin instead of a hard grey edge.
// - Sheen: a broad, faint key highlight (material roughness sets its size).
// - Rim: a fresnel rim of the key (strongest on the lit side) plus the ambient and fill, so the
//   silhouette reads against bright sand and dark jungle alike; the material emissive feeds it
//   too (Aku Aku's warm glow is brightest at his edge).
// - Feathers (alpha-masked materials): light through the thin vane from behind, stronger near
//   the cut-out edge where the feather thins.
// - Catchlight (material roughness below kCharEyeRoughness = an eye): a painted-style white dot
//   placed in the eye texture's own UV space, offset from the pupil towards the key light's side
//   of the screen and upwards, so both (mirrored) eyes carry it on the same side.
// ponytail: every k* here is a look choice judged on the sheets, not measured - the PS2 had none.
static const float kCharRelief = 0.06;       // height scale of the luminance bump, metres
static const float kCharReliefFade = 0.015;   // pixel footprint (m) where the relief is gone
static const float kCharWrap = 0.5;           // wrap width past the terminator
static const float kCharWrapStrength = 0.55;
static const float kCharSheen = 0.06;
static const float kCharRim = 0.4;
static const float kCharAmbientRim = 0.2;
static const float kCharGlowRim = 3.0;
static const float kCharFeatherLight = 0.45;
static const float kCharEyeRoughness = 0.25;
static const float kCharCatchlight = 0.85;
static const float2 kCharEyeCentre = float2(0.52, 0.5);   // pupil centre in the eye texture
static const float kCharCatchOffset = 0.14;                // pupil -> catchlight, eye UV units
static const float kCharCatchRadius = 0.075;               // eye UV units

float3 CharacterLook(FrameConstantsData* fc, float3 albedo, float alpha, float alphaCutoff, bool feather, float3 N,
    float3 V, float3 worldPos, float2 uv, float roughness, float3 emissive, float keyVisibility, float ao)
{
    const float3 L0 = fc->objectLight0Direction.xyz;
    const float3 L1 = fc->objectLight1Direction.xyz;
    const float3 key = pow(max(fc->objectLight0Color.rgb, float3(0.0, 0.0, 0.0)), 2.2) * keyVisibility;
    const float3 fill = pow(max(fc->objectLight1Color.rgb, float3(0.0, 0.0, 0.0)), 2.2);
    const float3 amb = pow(max(fc->objectAmbient.rgb, float3(0.0, 0.0, 0.0)), 2.2);
    // The surface's own hue at full brightness (the disc textures are dark: 0x80 = 1.0).
    const float peak = max(albedo.r, max(albedo.g, albedo.b));
    const float3 hue = albedo / max(peak, 1e-3f);
    // Near-black paint (Crash's nose, brows, Aku Aku's mouth) keeps most of its black: its
    // "hue" is a neutral grey, and a full rim would turn it lilac-grey.
    const float rimDark = saturate(peak * 5.0f);

    // Relief from the texture's luminance (Mikkelsen's surface gradient from screen derivatives).
    const float3 dpdx = ddx(worldPos);
    const float3 dpdy = ddy(worldPos);
    const float lum = dot(albedo, float3(0.299, 0.587, 0.114));
    // Every derivative is taken here, in uniform control flow.
    const float dlumdx = ddx(lum);
    const float dlumdy = ddy(lum);
    const float2 duvdx = ddx(uv);
    const float2 duvdy = ddy(uv);
    const float3 r1 = cross(dpdy, N);
    const float3 r2 = cross(N, dpdx);
    const float det = dot(dpdx, r1);
    const float footprint = length(dpdx) + length(dpdy);
    const float relief = kCharRelief * (1.0f - smoothstep(0.4f * kCharReliefFade, kCharReliefFade, footprint));
    float3 Nr = N;
    if (abs(det) > 1e-14f && relief > 0.0f)
    {
        const float3 grad = sign(det) * (dlumdx * r1 + dlumdy * r2) / abs(det);
        Nr = normalize(N - relief * grad);
    }

    const float n0 = dot(Nr, L0);
    const float n1 = dot(Nr, L1);
    float3 add = albedo * key * (saturate(n0) - saturate(dot(N, L0)));   // relief shading

    // Wrapped key and fill, returned in the saturated hue.
    const float band0 = saturate((n0 + kCharWrap) / (1.0f + kCharWrap)) - saturate(n0);
    const float band1 = saturate((n1 + kCharWrap) / (1.0f + kCharWrap)) - saturate(n1);
    add += albedo * hue * (key * band0 + fill * (0.5f * band1)) * kCharWrapStrength;

    // Broad sheen, mostly the light's colour.
    add += key * lerp(hue, float3(1.0, 1.0, 1.0), 0.6f) * (PickupHighlight(Nr, V, L0, max(roughness, 0.35f)) * kCharSheen);

    // Rim.
    const float fresnel = 1.0f - saturate(abs(dot(N, V)));
    // The key rim keeps a little white so it reads as light; the ambient rim is all surface hue,
    // or the level's lilac sky ambient greys the shaded side (jeans and shoes went chalky).
    const float3 rimTint = lerp(hue, float3(1.0, 1.0, 1.0), 0.2f);
    add += rimDark * rimTint * key * (kCharRim * saturate(dot(N, L0) * 0.5f + 0.5f) * pow(fresnel, 3.0f))
        + rimDark * hue * dot(amb + 0.5f * fill, float3(0.333, 0.333, 0.334)) * (kCharAmbientRim * ao * pow(fresnel, 3.0f));
    add += emissive * (kCharGlowRim * pow(fresnel, 2.0f));

    // Feathers: key light through the vane from behind, more where it thins at the cut-out edge.
    if (feather)
    {
        const float thin = 1.0f - 0.6f * smoothstep(alphaCutoff, min(alphaCutoff + 0.35f, 1.0f), alpha);
        add += hue * albedo * key * (kCharFeatherLight * thin * pow(saturate(dot(V, -L0)), 2.0f));
    }

    // Eye catchlight.
    if (roughness < kCharEyeRoughness)
    {
        const float3 right = cross(-V, float3(0.0, 1.0, 0.0));
        // Screen pixels grow downwards: up is -y.
        const float2 screenDir = float2(dot(L0, right) >= 0.0f ? 0.7071f : -0.7071f, -0.7071f);
        const float2 uvDir = duvdx * screenDir.x + duvdy * screenDir.y;
        if (dot(uvDir, uvDir) > 1e-12f)
        {
            const float2 centre = floor(uv) + kCharEyeCentre + normalize(uvDir) * kCharCatchOffset;
            const float pixelUv = length(duvdx) + length(duvdy);
            const float radius = max(kCharCatchRadius, 0.9f * pixelUv);
            const float d = length(uv - centre);
            const float dotMask = 1.0f - smoothstep(radius - max(0.5f * pixelUv, 0.2f * radius), radius, d);
            const float eye = 1.0f - smoothstep(0.6f * kCharEyeRoughness, kCharEyeRoughness, roughness);
            add += float3(1.0, 1.0, 1.0) * (kCharCatchlight * dotMask * eye * lerp(0.65f, 1.0f, keyVisibility));
        }
    }
    return add;
}

// CrateLook: the normal-mapped normal for an object-lit surface. Object models carry no
// TANGENT stream, so the frame comes from screen derivatives (a cotangent frame, Schuler 2013):
// B follows increasing V, which is how tw-extract's crate normal maps are written.
float3 ObjectLitNormal(float3 Ngeom, float3 worldPos, float2 uv, float3 tn)
{
    const float3 dp1 = ddx(worldPos);
    const float3 dp2 = ddy(worldPos);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float3 dp2perp = cross(dp2, Ngeom);
    const float3 dp1perp = cross(Ngeom, dp1);
    const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    const float m = max(dot(T, T), dot(B, B));
    if (m < 1e-20f)
    {
        return Ngeom;
    }
    const float invMax = rsqrt(m);
    return normalize(tn.x * (T * invMax) + tn.y * (B * invMax) + tn.z * Ngeom);
}

// Twinsanity gem crystal (kFlagGem). The PS2 gem is a pale translucent card-cut shape lit flat;
// this makes it read as a cut crystal while keeping the cartoon hue: flat facets (each facet
// one colour, so it shades like a cut stone and nothing crawls across a face), a saturated body
// whose shade changes facet by facet with a fake refracted view ray (the internal facets), light
// shining through when the key light is behind it, a sky fresnel, and a PickupHighlight glint per
// facet that flashes whole for a few frames as the gem spins. The glint widens with the pixel
// footprint so a far gem glows steadily instead of flickering. Lit by the object light records
// like every Twinsanity object; shadowing removes the key light's diffuse and glint only, so a
// gem in shade stays coloured (ambient + its own glow).
// ponytail: every weight here is a look choice (logs/gemlook/README.md), not measured.
// albedo is this texel's colour; albedoAvg the material's average (the texture's last mip), whose
// hue colours the whole stone - the disc textures fade to white at their edges, which per texel
// turned every facet edge into a white smear. The texel only modulates brightness a little.
float3 GemShade(FrameConstantsData* fc, float3 albedo, float3 albedoAvg, float3 worldPos, float roughness, float3 emissive, float keyVisibility, out float coverage)
{
    const float3 toEye = fc->cameraWorldPos.xyz - worldPos;
    const float eyeDist = length(toEye);
    const float3 V = toEye / max(eyeDist, 1e-4);
    float3 Nf = cross(ddx(worldPos), ddy(worldPos));
    Nf = dot(Nf, Nf) > 1e-14 ? normalize(Nf) : V;
    Nf = dot(Nf, V) < 0.0 ? -Nf : Nf;

    // The disc colour is a pale half-range PS2 tint: keep its hue at full value. A grey (clear)
    // gem gets a faint cool cast so it reads as glass rather than grey plastic.
    // The average is washed pale by the textures' white edges and the PS2 tint is pastel anyway:
    // push the hue out to a cartoon-vivid one (1.8x its distance from white).
    const float peak = max(max(albedoAvg.r, albedoAvg.g), max(albedoAvg.b, 1e-4));
    const float3 hue = saturate(1.0 - (1.0 - albedoAvg / peak) * 1.8);
    const float sat = 1.0 - min(min(hue.r, hue.g), hue.b);
    const float3 gemCol = lerp(float3(0.80, 0.92, 1.0), hue, smoothstep(0.08, 0.3, sat));
    const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
    const float detail = clamp(dot(albedo, kLuma) / max(dot(albedoAvg, kLuma), 1e-4), 0.8, 1.2);

    const float3 L0 = normalize(fc->objectLight0Direction.xyz);
    const float3 L1 = normalize(fc->objectLight1Direction.xyz);
    // Wrapped diffuse per facet: facets differ in brightness but no facet goes black.
    const float3 lightGamma = fc->objectAmbient.rgb
        + fc->objectLight0Color.rgb * ((0.15 + 0.85 * saturate(dot(Nf, L0))) * keyVisibility)
        + fc->objectLight1Color.rgb * (0.15 + 0.85 * saturate(dot(Nf, L1)));
    const float3 lit = pow(max(lightGamma, float3(0.0, 0.0, 0.0)), 2.2);

    // Internal facets: the refracted view ray against two fixed planes gives each facet its own
    // depth shade, which moves facet by facet as the gem turns.
    const float3 Rr = refract(-V, Nf, 1.0 / 1.6);
    const float band = dot(Rr, float3(0.57, 0.21, 0.79)) * 9.0 + dot(Nf, float3(-0.30, 0.90, 0.20)) * 7.0;
    const float inner = pow(0.5 + 0.5 * sin(band), 1.5);
    // Facets square to the eye look into the stone (deep colour); grazing ones catch light.
    const float depth = lerp(0.6, 1.2, pow(1.0 - saturate(dot(Nf, V)), 1.5));
    // The brightest internal facets light up paler, like light bouncing inside the cut.
    const float3 hot = lerp(gemCol, float3(1.0, 1.0, 1.0), 0.45) * (0.7 * smoothstep(0.8, 1.0, inner));
    const float3 body = lit * (gemCol * ((0.2 + 0.8 * inner) * depth) + hot) * detail;

    // Light through the stone: the key light seen through the gem lifts it towards its hue.
    const float through = pow(saturate(dot(-V, L0)), 3.0) * keyVisibility;
    const float3 transmitted = gemCol * pow(max(fc->objectLight0Color.rgb, float3(0.0, 0.0, 0.0)), 2.2) * (0.6 * through);

    // Sky in the facets at grazing angles; facets turned down reflect the ground instead (a dim,
    // warm horizon), which keeps the pavilion deep and saturated rather than sky-washed.
    const float NdotV = saturate(dot(Nf, V));
    const float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
    const float3 R = reflect(-V, Nf);
    const float3 sky = R.y >= 0.0
        ? lerp(fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, saturate(R.y))
        : fc->skyHorizonColor.rgb * lerp(0.35, 0.12, saturate(-R.y * 2.0));

    // Glint: roughness widened by the pixel's world footprint (a gem ~40 px across keeps the
    // authored sharpness, a speck of a few px gets a broad steady lobe).
    const float pixel = length(fwidth(worldPos));
    const float glintRough = max(roughness, saturate(pixel * 6.0));
    const float3 keyLin = pow(max(fc->objectLight0Color.rgb, float3(0.0, 0.0, 0.0)), 2.2);
    const float3 fillLin = pow(max(fc->objectLight1Color.rgb, float3(0.0, 0.0, 0.0)), 2.2);
    const float3 whiteKey = lerp(keyLin, float3(dot(keyLin, float3(0.333, 0.333, 0.333))), 0.6) * 1.5;
    // A soft studio light above the viewer: some top facet always carries a white highlight,
    // the cartoon gem read, and it walks facet to facet as the gem turns.
    const float3 studio = normalize(V + float3(0.0, 1.2, 0.0));
    const float3 glint = whiteKey * (PickupHighlight(Nf, V, L0, glintRough) * keyVisibility)
        + fillLin * (0.6 * PickupHighlight(Nf, V, L1, glintRough * 1.5))
        + 0.35 * PickupHighlight(Nf, V, studio, max(glintRough, 0.15));
    // Rim in the stone's own colour (a white one read as frosted plastic).
    const float3 rim = lerp(gemCol, float3(1.0, 1.0, 1.0), 0.3) * (keyLin * keyVisibility + lit * 0.5) * (0.3 * pow(1.0 - NdotV, 4.0));

    // Self glow keeps the colour in shade and at a distance (where the gem is a few pixels).
    const float3 glow = gemCol * (0.12 + 0.25 * smoothstep(12.0, 50.0, eyeDist)) + emissive;

    coverage = lerp(0.86, 1.0, fresnel);
    return body + transmitted + sky * (0.5 * fresnel) + glint + rim + glow;
}

// FoliageSkyLook (logs/look/FoliageSkyLook/README.md). ponytail: every k* below is a look choice
// judged on the sheets, not measured - the PS2 had none of it.
// Soft cutout edge: scenery foliage's edge pass keeps texels down to this far (in alpha, capped)
// below the cutoff as a one-pixel coverage ramp (MaterialSystem softCutout).
static const float kFoliageEdgeMaxBand = 0.35;
// Light through the leaves: how much a card lit from behind adds, as a fraction of its own lit
// colour. Cards lying flat on the ground face the high sun, so they never light from behind and
// the grass fringe stays the ground's shade.
static const float kLeafTranslucency = 0.6;
// Cloud layers: sky materials with metallic > 0 (the only use of metallic on a sky material,
// whose metallic is otherwise meaningless - 0 on every other layer). metallic = shading strength.
static const float2 kCloudDrift = float2(0.0015, 0.0);  // UV/s, added on top of the rig-matched uv_scroll
static const float kCloudShadeStep = 0.08;               // UV units towards the sun the thickness is read at
static const float kCloudShadeContrast = 4.0;
static const float3 kCloudShadow = float3(0.62, 0.62, 0.80); // cool lilac underside
static const float3 kCloudLight = float3(1.16, 1.09, 1.0);   // warm sunlit top
static const float kCloudSilver = 0.35;                  // thin edges facing the sun glow
// Aerial perspective: distant land fades towards a pale sky blue (linear), ramping in smoothly
// between kAerialStart and kAerialEnd metres to kAerialMax, so the play space (most of it within
// 100 m) keeps its colours and only far islands and backdrops go blue. Yields to the engine's own
// fog and volumetrics (both off here).
static const float3 kAerialHaze = float3(0.36, 0.47, 0.92);
static const float kAerialStart = 50.0;
static const float kAerialEnd = 450.0;
static const float kAerialMax = 0.22;

// Soft self-shading for a PS2 cloud card: its alpha is the cloud's thickness, so thickness read a
// step towards the sun (in the card's own UV space, found through the screen derivatives) says
// how much cloud stands between this texel and the sun. Returns the colour multiplier.
float3 CloudShade(Texture2D tex, float2 uv, float alpha0, float3 worldPos, float3 cameraPos, float3 L)
{
    const float3 dpdx = ddx(worldPos);
    const float3 dpdy = ddy(worldPos);
    const float2 duvdx = ddx(uv);
    const float2 duvdy = ddy(uv);
    const float3 D = normalize(worldPos - cameraPos);
    const float3 toSun = L - D * dot(L, D);
    // Least squares: toSun ~ a * dpdx + b * dpdy, then the same a, b in UV.
    const float a11 = dot(dpdx, dpdx);
    const float a12 = dot(dpdx, dpdy);
    const float a22 = dot(dpdy, dpdy);
    const float det = a11 * a22 - a12 * a12;
    float2 towardSun = float2(0.0, 0.0);
    if (abs(det) > 1e-20)
    {
        const float b1 = dot(toSun, dpdx);
        const float b2 = dot(toSun, dpdy);
        const float2 duv = ((b1 * a22 - b2 * a12) * duvdx + (a11 * b2 - a12 * b1) * duvdy) / det;
        towardSun = dot(duv, duv) > 1e-16 ? normalize(duv) * kCloudShadeStep : float2(0.0, 0.0);
    }
    const float alpha1 = tex.SampleGrad(g_linearSampler, uv + towardSun, duvdx, duvdy).a;
    const float lit = saturate(0.5 + (alpha0 - alpha1) * kCloudShadeContrast);
    const float thin = saturate(alpha0 * (1.0 - alpha0) * 4.0);
    return lerp(kCloudShadow, kCloudLight, lit) + kCloudSilver * thin * lit;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    FrameConstantsData* fc        = pc.frame.Ptr();
    GpuMaterial*        materials = (GpuMaterial*)(fc->materialBufferAddr);

    GpuMaterial mat;
    mat.baseColorFactor = float4(1.0, 1.0, 1.0, 1.0);
    mat.metallicFactor = 0.0;
    mat.roughnessFactor = 1.0;
    mat.occlusionStrength = 1.0;
    mat.alphaCutoff = 0.5;
    mat.emissiveFactor = float4(0.0, 0.0, 0.0, 0.0);
    mat.flags = 0u;
    mat.albedoSlot = kNoTexture;
    mat.normalSlot = kNoTexture;
    mat.metallicRoughnessSlot = kNoTexture;
    mat.occlusionSlot = kNoTexture;
    mat.emissiveSlot = kNoTexture;

    const bool hasMaterial = input.materialIndex != kNoTexture;
    if (hasMaterial)
    {
        mat = materials[input.materialIndex];
    }

    float2 uv = input.uv;
    // Per-material UV scroll (Twinsanity's sea, waterfalls, sky): the velocity is
    // UV units per second; fract keeps the offset's mantissa small so hour-long
    // sessions do not lose precision. The wrap sampler handles the mod.
    uv += fract(mat.uvScroll * fc->elapsedTime);

    // FoliageSkyLook: a cloud layer (sky + metallic > 0, see kCloudDrift) drifts slowly on top of
    // the rig-matched scroll; its fract keeps the mantissa small like the scroll's.
    const float lookStrength = FoliageSkyLookStrength(fc);
    const bool cloudLayer = (mat.flags & kFlagSky) != 0u && mat.metallicFactor > 0.0f && mat.albedoSlot != kNoTexture;
    if (cloudLayer)
    {
        uv += fract(kCloudDrift * (fc->elapsedTime * lookStrength));
    }

    // ── Base colour ───────────────────────────────────────────────────────
    float4 baseColor = mat.baseColorFactor;
    if (mat.albedoSlot != kNoTexture)
    {
        const float4 texel = g_textures[mat.albedoSlot].Sample(g_linearSampler, uv);
        baseColor *= texel;
        if (cloudLayer)
        {
            const float3 cloudShade = CloudShade(g_textures[mat.albedoSlot], uv, texel.a, input.worldPos, fc->cameraWorldPos.xyz,
                normalize(fc->sunDirectionIntensity.xyz));
            baseColor.rgb *= lerp(float3(1.0, 1.0, 1.0), cloudShade, saturate(mat.metallicFactor) * lookStrength);
        }
    }
    // Vertex color modulates base color only when the material opts in
    // (primitive meshes with meaningful vertex colors). Painted materials
    // clear the flag; material-less draws keep the legacy fallback.
    // Clearly invalid all-zero/out-of-range streams are ignored.
    float3 vertexColor = float3(1.0, 1.0, 1.0);
    if (!hasMaterial || (mat.flags & kFlagModulateVertexColor) != 0u)
    {
        vertexColor = input.vertexColor;
    }
    if (any(vertexColor < 0.0) || any(vertexColor > 1.0) || dot(vertexColor, vertexColor) < 0.0025)
    {
        vertexColor = float3(1.0, 1.0, 1.0);
    }
    baseColor.rgb *= vertexColor;

    const bool lightShaft = (mat.flags & kFlagLightShaft) != 0u;
    if (lightShaft)
    {
        baseColor.a *= LightShaftFade(fc, input.uv, input.uv2, input.worldPos, input.worldNormal);
    }

    // Alpha test. Scenery foliage (Foliage.slangh) draws twice (MaterialSystem softCutout): this
    // shader as a plain cutout with the opaque geometry, writing depth, then the edge pass
    // (gltf_mesh_foliage_edge.slang, AETHER_FOLIAGE_EDGE_PASS) in the blended group without a depth
    // write, keeping only the one-pixel ramp of texels just below the cutoff as partial coverage.
    // The silhouette is antialiased instead of stair-stepped, without blurring the texture, and
    // the fringe hides nothing drawn after it.
    const bool sceneryFoliage = IsSceneryFoliage(mat.flags) && (mat.flags & kFlagAlphaMask) != 0u;
    const float alphaWidth = fwidth(baseColor.a);
    float edgeCoverage = 1.0;
    if ((mat.flags & kFlagAlphaMask) != 0u)
    {
#ifdef AETHER_FOLIAGE_EDGE_PASS
        const float band = sceneryFoliage ? min(alphaWidth, kFoliageEdgeMaxBand) * lookStrength : 0.0;
        if (band <= 1e-4 || baseColor.a >= mat.alphaCutoff || baseColor.a < mat.alphaCutoff - band)
        {
            discard;
        }
        edgeCoverage = saturate((baseColor.a - (mat.alphaCutoff - band)) / band);
#else
        if (baseColor.a < mat.alphaCutoff)
        {
            discard;
        }
#endif
    }

    // ── TBN - world-space tangent frame ──────────────────────────────────
    float3 Nraw = input.worldNormal;
    const float nRawLen2 = dot(Nraw, Nraw);
    float3 N;
    const float faceSign = isFrontFace ? 1.0f : -1.0f;
    Nraw *= faceSign;
    if (nRawLen2 > 1e-12)
    {
        N = normalize(Nraw);
    }
    else
    {
        // Fallback: screen-space dFdxy face normal.
        float3 faceN = cross(ddx(input.worldPos), ddy(input.worldPos));
        const float faceLen2 = dot(faceN, faceN);
        N = (faceLen2 > 1e-12) ? normalize(faceN) : float3(0.0, 1.0, 0.0);
    }

    float3 Traw = input.worldTangent;
    const float tRawLen2 = dot(Traw, Traw);
    float3 T = (tRawLen2 > 1e-12) ? normalize(Traw) : float3(1.0, 0.0, 0.0);
    T = T - N * dot(T, N);                         // Gram-Schmidt re-orthogonalise
    const float tLen2 = dot(T, T);
    if (tLen2 > 1e-12)
    {
        T = normalize(T);
    }
    else
    {
        float3 fallbackT = cross(float3(0.0, 1.0, 0.0), N);
        if (dot(fallbackT, fallbackT) < 1e-12)
        {
            fallbackT = cross(float3(1.0, 0.0, 0.0), N);
        }
        T = normalize(fallbackT);
    }
    float3 B = cross(N, T) * input.tangentSign;
    // Rows of TBN = T, B, N  ->  mul(tangentVec, TBN) transforms to world space.
    float3x3 TBN = float3x3(T, B, N);

    // Preserve the geometric normal so local light response can be softened
    // against tangent-space normal-map directionality.
    float3 Ngeom = N;

    // Variation the normal map lost to mip filtering, recovered below as roughness.
    // Averaging conflicting normals shortens the result; the shortfall is Toksvig's measure
    // of the detail that averaging removed.
    float normalMapVariance = 0.0;
    if (mat.normalSlot != kNoTexture)
    {
        float3 tn = g_textures[mat.normalSlot].Sample(g_linearSampler, uv).xyz;
        tn = tn * 2.0 - 1.0;

        // A deadzone before this counts as lost variation: an 8-bit normal map does not
        // reconstruct to exactly unit length even at mip 0, and treating that quantisation
        // as detail would roughen every normal-mapped surface at every distance.
        const float tnLength = saturate(length(tn));
        const float kQuantisationSlack = 0.04;
        const float shortfall = max(0.0, 1.0 - tnLength - kQuantisationSlack);
        normalMapVariance = shortfall / max(tnLength, 1e-3);

        N = normalize(mul(tn, TBN));
        N = normalize(lerp(Ngeom, N, 0.60f));
    }

    // ── Metallic / Roughness ──────────────────────────────────────────────
    float metallic  = mat.metallicFactor;
    float roughness = mat.roughnessFactor;
    if (mat.metallicRoughnessSlot != kNoTexture)
    {
        // glTF ORM: R=occlusion(if packed), G=roughness, B=metallic
        float3 orm = g_textures[mat.metallicRoughnessSlot].Sample(g_linearSampler, uv).rgb;
        roughness *= orm.g;
        metallic  *= orm.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = saturate(metallic);

    // ── Geometric specular antialiasing ───────────────────────────────────
    // Kaplanyan et al., HPG 2016, "Filtering Distributions of Normals for
    // Shading Antialiasing".
    //
    // A pixel does not see one normal, it sees every normal inside its
    // footprint. Where the surface bends or the normal map varies faster than
    // the pixel grid - ground at a grazing angle, dense detail in the distance,
    // any curved silhouette - a near-smooth material lands its specular lobe on
    // wildly different answers in neighbouring pixels. That is the sparkle that
    // crawls whenever anything moves, and no post-process antialiasing can
    // touch it: FXAA reads the shimmer as real detail, and it is different
    // every frame anyway.
    //
    // The fix is to widen the lobe rather than shrink the pixel. Measure how
    // much the normal varies across this pixel from its screen-space
    // derivatives, and fold that variance into the roughness, so the material
    // is as rough as its footprint actually is and the highlight resolves
    // instead of aliasing. Distant geometry converges to its average response
    // rather than picking one sample of it.
    {
        const float3 dNdx = ddx(N);
        const float3 dNdy = ddy(N);
        // Half-pixel Gaussian filter width; 1/(2*pi) is the paper's fit.
        const float kSigma2 = 0.15915494;
        // Ceiling on the roughness that may be added, so a silhouette or a
        // crease cannot turn the whole material matte.
        const float kKappa = 0.18;
        const float variance = kSigma2 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        // Screen-space derivatives only see the change ACROSS a pixel. Detail finer than a
        // pixel is gone before this runs - the mip chain already averaged it away - so the
        // normal map's own shortfall is added alongside, scaled by the same influence the
        // map was blended in with.
        const float mipVariance = normalMapVariance * 0.60;
        // Dialable, because this widens roughness on every normal-mapped surface in the
        // scene and how much sparkle is worth trading for how much gloss is a look decision.
        const float filterStrength = saturate(fc->shadingParams.x);
        const float kernelRoughness = min(2.0 * variance + mipVariance, kKappa) * filterStrength;
        roughness = sqrt(saturate(roughness * roughness + kernelRoughness));
    }

    // ── Occlusion ─────────────────────────────────────────────────────────
    const float materialAo = MaterialOcclusion(mat, uv);
    // Alpha-tested and blended surfaces are not in the depth prepass GTAO is built from, so
    // its value at their pixels belongs to whatever stands behind them. Object-lit models
    // read GTAO ~0 across whole faces (a nitro crate's front went from 0.23 to 0.02 with GTAO
    // on), so they keep their blob and sun shadows instead. ponytail: the object models'
    // prepass normals are the suspect; until they are checked, GTAO is scenery-only.
    const bool gtaoReceiver = (mat.flags & (kFlagAlphaMask | kFlagAlphaBlend | kFlagObjectLit)) == 0u;
    const float gtaoVisibility = gtaoReceiver ? GtaoVisibility(fc, input.svPosition.xy) : 1.0f;
    float ao = saturate(materialAo * gtaoVisibility);

    // ── Emissive ──────────────────────────────────────────────────────────
    float3 emissive = mat.emissiveFactor.xyz;
    if (mat.emissiveSlot != kNoTexture)
    {
        emissive *= g_textures[mat.emissiveSlot].Sample(g_linearSampler, uv).rgb;
    }

    // ── PBR lighting (directional + tiled local lights) ───────────────────
    float3 sunDir       = normalize(fc->sunDirectionIntensity.xyz);
    float  sunIntensity = fc->sunDirectionIntensity.w;
    float3 sunColor     = fc->sunColor.rgb;
    float3 ambientColor = fc->ambientColor.rgb;

    float3 camWorldPos = fc->cameraWorldPos.xyz;

    float3 Vraw = camWorldPos - input.worldPos;
    const float vLen2 = dot(Vraw, Vraw);
    float3 V = (vLen2 > 1e-12) ? normalize(Vraw) : N;

    // Fresnel base reflectance: dielectrics use 0.04, metals use albedo.
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor.rgb, metallic);

    // Directional sun contribution.
    float3 L = sunDir;
    float3 Hraw = V + L;
    const float hLen2 = dot(Hraw, Hraw);
    float3 H = (hLen2 > 1e-12) ? normalize(Hraw) : N;

    float NdotLraw = dot(N, L);
    // Foliage cards are one-sided alphas whose normals face every way; a raw NdotL puts the
    // card's back half in hard shadow and the whole clump reads near-black on its shaded
    // side. The original lit them from baked vertex colour only - closest faithful stand-in
    // is a wrapped diffuse (half-Lambert), which keeps a sun-facing bias without a dark side.
    const bool foliage = (mat.flags & kFlagFoliage) != 0u;
    float NdotL = foliage ? saturate(NdotLraw * 0.5f + 0.5f) : max(NdotLraw, 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = D_GGX(NdotH, roughness);
    float G = G_SmithSchlick(NdotV, NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);
    float3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 1e-4);

    // Same multi-scatter deficit the ambient specular had, and the same terms fix it.
    // Leaving one path compensated and the other not would give a rough metal correct
    // ambient and a highlight half as bright as it should be, which is a worse state
    // than having both wrong together.
    //
    // Ess is the single-scatter directional albedo - the fraction of light the BRDF
    // manages to send back out - so 1/Ess is exactly the factor that restores what the
    // remaining bounces would have carried, weighted by F0 because those bounces are
    // themselves reflections and a dark material swallows them.
    const float2 directEnvBrdf = EnvBrdfApprox(roughness, NdotV);
    const float Ess = max(directEnvBrdf.x + directEnvBrdf.y, 1e-3);
    specular *= 1.0 + F0 * (1.0 / Ess - 1.0);

    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);

    // Occlusion - screen-space and the material's baked map alike - is an estimate
    // of INDIRECT visibility, so it modulates ambient only. Direct visibility is
    // what the shadow maps already answer, exactly and per light. Attenuating the
    // sun and the local lights by it as well darkened lit surfaces near corners
    // until they met the shadowed ones in the middle, which read as the AO
    // cancelling the shadows out. The clamps made it worse in both directions: a
    // shadow bottoms out at 0.08 while AO could take a *lit* pixel to 0.05, so in
    // a corner the contrast could invert outright. This also matches the glTF
    // spec, where the occlusion texture applies to indirect light.
    const float ambientAo = ao;
    float3 diffuse = kD * baseColor.rgb / kPI;

    // Foliage cards face every way, and the cascade lookup answers "unshadowed" for any face
    // turned from the sun, so a card is looked up through its sun-facing side.
    const float3 shadowN = (foliage && dot(Ngeom, L) < 0.0f) ? -Ngeom : Ngeom;
    const float shadow = ComputeDirectionalShadow(fc, input.worldPos, shadowN, L, input.svPosition.xy);
    // Materials can opt out of shadow receiving (flag set == opt out); such
    // surfaces stay fully lit by the sun regardless of the shadow map.
    // Foliage receives: prelit grass kept its full bake under the forest canopy while the
    // ground it grows from fell to the shade, and read lime against dark green (hubb). Its own
    // card cannot shade it - shadow_depth pushes foliage casters kFoliageCasterOffset away from
    // the sun - and a shadow only pulls a prelit card to the bake's shade floor, as the ground.
    // Leaves under a higher canopy tier shade the same way (a lookup lifted 2 m towards the sun
    // changed nothing there: logs/scenemotion/canopy_nolift_top_lift2_bottom.png).
    const bool receivesShadows = (mat.flags & kFlagNoReceiveShadows) == 0u;
    // Contact shadows sharpen only where the cascade cannot resolve: the last few
    // centimetres at the base of an object, which a cascade texel is simply too
    // coarse to contain. Combined by min, because either occluder blocking the sun
    // is enough - a surface is not lit twice over. Not on foliage: marching a clump of
    // cards finds the clump itself.
    const float contact = (fc->shadowParams.w > 0.0f && receivesShadows && !foliage)
        ? ContactShadow(input.worldPos, Ngeom, L, fc, input.svPosition.xy)
        : 1.0f;
    // Cloud shadow, from the same layer the sky draws. Combined by min for the same reason
    // the contact term is: either occluder blocking the sun is enough.
    const float cloudShadow = receivesShadows
        ? CloudSunVisibility(input.worldPos, L, fc->shadingParams.y, fc->shadingParams.z, fc->elapsedTime)
        : 1.0f;
    const float sunVisibility = receivesShadows ? min(min(shadow, contact), cloudShadow) : 1.0f;

    // PS2 prelit geometry (Twinsanity scenery and props): the vertex colour already IS the
    // original's lighting - its sun, its sky fill, its baked shadows - and the GS showed
    // texture x vertex colour as is. Running the PBR lights over it lit everything twice, the
    // scene's ambient flattening it and the sun's NdotL darkening faces the bake had lit.
    // So the colour stays exactly the original's, and the one thing the PS2 could not do -
    // real-time shadows - pulls it down towards the scene's ambient: the same floor plus sky
    // the PBR path below lights a fully shaded surface with. Never below it, and never below
    // what the bake already darkened: ground in baked shade stays as it was instead of
    // shading twice.
    float3 finalColor;
    // A light shaft IS light: it is neither shadowed nor turned from the sun (the baked path's
    // sun-facing term would blacken a beam card seen from its far side), so it shows its bake.
    if (lightShaft)
    {
        finalColor = baseColor.rgb + emissive;
    }
    else if ((mat.flags & kFlagGem) != 0u)
    {
        const float keyVisibility = lerp(1.0f, sunVisibility, fc->shadowParams.z);
        float3 albedoAvg = mat.baseColorFactor.rgb * vertexColor;
        if (mat.albedoSlot != kNoTexture)
        {
            albedoAvg *= g_textures[mat.albedoSlot].SampleLevel(g_linearSampler, uv, 12.0).rgb;
        }
        float gemCoverage;
        finalColor = GemShade(fc, baseColor.rgb, albedoAvg, input.worldPos, mat.roughnessFactor, emissive, keyVisibility, gemCoverage);
        baseColor.a = max(baseColor.a, gemCoverage);
    }
    else if ((mat.flags & kFlagWater) != 0u)
    {
        // Sea, shore, rivers, pools, waterfalls: depth tint, reflections, ripple, glints, foam
        // over the PS2 sheet (include/Water.slangh). Replaces the sheet's coverage outright.
        float waterCoverage;
        finalColor = WaterShade(fc, mat, baseColor.rgb, baseColor.a, uv, input.uv, input.worldPos, Ngeom, input.svPosition.xy,
            vertexColor * mat.baseColorFactor.rgb, sunVisibility, waterCoverage);
        baseColor.a = waterCoverage;
    }
    else if ((mat.flags & kFlagBakedLighting) != 0u)
    {
        // The material factor carries the part of the vertex colour COLOR_0 cannot (the
        // PS2's 0x80 = 1.0 range), so the two together are the vertex colour in lighting units.
        const float3 bakedLight = max(vertexColor * mat.baseColorFactor.rgb, float3(1e-4, 1e-4, 1e-4));
        // The floor alone (`ambient`, set to the bake's darkest canopy shade) took every cast
        // shadow near-black: at the cliff base, game (36, 34), shadowed grass went to 11-17% of
        // its bake in red and green and the rock walls went dark purple. With the sky added, the
        // shaded cliff wall renders about (40, 28, 30), against the rig's (44, 28, 32) (logs/cliff).
        const float3 skyShade = SkyGradient(Ngeom, sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams, fc->shadingParams.y, 1.0f);
        const float3 shadeLight = min(bakedLight, ambientColor + skyShade);
        // A surface turned away from the sun cannot be sun-lit, and the cascade lookup
        // answers "unshadowed" for it (it early-outs on NdotL <= 0, because the PBR path's
        // own NdotL already zeroes such faces). The bake has no NdotL, so without this the
        // sand-fade wedges at the cliff base - facing the camera, away from the sun - stayed
        // at full baked brightness inside the cliff's shadow. Foliage cards face every way
        // and keep their own opt-out. ponytail: the 0.1 terminator band is a smoothing
        // width, not measured - the PS2 had no real-time terminator to compare against.
        const float facing = foliage ? 1.0f : smoothstep(0.0f, 0.1f, dot(Ngeom, L));
        const float bakedVisibility = lerp(1.0f, sunVisibility * facing, fc->shadowParams.z);
        // Split the bake into the part the sun gave (bakedLight - shadeLight) and the part it
        // did not (shadeLight, the level's ambient). Shadows take away the first; ambient
        // occlusion only the second, so creases and contact areas darken in the ground's own
        // hue while sunlit open ground - where GTAO reads 1 - keeps its exact bake colour.
        const float3 bakedLit = shadeLight * ambientAo + (bakedLight - shadeLight) * bakedVisibility;
        finalColor = baseColor.rgb * (bakedLit / bakedLight) * BlobShadowVisibility(fc, input.worldPos, foliage ? 1.0f : saturate(Ngeom.y * 2.0f)) + emissive;
        if (sceneryFoliage && lookStrength > 0.0f)
        {
            // Light through the leaves: a card whose viewer-facing side is turned from the sun
            // (Ngeom faces the eye) is lit from behind, most when the eye looks into the sun.
            // The transmitted light is the leaf's own colour, pushed towards its hue (thin leaves
            // glow yellow-green). Shadowed leaves transmit nothing, like they receive nothing.
            const float back = saturate(-dot(Ngeom, L));
            const float into = saturate(dot(-V, L));
            const float peak = max(max(baseColor.r, baseColor.g), max(baseColor.b, 1e-4f));
            const float3 glow = baseColor.rgb * lerp(float3(1.0, 1.0, 1.0), baseColor.rgb / peak, 0.6f);
            finalColor += glow * (kLeafTranslucency * back * (0.4f + 0.6f * into * into) * bakedVisibility * lookStrength);
        }
    }
    else if ((mat.flags & kFlagObjectLit) != 0u)
    {
        // PS2 object lighting (Twinsanity objects, pickups, characters): the level's own
        // light records - an ambient plus two directional lights - as the GS applied them
        // to everything that was not prelit scenery. The multiply and its saturate run in
        // gamma space on the PS2, so the result is raised to 2.2 to land in the renderer's
        // linear pipeline with the same on-screen ratios. Light 0 is the level's key light and
        // the sun the shadow maps are cast from, so the same sun visibility the scenery gets
        // takes it away: without that an object standing in a palm's shadow stayed fully
        // key-lit and read as full-bright against the shaded ground around it.
        // ponytail: the PS2 had no real-time shadow here; it swapped per-zone light records
        // (radius-limited) instead. Shadowing light 0 is the closest per-pixel stand-in until
        // those zones are extracted.
        const float keyVisibility = lerp(1.0f, sunVisibility, fc->shadowParams.z);
        // CrateLook (logs/look/CrateLook/README.md): a normal map, when the material has one
        // (tw-extract writes one for crate materials only), shapes the PS2 object lighting.
        float3 Nobj = Ngeom;
        if (mat.normalSlot != kNoTexture)
        {
            const float3 tn = g_textures[mat.normalSlot].Sample(g_linearSampler, uv).xyz * 2.0f - 1.0f;
            Nobj = ObjectLitNormal(Ngeom, input.worldPos, uv, tn);
        }
        const float3 lit = fc->objectAmbient.rgb * ambientAo
            + fc->objectLight0Color.rgb * (saturate(dot(Nobj, fc->objectLight0Direction.xyz)) * keyVisibility)
            + fc->objectLight1Color.rgb * saturate(dot(Nobj, fc->objectLight1Direction.xyz));
        float3 objectColor = baseColor.rgb * pow(max(lit, float3(0.0, 0.0, 0.0)), 2.2);
        // CrateLook: with an ORM texture (crate materials only) the surface gets a real response
        // on top of the PS2 lighting: key + fill GGX specular (F0 = 0.04 wood .. albedo metal)
        // and a fresnel sky reflection. Metal keeps kCrateMetalDiffuse of its diffuse so the
        // iron stays a readable cartoon grey instead of going black away from the sky. Without
        // the texture none of this runs: every other object_lit material is unchanged.
        if (mat.metallicRoughnessSlot != kNoTexture)
        {
            const float kCrateMetalDiffuse = 0.65f;
            const float kCrateSpec = 1.0f;
            const float kCrateReflect = 0.75f;
            const float3 keyC = pow(max(fc->objectLight0Color.rgb, float3(0.0, 0.0, 0.0)), 2.2) * keyVisibility;
            const float3 fillC = pow(max(fc->objectLight1Color.rgb, float3(0.0, 0.0, 0.0)), 2.2);
            objectColor *= 1.0f - (1.0f - kCrateMetalDiffuse) * metallic;
            objectColor += (ObjectLitSpecular(Nobj, V, fc->objectLight0Direction.xyz, roughness, F0) * keyC
                + ObjectLitSpecular(Nobj, V, fc->objectLight1Direction.xyz, roughness, F0) * fillC) * kCrateSpec;
            const float2 envBrdf = EnvBrdfApprox(roughness, saturate(dot(Nobj, V)));
            const float3 skyRefl = SkyGradient(reflect(-V, Nobj), sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb,
                fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams, fc->shadingParams.y, roughness);
            objectColor += skyRefl * (F0 * envBrdf.x + envBrdf.y) * (ambientAo * kCrateReflect);
        }
        finalColor = objectColor + emissive;
        if ((mat.flags & kFlagCharacter) != 0u)
        {
            finalColor = max(finalColor + CharacterLook(fc, baseColor.rgb, baseColor.a, mat.alphaCutoff, (mat.flags & kFlagAlphaMask) != 0u,
                Nobj, V, input.worldPos, uv, roughness, emissive, keyVisibility, ambientAo), float3(0.0, 0.0, 0.0));
        }
        if ((mat.flags & kFlagSheen) != 0u)
        {
            // Sheen (bit 12): the wumpa's glossy skin on top of the PS2 object lighting above,
            // which stays the base so the fruit keeps its saturated orange-red. Only a small
            // highlight is added: every broad additive term (wrap warmth, fresnel rim, an idle
            // distance glow) pushed the red skin past 1, where the tonemapper walks bright
            // saturated colour to white, and the fruit read as pale pink. The level's key light
            // is warm and bright (well above 1 once linearised), so the lobe stays small.
            // Knobs: material roughness = highlight size, kSheenSpec. See logs/look/WumpaLook/README.md.
            const float3 L0 = fc->objectLight0Direction.xyz;
            const float3 key = pow(max(fc->objectLight0Color.rgb, float3(0.0, 0.0, 0.0)), 2.2) * keyVisibility;
            // Soft specular: the shared collectible lobe (PickupHighlight, as the gems use), so
            // gems and fruit read as one family; the light's colour with a touch of skin.
            const float kSheenSpec = 0.2f;
            const float3 specTint = lerp(baseColor.rgb, float3(1.0, 1.0, 1.0), 0.6f);
            finalColor += key * specTint * (PickupHighlight(N, V, L0, roughness) * kSheenSpec);
        }
    }
    else
    {
        // The 0.08 floor exists so a shadowed surface does not read as a black hole, and
        // that is a DIFFUSE concern - it stands in for bounce light the renderer does not
        // compute. Specular is a different thing entirely: it is an image of the light
        // source, and a light that is blocked has no image to reflect. Leaking 8% of it was
        // enough to put a sun glint on a glossy floor inside a wall's shadow, because the
        // lobe on a smooth surface is orders of magnitude brighter than 1 and 8% of that is
        // still blinding.
        //
        // So the floor applies where it was meant to and nowhere else. Shadowed specular
        // goes to zero, which is what a blocked sun does.
        const float diffuseVisibility = lerp(1.0f, max(sunVisibility, 0.08f), fc->shadowParams.z);
        const float specularVisibility = lerp(1.0f, sunVisibility, fc->shadowParams.z);
        float3 Lo = (diffuse * diffuseVisibility + specular * specularVisibility) * (sunColor * (sunIntensity * NdotL));

        // Local lights accumulate separately from the sun so each keeps its own
        // shadow term; neither carries an occlusion factor.
        float3 LoLocal = float3(0.0, 0.0, 0.0);

        // Tiled local lights (point + spot).
        // Blend toward geometric normal so local lights remain visibly radial on
        // the ground while still retaining some normal-map detail.
        const float localNormalMapInfluence = 0.35;
        const float3 Nlocal = normalize(lerp(Ngeom, N, localNormalMapInfluence));
        AccumulateTiledLocalLights(
            pc.tileHeaders,
            pc.tileLightIndices,
            pc.lightData,
            input.worldPos,
            Nlocal,
            V,
            F0,
            baseColor.rgb,
            metallic,
            roughness,
            fc,
            input.svPosition.xy,
            LoLocal);
        Lo += LoLocal;

        // Image-based ambient from the same procedural sky the skybox draws, rather than
        // one flat colour in every direction. A constant ambient gives an object no shape
        // at all where the sun does not reach: the underside of an arm reads exactly as
        // bright as the top of a shoulder. Sampling the sky along the normal restores that,
        // and costs two evaluations of a closed-form gradient - no probes, no prefiltering,
        // no cubemap.
        //
        // SkyGradient, never EvalSky: the latter carries the solar disk, and lighting a
        // surface with it would add the sun a second time on top of the direct term and
        // smear a duplicate sun across every glossy surface.
        const float3 skyDiffuse = SkyGradient(N, sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams, fc->shadingParams.y, 1.0f);

        // Specular probe along the reflection vector, drifting toward the diffuse sample
        // as the surface roughens - a one-sample stand-in for a prefiltered mip chain.
        const float3 R = reflect(-V, N);
        const float3 skySpecularDir = normalize(lerp(R, N, roughness * roughness));
        const float3 skySpecular = SkySpecularProbe(skySpecularDir, input.worldPos, sunDir, sunColor, sunIntensity,
                fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams,
                fc->shadingParams.y, fc->shadingParams.z, fc->elapsedTime, roughness);

        const float2 envBrdf = EnvBrdfApprox(roughness, NdotV);

        // ambientColor stays in as the scene's authored floor - the light that is not the
        // sky at all - so an indoor or stylised scene can still set its own base level.
        const float3 ambientDiffuse = (ambientColor + skyDiffuse) * baseColor.rgb * kD;
        // Multi-scatter compensated, so a rough surface stops losing the light that bounces
        // between its microfacets - see EnvBrdf.slangh.
        const float3 ambientSpecular = skySpecular * EnvSpecularWeight(F0, envBrdf);
        // Diffuse and specular are not occluded by the same amount. Diffuse gathers the
        // whole hemisphere, so ambient occlusion describes it directly. A specular lobe
        // gathers a cone whose width is set by roughness: the smoother the surface, the
        // narrower that cone, and the less likely the geometry that occludes the
        // hemisphere also occludes the one direction the reflection comes from. Applying
        // the diffuse term to both - which is what this did - over-darkens reflections on
        // smooth surfaces inside any crease or contact, so polished materials go dull
        // exactly where they should still catch light.
        //
        // Lagarde's approximation ("Moving Frostbite to Physically Based Rendering")
        // reconstructs the cone's occlusion from the hemisphere's. At roughness 1 it
        // returns the plain AO, because then the lobe IS the hemisphere.
        const float specularAo = SpecularOcclusion(ambientAo, NdotV, roughness);
        float3 ambient = ambientDiffuse * MultiBounceAo(ambientAo, baseColor.rgb) + ambientSpecular * specularAo;

        finalColor = max(Lo + ambient + emissive, float3(0.0, 0.0, 0.0));
    }

    // Atmospheric perspective. Applied here rather than as a separate screen pass
    // because the world position and the sky terms this needs are already in hand;
    // a fullscreen pass would reconstruct both from depth to reach the same answer.
    //
    // Unless the volumetric pass is running, which solves this very integral by
    // marching so that shadows can carve beams out of it. Applying both would count
    // the same scattering twice.
    const bool volumetricsOn = fc->skyParams.z > 0.0f;
    const float fogAmount = volumetricsOn ? 0.0f : HeightFogAmount(
        fc->cameraWorldPos.xyz,
        input.worldPos,
        fc->fogParams.x,
        fc->fogParams.y,
        fc->fogParams.w);
    // FoliageSkyLook aerial perspective: distant land fades gently towards a pale sky blue. Not on
    // the sky, the god rays or additive surfaces (an additive haze would only brighten). It gives
    // way to the engine's fog (only what the fog does not already cover is added, so the two
    // never stack) and is off while the volumetric pass runs, which scatters this itself.
    if (!volumetricsOn && lookStrength > 0.0f && (mat.flags & (kFlagSky | kFlagLightShaft | kFlagAdditiveBlend)) == 0u)
    {
        const float aerial = kAerialMax * lookStrength * smoothstep(kAerialStart, kAerialEnd, length(input.worldPos - fc->cameraWorldPos.xyz));
        finalColor = lerp(finalColor, kAerialHaze, max(aerial - fogAmount, 0.0f));
    }
    if (fogAmount > 0.0f)
    {
        const float3 fogViewDir = normalize(input.worldPos - fc->cameraWorldPos.xyz);
        const float3 fogColor = HeightFogColor(
            fogViewDir,
            sunDir,
            sunColor,
            sunIntensity,
            fc->skyHorizonColor.rgb,
            fc->skyZenithColor.rgb,
            fc->skyVoidColor.rgb,
            fc->fogParams.z,
            fc->skyParams,
            fc->shadingParams.y);
        finalColor = lerp(finalColor, fogColor, fogAmount);
    }

    // Alpha is this surface's coverage. For opaque geometry that is 1 - it fully hides
    // whatever is behind it, which is what the background compositor needs to know. A
    // blended material instead reports how much it actually covers, which is the same
    // number the blend hardware needs, so the two uses agree rather than competing: a
    // half-transparent surface half-covers the background and half-blends with it.
    // Scenery foliage reports its soft edge (its pipeline blends; inside the cutoff this is 1).
    const float coverage = ((mat.flags & kFlagAlphaBlend) != 0u) ? saturate(baseColor.a) : edgeCoverage;
    return float4(finalColor, coverage);
}
