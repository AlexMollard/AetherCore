using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Drives the shared menu atmosphere: rolls the ui_void_bg backdrop's shader time so the
/// void keeps roiling. The watching eyes now live inside the backdrop shader itself (procedural,
/// pixel-art), so there are no separate eye entities to animate anymore.</summary>
public sealed class MenuShell : EntityScript
{
    private Entity _bg;
    private float _t;

    public override void OnAttach()
    {
        _bg = Scene.Find("MenuBg"); // the ui_void_bg backdrop; we drive its shader time
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        // Roll the background shader's time (params.x); intensity defaults to full.
        if (_bg.IsValid) Ui.SetEffectParams(_bg, new Vector4(_t, 0f, 0f, 0f));
    }
}
