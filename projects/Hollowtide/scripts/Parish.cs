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
/// number both sides have to agree on - see <see cref="kHorizon"/>. Fractions rather than
/// pixels so the parish reflows with the window instead of baking a resolution.
/// </para>
/// </remarks>
public sealed class Parish
{
	/// <summary>Height of the horizon as a fraction of the nave, matching the same constant in
	/// ui_parish.slang. The rites stand on it, so the two MUST agree.</summary>
	private const float kHorizon = 0.62f;

	private const int kPopPool = 12;
	private const int kBearerPool = 40;
	/// <summary>How fast a bearer walks, in fractions of the backdrop per second.</summary>
	private const float kBearerSpeed = 0.16f;

	private struct Pop
	{
		public Entity Label;
		public Vector2 From;
		public Vector4 Colour;
		public float Age;
	}

	private struct Bearer
	{
		public Entity Body;
		public Vector2 From;
		public Vector2 To;
		public double Amount;
		public float Age;
		public float Bow;
		public float Bob;
		public bool Live;
	}

	private Entity _nave;
	private Entity _backdrop;
	private readonly Entity[] _rites = new Entity[Content.RiteCount];
	private readonly Entity[] _counts = new Entity[Content.RiteCount];
	private readonly float[] _flare = new float[Content.RiteCount];
	private readonly Bearer[] _bearers = new Bearer[kBearerPool];
	private readonly Pop[] _pops = new Pop[kPopPool];
	private int _nextBearer;
	private int _nextPop;

	private float _shake;
	private float _time;

	/// <summary>Per-rite purchase impulse: the structure punches in scale and flares.</summary>
	private readonly float[] _punch = new float[Content.RiteCount];

	/// <summary>Per-rite arrival: 0 the frame a rite is first owned, 1 once it has been
	/// raised into place. A rite that simply popped into existence full-size was the single
	/// clearest sign that the parish was a readout rather than a place.</summary>
	private readonly float[] _appear = new float[Content.RiteCount];

	/// <summary>The wave: 0 idle, otherwise 0 -> 1 as a front of light rolls out across the
	/// floor from whatever was just bought. Every purchase gets one, so spending always does
	/// something you can see even when what you bought has no body in the parish.</summary>
	private float _wave;
	private float _waveFrom = 0.5f;

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
			if (bd.Z <= 0.0f || sg.Z <= 0.0f)
			{
				return new Vector2(0.5f, 0.45f);
			}
			return new Vector2((sg.X + sg.Z * 0.5f - bd.X) / bd.Z, (sg.Y + sg.W * 0.5f - bd.Y) / bd.W);
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
		for (int i = 0; i < kBearerPool; i++)
		{
			Entity body = UiKit.Image(_nave, -500.0f, -500.0f, 14.0f, 14.0f, Palette.Transparent);
			Ui.SetImageTexture(body, "project://assets/textures/rites/bearer.png");
			Ui.SetImagePixelArt(body, true);
			_bearers[i] = new Bearer { Body = body, Live = false };
		}
		for (int i = 0; i < kPopPool; i++)
		{
			Entity label = UiKit.Text(_nave, "", -500.0f, -500.0f, 180.0f, 24.0f, 18.0f, Palette.Ichor,
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
	/// A fraction of the backdrop to a pixel offset inside the nave.
	/// </summary>
	/// <remarks>
	/// The two rects are not the same - the backdrop covers the whole canvas and the nave is a
	/// panel inside it - so measuring the horizon against the nave put every rite's feet a long
	/// way below the line the shader had drawn. Going through the backdrop and subtracting the
	/// nave's origin is what makes <see cref="kHorizon"/> mean the same thing on both sides.
	/// </remarks>
	private Vector2 ToLocal(Vector2 f)
	{
		Vector4 bd = Backdrop;
		Vector4 nave = Nave;
		return new Vector2(bd.X + bd.Z * f.X - nave.X, bd.Y + bd.W * f.Y - nave.Y);
	}

	/// <summary>Left end of the stretch of horizon the rites stand along, as a backdrop fraction.</summary>
	private const float kRiteLeft = 0.035f;

	/// <summary>
	/// Right end of that stretch: just clear of the sigil's left edge.
	/// </summary>
	/// <remarks>
	/// Measured off the sigil rather than written down as a constant. The sigil is centred in
	/// the nave and the nave is not centred in the canvas, so any number picked by hand here is
	/// only correct at one window size - and the one I picked put the deepest rite directly on
	/// top of it.
	/// </remarks>
	private float RiteRight
	{
		get
		{
			Vector4 bd = Backdrop;
			Vector4 sg = Ui.GetRect(_sigil);
			if (bd.Z <= 0.0f || sg.Z <= 0.0f)
			{
				return 0.34f;
			}
			return MathF.Max(kRiteLeft + 0.05f, (sg.X - bd.X) / bd.Z - 0.03f);
		}
	}

	/// <summary>How many rites are standing. The band is shared between these and not between
	/// all eight - budgeting for a parish the keeper will not have for hours left the early
	/// ones as specks in slots sized for someone else.</summary>
	private int _standing = 1;

	/// <summary>Gap between neighbouring rites, in backdrop fractions. Every rite is capped to
	/// this wide, which is what stops a heavily-bought rite from swallowing the one beside it.</summary>
	private float RiteSpacing => _standing > 1 ? (RiteRight - kRiteLeft) / (_standing - 1) : 0.14f;

	/// <summary>Where a rite stands, in backdrop fractions: evenly along the horizon, deepest
	/// furthest in. Centred when there are too few to fill the band.</summary>
	private float RiteX(int rite)
	{
		float span = (_standing - 1) * RiteSpacing;
		float left = kRiteLeft + ((RiteRight - kRiteLeft) - span) * 0.5f;
		return left + rite * RiteSpacing;
	}

	public void Update(float deltaTime, float unscaledDelta)
	{
		_time += unscaledDelta;
		_shake = MathF.Max(0.0f, _shake - unscaledDelta * 1.7f);
		if (_wave > 0.0f)
		{
			_wave = MathF.Min(1.0f, _wave + unscaledDelta * 1.15f);
			if (_wave >= 1.0f)
			{
				_wave = 0.0f;
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

		float dread = (float)Vigil.Dread;
		Ui.SetEffectParams(_backdrop,
			new Vector4(_time, dread, _shake * _shake * SaveSystem.DreadShake, _wave));
		// The void's ALPHA carries where the wave started, because params is full and the
		// backdrop only ever reads the void's rgb. Documented on both sides rather than
		// silently smuggled: see the same note in ui_parish.slang.
		Ui.SetEffectColors(_backdrop, Palette.Fade(Palette.Ink, _waveFrom), Palette.Dread);

		SyncRites(dread, unscaledDelta);
		UpdateBearers(unscaledDelta);
		UpdatePops(unscaledDelta);
	}

	/// <summary>Create, retire and dress one element per rite the keeper holds.</summary>
	private void SyncRites(float dread, float unscaledDelta)
	{
		Vector4 bd = Backdrop;
		if (bd.Z <= 0.0f || Nave.Z <= 0.0f)
		{
			return;
		}

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
				Ui.SetMaterialParams(_rites[rite], new Vector4(_time, rite, 0.0f, 0.0f));
				Ui.SetMaterialColors(_rites[rite], Palette.RiteTint(rite), Palette.Ichor);
				_counts[rite] = UiKit.Text(_nave, "", 0.0f, 0.0f, 90.0f, 20.0f, 15.0f, Palette.TextDim,
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
			// Raised into place with an overshoot the first time, and punched on every
			// purchase after that: the reward for spending is that the parish moves.
			float a = _appear[rite];
			float raise = a >= 1.0f ? 1.0f : 1.0f - MathF.Cos(a * 1.9f) * (1.0f - a);
			float punch = _punch[rite] * _punch[rite];
			float size = bd.W * 0.20f * bulk * raise
				* (1.0f + working * 0.05f + flare * 0.16f + punch * 0.20f);
			// Capped to the gap between neighbours. Growth is the reward for buying, but a rite
			// that has outgrown its slot reads as a collision, not as progress - so past this
			// point it stops widening and the count under it carries the rest of the story.
			size = MathF.Min(size, bd.Z * RiteSpacing * 0.92f);

			// Art is drawn on a square canvas but its subject stands in the middle of it, so
			// the sprite is sunk slightly to put the SUBJECT'S feet on the line, not the box's.
			Vector2 foot = ToLocal(new Vector2(RiteX(rite), kHorizon));
			Ui.SetAnchors(_rites[rite], Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(_rites[rite], new Vector2(0.5f, 1.0f));
			// Pivot is bottom-centre, so the foot IS the y - subtracting the height as well
			// lifted every rite by its own size and turned the row into a diagonal.
			Ui.SetRect(_rites[rite], foot.X, foot.Y, size, size);

			// Structure stays on the ramp; the flare is the only thing that brightens it, and
			// only for as long as the rite is handing something over.
			Ui.SetMaterialParams(_rites[rite], new Vector4(_time, rite, working, flare));
			Ui.SetMaterialColors(_rites[rite],
				Palette.Mix(Palette.RiteTint(rite), Palette.Dread, dread * 0.30f),
				Palette.Mix(Palette.Ichor, Palette.Dread, dread * 0.45f));

			// Above each rite rather than in a row on the floor. A row was tidier, but the floor
			// just below the horizon is where the buttons are, and a count sitting on a control
			// is worse than a count at an uneven height.
			Ui.SetAnchors(_counts[rite], Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(_counts[rite], new Vector2(0.5f, 1.0f));
			Ui.SetRect(_counts[rite], foot.X, foot.Y - size - 8.0f, 90.0f, 20.0f);
			Ui.SetText(_counts[rite], "x" + owned);
		}
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
		b.From = new Vector2(RiteX(rite), kHorizon);
		b.Amount = amount;
		b.Age = 0.0f;
		b.To = Collection;
		b.Bow = AetherCore.Random.Range(-0.035f, 0.035f);
		b.Bob = AetherCore.Random.Range(0.0f, 6.28f);
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
		_wave = 0.0001f;
		_waveFrom = rite >= 0 && rite < Content.RiteCount ? RiteX(rite) : Collection.X;
		if (rite >= 0 && rite < Content.RiteCount)
		{
			_punch[rite] = 1.0f;
			_flare[rite] = 1.0f;
		}
	}

	/// <summary>Something arrived. Shake the place and mark where it happened.</summary>
	public void Visitation()
	{
		_shake = 1.0f;
		ShowPop(Collection + new Vector2(0.0f, -0.05f), "TAKEN", Palette.Dread);
	}

	/// <summary>A gather by hand, at the point the keeper struck.</summary>
	public void Gathered(double amount, Vector2 screenPoint)
	{
		Vector4 bd = Backdrop;
		if (bd.Z <= 0.0f)
		{
			return;
		}
		Vector2 f = new Vector2((screenPoint.X - bd.X) / bd.Z, (screenPoint.Y - bd.Y) / bd.W);
		ShowPop(f, "+" + Numbers.Short(amount), Palette.Ichor);
	}

	/// <summary>Walk every live bearer down off its rite and along the floor to the keeper.</summary>
	private void UpdateBearers(float unscaledDelta)
	{
		Vector4 bd = Backdrop;
		if (bd.Z <= 0.0f || Nave.Z <= 0.0f)
		{
			return;
		}

		for (int i = 0; i < kBearerPool; i++)
		{
			Bearer b = _bearers[i];
			if (!b.Live)
			{
				continue;
			}
			b.Age += unscaledDelta;
			float total = (b.To - b.From).Length() / kBearerSpeed;

			// A single walk from the rite's foot toward the keeper, eased at both ends so a
			// bearer sets off and arrives rather than snapping into a constant slide, and bowed
			// out sideways so a dozen of them on the same route do not stack into one line.
			float w = Math.Clamp(b.Age / total, 0.0f, 1.0f);
			float e = w * w * (3.0f - 2.0f * w);
			Vector2 at = b.From + (b.To - b.From) * e;
			// Bowed sideways AND lifted, so the wisp rises off the floor into the sigil rather
			// than sliding up the screen in a straight line.
			at.X += MathF.Sin(e * 3.14159f) * b.Bow;
			at.Y += MathF.Sin(e * 3.14159f) * 0.05f;
			// The gait. Cheap, but a step is the difference between a figure walking and a
			// sprite being interpolated across the floor.
			at.Y -= MathF.Abs(MathF.Sin(b.Age * 8.0f + b.Bob)) * 0.010f;

			Vector2 px = ToLocal(at);
			// Bearers shrink toward the horizon, which the perspective floor makes the eye
			// expect: the same walk further away is a smaller figure.
			// Fades away to nothing as it is drawn into the sigil.
			float scale = 1.0f - e * 0.55f;
			float size = bd.W * 0.030f * Math.Clamp(scale, 0.5f, 1.4f);
			Ui.SetAnchors(b.Body, Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(b.Body, new Vector2(0.5f, 1.0f));
			Ui.SetRect(b.Body, px.X, px.Y, size, size);

			float fade = MathF.Min(1.0f, MathF.Min(b.Age * 6.0f, (1.0f - w) * 5.0f));
			Ui.SetImageColor(b.Body, Palette.Fade(Palette.Ichor, Math.Clamp(fade, 0.0f, 1.0f)));

			if (b.Age >= total)
			{
				b.Live = false;
				Ui.SetRect(b.Body, -500.0f, -500.0f, 1.0f, 1.0f);
				// The payout lands where the keeper is, which is the point of the walk.
				ShowPop(b.To + new Vector2(AetherCore.Random.Range(-0.03f, 0.03f), -0.08f),
					"+" + Numbers.Short(b.Amount), Palette.Ichor);
			}
			_bearers[i] = b;
		}
	}

	private void ShowPop(Vector2 at, string text, Vector4 colour)
	{
		Pop pop = _pops[_nextPop];
		pop.From = at;
		pop.Colour = colour;
		pop.Age = 0.0f;
		Ui.SetText(pop.Label, text);
		_pops[_nextPop] = pop;
		_nextPop = (_nextPop + 1) % kPopPool;
	}

	/// <summary>Float the live popups up and fade them out, on unscaled time so they still
	/// read while the game is frozen behind a menu.</summary>
	private void UpdatePops(float unscaledDelta)
	{
		Vector4 bd = Backdrop;
		if (bd.Z <= 0.0f || Nave.Z <= 0.0f)
		{
			return;
		}
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
			Ui.SetAnchors(pop.Label, Vector2.Zero, Vector2.Zero);
			Ui.SetPivot(pop.Label, new Vector2(0.5f, 0.5f));
			Ui.SetRect(pop.Label, px.X, px.Y - rise * bd.W * 0.05f, 180.0f, 24.0f);
			Ui.SetTextColor(pop.Label, Palette.Fade(pop.Colour, 1.0f - t));
			if (t >= 1.0f)
			{
				Ui.SetText(pop.Label, "");
			}
			_pops[i] = pop;
		}
	}
}
