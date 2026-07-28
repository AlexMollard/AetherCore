using System.Numerics;

namespace AetherCore;

/// <summary>
/// The narrow set of engine operations the SDK's own stateful classes -
/// <see cref="NetSessionDirector"/>, <see cref="NetSession"/> and
/// <see cref="WorldLabel"/> - reach the engine through, so their logic can be
/// exercised without a running engine.
/// </summary>
/// <remarks>
/// <para>
/// This is deliberately NOT an abstraction over <c>Internal/Native.cs</c>. That file
/// declares hundreds of P/Invokes and every one of them stays a direct call: the public
/// facades (<see cref="Net"/>, <see cref="Scene"/>, <see cref="Ui"/>,
/// <see cref="Input"/>, <see cref="Camera"/>, <see cref="Entity"/>) are untouched and
/// game code that calls them pays exactly what it paid before. What is behind this
/// interface is only the handful of operations the SDK classes that carry non-trivial
/// LOGIC happen to use - state machines, diffing and projection, the code where a
/// refactor can silently change behaviour and nothing would notice.
/// </para>
/// <para>
/// Everything here runs on the engine's main loop thread, like the rest of the script
/// surface, so <see cref="EngineBackend.Api"/> needs no synchronisation.
/// </para>
/// </remarks>
internal interface IEngineBackend
{
    // ── Net: session role and link state ────────────────────────────────────────

    /// <summary><see cref="Net.IsHost"/>.</summary>
    bool NetIsHost { get; }

    /// <summary><see cref="Net.IsClient"/>.</summary>
    bool NetIsClient { get; }

    /// <summary><see cref="Net.IsConnected"/>.</summary>
    bool NetIsConnected { get; }

    /// <summary><see cref="Net.DisconnectReason"/>.</summary>
    string NetDisconnectReason { get; }

    /// <summary><see cref="Net.LocalConnectionId"/>.</summary>
    uint NetLocalConnectionId { get; }

    /// <summary><see cref="Net.Connections"/>.</summary>
    uint[] NetConnections { get; }

    /// <summary><see cref="Net.ReplicationReady"/>, write half only - nothing in the SDK
    /// reads it back.</summary>
    void NetSetReplicationReady(bool ready);

    /// <summary><see cref="Net.Host"/>.</summary>
    bool NetHost(int port, int maxConnections);

    /// <summary><see cref="Net.Connect"/>.</summary>
    bool NetConnect(string address, int port);

    /// <summary><see cref="Net.Disconnect"/>.</summary>
    void NetDisconnect();

    // ── Net: players ────────────────────────────────────────────────────────────

    /// <summary><see cref="Net.Players"/>.</summary>
    Entity[] NetPlayers { get; }

    /// <summary><see cref="Net.Spawn"/>.</summary>
    Entity NetSpawn(string prefab, Vector3 position, uint owner);

    /// <summary><see cref="Net.SetPlayerName"/>.</summary>
    void NetSetPlayerName(Entity entity, string name);

    /// <summary><see cref="Net.GetPlayerName"/>.</summary>
    string NetGetPlayerName(Entity entity);

    /// <summary><see cref="Net.GetPlayerPing"/>.</summary>
    uint NetGetPlayerPing(Entity entity);

    /// <summary><see cref="Net.OwnerOf"/>.</summary>
    uint NetOwnerOf(Entity entity);

    // ── Scene and entities ──────────────────────────────────────────────────────

    /// <summary><see cref="Scene.Find"/>.</summary>
    Entity SceneFind(string name);

    /// <summary><see cref="Scene.Instantiate"/>.</summary>
    Entity SceneInstantiate(string prefabName, Vector3 position);

    /// <summary><see cref="Scene.Load"/>.</summary>
    void SceneLoad(string sceneName);

    /// <summary><see cref="Entity.Position"/>, read half.</summary>
    Vector3 EntityPosition(Entity entity);

    /// <summary><see cref="Entity.SetActive"/>.</summary>
    void EntitySetActive(Entity entity, bool active);

    /// <summary><see cref="Entity.Destroy"/>.</summary>
    void EntityDestroy(Entity entity);

    // ── UI ──────────────────────────────────────────────────────────────────────

    /// <summary><see cref="Ui.CreateText(Entity, string)"/> with no initial text.</summary>
    Entity UiCreateText(Entity canvas);

    /// <summary><see cref="Ui.SetText"/>.</summary>
    void UiSetText(Entity element, string text);

    /// <summary><see cref="Ui.SetRect"/>.</summary>
    void UiSetRect(Entity element, float x, float y, float width, float height);

    /// <summary><see cref="Ui.SetAnchors"/>.</summary>
    void UiSetAnchors(Entity element, Vector2 min, Vector2 max);

    /// <summary><see cref="Ui.SetPivot"/>.</summary>
    void UiSetPivot(Entity element, Vector2 pivot);

    /// <summary><see cref="Ui.SetTextAlign"/>.</summary>
    void UiSetTextAlign(Entity element, UiHAlign horizontal, UiVAlign vertical);

    /// <summary><see cref="Ui.SetTextColor"/>.</summary>
    void UiSetTextColor(Entity element, Vector4 color);

    /// <summary><see cref="Ui.SetFont"/>.</summary>
    void UiSetFont(Entity element, string fontName);

    /// <summary><see cref="Ui.SetFontSize"/>.</summary>
    void UiSetFontSize(Entity element, float pixelSize);

    // ── Input and camera ────────────────────────────────────────────────────────

    /// <summary><see cref="Input.IsKeyPressed"/>.</summary>
    bool InputIsKeyPressed(Key key);

    /// <summary><see cref="Camera.WorldToScreen"/>.</summary>
    Vector2 CameraWorldToScreen(Vector3 worldPosition);
}

/// <summary>
/// The shipping implementation: every member forwards, in one line, to the public facade
/// that already wraps the P/Invoke. Nothing here adds logic, and nothing here is the
/// only route to the engine - it exists so the classes above it can be handed a
/// different one in a test.
/// </summary>
internal sealed class NativeEngineBackend : IEngineBackend
{
    /// <summary>The single instance. Stateless, so one is enough.</summary>
    internal static readonly NativeEngineBackend Instance = new();

    private NativeEngineBackend()
    {
    }

    /// <inheritdoc/>
    public bool NetIsHost => Net.IsHost;

    /// <inheritdoc/>
    public bool NetIsClient => Net.IsClient;

    /// <inheritdoc/>
    public bool NetIsConnected => Net.IsConnected;

    /// <inheritdoc/>
    public string NetDisconnectReason => Net.DisconnectReason;

    /// <inheritdoc/>
    public uint NetLocalConnectionId => Net.LocalConnectionId;

    /// <inheritdoc/>
    public uint[] NetConnections => Net.Connections;

    /// <inheritdoc/>
    public void NetSetReplicationReady(bool ready) => Net.ReplicationReady = ready;

    /// <inheritdoc/>
    public bool NetHost(int port, int maxConnections) => Net.Host(port, maxConnections);

    /// <inheritdoc/>
    public bool NetConnect(string address, int port) => Net.Connect(address, port);

    /// <inheritdoc/>
    public void NetDisconnect() => Net.Disconnect();

    /// <inheritdoc/>
    public Entity[] NetPlayers => Net.Players;

    /// <inheritdoc/>
    public Entity NetSpawn(string prefab, Vector3 position, uint owner) => Net.Spawn(prefab, position, owner);

    /// <inheritdoc/>
    public void NetSetPlayerName(Entity entity, string name) => Net.SetPlayerName(entity, name);

    /// <inheritdoc/>
    public string NetGetPlayerName(Entity entity) => Net.GetPlayerName(entity);

    /// <inheritdoc/>
    public uint NetGetPlayerPing(Entity entity) => Net.GetPlayerPing(entity);

    /// <inheritdoc/>
    public uint NetOwnerOf(Entity entity) => Net.OwnerOf(entity);

    /// <inheritdoc/>
    public Entity SceneFind(string name) => Scene.Find(name);

    /// <inheritdoc/>
    public Entity SceneInstantiate(string prefabName, Vector3 position) => Scene.Instantiate(prefabName, position);

    /// <inheritdoc/>
    public void SceneLoad(string sceneName) => Scene.Load(sceneName);

    /// <inheritdoc/>
    public Vector3 EntityPosition(Entity entity) => entity.Position;

    /// <inheritdoc/>
    public void EntitySetActive(Entity entity, bool active) => entity.SetActive(active);

    /// <inheritdoc/>
    public void EntityDestroy(Entity entity) => entity.Destroy();

    /// <inheritdoc/>
    public Entity UiCreateText(Entity canvas) => Ui.CreateText(canvas);

    /// <inheritdoc/>
    public void UiSetText(Entity element, string text) => Ui.SetText(element, text);

    /// <inheritdoc/>
    public void UiSetRect(Entity element, float x, float y, float width, float height)
        => Ui.SetRect(element, x, y, width, height);

    /// <inheritdoc/>
    public void UiSetAnchors(Entity element, Vector2 min, Vector2 max) => Ui.SetAnchors(element, min, max);

    /// <inheritdoc/>
    public void UiSetPivot(Entity element, Vector2 pivot) => Ui.SetPivot(element, pivot);

    /// <inheritdoc/>
    public void UiSetTextAlign(Entity element, UiHAlign horizontal, UiVAlign vertical)
        => Ui.SetTextAlign(element, horizontal, vertical);

    /// <inheritdoc/>
    public void UiSetTextColor(Entity element, Vector4 color) => Ui.SetTextColor(element, color);

    /// <inheritdoc/>
    public void UiSetFont(Entity element, string fontName) => Ui.SetFont(element, fontName);

    /// <inheritdoc/>
    public void UiSetFontSize(Entity element, float pixelSize) => Ui.SetFontSize(element, pixelSize);

    /// <inheritdoc/>
    public bool InputIsKeyPressed(Key key) => Input.IsKeyPressed(key);

    /// <inheritdoc/>
    public Vector2 CameraWorldToScreen(Vector3 worldPosition) => Camera.WorldToScreen(worldPosition);
}

/// <summary>Which <see cref="IEngineBackend"/> the SDK's stateful classes are talking
/// to. The real engine, always, outside a test.</summary>
internal static class EngineBackend
{
    /// <summary>The live backend. Assigning is a test-only affordance; nothing in the
    /// shipped SDK or in game code ever writes it, so the shipped path is
    /// <see cref="NativeEngineBackend"/> from process start to exit.</summary>
    internal static IEngineBackend Api = NativeEngineBackend.Instance;
}
