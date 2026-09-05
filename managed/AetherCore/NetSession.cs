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
    /// "there is nowhere to reconnect to" answerable. Empty too when the join was by
    /// room code instead of a typed address - see <see cref="HostRoomCode"/>, its
    /// mutually exclusive counterpart.</summary>
    public static string HostAddress = string.Empty;

    /// <summary>Port half of <see cref="HostAddress"/>.</summary>
    public static ushort HostPort;

    /// <summary>The room code this player last asked to join, so a dropped link can be
    /// rejoined by the same code instead of by address. Empty when this player is
    /// hosting or joined by a typed address instead - see <see cref="HostAddress"/>,
    /// its mutually exclusive counterpart.</summary>
    /// <remarks>
    /// A code identifies the SESSION, not an endpoint, so it survives exactly the
    /// change a stored address does not: the far end's real address moving behind its
    /// NAT, or simply not being known yet when this join began. That is the whole
    /// reason a room-code join needs its own reconnect target rather than reusing
    /// <see cref="HostAddress"/> for whatever endpoint the punch happened to find.
    /// </remarks>
    public static string HostRoomCode = string.Empty;

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
        HostRoomCode = string.Empty;
        return true;
    }

    /// <summary>
    /// Host a session that needs no forwarded port: mint or accept a room code and
    /// start listening through <see cref="Net.HostWithCode"/>, recording that this
    /// player is the host and so has nowhere to reconnect to - exactly
    /// <see cref="BeginHost"/>'s contract, run over the connect ladder instead of a
    /// bound port.
    /// </summary>
    /// <param name="code">A code to host under - e.g. one a player was handed by
    /// somebody re-hosting after a drop. Null or empty mints a fresh one with
    /// <see cref="Net.NewRoomCode"/>.</param>
    /// <param name="port">Local UDP port to bind. 0 lets the OS choose - see
    /// <see cref="Net.HostWithCode"/> for why there is normally no reason to pin
    /// it.</param>
    /// <param name="maxConnections">Same meaning as on <see cref="BeginHost"/>.</param>
    /// <remarks>
    /// <para>
    /// <b>Signalling is deliberately not chosen here.</b> Candidates already publish
    /// over LAN broadcast with no call at all - the connect ladder's own
    /// zero-configuration default, see <see cref="Net.UseLanSignaling"/> - so a
    /// same-network game needs nothing else. Reaching a peer across the internet needs
    /// a rendezvous address: call <see cref="Net.UseRendezvousSignaling"/> once,
    /// before this. This method never calls either itself, on purpose - an implicit
    /// choice here would be an explicit selection that silently and permanently
    /// overrides a signalling backend a settings screen configured, the same way a
    /// direct <see cref="Net.UseLanSignaling"/> call would. "Calls neither" is not a
    /// misconfiguration to guard against: it is exactly what a LAN-only game is
    /// supposed to do, and it already works.
    /// </para>
    /// <para>
    /// Progress and failure live on <see cref="Net"/>, not duplicated here: poll
    /// <see cref="Net.TraversalState"/>, and once it reaches
    /// <see cref="NetTraversalState.Failed"/> read <see cref="Net.TraversalError"/> -
    /// typically a symmetric NAT with no relay configured, see
    /// <see cref="Net.ConfigureRelay"/>.
    /// </para>
    /// </remarks>
    /// <returns>The code hosted under - whatever was supplied, or minted when
    /// <paramref name="code"/> was null or empty - paired with whether the local bind
    /// even started. A false <c>Started</c> leaves every reconnect field untouched,
    /// the same as a failed <see cref="BeginHost"/>.</returns>
    public static (bool Started, string Code) BeginHostWithCode(string? code = null, int port = 0,
        int maxConnections = 32)
    {
        string roomCode = string.IsNullOrEmpty(code) ? Api.NetNewRoomCode() : code;
        if (!Api.NetHostWithCode(roomCode, port, maxConnections))
        {
            return (false, roomCode);
        }
        JoinRequested = false;
        HostAddress = string.Empty;
        HostPort = 0;
        HostRoomCode = string.Empty;
        return (true, roomCode);
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
        HostRoomCode = string.Empty;
        return true;
    }

    /// <summary>
    /// Join the session published under <paramref name="code"/> - fetching the host's
    /// candidates, punching a hole to them, and connecting through the result - and
    /// record the CODE, not an address, as where to come back to if the link later
    /// drops without explanation.
    /// </summary>
    /// <remarks>
    /// <para>
    /// A room code rather than <see cref="HostAddress"/>/<see cref="HostPort"/>: the
    /// whole reason this join needed a code instead of a typed address is that the
    /// host's real address either sits behind a NAT or was not known ahead of time, so
    /// an endpoint captured at join time could easily be stale by the time a reconnect
    /// needs it. The code names the SESSION and survives exactly the kind of change an
    /// address does not - see <see cref="HostRoomCode"/>.
    /// </para>
    /// <para>
    /// Same signalling rule as <see cref="BeginHostWithCode"/>: candidates already
    /// exchange over LAN broadcast with no call needed; call
    /// <see cref="Net.UseRendezvousSignaling"/> once beforehand for a peer across the
    /// internet. This method makes no signalling choice on its own.
    /// </para>
    /// </remarks>
    /// <returns>False if the local bind failed or <paramref name="code"/> is not a
    /// well-formed room code - see <see cref="Net.JoinByCode"/>. A true return means
    /// traversal has STARTED, not that it will succeed - poll
    /// <see cref="Net.TraversalState"/> the same way <see cref="BeginHostWithCode"/>
    /// says to.</returns>
    public static bool BeginJoinByCode(string code)
    {
        if (!Api.NetJoinByCode(code))
        {
            return false;
        }
        JoinRequested = true;
        HostAddress = string.Empty;
        HostPort = 0;
        HostRoomCode = code;
        return true;
    }

    /// <summary>
    /// Split a typed address into a host and a port, falling back to
    /// <paramref name="defaultPort"/> rather than refusing to connect.
    /// </summary>
    /// <remarks>
    /// <para>
    /// An empty string means the loopback address, so a player testing two processes on
    /// one machine can leave the field blank. A port that will not parse is treated as
    /// absent, on the grounds that a typo in the port is not a reason to reject a
    /// perfectly good address.
    /// </para>
    /// <para>
    /// IPv6 literals use colons for the address itself, so "split on the last colon"
    /// - correct for everything else - would carve one apart (<c>::1</c> becomes host
    /// <c>:</c>, port <c>1</c>). The discriminator is colon COUNT, not position: a bare,
    /// unbracketed literal (<c>::1</c>, <c>fe80::1</c>) always has more than one colon,
    /// so more than one colon with no brackets means there is no port to split off, and
    /// the text comes back whole with <paramref name="defaultPort"/>. A bracketed
    /// literal (<c>[::1]:7777</c>) is unambiguous - it is split at the closing bracket
    /// instead, with the brackets stripped from the returned host, and <c>[::1]</c> with
    /// nothing after the bracket also takes <paramref name="defaultPort"/>.
    /// </para>
    /// <para>
    /// A malformed bracketed form is not an error, on the same grounds as the port typo
    /// above: a missing closing bracket (<c>[::1</c>) is kept whole as the host, and a
    /// closing bracket followed by junk that is not a valid <c>:port</c> (<c>[::1]:x</c>)
    /// keeps the bracketed host and falls back to <paramref name="defaultPort"/>. Text
    /// that never opens a bracket in the first place (<c>]:7777</c>) does not engage this
    /// path at all and is read as ordinary "host:port" whose host happens to contain
    /// <c>]</c>.
    /// </para>
    /// <para>
    /// A zone id (<c>fe80::1%eth0</c>) is out of scope: it is not stripped or otherwise
    /// special-cased, so it rides along as part of the host on whichever path the literal
    /// takes.
    /// </para>
    /// </remarks>
    public static (string Address, ushort Port) ParseAddress(string text, ushort defaultPort)
    {
        string trimmed = text is null ? string.Empty : text.Trim();
        if (trimmed.Length == 0)
        {
            return ("127.0.0.1", defaultPort);
        }

        if (trimmed[0] == '[')
        {
            return ParseBracketedAddress(trimmed, defaultPort);
        }

        int firstColon = trimmed.IndexOf(':');
        int lastColon = trimmed.LastIndexOf(':');
        if (firstColon >= 0 && firstColon != lastColon)
        {
            // More than one colon with no brackets: a bare IPv6 literal, not a
            // "host:port" pair - there is no port to split off.
            return (trimmed, defaultPort);
        }

        if (lastColon <= 0 || lastColon == trimmed.Length - 1)
        {
            return (trimmed, defaultPort);
        }
        string host = trimmed.Substring(0, lastColon);
        return ushort.TryParse(trimmed.Substring(lastColon + 1), out ushort port)
            ? (host, port)
            : (host, defaultPort);
    }

    /// <summary>Split a "[host]" or "[host]:port" literal, called once <see
    /// cref="ParseAddress"/> has seen the leading bracket.</summary>
    private static (string Address, ushort Port) ParseBracketedAddress(string trimmed, ushort defaultPort)
    {
        int close = trimmed.IndexOf(']');
        if (close < 0)
        {
            // No closing bracket - cannot be split, so it is kept whole, the same as any
            // other address this method cannot make sense of.
            return (trimmed, defaultPort);
        }

        string host = trimmed.Substring(1, close - 1);
        string afterBracket = trimmed.Substring(close + 1);
        if (afterBracket.Length >= 2 && afterBracket[0] == ':'
            && ushort.TryParse(afterBracket.Substring(1), out ushort port))
        {
            return (host, port);
        }
        // Nothing after the bracket, or something that is not a valid ":port" - same
        // rule as the unbracketed form: a bad or missing port does not cost the host.
        return (host, defaultPort);
    }
}
