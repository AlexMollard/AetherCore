using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The original's in-game pause menu (N. Sanity Beach). Escape or gamepad Start freezes the
/// game (Time.Scale 0) and plays the menu's opening; Up/Down moves the selection through the
/// option drum, Start/Back/RESUME closes it and the HUD pops back in (TwinsanityLevel calls
/// TwinsanityHud.PopBoth on JustClosed). All menu animation runs on the unscaled clock, as in
/// the original: the game is frozen underneath.
///
/// Art is the disc's own: the option text, counter digits, "0%", SELECT/BACK and the
/// L1/L2/R1/R2 + arrow prompts are Crash_Euro glyphs (ui/fonts, item strings from
/// ui/text/English.txt; the prompt glyph characters are English.txt lines 30-33, kept as
/// literals because the file's high-byte characters do not survive the engine's UTF-8 asset
/// reads). The badge is ui/titles/English/Hub01_00.png, the counter icons are ui/icons. The
/// swirl backdrop, the counter discs, the menu ball and the gem track are drawn by the PS2 game
/// as coloured geometry - no texture for them exists on the disc (tw-extract walked every
/// archive) - so they are analytic SDF shapes in the project's ui_pause.slang UI material,
/// measured on 35 fps rig captures (logs/pause/rig/).
///
/// Not faithful, stated plainly: OPTIONS, SAVE GAME, LOAD GAME and QUIT GAME have no target in
/// this single-level build, so selecting them closes the menu exactly like RESUME (the
/// original opens sub-screens); DISABLE AUTOSAVE is drawn greyed and Up/Down skips it, as in
/// the original. Menu sounds are not reproduced (none extracted yet). Glyph rows are placed on
/// the bowed arc the rig shows, but the original also tilts each glyph along the arc; our
/// upright glyphs from the disc's font already match the rig rows closely.
/// </summary>
public sealed class TwinsanityPause
{
	private const string FontDir = "project://assets/ui/fonts/Crash_Euro/";
	private const string FontMetrics = "project://assets/ui/fonts/Crash_Euro.font.json";
	private const string IconDir = "project://assets/ui/icons/";
	private const string TitlePath = "project://assets/ui/titles/English/Hub01_00.png";
	private const string StringsPath = "project://assets/ui/text/English.txt";
	private const float FrameHeight = 480.0f;

	// Option drum: English.txt line indexes - 12 'options', 27 'save game', 11 'load game',
	// 23 'disable autosave', 28 'quit game', 29 'resume'. Line 3 (DISABLE AUTOSAVE) is greyed
	// out and skipped by Up/Down, exactly as in the original.
	private static readonly int[] ItemLines = { 12, 27, 11, 23, 28, 29 };
	private const int GreyedItem = 3;
	private const int DefaultItem = 5; // the menu opens on RESUME (rig: the idle pulse is on RESUME)

	// The bottom prompts: Crash_Euro button glyphs - '\' cross, '^' triangle, '<' '>' arrows,
	// '{' '¦' L1 L2, '}' '¬' R1 R2 (English.txt lines 30-33).
	private const string SelectRun = "select \\";
	private const string BackRun = "^ back";
	private const string ShoulderLeft = "< { \u00A6";   // '< { ¦'
	private const string ShoulderRight = "} \u00AC >";  // '} ¬ >'

	// Measured on the rig's idle menu frame (logs/pause/rig/idle/t00334.png, 640x480 PAL) with
	// absolute-labelled 3x crops (logs/pause/q_*.png) and colour scan lines (see ui_pause.slang):
	// element centres/boxes and glyph run scales from ink widths against the font's texel widths
	// ('options' 124 px of 128 texels).
	// Transitions (logs/pause/rig/open and close, 35 fps): the swirl, badge, ball and text grow out
	// of the centre from ~0.2 scale while the counter discs slide in from the left edge and the gem
	// track from the right, ~0.2 s; closing plays it backwards in ~0.15 s (rig close: art from 412 ms
	// to gone at 560 ms), while the prompts and the world dim fade more slowly and are clear at
	// ~0.26 s (the world is back to full brightness at 671 ms).
	private const float OpenTime = 0.2f, CloseArtTime = 0.15f, CloseTime = 0.26f;
	private const float MinScale = 0.2f, SlideOut = 220.0f;
	// While the menu is up the whole PS2 frame reads darker: the world drops to ~24% (dim) and the
	// menu's own textured art peaks at 191 of 255 (the pre-open frame peaks at 248). The disc textures
	// are exported at full brightness, so they are drawn tinted to match; the code-drawn shapes
	// already use the rig's on-screen colours.
	private static readonly Vector4 DiscLit = new(0.35f, 0.64f, 0.73f, 1.0f);   // rig 89,163,187
	private static readonly Vector4 DiscShade = new(0.06f, 0.28f, 0.73f, 1.0f); // rig 16,72,187
	private static readonly Vector4 BallLit = new(0.25f, 0.58f, 0.72f, 1.0f);   // rig 64,149,184
	private static readonly Vector4 BallShade = new(0.06f, 0.36f, 0.56f, 1.0f); // rig 16,91,144
	private const float DimAlpha = 0.76f;
	private const float MenuBright = 0.75f;
	private const float GroupCenterX = 320.0f, GroupCenterY = 240.0f;
	// Swirl box: the outer ellipse (238,185) about (318,210) plus its white rim; track box: the
	// arc band about the gem circle between -88 and +74 degrees (ui_pause.slang holds the shapes).
	private const float SwirlX = 318.0f, SwirlY = 210.0f, SwirlW = 484.0f, SwirlH = 378.0f;
	private const float TrackX = 509.0f, TrackY = 201.0f, TrackW = 188.0f, TrackH = 342.0f;
	// Gem slots: least-squares circle through the six rig gem centres (residuals <= 5 px).
	private const float GemCx = 428.9f, GemCy = 204.2f, GemR = 146.0f, GemAngle = -72.0f, GemStep = 26.0f, GemSize = 44.0f;
	private const float BadgeX = 290.0f, BadgeY = 174.0f, BadgeW = 180.0f, BadgeH = 184.0f;
	private const float BallX = 320.0f, BallY = 368.0f, BallSize = 166.0f;
	private const float ZeroPctX = 454.0f, ZeroPctY = 271.0f, ZeroPctScaleX = 1.35f, ZeroPctScaleY = 0.9f;
	private const float RowsX = 320.0f, RowsY = 302.0f, RowStep = 24.5f;
	private const float RowScaleX = 0.97f, RowScaleY = 0.65f, RowSelectedX = 1.09f, RowSelectedY = 0.78f;
	private const float RowPulse = 0.03f, PulsePeriod = 0.65f;
	private const float RowBow = 3.5f, RowBowResume = 6.0f;
	private const float GreyedTintR = 0.28f, GreyedTintG = 0.16f, GreyedTintB = 0.07f; // dark brown, rig row 4
	private const float PromptScaleX = 0.9f, PromptScaleY = 0.55f, ShoulderScaleX = 1.08f, ShoulderScaleY = 0.9f;
	private const float SelectX = 105.0f, BackX = 550.0f, PromptY = 451.0f;
	private const float ShoulderLX = 104.0f, ShoulderRX = 536.0f, ShoulderY = 424.0f;
	private const float DigitScaleX = 1.35f, DigitScaleY = 1.0f;
	private const float IconWobble = 0.03f, WobblePeriod = 1.4f; // badge/icons breathe while idle

	// Counter discs, icons and read-outs, top to bottom: wumpa, Crash head (lives), crystals. The
	// count is right-aligned at CountRight, left of its icon; the icons are drawn stretched as the
	// PS2 draws them (the crystal cluster is 50x129 px on screen from a 64x53 texel tile).
	private static readonly (float X, float Y, float Size)[] Discs = { (113.0f, 103.0f, 98.0f), (91.0f, 216.0f, 98.0f), (148.0f, 318.0f, 100.0f) };
	private static readonly (float X, float Y, float W, float H)[] Icons = { (131.0f, 97.0f, 56.0f, 70.0f), (96.0f, 212.0f, 58.0f, 75.0f), (157.0f, 314.0f, 50.0f, 156.0f) };
	private static readonly (float Right, float Y)[] Counts = { (97.5f, 103.0f), (62.0f, 215.0f), (125.0f, 317.0f) };

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
	private Entity _swirl;
	private Entity _track;
	private readonly Entity[] _gems = new Entity[6];
	private Entity _badge;
	private Entity _ball;
	private Entity _zeroPct;
	private readonly Entity[] _discs = new Entity[3];
	private readonly Entity[] _icons = new Entity[3];
	private readonly Entity[] _rows = new Entity[6];
	// Each counter owns three digit images, re-textured when its value changes (as the HUD does).
	private readonly Entity[,] _digits = new Entity[3, 3];
	private readonly int[] _shownCounts = { -1, -1, -1 };
	private Entity _select, _back, _shoulderL, _shoulderR;

	// Per-frame placement state.
	private float _s = 1.0f;  // screen scale: frame px -> screen px
	private float _ox;        // screen x of the 640x480 frame's left edge
	private float _k = 1.0f;  // menu-open group scale about the group centre
	private float _a = 1.0f;  // master alpha
	private float _slide;         // how far the side groups sit off screen (frame px)
	private float _dimAlpha;      // world dim, fades on its own (slower) curve when closing
	private float _promptAlpha;   // the bottom prompts, likewise
	private Group _group;         // which transition group Place() is laying out

	// Transition groups: the centre art scales about the frame centre, the counter discs slide
	// off the left edge, the gem track off the right; the prompts only fade.
	private enum Group { Centre, Left, Right, Fixed }

	private sealed class TextRun
	{
		public Entity First = default;
		public readonly List<(Entity Glyph, float Advance, float Width)> Glyphs = new();
		public float Total;
		public Vector4 Tint = new(MenuBright, MenuBright, MenuBright, 1.0f);
	}

	/// <summary>True on the frame the menu finished closing: TwinsanityLevel pops the HUD back in.</summary>
	public bool JustClosed => _justClosed;

	/// <summary>Per frame, from TwinsanityLevel.OnUpdate. The level's scaled delta freezes with the
	/// game, so timing here is taken from the unscaled clock.</summary>
	public void Update(int wumpa, int lives)
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
					_selected = DefaultItem;
					_phase = Phase.Opening;
					_phaseTime = 0.0f;
					Time.Pause(); // gameplay freezes; this menu keeps animating unscaled
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
				_justClosed = true;
				return;
		}
		Layout(wumpa, lives);
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
				_selected = i;
				return;
			}
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
		// does the same); the dim draws first, so everything else lands above it.
		_frame = Ui.CreateImage(_canvas);
		Ui.SetAnchors(_frame, Vector2.Zero, Vector2.One);
		Ui.SetOffsets(_frame, Vector2.Zero, Vector2.Zero);
		Ui.SetImageColor(_frame, Vector4.Zero);
		_dim = Ui.CreateImage(_canvas);
		Ui.SetAnchors(_dim, Vector2.Zero, Vector2.One);
		Ui.SetOffsets(_dim, Vector2.Zero, Vector2.Zero);
		Ui.SetImageColor(_dim, new Vector4(0.0f, 0.0f, 0.0f, DimAlpha));

		_swirl = ArtShape(0, SwirlW, SwirlH);
		_track = ArtShape(3, TrackW, TrackH);
		for (int i = 0; i < _gems.Length; i++)
		{
			_gems[i] = Icon(IconDir + "Icons_06.png", GemSize, GemSize);
		}
		_badge = Icon(TitlePath, BadgeW, BadgeH);
		_ball = ArtShape(2, BallSize, BallSize);
		Ui.SetMaterialColors(_ball, BallLit, BallShade);

		// Counter discs: wumpa, Crash head (lives), crystals - disc, icon and count each.
		string[] iconFiles = { "Icons_13.png", "Icons_00.png", "Icons_15.png" };
		for (int i = 0; i < _discs.Length; i++)
		{
			_discs[i] = ArtShape(1, Discs[i].Size, Discs[i].Size);
			_icons[i] = Icon(IconDir + iconFiles[i], Icons[i].W, Icons[i].H);
		}
		for (int i = 0; i < _discs.Length; i++)
		{
			for (int d = 0; d < 3; d++)
			{
				Entity e = Ui.CreateImage(_canvas);
				Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
				Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
				_digits[i, d] = e;
			}
		}

		_zeroPct = TextImage("0%");
		string[] items = LoadItems();
		for (int i = 0; i < _rows.Length; i++)
		{
			_rows[i] = TextImage(items[i]);
			if (i == GreyedItem)
			{
				_runs[_rows[i]].Tint = new Vector4(GreyedTintR, GreyedTintG, GreyedTintB, 1.0f);
			}
		}
		_select = TextImage(SelectRun);
		_back = TextImage(BackRun);
		_shoulderL = TextImage(ShoulderLeft);
		_shoulderR = TextImage(ShoulderRight);

		SetVisible(false);
		return true;
	}

	// A pause-geometry element: a plain image with the ui_pause UI material, one shape per
	// params.y (see ui_pause.slang for the measurements).
	private Entity ArtShape(int shape, float w, float h)
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetImageColor(e, Vector4.One);
		Ui.SetMaterial(e, "ui_pause");
		Ui.SetMaterialParams(e, new Vector4(0.0f, shape, 0.0f, 0.0f));
		// Disc fill: lit edge (top left) to shaded edge, sampled on the rig frames (the ball's
		// own pair is set after).
		Ui.SetMaterialColors(e, DiscLit, DiscShade);
		Ui.SetRect(e, 0.0f, 0.0f, w, h);
		return e;
	}

	private Entity Icon(string texture, float w, float h)
	{
		Entity e = Ui.CreateImage(_canvas);
		Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetImageColor(e, Vector4.One);
		Ui.SetImageTexture(e, texture);
		Ui.SetRect(e, 0.0f, 0.0f, w, h);
		return e;
	}

	// One image per glyph, pivoted at its centre. The run is keyed on its first glyph entity;
	// a run is always a real glyph followed by whatever else the string carries.
	private Entity TextImage(string text, Vector4? tint = null)
	{
		var run = new TextRun();
		if (tint is Vector4 given)
		{
			run.Tint = given;
		}
		foreach (char c in text)
		{
			if (c != ' ' && _glyphs.TryGetValue(c, out var g) && g.W > 0.0f)
			{
				Entity e = Ui.CreateImage(_canvas);
				Ui.SetAnchors(e, Vector2.Zero, Vector2.Zero);
				Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
				Ui.SetImageColor(e, run.Tint);
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
	private void SetCount(int index, int value)
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
		float xEnd = Counts[index].Right;
		for (int d = 2; d >= 0; d--) // right to left: image d shows text[d]
		{
			Entity e = _digits[index, d];
			bool used = d < text.Length;
			e.SetActive(used && _phase != Phase.Closed);
			if (!used)
			{
				continue;
			}
			float w = _glyphs[text[d]].W;
			Place(e, xEnd - w * DigitScaleX * 0.5f, Counts[index].Y, w * DigitScaleX, 39.0f * DigitScaleY);
			Ui.SetImageColor(e, Tint());
			xEnd -= w * DigitScaleX;
		}
	}

	// Centre-place an element in 640x480 frame units through its transition group, then to screen px.
	private void Place(Entity e, float x, float y, float w, float h)
	{
		float k = _group == Group.Centre ? _k : 1.0f;
		float dx = _group == Group.Left ? -_slide : _group == Group.Right ? _slide : 0.0f;
		x = GroupCenterX + (x - GroupCenterX) * k + dx;
		y = GroupCenterY + (y - GroupCenterY) * k;
		Ui.SetRect(e, _ox + x * _s, y * _s, w * k * _s, h * k * _s);
	}

	// The disc textures' tint: the rig's menu brightness, faded with the menu.
	private Vector4 Tint() => new(MenuBright, MenuBright, MenuBright, _a);

	// Place a glyph run centred at (cx, cy), scaled (sx, sy), bowed: each glyph sits on the
	// downward arc the rig rows show (dy = 4*bow*u*(1-u), y down).
	private void PlaceRun(Entity key, float cx, float cy, float sx, float sy, float bow, float alphaScale)
	{
		TextRun run = _runs[key];
		Vector4 tint = run.Tint;
		tint.W *= _group == Group.Fixed ? alphaScale : _a * alphaScale;
		float startX = cx - run.Total * sx * 0.5f;
		foreach ((Entity glyph, float advance, float width) in run.Glyphs)
		{
			float u = advance / Math.Max(run.Total, 1.0f);
			float dy = 4.0f * bow * u * (1.0f - u);
			Place(glyph, startX + advance * sx, cy + dy, width * sx, 39.0f * sy);
			Ui.SetImageColor(glyph, tint);
		}
	}

	private void Layout(int wumpa, int lives)
	{
		Vector4 screen = Ui.GetRect(_frame);
		_s = screen.W / FrameHeight;
		// The original composes in a 4:3 640x480 frame; a wider viewport centres it.
		_ox = (screen.Z - 640.0f * _s) * 0.5f;
		if (_s <= 0.0f)
		{
			return;
		}

		float idle = 0.0f;
		float p; // 0 = off (tiny / slid away / transparent), 1 = in place
		switch (_phase)
		{
			case Phase.Opening:
			{
				float t = Math.Min(_phaseTime / OpenTime, 1.0f);
				p = 1.0f - (1.0f - t) * (1.0f - t); // ease out
				_dimAlpha = DimAlpha * p;
				_promptAlpha = Math.Clamp((_phaseTime - OpenTime * 0.5f) / (OpenTime * 0.5f), 0.0f, 1.0f);
				break;
			}
			case Phase.Closing:
			{
				float t = Math.Min(_phaseTime / CloseArtTime, 1.0f);
				p = 1.0f - t * t; // ease in: holds, then goes
				float slow = 1.0f - Math.Min(_phaseTime / CloseTime, 1.0f);
				_dimAlpha = DimAlpha * slow;
				_promptAlpha = slow;
				break;
			}
			default:
				p = 1.0f;
				_dimAlpha = DimAlpha;
				_promptAlpha = 1.0f;
				idle = _phaseTime;
				break;
		}
		_k = MinScale + (1.0f - MinScale) * p;
		_a = p;
		_slide = SlideOut * (1.0f - p);

		Ui.SetImageColor(_dim, new Vector4(0.0f, 0.0f, 0.0f, _dimAlpha));
		_group = Group.Centre;
		Vector4 shapeAlpha = new(1.0f, 1.0f, 1.0f, _a); // the shader keeps its own colours
		Place(_swirl, SwirlX, SwirlY, SwirlW, SwirlH);
		Ui.SetImageColor(_swirl, shapeAlpha);
		_group = Group.Right;
		Place(_track, TrackX, TrackY, TrackW, TrackH);
		Ui.SetImageColor(_track, shapeAlpha);
		for (int i = 0; i < _gems.Length; i++)
		{
			double ang = (GemAngle + GemStep * i) * Math.PI / 180.0;
			Place(_gems[i], GemCx + GemR * (float)Math.Cos(ang), GemCy + GemR * (float)Math.Sin(ang), GemSize, GemSize);
			Ui.SetImageColor(_gems[i], Tint());
		}
		_group = Group.Centre;
		// The badge and the counter icons breathe gently while the menu idles (the rig's idle
		// diff moves exactly these), each on its own phase.
		float[] phases = { 0.5f, 1.1f, 1.7f };
		float wobble = 1.0f + IconWobble * (float)Math.Sin(idle * 2.0 * Math.PI / WobblePeriod);
		Place(_badge, BadgeX, BadgeY, BadgeW * wobble, BadgeH * wobble);
		Ui.SetImageColor(_badge, Tint());
		Place(_ball, BallX, BallY, BallSize, BallSize);
		Ui.SetImageColor(_ball, shapeAlpha);

		_group = Group.Left;
		int[] values = { wumpa, lives, 0 }; // crystals: this build has no crystal pickup yet
		for (int i = 0; i < _discs.Length; i++)
		{
			float breathe = 1.0f + IconWobble * (float)Math.Sin(idle * 2.0 * Math.PI / WobblePeriod + phases[i]);
			Place(_discs[i], Discs[i].X, Discs[i].Y, Discs[i].Size, Discs[i].Size);
			Ui.SetImageColor(_discs[i], shapeAlpha);
			Place(_icons[i], Icons[i].X, Icons[i].Y, Icons[i].W * breathe, Icons[i].H * breathe);
			Ui.SetImageColor(_icons[i], Tint());
			SetCount(i, values[i]);
		}

		_group = Group.Centre;
		PlaceRun(_zeroPct, ZeroPctX, ZeroPctY, ZeroPctScaleX, ZeroPctScaleY, 0.0f, 1.0f);

		// The option drum: the selected row grows (rig: 'options' ink 124 -> 139 px wide) and
		// pulses gently; the greyed row keeps its fixed dark brown.
		float pulse = 1.0f + RowPulse * (float)Math.Sin(idle * 2.0 * Math.PI / PulsePeriod);
		for (int i = 0; i < _rows.Length; i++)
		{
			float sx = i == _selected ? RowSelectedX * pulse : RowScaleX;
			float sy = i == _selected ? RowSelectedY * pulse : RowScaleY;
			float bow = i == DefaultItem ? RowBowResume : RowBow;
			PlaceRun(_rows[i], RowsX, RowsY + RowStep * i, sx, sy, bow, 1.0f);
		}

		_group = Group.Fixed;
		PlaceRun(_shoulderL, ShoulderLX, ShoulderY, ShoulderScaleX, ShoulderScaleY, 0.0f, _promptAlpha);
		PlaceRun(_shoulderR, ShoulderRX, ShoulderY, ShoulderScaleX, ShoulderScaleY, 0.0f, _promptAlpha);
		PlaceRun(_select, SelectX, PromptY, PromptScaleX, PromptScaleY, 0.0f, _promptAlpha);
		PlaceRun(_back, BackX, PromptY, PromptScaleX, PromptScaleY, 0.0f, _promptAlpha);
	}

	private void SetVisible(bool show)
	{
		_dim.SetActive(show);
		_swirl.SetActive(show);
		_track.SetActive(show);
		foreach (Entity g in _gems)
		{
			g.SetActive(show);
		}
		_badge.SetActive(show);
		_ball.SetActive(show);
		foreach (Entity e in _discs)
		{
			e.SetActive(show);
		}
		foreach (Entity e in _icons)
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
