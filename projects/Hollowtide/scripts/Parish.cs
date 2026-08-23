using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The parish itself: the world behind the interface. One lantern per rite the keeper
/// holds, lit by transient 2D lights, with motes coming off a gather and the whole place
/// going dark and unsteady as dread rises.
/// </summary>
/// <remarks>
/// <para>
/// Nothing here is authored in the scene. A rite the keeper does not own has no entity at
/// all, and buying the first copy of one is what creates it - so the world is a readout of
/// the save rather than a set piece that has to be kept in step with it.
/// </para>
/// <para>
/// The lights are submitted per frame through <see cref="Lighting2D"/> rather than placed
/// as components. A lantern's brightness is a function of how many copies are held, which
/// changes constantly; a submitted light is re-stated every frame anyway, so there is no
/// component field to write and nothing to clean up when a visitation takes the last copy.
/// </para>
/// </remarks>
public sealed class Parish
{
	private const int kPopPool = 10;

	private readonly Entity[] _lanterns = new Entity[Content.RiteCount];
	private readonly WorldLabel?[] _labels = new WorldLabel?[Content.RiteCount];

	private struct Pop
	{
		public WorldLabel Label;
		public Vector3 From;
		public string Text;
		public Vector4 Colour;
		public float Age;
	}

	private readonly Pop[] _pops = new Pop[kPopPool];
	private int _nextPop;

	private Entity _motes;
	private Entity _camera;
	private Vector3 _cameraHome;
	private Entity _ambient;

	private float _shake;
	private float _time;

	public void Build(Entity canvas, Entity ambientOwner)
	{
		_camera = Camera.Main;
		if (_camera.IsValid)
		{
			_cameraHome = _camera.Position;
		}
		_ambient = ambientOwner;

		// One emitter, parked at the middle of the nave, fired by hand. Authoring it here
		// rather than in the scene keeps every particle setting next to the code that bursts
		// it, which is the only place either is ever read.
		_motes = Scene.Create("Motes", new Vector3(-3.4f, -0.4f, 0.0f));
		ComponentAccess emitter = _motes.Component("Particle Emitter");
		emitter.Add();
		emitter.SetFloat("rate", 0.0f);
		emitter.SetBool("emit_on_start", false);
		emitter.SetBool("emitting", false);
		emitter.SetInt("max_particles", 400);
		emitter.SetFloat("lifetime_min", 0.7f);
		emitter.SetFloat("lifetime_max", 1.8f);
		emitter.SetFloat("speed_min", 1.2f);
		emitter.SetFloat("speed_max", 3.6f);
		emitter.SetFloat("direction_deg", 90.0f);
		emitter.SetFloat("spread_deg", 150.0f);
		emitter.SetVector2("gravity", new Vector2(0.0f, -1.4f));
		emitter.SetFloat("start_size", 0.14f);
		emitter.SetFloat("end_size", 0.0f);
		emitter.SetVector4("start_color", Palette.Ichor);
		emitter.SetVector4("end_color", Palette.Fade(Palette.IchorDim, 0.0f));
		emitter.SetInt("sorting_layer", 2);

		for (int i = 0; i < kPopPool; i++)
		{
			// No outline. A WorldLabel's outline is four extra dark copies of the string, and
			// SetColor deliberately does not touch them - so a label that FADES keeps a fully
			// opaque black ghost of itself once its text has gone transparent. That is exactly
			// wrong for a floating number, and the parish behind it is dark enough that the
			// outline was buying no readability anyway.
			WorldLabel label = new WorldLabel(200.0f, 26.0f, canvas, outlineWidth: 0.0f);
			label.SetFontSize(19.0f);
			label.SetColor(Palette.Fade(Palette.Ichor, 0.0f));
			_pops[i] = new Pop { Label = label, Age = 99.0f, From = Vector3.Zero, Text = "", Colour = Palette.Ichor };
		}
	}

	/// <summary>Where a rite's lantern stands. An arc across the left of the view, deepest
	/// rites furthest in - so the parish visibly extends as the keeper goes down.</summary>
	private static Vector3 LanternAt(int rite)
	{
		float t = rite / (float)Math.Max(1, Content.RiteCount - 1);
		float x = -7.2f + t * 7.4f;
		float y = 2.9f - MathF.Sin(t * 2.4f) * 1.7f;
		return new Vector3(x, y, 0.0f);
	}

	public void Update(float deltaTime, float unscaledDelta)
	{
		_time += unscaledDelta;
		float dread = (float)Vigil.Dread;

		SyncLanterns(dread);
		UpdatePops(unscaledDelta);
		UpdateAmbient(dread);
		UpdateCamera(unscaledDelta, dread);
	}

	/// <summary>Create, retire and light one lantern per rite.</summary>
	private void SyncLanterns(float dread)
	{
		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			int owned = Vigil.Owned[rite];
			if (owned <= 0)
			{
				if (_lanterns[rite].IsValid)
				{
					_lanterns[rite].Destroy();
					_lanterns[rite] = default;
					_labels[rite]?.Destroy();
					_labels[rite] = null;
				}
				continue;
			}

			Vector3 at = LanternAt(rite);
			if (!_lanterns[rite].IsValid)
			{
				Entity e = Scene.Create("Lantern_" + Content.Rites[rite].Name, at);
				ComponentAccess sprite = e.Component("Sprite Renderer");
				sprite.Add();
				// No texture: an empty path samples plain white, so the tint below IS the
				// colour. The parish needs no art to read.
				sprite.SetVector2("pixel_size", new Vector2(16.0f, 16.0f));
				sprite.SetFloat("pixels_per_unit", 16.0f);
				sprite.SetVector4("tint", Content.Rites[rite].Colour);
				sprite.SetInt("sorting_layer", 1);
				// The engine's own behaviours do the idle motion, so nothing here has to run a
				// sine per lantern per frame.
				e.AddBob(0.12f + rite * 0.01f, 0.5f + rite * 0.07f, rite * 0.9f);
				e.AddSpin(new Vector3(0.0f, 0.0f, rite % 2 == 0 ? 9.0f : -7.0f));
				_lanterns[rite] = e;

				WorldLabel label = new WorldLabel(240.0f, 22.0f);
				label.SetFontSize(17.0f);
				_labels[rite] = label;
			}

			// A lantern grows with the tier it stands for, on a log curve: the difference
			// between 1 and 10 should be visible, and the difference between 1000 and 10000
			// should not fill the screen.
			float bulk = 0.42f + (float)Math.Log10(owned + 1.0) * 0.30f;
			_lanterns[rite].Scale = new Vector3(bulk, bulk, 1.0f);

			Vector4 colour = Content.Rites[rite].Colour;
			Vector4 lit = Palette.Mix(colour, Palette.Dread, dread * 0.7f);
			_lanterns[rite].Component("Sprite Renderer").SetVector4("tint", lit);

			// Flicker is per-lantern and out of phase, so the parish never pulses in unison -
			// a synchronised flicker reads as a rendering bug rather than as candlelight.
			float flicker = 0.86f + MathF.Sin(_time * (3.1f + rite * 0.7f) + rite) * 0.09f
				+ MathF.Sin(_time * (11.0f + rite)) * 0.05f * (0.3f + dread);
			float reach = 2.1f + bulk * 2.6f;
			Lighting2D.SubmitLight(new Vector2(at.X, at.Y), reach,
				new Vector3(lit.X, lit.Y, lit.Z), (1.9f + bulk) * flicker, castsShadow: false);

			_labels[rite]?.SetColor(Palette.Fade(lit, 0.75f));
			_labels[rite]?.Track(at + new Vector3(0.0f, bulk + 0.45f, 0.0f),
				Content.Rites[rite].Name + " x" + owned);
		}
	}

	/// <summary>A gather: motes off the emitter and a number that floats away.</summary>
	public void Gathered(double amount)
	{
		if (_motes.IsValid)
		{
			Particles.Burst(_motes, 14);
		}
		ShowPop(new Vector3(-3.4f + AetherCore.Random.Range(-0.8f, 0.8f), -0.3f, 0.0f), "+" + Numbers.Short(amount), Palette.Ichor);
	}

	/// <summary>A visitation: shake the parish, and throw the motes the wrong colour.</summary>
	public void Visitation()
	{
		_shake = 1.0f;
		if (_motes.IsValid)
		{
			ComponentAccess emitter = _motes.Component("Particle Emitter");
			emitter.SetVector4("start_color", Palette.Dread);
			emitter.SetFloat("speed_max", 7.0f);
			Particles.Burst(_motes, 220);
			// Put the emitter back for the next ordinary gather; the burst above has already
			// been spawned with the settings it wanted.
			emitter.SetVector4("start_color", Palette.Ichor);
			emitter.SetFloat("speed_max", 3.6f);
		}
		ShowPop(new Vector3(-3.4f, 0.6f, 0.0f), "TAKEN", Palette.Dread);
	}

	private void ShowPop(Vector3 at, string text, Vector4 colour)
	{
		Pop pop = _pops[_nextPop];
		pop.Age = 0.0f;
		pop.From = at;
		pop.Text = text;
		pop.Colour = colour;
		pop.Label.SetColor(colour);
		pop.Label.Track(at, text);
		_pops[_nextPop] = pop;
		_nextPop = (_nextPop + 1) % kPopPool;
	}

	/// <summary>Float the live popups upward and fade them out. Unscaled, so they still read
	/// while the game is frozen behind a menu.</summary>
	private void UpdatePops(float unscaledDelta)
	{
		for (int i = 0; i < kPopPool; i++)
		{
			Pop pop = _pops[i];
			if (pop.Age > 1.4f)
			{
				continue;
			}
			pop.Age += unscaledDelta;
			float t = Math.Clamp(pop.Age / 1.4f, 0.0f, 1.0f);
			// Rises quickly and slows: a linear float reads as a UI element sliding, an eased
			// one reads as something leaving.
			float rise = 1.0f - (1.0f - t) * (1.0f - t);
			pop.Label.SetColor(Palette.Fade(pop.Colour, 1.0f - t));
			// Cleared rather than left at zero alpha on the last tick. A transparent label is
			// still a label: it holds its string and its rect, and anything that later reads or
			// re-styles it brings the ghost back. Blanking is what actually ends it.
			string shown = t >= 1.0f ? "" : pop.Text;
			pop.Label.Track(pop.From + new Vector3(0.0f, rise * 1.5f, 0.0f), shown);
			_pops[i] = pop;
		}
	}

	/// <summary>The parish darkens as dread rises - the ambient light drops away, so the only
	/// thing left lighting the place is the rites the keeper is afraid to sell.</summary>
	private void UpdateAmbient(float dread)
	{
		if (!_ambient.IsValid)
		{
			return;
		}
		ComponentAccess settings = _ambient.Component("Light 2D Settings");
		if (!settings.Exists)
		{
			return;
		}
		settings.SetFloat("ambientIntensity", 0.42f - dread * 0.34f);
		settings.SetVector3("ambient_color", new Vector3(
			0.10f + dread * 0.16f,
			0.12f - dread * 0.06f,
			0.16f - dread * 0.07f));
	}

	/// <summary>Two things move the camera: a decaying kick after a visitation, and a slow
	/// unease that only exists at high dread. Both are added to the scene-authored home
	/// position rather than accumulated, so they cannot drift.</summary>
	private void UpdateCamera(float unscaledDelta, float dread)
	{
		if (!_camera.IsValid)
		{
			return;
		}
		_shake = MathF.Max(0.0f, _shake - unscaledDelta * 1.7f);

		float kick = _shake * _shake * 0.55f * SaveSystem.DreadShake;
		float unease = MathF.Max(0.0f, dread - 0.55f) * 0.10f * SaveSystem.DreadShake;

		float x = MathF.Sin(_time * 37.0f) * kick + MathF.Sin(_time * 1.3f) * unease;
		float y = MathF.Cos(_time * 41.0f) * kick + MathF.Cos(_time * 0.9f) * unease;
		_camera.Position = _cameraHome + new Vector3(x, y, 0.0f);
	}
}
