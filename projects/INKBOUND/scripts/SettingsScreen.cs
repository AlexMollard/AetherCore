using AetherCore;

namespace AetherGame;

/// <summary>Settings ("attune") controller. Stub — behavior authored in Phase 4.</summary>
public sealed class SettingsScreen : EntityScript, IMenuScreen
{
    public override void OnAttach() => ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
    public void OnShown() { }
    public void HandleInput() { }
}
