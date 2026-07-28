namespace AetherCore;

/// <summary>
/// The handful of session-wide choices that have to outlive a scene load: who this
/// player is, which host they asked to play on, and why the last session ended.
/// </summary>
/// <remarks>
/// <para>
/// Static because a scene load discards every entity, including the menu that
/// collected these answers. The menu writes them, the gameplay scene reads them, and
/// nothing in between survives to carry them.
/// </para>
/// <para>
/// <see cref="BeginHost"/> and <see cref="BeginJoin"/> exist so a menu cannot get the
/// pairing wrong. Starting a session and recording what kind of session it is are one
/// decision, not two - a join that forgets to record its address has no way back after
/// a dropped link, and a host that leaves a stale address behind will try to reconnect
/// to somebody else's session. Both are silent, both are only visible minutes later.
/// </para>
/// <para>
/// Nothing here loads a scene. When (and whether) to change scene is the game's
/// decision, and a menu that wants to show "Connecting..." for a moment first should be
/// able to.
/// </para>
/// </remarks>
public static class NetSession
{
    // How this reaches the engine. See IEngineBackend: the shipped value is always the
    // direct-P/Invoke backend, and it is a seam only so the transitions below can be
    // exercised without one.
    private static IEngineBackend Api => EngineBackend.Api;

    private static string s_localPlayerName = DefaultPlayerName;

    /// <summary>What a blank name becomes.</summary>
    public const string DefaultPlayerName = "Player";

    /// <summary>
    /// The name this player chose. Assigning trims it, and substitutes
    /// <see cref="DefaultPlayerName"/> for a blank one, so a caller never has to.
    /// </summary>
    /// <remarks>
    /// This is the DESIRED name, not necessarily the one in use: a player is renamed to
    /// "Alice (2)" by <see cref="Net.ClaimPlayerName"/> when somebody ahead of them is
    /// already Alice. Read the name off the player entity, not from here, when what you
    /// want is the name on screen.
    /// </remarks>
    public static string LocalPlayerName
    {
        get => s_localPlayerName;
        set
        {
            string trimmed = value is null ? string.Empty : value.Trim();
            s_localPlayerName = trimmed.Length == 0 ? DefaultPlayerName : trimmed;
        }
    }

    /// <summary>
    /// Why the last session ended, for a menu to show after an involuntary return.
    /// Empty when the menu was reached on purpose.
    /// </summary>
    /// <remarks>Read it with <see cref="TakeStatusMessage"/> rather than directly, so a
    /// later voluntary visit is not still apologising for a session two ago.</remarks>
    public static string StatusMessage = string.Empty;

    /// <summary>The host this player last asked to join, so a dropped link knows where
    /// to try coming back to. Empty when this player is hosting, which is what makes
    /// "there is nowhere to reconnect to" answerable.</summary>
    public static string HostAddress = string.Empty;

    /// <summary>Port half of <see cref="HostAddress"/>.</summary>
    public static ushort HostPort;

    /// <summary>
    /// True from the moment a join is requested until the session is left.
    /// </summary>
    /// <remarks>
    /// It exists for one case no engine state can express: a join REFUSED before the
    /// gameplay scene's first tick. The framework tears the session down as soon as the
    /// refusal lands, so <see cref="Net.IsClient"/> is already false and the scene would
    /// otherwise look exactly like a single-player session and spawn a solo player into
    /// a server it was just thrown out of.
    /// </remarks>
    public static bool JoinRequested;

    /// <summary>Read <see cref="StatusMessage"/> and clear it in one step.</summary>
    /// <returns>The message, or an empty string when there is nothing to explain.</returns>
    public static string TakeStatusMessage()
    {
        string message = StatusMessage;
        StatusMessage = string.Empty;
        return message;
    }

    /// <summary>
    /// Start hosting on <paramref name="port"/>, and record that this player is the
    /// host and so has nowhere to reconnect to.
    /// </summary>
    /// <param name="port">UDP port to listen on.</param>
    /// <param name="maxConnections">How many CLIENTS to accept. The host is not one of
    /// its own, so an N-player game hosts with N-1 - see <see cref="Net.Host"/>.</param>
    /// <returns>False if the port could not be opened; see <see cref="Net.LastError"/>.</returns>
    public static bool BeginHost(int port, int maxConnections)
    {
        if (!Api.NetHost(port, maxConnections))
        {
            return false;
        }
        JoinRequested = false;
        HostAddress = string.Empty;
        HostPort = 0;
        return true;
    }

    /// <summary>
    /// Begin connecting to <paramref name="address"/>, and record where to come back to
    /// if the link later drops without explanation.
    /// </summary>
    /// <returns>False only if the address could not be resolved or a socket could not be
    /// opened. A true return means the attempt STARTED - see <see cref="Net.Connect"/>.</returns>
    public static bool BeginJoin(string address, ushort port)
    {
        if (!Api.NetConnect(address, port))
        {
            return false;
        }
        JoinRequested = true;
        HostAddress = address;
        HostPort = port;
        return true;
    }

    /// <summary>
    /// Split a typed "host" or "host:port" into its two halves, falling back to
    /// <paramref name="defaultPort"/> rather than refusing to connect.
    /// </summary>
    /// <remarks>
    /// An empty string means the loopback address, so a player testing two processes on
    /// one machine can leave the field blank. A port that will not parse is treated as
    /// absent, on the grounds that a typo in the port is not a reason to reject a
    /// perfectly good address.
    /// </remarks>
    public static (string Address, ushort Port) ParseAddress(string text, ushort defaultPort)
    {
        string trimmed = text is null ? string.Empty : text.Trim();
        if (trimmed.Length == 0)
        {
            return ("127.0.0.1", defaultPort);
        }
        int colon = trimmed.LastIndexOf(':');
        if (colon <= 0 || colon == trimmed.Length - 1)
        {
            return (trimmed, defaultPort);
        }
        string host = trimmed.Substring(0, colon);
        return ushort.TryParse(trimmed.Substring(colon + 1), out ushort port)
            ? (host, port)
            : (host, defaultPort);
    }
}
