using System.Numerics;
using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// <see cref="PredictedSpawnQueue{T}"/> in isolation: FIFO reconciliation order and the
/// orphan-timeout bound - the two behaviours Whisper had to hand-roll (and that this
/// batch exists to give every game for free) before this type existed. Deliberately
/// uses only <c>default(Entity)</c> ghosts throughout: a real ghost's destruction goes
/// through a native P/Invoke this test host has no engine behind, and every entity here
/// being invalid (<see cref="Entity.IsValid"/> false) means <see cref="Entity.Destroy"/>
/// is never reached, so the queue's own bookkeeping is exercised with no native
/// dependency. <see cref="Net.SpawnPredicted{T}"/>, <see cref="Net.TryTakePredictedSpawn{T}"/>
/// and the native <c>aether_net_mark_predicted</c> fix-up all call straight through the
/// SDK's public facades (see <c>Internal/EngineBackend.cs</c>'s remarks: those facades
/// are never abstracted, even in tests) and so have no seam a headless test can reach -
/// they are exercised instead by the C++-side authority/ownership analysis recorded in
/// the batch report.
/// </summary>
public sealed class NetPredictionTests
{
    [Fact]
    public void TryReconcile_ClaimsOldestRequestFirst()
    {
        var queue = new PredictedSpawnQueue<int>();
        queue.Expect(1);
        queue.Expect(2);
        queue.Expect(3);

        Assert.True(queue.TryReconcile(out int first));
        Assert.Equal(1, first);
        Assert.True(queue.TryReconcile(out int second));
        Assert.Equal(2, second);
        Assert.Equal(1, queue.Count);
    }

    [Fact]
    public void TryReconcile_OnEmptyQueue_ReturnsFalse()
    {
        var queue = new PredictedSpawnQueue<int>();

        Assert.False(queue.TryReconcile(out int payload));
        Assert.Equal(0, payload);
    }

    [Fact]
    public void Age_RetiresOnlyEntriesPastTheTimeout()
    {
        // The exact hazard PendingShotSeconds existed to bound in the hand-rolled
        // version: a request the host never answers (refused, or the reply was lost)
        // must not sit in the queue - or fly on screen - forever, but a request still
        // well within its own budget must survive being aged.
        var queue = new PredictedSpawnQueue<Vector2> { TimeoutSeconds = 1.0f };
        queue.Expect(new Vector2(1, 0)); // requested first, so it ages out first
        queue.Age(0.7f); // 0.7s old - still within the 1.0s bound
        queue.Expect(new Vector2(0, 1)); // requested second, 0.0s old
        Assert.Equal(2, queue.Count);

        queue.Age(0.4f); // first entry is now 1.1s old (past the bound); second is 0.4s old
        Assert.Equal(1, queue.Count);

        // The survivor is the SECOND request - the first was dropped, not just aged -
        // so reconciling now must not hand back the retired one's direction.
        Assert.True(queue.TryReconcile(out Vector2 remaining));
        Assert.Equal(new Vector2(0, 1), remaining);
    }

    [Fact]
    public void Clear_DrainsEveryOutstandingRequest()
    {
        var queue = new PredictedSpawnQueue<int>();
        queue.Expect(1);
        queue.Expect(2);

        queue.Clear();

        Assert.Equal(0, queue.Count);
        Assert.False(queue.TryReconcile(out _));
    }
}
