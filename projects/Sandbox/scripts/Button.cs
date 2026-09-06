using AetherCore;

namespace AetherGame;

/// <summary>
/// Wiremod's simplest source: a momentary output, 1.0 for exactly one frame after being
/// pressed and 0.0 otherwise - a rising edge, not a toggle. Pressed is a plain public
/// field; this script has no idea a wire, or WireHub, exists at all (see WireHub's own
/// file comment on why device scripts stay ignorant of the graph on top of them).
///
/// Interact() is called by ToolGun's own interact raycast, not read from Input
/// directly here - the raycast, range check, and authority gate already live in one place
/// (ToolGun), shared by every interactable, instead of every device re-deriving them.
/// </summary>
public sealed class Button : EntityScript
{
    /// <summary>OUTPUT.</summary>
    public float Pressed;

    private WireHub? _hub;
    private bool _pendingDecay;

    /// <summary>Resolved lazily, not in OnAttach: WireHub is an ordinary scene entity
    /// with its own attach order, and this script's OnAttach can run before WireHub's
    /// own C# instance exists yet (confirmed live - Scene.Find("WireHub") finds the
    /// entity immediately since that is a plain ECS lookup, but GetScript&lt;WireHub&gt;()
    /// raced it and returned null, silently no-opping every press through the ?.
    /// operator). Resolving on first actual use instead of at attach time means it
    /// always finds WireHub once anything has actually happened this scene, regardless
    /// of which of the two attached first.</summary>
    private WireHub? Hub => _hub ??= Scene.Find("WireHub").GetScript<WireHub>();

    public override void OnUpdate(float deltaTime)
    {
        if (!_pendingDecay)
        {
            return;
        }
        // Decay back to 0 the frame after the press, so this reads as a pulse rather
        // than something that stays "on" until pressed again.
        Pressed = 0.0f;
        _pendingDecay = false;
        Hub?.Propagate();
    }

    public void Interact()
    {
        Pressed = 1.0f;
        _pendingDecay = true;
        // Push, not just poll: propagate right now rather than waiting for WireHub's own
        // OnUpdate to happen to run this frame (see WireHub's own file comment on why).
        Hub?.Propagate();
    }
}
