using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The threshold: name yourself, choose whether anyone else is coming, and step in.
/// </summary>
/// <remarks>
/// <para>
/// Also where the two settings live, because they are read by the vigil and set exactly
/// once. Both are engine widgets driven by the engine's own widget system - a toggle and a
/// slider that keyboard, pad and mouse all reach without this screen tracking a highlight.
/// </para>
/// <para>
/// Hosting enters immediately; joining waits here, because a client that changes scene
/// before the link is live builds the whole session's cast into the title screen and then
/// destroys it on the load. <see cref="Net.ReplicationReady"/> going false first is what
/// makes the wait safe.
/// </para>
/// </remarks>
public sealed class ThresholdScreen : EntityScript
{
	public ushort Port = 7777;
	public float JoinTimeoutSeconds = 8.0f;

	private Entity _canvas;
	private Entity _nameBox;
	private Entity _addressBox;
	private Entity _status;
	private Entity _standing;

	private readonly VigilSettings _presentation = new();

	private Button _play;
	private Button _congregation;
	private Button _settings;
	private Button _leave;
	private Button _back;
	private Button _host;
	private Button _join;
	private Button _wipe;

	/// <summary>Every control the menu knows about, kept beside its name so the page switch is
	/// one loop rather than twenty calls that have to stay in agreement.</summary>
	private readonly System.Collections.Generic.Dictionary<string, Entity> _widgets = new();

	/// <summary>True once the wipe has been pressed and is waiting to be pressed again.</summary>
	private bool _wipeArmed;
	private float _wipeArms;

	private string _joinTarget = "";
	private float _joinElapsed;
	private bool _leaving;
	private bool _focusSeeded;

	public override void OnAttach()
	{
		_canvas = Scene.Find("ThresholdUI");

		// The name field has to come up holding the keeper who was last on this machine, and
		// the two settings below have to come up where they were left. EnsureLoaded is what
		// makes asking here free: the vigil asks too, and only the first read does any work,
		// so the offline catch-up is computed once and reported once - by the vigil, which is
		// the screen that can show it.
		SaveSystem.EnsureLoaded();

		// Everything on this screen is authored in the scene. Binding it is the whole of the
		// setup: where the fields sit and what colour they are is the editor's business, and
		// this script only seeds their values and reads them back.
		_nameBox = Scene.Find("ThNameBox");
		_addressBox = Scene.Find("ThAddressBox");
		_status = Scene.Find("ThStatus");
		_play = Button.Find("ThPlay");
		_congregation = Button.Find("ThCongregation");
		_settings = Button.Find("ThSettings");
		_leave = Button.Find("ThLeave");
		_back = Button.Find("ThBack");
		_host = Button.Find("ThHost");
		_join = Button.Find("ThJoin");
		_wipe = Button.Find("ThWipe");
		_standing = Scene.Find("ThStanding");

		// Found by the names the menu itself uses, so a control the table mentions and the scene
		// does not is an invalid entity here rather than a control that never hides.
		Menu.Reset();
		foreach (MenuPage page in Menu.Pages)
		{
			foreach (string name in Menu.Widgets(page))
			{
				_widgets[name] = Scene.Find(name);
			}
		}
		_widgets[Menu.BackWidget] = _back.Root;

		Ui.SetTextBoxText(_nameBox, Vigil.KeeperName);
		ShowStanding();
		_presentation.Bind("Th");
		TypeScale.Adopt(_canvas);
		TypeScale.Apply(SaveSystem.TypeScale);
		ShowPage();

		// Whatever ended the last session - a host that vanished, a deliberate exit - is
		// reported here, because this is where the player was sent.
		string carried = NetSession.TakeStatusMessage();
		if (!string.IsNullOrEmpty(carried))
		{
			SetStatus(carried, Palette.Dread);
		}

		Net.ReplicationReady = true;
	}

	public override void OnUpdate(float deltaTime)
	{
		if (_leaving)
		{
			return;
		}

		// One seeded focus, on the first frame the screen is up: a pad or a keyboard then has
		// somewhere to start, and the mouse is unaffected either way.
		if (!_focusSeeded)
		{
			_focusSeeded = true;
			Ui.SetFocus(_play.Root);
		}

		_presentation.Read();
		ReadWipe(deltaTime);

		if (_joinTarget.Length > 0)
		{
			WatchJoin(deltaTime);
			return;
		}

		// Escape goes back a page before it does anything else. On the root it does nothing at
		// all - LEAVE is a button a keeper has to mean, and a menu that quits on a stray Escape
		// is a menu people learn to be careful around.
		if (Input.IsKeyPressed(Key.Escape) && !Ui.HasFocus && Menu.Back())
		{
			ShowPage();
			return;
		}

		if (_back.Activated && Menu.Back())
		{
			ShowPage();
			return;
		}

		switch (Menu.Page)
		{
			case MenuPage.Root:
				ReadRoot();
				break;
			case MenuPage.Congregation:
				ReadCongregation();
				break;
			default:
				break;
		}
	}

	private void ReadRoot()
	{
		if (_play.Activated)
		{
			CommitName();
			Enter("");
		}
		else if (_congregation.Activated && Menu.Open(MenuPage.Congregation))
		{
			CommitName();
			ShowPage();
		}
		else if (_settings.Activated && Menu.Open(MenuPage.Settings))
		{
			ShowPage();
		}
		else if (_leave.Activated)
		{
			// The name first: somebody who types a name and then leaves has still named
			// themselves, and losing it would make the field feel like it did not take.
			CommitName();
			App.Quit();
		}
	}

	private void ReadCongregation()
	{
		if (_host.Activated)
		{
			StartHost();
		}
		else if (_join.Activated || (Ui.WasSubmitted(_addressBox) && Ui.GetTextBoxText(_addressBox).Length > 0))
		{
			StartJoin(Ui.GetTextBoxText(_addressBox));
		}
	}

	/// <summary>
	/// Put the screen where the menu says it is.
	/// </summary>
	/// <remarks>
	/// One loop over one table. Every control the menu names is asked the same question and
	/// nothing else decides visibility, so a control cannot be shown on its own page and left
	/// standing on the other two - which is the failure that hand-written SetActive calls
	/// produce and which looks entirely correct until somebody opens the other page.
	/// </remarks>
	private void ShowPage()
	{
		foreach (System.Collections.Generic.KeyValuePair<string, Entity> pair in _widgets)
		{
			if (pair.Value.IsValid)
			{
				pair.Value.SetActive(Menu.Shows(pair.Key));
			}
		}
		_wipeArmed = false;
		_wipe.SetLabel("BEGIN A NEW VIGIL");
		// Focus follows the page, because a pad or a keyboard left pointing at a control that is
		// no longer on screen has nowhere sensible to go next.
		Ui.SetFocus(Menu.Page switch
		{
			MenuPage.Congregation => _host.Root,
			MenuPage.Settings => _back.Root,
			_ => _play.Root,
		});
		if (SaveSystem.HadSave)
		{
			_wipe.SetActive(Menu.Page == MenuPage.Settings);
		}
	}

	/// <summary>
	/// Say what this keeper already is, or say nothing at all.
	/// </summary>
	/// <remarks>
	/// Drawn from the save rather than from the parish, because the parish is gone by now: what
	/// survives a communion - sigils taken, marks kept, visitors named - is exactly what a
	/// returning player wants to see they still have. A keeper with no save gets a blank line
	/// and no wipe button, so a first run is not greeted with an offer to delete itself.
	/// </remarks>
	private void ShowStanding()
	{
		bool returning = SaveSystem.HadSave;
		_wipe.SetActive(returning);
		if (!returning)
		{
			Ui.SetText(_standing, "");
			return;
		}

		int named = 0;
		foreach (bool bested in Vigil.VisitorsBested)
		{
			if (bested)
			{
				named++;
			}
		}

		// What they turned up, which had grown into the largest thing in the game while this
		// line still listed only time, sigils, marks and visitors. A keeper coming back after a
		// week should see the part of their record they are proudest of.
		string dug = Vigil.RelicsFound > 0
			? "   " + Vigil.RelicsFound + " found"
				+ (Vigil.BestRelicGrade >= 0
					? ", best " + Relics.GradeName((Grade)Vigil.BestRelicGrade).ToLowerInvariant()
					: "")
			: "";
		// Only mentioned once it has happened, so a first-time keeper is not shown a zero for a
		// system they have not met.
		string lost = Vigil.RelicsLost > 0 ? "   " + Vigil.RelicsLost + " taken from you" : "";

		Ui.SetText(_standing,
			"kept " + Numbers.Duration(Vigil.PlayedSeconds) +
			"   " + Vigil.SigilsEarned + " sigils taken" +
			"   " + Vigil.MarksHeld() + "/" + Content.Marks.Length + " marks" +
			"   " + named + "/" + Content.RiteCount + " named" + dug + lost);
		Ui.SetTextColor(_standing, Palette.TextDim);
	}

	/// <summary>The wipe asks twice. There is no undo behind it and no dialog system in this
	/// project to put in front of it, so the button is its own confirmation - and it disarms on
	/// its own, because a destructive control left armed is a trap for the next click.</summary>
	private void ReadWipe(float deltaTime)
	{
		if (!SaveSystem.HadSave)
		{
			return;
		}

		if (_wipeArmed)
		{
			_wipeArms -= deltaTime;
			if (_wipeArms <= 0.0f)
			{
				_wipeArmed = false;
				_wipe.SetLabel("BEGIN A NEW VIGIL");
			}
		}

		if (!_wipe.Activated)
		{
			return;
		}
		if (!_wipeArmed)
		{
			_wipeArmed = true;
			_wipeArms = 4.0f;
			_wipe.SetLabel("EVERYTHING? PRESS AGAIN");
			return;
		}

		SaveSystem.Wipe();
		_wipeArmed = false;
		_wipe.SetLabel("BEGIN A NEW VIGIL");
		Ui.SetTextBoxText(_nameBox, Vigil.KeeperName);
		ShowStanding();
		SetStatus("The ledger is blank. Whoever you were, the parish has forgotten.", Palette.Dread);
	}

	private void CommitName()
	{
		string typed = Ui.GetTextBoxText(_nameBox).Trim();
		Vigil.KeeperName = typed.Length > 0 ? typed : "Keeper";
		SaveSystem.Save();
	}

	private void StartHost()
	{
		CommitName();
		if (!NetSession.BeginHost(Port, HollowtideSession.MaxKeepers - 1))
		{
			SetStatus("Could not open the door on port " + Port + " - " + Net.LastError, Palette.Dread);
			return;
		}
		Enter("The door is open on port " + Port + ".");
	}

	private void StartJoin(string typed)
	{
		CommitName();
		(string ip, ushort port) = NetSession.ParseAddress(typed, Port);
		string target = ip + ":" + port;

		// Held BEFORE the connect: this peer is standing on a menu, and a replicated entity is
		// built into whichever scene the peer is in when its spawn arrives.
		Net.ReplicationReady = false;
		if (!NetSession.BeginJoin(ip, port))
		{
			Net.ReplicationReady = true;
			SetStatus("Nothing answers at " + target + " - " + Net.LastError, Palette.Dread);
			return;
		}

		_joinTarget = target;
		_joinElapsed = 0.0f;
		Ui.ClearFocus();
		SetStatus("Listening at " + target + "...", Palette.Sigil);
	}

	private void WatchJoin(float deltaTime)
	{
		_joinElapsed += deltaTime;

		if (Net.IsConnected)
		{
			Enter("Something at " + _joinTarget + " lets you in.");
			return;
		}
		if (!Net.IsClient)
		{
			AbandonJoin("Nothing is listening at " + _joinTarget + ".");
			return;
		}
		if (_joinElapsed >= JoinTimeoutSeconds)
		{
			AbandonJoin("No answer from " + _joinTarget + ".");
			return;
		}
		SetStatus("Listening at " + _joinTarget + "... " + _joinElapsed.ToString("0.0") + "s", Palette.Sigil);
	}

	private void AbandonJoin(string message)
	{
		Net.Disconnect();
		Net.ReplicationReady = true;
		NetSession.JoinRequested = false;
		_joinTarget = "";
		_joinElapsed = 0.0f;
		_focusSeeded = false;
		SetStatus(message, Palette.Dread);
	}

	private void Enter(string message)
	{
		_leaving = true;
		if (message.Length > 0)
		{
			SetStatus(message, Palette.Sigil);
		}
		// Hand the keyboard back before the vigil exists to take it.
		Ui.ClearFocus();
		Scene.Load("Vigil");
	}

	private void SetStatus(string message, Vector4 colour)
	{
		Ui.SetText(_status, message);
		Ui.SetTextColor(_status, colour);
	}
}
