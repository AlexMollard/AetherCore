using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherCore.Tests;

/// <summary>One UI call the SDK made, in the order it made it.</summary>
/// <remarks>A record of intent rather than a rendered result: there is no UI system
/// here, so what these tests can check is that the right element was told the right
/// thing at the right moment - which is precisely what silently breaks.</remarks>
public readonly record struct UiCall(string Op, uint Element, float A, float B, float C, float D, string Text);

/// <summary>
/// A scriptable stand-in for the engine. Test bodies set the session state they want to
/// describe, tick the class under test, and read back what it did.
/// </summary>
public sealed class TestEngineBackend : IEngineBackend
{
    private uint _nextEntityId = 1000;

    // ── Scripted state ──────────────────────────────────────────────────────────

    /// <summary>What <see cref="Net.IsHost"/> reports.</summary>
    public bool IsHost;

    /// <summary>What <see cref="Net.IsClient"/> reports.</summary>
    public bool IsClient;

    /// <summary>What <see cref="Net.IsConnected"/> reports.</summary>
    public bool IsConnected;

    /// <summary>What <see cref="Net.DisconnectReason"/> reports.</summary>
    public string DisconnectReason = string.Empty;

    /// <summary>What <see cref="Net.LocalConnectionId"/> reports.</summary>
    public uint LocalConnectionId;

    /// <summary>What <see cref="Net.Connections"/> reports.</summary>
    public List<uint> Connections = new();

    /// <summary>Player entities, with the display name each currently carries. An empty
    /// name is a player that has been spawned but not yet introduced itself.</summary>
    public List<(Entity Entity, string Name, uint Owner)> Players = new();

    /// <summary>Entities <see cref="Scene.Find"/> resolves, by name.</summary>
    public Dictionary<string, Entity> SceneEntities = new();

    /// <summary>World positions of the entities above.</summary>
    public Dictionary<uint, Vector3> Positions = new();

    /// <summary>Round-trip times reported per player entity.</summary>
    public Dictionary<uint, uint> Pings = new();

    /// <summary>Keys reported as pressed this frame.</summary>
    public HashSet<Key> PressedKeys = new();

    /// <summary>What <see cref="Camera.WorldToScreen"/> returns. Default is the
    /// behind-camera sentinel's opposite: a plain identity-ish projection.</summary>
    public Func<Vector3, Vector2> Project = w => new Vector2(w.X, w.Y);

    /// <summary>Whether <see cref="Net.Spawn"/> succeeds. False returns an invalid
    /// entity, the way the engine does for a missing prefab or a non-host caller.</summary>
    public bool SpawnSucceeds = true;

    /// <summary>Whether <see cref="Net.Connect"/> succeeds.</summary>
    public bool ConnectSucceeds = true;

    /// <summary>Whether <see cref="Net.Host"/> succeeds.</summary>
    public bool HostSucceeds = true;

    /// <summary>Whether <see cref="Net.HostWithCode"/> succeeds.</summary>
    public bool HostWithCodeSucceeds = true;

    /// <summary>Whether <see cref="Net.JoinByCode"/> succeeds.</summary>
    public bool JoinByCodeSucceeds = true;

    /// <summary>What <see cref="Net.NewRoomCode"/> mints.</summary>
    public string RoomCodeToMint = "ABCDEF";

    /// <summary>What <see cref="Net.TraversalState"/> reports.</summary>
    public NetTraversalState TraversalState;

    /// <summary>What <see cref="Net.TraversalError"/> reports.</summary>
    public string TraversalError = string.Empty;

    /// <summary>What <see cref="Net.LastError"/> reports.</summary>
    public string LastError = string.Empty;

    // ── Recorded calls ──────────────────────────────────────────────────────────

    /// <summary>Every <c>Net.Spawn</c>, as (prefab, position, owner).</summary>
    public List<(string Prefab, Vector3 Position, uint Owner)> Spawned = new();

    /// <summary>Every <c>Scene.Instantiate</c>, as (prefab, position).</summary>
    public List<(string Prefab, Vector3 Position)> Instantiated = new();

    /// <summary>Every <c>Net.SetPlayerName</c>, as (entity, name).</summary>
    public List<(Entity Entity, string Name)> NamesSet = new();

    /// <summary>Scene names passed to <c>Scene.Load</c>.</summary>
    public List<string> ScenesLoaded = new();

    /// <summary>How many times <c>Net.Disconnect</c> was called.</summary>
    public int DisconnectCount;

    /// <summary>Every <c>Net.Connect</c>, as (address, port).</summary>
    public List<(string Address, int Port)> ConnectAttempts = new();

    /// <summary>Every <c>Net.HostWithCode</c> call, as (code, port, maxConnections).</summary>
    public List<(string Code, int Port, int MaxConnections)> HostWithCodeAttempts = new();

    /// <summary>Every <c>Net.JoinByCode</c> call, by code.</summary>
    public List<string> JoinByCodeAttempts = new();

    /// <summary>Every value written to <c>Net.ReplicationReady</c>.</summary>
    public List<bool> ReplicationReadyWrites = new();

    /// <summary>UI calls in order.</summary>
    public List<UiCall> UiCalls = new();

    /// <summary>UI text elements created, in creation order.</summary>
    public List<Entity> UiElements = new();

    /// <summary>Entities destroyed, in order.</summary>
    public List<Entity> Destroyed = new();

    /// <summary>Latest active state written per entity.</summary>
    public Dictionary<uint, bool> ActiveState = new();

    // ── Helpers for building a scenario ─────────────────────────────────────────

    /// <summary>Mint a fresh entity id that no scenario has used yet.</summary>
    public Entity NewEntity() => new(_nextEntityId++);

    /// <summary>Put a named entity in the scene at <paramref name="position"/>, the way
    /// a spawn marker is authored.</summary>
    public Entity PlaceEntity(string name, Vector3 position)
    {
        Entity e = NewEntity();
        SceneEntities[name] = e;
        Positions[e.Id] = position;
        return e;
    }

    /// <summary>Add a player entity owned by <paramref name="owner"/>, optionally
    /// already carrying a display name.</summary>
    public Entity AddPlayer(uint owner, string name = "")
    {
        Entity e = NewEntity();
        Players.Add((e, name, owner));
        return e;
    }

    /// <summary>Give an existing player a display name, as replication would.</summary>
    public void NamePlayer(Entity entity, string name)
    {
        for (int i = 0; i < Players.Count; i++)
        {
            if (Players[i].Entity == entity)
            {
                Players[i] = (entity, name, Players[i].Owner);
                return;
            }
        }
        throw new InvalidOperationException($"No player entity {entity.Id} in this scenario.");
    }

    /// <summary>Remove a player entity, as the framework's despawn does when its owner
    /// drops.</summary>
    public void RemovePlayer(Entity entity) => Players.RemoveAll(p => p.Entity == entity);

    /// <summary>The recorded UI calls of one kind against one element.</summary>
    public List<UiCall> CallsOn(Entity element, string op)
        => UiCalls.FindAll(c => c.Element == element.Id && c.Op == op);

    /// <summary>The last recorded call of one kind against one element. Throws when
    /// there is none, so a test that expected one fails on the missing call rather than
    /// on a null dereference somewhere further down.</summary>
    public UiCall LastCall(Entity element, string op)
    {
        List<UiCall> calls = CallsOn(element, op);
        if (calls.Count == 0)
        {
            throw new InvalidOperationException($"No '{op}' call was made on element {element.Id}.");
        }
        return calls[calls.Count - 1];
    }

    // ── IEngineBackend ──────────────────────────────────────────────────────────

    /// <inheritdoc/>
    public bool NetIsHost => IsHost;

    /// <inheritdoc/>
    public bool NetIsClient => IsClient;

    /// <inheritdoc/>
    public bool NetIsConnected => IsConnected;

    /// <inheritdoc/>
    public string NetDisconnectReason => DisconnectReason;

    /// <inheritdoc/>
    public uint NetLocalConnectionId => LocalConnectionId;

    /// <inheritdoc/>
    public uint[] NetConnections => Connections.ToArray();

    /// <inheritdoc/>
    public void NetSetReplicationReady(bool ready) => ReplicationReadyWrites.Add(ready);

    /// <inheritdoc/>
    public bool NetHost(int port, int maxConnections) => HostSucceeds;

    /// <inheritdoc/>
    public bool NetConnect(string address, int port)
    {
        ConnectAttempts.Add((address, port));
        return ConnectSucceeds;
    }

    /// <inheritdoc/>
    public void NetDisconnect() => DisconnectCount++;

    /// <inheritdoc/>
    public bool NetHostWithCode(string code, int port, int maxConnections)
    {
        HostWithCodeAttempts.Add((code, port, maxConnections));
        return HostWithCodeSucceeds;
    }

    /// <inheritdoc/>
    public bool NetJoinByCode(string code)
    {
        JoinByCodeAttempts.Add(code);
        return JoinByCodeSucceeds;
    }

    /// <inheritdoc/>
    public string NetNewRoomCode() => RoomCodeToMint;

    /// <inheritdoc/>
    public NetTraversalState NetTraversalState => TraversalState;

    /// <inheritdoc/>
    public string NetTraversalError => TraversalError;

    /// <inheritdoc/>
    public string NetLastError => LastError;

    /// <inheritdoc/>
    public Entity[] NetPlayers
    {
        get
        {
            Entity[] result = new Entity[Players.Count];
            for (int i = 0; i < Players.Count; i++)
            {
                result[i] = Players[i].Entity;
            }
            return result;
        }
    }

    /// <inheritdoc/>
    public Entity NetSpawn(string prefab, Vector3 position, uint owner)
    {
        Spawned.Add((prefab, position, owner));
        if (!SpawnSucceeds)
        {
            return default;
        }
        return AddPlayer(owner);
    }

    /// <inheritdoc/>
    public void NetSetPlayerName(Entity entity, string name)
    {
        NamesSet.Add((entity, name));
        for (int i = 0; i < Players.Count; i++)
        {
            if (Players[i].Entity == entity)
            {
                Players[i] = (entity, name, Players[i].Owner);
                return;
            }
        }
    }

    /// <inheritdoc/>
    public string NetGetPlayerName(Entity entity)
    {
        foreach ((Entity e, string name, uint _) in Players)
        {
            if (e == entity)
            {
                return name;
            }
        }
        return string.Empty;
    }

    /// <inheritdoc/>
    public uint NetGetPlayerPing(Entity entity) => Pings.TryGetValue(entity.Id, out uint ms) ? ms : 0u;

    /// <inheritdoc/>
    public uint NetOwnerOf(Entity entity)
    {
        foreach ((Entity e, string _, uint owner) in Players)
        {
            if (e == entity)
            {
                return owner;
            }
        }
        return LocalConnectionId;
    }

    /// <inheritdoc/>
    public Entity SceneFind(string name) => SceneEntities.TryGetValue(name, out Entity e) ? e : default;

    /// <inheritdoc/>
    public Entity SceneInstantiate(string prefabName, Vector3 position)
    {
        Instantiated.Add((prefabName, position));
        if (!SpawnSucceeds)
        {
            return default;
        }
        return AddPlayer(LocalConnectionId);
    }

    /// <inheritdoc/>
    public void SceneLoad(string sceneName) => ScenesLoaded.Add(sceneName);

    /// <inheritdoc/>
    public Vector3 EntityPosition(Entity entity)
        => Positions.TryGetValue(entity.Id, out Vector3 p) ? p : Vector3.Zero;

    /// <inheritdoc/>
    public void EntitySetActive(Entity entity, bool active) => ActiveState[entity.Id] = active;

    /// <inheritdoc/>
    public void EntityDestroy(Entity entity) => Destroyed.Add(entity);

    /// <inheritdoc/>
    public Entity UiCreateText(Entity canvas)
    {
        Entity e = NewEntity();
        UiElements.Add(e);
        return e;
    }

    /// <inheritdoc/>
    public void UiSetText(Entity element, string text)
        => UiCalls.Add(new UiCall("SetText", element.Id, 0, 0, 0, 0, text));

    /// <inheritdoc/>
    public void UiSetRect(Entity element, float x, float y, float width, float height)
        => UiCalls.Add(new UiCall("SetRect", element.Id, x, y, width, height, string.Empty));

    /// <inheritdoc/>
    public void UiSetAnchors(Entity element, Vector2 min, Vector2 max)
        => UiCalls.Add(new UiCall("SetAnchors", element.Id, min.X, min.Y, max.X, max.Y, string.Empty));

    /// <inheritdoc/>
    public void UiSetPivot(Entity element, Vector2 pivot)
        => UiCalls.Add(new UiCall("SetPivot", element.Id, pivot.X, pivot.Y, 0, 0, string.Empty));

    /// <inheritdoc/>
    public void UiSetTextAlign(Entity element, UiHAlign horizontal, UiVAlign vertical)
        => UiCalls.Add(new UiCall("SetTextAlign", element.Id, (int)horizontal, (int)vertical, 0, 0, string.Empty));

    /// <inheritdoc/>
    public void UiSetTextColor(Entity element, Vector4 color)
        => UiCalls.Add(new UiCall("SetTextColor", element.Id, color.X, color.Y, color.Z, color.W, string.Empty));

    /// <inheritdoc/>
    public void UiSetFont(Entity element, string fontName)
        => UiCalls.Add(new UiCall("SetFont", element.Id, 0, 0, 0, 0, fontName));

    /// <inheritdoc/>
    public void UiSetFontSize(Entity element, float pixelSize)
        => UiCalls.Add(new UiCall("SetFontSize", element.Id, pixelSize, 0, 0, 0, string.Empty));

    /// <inheritdoc/>
    public bool InputIsKeyPressed(Key key) => PressedKeys.Contains(key);

    /// <inheritdoc/>
    public Vector2 CameraWorldToScreen(Vector3 worldPosition) => Project(worldPosition);
}
