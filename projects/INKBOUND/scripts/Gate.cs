using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A slab that sinks into the floor while its <see cref="Channel"/> is asserted, and grinds back up
/// when it is dropped. Anything can drive it - a <see cref="PressurePlate"/> under weight, an
/// <see cref="InkConduit"/> whose circuit you closed - so one door can have several answers.
///
/// Kinematic, not static: a kinematic 2D body is driven by writing its transform, which is what lets
/// it carry the player and shove the world around as it moves. Put one where the player cannot simply
/// draw a bridge over it (under an overhang) and it becomes a real gate rather than a suggestion.
/// </summary>
public sealed class Gate : EntityScript
{
    /// <summary>Plates broadcasting on this name drive this gate.</summary>
    public string Channel = "gate1";
    /// <summary>How far it drops when open, in world units. Roughly its own height.</summary>
    public float OpenDrop = 3.0f;
    /// <summary>Seconds to travel between shut and open.</summary>
    public float MoveSeconds = 0.55f;

    private Vector3 _shut;
    private float _t; // 0 = shut, 1 = open

    public override void OnAttach()
    {
        _shut = Self.Position;
        // Start settled in whatever state the channel is already in, so a reload does not animate.
        _t = Signal.IsOn(Channel) ? 1.0f : 0.0f;
        Apply();
    }

    public override void OnUpdate(float deltaTime)
    {
        float target = Signal.IsOn(Channel) ? 1.0f : 0.0f;
        if (Math.Abs(target - _t) < 0.0001f) { return; }

        float step = MoveSeconds > 0.0f ? deltaTime / MoveSeconds : 1.0f;
        _t = target > _t ? Math.Min(target, _t + step) : Math.Max(target, _t - step);
        Apply();
    }

    private void Apply()
    {
        // Ease so the slab settles rather than snapping.
        float e = _t * _t * (3.0f - 2.0f * _t);
        Self.Position = new Vector3(_shut.X, _shut.Y - OpenDrop * e, _shut.Z);
    }
}
