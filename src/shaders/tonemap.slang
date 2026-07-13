// Post-process pass: HDR tonemap + gamma correction.
//
// Vertex shader generates a full-screen triangle from SV_VertexID so no
// vertex buffer is required.  The fragment shader reads the HDR offscreen
// buffer directly via a Load() call (integer pixel coordinates) to avoid
// needing a sampler, then applies the selected tonemap operator.
//
// Binding layout for this standalone pass (no per-draw set 0):
//   set 0, binding 0 - bindless sampled images
// This differs from the forward-pass contract (which uses set 1) because
// there is no per-draw descriptor set in a full-screen post-process pass.

// Bindless sampled images at set 0 (no per-draw set in this pass).
[[vk::binding(0, 0)]]
Texture2D g_textures[];

#include "include/FullScreenTriangle.slangh"

// Push constants.
struct TonemapPush
{
    uint  hdrSlot;
    uint  mode;           // tonemap operator index
    float exposure;
    uint  debugCompare;   // 0 = normal, 1 = side-by-side comparison
    uint  debugModeCount; // number of modes to compare (0 = all)
    int   inspectX;       // pixel inspect X (-1 = disabled)
    int   inspectY;
    uint  screenWidth;    // render target width for debug division
    uint  screenHeight;   // render target height for header bar
};
[[vk::push_constant]] TonemapPush pc;

// Tonemap operators live in individual family files under include/Tonemap/.
// Add a new operator there, then register it in TonemapOperators.slangh.
#include "include/Tonemap/TonemapOperators.slangh"

// ── Debug header bar: operator labels ──────────────────────────────────────
// Each vertical strip gets a colored header bar with the operator index digit.
// Must match kTonemapCount in TonemapDefs.hpp.
static const uint kNumOperators = 10u;

// 3×5 bitmap font digits (0 = empty, 1 = filled), rows packed as 3-bit values.
static const uint kDigitRows[10][5] = {
    {7u, 5u, 5u, 5u, 7u}, // 0: ###, #.#, #.#, #.#, ###
    {1u, 1u, 1u, 1u, 1u}, // 1: ..#, ..#, ..#, ..#, ..#
    {7u, 1u, 7u, 4u, 7u}, // 2: ###, ..#, ###, #.., ###
    {7u, 1u, 7u, 1u, 7u}, // 3: ###, ..#, ###, ..#, ###
    {5u, 5u, 7u, 1u, 1u}, // 4: #.#, #.#, ###, ..#, ..#
    {7u, 4u, 7u, 1u, 7u}, // 5: ###, #.., ###, ..#, ###
    {7u, 4u, 7u, 5u, 7u}, // 6: ###, #.., ###, #.#, ###
    {7u, 1u, 1u, 1u, 1u}, // 7: ###, ..#, ..#, ..#, ..#
    {7u, 5u, 7u, 5u, 7u}, // 8: ###, #.#, ###, #.#, ###
    {7u, 5u, 7u, 1u, 1u}, // 9: ###, #.#, ###, ..#, ..#
};

// Unique hue per operator for the header bar.
static const float3 kBarColors[10] = {
    float3(0.78f, 0.39f, 0.39f), // 0 Reinhard      - muted red
    float3(0.39f, 0.78f, 0.39f), // 1 ACES Filmic   - muted green
    float3(0.39f, 0.39f, 0.78f), // 2 Uncharted 2   - muted blue
    float3(0.78f, 0.78f, 0.31f), // 3 HejlRichard    - yellow
    float3(0.47f, 0.47f, 0.47f), // 4 Linear         - gray
    float3(0.78f, 0.47f, 0.78f), // 5 Exponential    - pink
    float3(0.31f, 0.78f, 0.78f), // 6 Filmic Dice    - cyan
    float3(0.78f, 0.59f, 0.31f), // 7 Lottes         - orange
    float3(0.31f, 0.78f, 0.59f), // 8 RomBinDaHouse  - teal
    float3(1.00f, 1.00f, 1.00f), // 9 Vanilla        - white
};

// ── Fragment shader ────────────────────────────────────────────────────────
[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    const int2   coord = int2(svPos.xy);
    const float3 hdr   = g_textures[pc.hdrSlot].Load(int3(coord, 0)).rgb * pc.exposure;

    float3 mapped;

    if (pc.debugCompare != 0u)
    {
        // Side-by-side: divide into vertical strips, one operator per strip.
        uint numModes = (pc.debugModeCount != 0u) ? pc.debugModeCount : kNumOperators;
        uint stripIdx = (uint(coord.x) * numModes) / max(pc.screenWidth, 1u);

        // Compute strip pixel bounds so the header bar can position its digit.
        uint stripStart = (stripIdx * pc.screenWidth) / numModes;
        uint stripEnd   = ((stripIdx + 1u) * pc.screenWidth) / numModes;
        uint stripWidth = stripEnd - stripStart;

        // ── Operator-identifying header bar ────────────────────────────
        const uint kBarHeight = 24u;
        if (uint(coord.y) < kBarHeight)
        {
            float3 barColor = kBarColors[stripIdx];

            // Render the operator index digit (0–9) using a 3×5 bitmap font
            // with 4×4 screen-pixel blocks per font pixel.
            uint localX   = uint(coord.x) - stripStart;
            uint digitW   = 12u; // 3 font cols × 4 px
            uint digitH   = 20u; // 5 font rows × 4 px
            uint digitOff = (stripWidth > digitW) ? (stripWidth - digitW) / 2u : 0u;

            if (localX >= digitOff && localX < digitOff + digitW &&
                uint(coord.y) < digitH)
            {
                uint fontX = (localX - digitOff) / 4u;
                uint fontY = uint(coord.y) / 4u;
                if (((kDigitRows[stripIdx][fontY] >> (2u - fontX)) & 1u) != 0u)
                {
                    barColor = float3(1.0f, 1.0f, 1.0f);
                }
            }

            mapped = barColor;
        }
        else
        {
            mapped = TonemapByIndex(stripIdx, hdr);
        }
    }
    else
    {
        mapped = TonemapByIndex(pc.mode, hdr);
    }

    // Approximate sRGB gamma (linear -> display).
    const float3 gamma = pow(max(mapped, float3(0.0f)), float3(1.0f / 2.2f));
    return float4(gamma, 1.0f);
}
