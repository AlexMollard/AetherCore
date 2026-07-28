using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen: choose a name, then host a session or join one by address.
/// Loads the arena once the transport reports success. This script is the only
/// place that decides a port default, so the address field can stay optional.
/// </summary>
/// <remarks>
/// Opening the session goes through <see cref="NetSession"/> rather than
/// <see cref="Net"/> directly. Starting a session and recording what kind of session it
/// is are one decision: a join that forgets its address has no way back after a dropped
/// link, and a host that leaves a stale address behind will try to reconnect to somebody
/// else's. Keeping the pair inside the SDK is what stops the arena and this screen
/// drifting apart.
/// </remarks>
public sealed class ConnectScreen : EntityScript
{
    /// <summary>The player's display name. No <c>UiTextBoxRef</c> component-ref
    /// wrapper exists in this build (see <c>ComponentRef.cs</c>), so this is a
    /// plain entity slot read through <see cref="Ui.GetTextBoxText"/>.</summary>
    public Entity NameField;

    /// <summary>The host:port to join. Same plain-entity route as <see cref="NameField"/>.</summary>
    public Entity AddressField;

    /// <summary>Starts hosting on <see cref="DefaultPort"/>.</summary>
    public Entity HostButton;

    /// <summary>Connects to the parsed address.</summary>
    public Entity JoinButton;

    /// <summary>Where connection errors and progress are reported.</summary>
    public Entity StatusText;

    /// <summary>Port used when hosting, and when the address field omits one.</summary>
    public ushort DefaultPort = 7777;

    /// <inheritdoc/>
    public override void OnAttach()
    {
        // Explain an involuntary return - the arena sends us back here when the host
        // goes away, and an unexplained title screen looks like a crash. Taken rather
        // than read so a later voluntary visit is not still apologising for it.
        string status = NetSession.TakeStatusMessage();
        if (status.Length > 0)
        {
            Ui.SetText(StatusText, status);
        }
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (Ui.WasClicked(HostButton))
        {
            StartHost();
        }
        else if (Ui.WasClicked(JoinButton) || Ui.WasSubmitted(AddressField))
        {
            StartJoin();
        }
    }

    /// <summary>Open a session and go straight to the arena.</summary>
    /// <remarks>
    /// The cap passed to <see cref="Net.Host"/> counts CONNECTIONS, and a host is not
    /// one of its own, so a four-player game hosts with three. The framework refuses the
    /// next joiner with a reason it can read and show, rather than dropping it - which
    /// is why the number lives here, in the game, and the refusal lives in the
    /// framework.
    /// </remarks>
    private void StartHost()
    {
        RememberName();
        if (NetSession.BeginHost(DefaultPort, WhisperSession.MaxPlayers - 1))
        {
            Scene.Load("Arena");
        }
        else
        {
            Ui.SetText(StatusText, $"Could not host: {Net.LastError}");
        }
    }

    private void StartJoin()
    {
        RememberName();
        (string ip, ushort port) = NetSession.ParseAddress(Ui.GetTextBoxText(AddressField), DefaultPort);
        if (NetSession.BeginJoin(ip, port))
        {
            Ui.SetText(StatusText, $"Connecting to {ip}:{port}...");
            Scene.Load("Arena");
        }
        else
        {
            Ui.SetText(StatusText, $"Could not connect: {Net.LastError}");
        }
    }

    // Assigning trims, and substitutes "Player" for a blank, so there is nothing to
    // check here.
    private void RememberName() => NetSession.LocalPlayerName = Ui.GetTextBoxText(NameField);
}
