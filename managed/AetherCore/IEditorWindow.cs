namespace AetherCore;

/// <summary>Implement on any class in a project (or the SDK) to add a window to the editor. The editor
/// discovers implementers by reflection (like EntityScript) and calls OnGui() each editor frame; draw
/// with the EditorGui.* API. Editor/dev-tooling only - never invoked in a shipped game.</summary>
public interface IEditorWindow
{
    /// <summary>Window title (also its ImGui id). Keep it stable + unique.</summary>
    string Title { get; }

    /// <summary>Called once per editor frame. Do your own EditorGui.Begin(Title, ...) / End here.</summary>
    void OnGui();
}
