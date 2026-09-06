using System.Numerics;

namespace AetherGame;

/// <summary>
/// Shared visual language for every screen this project builds from script - Main Menu,
/// Pause Menu, Settings, the spawn catalogue, the HUD. One file so a later palette change
/// is a single edit instead of a grep across five screens that each invented their own
/// Vector4 literals, which is exactly the "per-screen invention" the UI/UX design called
/// out as the thing to avoid.
///
/// Constants only, deliberately: this is not a theming SYSTEM (no runtime palette swap,
/// no per-element override table) because nothing asked for one - just the numbers every
/// other Ui*.cs file in this project should read instead of picking its own.
/// </summary>
public static class UiTheme
{
    // ── Panel ─────────────────────────────────────────────────────────────────────
    public static readonly Vector4 PanelBackground = new(0.05f, 0.05f, 0.07f, 0.92f);
    public const float PanelCornerRadius = 8.0f;

    // ── Text ──────────────────────────────────────────────────────────────────────
    public static readonly Vector4 TextColor = new(0.95f, 0.95f, 0.97f, 1.0f);
    public static readonly Vector4 TextMuted = new(0.62f, 0.62f, 0.68f, 1.0f);
    public const float FontSizeHeader = 28.0f;
    public const float FontSizeBody = 18.0f;
    public const float FontSizeHint = 14.0f;

    // ── Accent ────────────────────────────────────────────────────────────────────
    // One accent colour, used for anything that wants to read as "the interactive one" -
    // a focused/selected tile border, a value fill bar. Chosen close to PhysicsGun's own
    // HeldEmissiveTint hue so the HUD's held-prop tint and every menu's accent agree.
    public static readonly Vector4 Accent = new(0.30f, 0.65f, 0.95f, 1.0f);

    // ── Crosshair (HUD) ───────────────────────────────────────────────────────────
    public static readonly Vector4 CrosshairIdle = new(1.0f, 1.0f, 1.0f, 0.85f);
    public static readonly Vector4 CrosshairHolding = new(0.35f, 0.75f, 1.0f, 0.9f);
    public static readonly Vector4 CrosshairRotating = new(1.0f, 0.75f, 0.3f, 0.9f);

    // ── Layout rhythm ─────────────────────────────────────────────────────────────
    public const float Spacing = 8.0f;
    public const float SpacingLarge = 16.0f;

    // ── Button rows ───────────────────────────────────────────────────────────────
    public const float ButtonWidth = 260.0f;
    public const float ButtonHeight = 44.0f;
    public const float ButtonRowHeight = 56.0f; // ButtonHeight + Spacing*1.5, the gap between stacked buttons
}
