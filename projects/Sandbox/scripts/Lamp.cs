using AetherCore;

namespace AetherGame;

/// <summary>
/// Wiremod's sink: drives a real Point Light's intensity from a wire - proving a signal
/// can reach back OUT into the world, not just be consumed by another script. Self must
/// already carry a Point Light component (add it in whatever scene/prefab places a Lamp);
/// this script drives an existing light, it does not create one.
///
/// VERIFIED END TO END BEFORE PROMISING IT, not assumed from the component's name: Self.
/// Component("Point Light") (ComponentAccess, Component.cs) calls the genuinely
/// reflection-driven aether_component_set_number export (ComponentExports.cpp) - its own
/// file header: "every component declared with AE_COMPONENT... reachable from C#", not a
/// hardcoded allowlist - and Point Light's "intensity" field is declared exactly that way
/// (Float, AE_GENERIC_SERIALIZE) in CoreComponents.reflect.cpp. The other light API,
/// Renderer.SetPointLightIntensity(index, ...), was checked and rejected for this: it
/// addresses a flat renderer-owned list by position, with no entity handle in or out, so
/// there is no way to say "this specific scene-placed lamp" through it.
/// </summary>
public sealed class Lamp : EntityScript
{
    /// <summary>INPUT. Greater than 0.5 reads as "on".</summary>
    public float Enable;

    public float OnIntensity = 20.0f;
    public float OffIntensity = 0.0f;

    private ComponentAccess _light;

    public override void OnAttach()
    {
        _light = Self.Component("Point Light");
        if (!_light.Exists)
        {
            Log.Warn("[Sandbox] Lamp: this entity has no 'Point Light' component - add one in the scene/prefab. Lamp drives an existing light, it does not create one.");
        }
    }

    public override void OnUpdate(float deltaTime)
    {
        if (_light.Exists)
        {
            _light.SetFloat("intensity", Enable > 0.5f ? OnIntensity : OffIntensity);
        }
    }
}
