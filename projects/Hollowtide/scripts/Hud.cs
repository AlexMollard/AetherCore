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
	public const float BarHeight = 104.0f;

	private const float kSigilSize = 268.0f;

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
	private Button _stoke;
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
	public bool StokePressed { get; private set; }

	/// <summary>Where the last gather landed, in SCREEN pixels: the pointer when the sigil was
	/// clicked, and the sigil itself when it was the space bar. Feedback is thrown from here,
	/// so it appears where the keeper actually struck.</summary>
	public Vector2 StrikePoint { get; private set; }

	/// <summary>Bind the authored chrome. Every element here lives in the scene, so this only
	/// looks things up - laying the bar out is a job for the editor, not for a rebuild.</summary>
	public void Bind()
	{
		_bar = Scene.Find("HudBar");
		_title = Scene.Find("HudTitle");
		_keeper = Scene.Find("HudKeeper");
		_ichor = Scene.Find("HudIchor");
		_rate = Scene.Find("HudRate");
		_multiplier = Scene.Find("HudMultiplier");
		_dreadTrack = Scene.Find("HudDreadBar");
		_dreadLabel = Scene.Find("HudDreadValue");
		_sigilCount = Scene.Find("HudSigils");

		_nave = Scene.Find("HudNave");
		_sigil = Scene.Find("NaveSigil");
		_sigilHint = Scene.Find("NaveHint");
		_handValue = Scene.Find("NaveHandValue");
		_stoke = Button.Find("NaveStoke");
		_ward = Button.Find("NaveWard");
		_bell = Button.Find("NaveBell");
	}

	/// <summary>Read input, animate, and write every live number. Called once a frame.</summary>
	public void Update(float deltaTime, bool connected, int keeperCount)
	{
		GatheredThisFrame = false;
		WardPressed = false;
		BellPressed = false;
		StokePressed = false;

		// ── Input ────────────────────────────────────────────────────────────────────
		// WasActivated covers the click, Enter/Space-while-focused and the pad button, and
		// consumes the key so it cannot also reach the shortcut below on the same frame.
		if (_sigil.IsValid && Ui.WasActivated(_sigil))
		{
			GatheredThisFrame = true;
			// The pointer if it is over the sigil, the sigil's middle if the activation came
			// from a key or a pad - either way, a place the player was looking at.
			Vector4 rect = Ui.GetRect(_sigil);
			StrikePoint = Ui.IsHovered(_sigil)
				? Input.MousePosition
				: new Vector2(rect.X + rect.Z * 0.5f, rect.Y + rect.W * 0.5f);
		}
		else if (Input.IsKeyPressed(Key.Space) && !Ui.HasFocus)
		{
			Vector4 rect = Ui.GetRect(_sigil);
			StrikePoint = new Vector2(rect.X + rect.Z * 0.5f, rect.Y + rect.W * 0.5f);
			// Space works with nothing focused, which is the state the game spends most of its
			// time in. Gated on HasFocus so it cannot double-fire with the line above, and so a
			// keeper typing a name into the threshold screen is not also gathering.
			GatheredThisFrame = true;
		}

		if (_stoke.Activated)
		{
			StokePressed = true;
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
		bool canStoke = Vigil.Dread < 0.999;
		_stoke.SetLabel(canStoke ? "STOKE THE DARK" : "IT IS AS CLOSE AS IT GETS");
		_stoke.SetEnabled(canStoke);
		_stoke.Style(canStoke, Palette.Mix(Palette.Row, Palette.Dread, 0.55f), Palette.Row, Palette.PanelDeep);

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
