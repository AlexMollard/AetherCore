using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Keyboard <see cref="Key"/> -&gt; a single-glyph character in the baked
/// "InputPromptsKeyboardMouse" icon font (assets/fonts/InputPromptsKeyboardMouse.ttf,
/// imported this session via the Kenney importer's `ImportFont` - see
/// KenneyImport.hpp's doc comment on it).
/// </summary>
/// <remarks>
/// <para>
/// <b>Why a font, not a sprite atlas</b> (the ladder result this file's existence is the
/// answer to): Kenney's Input Prompts pack ships each icon family in two forms - a
/// <c>Vector/</c> folder of ~100-260 individual per-key <c>.svg</c> files, and a
/// <c>Fonts/</c> folder with exactly one ready-to-bake <c>.ttf</c>/<c>.otf</c> pair plus a
/// glyph-name-to-codepoint map (confirmed against the actual cached zip's file list, not
/// assumed: <c>Keyboard &amp; Mouse/Vector/</c> has 257 files, <c>Keyboard &amp;
/// Mouse/Fonts/</c> has exactly 4). The sprite/Vector route loses on two independent
/// counts, not one: (1) this project's texture pipeline (stb_image, via
/// tools/assetpack/TextureProcessor) has no SVG rasterizer at all - adopting it needs a
/// brand-new dependency just to get pixels, where the font route needs zero new
/// dependencies (FontProcessor's existing FreeType-based baker, already used for every UI
/// font in this project, consumes the .ttf directly); (2) composing a sprite icon INTO the
/// middle of an existing text label needs a new inline-image-in-text layout primitive this
/// UI system does not have (baseline alignment, per-glyph width reservation in text flow) -
/// genuinely new UI machinery. A font glyph needs none of that: it is one character like
/// any other, in a string, at whatever size the surrounding text already uses.
/// </para>
/// <para>
/// <b>The one asterisk on "no new machinery":</b> this specific Kenney font is icon glyphs
/// ONLY - dumping its cmap table shows 257 codepoints, every one in the Private Use Area
/// (0xE0xx), zero in ordinary ASCII. So it cannot share a single string/font with the rest
/// of an English-language sentence the way a hybrid icon+Latin font could - "[key] Wire
/// tool" cannot become one string in one font with the bracket text simply swapped for a
/// codepoint, because the words around it would render as missing-glyph boxes in this font.
/// The fix composes TWO existing widgets instead of one (an icon-font <c>Text</c> for the
/// key, an ordinary-font <c>Text</c> for the words) rather than inventing multi-font text
/// runs - see <c>UiHud.SetKeyHint</c> for the layout. Still zero new component types, zero
/// new rendering code: <see cref="Ui.CreateText"/>/<see cref="Ui.SetFont"/>/
/// <see cref="Ui.SetRect"/> are all this needs.
/// </para>
/// </remarks>
public static class InputGlyphs
{
    /// <summary>The baked font name to pass to <see cref="Ui.SetFont"/> for any entity
    /// displaying a glyph from <see cref="GetGlyph"/>.</summary>
    public const string FontName = "InputPromptsKeyboardMouse";

    // Codepoints copied verbatim from assets/fonts/InputPromptsKeyboardMouse.charmap.txt
    // (the exact map ImportFont wrote from the pack's own kenney_input_keyboard_&_mouse_map.txt).
    // Deliberately covers every Key this project could plausibly bind to a HUD-visible
    // action (all letters/digits/punctuation/navigation/function keys), not just the two
    // in use today (T, G) - a level designer rebinding ToolFireKey/InteractKey to anything
    // reasonable should still get an icon, not silently fall back to bracket text.
    private static readonly Dictionary<Key, char> Map = new()
    {
        [Key.A] = '\uE015', [Key.B] = '\uE036', [Key.C] = '\uE046', [Key.D] = '\uE056',
        [Key.E] = '\uE05A', [Key.F] = '\uE066', [Key.G] = '\uE082', [Key.H] = '\uE084',
        [Key.I] = '\uE088', [Key.J] = '\uE08C', [Key.K] = '\uE08E', [Key.L] = '\uE090',
        [Key.M] = '\uE092', [Key.N] = '\uE096', [Key.O] = '\uE09E', [Key.P] = '\uE0A3',
        [Key.Q] = '\uE0B3', [Key.R] = '\uE0B9', [Key.S] = '\uE0BD', [Key.T] = '\uE0CF',
        [Key.U] = '\uE0D9', [Key.V] = '\uE0DD', [Key.W] = '\uE0DF', [Key.X] = '\uE0E3',
        [Key.Y] = '\uE0E5', [Key.Z] = '\uE0E7',

        [Key.Num0] = '\uE001', [Key.Num1] = '\uE003', [Key.Num2] = '\uE005', [Key.Num3] = '\uE007',
        [Key.Num4] = '\uE009', [Key.Num5] = '\uE00B', [Key.Num6] = '\uE00D', [Key.Num7] = '\uE00F',
        [Key.Num8] = '\uE011', [Key.Num9] = '\uE013',

        [Key.Space] = '\uE0CB', [Key.Apostrophe] = '\uE01B', [Key.Comma] = '\uE050',
        [Key.Minus] = '\uE094', [Key.Period] = '\uE0AD', [Key.Slash] = '\uE0C9',
        [Key.Semicolon] = '\uE0C1', [Key.Equal] = '\uE060', [Key.LeftBracket] = '\uE044',
        [Key.Backslash] = '\uE0C7', [Key.RightBracket] = '\uE03E', [Key.GraveAccent] = '\uE0D7',

        [Key.Escape] = '\uE062', [Key.Enter] = '\uE05E', [Key.Tab] = '\uE0D1',
        [Key.Backspace] = '\uE038', [Key.Insert] = '\uE08A', [Key.Delete] = '\uE058',
        [Key.Right] = '\uE021', [Key.Left] = '\uE01F', [Key.Down] = '\uE01D', [Key.Up] = '\uE023',
        [Key.PageUp] = '\uE0A7', [Key.PageDown] = '\uE0A5', [Key.Home] = '\uE086', [Key.End] = '\uE05C',

        [Key.CapsLock] = '\uE048', [Key.ScrollLock] = '\uE0BF', [Key.NumLock] = '\uE098',
        [Key.PrintScreen] = '\uE0B1', [Key.Pause] = '\uE0A9',

        [Key.F1] = '\uE067', [Key.F2] = '\uE06F', [Key.F3] = '\uE071', [Key.F4] = '\uE073',
        [Key.F5] = '\uE075', [Key.F6] = '\uE077', [Key.F7] = '\uE079', [Key.F8] = '\uE07B',
        [Key.F9] = '\uE07D', [Key.F10] = '\uE068', [Key.F11] = '\uE06A', [Key.F12] = '\uE06C',

        [Key.KeypadEnter] = '\uE09A', [Key.KeypadAdd] = '\uE09C',

        [Key.LeftShift] = '\uE0C3', [Key.RightShift] = '\uE0C3',
        [Key.LeftCtrl] = '\uE054', [Key.RightCtrl] = '\uE054',
        [Key.LeftAlt] = '\uE017', [Key.RightAlt] = '\uE017',
        [Key.LeftSuper] = '\uE0E1', [Key.RightSuper] = '\uE0E1',
    };

    /// <summary>The icon glyph for <paramref name="key"/>, or null if this pack has no
    /// icon for it (e.g. <see cref="Key.Menu"/>, or a keypad key without a dedicated
    /// glyph) - callers fall back to plain bracket text ("[KeyName]") in that case, so a
    /// rebind to an unmapped key is never silently unreadable.</summary>
    public static char? GetGlyph(Key key) => Map.TryGetValue(key, out char glyph) ? glyph : null;
}
