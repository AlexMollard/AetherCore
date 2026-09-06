using AetherCore;

namespace AetherGame;

public enum GateKind
{
    And,
    Or,
    Not,
    GreaterThan,
    LessThan,
    Latch,
}

/// <summary>
/// Wiremod's combinator: proves a node that is simultaneously a wire TARGET (A, B) and a
/// wire SOURCE (Out) - the one thing a Button or Lamp alone cannot prove. Purely data -
/// Recompute() is called by WireHub, in topological order, immediately before Out is read
/// for propagation, never by this script's own OnUpdate (there isn't one). Centralising
/// the computation in the one place that already walks the graph in the right order is
/// what makes a straight three-gate chain settle in a single WireHub.Propagate() call
/// instead of needing three frames - see WireHub's own file comment.
/// </summary>
public sealed class Gate : EntityScript
{
    public GateKind Kind;

    /// <summary>INPUT.</summary>
    public float A;

    /// <summary>INPUT. Left at its authored default if never wired - a Gate with only one
    /// input connected is a valid (if slightly pointless) single-input gate, not an
    /// error.</summary>
    public float B;

    /// <summary>Comparison point for GreaterThan/LessThan - ignored by every other Kind
    /// (including Latch, which reads B as Reset instead - see Recompute).</summary>
    public float Threshold = 0.5f;

    /// <summary>OUTPUT.</summary>
    public float Out;

    /// <summary>Recomputes Out from the current A/B/Kind. Called by WireHub - see this
    /// class's own file comment for why not from OnUpdate.</summary>
    public void Recompute()
    {
        Out = Kind switch
        {
            GateKind.And => System.MathF.Min(A, B),
            GateKind.Or => System.MathF.Max(A, B),
            GateKind.Not => 1.0f - A,
            GateKind.GreaterThan => A > Threshold ? 1.0f : 0.0f,
            GateKind.LessThan => A < Threshold ? 1.0f : 0.0f,
            // Set/Reset flip-flop - A sets, B resets, neither holds the last Out. The
            // one Kind here with actual memory (every other Kind is a pure function of
            // its inputs); this is what lets a momentary Button (Pressed is a one-frame
            // pulse, never a level - see Button's own file comment) hold a Door open
            // instead of only driving it while held. Reset-dominant when both are high
            // at once: a stuck-on Set losing the race to Reset is the safer failure mode
            // than a stuck-on Reset never being able to force the latch off.
            GateKind.Latch => B > 0.5f ? 0.0f : (A > 0.5f ? 1.0f : Out),
            _ => 0.0f,
        };
    }
}
