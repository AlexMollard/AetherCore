using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A collectible orb. Bobs on a sine wave, spins, and pulses its emissive colour
/// (Mathf + Time + Material), and tags itself so the GameManager can find and
/// collect it. Spawned from the "Orb" prefab via Scene.Instantiate.
/// </summary>
public sealed class Orb : EntityScript
{
    private float _baseY;
    private float _phase;
    private static TagId s_tag;

    public override void OnAttach()
    {
        _baseY = Self.Position.Y;
        _phase = Random.Range(0.0f, Mathf.PI * 2.0f);
        if (!s_tag.IsValid)
        {
            s_tag = Tags.Create("orb");
        }
        Tags.Add(Self, s_tag);
    }

    public override void OnUpdate(float dt)
    {
        float t = Time.TotalTime + _phase;

        // Bob and spin.
        Vector3 p = Self.Position;
        p.Y = _baseY + Mathf.Sin(t * 2.0f) * 0.25f;
        Self.SetTransform(p, new Vector3(0.0f, (t * 120.0f) % 360.0f, 0.0f), Self.Scale);

        // Pulse the emissive glow.
        float glow = 0.35f + 0.35f * Mathf.Sin(t * 4.0f);
        Material.SetEmissive(Self, new Vector3(0.08f, 0.55f * glow, 0.85f * glow));
    }
}
