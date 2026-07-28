using AetherCore;
using Xunit;

namespace AetherCore.Tests;

/// <summary>
/// The sibling-script lookup table behind <see cref="Entity.GetScript{T}"/>.
/// </summary>
public sealed class ScriptInstancesTests : SdkTestBase
{
    private sealed class Movement : EntityScript
    {
    }

    private sealed class Health : EntityScript
    {
    }

    private class Weapon : EntityScript
    {
    }

    private sealed class Shotgun : Weapon
    {
    }

    private static T Attach<T>(uint entityId) where T : EntityScript, new()
    {
        T script = new();
        script.Self = new Entity(entityId);
        ScriptInstances.Register(entityId, script);
        return script;
    }

    [Fact]
    public void Find_ReturnsTheScriptAttachedToThatEntity()
    {
        Movement movement = Attach<Movement>(7);

        Assert.Same(movement, new Entity(7).GetScript<Movement>());
    }

    [Fact]
    public void Find_ReturnsNullForATypeTheEntityDoesNotCarry()
    {
        Attach<Movement>(7);

        Assert.Null(new Entity(7).GetScript<Health>());
    }

    [Fact]
    public void Find_ReturnsNullForAnEntityWithNoScriptsAtAll()
    {
        Assert.Null(new Entity(7).GetScript<Movement>());
    }

    [Fact]
    public void Find_DoesNotLookOnOtherEntities()
    {
        Attach<Movement>(7);

        Assert.Null(new Entity(8).GetScript<Movement>());
    }

    [Fact]
    public void Find_ReturnsTheFirstAttachedWhenAnEntityCarriesTheTypeTwice()
    {
        Movement first = Attach<Movement>(7);
        Movement second = Attach<Movement>(7);

        Assert.Same(first, new Entity(7).GetScript<Movement>());
        Assert.NotSame(second, new Entity(7).GetScript<Movement>());
    }

    [Fact]
    public void Find_MatchesASubclassThroughItsBaseType()
    {
        Shotgun shotgun = Attach<Shotgun>(7);

        Assert.Same(shotgun, new Entity(7).GetScript<Weapon>());
    }

    [Fact]
    public void Find_SkipsPastAnUnrelatedScriptToTheMatchingOne()
    {
        Attach<Movement>(7);
        Health health = Attach<Health>(7);

        Assert.Same(health, new Entity(7).GetScript<Health>());
    }

    [Fact]
    public void GetScript_OnTheScriptItselfLooksOnItsOwnEntity()
    {
        Movement movement = Attach<Movement>(7);
        Health health = Attach<Health>(7);

        Assert.Same(health, movement.GetScript<Health>());
        Assert.Same(movement, health.GetScript<Movement>());
    }

    [Fact]
    public void Unregister_RemovesOnlyThatInstance()
    {
        Movement first = Attach<Movement>(7);
        Movement second = Attach<Movement>(7);

        ScriptInstances.Unregister(first);

        Assert.Same(second, new Entity(7).GetScript<Movement>());
    }

    [Fact]
    public void Unregister_OfAnInstanceThatWasNeverRegisteredIsHarmless()
    {
        Movement stray = new() { Self = new Entity(7) };

        ScriptInstances.Unregister(stray); // no bucket at all
        Attach<Movement>(7);
        ScriptInstances.Unregister(stray); // bucket exists, instance is not in it

        Assert.NotNull(new Entity(7).GetScript<Movement>());
    }

    [Fact]
    public void Unregister_DropsTheBucketOnceTheLastScriptIsGone()
    {
        // The empty-bucket cleanup has no other observable effect - an emptied bucket
        // still resolves every lookup to null - so the table's size is what pins it. Left
        // out, an entity that churns scripts leaks an entry per entity id, forever.
        Movement movement = Attach<Movement>(7);
        Health health = Attach<Health>(7);
        Assert.Equal(1, ScriptInstances.TrackedEntityCount);

        ScriptInstances.Unregister(movement);
        Assert.Equal(1, ScriptInstances.TrackedEntityCount); // still holding Health

        ScriptInstances.Unregister(health);
        Assert.Equal(0, ScriptInstances.TrackedEntityCount);
    }

    [Fact]
    public void Unregister_LeavesNothingBehindForManyEntitiesComingAndGoing()
    {
        for (uint id = 1; id <= 64; id++)
        {
            Movement movement = Attach<Movement>(id);
            ScriptInstances.Unregister(movement);
        }

        Assert.Equal(0, ScriptInstances.TrackedEntityCount);
    }

    [Fact]
    public void AnEntityIdReusedAfterADestroyInheritsNothing()
    {
        Movement movement = Attach<Movement>(7);
        ScriptInstances.Unregister(movement);

        Health reborn = Attach<Health>(7);

        Assert.Null(new Entity(7).GetScript<Movement>());
        Assert.Same(reborn, new Entity(7).GetScript<Health>());
    }

    [Fact]
    public void Clear_EmptiesTheWholeTable()
    {
        Attach<Movement>(7);
        Attach<Health>(8);

        ScriptInstances.Clear();

        Assert.Equal(0, ScriptInstances.TrackedEntityCount);
        Assert.Null(new Entity(7).GetScript<Movement>());
        Assert.Null(new Entity(8).GetScript<Health>());
    }
}
