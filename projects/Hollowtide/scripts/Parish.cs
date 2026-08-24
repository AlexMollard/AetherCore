using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The parish: the place behind the interface. A shader draws the world - sky, skyline, fog
/// and a flagged floor in perspective - and the rites stand on it with their bearers walking
/// between them.
/// </summary>
/// <remarks>
/// <para>
/// <b>Why this is UI and not the scene.</b> The ground used to be a world sprite lit by the
/// 2D light map, and the light map washed it to the palest thing on screen whatever it was
/// tinted - a flat quad standing in for a surface. Drawing it analytically fixes that, but a
/// background UI effect composites over the scene, so anything left in the world would be
/// hidden behind it. The rites came with it. They lose nothing by the move: they were always
/// flat and always at fixed positions, and the glow they had from submitted lights is a
/// better glow now that a shader is drawing it.
/// </para>
/// <para>
/// Everything is positioned in NAVE FRACTIONS against the shader's horizon, which is the one
/// number both sides have to agree on - see <see cref="Layout.Horizon"/>. Fractions rather than
/// pixels so the parish reflows with the window instead of baking a resolution.
/// </para>
/// </remarks>
public sealed class Parish
{
	private const int kPopPool = 12;
	private const int kBearerPool = 40;
	/// <summary>How fast a bearer travels, in fractions of the backdrop per second. Faster than
	/// the old straight walk because the conduit route is about twice as long; the time from a
	/// rite finishing to the yield landing is roughly what it was.</summary>
	private const float kBearerSpeed = 0.34f;

	/// <summary>How far the widest lane of the pop fan carries, as a fraction of the backdrop
	/// width. Wide enough that two figures beside each other are legible, narrow enough that a
	/// gather still reads as coming from the sigil rather than from somewhere near it.</summary>
	private const float kPopFan = 0.045f;

	/// <summary>Pips drawn per conduit. Enough to read as a continuous line at the sizes the
	/// nave is actually drawn, few enough that eight of them cost nothing.</summary>
	private const int kWirePips = 18;

	private struct Pop
	{
		public Entity Label;
		public Vector2 From;
		public Vector4 Colour;
		public float Age;
		/// <summary>Sideways travel per unit of rise, in backdrop fractions. Zero would be
		/// straight up, which is what put every figure in one column.</summary>
		public float Drift;
		/// <summary>How hard this one climbs. Varying it separates two pops that share a lane
		/// as well, so the fan does not have to be the only thing keeping them apart.</summary>
		public float Lift;
		/// <summary>How much this one is worth keeping. A gather is 0 and everything else is
		/// above it, so the thing a keeper actually wants to see cannot be recycled away by the
		/// ordinary clicking that produced it.</summary>
		public int Worth;
	}

	private struct Bearer
	{
		public Entity Body;
		/// <summary>Which rite sent it. The rite, not the point it left from: a structure grows
		/// as the keeper buys, and the canopy lifts with it, so a captured start would leave a
		/// bead travelling a route its own wire had already moved off.</summary>
		public int Rite;
		public Vector2 To;
		public double Amount;
		public float Age;
		/// <summary>A small per-bearer speed multiplier. The conduit is a fixed line, so two
		/// yields leaving the same rite a moment apart would otherwise sit exactly on top of
		/// each other for the whole run; varying the pace separates them without pulling either
		/// of them off the wire.</summary>
		public float Pace;
		public bool Live;
	}

	private Entity _nave;
	private Entity _backdrop;
	private readonly Entity[] _rites = new Entity[Content.RiteCount];
	private readonly Entity[] _counts = new Entity[Content.RiteCount];
	private readonly float[] _flare = new float[Content.RiteCount];
	private readonly Bearer[] _bearers = new Bearer[kBearerPool];
	/// <summary>The conduits themselves, one run of pips per rite, raised with the rite.</summary>
	private readonly Entity[,] _wire = new Entity[Content.RiteCount, kWirePips];
	/// <summary>Where each standing rite's conduit leaves it: the top of its count, in backdrop
	/// fractions. Recorded while the rites are dressed, because the canopy has to clear the
	/// tallest of them and none of their heights are known until they have all been sized.</summary>
	private readonly Vector2[] _crown = new Vector2[Content.RiteCount];
	/// <summary>The height every conduit arches at this frame. See Layout.CanopyY.</summary>
	private float _canopy = Layout.Horizon - 0.2f;
	private readonly Pop[] _pops = new Pop[kPopPool];
	private int _nextBearer;
	/// <summary>Which lane of the fan the next pop takes. A counter, not a die roll - see
	/// <see cref="ShowPop"/>.</summary>
	private int _nextLane;

	private float _shake;
	/// <summary>
	/// The parish's own clock, in seconds, kept as a double and handed to shaders wrapped.
	/// </summary>
	/// <remarks>
	/// It was a float, and a float accumulating a frame delta STOPS at about 524,300 seconds -
	/// six days of continuous running - because by then its own spacing is wider than a
	/// sixtieth of a second and the addition rounds to nothing. Every animation the parish
	/// drives would simply stop, permanently, in a game whose whole premise is being left
	/// running. A double is exact for longer than anybody will ever leave it on.
	/// </remarks>
	private double _time;

	/// <summary>What the shaders get: the clock wrapped into an hour, so the value handed
	/// across is always small enough for a float to resolve finely. Everything drawn from it is
	/// periodic, so the wrap costs one discontinuity an hour in drifting fog and grain -
	/// against a clock that otherwise freezes solid on the sixth day.</summary>
	private float ShaderTime => (float)(_time % kClockWrap);

	/// <summary>How long the wrap is. Any animation slower than this would visibly restart, so
	/// nothing driven by the clock should have a period near it.</summary>
	private const double kClockWrap = 3600.0;

	/// <summary>Per-rite purchase impulse: the structure punches in scale and flares.</summary>
	private readonly float[] _punch = new float[Content.RiteCount];

	/// <summary>Per-rite arrival: 0 the frame a rite is first owned, 1 once it has been
	/// raised into place. A rite that simply popped into existence full-size was the single
	/// clearest sign that the parish was a readout rather than a place.</summary>
	private readonly float[] _appear = new float[Content.RiteCount];

	/// <summary>How many fronts can be crossing the floor at once. Three, because there was one
	/// and a keeper buying quickly could not tell anything had happened - the second purchase
	/// overwrote the first's progress, so a front halfway across snapped back to the horizon and
	/// set off again, and one purchase looked exactly like five. The ceiling is the shader's:
	/// three packed slots is all the room the effect's parameters have.</summary>
	private const int kWavePool = 3;

	/// <summary>Each wave: 0 idle, otherwise 0 -> 1 as a front of light rolls out across the
	/// floor from whatever was just bought. Every purchase gets one, so spending always does
	/// something you can see even when what you bought has no body in the parish.</summary>
	private readonly float[] _wave = new float[kWavePool];
	/// <summary>Where each front set off from, as a fraction of the width.</summary>
	private readonly float[] _waveFrom = new float[kWavePool];

	/// <summary>Eased 0..1 while a visitation's aftermath runs, so the parish sags into it and
	/// comes back rather than snapping between two looks.</summary>
	private float _reel;

	/// <summary>
	/// 0 the moment something is named, 1 as it arrives, and 0 again the instant it resolves.
	/// </summary>
	/// <remarks>
	/// The encounter used to happen entirely inside a UI panel: for nine seconds a visitor was
	/// walking toward the keeper and the world behind the banner carried on exactly as before -
	/// bearers strolling, lanterns steady, nothing amiss. The best moment in the game was a
	/// rectangle of text laid over an unbothered parish.
	/// </remarks>
	private float _walking;

	/// <summary>Which rite called the thing currently walking, or -1.</summary>
	private int _calling = -1;

	private Entity _sigil;

	/// <summary>
	/// Where deliveries are carried to: the centre of the sigil, in backdrop fractions.
	/// </summary>
	/// <remarks>
	/// Read off the sigil's live rect rather than written down, so the destination is wherever
	/// the sigil actually is. Bearers used to walk to a bare patch of floor, which left the
	/// obvious question of what they were walking towards unanswered - carrying the yield into
	/// the thing that counts it is the whole point of the walk, so it should be visible.
	/// </remarks>
	private Vector2 Collection
	{
		get
		{
			Vector4 bd = Backdrop;
			Vector4 sg = Ui.GetRect(_sigil);
			if (bd.Z < kMinFrame || sg.Z < kMinFrame)
			{
				return new Vector2(0.5f, 0.45f);
			}
			return new Vector2((sg.X + sg.Z * 0.5f - bd.X) / bd.Z, (sg.Y + sg.W * 0.5f - bd.Y) / bd.W);
		}
	}

	/// <summary>Where the conduits arrive. See Layout.Intake.</summary>
	private Vector2 Intake
	{
		get
		{
			Vector2 centre = Collection;
			Vector4 bd = Backdrop;
			if (bd.Z < kMinFrame || bd.W < kMinFrame || SigilRest < kMinFrame)
			{
				return centre;
			}
			return Layout.Intake(centre, SigilRest / bd.Z, SigilRest / bd.W);
		}
	}

	public void Bind()
	{
		_nave = Scene.Find("HudNave");
		_backdrop = Scene.Find("ParishBackdrop");
		_sigil = Scene.Find("NaveSigil");

		// Bearers and rites are created here rather than authored: there is one per rite the
		// keeper actually owns, and forty bearers is a pool sized to traffic, neither of which
		// a scene file can know.
		// The conduits, pooled up front and left inactive. Created BEFORE the bearers on
		// purpose: within one parent, later elements draw over earlier ones, and a three-pixel
		// pip laid over the bead travelling along it reads as a notch punched in the bead.
		// Raising them lazily with each rite would have put them on the wrong side of that.
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			for (int i = 0; i < kWirePips; i++)
			{
				Entity pip = UiKit.Image(_nave, -500.0f, -500.0f, 3.0f, 3.0f, Palette.Ichor);
				pip.SetActive(false);
				_wire[rite, i] = pip;
			}
		}

		for (int i = 0; i < kBearerPool; i++)
		{
			// A rounded rect at full radius IS a circle, so the wisp needs no texture. The
			// sprite it used to carry was a near-black outline ring around a few green texels,
			// and at the size these are actually drawn that outline swallowed the core - every
			// bearer read as a black blob crossing the floor.
			Entity body = UiKit.Image(_nave, -500.0f, -500.0f, 14.0f, 14.0f, Palette.Ichor);
			Ui.SetImageCornerRadius(body, 7.0f);
			_bearers[i] = new Bearer { Body = body, Live = false };
		}
		for (int i = 0; i < kPopPool; i++)
		{
			Entity label = UiKit.Text(_nave, "", -500.0f, -500.0f, Typography.Box(164.0f),
				Typography.Box(24.0f), Typography.Face(18.0f), Palette.Ichor,
				UiHAlign.Center);
			_pops[i] = new Pop { Label = label, Age = 99.0f, Colour = Palette.Ichor };
		}
	}

	/// <summary>The nave's pixel rect: the frame these entities are laid out in.</summary>
	private Vector4 Nave => Ui.GetRect(_nave);

	/// <summary>The backdrop's pixel rect: the frame the SHADER is drawing in, and so the frame
	/// every fraction in this file is measured against.</summary>
	private Vector4 Backdrop => Ui.GetRect(_backdrop);

	/// <summary>
	/// The smallest rect this file will do arithmetic against, in pixels.
	/// </summary>
	/// <remarks>
	/// Guarding on "width greater than zero" is not enough. Layout resolves over a frame, and a
	/// resize or a swapchain recreate can hand back a rect that is a pixel or two wide before it
	/// settles. Every fraction here is divided by that width, so a rect of 1 turned
	/// <see cref="RiteRight"/> into a huge number, which turned the rite spacing into a huge
	/// number, which sized a rite off the screen - a structure stretched across the interface for
	/// exactly one frame. The more rites are standing the more chances there are to land on it,
	/// which is why it looked like a renderer fault that got worse with the parish.
	/// </remarks>
	private const float kMinFrame = 32.0f;

	/// <summary>Whether both frames are resolved enough to position anything against.</summary>
	private bool Laid
	{
		get
		{
			Vector4 bd = Backdrop;
			Vector4 nave = Nave;
			return bd.Z >= kMinFrame && bd.W >= kMinFrame && nave.Z >= kMinFrame && nave.W >= kMinFrame;
		}
	}

	/// <summary>
	/// A fraction of the backdrop to a pixel offset inside the nave.
	/// </summary>
	/// <remarks>
	/// The two rects are not the same - the backdrop covers the whole canvas and the nave is a
	/// panel inside it - so measuring the horizon against the nave put every rite's feet a long
	/// way below the line the shader had drawn. Going through the backdrop and subtracting the
	/// nave's origin is what makes <see cref="Layout.Horizon"/> mean the same thing on both sides.
	/// </remarks>
	private Vector2 ToLocal(Vector2 f)
	{
		Vector4 bd = Backdrop;
		Vector4 nave = Nave;
		return new Vector2(bd.X + bd.Z * f.X - nave.X, bd.Y + bd.W * f.Y - nave.Y);
	}

	/// <summary>Right end of the stretch the rites stand along. See Layout.RiteRight - the
	/// arithmetic is there so the harness can hold it; this only supplies the measurements.</summary>
	private float RiteRight
	{
		get
		{
			Vector4 bd = Backdrop;
			Vector4 sg = Ui.GetRect(_sigil);
			if (bd.Z < kMinFrame || sg.Z < kMinFrame)
			{
				return 0.34f;
			}
			float centre = (sg.X + sg.Z * 0.5f - bd.X) / bd.Z;
			return Layout.RiteRight(centre, SigilRest / bd.Z,
				Layout.RiteWidthCap(bd.Z, bd.W) * 0.5f);
		}
	}

	/// <summary>How many rites are standing. The band is shared between these and not between
	/// all eight - budgeting for a parish the keeper will not have for hours left the early
	/// ones as specks in slots sized for someone else.</summary>
	private int _standing = 1;

	/// <summary>The smallest sigil width seen. See <see cref="MeasureSigil"/>.</summary>
	private float _sigilRest = float.MaxValue;

	/// <summary>The sigil's unstruck width in pixels, or its live width if it has not been
	/// measured yet - which only happens before the first frame that laid out.</summary>
	private float SigilRest => _sigilRest == float.MaxValue ? Ui.GetRect(_sigil).Z : _sigilRest;

	/// <summary>
	/// Take the sigil's resting size, once per frame, before anything reads it.
	/// </summary>
	/// <remarks>
	/// <para>
	/// Self-calibrating rather than a constant: a punch only ever makes the sigil BIGGER, so the
	/// smallest width ever seen is its unstruck size at whatever window the keeper is playing
	/// at. A fixed fraction would have been right at exactly one window width.
	/// </para>
	/// <para>
	/// This used to happen as a side effect of reading <c>RiteRight</c>, which meant everything
	/// else that needs the resting size - the rite spacing, and now the point the conduits
	/// arrive at - was quietly depending on the rite layout having been computed first. It held
	/// only because <c>SyncRites</c> happened to run before the rest; a yield delivered on a
	/// frame before the first layout took the fallback instead, putting one bead off its own
	/// wire. A measurement that everything reads belongs in one place at the top of the frame,
	/// not in the getter of whichever caller happened to need it first.
	/// </para>
	/// </remarks>
	private void MeasureSigil()
	{
		Vector4 sg = Ui.GetRect(_sigil);
		if (sg.Z >= kMinFrame)
		{
			_sigilRest = MathF.Min(_sigilRest, sg.Z);
		}
	}

	/// <summary>Gap between neighbouring rites, in backdrop fractions. Every rite is capped to
	/// this wide, which is what stops a heavily-bought rite from swallowing the one beside it.</summary>
	private float RiteSpacing => Layout.RiteSpacing(_standing, RiteRight);

	/// <summary>Where a rite stands, in backdrop fractions.</summary>
	private float RiteX(int rite) => Layout.RiteX(rite, _standing, RiteRight);

	public void Update(float deltaTime, float unscaledDelta)
	{
		MeasureSigil();
		_time += unscaledDelta;
		_shake = MathF.Max(0.0f, _shake - unscaledDelta * 1.7f);
		for (int i = 0; i < kWavePool; i++)
		{
			if (_wave[i] <= 0.0f)
			{
				continue;
			}
			_wave[i] = MathF.Min(1.0f, _wave[i] + unscaledDelta * 1.15f);
			if (_wave[i] >= 1.0f)
			{
				_wave[i] = 0.0f;
			}
		}
		for (int i = 0; i < _punch.Length; i++)
		{
			_punch[i] = MathF.Max(0.0f, _punch[i] - unscaledDelta * 2.4f);
		}
		for (int i = 0; i < _flare.Length; i++)
		{
			_flare[i] = MathF.Max(0.0f, _flare[i] - unscaledDelta * 2.2f);
		}

		// ── Something is walking ─────────────────────────────────────────────────────
		// Read off the simulation rather than pushed in through a callback, exactly as the
		// dread and the aftermath below already are: the parish is a VIEW, and a view that has
		// to be told things can be told them late or not at all.
		_calling = Vigil.Approaching ? Vigil.ApproachRite : -1;
		_walking = Vigil.Approaching
			? Math.Clamp(1.0f - (float)(Vigil.ApproachSeconds / Math.Max(0.001, Vigil.ApproachTotal)), 0.0f, 1.0f)
			: 0.0f;
		if (Vigil.Approaching)
		{
			// A tremor that GROWS rather than a hit that fades - this is the sound of something
			// coming, not the moment it lands, and the two should not feel alike. Folded in with
			// Max so it rides over the decay above without fighting the impact shake.
			_shake = MathF.Max(_shake, 0.10f + _walking * 0.45f);
		}

		float dread = (float)Vigil.Dread;
		// The parish is visibly subdued while it is reeling. A cost the player cannot see is
		// not a cost they can weigh, and the aftermath is the whole price of a visitation now.
		float reeling = Vigil.AftermathSeconds > 0.0 ? 1.0f : 0.0f;
		_reel += (reeling - _reel) * MathF.Min(1.0f, unscaledDelta * 3.0f);
		// The approach drives the same channel as dread, so the parish sours toward its worst
		// while something is on its way in whatever the meter happens to read. The shake still
		// passes through DreadShake, so a keeper who turned the unsteadiness down keeps it down.
		Ui.SetEffectParams(_backdrop,
			new Vector4(ShaderTime, MathF.Max(dread, MathF.Max(_reel * 0.75f, _walking)),
				_shake * _shake * SaveSystem.DreadShake, PackWave(0)));
		// The two ALPHAS carry the other two waves. Params holds four numbers and three of them
		// are spoken for, while neither colour's alpha is ever read as a colour - so the spare
		// waves ride there. Documented on both sides rather than silently smuggled: see the
		// same note in ui_parish.slang.
		// Reeling reads as the dread souring without any of the dread payoff - the parish
		// looks like it has been got at, which is exactly what has happened to it.
		Ui.SetEffectColors(_backdrop,
			Palette.Fade(Palette.Ink, PackWave(1)),
			Palette.Fade(Palette.Mix(Palette.Dread, Palette.DreadDeep, _reel), PackWave(2)));

		SyncRites(dread, unscaledDelta);
		UpdateBearers(unscaledDelta);
		UpdatePops(unscaledDelta);
	}

	/// <summary>Create, retire and dress one element per rite the keeper holds.</summary>
	private void SyncRites(float dread, float unscaledDelta)
	{
		if (!Laid)
		{
			return;
		}
		Vector4 bd = Backdrop;

		_standing = 0;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			if (Vigil.Owned[rite] > 0)
			{
				_standing++;
			}
		}

		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			int owned = Vigil.Owned[rite];
			if (owned <= 0)
			{
				if (_rites[rite].IsValid)
				{
					_rites[rite].SetActive(false);
					_counts[rite].SetActive(false);
				}
				HideWire(rite);
				// Reset, so a communion's next parish is raised again rather than snapping in.
				_appear[rite] = 0.0f;
				continue;
			}

			if (!_rites[rite].IsValid)
			{
				// A material, not a sprite. Drawn analytically for the same reason the floor
				// is: a 48x48 hand-drawn image has no light on it, no contact with the ground
				// and no resolution beyond its own, so however crisply it was sampled it still
				// read as a sticker laid over a lit scene.
				// Created INK, not white. ui_rite takes only the alpha from the element's colour
				// and computes its own rgb, so the fill is invisible while the material is
				// drawing - but on any frame the material is not yet resolved (the creation
				// frame, and again after a shader hot-reload) the plain image draws instead, and
				// a white one flashed as a solid quad across the parish. Ink flashes as nothing.
				_rites[rite] = UiKit.Image(_nave, 0.0f, 0.0f, 10.0f, 10.0f, Palette.Ink);
				Ui.SetMaterial(_rites[rite], "ui_rite");
				// Params before the first draw, so a rite never renders as rite 0 for a frame.
				Ui.SetMaterialParams(_rites[rite], new Vector4(ShaderTime, rite, 0.0f, 0.0f));
				Ui.SetMaterialColors(_rites[rite], Palette.RiteTint(rite), Palette.Ichor);
				_counts[rite] = UiKit.Text(_nave, "", 0.0f, 0.0f, Typography.Box(90.0f),
					Typography.Box(21.0f), Typography.Face(15.0f), Palette.TextDim,
					UiHAlign.Center);
			}
			_rites[rite].SetActive(true);
			_counts[rite].SetActive(true);
			_appear[rite] = MathF.Min(1.0f, _appear[rite] + unscaledDelta * 2.2f);

			// Grows on a log curve and stands ON the horizon: its base is pinned there and its
			// top rises, so buying more is a bigger thing in the same place.
			float bulk = 0.42f + (float)Math.Log10(owned + 1.0) * 0.30f;
			float working = (float)Vigil.CycleProgress[rite];
			float flare = _flare[rite] * _flare[rite];
			if (_walking > 0.0f && rite == _calling)
			{
				// The rite that called it, lit and pulsing. This is the only place the game
				// shows WHICH holding summoned the thing now walking - the visitor-to-rite link
				// the bestiary asks the player to learn, taught by the parish instead of by a
				// table. Max rather than assignment so a working that finishes mid-approach can
				// still flare over the top of it.
				flare = MathF.Max(flare, (0.45f + 0.35f * MathF.Sin(ShaderTime * 11.0f)) * _walking);
			}
			// Raised into place with an overshoot the first time, and punched on every
			// purchase after that: the reward for spending is that the parish moves.
			float a = _appear[rite];
			// Never exactly zero. The curve is 1 - cos(0)*(1-0) = 0 at a = 0, so every rite spent
			// its first frame as a zero-area quad carrying a material - a degenerate rect whose
			// derivatives are meaningless, handed to a shader that antialiases off fwidth. A
			// floor costs nothing and means the element is always a real rectangle.
			float raise = a >= 1.0f ? 1.0f : MathF.Max(0.06f, 1.0f - MathF.Cos(a * 1.9f) * (1.0f - a));
			float punch = _punch[rite] * _punch[rite];
			float size = bd.W * 0.20f * bulk * raise
				* (1.0f + working * 0.05f + flare * 0.16f + punch * 0.20f);
			// Capped to the gap between neighbours. Growth is the reward for buying, but a rite
			// that has outgrown its slot reads as a collision, not as progress - so past this
			// point it stops widening and the count under it carries the rest of the story.
			size = MathF.Min(size, bd.Z * RiteSpacing * 0.92f);
			// A last clamp against the backdrop itself. Every term above is derived from live
			// rects, and one screen-filling quad is worse than every rite being a little small.
			//
			// If the clamp ever actually BITES, say so. The stretched-material artifact this was
			// added to chase turned out to be a producer/GPU race on the UI command buffer, not
			// this arithmetic, and it is fixed in the renderer. The report stays because it costs
			// nothing and it is what separated the two the first time: if a rite ever looks wrong
			// again, silence here puts the fault below this script.
			// Capped by the room above the horizon: the structure, its count and the canopy the
			// conduits run along all have to fit in there. There was a second cap here for a
			// while, squeezing the rites nearest the sigil to buy the conduits room to descend
			// through - unnecessary once the descent was anchored to the sigil's own column.
			float tallest = Layout.RiteHeightCap(bd.W) * bd.W;
			if (size > tallest || size < 4.0f)
			{
				Log.Warn("Parish: rite " + rite + " sized " + size.ToString("F0")
					+ "px against backdrop " + bd.Z.ToString("F0") + "x" + bd.W.ToString("F0")
					+ " nave " + Nave.Z.ToString("F0") + "x" + Nave.W.ToString("F0")
					+ " standing " + _standing + " spacing " + RiteSpacing.ToString("F3")
					+ " riteRight " + RiteRight.ToString("F3"));
			}
			// Capped by the room above the horizon, not by a flat fraction of the window: the
			// structure, its count and the canopy the conduits arch along all have to fit in
			// there. See Layout.RiteHeightCap.
			size = Math.Clamp(size, 4.0f, tallest);

			// Art is drawn on a square canvas but its subject stands in the middle of it, so
			// the sprite is sunk slightly to put the SUBJECT'S feet on the line, not the box's.
			Vector2 foot = ToLocal(new Vector2(RiteX(rite), Layout.Horizon));
			Ui.SetAnchors(_rites[rite], Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(_rites[rite], new Vector2(0.5f, 1.0f));
			// Pivot is bottom-centre, so the foot IS the y - subtracting the height as well
			// lifted every rite by its own size and turned the row into a diagonal.
			Ui.SetRect(_rites[rite], foot.X, foot.Y, size, size);

			// Structure stays on the ramp; the flare is the only thing that brightens it, and
			// only for as long as the rite is handing something over.
			Ui.SetMaterialParams(_rites[rite], new Vector4(ShaderTime, rite, working, flare));
			Ui.SetMaterialColors(_rites[rite],
				Palette.Mix(Palette.RiteTint(rite), Palette.Dread, dread * 0.30f),
				Palette.Mix(Palette.Ichor, Palette.Dread, dread * 0.45f));

			// Above each rite rather than in a row on the floor. A row was tidier, but the floor
			// just below the horizon is where the buttons are, and a count sitting on a control
			// is worse than a count at an uneven height.
			Ui.SetAnchors(_counts[rite], Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(_counts[rite], new Vector2(0.5f, 1.0f));
			Ui.SetRect(_counts[rite], foot.X, foot.Y - size - 8.0f,
				Typography.Box(90.0f), Typography.Box(21.0f));
			Ui.SetText(_counts[rite], "x" + owned);

			// Where this rite's conduit leaves it: above its count, not at its foot. The wire is
			// tapped off the top of the structure, so it must start clear of the number sitting
			// there or the first pip lands on a digit.
			_crown[rite] = new Vector2(RiteX(rite), Layout.CrownY(size / bd.W, bd.W));
		}

		// The canopy, once every crown is known, and then the conduits under it. Two passes
		// rather than one: the arch has to clear the TALLEST thing standing, and on the frame a
		// keeper buys their eighth rite that is not known until the last one has been sized.
		Vector2 intake = Intake;
		float highest = intake.Y;
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			if (Vigil.Owned[rite] > 0)
			{
				highest = MathF.Min(highest, _crown[rite].Y);
			}
		}
		_canopy = Layout.CanopyY(highest);

		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			// The conduit this rite sends its yield down, drawn whether anything is on it or not.
			// A wire that only appears when a bead is travelling is just a longer bead: what
			// makes the yield read as CARRIED rather than as floating is that the route was
			// visibly there beforehand.
			if (Vigil.Owned[rite] > 0)
			{
				DressWire(rite, _crown[rite], intake, bd);
			}
		}
	}

	/// <summary>
	/// Lay out the conduit pips for one rite.
	/// </summary>
	/// <remarks>
	/// Positioned every frame rather than once, because the rites reflow with the window and
	/// with the size of the parish - a conduit laid once would still point at wherever its rite
	/// stood when the eighth was bought. Cheap: eighteen rects against a parish that is already
	/// moving forty bearers.
	/// </remarks>
	private void DressWire(int rite, Vector2 from, Vector2 to, Vector4 bd)
	{
		float radius = Layout.CornerRadius(from, to, _canopy, RiteSpacing);
		for (int i = 0; i < kWirePips; i++)
		{
			Entity pip = _wire[rite, i];
			if (!pip.IsValid)
			{
				continue;
			}
			pip.SetActive(true);
			// Skips t=0: the first pip would sit on the rite's crown, where it reads as a smudge
			// on the structure rather than the start of a line.
			float t = (i + 1.0f) / (kWirePips + 1.0f);
			Vector2 px = ToLocal(Layout.Conduit(from, to, _canopy, radius, t));
			float size = Math.Clamp(bd.W * 0.004f, 1.0f, 6.0f);
			Ui.SetAnchors(pip, Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(pip, new Vector2(0.5f, 0.5f));
			Ui.SetRect(pip, px.X, px.Y, size, size);
			Ui.SetImageCornerRadius(pip, size * 0.5f);
			// Barely there at rest, and lit by the rite it serves as that rite works. The line
			// is infrastructure; the bead is the light. Anything brighter and eight conduits
			// would out-shout the eight structures that own them.
			Ui.SetImageColor(pip, Palette.Fade(Palette.Ichor, 0.10f + _flare[rite] * 0.30f));
		}
	}

	/// <summary>Put a rite's conduit away with the rite.</summary>
	private void HideWire(int rite)
	{
		for (int i = 0; i < kWirePips; i++)
		{
			if (_wire[rite, i].IsValid)
			{
				_wire[rite, i].SetActive(false);
			}
		}
	}

	/// <summary>
	/// Send a front of light out across the floor from <paramref name="from"/>.
	/// </summary>
	/// <remarks>
	/// Takes an idle slot if there is one, and otherwise the front that is nearest finished - so
	/// a burst of purchases each get their own wave, and the only one ever cut short is the one
	/// closest to being over anyway.
	/// </remarks>
	private void FireWave(float from)
	{
		int chosen = 0;
		float furthest = -1.0f;
		for (int i = 0; i < kWavePool; i++)
		{
			if (_wave[i] <= 0.0f)
			{
				chosen = i;
				break;
			}
			if (_wave[i] > furthest)
			{
				furthest = _wave[i];
				chosen = i;
			}
		}
		_wave[chosen] = 0.0001f;
		_waveFrom[chosen] = from;
	}

	/// <summary>
	/// One wave's origin and progress squeezed into a single float in [0,1).
	/// </summary>
	/// <remarks>
	/// The origin quantised to 255 steps in the high part, the progress in the low. Kept inside
	/// [0,1) rather than spread over a wider range because these ride in colour alphas, and a
	/// colour channel is not a place to assume nothing will ever clamp. Zero means idle, which a
	/// live wave cannot collide with: it starts at a progress of 0.0001, never at 0.
	///
	/// Unpacked by Unwave in ui_parish.slang. One format in two places - change either and you
	/// have changed both.
	/// </remarks>
	private float PackWave(int slot)
	{
		float progress = _wave[slot];
		if (progress <= 0.0f)
		{
			return 0.0f;
		}
		float origin = MathF.Floor(Math.Clamp(_waveFrom[slot], 0.0f, 1.0f) * 255.0f);
		return (origin + MathF.Min(progress, 0.999f)) / 256.0f;
	}

	/// <summary>A rite finished a working: flare it, and send a bearer with the yield.</summary>
	public void Delivered(int rite, double amount)
	{
		if (rite < 0 || rite >= Content.RiteCount)
		{
			return;
		}
		_flare[rite] = 1.0f;

		Bearer b = _bearers[_nextBearer];
		b.Rite = rite;
		b.Amount = amount;
		b.Age = 0.0f;
		b.To = Intake;
		b.Pace = AetherCore.Random.Range(0.88f, 1.14f);
		b.Live = true;
		_bearers[_nextBearer] = b;
		_nextBearer = (_nextBearer + 1) % kBearerPool;
	}

	/// <summary>
	/// The keeper spent on something. <paramref name="rite"/> is which rite, or -1 for an
	/// offering, a mark or a communion.
	/// </summary>
	/// <remarks>
	/// Every purchase sends a front of light out across the floor, so buying anything at all
	/// does something in the parish - offerings, marks and communions previously took the
	/// money and changed nothing visible, which made most of the shop feel inert. A rite also
	/// punches and flares where it stands, so the thing you actually bought is the thing the
	/// eye is pulled to.
	/// </remarks>
	public void Bought(int rite)
	{
		FireWave(rite >= 0 && rite < Content.RiteCount ? RiteX(rite) : Collection.X);
		if (rite >= 0 && rite < Content.RiteCount)
		{
			_punch[rite] = 1.0f;
			_flare[rite] = 1.0f;
		}
	}

	/// <summary>The dark arrived. <paramref name="held"/> is whether a ward stood in the way -
	/// the two read completely differently, because they mean opposite things.</summary>
	public void Visitation(bool held)
	{
		if (held)
		{
			_shake = 0.35f;
			FireWave(Collection.X);
			ShowPop(Collection + new Vector2(0.0f, -0.05f), "WARDED", Palette.Sigil);
			return;
		}
		_shake = 1.0f;
		ShowPop(Collection + new Vector2(0.0f, -0.05f), "TAKEN", Palette.Dread);
	}

	/// <summary>A gather by hand, at the point the keeper struck.</summary>
	public void Gathered(double amount, Vector2 screenPoint)
	{
		if (!Laid)
		{
			return;
		}
		Vector4 bd = Backdrop;
		Vector2 f = new Vector2((screenPoint.X - bd.X) / bd.Z, (screenPoint.Y - bd.Y) / bd.W);
		ShowPop(f, "+" + Numbers.Short(amount), Palette.Ichor);
	}

	/// <summary>Draw every live bearer along its conduit, over the parish and into the sigil.</summary>
	private void UpdateBearers(float unscaledDelta)
	{
		if (!Laid)
		{
			return;
		}
		Vector4 bd = Backdrop;

		for (int i = 0; i < kBearerPool; i++)
		{
			Bearer b = _bearers[i];
			if (!b.Live)
			{
				continue;
			}
			b.Age += unscaledDelta * b.Pace;
			// The rite's CURRENT crown, so the bead stays on the wire even if the structure has
			// grown or the canopy lifted since it set off.
			Vector2 from = _crown[b.Rite];
			float radius = Layout.CornerRadius(from, b.To, _canopy, RiteSpacing);
			float total = Layout.ConduitLength(from, b.To, _canopy, radius) / kBearerSpeed;

			// A bead drawn along the conduit, eased at both ends so it sets off and arrives
			// rather than snapping into a constant slide. No bow and no gait: it is not walking,
			// it is being drawn down a line, and a bounce on a fixed wire reads as a fault.
			float w = Math.Clamp(b.Age / total, 0.0f, 1.0f);
			float e = w * w * (3.0f - 2.0f * w);
			Vector2 at = Layout.Conduit(from, b.To, _canopy, radius, e);

			Vector2 px = ToLocal(at);
			// Holds its size along the run and only narrows over the last stretch, as the sigil
			// takes it in. The old shrink began at once and ran the whole way, which was right
			// for a figure walking away toward the horizon and wrong for a bead on a wire: the
			// route is level, so nothing about it is getting further off.
			float scale = 1.0f - MathF.Max(0.0f, e - 0.72f) / 0.28f * 0.6f;
			// Small. As an outlined sprite this could afford to be forty pixels across; as a
			// solid disc of the one colour in the game that means anything, that size made
			// every wisp louder than the rite that sent it.
			float size = Math.Clamp(bd.W * 0.0125f * Math.Clamp(scale, 0.5f, 1.4f), 2.0f, bd.W * 0.05f);
			Ui.SetAnchors(b.Body, Vector2.Zero, Vector2.Zero);
			// Centre-pivoted, so the bead sits ON the wire. Bottom-pivoted it hung below it by
			// half its own height, which at these sizes is the whole width of the line.
			Ui.SetPivot(b.Body, new Vector2(0.5f, 0.5f));
			Ui.SetRect(b.Body, px.X, px.Y, size, size);
			Ui.SetImageCornerRadius(b.Body, size * 0.5f);

			float fade = MathF.Min(1.0f, MathF.Min(b.Age * 6.0f, (1.0f - w) * 5.0f));
			Ui.SetImageColor(b.Body, Palette.Fade(Palette.Ichor, Math.Clamp(fade, 0.0f, 1.0f) * 0.85f));

			if (b.Age >= total)
			{
				b.Live = false;
				Ui.SetRect(b.Body, -500.0f, -500.0f, 1.0f, 1.0f);
				// The payout lands where the sigil took it in, which is the point of the run. No
				// jitter of its own: the fan in ShowPop separates these properly, and a random
				// nudge on top of it only blurred which lane a figure was in. No offset either -
				// the intake is already above the sigil now, so lifting it further put the
				// figure out among the conduits it had just arrived down.
				ShowPop(b.To,
					"+" + Numbers.Short(b.Amount), Palette.Ichor);
			}
			_bearers[i] = b;
		}
	}

	/// <summary>
	/// The parish gives something up.
	/// </summary>
	/// <remarks>
	/// The payoff for clicking, and it was presented by nothing: relics arrived in a panel the
	/// keeper might not have open, with a whisper line only for the better half of them. The
	/// whole reason the feature exists is to make hand-gathering worth doing at hour ten, and a
	/// reward you have to go and look for is not a reward you feel.
	///
	/// Named rather than numbered, because a relic is not a quantity - and the better it is the
	/// louder it lands, so a Hollowed thing shakes the parish and Leavings do not.
	/// </remarks>
	public void Found(Relic relic, Vector2 screenPoint)
	{
		if (!Laid || !relic.Exists)
		{
			return;
		}
		Vector4 bd = Backdrop;
		Vector2 f = new Vector2((screenPoint.X - bd.X) / bd.Z, (screenPoint.Y - bd.Y) / bd.W);
		// Grade carried by how much ichor the text takes, exactly as the ledger shows it, so
		// the same thing means the same thing in both places.
		float weight = (int)relic.Grade / 4.0f;
		ShowPop(f, Relics.GradeName(relic.Grade).ToUpperInvariant(),
			Palette.Mix(Palette.TextDim, Palette.Ichor, 0.35f + weight * 0.65f),
			worth: 1 + (int)relic.Grade);
		if (relic.Grade >= Grade.Hallowed)
		{
			_shake = MathF.Max(_shake, 0.20f + weight * 0.25f);
		}
	}

	/// <summary>
	/// Throw a popup, taking the slot that will be missed least.
	/// </summary>
	/// <remarks>
	/// Twelve slots, a second and a half each: round-robin, a popup survives until twelve more
	/// are thrown, so anything above about eight a second starts evicting popups that are still
	/// on screen. That rate used to be unreachable - and then relics made hand-gathering worth
	/// hammering, which means the keepers most likely to turn something up are exactly the ones
	/// clicking fast enough to wipe it out before they read it.
	///
	/// So a slot is chosen rather than taken in turn: a dead one first, then the least
	/// important, and only then the oldest. A gather is worth nothing to keep - there is
	/// another one coming in a quarter of a second.
	/// </remarks>
	private void ShowPop(Vector2 at, string text, Vector4 colour, int worth = 0)
	{
		int chosen = -1;
		int worstWorth = int.MaxValue;
		float oldest = -1.0f;
		for (int i = 0; i < kPopPool; i++)
		{
			Pop candidate = _pops[i];
			// Aged out: free, take it and stop looking.
			if (candidate.Age > 1.5f)
			{
				chosen = i;
				break;
			}
			// Otherwise remember the least worth keeping, oldest first among equals.
			if (candidate.Worth < worstWorth || (candidate.Worth == worstWorth && candidate.Age > oldest))
			{
				worstWorth = candidate.Worth;
				oldest = candidate.Age;
				chosen = i;
			}
		}

		// Everything alive is worth more than what is being thrown: let it be. A find on screen
		// beats a gather figure that will be replaced before anybody looks at it.
		if (chosen < 0 || (worstWorth > worth && _pops[chosen].Age <= 1.5f))
		{
			return;
		}

		// A fan, not a jitter.
		//
		// Every gather originates at exactly the same place - the keeper strikes the sigil and
		// the sigil does not move - so a keeper clicking quickly sent a dozen figures up one
		// column, each hiding the one before it. Bearer arrivals now do the same, since they all
		// land on the one intake.
		//
		// Lanes are handed out by a counter rather than by chance, because chance still drops two
		// consecutive pops into the same place often enough to be noticed, and it is precisely
		// the consecutive ones that need to be told apart. Six lanes, alternating sides and
		// widening: at the pool's twelve slots and a second and a half each, that is enough for
		// anyone clicking as fast as a hand can.
		int step = _nextLane % 6;
		_nextLane = (_nextLane + 1) % 6;
		float side = step % 2 == 0 ? 1.0f : -1.0f;
		float spread = (step / 2 + 1) / 3.0f;

		Pop pop = _pops[chosen];
		pop.Drift = side * spread * kPopFan;
		pop.Lift = 0.85f + spread * 0.35f;
		pop.From = at;
		pop.Colour = colour;
		pop.Age = 0.0f;
		pop.Worth = worth;
		Ui.SetText(pop.Label, text);
		_pops[chosen] = pop;
	}

	/// <summary>Float the live popups up and fade them out, on unscaled time so they still
	/// read while the game is frozen behind a menu.</summary>
	private void UpdatePops(float unscaledDelta)
	{
		if (!Laid)
		{
			return;
		}
		Vector4 bd = Backdrop;
		for (int i = 0; i < kPopPool; i++)
		{
			Pop pop = _pops[i];
			if (pop.Age > 1.5f)
			{
				continue;
			}
			pop.Age += unscaledDelta;
			float t = Math.Clamp(pop.Age / 1.5f, 0.0f, 1.0f);
			float rise = 1.0f - (1.0f - t) * (1.0f - t);

			Vector2 px = ToLocal(pop.From);
			// Drift scales with the rise, so every figure leaves from the same point and splays
			// as it climbs. Fanning from the start would have read as figures appearing beside
			// the sigil rather than out of it.
			Ui.SetAnchors(pop.Label, Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(pop.Label, new Vector2(0.5f, 0.5f));
			Ui.SetRect(pop.Label, px.X + rise * pop.Drift * bd.Z,
				px.Y - rise * bd.W * 0.05f * pop.Lift, Typography.Box(164.0f), Typography.Box(24.0f));
			Ui.SetTextColor(pop.Label, Palette.Fade(pop.Colour, 1.0f - t));
			if (t >= 1.0f)
			{
				Ui.SetText(pop.Label, "");
			}
			_pops[i] = pop;
		}
	}
}
