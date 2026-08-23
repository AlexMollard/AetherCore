using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The two pieces of screen that are always there: the bar across the top that says what
/// the vigil is worth, and the nave under it - the sigil you gather by hand from, the ward,
/// and the bell.
/// </summary>
/// <remarks>
/// Both are laid out inside the screen MINUS the ledger's width, anchored to the edges
/// rather than placed at absolute pixels, so the whole HUD reflows when the window is
/// resized without anything re-measuring or rebuilding.
/// </remarks>
public sealed class Hud
{
	/// <summary>Width the ledger reserves on the right. Everything here stops short of it.</summary>
	public const float LedgerWidth = 620.0f;
	public const float BarHeight = 96.0f;

	private const float kSigilSize = 268.0f;

	private Entity _canvas;
	private Entity _bar;
	private Entity _nave;

	private Entity _title;
	private Entity _keeper;
	private Entity _ichor;
	private Entity _rate;
	private Entity _multiplier;
	private Entity _dreadTrack;
	private Entity _dreadLabel;
	private Entity _sigilCount;

	private Entity _sigil;
	private Entity _sigilHint;
	private Entity _handValue;
	private Button _ward;
	private Button _bell;

	/// <summary>Seconds of punch left on the sigil. Drives its size and its shader, and is the
	/// only reason a click feels like anything.</summary>
	private float _punch;
	private float _bellCooldown;

	public Entity SigilElement => _sigil;

	/// <summary>True on the frame the keeper gathered by hand - by clicking the sigil, by
	/// activating it with a pad, or with the space bar.</summary>
	public bool GatheredThisFrame { get; private set; }

	public bool WardPressed { get; private set; }
	public bool BellPressed { get; private set; }

	public void Build(Entity canvas)
	{
		_canvas = canvas;

		// ── The bar ──────────────────────────────────────────────────────────────────
		_bar = UiKit.Stretch(canvas, new Vector2(0.0f, 0.0f), new Vector2(1.0f, 0.0f),
			new Vector2(0.0f, 0.0f), new Vector2(-LedgerWidth, BarHeight), Palette.PanelDeep);

		_title = UiKit.Text(_bar, "HOLLOWTIDE", 28.0f, 14.0f, 320.0f, 30.0f, 22.0f, Palette.BoneDim,
			UiHAlign.Left, Palette.Display);
		_keeper = UiKit.Text(_bar, "", 28.0f, 48.0f, 340.0f, 26.0f, 17.0f, Palette.BoneDim,
			UiHAlign.Left, Palette.Body);

		_ichor = UiKit.Text(_bar, "0", 320.0f, 12.0f, 300.0f, 46.0f, 40.0f, Palette.Ichor, UiHAlign.Left);
		_rate = UiKit.Text(_bar, "0/s", 322.0f, 56.0f, 300.0f, 24.0f, 18.0f, Palette.IchorDim, UiHAlign.Left);
		_multiplier = UiKit.Text(_bar, "", 322.0f, 56.0f, 300.0f, 24.0f, 17.0f, Palette.Sigil, UiHAlign.Right);

		// The dread meter is an engine UI Progress Bar rather than two images kept in step by
		// hand: the widget already owns the track/fill/rounding, and driving it is one call.
		_dreadTrack = UiKit.Image(_bar, 660.0f, 34.0f, 360.0f, 20.0f, Palette.Transparent, 6.0f);
		ComponentAccess progress = _dreadTrack.Component("UI Progress Bar");
		progress.Add();
		progress.SetVector4("track_color", new Vector4(0.086f, 0.075f, 0.078f, 1.0f));
		progress.SetVector4("fill_color", Palette.Dread);
		progress.SetFloat("corner_radius", 6.0f);
		UiKit.Text(_bar, "DREAD", 660.0f, 12.0f, 120.0f, 20.0f, 15.0f, Palette.BoneFaint, UiHAlign.Left, Palette.Display);
		_dreadLabel = UiKit.Text(_bar, "0.0%", 900.0f, 12.0f, 120.0f, 20.0f, 16.0f, Palette.Dread, UiHAlign.Right);

		_sigilCount = UiKit.Text(_bar, "", -260.0f, 34.0f, 240.0f, 28.0f, 18.0f, Palette.Sigil, UiHAlign.Right);
		// Pinned to the bar's own right edge so it stays put when the window widens.
		Ui.SetAnchors(_sigilCount, new Vector2(1.0f, 0.0f), new Vector2(1.0f, 0.0f));
		Ui.SetPivot(_sigilCount, new Vector2(1.0f, 0.0f));
		Ui.SetRect(_sigilCount, -24.0f, 34.0f, 260.0f, 28.0f);

		// ── The nave ─────────────────────────────────────────────────────────────────
		_nave = UiKit.Stretch(canvas, new Vector2(0.0f, 0.0f), new Vector2(1.0f, 1.0f),
			new Vector2(0.0f, BarHeight), new Vector2(-LedgerWidth, 0.0f), Palette.Transparent);

		_sigil = Ui.CreateImage(_nave);
		_sigil.SetParent(_nave);
		Ui.SetAnchors(_sigil, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
		Ui.SetPivot(_sigil, new Vector2(0.5f, 0.5f));
		Ui.SetRect(_sigil, 0.0f, -60.0f, kSigilSize, kSigilSize);
		Ui.SetImageColor(_sigil, Vector4.One);
		Ui.SetImageCornerRadius(_sigil, 0.0f);
		// Its own fragment shader, masked to the element: the sigil is the one thing on screen
		// that has to look drawn rather than laid out.
		Ui.SetMaterial(_sigil, "ui_sigil");
		Ui.SetSelectable(_sigil);

		_sigilHint = Centered(_nave, "GATHER", 0.0f, 108.0f, 400.0f, 26.0f, 17.0f, Palette.BoneFaint, Palette.Display);
		_handValue = Centered(_nave, "", 0.0f, 136.0f, 400.0f, 26.0f, 17.0f, Palette.IchorDim, Palette.Body);

		_ward = CenteredButton(_nave, "RAISE WARD", -132.0f, 186.0f, 250.0f, 44.0f);
		_bell = CenteredButton(_nave, "RING THE BELL", 132.0f, 186.0f, 250.0f, 44.0f);
	}

	private static Entity Centered(Entity parent, string text, float x, float y, float w, float h,
		float size, Vector4 colour, string font)
	{
		Entity e = UiKit.Text(parent, text, 0.0f, 0.0f, w, h, size, colour, UiHAlign.Center, font);
		Ui.SetAnchors(e, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetRect(e, x, y, w, h);
		return e;
	}

	private static Button CenteredButton(Entity parent, string label, float x, float y, float w, float h)
	{
		Button b = UiKit.MakeButton(parent, label, 0.0f, 0.0f, w, h, 16.0f, Palette.Display);
		Ui.SetAnchors(b.Root, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
		Ui.SetPivot(b.Root, new Vector2(0.5f, 0.5f));
		Ui.SetRect(b.Root, x, y, w, h);
		return b;
	}

	/// <summary>Read input, animate, and write every live number. Called once a frame.</summary>
	public void Update(float deltaTime, bool connected, int keeperCount)
	{
		GatheredThisFrame = false;
		WardPressed = false;
		BellPressed = false;

		// ── Input ────────────────────────────────────────────────────────────────────
		// WasActivated covers the click, Enter/Space-while-focused and the pad button, and
		// consumes the key so it cannot also reach the shortcut below on the same frame.
		if (_sigil.IsValid && Ui.WasActivated(_sigil))
		{
			GatheredThisFrame = true;
		}
		else if (Input.IsKeyPressed(Key.Space) && !Ui.HasFocus)
		{
			// Space works with nothing focused, which is the state the game spends most of its
			// time in. Gated on HasFocus so it cannot double-fire with the line above, and so a
			// keeper typing a name into the threshold screen is not also gathering.
			GatheredThisFrame = true;
		}

		if (_ward.Activated)
		{
			WardPressed = true;
		}
		if (_bell.Activated && _bellCooldown <= 0.0f)
		{
			BellPressed = true;
			_bellCooldown = 25.0f;
		}

		if (GatheredThisFrame)
		{
			_punch = 1.0f;
		}

		// ── Animation ────────────────────────────────────────────────────────────────
		_punch = MathF.Max(0.0f, _punch - deltaTime * 3.4f);
		_bellCooldown = MathF.Max(0.0f, _bellCooldown - deltaTime);

		float dread = (float)Vigil.Dread;
		bool hovered = _sigil.IsValid && Ui.IsHovered(_sigil);
		// A quick snap out and a slow settle back: the size curve is the whole feel of the
		// click, so it is eased rather than linear.
		float eased = _punch * _punch;
		float scale = 1.0f + eased * 0.10f + (hovered ? 0.02f : 0.0f);
		float size = kSigilSize * scale;
		if (_sigil.IsValid)
		{
			Ui.SetRect(_sigil, 0.0f, -60.0f, size, size);
			Ui.SetMaterialParams(_sigil, new Vector4(Time.UnscaledTime, dread, eased,
				Vigil.SurgeSeconds > 0.0 ? 1.0f : 0.0f));
			Ui.SetMaterialColors(_sigil, Palette.Ichor, Palette.Dread);
		}

		// ── Readouts ─────────────────────────────────────────────────────────────────
		Ui.SetText(_ichor, Numbers.Short(Vigil.Ichor));
		Ui.SetText(_rate, Numbers.Rate(Vigil.Rate));
		Ui.SetText(_keeper, Vigil.KeeperName + (connected ? "  -  keeping vigil with " + Math.Max(0, keeperCount - 1) : "  -  alone"));
		Ui.SetText(_handValue, "+" + Numbers.Short(Vigil.HandGain) + " by hand");

		string mult = Numbers.Mult(Vigil.GlobalMultiplier);
		Ui.SetText(_multiplier, Vigil.SurgeSeconds > 0.0
			? mult + "  SURGE " + Numbers.Duration(Vigil.SurgeSeconds)
			: mult);
		Ui.SetTextColor(_multiplier, Vigil.SurgeSeconds > 0.0 ? Palette.Ichor : Palette.Sigil);

		Ui.SetProgress(_dreadTrack, dread);
		Ui.SetText(_dreadLabel, Numbers.Percent(dread));
		// The meter goes from rust to something brighter as it fills, so the last quarter
		// reads as urgent without a second widget to say so.
		_dreadTrack.Component("UI Progress Bar").SetVector4("fill_color",
			Palette.Mix(Palette.DreadDeep, new Vector4(0.94f, 0.42f, 0.30f, 1.0f), MathF.Min(1.0f, dread * 1.15f)));

		Ui.SetText(_sigilCount, Vigil.Sigils + " sigils   " + Vigil.MarksHeld() + "/" + Content.Marks.Length + " marks");

		// ── Buttons ──────────────────────────────────────────────────────────────────
		bool canWard = Vigil.Ichor >= Vigil.WardCost && Vigil.Dread > 0.0;
		_ward.SetLabel("RAISE WARD  " + Numbers.Short(Vigil.WardCost));
		_ward.SetEnabled(canWard);
		_ward.Style(canWard, Palette.Mix(Palette.Row, Palette.Dread, 0.45f), Palette.Row, Palette.PanelDeep);

		_bell.SetActive(connected);
		if (connected)
		{
			bool ready = _bellCooldown <= 0.0f;
			_bell.SetLabel(ready ? "RING THE BELL" : "BELL  " + Numbers.Duration(_bellCooldown));
			_bell.SetEnabled(ready);
			_bell.Style(ready, Palette.Mix(Palette.Row, Palette.Sigil, 0.40f), Palette.Row, Palette.PanelDeep);
		}

		Ui.SetTextColor(_sigilHint, Palette.Mix(Palette.BoneFaint, Palette.Ichor, eased));
		Ui.SetTextColor(_title, Palette.Mix(Palette.BoneDim, Palette.Dread, dread * 0.8f));
	}
}
