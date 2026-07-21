using AetherCore;

namespace AetherGame;

/// <summary>Level select controller. Stub — behavior authored in Phase 3.</summary>
public sealed class LevelSelectScreen : EntityScript, IMenuScreen
{
    public override void OnAttach() => ScreenRegistry<LevelSelectScreen>.Register("LevelSelectRoot", this);
    public void OnShown() { }
    public void HandleInput() { }
}
