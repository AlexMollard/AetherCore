using AetherCore;

namespace AetherGame;

/// <summary>Title screen controller. Stub — behavior authored in Phase 2.</summary>
public sealed class TitleScreen : EntityScript, IMenuScreen
{
    public override void OnAttach() => ScreenRegistry<TitleScreen>.Register("TitleRoot", this);
    public void OnShown() { }
    public void HandleInput() { }
}
