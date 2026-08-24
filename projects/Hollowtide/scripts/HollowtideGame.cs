using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The one script the vigil scene carries. Owns the panels, drives the simulation, and is
/// the only place the game decides anything about a frame.
/// </summary>
/// <remarks>
/// <para>
/// Everything visible is built here at runtime rather than authored in the scene. An idle
/// game's interface is a function of its content tables - eight rites, thirty offerings,
/// twelve marks - and hand-placing rows for those in a scene file would mean editing a
/// scene every time a rite is added. The scene therefore holds a camera, a canvas and this
/// script, and nothing else.
/// </para>
/// <para>
/// The save is read through <see cref="SaveSystem.EnsureLoaded"/>, which runs once per
/// process - so neither the threshold before it nor the editor's Stop/Play cycle can
/// re-run the offline catch-up and hand out the same eight hours twice.
/// </para>
/// </remarks>
public sealed class HollowtideGame : EntityScript
{
	/// <summary>Seconds between autosaves. Public, so it is editable in the Inspector and
	/// saved with the scene.</summary>
	public float AutosaveSeconds = 15.0f;

	private readonly Hud _hud = new Hud();
	private readonly Ledger _ledger = new Ledger();
	private readonly Congregation _congregation = new Congregation();
	private readonly Parish _parish = new Parish();
	private readonly Whispers _whispers = new Whispers();

	private Entity _canvas;
	private Entity _gloom;
	private Entity _flash;

	private Entity _offlinePanel;
	private Entity _offlineBody;
	private Button _offlineDismiss;

	private float _autosave;
	private float _flashAmount;
	/// <summary>Seconds left of the slowed moment after a visitation. The one place the game
	/// touches the global time scale.</summary>
	private float _heldBreath;
	private float _lastUnscaled;

	public override void OnAttach()
	{
		_canvas = Scene.Find("HollowUI");
		if (!_canvas.IsValid)
		{
			// A scene opened without its canvas still runs; the UI simply makes its own.
			_canvas = Ui.CreateCanvas();
		}

		BindBackdrop();
		_hud.Bind();
		_ledger.Bind();
		_congregation.Bind();
		_whispers.Bind();
		_parish.Bind();
		BindOfflinePanel();

		// Assigned, never subscribed: these are statics that outlive a hot reload, and a
		// += here would stack a second copy of the feed onto every rebuild.
		Vigil.Announce = (line, omen) => _whispers.Say(line, omen);
		Vigil.OnVisitation = OnVisitation;
		Vigil.OnApproach = OnApproach;
		Vigil.OnFound = OnFound;
		Vigil.OnYield = (rite, amount) => _parish.Delivered(rite, amount);
		Vigil.OnSpent = rite => _parish.Bought(rite);

		if (!SaveSystem.EnsureLoaded())
		{
			_whispers.Say("You take the ledger. The last keeper left it open.", Omen.Plain);
		}

		_lastUnscaled = Time.UnscaledTime;
		Time.Resume();
	}

	public override void OnDetach()
	{
		// Leaving the scene is the last chance to keep the run; the autosave timer may be
		// most of its interval away.
		SaveSystem.Save();
		Vigil.Announce = null;
		Vigil.OnVisitation = null;
		Vigil.OnApproach = null;
		Vigil.OnFound = null;
		Vigil.OnYield = null;
		Vigil.OnSpent = null;
		Time.Resume();
	}

	public override void OnUpdate(float deltaTime)
	{
		// Unscaled delta, derived rather than read: presentation must keep moving through the
		// slowed moment after a visitation, and Time.DeltaTime is scaled by definition.
		float now = Time.UnscaledTime;
		float unscaled = MathF.Max(0.0f, now - _lastUnscaled);
		_lastUnscaled = now;

		bool modal = _offlinePanel.IsValid && _offlinePanel.ActiveInHierarchy;
		bool connected = Net.IsConnected;
		int keepers = _congregation.Update(connected);

		// Standing with others is worth five percent each. It is the only multiplier in the
		// game that another player can give you just by being there.
		Vigil.Alone = !connected || keepers <= 1;
		Vigil.CongregationBonus = 1.0 + 0.05 * Math.Max(0, keepers - 1);

		HoldBreath(unscaled);

		if (!modal)
		{
			Vigil.Tick(deltaTime);
			_hud.Update(unscaled, connected, keepers);
			_ledger.Update();
			Hotkeys();

			if (_hud.GatheredThisFrame)
			{
				double before = Vigil.Ichor;
				Vigil.Gather();
				_parish.Gathered(Vigil.Ichor - before, _hud.StrikePoint);
			}
			if (_hud.StokePressed)
			{
				Vigil.Stoke();
			}
			if (_hud.AnswerGiven != Answer.None)
			{
				Vigil.Give(_hud.AnswerGiven);
			}
			if (_hud.WardPressed)
			{
				Vigil.RaiseWard();
			}
			if (_hud.BellPressed)
			{
				RingBell();
			}
		}

		_parish.Update(deltaTime, unscaled);
		_whispers.Update(unscaled, SaveSystem.ShowWhispers);
		UpdateBackdrop(unscaled);
		UpdateOfflinePanel();
		Autosave(unscaled);
		Leaving();
	}

	// ── Backdrop ─────────────────────────────────────────────────────────────────────

	/// <summary>Two instances of one shader, both authored: a fog BEHIND the interface that
	/// thickens with dread, and a flash IN FRONT of it for the moment something arrives. Only
	/// their per-frame params are driven from here.</summary>
	private void BindBackdrop()
	{
		_gloom = Scene.Find("VigilGloom");
		_flash = Scene.Find("VigilFlash");
	}

	private void UpdateBackdrop(float unscaled)
	{
		float dread = (float)Vigil.Dread;
		_flashAmount = MathF.Max(0.0f, _flashAmount - unscaled * 1.25f);

		Ui.SetEffectParams(_gloom, new Vector4(Time.UnscaledTime, dread, 0.0f,
			Vigil.SurgeSeconds > 0.0 ? 1.0f : 0.0f));
		// z picks which half of the shader runs, and w carries the flash - so the overlay
		// costs nothing while it is zero: the shader discards on it rather than blending a
		// transparent full-screen quad every frame.
		Ui.SetEffectParams(_flash, new Vector4(Time.UnscaledTime, dread, 1.0f, _flashAmount));
	}

	/// <summary>The parish gave something up. Thrown from wherever the keeper struck, because
	/// that is where they were looking - the same reason a gather's figure is thrown there.</summary>
	private void OnFound(Relic relic)
	{
		_parish.Found(relic, _hud.StrikePoint);
		if (relic.Grade >= Grade.Hollowed)
		{
			// Only the top grade takes the screen. A flash for anything less would spend the
			// game's loudest gesture on something a keeper turns up every few minutes.
			_flashAmount = MathF.Max(_flashAmount, 0.5f);
		}
	}

	/// <summary>Something started walking. A flash, but no held breath - the slowed moment
	/// belongs to the ARRIVAL, and spending it on the warning would leave the arrival itself
	/// with nothing left to do.</summary>
	private void OnApproach(int rite)
	{
		_flashAmount = 0.6f;
	}

	private void OnVisitation(bool held)
	{
		_parish.Visitation(held);
		if (held)
		{
			// A ward holding is a RELIEF. No held breath and a much smaller flash, because
			// stopping the world for something that did not happen teaches the player that
			// insurance feels the same as being caught.
			_flashAmount = 0.35f;
			return;
		}
		_flashAmount = 1.0f;
		_heldBreath = 0.85f;
	}

	/// <summary>The world goes slow for a moment after a visitation. Scripts keep ticking at
	/// the scaled rate, so the simulation slows with everything else - which is the point:
	/// the parish stops producing while whatever it was is still in the room.</summary>
	private void HoldBreath(float unscaled)
	{
		if (_heldBreath > 0.0f)
		{
			_heldBreath = MathF.Max(0.0f, _heldBreath - unscaled);
			Time.Scale = _heldBreath > 0.0f ? 0.22f : 1.0f;
		}
	}

	// ── Input ────────────────────────────────────────────────────────────────────────

	/// <summary>Number keys buy the matching rite at the ledger's current amount. Skipped
	/// whenever the UI owns the keyboard, so typing into a field never buys anything.</summary>
	private void Hotkeys()
	{
		if (Ui.HasFocus)
		{
			return;
		}

		// While something is walking, 1-4 answer it rather than buying rites. The encounter
		// borrows the keys it needs and gives them straight back, so a keeper reaching for an
		// answer in a hurry cannot accidentally buy a tier instead.
		if (Vigil.Approaching)
		{
			for (int i = 0; i < 4; i++)
			{
				if (Input.IsKeyPressed((Key)((int)Key.Num1 + i)))
				{
					Vigil.Give((Answer)(i + 1));
				}
			}
			return;
		}

		for (int rite = 0; rite < Content.RiteCount; rite++)
		{
			Key key = (Key)((int)Key.Num1 + rite);
			if (Input.IsKeyPressed(key))
			{
				int amount = _ledger.BuyAmount > 0 ? _ledger.BuyAmount : Math.Max(1, Vigil.Affordable(rite));
				Vigil.BuyRite(rite, amount);
			}
		}

		if (Input.IsKeyPressed(Key.W))
		{
			Vigil.RaiseWard();
		}
		if (Input.IsKeyPressed(Key.B))
		{
			RingBell();
		}
	}

	private void RingBell()
	{
		VigilPresence? local = VigilPresence.Local;
		VigilRites? rites = local?.Self.GetScript<VigilRites>();
		if (rites != null)
		{
			rites.RingBell();
			return;
		}
		// Alone, the bell still does something - a smaller surge, granted locally. A control
		// that is dead in single player teaches the player to ignore it in multiplayer too.
		Vigil.BeginSurge(8.0, 1.6, fromCongregation: false);
		_whispers.Say("You ring the bell. Only the parish hears it.", Omen.Plain);
	}

	/// <summary>Escape leaves: out of a session first, and out of the vigil after that.</summary>
	private void Leaving()
	{
		if (!Input.IsKeyPressed(Key.Escape) || Ui.HasFocus)
		{
			return;
		}
		SaveSystem.Save();
		if (Net.IsConnected)
		{
			Entity session = Scene.Find("Session");
			HollowtideSession? director = session.IsValid ? session.GetScript<HollowtideSession>() : null;
			if (director != null)
			{
				director.Leave("You step back out of the circle.");
				return;
			}
			Net.Disconnect();
		}
		Scene.Load("Threshold");
	}

	// ── Offline ──────────────────────────────────────────────────────────────────────

	/// <summary>Bind the authored report. Hidden here rather than in the scene because a scene
	/// stores no active flag - what is authored is the layout, and whether it is up is state.</summary>
	private void BindOfflinePanel()
	{
		_offlinePanel = Scene.Find("OfflinePanel");
		_offlineBody = Scene.Find("OfflineBody");
		_offlineDismiss = Button.Find("OfflineDismiss");
		_offlinePanel.SetActive(false);
	}

	private void UpdateOfflinePanel()
	{
		if (!SaveSystem.PendingOffline.HasValue)
		{
			return;
		}
		if (!_offlinePanel.ActiveInHierarchy)
		{
			OfflineReport report = SaveSystem.PendingOffline.Value;
			if (_offlineBody.IsValid)
			{
				Ui.SetText(_offlineBody,
					"The parish kept working for " + Numbers.Duration(report.Seconds) +
					(report.Capped ? " (as long as it will keep going unattended).\n\n" : ".\n\n") +
					"It gathered " + Numbers.Short(report.Ichor) + " ichor at " +
					Numbers.Percent(Vigil.OfflineEfficiency) + " of its usual pace, " +
					"and the dark came " + Numbers.Percent(report.Dread) + " closer.");
			}
			_offlinePanel.SetActive(true);
			Ui.SetFocus(_offlineDismiss.Root);
		}

		if (_offlineDismiss.Activated)
		{
			_offlinePanel.SetActive(false);
			SaveSystem.PendingOffline = null;
			Ui.ClearFocus();
		}
	}

	// ── Persistence ──────────────────────────────────────────────────────────────────

	private void Autosave(float unscaled)
	{
		_autosave += unscaled;
		if (_autosave >= AutosaveSeconds)
		{
			_autosave = 0.0f;
			SaveSystem.Save();
		}
	}
}
