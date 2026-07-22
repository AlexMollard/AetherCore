namespace AetherCore;

/// <summary>Implement on any class in a project (or the SDK) to add a window to the editor. The editor
/// discovers implementers by reflection (like EntityScript) and calls OnGui() each editor frame; draw
/// with the EditorGui.* API. Editor/dev-tooling only - never invoked in a shipped game.</summary>
public interface IEditorWindow
{
    /// <summary>Window title (also its ImGui id). Keep it stable + unique.</summary>
    string Title { get; }

    /// <summary>Whether the window is currently shown. The editor's <c>Project</c> menu reads this to
    /// draw the toggle and writes it to show/hide the window. Back it with the same bool you pass to
    /// <c>EditorGui.Begin(Title, ref open)</c> so closing via the window's X also updates the menu, e.g.
    /// <c>public bool Visible { get => _open; set => _open = value; }</c>.</summary>
    bool Visible { get; set; }

    /// <summary>Called once per editor frame while <see cref="Visible"/> is true. Do your own
    /// EditorGui.Begin(Title, ...) / End here.</summary>
    void OnGui();
}
