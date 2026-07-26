using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The game's own pointer, everywhere.
///
/// The OS arrow was a visitor from another program sitting on top of a cave drawn entirely out of wet
/// black ink, and it was doing the most important job on screen - telling you where the ink lands.
/// So it is switched off and this draws the pointer instead, in the game's own hand:
///
///   nib   - while you can draw. A quill, tip on the exact point ink will start, so the anchor guide's
///           hanging drop reads as ink about to fall off the pen rather than as a separate marker.
///   arrow - everywhere else (title, slot select, level select, pause, dialogue): a pointer poured out
///           of ink, with the same cold wet edge the ink itself carries.
///
/// One instance serves the whole game (DontDestroyOnLoad), because a pointer that blinks out and
/// respawns on every scene load is worse than no pointer at all. It restores the OS cursor on detach,
/// so quitting to the editor never leaves you without one.
/// </summary>
public sealed class CursorController : EntityScript
{
    /// <summary>On-screen size in pixels. The art is authored at 32 and drawn a little larger: the UI
    /// samples linearly and has no pixel-art mode, so this is as far as it can be stretched before the
    /// edges start to soften. In the editor the game view is scaled down inside the viewport, so it
    /// reads smaller there than it will in the shipped game.</summary>
    public float Size = 40.0f;

    private Entity _canvas;
    private Entity _image;
    private bool _showingNib;
    private bool _haveShown;
    private bool _owner;

    // The pointer has to exist whether you came in through the menu or straight into a level from the
    // editor, so it is attached in both places - and then only the first one to arrive does any work.
    // A surviving instance from the previous scene always wins; the newcomer stands down rather than
    // fighting it for the OS cursor.
    private static CursorController? s_live;

    public override void OnAttach()
    {
        if (s_live != null && s_live != this)
        {
            return; // a live pointer already carried over from the last scene
        }
        s_live = this;
        _owner = true;
        Self.DontDestroyOnLoad();
        Input.OsCursorVisible = false;
    }

    public override void OnDetach()
    {
        if (!_owner)
        {
            return;
        }
        s_live = null;
        // Never strand the player - or the editor - without a pointer.
        Input.OsCursorVisible = true;
    }

    private void EnsureCursor()
    {
        if (_image.IsValid)
        {
            return;
        }

        // Its own canvas, not the HUD's: the pointer has to outlive and out-rank every screen it is
        // drawn over, including the ones (title, level select) that have no HUD at all.
        _canvas = Ui.CreateCanvas();
        _canvas.DontDestroyOnLoad();
        _image = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_image, Vector2.Zero, Vector2.Zero); // top-left origin; y grows downward
        Ui.SetPivot(_image, Vector2.Zero);
        Ui.SetImageColor(_image, Vector4.One);
        _haveShown = false;
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_owner)
        {
            return;
        }

        EnsureCursor();
        if (!_image.IsValid)
        {
            return;
        }

        // The nib means "this is a drawing tool and it is live". That is exactly the condition the ink
        // verb runs under, so it is asked the same way AetherInk asks it - the clock - rather than by
        // keeping a second list of what counts as gameplay.
        bool canDraw = AetherInk.Instance != null && !Time.IsPaused && !GameState.Won;
        if (canDraw != _showingNib || !_haveShown)
        {
            _showingNib = canDraw;
            _haveShown = true;
            Ui.SetImageTexture(_image, canDraw
                ? "project://assets/textures/ui/cursor_nib.png"
                : "project://assets/textures/ui/cursor_ink.png");
        }

        // Put the art's HOTSPOT on the mouse, not its corner. The arrow points from its top-left, the
        // nib writes with its bottom-left, so the nib hangs up and right of the point it marks.
        Vector2 m = Input.MousePosition;
        float y = canDraw ? m.Y - Size : m.Y;
        Ui.SetRect(_image, m.X, y, Size, Size);
    }
}
