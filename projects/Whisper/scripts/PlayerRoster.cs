using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The "who is in this session" panel in the arena's top-right corner: one row per
/// player, in that player's colour, with their link quality beside the name.
/// </summary>
/// <remarks>
/// <para>
/// Reads <see cref="NetSessionDirector.Players"/> and keeps NOTHING of its own. The
/// director already rebuilds that list from the players this peer is holding, on the host
/// and on a client alike, so a second copy here would only be something that could fall
/// out of step with the characters actually on screen - which for a list whose entire job
/// is to tell you who is on screen would be worse than not having it.
/// </para>
/// <para>
/// The colour is the same <see cref="PlayerPalette"/> entry the character and the name
/// tag wear, looked up through that player's own <see cref="NetPlayerSync"/> rather than
/// recomputed here, so a row cannot name one colour while the character wears another.
/// </para>
/// <para>
/// The row elements are created once and REUSED - text and colour rewritten, unused rows
/// switched off - rather than rebuilt when the roster changes. Creating and destroying UI
/// entities on join and leave would churn the canvas on exactly the frames the session is
/// already busiest.
/// </para>
/// <para>
/// Layout: anchored to the screen's top-right corner, which in UI space is (1, 0) - Y=0
/// is the TOP - with a top-right pivot so the offsets below are a straight stack down the
/// right-hand edge. All text is ASCII: the font pipeline bakes nothing else.
/// </para>
/// </remarks>
public sealed class PlayerRoster : EntityScript
{
    /// <summary>How many rows exist. Matches the session cap; a session somehow larger
    /// shows the first this many rather than growing the panel off the screen.</summary>
    public const int MaxRows = WhisperSession.MaxPlayers;

    /// <summary>Gap from the screen's top and right edges, in pixels.</summary>
    public float Margin = 16.0f;

    /// <summary>Row width, in pixels. Wide enough for the longest row the columns
    /// below can produce, at the monospaced font this HUD uses.</summary>
    public float Width = 330.0f;

    /// <summary>Vertical space per row, in pixels.</summary>
    public float LineHeight = 22.0f;

    /// <summary>Row text size, in pixels.</summary>
    public float FontSize = 16.0f;

    /// <summary>Header text size, in pixels.</summary>
    public float HeaderFontSize = 14.0f;

    /// <summary>Longest name shown before it is clipped. The font is monospaced, so a
    /// fixed column is what lets the ping line up without a second element per row.</summary>
    public int NameColumn = 12;

    private static readonly Vector4 HeaderColor = new(0.478f, 0.518f, 0.596f, 1.0f);

    private Entity _header;
    private readonly Entity[] _rows = new Entity[MaxRows];

    /// <inheritdoc/>
    public override void OnAttach()
    {
        Vector2 topRight = new(1.0f, 0.0f);

        _header = Ui.CreateText();
        StyleRow(_header, topRight, Margin, HeaderFontSize);
        Ui.SetTextColor(_header, HeaderColor);

        for (int i = 0; i < _rows.Length; i++)
        {
            _rows[i] = Ui.CreateText();
            StyleRow(_rows[i], topRight, Margin + LineHeight * (i + 1), FontSize);
            _rows[i].SetActive(false);
        }
    }

    /// <inheritdoc/>
    public override void OnDetach()
    {
        foreach (Entity row in _rows)
        {
            if (row.IsValid)
            {
                row.Destroy();
            }
        }
        if (_header.IsValid)
        {
            _header.Destroy();
        }
    }

    /// <inheritdoc/>
    public override void OnUpdate(float deltaTime)
    {
        if (GetScript<WhisperSession>() is not { } session)
        {
            return;
        }

        IReadOnlyList<NetSessionPlayer> players = session.Players;
        // The occupancy keeps the header's own column rather than getting a row of its
        // own: "how full is this session" is the one fact about the roster that is not
        // about any single player, and it reads where the names begin. The three column
        // captions sit over the three columns the rows below actually print.
        string occupancy = $"PLAYERS {players.Count}/{WhisperSession.MaxPlayers}";
        Ui.SetText(_header, $"{Fit(occupancy)} {"HP",3} {"K/D",5} {"PING",5}");

        for (int i = 0; i < _rows.Length; i++)
        {
            bool used = i < players.Count;
            _rows[i].SetActive(used);
            if (!used)
            {
                continue;
            }
            NetSessionPlayer player = players[i];
            Ui.SetText(_rows[i], $"{Fit(player.Name)} {Health(player)} {Score(player)} {Latency(player)}");
            Ui.SetTextColor(_rows[i], ColorOf(player));
        }
    }

    /// <summary>That player's replicated health, or blank while its combat script has
    /// not been created yet - a frame or two after a join.</summary>
    /// <remarks>
    /// Read straight off <see cref="PlayerCombat.Health"/> on the entity, which on
    /// every peer but that player's owner is the value replication delivered. This row
    /// is therefore a readout of the replicated field itself, not of anything derived
    /// from what the character looks like.
    /// </remarks>
    private static string Health(NetSessionPlayer player)
        => player.Entity.GetScript<PlayerCombat>() is { } combat ? $"{combat.Health,3}" : "   ";

    /// <summary>Kills and deaths, in the same column on every row.</summary>
    private static string Score(NetSessionPlayer player)
        => player.Entity.GetScript<PlayerCombat>() is { } combat ? $"{combat.Kills,2}/{combat.Deaths,-2}" : "     ";

    /// <summary>That player's own colour, straight off the character it belongs to.
    /// Falls back to the palette entry for its connection only while the character's
    /// script has not been created yet, which is a frame or two after a join.</summary>
    private static Vector4 ColorOf(NetSessionPlayer player)
        => player.Entity.GetScript<NetPlayerSync>() is { } sync
            ? sync.Color
            : PlayerPalette.At((int) (player.Connection % (uint) PlayerPalette.Count));

    /// <summary>What to show where a ping would go.</summary>
    /// <remarks>
    /// The host is labelled rather than given a number, because the number would be 0 and
    /// "0 ms" reads as a suspiciously good connection rather than as "this is the machine
    /// everyone else's latency is measured against". A client whose first acknowledgement
    /// has not landed yet also reads 0, and is shown as still being measured rather than
    /// as instantaneous.
    /// </remarks>
    private static string Latency(NetSessionPlayer player)
    {
        if (player.IsHost)
        {
            return " HOST";
        }
        uint ping = player.PingMs;
        return ping == 0 ? "   --" : $"{ping,3}ms";
    }

    /// <summary>Pad or clip a name to the fixed column the ping lines up after.</summary>
    private string Fit(string name)
        => name.Length > NameColumn ? name.Substring(0, NameColumn) : name.PadRight(NameColumn);

    private void StyleRow(Entity element, Vector2 anchor, float top, float fontSize)
    {
        Ui.SetAnchors(element, anchor, anchor);
        Ui.SetPivot(element, anchor);
        Ui.SetRect(element, -Margin, top, Width, LineHeight);
        Ui.SetFont(element, "IBMPlexMono-Italic");
        Ui.SetFontSize(element, fontSize);
        Ui.SetTextWrap(element, false);
        Ui.SetTextAlign(element, UiHAlign.Right, UiVAlign.Middle);
    }
}
