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
	private Entity _gloom;
	private Entity _nameBox;
	private Entity _addressBox;
	private Entity _status;
	private Entity _whisperToggle;
	private Entity _shakeSlider;
	private Entity _shakeLabel;

	private Button _alone;
	private Button _host;
	private Button _join;

	private string _joinTarget = "";
	private float _joinElapsed;
	private bool _leaving;
	private bool _focusSeeded;

	public override void OnAttach()
	{
		_canvas = Scene.Find("ThresholdUI");
		if (!_canvas.IsValid)
		{
			_canvas = Ui.CreateCanvas();
		}

		// The name field has to come up holding the keeper who was last on this machine, and
		// the two settings below have to come up where they were left. EnsureLoaded is what
		// makes asking here free: the vigil asks too, and only the first read does any work,
		// so the offline catch-up is computed once and reported once - by the vigil, which is
		// the screen that can show it.
		SaveSystem.EnsureLoaded();

		_gloom = Ui.CreateEffect(_canvas, "ui_gloom");
		_gloom.Component("UI Effect").SetBool("background", true);
		Ui.SetEffectSortOrder(_gloom, -10);
		Ui.SetEffectColors(_gloom, Palette.Void, Palette.DreadDeep);

		Centred(UiKit.Text(_canvas, "HOLLOWTIDE", 0.0f, 0.0f, 900.0f, 90.0f, 72.0f, Palette.Bone,
			UiHAlign.Center, Palette.Display), 0.0f, -250.0f, 900.0f, 90.0f);
		Centred(UiKit.Text(_canvas, "keep the parish, and count what it costs you", 0.0f, 0.0f,
			900.0f, 30.0f, 18.0f, Palette.BoneFaint, UiHAlign.Center, Palette.Whisper),
			0.0f, -186.0f, 900.0f, 30.0f);

		Centred(UiKit.Text(_canvas, "YOUR NAME", 0.0f, 0.0f, 400.0f, 26.0f, 15.0f, Palette.BoneFaint,
			UiHAlign.Left, Palette.Display), -200.0f, -118.0f, 400.0f, 26.0f);
		_nameBox = MakeBox("Keeper", 20, UiContentType.Alphanumeric, -86.0f);
		Ui.SetTextBoxText(_nameBox, Vigil.KeeperName);

		Centred(UiKit.Text(_canvas, "HOST ADDRESS", 0.0f, 0.0f, 400.0f, 26.0f, 15.0f, Palette.BoneFaint,
			UiHAlign.Left, Palette.Display), -200.0f, -30.0f, 400.0f, 26.0f);
		_addressBox = MakeBox("127.0.0.1:7777", 48, UiContentType.Host, 2.0f);

		_alone = CentredButton("KEEP VIGIL ALONE", 0.0f, 72.0f, 400.0f, 48.0f);
		_host = CentredButton("HOST A CONGREGATION", -104.0f, 130.0f, 192.0f, 44.0f);
		_join = CentredButton("JOIN ONE", 104.0f, 130.0f, 192.0f, 44.0f);

		BuildSettings();

		_status = Centred(UiKit.Text(_canvas, "", 0.0f, 0.0f, 900.0f, 26.0f, 16.0f, Palette.BoneDim,
			UiHAlign.Center, Palette.Whisper), 0.0f, 262.0f, 900.0f, 26.0f);

		// Whatever ended the last session - a host that vanished, a deliberate exit - is
		// reported here, because this is where the player was sent.
		string carried = NetSession.TakeStatusMessage();
		if (!string.IsNullOrEmpty(carried))
		{
			SetStatus(carried, Palette.Dread);
		}

		Net.ReplicationReady = true;
	}

	private Entity MakeBox(string placeholder, int maxLength, UiContentType type, float y)
	{
		Entity box = Ui.CreateTextBox(_canvas);
		box.SetParent(_canvas);
		Ui.SetAnchors(box, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
		Ui.SetPivot(box, new Vector2(0.5f, 0.5f));
		Ui.SetRect(box, 0.0f, y, 400.0f, 44.0f);
		Ui.SetPlaceholder(box, placeholder);
		Ui.SetMaxLength(box, maxLength);
		Ui.SetContentType(box, type);
		Ui.SetFontSize(box, 18.0f);
		return box;
	}

	private void BuildSettings()
	{
		Centred(UiKit.Text(_canvas, "SHOW WHISPERS", 0.0f, 0.0f, 240.0f, 26.0f, 15.0f, Palette.BoneFaint,
			UiHAlign.Left, Palette.Display), -200.0f, 196.0f, 240.0f, 26.0f);

		// No create-export exists for a toggle or a slider, so the widget is an image with the
		// component added: the same entity the editor would author, assembled from script.
		_whisperToggle = UiKit.Image(_canvas, 0.0f, 0.0f, 56.0f, 26.0f, Palette.Transparent);
		Centred(_whisperToggle, 24.0f, 196.0f, 56.0f, 26.0f);
		_whisperToggle.Component("UI Toggle").Add();
		Ui.SetToggle(_whisperToggle, SaveSystem.ShowWhispers);
		Ui.SetSelectable(_whisperToggle);

		_shakeLabel = Centred(UiKit.Text(_canvas, "", 0.0f, 0.0f, 240.0f, 26.0f, 15.0f, Palette.BoneFaint,
			UiHAlign.Left, Palette.Display), 68.0f, 196.0f, 240.0f, 26.0f);

		_shakeSlider = UiKit.Image(_canvas, 0.0f, 0.0f, 120.0f, 22.0f, Palette.Transparent);
		Centred(_shakeSlider, 200.0f, 196.0f, 120.0f, 22.0f);
		ComponentAccess slider = _shakeSlider.Component("UI Slider");
		slider.Add();
		slider.SetFloat("min", 0.0f);
		slider.SetFloat("max", 1.5f);
		slider.SetFloat("step", 0.1f);
		slider.SetVector4("fill_color", Palette.Dread);
		Ui.SetSliderValue(_shakeSlider, SaveSystem.DreadShake);
		Ui.SetSelectable(_shakeSlider);
	}

	private static Entity Centred(Entity e, float x, float y, float w, float h)
	{
		Ui.SetAnchors(e, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f));
		Ui.SetPivot(e, new Vector2(0.5f, 0.5f));
		Ui.SetRect(e, x, y, w, h);
		return e;
	}

	private Button CentredButton(string label, float x, float y, float w, float h)
	{
		Button b = UiKit.MakeButton(_canvas, label, 0.0f, 0.0f, w, h, 15.0f, Palette.Display);
		Centred(b.Root, x, y, w, h);
		return b;
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

		StyleButtons();
		ReadSettings();

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

	private void StyleButtons()
	{
		_alone.Style(true, Palette.Mix(Palette.RowHot, Palette.Ichor, 0.30f), Palette.Row, Palette.PanelDeep);
		_host.Style(true, Palette.Mix(Palette.RowHot, Palette.Sigil, 0.30f), Palette.Row, Palette.PanelDeep);
		_join.Style(true, Palette.Mix(Palette.RowHot, Palette.Sigil, 0.30f), Palette.Row, Palette.PanelDeep);
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
