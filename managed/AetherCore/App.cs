namespace AetherCore;

/// <summary>
/// The process the game is running in.
/// </summary>
/// <remarks>
/// Deliberately small. Almost everything a game needs is the world, the renderer or the input,
/// and the few things that are not belong here rather than being smuggled onto one of those.
/// </remarks>
public static class App
{
    /// <summary>
    /// Ask the game to close.
    /// </summary>
    /// <remarks>
    /// The same request the title bar's close button makes, so the main loop unwinds normally:
    /// the scene is torn down in order and anything with a shutdown to run gets to run it. It is
    /// not a process exit, and it is not immediate - code after this call still runs, and the
    /// current frame finishes.
    /// <para>
    /// This exists because a game with a main menu has a way out of it. Without it the only exit
    /// from a shipped game is the title bar or Alt+F4, which is a hole in the front door rather
    /// than a missing convenience.
    /// </para>
    /// </remarks>
    public static void Quit() => Native.aether_app_quit();
}
