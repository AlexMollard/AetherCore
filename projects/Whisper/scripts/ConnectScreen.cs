using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen: choose a name, then host a session or join one by address.
/// Loads the arena once the transport reports success. This script is the only
/// place that decides a port default, so the address field can stay optional.
/// </summary>
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

    private void StartHost()
    {
        RememberName();
        if (Net.Host(DefaultPort))
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
        (string ip, ushort port) = ParseAddress(Ui.GetTextBoxText(AddressField));
        if (Net.Connect(ip, port))
        {
            Ui.SetText(StatusText, $"Connecting to {ip}:{port}...");
            Scene.Load("Arena");
        }
        else
        {
            Ui.SetText(StatusText, $"Could not connect: {Net.LastError}");
        }
    }

    private void RememberName()
    {
        string name = Ui.GetTextBoxText(NameField).Trim();
        WhisperSession.LocalPlayerName = string.IsNullOrEmpty(name) ? "Player" : name;
    }

    /// <summary>Split "host" or "host:port"; an absent or unparseable port falls back
    /// to the default rather than refusing to connect.</summary>
    private (string, ushort) ParseAddress(string text)
    {
        string trimmed = text.Trim();
        if (trimmed.Length == 0)
        {
            return ("127.0.0.1", DefaultPort);
        }
        int colon = trimmed.LastIndexOf(':');
        if (colon <= 0 || colon == trimmed.Length - 1)
        {
            return (trimmed, DefaultPort);
        }
        string host = trimmed.Substring(0, colon);
        return ushort.TryParse(trimmed.Substring(colon + 1), out ushort port)
            ? (host, port)
            : (host, DefaultPort);
    }
}
