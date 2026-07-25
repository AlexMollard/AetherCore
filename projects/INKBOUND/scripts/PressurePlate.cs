using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A stone pad that stays pressed while something heavy rests on it, and drives every
/// <see cref="Gate"/> sharing its <see cref="Channel"/>.
///
/// The point of the mechanic is that INK CANNOT HOLD IT. Ink expires after a few seconds, so a
/// drawn slab is no substitute for real weight - you have to get a boulder onto the plate, which is
/// what turns "draw a floor" into "shape a path for something else to travel". By default the
/// player's own weight does not count either, so you cannot simply stand there and be your own key.
/// </summary>
public sealed class PressurePlate : EntityScript
{
    /// <summary>Gates listening on this name open while the plate is held down.</summary>
    public string Channel = "gate1";
    /// <summary>Let the player's own weight hold the plate. Off by default - the puzzle is usually
    /// "the plate is over there and so are you", and standing on it must not be the answer.</summary>
    public bool AllowPlayer = false;

    /// <summary>Is anything currently holding this channel down? (Kept for readability at call
    /// sites; the state itself lives on the shared <see cref="Signal"/> bus.)</summary>
    public static bool IsHeld(string channel) => Signal.IsOn(channel);

    private readonly HashSet<uint> _resting = new();
    private Vector3 _up;
    private bool _wasHeld;

    public override void OnAttach()
    {
        Physics2D.SetTrigger(Self, true);
        Physics2D.EnableEvents(Self);
        _up = Self.Position;
    }

    public override void OnDetach()
    {
        // Drop this plate's contribution so a scene change cannot leave a channel stuck open.
        Signal.Set(Channel, Self.Id, false);
        _resting.Clear();
    }

    private bool Qualifies(Entity e)
        => Boulder.IsBoulder(e.Id) || (AllowPlayer && PlayerController.Instance != null && e.Id == PlayerController.Instance.Self.Id);

    public override void OnTriggerEnter2D(Entity other)
    {
        if (!Qualifies(other) || !_resting.Add(other.Id)) { return; }
        Signal.Set(Channel, Self.Id, true);
    }

    public override void OnTriggerExit2D(Entity other)
    {
        if (!_resting.Remove(other.Id)) { return; }
        if (_resting.Count == 0) { Signal.Set(Channel, Self.Id, false); }
    }

    public override void OnUpdate(float deltaTime)
    {
        bool held = _resting.Count > 0;
        if (held == _wasHeld) { return; }
        _wasHeld = held;

        // Sink the pad a little and light its seam, so the state is readable from across the room.
        Self.Position = held ? new Vector3(_up.X, _up.Y - 0.07f, _up.Z) : _up;
        SpriteRenderer.SetTint(Self, held ? new Vector4(0.55f, 1.0f, 1.0f, 1.0f) : Vector4.One);
        Log.Info($"[INKBOUND] Plate '{Channel}' {(held ? "pressed" : "released")}.");
    }
}
