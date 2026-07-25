using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A socket that wants to be JOINED to a source, not stood on.
///
/// This is the second thing ink can be. A boulder puzzle asks "where does weight need to go"; a
/// conduit asks "what shape does the line have to be". Draw an unbroken run of ink from the source to
/// this socket - around corners, over gaps, through whatever the room makes awkward - and the socket
/// powers its <see cref="Channel"/>, driving the same <see cref="Gate"/>s a pressure plate would.
///
/// Set ink conducts too, so a circuit you complete and then die on stays completed. Cut the line
/// anywhere - reclaim a segment, let a live span expire - and the power drops.
/// </summary>
public sealed class InkConduit : EntityScript
{
    /// <summary>Gates listening on this name open while the circuit is closed.</summary>
    public string Channel = "conduit1";
    /// <summary>World position of the source this socket must be joined to. Set SourceX/SourceY to
    /// the matching InkSource's position (or drop one and copy its coordinates).</summary>
    public float SourceX = 0.0f;
    public float SourceY = 0.0f;
    /// <summary>How close ink must come to the socket or the source to couple with it.</summary>
    public float CoupleRadius = 1.1f;
    /// <summary>Seconds between circuit checks. The path walk is cheap but there is no reason to run
    /// it every frame.</summary>
    public float PollSeconds = 0.15f;

    private float _poll;
    private bool _live;
    private Vector4 _baseTint = Vector4.One;

    public override void OnAttach()
    {
        _baseTint = SpriteRenderer.GetTint(Self);
        Apply(false);
    }

    public override void OnDetach() => Signal.Set(Channel, Self.Id, false);

    public override void OnUpdate(float deltaTime)
    {
        _poll -= deltaTime;
        if (_poll > 0.0f) { return; }
        _poll = PollSeconds;

        AetherInk? ink = AetherInk.Instance;
        bool live = ink != null && ink.HasInkPath(
                new Vector2(SourceX, SourceY),
                new Vector2(Self.Position.X, Self.Position.Y),
                CoupleRadius);

        if (live != _live) { Apply(live); }
    }

    private void Apply(bool live)
    {
        _live = live;
        Signal.Set(Channel, Self.Id, live);
        SpriteRenderer.SetTint(Self, live ? new Vector4(0.45f, 1.0f, 1.0f, 1.0f) : _baseTint);
        if (live) { Log.Info($"[INKBOUND] Circuit '{Channel}' closed."); }
    }
}
