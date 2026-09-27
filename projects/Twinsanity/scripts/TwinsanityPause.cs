using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The in-game pause menu (N. Sanity Beach). Escape or gamepad Start freezes the game
/// (Time.Scale 0) and fades the menu in; Up/Down (d-pad, left stick) moves the selection through
/// the option drum, Start/Back/confirm closes it and the HUD pops back in (TwinsanityLevel calls
/// TwinsanityHud.PopBoth on JustClosed). All menu animation runs on the unscaled clock: the game
/// is frozen underneath.
///
/// A redesign of the original's layout, not a copy: the same parts - the N. Sanity Island
/// badge on the menu ball, the wumpa / lives / crystal counters on their discs, the six-gem
/// track with its completion "0%", the option drum and the SELECT/BACK and L1/L2/R1/R2 prompts -
/// float straight on a vignette dim of the frozen world - no big backdrop - in a two-column grid
/// with even margins. The selection is a glowing capsule that glides between rows. Everything is
/// laid out in a 640x480 design frame fitted inside the screen (scale = min of the two axis
/// fits, centred), so 16:9, 4:3 and ultrawide show the same composition with nothing clipped.
///
/// Art is the disc's own: the option text, counter digits, "0%", SELECT/BACK and the
/// L1/L2/R1/R2 + arrow prompts are Crash_Euro glyphs (ui/fonts, item strings from
/// ui/text/English.txt; the prompt glyph characters are English.txt lines 30-33, kept as
/// literals because the file's high-byte characters do not survive the engine's UTF-8 asset
/// reads). The badge is ui/titles/English/Hub01_00.png, the counter and gem icons are ui/icons.
/// The boxes, discs and ball are analytic shapes in the project's ui_pause.slang.
///
/// Not faithful, stated plainly: OPTIONS, SAVE GAME, LOAD GAME and QUIT GAME have no target in
/// this single-level build, so selecting them closes the menu exactly like RESUME (the
/// original opens sub-screens); DISABLE AUTOSAVE is drawn greyed and Up/Down skips it, as in
/// the original.
/// </summary>
public sealed class TwinsanityPause
{
	private const string FontDir = "project://assets/ui/fonts/Crash_Euro/";
	private const string FontMetrics = "project://assets/ui/fonts/Crash_Euro.font.json";
	private const string IconDir = "project://assets/ui/icons/";
	private const string TitlePath = "project://assets/ui/titles/English/Hub01_00.png";
	private const string StringsPath = "project://assets/ui/text/English.txt";
	private const float GlyphH = 39.0f; // Crash_Euro cell height, texels

	// Option drum: English.txt line indexes - 12 'options', 27 'save game', 11 'load game',
	// 23 'disable autosave', 28 'quit game', 29 'resume'. Line 3 (DISABLE AUTOSAVE) is greyed
	// out and skipped by Up/Down, exactly as in the original.
	private static readonly int[] ItemLines = { 12, 27, 11, 23, 28, 29 };
	private const int GreyedItem = 3;
	private const int DefaultItem = 5; // the menu opens on RESUME, as in the original

	// The bottom prompts: Crash_Euro button glyphs - '\' cross, '^' triangle, '<' '>' arrows,
	// '{' '¦' L1 L2, '}' '¬' R1 R2 (English.txt lines 30-33).
	private const string SelectRun = "select \\";
	private const string BackRun = "^ back";
	private const string ShoulderLeft = "< { \u00A6";   // '< { ¦'
	private const string ShoulderRight = "} \u00AC >";  // '} ¬ >'

	// Transitions: the menu fades in while growing from OpenScale with an ease-out; closing
	// fades and shrinks back with an ease-in. The dim follows the same curve.
	private const float OpenTime = 0.22f, CloseTime = 0.16f, OpenScale = 0.92f;
	private const float DimAlpha = 1.0f;          // the vignette shader carries the dim's depth
	private const float HighlightRate = 18.0f;    // 1/s: capsule glide and row grow (exponential)
	private const float PulsePeriod = 1.1f, RowPulse = 0.02f;
	private const float IconWobble = 0.03f, WobblePeriod = 1.4f; // badge/icons breathe while idle

	// Layout, in the 640x480 design frame (element centres unless named X0/Y0), with a 40-unit
	// outer margin: left column 40..280 (badge + counters), right column 300..600 (option drum),
	// the gem track across the width, a footer row of prompts. Boxes cast a small soft shadow.
	private const float FrameW = 640.0f, FrameH = 480.0f, CentreX = 320.0f, CentreY = 240.0f;
	private const float BoxShadow = 6.0f;
	private const float BadgeX = 160.0f, BadgeY = 106.0f, BadgeW = 108.0f, BadgeH = 110.0f, BallSize = 132.0f;
	private const float PillX = 164.0f, PillY = 204.0f, PillStep = 44.0f, PillW = 200.0f, PillH = 34.0f;
	private const float DiscX = 72.0f, DiscSize = 42.0f, CountRight = 246.0f, DigitScaleX = 0.9f, DigitScaleY = 0.66f;
	private const float RowsX = 450.0f, RowsY = 62.0f, RowStep = 47.0f, CapsuleW = 284.0f, CapsuleH = 42.0f, CapsuleGlow = 12.0f;
	private const float RowScaleX = 0.78f, RowScaleY = 0.56f, RowSelectedX = 0.9f, RowSelectedY = 0.66f;
	private const float StripX = 320.0f, StripY = 356.0f, StripW = 560.0f, StripH = 52.0f;
	private const float GemX0 = 80.0f, GemStep = 70.0f, GemSize = 38.0f, SocketSize = 44.0f;
	private const float ZeroPctX = 540.0f, ZeroPctScaleX = 1.0f, ZeroPctScaleY = 0.72f;
	private const float FooterY = 428.0f, FooterLeft = 44.0f, FooterRight = 596.0f, FooterGap = 18.0f;
	private const float PromptScaleX = 0.7f, PromptScaleY = 0.46f, ShoulderScaleX = 0.72f, ShoulderScaleY = 0.6f;

	// Counters, top to bottom: wumpa, Crash head (lives), crystals. Icon boxes in design units
	// (the crystal cluster is a tall 64x53 texel tile the PS2 draws stretched).
	private static readonly string[] CounterIcons = { "Icons_13.png", "Icons_00.png", "Icons_15.png" };
	private static readonly (float W, float H)[] IconSizes = { (29.0f, 36.0f), (31.0f, 40.0f), (18.0f, 50.0f) };

	// Colours: the menu's blues and the gem track's cyan, at full brightness.
	private static readonly Vector4 PillTop = new(0.02f, 0.11f, 0.28f, 1.0f);
	private static readonly Vector4 PillBottom = new(0.05f, 0.24f, 0.5f, 0.35f);
	private static readonly Vector4 StripTop = new(0.3f, 0.82f, 0.74f, 1.0f);
	private static readonly Vector4 StripBottom = new(0.13f, 0.55f, 0.56f, 0.95f);
	private static readonly Vector4 SocketTop = new(0.04f, 0.27f, 0.33f, 1.0f);
	private static readonly Vector4 SocketBottom = new(0.09f, 0.42f, 0.45f, 0.4f);
	private static readonly Vector4 CapsuleTop = new(1.0f, 0.99f, 0.93f, 1.0f);
	private static readonly Vector4 CapsuleBottom = new(0.8f, 0.88f, 0.94f, 1.0f);
	private static readonly Vector4 DiscLit = new(0.47f, 0.85f, 0.97f, 1.0f);
	private static readonly Vector4 DiscShade = new(0.08f, 0.37f, 0.86f, 1.0f);
	private static readonly Vector4 BallLit = new(0.33f, 0.77f, 0.96f, 1.0f);
	private static readonly Vector4 BallShade = new(0.08f, 0.48f, 0.75f, 1.0f);
	private const float RowIdle = 0.82f;                       // unselected row brightness
	private static readonly Vector3 GreyedTint = new(0.5f, 0.58f, 0.7f);
	private const float GreyedAlpha = 0.55f;

	private readonly Dictionary<char, (float W, float H)> _glyphs = new();
	private readonly Dictionary<Entity, TextRun> _runs = new();

	private enum Phase { Closed, Opening, Open, Closing }

	private Phase _phase = Phase.Closed;
	private float _phaseTime; // unscaled seconds in the current phase
	private float _lastUnscaled = -1.0f;
	private int _selected = DefaultItem;
	private bool _justClosed;
	private bool _built;
	private readonly bool[] _latched = new bool[8]; // open, up, down, confirm, back, stick-up, stick-down, spare

	private Entity _canvas;
	private Entity _frame;
	private Entity _dim;
	private Entity _ball;
	private Entity _badge;
	private readonly Entity[] _pills = new Entity[3];
	private readonly Entity[] _discs = new Entity[3];
	private readonly Entity[] _icons = new Entity[3];
	// Each counter owns three digit images, re-textured when its value changes (as the HUD does).
	private readonly Entity[,] _digits = new Entity[3, 3];
	private readonly int[] _shownCounts = { -1, -1, -1 };
	private Entity _strip;
	private readonly Entity[] _sockets = new Entity[6];
	private readonly Entity[] _gems = new Entity[6];
	private Entity _zeroPct;
	private Entity _capsule;
	private readonly Entity[] _rows = new Entity[6];
	private Entity _select, _back, _shoulderL, _shoulderR;
	private readonly List<Entity> _shapes = new(); // every non-glyph element, for SetVisible

	// Per-frame placement state.
	private float _s = 1.0f;     // screen scale: design units -> screen px
	private float _ox, _oy;      // screen px of the design frame's top-left corner
	private float _k = 1.0f;     // open/close scale about the frame centre
	private float _a = 1.0f;     // master alpha
	private float _hiY;          // the selection capsule's current y (design units)
	private readonly float[] _rowK = new float[6]; // per row: 0 idle .. 1 selected, eased

	private sealed class TextRun
	{
		public Entity First = default;
		public readonly List<(Entity Glyph, float Advance, float Width)> Glyphs = new();
		public float Total;
	}

	/// <summary>True on the frame the menu finished closing: TwinsanityLevel pops the HUD back in.</summary>
	public bool JustClosed => _justClosed;

	// Gem track slots, left to right (rig logs/gameplay/rig_gemtrack_all.png: the level's gem word
	// at 0x98EFA4 set bit by bit, bit 5 - slot): yellow, red, purple, green, clear, blue. The disc
	// icons for them are Icons_12 (yellow) down to Icons_07 (blue); Icons_06 is the empty slot.
	private static readonly string[] GemColours = { "yellow", "red", "purple", "green", "clear", "blue" };

	/// <summary>The gem-track slot of a gem pickup (GEM_RED, act_GEM_RED ...), or -1 if it is not a gem.</summary>
	public static int GemSlot(string objectName)
	{
		string n = objectName.ToLowerInvariant();
		n = n.StartsWith("act_", StringComparison.Ordinal) ? n[4..] : n;
		return n.StartsWith("gem_", StringComparison.Ordinal) ? Array.FindIndex(GemColours, c => n.AsSpan(4).StartsWith(c)) : -1;
	}

	/// <summary>Per frame, from TwinsanityLevel.OnUpdate. The level's scaled delta freezes with the
	/// game, so timing here is taken from the unscaled clock. <paramref name="gems"/> holds one bit
	/// per collected gem, by GemSlot.</summary>
	public void Update(int wumpa, int lives, int gems)
	{
		_justClosed = false;
		float now = Time.UnscaledTime;
		float dt = _lastUnscaled < 0.0f ? 0.0f : Math.Clamp(now - _lastUnscaled, 0.0f, 0.1f);
		_lastUnscaled = now;

		bool startEdge = (Input.IsKeyDown(Key.Escape) || Gamepad.IsDown(GamepadButton.Start)) && !_latched[0];
		_latched[0] = Input.IsKeyDown(Key.Escape) || Gamepad.IsDown(GamepadButton.Start);

		switch (_phase)
		{
			case Phase.Closed:
				if (startEdge && !_built)
				{
					_built = Build();
					if (!_built)
					{
						return; // art missing: behave as if the key was never pressed
					}
				}
				if (startEdge && _built)
				{
					for (int i = 0; i < _gems.Length; i++)
					{
						bool have = (gems >> i & 1) != 0;
						Ui.SetImageTexture(_gems[i], IconDir + (have ? $"Icons_{12 - i:00}.png" : "Icons_06.png"));
					}
					_selected = DefaultItem;
					SnapHighlight();
					_phase = Phase.Opening;
					_phaseTime = 0.0f;
					Time.Pause(); // gameplay freezes; this menu keeps animating unscaled
					TwinsanityAudio.PauseOpened();
					// The press that opened the menu is still down next frame: Escape is also Back
					// and would close it again at once. Every menu button starts latched, so only a
					// fresh press acts.
					for (int i = 1; i < _latched.Length; i++)
					{
						_latched[i] = true;
					}
					SetVisible(true);
				}
				return;
			case Phase.Opening:
			case Phase.Open:
				PollMenuInput(startEdge);
				break;
			case Phase.Closing:
				break;
		}

		_phaseTime += dt;
		switch (_phase)
		{
			case Phase.Opening when _phaseTime >= OpenTime:
				_phase = Phase.Open;
				_phaseTime = 0.0f;
				break;
			case Phase.Closing when _phaseTime >= CloseTime:
				_phase = Phase.Closed;
				_phaseTime = 0.0f;
				_selected = DefaultItem;
				SetVisible(false);
				Time.Resume();
				TwinsanityAudio.PauseClosed();
				_justClosed = true;
				return;
		}
		Layout(wumpa, lives, dt, now);
	}

	private void PollMenuInput(bool startEdge)
	{
		bool up = Input.IsKeyDown(Key.Up) || Gamepad.IsDown(GamepadButton.DpadUp);
		bool down = Input.IsKeyDown(Key.Down) || Gamepad.IsDown(GamepadButton.DpadDown);
		float stick = Gamepad.LeftStick.Y;
		if ((up && !_latched[1]) || (stick > 0.5f && !_latched[5]))
		{
			Move(-1);
		}
		if ((down && !_latched[2]) || (stick < -0.5f && !_latched[6]))
		{
			Move(1);
		}
		_latched[1] = up;
		_latched[2] = down;
		_latched[5] = stick > 0.5f;
		_latched[6] = stick < -0.5f;

		bool confirm = Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.Enter) || Gamepad.IsDown(GamepadButton.A);
		bool back = Input.IsKeyDown(Key.Escape) || Gamepad.IsDown(GamepadButton.Y) || Gamepad.IsDown(GamepadButton.Back);
		if ((confirm && !_latched[3]) || (back && !_latched[4]) || startEdge)
		{
			// OPTIONS / SAVE / LOAD / QUIT have no target in this single-level build: like
			// RESUME (and Start / Back) they close the menu. DISABLE AUTOSAVE is unreachable.
			// Rig: Start closes in silence; a confirmed item plays the select sound.
			if (confirm && !_latched[3])
			{
				TwinsanityAudio.MenuSelect();
			}
			_phase = Phase.Closing;
			_phaseTime = 0.0f;
		}
		_latched[3] = confirm;
		_latched[4] = back;
	}

	private void Move(int dir)
	{
		int i = _selected;
		for (int n = 0; n < ItemLines.Length; n++)
		{
			i = (i + dir + ItemLines.Length) % ItemLines.Length;
			if (i != GreyedItem)
			{
				if (_selected != i)
				{
					TwinsanityAudio.MenuMove();
				}
				_selected = i;
				return;
			}
		}
	}

	private void SnapHighlight()
	{
		_hiY = RowsY + RowStep * _selected;
		for (int i = 0; i < _rowK.Length; i++)
		{
			_rowK[i] = i == _selected ? 1.0f : 0.0f;
		}
	}

	private bool Build()
	{
		if (!LoadMetrics())
		{
			return false;
		}
		_canvas = Ui.CreateCanvas();
		_canvas.MarkTransient();
		// The frame image carries the canvas's resolved rect for the layout scale (the HUD
		// does the same). Creation order is draw order: the dim first, the prompts last.
		_frame = Ui.CreateImage(_canvas);
		Ui.SetAnchors(_frame, Vector2.Zero, Vector2.One);
		Ui.SetOffsets(_frame, Vector2.Zero, Vector2.Zero);
		Ui.SetImageColor(_frame, Vector4.Zero);
		_dim = Shape(5);
		Ui.SetAnchors(_dim, Vector2.Zero, Vector2.One);
		Ui.SetOffsets(_dim, Vector2.Zero, Vector2.Zero);

		_ball = Shape(2, BallLit, BallShade);
		_badge = Icon(TitlePath);
		for (int i = 0; i < 3; i++)
		{
			_pills[i] = Shape(6, PillTop, PillBottom);
			_discs[i] = Shape(1, DiscLit, DiscShade);
			_icons[i] = Icon(IconDir + CounterIcons[i]);
			for (int d = 0; d < 3; d++)
			{
				Entity e = Ui.CreateImage(_canvas);
				Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
				Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
				_digits[i, d] = e;
			}
		}
		_strip = Shape(6, StripTop, StripBottom);
		for (int i = 0; i < _gems.Length; i++)
		{
			_sockets[i] = Shape(6, SocketTop, SocketBottom);
			_gems[i] = Icon(IconDir + "Icons_06.png");
		}
		_zeroPct = TextImage("0%");
		_capsule = Shape(7, CapsuleTop, CapsuleBottom);

		string[] items = LoadItems();
		for (int i = 0; i < _rows.Length; i++)
		{
			_rows[i] = TextImage(items[i]);
		}
		_shoulderL = TextImage(ShoulderLeft);
		_select = TextImage(SelectRun);
		_back = TextImage(BackRun);
		_shoulderR = TextImage(ShoulderRight);

		SetVisible(false);
		return true;
	}

	// A ui_pause element: a plain image with the project's UI material (shape per params.y).
	private Entity Shape(int shape, Vector4 top = default, Vector4 bottom = default)
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetImageColor(e, Vector4.One);
		Ui.SetMaterial(e, "ui_pause");
		Ui.SetMaterialParams(e, new Vector4(0.0f, shape, 0.0f, 0.0f));
		Ui.SetMaterialColors(e, top, bottom);
		_shapes.Add(e);
		return e;
	}

	private Entity Icon(string texture)
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetImageColor(e, Vector4.One);
		Ui.SetImageTexture(e, texture);
		_shapes.Add(e);
		return e;
	}

	// One image per glyph, pivoted at its centre. The run is keyed on its first glyph entity;
	// a run is always a real glyph followed by whatever else the string carries.
	private Entity TextImage(string text)
	{
		var run = new TextRun();
		foreach (char c in text)
		{
			if (c != ' ' && _glyphs.TryGetValue(c, out var g) && g.W > 0.0f)
			{
				Entity e = Ui.CreateImage(_canvas);
				Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
				Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
				Ui.SetImageTexture(e, $"{FontDir}{(int)c}.png");
				run.Glyphs.Add((e, run.Total + g.W * 0.5f, g.W));
				if (!run.First.IsValid)
				{
					run.First = e;
				}
				run.Total += g.W;
			}
			else
			{
				// No glyph art (a space, or a character Crash_Euro lacks): advance only. The
				// font has no space glyph; its metrics carry a 16 texel advance, used here.
				run.Total += 16.0f;
			}
		}
		_runs[run.First] = run;
		return run.First;
	}

	// The counter read-outs: up to three right-aligned Crash_Euro digits.
	private void SetCount(int index, int value, float y)
	{
		string text = Math.Clamp(value, 0, 999).ToString();
		if (_shownCounts[index] != value)
		{
			_shownCounts[index] = value;
			for (int d = 0; d < text.Length; d++)
			{
				Ui.SetImageTexture(_digits[index, d], $"{FontDir}{(int)text[d]}.png");
			}
		}
		float xEnd = CountRight;
		for (int d = 2; d >= 0; d--) // right to left: image d shows text[d]
		{
			Entity e = _digits[index, d];
			bool used = d < text.Length;
			e.SetActive(used && _phase != Phase.Closed);
			if (!used)
			{
				continue;
			}
			float w = _glyphs[text[d]].W * DigitScaleX;
			Place(e, xEnd - w * 0.5f, y, w, GlyphH * DigitScaleY);
			Ui.SetImageColor(e, new Vector4(1.0f, 1.0f, 1.0f, _a));
			xEnd -= w;
		}
	}

	// Centre-place an element in design units, through the open/close scale, to screen px.
	private void Place(Entity e, float x, float y, float w, float h)
	{
		x = CentreX + (x - CentreX) * _k;
		y = CentreY + (y - CentreY) * _k;
		Ui.SetRect(e, _ox + x * _s, _oy + y * _s, w * _k * _s, h * _k * _s);
	}

	// A rounded ui_pause box (shape 6/7): the rect grows by the shadow/glow margin on every side,
	// and the shader takes the radius and margin in screen px.
	private void PlaceBox(Entity e, int shape, float x, float y, float w, float h, float radius, float margin, float alpha)
	{
		Place(e, x, y, w + margin * 2.0f, h + margin * 2.0f);
		float px = _s * _k;
		Ui.SetMaterialParams(e, new Vector4(Time.UnscaledTime, shape, radius * px, margin * px));
		Ui.SetImageColor(e, new Vector4(1.0f, 1.0f, 1.0f, alpha * _a));
	}

	// Place a glyph run at (x, cy), scaled (sx, sy), tinted; align -1 puts x at the run's left
	// edge, 0 at its centre, 1 at its right edge.
	private void PlaceRun(Entity key, float x, float cy, float sx, float sy, Vector4 tint, int align = 0)
	{
		TextRun run = _runs[key];
		float startX = x - run.Total * sx * (align + 1) * 0.5f;
		foreach ((Entity glyph, float advance, float width) in run.Glyphs)
		{
			Place(glyph, startX + advance * sx, cy, width * sx, GlyphH * sy);
			Ui.SetImageColor(glyph, tint);
		}
	}

	private float RunWidth(Entity key, float sx) => _runs[key].Total * sx;

	private void Layout(int wumpa, int lives, float dt, float now)
	{
		Vector4 screen = Ui.GetRect(_frame);
		if (screen.Z <= 0.0f || screen.W <= 0.0f)
		{
			return;
		}
		// Fit the design frame inside the screen and centre it: 4:3 fills it, wider screens
		// pillarbox it, taller ones letterbox it - nothing is ever cut off.
		_s = MathF.Min(screen.W / FrameH, screen.Z / FrameW);
		_ox = (screen.Z - FrameW * _s) * 0.5f;
		_oy = (screen.W - FrameH * _s) * 0.5f;

		float p = 1.0f; // 0 = gone, 1 = in place
		float idle = 0.0f;
		switch (_phase)
		{
			case Phase.Opening:
			{
				float t = 1.0f - Math.Min(_phaseTime / OpenTime, 1.0f);
				p = 1.0f - t * t * t; // ease out
				break;
			}
			case Phase.Closing:
			{
				float t = Math.Min(_phaseTime / CloseTime, 1.0f);
				p = 1.0f - t * t; // ease in
				break;
			}
			default:
				idle = _phaseTime;
				break;
		}
		_k = OpenScale + (1.0f - OpenScale) * p;
		_a = p;

		// The selection glides to its row; each row eases toward its idle / selected size.
		float ease = 1.0f - MathF.Exp(-HighlightRate * dt);
		_hiY += (RowsY + RowStep * _selected - _hiY) * ease;
		for (int i = 0; i < _rowK.Length; i++)
		{
			_rowK[i] += ((i == _selected ? 1.0f : 0.0f) - _rowK[i]) * ease;
		}

		Ui.SetImageColor(_dim, new Vector4(1.0f, 1.0f, 1.0f, DimAlpha * p));
		Ui.SetMaterialParams(_dim, new Vector4(now, 5.0f, 0.0f, 0.0f));

		// Left column: the badge on its ball, then the three counters.
		float wobble = 1.0f + IconWobble * MathF.Sin(idle * MathF.Tau / WobblePeriod);
		Vector4 art = new(1.0f, 1.0f, 1.0f, _a);
		Place(_ball, BadgeX, BadgeY, BallSize, BallSize);
		Ui.SetImageColor(_ball, art);
		Place(_badge, BadgeX, BadgeY, BadgeW * wobble, BadgeH * wobble);
		Ui.SetImageColor(_badge, art);

		int[] values = { wumpa, lives, 0 }; // crystals: this build has no crystal pickup yet
		for (int i = 0; i < 3; i++)
		{
			float y = PillY + PillStep * i;
			float breathe = 1.0f + IconWobble * MathF.Sin(idle * MathF.Tau / WobblePeriod + 0.6f * (i + 1));
			PlaceBox(_pills[i], 6, PillX, y, PillW, PillH, PillH * 0.5f, BoxShadow, 1.0f);
			Place(_discs[i], DiscX, y, DiscSize, DiscSize);
			Ui.SetImageColor(_discs[i], art);
			Place(_icons[i], DiscX, y, IconSizes[i].W * breathe, IconSizes[i].H * breathe);
			Ui.SetImageColor(_icons[i], art);
			SetCount(i, values[i], y);
		}

		// The gem track: a cyan bar of six sockets, the completion to its right.
		PlaceBox(_strip, 6, StripX, StripY, StripW, StripH, StripH * 0.5f, BoxShadow, 1.0f);
		for (int i = 0; i < _gems.Length; i++)
		{
			float x = GemX0 + GemStep * i;
			PlaceBox(_sockets[i], 6, x, StripY, SocketSize, SocketSize, SocketSize * 0.5f, 0.0f, 1.0f);
			Place(_gems[i], x, StripY, GemSize, GemSize);
			Ui.SetImageColor(_gems[i], art);
		}
		PlaceRun(_zeroPct, ZeroPctX, StripY, ZeroPctScaleX, ZeroPctScaleY, art);

		// The option drum and its selection capsule.
		float pulse = MathF.Sin(idle * MathF.Tau / PulsePeriod);
		PlaceBox(_capsule, 7, RowsX, _hiY, CapsuleW, CapsuleH, CapsuleH * 0.5f, CapsuleGlow, 1.0f);
		for (int i = 0; i < _rows.Length; i++)
		{
			float k = _rowK[i];
			float grow = 1.0f + RowPulse * pulse * k;
			float sx = (RowScaleX + (RowSelectedX - RowScaleX) * k) * grow;
			float sy = (RowScaleY + (RowSelectedY - RowScaleY) * k) * grow;
			float bright = RowIdle + (1.0f - RowIdle) * k;
			Vector4 tint = i == GreyedItem
				? new Vector4(GreyedTint, GreyedAlpha * _a)
				: new Vector4(bright, bright, bright, _a);
			PlaceRun(_rows[i], RowsX, RowsY + RowStep * i, sx, sy, tint);
		}

		// Footer: the prompts pinned to the column margins.
		float shoulderW = RunWidth(_shoulderL, ShoulderScaleX);
		PlaceRun(_shoulderL, FooterLeft, FooterY, ShoulderScaleX, ShoulderScaleY, art, -1);
		PlaceRun(_select, FooterLeft + shoulderW + FooterGap, FooterY, PromptScaleX, PromptScaleY, art, -1);
		float shoulderRW = RunWidth(_shoulderR, ShoulderScaleX);
		PlaceRun(_shoulderR, FooterRight, FooterY, ShoulderScaleX, ShoulderScaleY, art, 1);
		PlaceRun(_back, FooterRight - shoulderRW - FooterGap, FooterY, PromptScaleX, PromptScaleY, art, 1);
	}

	private void SetVisible(bool show)
	{
		foreach (Entity e in _shapes)
		{
			e.SetActive(show);
		}
		// Every glyph of every text run (the rows, "0%" and the prompts), not just each run's key.
		foreach (TextRun run in _runs.Values)
		{
			foreach ((Entity glyph, float _, float _) in run.Glyphs)
			{
				glyph.SetActive(show);
			}
		}
		foreach (Entity g in _digits)
		{
			g.SetActive(false); // SetCount shows the digits it uses
		}
	}

	private bool LoadMetrics()
	{
		string? metrics = Assets.ReadText(FontMetrics);
		if (metrics == null)
		{
			Log.Warn($"[Twinsanity] {FontMetrics} missing - run tw-extract (Startup/) for the pause menu art.");
			return false;
		}
		using (System.Text.Json.JsonDocument doc = System.Text.Json.JsonDocument.Parse(metrics))
		{
			foreach (System.Text.Json.JsonProperty g in doc.RootElement.GetProperty("glyphs").EnumerateObject())
			{
				System.Text.Json.JsonElement v = g.Value;
				_glyphs[(char)int.Parse(g.Name)] = (v[3].GetSingle(), v[4].GetSingle());
			}
		}
		return true;
	}

	private string[] LoadItems()
	{
		string? text = Assets.ReadText(StringsPath);
		if (text == null)
		{
			Log.Warn($"[Twinsanity] {StringsPath} missing - the pause menu falls back to English literals.");
			return new[] { "options", "save game", "load game", "disable autosave", "quit game", "resume" };
		}
		string[] lines = text.Split('\n');
		var items = new string[ItemLines.Length];
		for (int i = 0; i < ItemLines.Length; i++)
		{
			int n = ItemLines[i];
			items[i] = n < lines.Length ? lines[n].TrimEnd('\r') : "";
		}
		return items;
	}
}
