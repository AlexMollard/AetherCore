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

	/// <summary>The three presentation settings, shared with the threshold so a setting cannot
	/// be added to one page and forgotten on the other.</summary>
	private readonly VigilSettings _presentation = new VigilSettings();

	private Entity _menuVeil;
	private Button _menuResume;
	private Button _menuSettings;
	private Button _menuLeave;
	private Button _menuBack;

	/// <summary>Every control the vigil's menu knows about, kept beside its name so showing a
	/// page is one loop rather than a dozen calls that have to stay in agreement.</summary>
	private readonly System.Collections.Generic.Dictionary<string, Entity> _menuWidgets = new();

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
		BindMenu();

		// The parish is a different scene from the threshold, so its authored labels are
		// different entities and have to be adopted in their own right. Adopt clears what the
		// threshold left behind, which is a list of entities that no longer exist.
		TypeScale.Adopt(_canvas);
		TypeScale.Apply(SaveSystem.TypeScale);

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
		ReportSalvage();
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
			// The vigil is TICKED whether or not its menu is up. That is the point of it: the
			// parish keeps working, the meter keeps climbing, and a visitation that arrives while
			// somebody is reading the settings resolves exactly as it would have. A menu that
			// stopped the clock would be a button that suspends the game's one bargain.
			//
			// What the menu does stop is ACTING. The HUD and the ledger are not drawn either,
			// because the only way to stop a click reaching a button underneath is not to read
			// that button - and behind a veil at four fifths black there is nothing legible left
			// to go stale.
			Vigil.Tick(deltaTime);
		}
		if (!modal && !VigilMenu.Showing)
		{
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
		_hud.ShowFound(relic);
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
		// Max, not assignment. Three things reach for this one number now - a visitation, an
		// approach, and turning up something Hollowed - and a plain assignment lets a quieter
		// event STEAL the screen back from a louder one that is still playing out.
		_flashAmount = MathF.Max(_flashAmount, 0.6f);
	}

	private void OnVisitation(bool held)
	{
		_parish.Visitation(held);
		if (held)
		{
			// A ward holding is a RELIEF. No held breath and a much smaller flash, because
			// stopping the world for something that did not happen teaches the player that
			// insurance feels the same as being caught.
			_flashAmount = MathF.Max(_flashAmount, 0.35f);
			return;
		}
		_flashAmount = MathF.Max(_flashAmount, 1.0f);
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
		if (!Vigil.Ring())
		{
			return;
		}
		VigilPresence? local = VigilPresence.Local;
		VigilRites? rites = local?.Self.GetScript<VigilRites>();
		if (rites != null)
		{
			rites.RingBell();
			return;
		}
		// Reached while connected but before this keeper's entity exists - a real window during
		// a join, and the only time a ring cannot be put to the host. It grants the lone bell
		// LOCALLY, and it is deliberately the same lone bell the host would have granted: this
		// used to be its own pair of numbers, so the identical press paid eight seconds at one
		// and a half here and ten at double a moment later, for a reason no player could see.
		Vigil.BeginSurge(Vigil.kLoneBellSeconds, Vigil.kLoneBellMultiplier, fromCongregation: false);
		_whispers.Say("You ring the bell. Only the parish hears it.", Omen.Plain);
	}

	/// <summary>
	/// Escape opens the vigil's menu, and the menu is where leaving lives.
	/// </summary>
	/// <remarks>
	/// It used to leave outright: one press of a key people use to mean "not this", and the game
	/// closed. Nothing was lost - it saved first - but a parish somebody has kept for a week
	/// should not be one keystroke from gone, and there was nowhere else to put the settings that
	/// have to be judged against the parish's own text.
	/// </remarks>
	private void Leaving()
	{
		if (Input.IsKeyPressed(Key.Escape) && !Ui.HasFocus)
		{
			if (!VigilMenu.Showing)
			{
				VigilMenu.Show();
			}
			else
			{
				VigilMenu.Back();
			}
			ShowMenu();
			return;
		}

		if (!VigilMenu.Showing)
		{
			return;
		}
		_presentation.Read();

		if (_menuBack.Activated && VigilMenu.Back())
		{
			ShowMenu();
		}
		else if (VigilMenu.Page == MenuPage.Settings)
		{
			return;
		}
		else if (_menuResume.Activated)
		{
			VigilMenu.Hide();
			ShowMenu();
		}
		else if (_menuSettings.Activated && VigilMenu.Open(MenuPage.Settings))
		{
			ShowMenu();
		}
		else if (_menuLeave.Activated)
		{
			LeaveTheParish();
		}
	}

	/// <summary>Find the menu's controls by the names the model uses, so one it names and the
	/// scene never authors is an invalid entity here rather than a control that never appears.
	/// </summary>
	private void BindMenu()
	{
		_menuResume = Button.Find("VgResume");
		_menuSettings = Button.Find("VgSettings");
		_menuLeave = Button.Find("VgLeave");
		_menuBack = Button.Find("VgBack");

		VigilMenu.Hide();
		foreach (string name in VigilMenu.Always)
		{
			_menuWidgets[name] = Scene.Find(name);
		}
		foreach (MenuPage page in Menu.Pages)
		{
			foreach (string name in VigilMenu.Widgets(page))
			{
				_menuWidgets[name] = Scene.Find(name);
			}
		}
		_menuWidgets[VigilMenu.BackWidget] = _menuBack.Root;
		_menuVeil = Scene.Find("VgMenuVeil");
		_presentation.Bind("Vg");
		ShowMenu();
	}

	/// <summary>Put the vigil's menu where the model says it is. The same one loop over one
	/// table the threshold uses, for the same reason.</summary>
	private void ShowMenu()
	{
		foreach (System.Collections.Generic.KeyValuePair<string, Entity> pair in _menuWidgets)
		{
			if (pair.Value.IsValid)
			{
				pair.Value.SetActive(VigilMenu.Shows(pair.Key));
			}
		}
		if (!VigilMenu.Showing)
		{
			Ui.ClearFocus();
			return;
		}
		bool judging = VigilMenu.Page == MenuPage.Settings;
		Ui.SetImageColor(_menuVeil, judging ? Palette.VeilLight : Palette.Veil);
		Ui.SetFocus(judging ? _menuBack.Root : _menuResume.Root);
	}

	private void LeaveTheParish()
	{
		VigilMenu.Hide();
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

	/// <summary>Whether the keeper has been told their save was rescued.</summary>
	private bool _saidSalvaged;

	/// <summary>
	/// Tell the keeper if their vigil came out of a half-finished write.
	/// </summary>
	/// <remarks>
	/// Said here rather than where the recovery happens, because the recovery runs during boot -
	/// before anything is listening to the feed - and a line nobody is there to hear is the same
	/// as no line. A keeper whose save was rescued has had something quietly unusual happen to
	/// them and should be told, both because it explains any small discrepancy they notice and
	/// because a game that silently repairs itself is a game you cannot trust when it does not.
	/// </remarks>
	private void ReportSalvage()
	{
		if (_saidSalvaged || !SaveSystem.Salvaged)
		{
			return;
		}
		_saidSalvaged = true;
		Vigil.Tell("Your last save was cut short. The vigil has been recovered from what had"
			+ " been written; the unreadable copy has been kept.", Omen.Dread);
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
