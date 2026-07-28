using System.Numerics;

namespace AetherGame;

/// <summary>
/// The colours Whisper tells players apart by, and the one place that decides which
/// player gets which.
/// </summary>
/// <remarks>
/// <para>
/// A single table shared by everything that has to agree: the character tint, the name
/// tag above it, and the roster row in the corner. Two of those looking up the same
/// player and disagreeing would be worse than no colour at all - the player would be
/// hunting for a green character while the list said blue - so there is one lookup and
/// no second copy of the numbers.
/// </para>
/// <para>
/// Chosen to stay apart for the common colour-vision deficiencies (blue / amber / pink /
/// green rather than the reflexive red / green / blue / yellow), and kept light enough
/// to read as a text colour over the arena's dark tiles as well as to tint a sprite.
/// </para>
/// </remarks>
public static class PlayerPalette
{
    private static readonly Vector4[] Colors =
    [
        new(0.451f, 0.729f, 1.000f, 1.0f), // sky
        new(1.000f, 0.729f, 0.310f, 1.0f), // amber
        new(1.000f, 0.510f, 0.694f, 1.0f), // rose
        new(0.545f, 0.906f, 0.596f, 1.0f), // mint
        new(0.780f, 0.667f, 1.000f, 1.0f), // violet
        new(0.427f, 0.902f, 0.878f, 1.0f), // teal
    ];

    /// <summary>How many distinct colours there are. An index past this wraps.</summary>
    public static int Count => Colors.Length;

    /// <summary>The colour for <paramref name="index"/>, wrapping rather than throwing:
    /// a session bigger than the palette should look repetitive, not crash.</summary>
    public static Vector4 At(int index)
    {
        int wrapped = index % Colors.Length;
        return Colors[wrapped < 0 ? wrapped + Colors.Length : wrapped];
    }
}
