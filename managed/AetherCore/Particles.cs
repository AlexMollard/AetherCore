namespace AetherCore;

/// <summary>Which over-lifetime track <see cref="Particles.SetKeys"/> replaces.</summary>
public enum ParticleKeyChannel
{
    /// <summary>RGB per key; key.z is unused (alpha has its own channel).</summary>
    Color = 0,
    Alpha = 1,
    /// <summary>Quad edge in world units.</summary>
    Size = 2,
    /// <summary>Degrees.</summary>
    Rotation = 3,
}

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

    /// <summary>
    /// Replace an over-lifetime key track (colour / alpha / size / rotation) with <paramref name="keys"/>,
    /// evaluated piecewise-linearly at normalised particle age. Each key is <c>(t, x, y, z)</c>: t in 0..1,
    /// then the value - rgb for <see cref="ParticleKeyChannel.Color"/>, a scalar in x for the others.
    /// An empty span clears the track, falling back to the start/end colour and size.
    /// </summary>
    public static unsafe void SetKeys(Entity entity, ParticleKeyChannel channel, System.ReadOnlySpan<System.Numerics.Vector4> keys)
    {
        fixed (System.Numerics.Vector4* p = keys)
        {
            Native.aether_particles_set_keys(entity.Id, (int)channel, p, keys.Length);
        }
    }
}
