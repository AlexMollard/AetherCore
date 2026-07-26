using System;
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

    /// <summary>The socket, bone dry. Same font as the source so the two read as a pair - one brimming,
    /// one empty - which states the whole puzzle in silhouette without a legend.</summary>
    public string DryTexture = "project://assets/textures/conduit_socket.png";
    /// <summary>The socket once the line is joined: identical to the source. Filling it IS the payoff.</summary>
    public string FullTexture = "project://assets/textures/conduit_filled.png";

    private float _poll;
    private bool _live;
    private float _pulse;
    private bool _applied;

    // Ink cyan, the colour ink turns when it takes; the dry socket only ever gets a thin, cold breath
    // of it, so a lit font is unmistakably different from a waiting one.
    private static readonly Vector3 LiveLight = new(0.35f, 0.85f, 1.0f);
    private static readonly Vector3 WaitLight = new(0.30f, 0.44f, 0.58f);

    public override void OnAttach()
    {
        Apply(false);
    }

    public override void OnDetach() => Signal.Set(Channel, Self.Id, false);

    public override void OnUpdate(float deltaTime)
    {
        // The state lives in the ART now - a dry font or a brimming one - so the light only has to sell
        // it. Waiting, it breathes: a slow cold pull, like something drawing breath in the dark. Joined,
        // it burns steady and ink-coloured. Submitted per frame rather than parked on the entity so the
        // two states can differ by more than a tint.
        _pulse += deltaTime;
        Vector2 at = new(Self.Position.X, Self.Position.Y + 0.35f); // the bowl, not the foot
        if (_live)
        {
            Lighting2D.SubmitLight(at, 3.4f, LiveLight, 2.3f);
        }
        else
        {
            float breath = 0.55f + 0.45f * MathF.Sin(_pulse * 2.1f);
            Lighting2D.SubmitLight(at, 2.2f, WaitLight, 0.35f + 0.5f * breath);
        }

        _poll -= deltaTime;
        if (_poll > 0.0f) { return; }
        _poll = PollSeconds;

        AetherInk? ink = AetherInk.Instance;
        bool live = ink != null && ink.HasInkPath(
                new Vector2(SourceX, SourceY),
                new Vector2(Self.Position.X, Self.Position.Y),
                CoupleRadius);

        if (live != _live || !_applied) { Apply(live); }
    }

    private void Apply(bool live)
    {
        _live = live;
        Signal.Set(Channel, Self.Id, live);
        // Swap the whole vessel, not its colour. A stone font washed cyan reads as a lighting bug; a
        // dry font that fills with ink reads as the thing you just did.
        SpriteRenderer.SetTexture(Self, live ? FullTexture : DryTexture);
        SpriteRenderer.SetTint(Self, Vector4.One);
        _applied = true;
        if (live) { Log.Info($"[INKBOUND] Circuit '{Channel}' closed."); }
    }
}
