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
	private Entity _whisperToggle;
	private Entity _shakeSlider;
	private Entity _shakeLabel;
	private Entity _standing;

	private Button _alone;
	private Button _host;
	private Button _join;
	private Button _wipe;

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
		_whisperToggle = Scene.Find("ThWhisperToggle");
		_shakeSlider = Scene.Find("ThShakeSlider");
		_shakeLabel = Scene.Find("ThShakeLabel");
		_alone = Button.Find("ThAlone");
		_host = Button.Find("ThHost");
		_join = Button.Find("ThJoin");
		_wipe = Button.Find("ThWipe");
		_standing = Scene.Find("ThStanding");

		Ui.SetTextBoxText(_nameBox, Vigil.KeeperName);
		ShowStanding();
		Ui.SetToggle(_whisperToggle, SaveSystem.ShowWhispers);
		Ui.SetSliderValue(_shakeSlider, SaveSystem.DreadShake);

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
			Ui.SetFocus(_alone.Root);
		}

		ReadSettings();
		ReadWipe(deltaTime);

		if (_joinTarget.Length > 0)
		{
			WatchJoin(deltaTime);
			return;
		}

		if (_alone.Activated)
		{
			CommitName();
			Enter("");
		}
		else if (_host.Activated)
		{
			StartHost();
		}
		else if (_join.Activated || (Ui.WasSubmitted(_addressBox) && Ui.GetTextBoxText(_addressBox).Length > 0))
		{
			StartJoin(Ui.GetTextBoxText(_addressBox));
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

		Ui.SetText(_standing,
			"kept " + Numbers.Duration(Vigil.PlayedSeconds) +
			"   " + Vigil.SigilsEarned + " sigils taken" +
			"   " + Vigil.MarksHeld() + "/" + Content.Marks.Length + " marks" +
			"   " + named + "/" + Content.RiteCount + " named");
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

	/// <summary>Settings are persisted the moment they change rather than on the way out, so a
	/// player who alt-F4s off this screen keeps what they just set.</summary>
	private void ReadSettings()
	{
		if (Ui.WasChanged(_whisperToggle))
		{
			SaveSystem.ShowWhispers = Ui.GetToggle(_whisperToggle);
			SaveSystem.Save();
		}
		if (Ui.WasChanged(_shakeSlider))
		{
			SaveSystem.DreadShake = Ui.GetSliderValue(_shakeSlider);
			SaveSystem.Save();
		}
		Ui.SetText(_shakeLabel, "UNSTEADINESS " + Numbers.Percent(SaveSystem.DreadShake / 1.5f));
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
