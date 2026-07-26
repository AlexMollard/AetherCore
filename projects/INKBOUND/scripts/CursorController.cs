using AetherCore;

namespace AetherGame;

/// <summary>
/// Decides when INKBOUND has a pointer at all.
///
/// The pointer itself is the engine's: it is turned on and given its arrow under <c>[cursor]</c> in
/// ProjectSettings.toml, and drawn over everything without an entity or a canvas of our own. All this
/// script does is answer one question - is there anything for a pointer to do right now?
///
/// While ink can be drawn, there isn't. The anchor guide already marks the cursor: a drop hanging at
/// the point when ink will hold, beads scattering off it when it won't. That IS the pointer, and it
/// says more than an arrow could. Drawing an arrow on top of it would be two pointers fighting over
/// one pixel, so the engine one steps aside and comes back the moment the ink verb does - a dialogue,
/// the pause menu, the end of a level - where there are things to click and no guide to click them
/// with.
/// </summary>
public sealed class CursorController : EntityScript
{
    private bool _hidden;
    private bool _applied;

    public override void OnAttach()
    {
        // Nothing is kept alive across scenes: the pointer is engine state, not an object, so the last
        // level's controller leaves nothing behind for this one to trip over.
        _applied = false;
    }

    public override void OnDetach()
    {
        // Whatever happens next - the menu, the editor, a crash out of play mode - gets a pointer back.
        Cursor.Visible = true;
    }

    public override void OnUpdate(float deltaTime)
    {
        // The same question AetherInk asks to decide whether the ink verb is live, asked the same way:
        // the clock. Dialogue and pause both stop it, and both are exactly when the arrow is wanted.
        bool inkIsTheCursor = AetherInk.Instance != null && !Time.IsPaused && !GameState.Won;
        if (inkIsTheCursor == _hidden && _applied)
        {
            return;
        }
        _hidden = inkIsTheCursor;
        _applied = true;
        Cursor.Visible = !inkIsTheCursor;
    }
}
