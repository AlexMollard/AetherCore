using System;
using AetherCore;
using Xunit;

// The SDK's seam (EngineBackend.Api), NetSession's cross-scene fields and the
// ScriptInstances table are all process-wide statics, exactly as they are in the engine.
// Tests therefore run one at a time and reset them between cases; parallelising would
// only be testing which case got there first.
[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace AetherCore.Tests;

/// <summary>
/// Puts a fresh <see cref="TestEngineBackend"/> behind the SDK for the duration of one
/// test and clears every static the SDK carries across scenes, so no case can inherit
/// another's session.
/// </summary>
public abstract class SdkTestBase : IDisposable
{
    /// <summary>The engine this test is describing.</summary>
    protected readonly TestEngineBackend Engine = new();

    /// <summary>Install the double and reset the SDK's cross-scene statics.</summary>
    protected SdkTestBase()
    {
        EngineBackend.Api = Engine;
        ResetSessionStatics();
        ScriptInstances.Clear();
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        EngineBackend.Api = NativeEngineBackend.Instance;
        ResetSessionStatics();
        ScriptInstances.Clear();
        GC.SuppressFinalize(this);
    }

    private static void ResetSessionStatics()
    {
        NetSession.LocalPlayerName = string.Empty; // becomes DefaultPlayerName
        NetSession.StatusMessage = string.Empty;
        NetSession.HostAddress = string.Empty;
        NetSession.HostPort = 0;
        NetSession.HostRoomCode = string.Empty;
        NetSession.JoinRequested = false;
    }
}
