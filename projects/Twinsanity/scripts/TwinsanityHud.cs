using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The original's in-game HUD: a wumpa counter top left and a Crash-head lives counter top
/// right, both hidden until their value changes (pickup, crate, death, respawn), then held
/// and put away again. Art is the disc's own: Startup/Icons.psm tiles (ui/icons) and the
/// Startup/Fonts/Crash_Euro.psf bitmap font cut into one PNG per glyph (ui/fonts), both
/// written by tw-extract. Layout is in the PS2's 640x480 frame, scaled by screen height and
/// pinned to the screen's corners. TwinsanityLevel feeds it every frame.
/// </summary>
public sealed class TwinsanityHud
{
	private const string IconDir = "project://assets/ui/icons/";
	private const string FontDir = "project://assets/ui/fonts/Crash_Euro/";
	private const string FontMetrics = "project://assets/ui/fonts/Crash_Euro.font.json";
	private const float FrameHeight = 480.0f;

	// Measured on rig frames (logs/hud/rig_hud_top.png, 640x480 PAL frame): icon boxes, the digit run's
	// start (wumpa) / end (lives), and the glyph scale ('4' ink 17x25 texels shows 33x28 px).
	// Counters slide in from their own screen edge and back out (logs/hud/rig_crate_slow.png: the lives
	// counter leaving through the right edge); hold ~3 s (logs/hud/rig_drown_slow.png, respawn 6.2 s ->
	// hidden by 9.8 s). ponytail: slide time is estimated from ~0.6 s-spaced rig frames; a 30 fps
	// capture would pin it.
	private const float WumpaIconX = 37.0f, WumpaIconY = 25.0f, WumpaIconW = 53.0f, WumpaIconH = 67.0f;
	private const float WumpaDigitsX = 102.0f;
	private const float LivesIconRight = 605.0f, LivesIconY = 22.0f, LivesIconW = 78.0f, LivesIconH = 73.0f;
	private const float LivesDigitsRight = 511.0f;
	private const float DigitsY = 37.0f, GlyphScaleX = 1.9f, GlyphScaleY = 1.12f;
	private const float SlideTime = 0.25f, HoldTime = 3.0f;

	// The gem pickup pop (COM_GEM_PICKUP's AddGem). Rig: logs/gemreward/rig_red10_*.png, a 0.1x-speed capture
	// timed from the level's gem word (0x98EFA4) changing. The collected gem's icon (Icons_12..07, as the pause
	// track) grows and fades in at the top centre 0.07-0.30 s after the pickup, holds, then shrinks and fades
	// out 1.90-2.13 s; sparkles in the gem's colour burst out of it in a ring from ~0.2 s, drift and are gone by
	// ~1.9 s. Box from the 1.9 s frame: ink x 268..370, y 66..136 = the 256x256 icon (ink rows 38..231) drawn
	// 102 x 93 about (319, 98.5), i.e. centred on the frame. Crash keeps control (the script sets no input).
	private const float GemPopW = 102.0f, GemPopH = 93.0f, GemPopCy = 98.5f;
	private const float GemPopIn0 = 0.07f, GemPopIn1 = 0.30f, GemPopOut0 = 1.90f, GemPopOut1 = 2.13f;
	private const int GemSparkCount = 24;
	// Sparkle tints per TwinsanityPause.GemSlot: yellow, red, purple, green, clear, blue.
	private static readonly Vector4[] GemSparkTint =
	{
		new(1.0f, 0.9f, 0.3f, 1.0f), new(1.0f, 0.22f, 0.2f, 1.0f), new(0.8f, 0.4f, 1.0f, 1.0f),
		new(0.45f, 1.0f, 0.3f, 1.0f), new(0.9f, 0.95f, 1.0f, 1.0f), new(0.35f, 0.6f, 1.0f, 1.0f),
	};

	private sealed class Counter
	{
		public Entity Icon;
		public readonly List<Entity> Digits = new();
		public int Value = int.MinValue;
		public float Age = float.MaxValue; // seconds since last shown; MaxValue = hidden
		public bool Left;
	}

	/// <summary>Where collected wumpa fly to: the wumpa icon's resting centre, 0..1 viewport UV (y down).</summary>
	public static Vector2 WumpaCounterScreen { get; private set; } = new(0.1f, 0.1f);

	/// <summary>Ink box of a Crash_Euro glyph, in texels (advance excluded). False when the
	/// font has no such glyph. TwinsanityPause's menu text uses the same font.</summary>
	public bool TryGlyph(char c, out float w, out float h)
	{
		(w, h) = _glyphs.TryGetValue(c, out var g) ? g : (0.0f, 0.0f);
		return g.W > 0.0f;
	}

	/// <summary>Pop both counters back in, as the original does when the pause menu closes
	/// (logs/hud/rig_pause_slow.png: the HUD reappears after Start).</summary>
	public void PopBoth()
	{
		if (_canvas.IsValid)
		{
			Show(_wumpa!);
			Show(_lives!);
		}
	}

	private readonly Dictionary<char, (float W, float H)> _glyphs = new();
	private Entity _canvas;
	private Entity _frame;
	private Counter? _wumpa;
	private Counter? _lives;
	private float _lastTime = -1.0f;
	private bool _wasDead;
	private Entity _gemPop;
	private readonly Entity[] _gemSparks = new Entity[GemSparkCount];
	private readonly (float Angle, float Born, float Life, float Reach, float Size, float Drift)[] _sparkSeeds = new (float, float, float, float, float, float)[GemSparkCount];
	private float _gemPopStart = -1.0f;
	private int _gemPopSlot;

	/// <summary>AddGem: pop gem <paramref name="slot"/> (TwinsanityPause.GemSlot) on the HUD.</summary>
	public void PopGem(int slot)
	{
		_gemPopSlot = slot;
		_gemPopStart = Time.TotalTime;
		var rng = new System.Random(0x6E3 + slot);
		float R(float lo, float hi) => lo + (hi - lo) * (float)rng.NextDouble();
		for (int i = 0; i < GemSparkCount; i++)
		{
			float born = R(0.18f, 0.5f);
			_sparkSeeds[i] = (MathF.Tau * i / GemSparkCount + R(-0.15f, 0.15f), born, MathF.Min(R(0.9f, 1.6f), 1.9f - born),
				R(50.0f, 110.0f), R(10.0f, 18.0f), R(-15.0f, 25.0f));
		}
		if (_gemPop.IsValid)
		{
			Ui.SetImageTexture(_gemPop, $"{IconDir}Icons_{12 - slot:00}.png");
		}
	}

	/// <summary>Per frame. A changed count pops its counter; the respawn pops both, as in the original.</summary>
	public void Update(int wumpa, int lives, bool dead)
	{
		if (!_canvas.IsValid && !Build())
		{
			return;
		}
		float now = Time.UnscaledTime;
		float dt = _lastTime < 0.0f ? 0.0f : now - _lastTime;
		_lastTime = now;

		Set(_wumpa!, wumpa);
		Set(_lives!, lives);
		if (_wasDead && !dead)
		{
			Show(_wumpa!);
			Show(_lives!);
		}
		_wasDead = dead;

		Vector4 screen = Ui.GetRect(_frame);
		float scale = screen.W / FrameHeight;
		if (scale <= 0.0f)
		{
			return;
		}
		Layout(_wumpa!, dt, scale, screen.Z);
		Layout(_lives!, dt, scale, screen.Z);
		WumpaCounterScreen = new Vector2((WumpaIconX + WumpaIconW * 0.5f) * scale / screen.Z, (WumpaIconY + WumpaIconH * 0.5f) / FrameHeight);
		LayoutGemPop(scale, screen.Z);
	}

	private bool Build()
	{
		string? metrics = Assets.ReadText(FontMetrics);
		if (metrics == null)
		{
			Log.Warn($"[Twinsanity] {FontMetrics} missing - run tw-extract (Startup/) for the HUD art.");
			return false;
		}
		using (JsonDocument doc = JsonDocument.Parse(metrics))
		{
			foreach (JsonProperty g in doc.RootElement.GetProperty("glyphs").EnumerateObject())
			{
				JsonElement v = g.Value;
				_glyphs[(char)int.Parse(g.Name)] = (v[3].GetSingle(), v[4].GetSingle());
			}
		}
		_canvas = Ui.CreateCanvas();
		_canvas.MarkTransient();
		_frame = Ui.CreateImage(_canvas);
		Ui.SetAnchors(_frame, Vector2.Zero, Vector2.One);
		Ui.SetOffsets(_frame, Vector2.Zero, Vector2.Zero);
		Ui.SetImageColor(_frame, Vector4.Zero);
		_wumpa = NewCounter("Icons_13.png", true);
		_lives = NewCounter("Icons_00.png", false);
		for (int i = 0; i < GemSparkCount; i++)
		{
			// ui_pause shape 4: a soft star in material colour 0, faded by the image alpha.
			Entity e = Image(null);
			Ui.SetMaterial(e, "ui_pause");
			Ui.SetMaterialParams(e, new Vector4(0.0f, 4.0f, 0.0f, 0.0f));
			e.SetActive(false);
			_gemSparks[i] = e;
		}
		_gemPop = Image(IconDir + "Icons_06.png");
		_gemPop.SetActive(false);
		if (_gemPopStart >= 0.0f)
		{
			Ui.SetImageTexture(_gemPop, $"{IconDir}Icons_{12 - _gemPopSlot:00}.png");
		}
		return true;
	}

	private Counter NewCounter(string icon, bool left)
	{
		var c = new Counter { Left = left, Icon = Image(IconDir + icon) };
		for (int i = 0; i < 3; i++)
		{
			c.Digits.Add(Image(null));
		}
		Hide(c);
		return c;
	}

	private Entity Image(string? texture)
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetPivot(e, Vector2.Zero);
		Ui.SetImageColor(e, Vector4.One);
		if (texture != null)
		{
			Ui.SetImageTexture(e, texture);
		}
		return e;
	}

	private void Set(Counter c, int value)
	{
		if (value == c.Value)
		{
			return;
		}
		bool first = c.Value == int.MinValue;
		c.Value = value;
		string text = Math.Max(value, 0).ToString();
		for (int i = 0; i < c.Digits.Count; i++)
		{
			if (i < text.Length)
			{
				Ui.SetImageTexture(c.Digits[i], $"{FontDir}{(int)text[i]}.png");
			}
		}
		if (!first)
		{
			Show(c);
		}
	}

	private static void Show(Counter c)
	{
		// Already fully on screen: restart the hold. Sliding out: come back from where it is.
		if (c.Age >= SlideTime && c.Age < SlideTime + HoldTime)
		{
			c.Age = SlideTime;
		}
		else if (c.Age >= SlideTime + HoldTime && c.Age < 2.0f * SlideTime + HoldTime)
		{
			c.Age = 2.0f * SlideTime + HoldTime - c.Age;
		}
		else if (c.Age >= 2.0f * SlideTime + HoldTime)
		{
			c.Age = 0.0f;
		}
		c.Icon.SetActive(true);
	}

	private static void Hide(Counter c)
	{
		c.Age = float.MaxValue;
		c.Icon.SetActive(false);
		foreach (Entity d in c.Digits)
		{
			d.SetActive(false);
		}
	}

	private void Layout(Counter c, float dt, float s, float screenW)
	{
		if (c.Age == float.MaxValue)
		{
			return;
		}
		c.Age += dt;
		float total = 2.0f * SlideTime + HoldTime;
		if (c.Age >= total)
		{
			Hide(c);
			return;
		}
		// 0 = off its screen edge, 1 = in place.
		float t = c.Age < SlideTime ? c.Age / SlideTime : c.Age < SlideTime + HoldTime ? 1.0f : (total - c.Age) / SlideTime;

		string text = Math.Max(c.Value, 0).ToString();
		float width = 0.0f;
		foreach (char ch in text)
		{
			width += _glyphs.TryGetValue(ch, out var g) ? g.W * GlyphScaleX : 0.0f;
		}
		float x;
		if (c.Left)
		{
			float shift = -(1.0f - t) * (WumpaDigitsX + width);
			Place(c.Icon, (WumpaIconX + shift) * s, WumpaIconY * s, WumpaIconW * s, WumpaIconH * s);
			x = WumpaDigitsX + shift;
		}
		else
		{
			// Right-hand elements keep their distance from the screen's right edge.
			float right = screenW / s - 640.0f + (1.0f - t) * (640.0f - LivesDigitsRight + width);
			Place(c.Icon, (right + LivesIconRight - LivesIconW) * s, LivesIconY * s, LivesIconW * s, LivesIconH * s);
			x = right + LivesDigitsRight - width;
		}
		for (int i = 0; i < c.Digits.Count; i++)
		{
			Entity d = c.Digits[i];
			bool used = i < text.Length && _glyphs.ContainsKey(text[i]);
			d.SetActive(used);
			if (!used)
			{
				continue;
			}
			var g = _glyphs[text[i]];
			Place(d, x * s, DigitsY * s, g.W * GlyphScaleX * s, g.H * GlyphScaleY * s);
			x += g.W * GlyphScaleX;
		}
	}

	private void LayoutGemPop(float s, float screenW)
	{
		float t = _gemPopStart < 0.0f ? float.MaxValue : Time.TotalTime - _gemPopStart;
		bool live = t < GemPopOut1;
		// Grow and fade in, hold, shrink (to ~0.3, rig 2.10 s) and fade out.
		float k = t < GemPopIn0 ? 0.0f : t < GemPopIn1 ? (t - GemPopIn0) / (GemPopIn1 - GemPopIn0)
			: t < GemPopOut0 ? 1.0f : 1.0f - (t - GemPopOut0) / (GemPopOut1 - GemPopOut0);
		k = Math.Clamp(k, 0.0f, 1.0f);
		float size = t < GemPopIn1 ? k : 0.3f + 0.7f * k;
		float cx = screenW / s * 0.5f; // frame px, centred on the screen
		_gemPop.SetActive(live && k > 0.0f);
		if (live)
		{
			float w = GemPopW * size, h = GemPopH * size;
			Place(_gemPop, (cx - w * 0.5f) * s, (GemPopCy - h * 0.5f) * s, w * s, h * s);
			Ui.SetImageColor(_gemPop, new Vector4(1.0f, 1.0f, 1.0f, k));
		}
		Vector4 tint = GemSparkTint[Math.Clamp(_gemPopSlot, 0, GemSparkTint.Length - 1)];
		for (int i = 0; i < GemSparkCount; i++)
		{
			var p = _sparkSeeds[i];
			float age = t - p.Born;
			bool on = live && age >= 0.0f && age < p.Life;
			Entity e = _gemSparks[i];
			e.SetActive(on);
			if (!on)
			{
				continue;
			}
			// Out of the gem's rim in a ring that slows, then drifts; twinkles as it fades.
			float r = 40.0f + p.Reach * (1.0f - MathF.Exp(-3.0f * age));
			float x = cx + MathF.Cos(p.Angle) * r;
			float y = GemPopCy + MathF.Sin(p.Angle) * r * 0.75f + p.Drift * age;
			float a = MathF.Min(age / 0.05f, 1.0f) * (1.0f - age / p.Life) * (0.7f + 0.3f * MathF.Sin(age * 25.0f + i));
			Place(e, (x - p.Size * 0.5f) * s, (y - p.Size * 0.5f) * s, p.Size * s, p.Size * s);
			Ui.SetMaterialColors(e, tint, tint);
			Ui.SetImageColor(e, new Vector4(1.0f, 1.0f, 1.0f, a));
		}
	}

	// Top-left anchored box at (x, y) screen px, y down.
	private static void Place(Entity e, float x, float y, float w, float h)
	{
		Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
		Ui.SetOffsets(e, new Vector2(x, y), new Vector2(x + w, y + h));
	}
}
