using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

/// "HOLD △ TO SKIP" in the bottom letterbox bar, drawn with the disc font (Crash_Euro, as the HUD and pause
/// menu) the way the CrashModded mod shows it (wiki modding/what-changed.md, skip-prompt-beach.png): '^' is the
/// font's triangle glyph. Without a controller the keyboard key is named too. Show draws any other line the
/// same way (the scripts' BottomTextDisplay prompts; '[' '\' ']' are the square, cross and circle glyphs).
public sealed class TwinsanitySkipPrompt
{
	private const string FontDir = "project://assets/ui/fonts/Crash_Euro/";
	private const string FontMetrics = "project://assets/ui/fonts/Crash_Euro.font.json";
	private const float Scale = 0.32f;          // 39 font units across, as a fraction of the bar height
	// The rig draws the font pages 0.7 as tall as wide for their pixels: the same letters' ink measures
	// P 0.91, S 1.0, T 1.10, O 1.5 wide/tall there (logs/tutorial/rig_s2_prompt_full.png, rig_prompt_zoom.png)
	// against 0.63, 0.72, 0.75, 1.02 on the pages (logs/hintstrip/glyph_aspect.py).
	private const float Squash = 0.7f;
	private const float InkCentre = 0.53f;       // the letters' ink centres this far down their 39-unit cells
	// The rig's glyphs are dimmer than the extracted pages (brightened for the HUD): their brightest pixels peak at
	// 184/255 in the hint strip and the scene prompts alike (logs/tutorialroute/rig_hint_tp.png,
	// logs/tutorial/rig_s1_prompt.png) against 255 here.
	private static readonly Vector4 GlyphTint = new(0.72f, 0.72f, 0.72f, 1.0f);
	private const string PadText = "HOLD ^ TO SKIP";
	private const string KeyText = "HOLD ^ / eNTeR TO SKIP"; // the font's 'E' slot is the circle button: 'e' is its E

	private static readonly Dictionary<char, float> s_widths = new();
	private readonly List<(Entity Glyph, float X, float W)> _glyphs = new();
	private Entity _canvas;
	private string _text = "";
	private float _total;

	/// <summary>Show the prompt in <paramref name="bar"/> (an image whose rect is the bottom bar), or hide it. On the
	/// modded disc it is the scripts' bottom-text slot (TwinsanityCutscenes.HintCentre).</summary>
	public void Update(Entity canvas, Entity bar, bool visible) =>
		Show(canvas, bar, visible ? (Gamepad.IsConnected() ? PadText : KeyText) : "", Scale, 0.345f);

	/// <summary>Draw <paramref name="text"/> centred across <paramref name="bar"/>, 39 font units spanning
	/// <paramref name="scale"/> of the bar's height across, the letters centred <paramref name="centre"/> of the way down it.</summary>
	public void Show(Entity canvas, Entity bar, string text, float scale, float centre)
	{
		if (text != _text || canvas != _canvas)
		{
			Build(canvas, text);
		}
		if (_glyphs.Count == 0)
		{
			return;
		}
		Vector4 r = Ui.GetRect(bar); // x, y, width, height in pixels
		float s = r.W * scale / 39.0f, h = 39.0f * s * Squash; // glyphs are 39 units tall in the font's own frame
		float x0 = r.X + (r.Z - _total * s) * 0.5f, y = r.Y + r.W * centre - h * InkCentre;
		foreach ((Entity g, float x, float w) in _glyphs)
		{
			Ui.SetRect(g, x0 + x * s, y, w * s, h);
		}
	}

	private void Build(Entity canvas, string text)
	{
		foreach ((Entity g, _, _) in _glyphs)
		{
			if (g.IsValid)
			{
				g.Destroy();
			}
		}
		_glyphs.Clear();
		_canvas = canvas;
		_text = text;
		_total = 0.0f;
		LoadWidths();
		foreach (char c in text)
		{
			float w = s_widths.GetValueOrDefault(c, 16.0f);
			if (c != ' ')
			{
				Entity e = Ui.CreateImage(canvas);
				Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
				Ui.SetPivot(e, Vector2.Zero); // SetRect's x/y is the glyph's top-left, not its (default) centre
				Ui.SetImageColor(e, GlyphTint);
				Ui.SetImageTexture(e, $"{FontDir}{(int)c}.png");
				_glyphs.Add((e, _total, w));
			}
			_total += w;
		}
	}

	private static void LoadWidths()
	{
		if (s_widths.Count > 0 || Assets.ReadText(FontMetrics) is not string json)
		{
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(json);
		foreach (JsonProperty g in doc.RootElement.GetProperty("glyphs").EnumerateObject())
		{
			s_widths[(char)int.Parse(g.Name)] = g.Value[3].GetSingle();
		}
	}
}
