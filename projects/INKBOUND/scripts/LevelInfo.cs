using AetherCore;

namespace AetherGame;

/// <summary>Authored once per level scene. Tells GameState which level key is running so completion,
/// checkpoints and best-times record against the right save slot. Empty Key = untracked (Sandbox).</summary>
public sealed class LevelInfo : EntityScript
{
    public string Key = "";

    public override void OnAttach() => GameState.CurrentLevel = Key;
}
