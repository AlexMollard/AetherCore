namespace AetherCore;

/// <summary>
/// Drives a <c>ParticleEmitterComponent</c> from script: fire one-off bursts or
/// gate continuous emission. Author the emitter's look in the inspector / scene;
/// this only triggers it.
/// </summary>
public static class Particles
{
    /// <summary>Emit <paramref name="count"/> particles at once on this entity's emitter.</summary>
    public static void Burst(Entity entity, int count) => Native.aether_particles_burst(entity.Id, count);

    /// <summary>Turn the continuous emission rate on or off (bursts are unaffected).</summary>
    public static void SetEmitting(Entity entity, bool emitting) => Native.aether_particles_set_emitting(entity.Id, emitting ? 1 : 0);
}
