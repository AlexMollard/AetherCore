using System.Numerics;
using AetherCore.Managed.Interop;

namespace AetherCore.Managed;

/// <summary>Scene lighting, sky, and the day/night cycle.</summary>
public static class Renderer
{
    public static void SetAmbient(Vector3 color) => Native.aether_render_set_ambient(color);

    public static void SetSun(Vector3 direction, float intensity, Vector3 color)
        => Native.aether_render_set_sun(direction, intensity, color);

    /// <summary>Spawns a point-light entity (republished each frame by LightSystem).</summary>
    public static void AddPointLight(Vector3 position, Vector3 color, float intensity, float radius, bool castsShadow = false)
        => Native.aether_render_add_point_light(position, color, intensity, radius, castsShadow ? 1 : 0);

    public static void AddSpotLight(Vector3 position, Vector3 color, float intensity, float radius, Vector3 direction,
        float innerAngleRad, float outerAngleRad, bool castsShadow = false)
        => Native.aether_render_add_spot_light(position, color, intensity, radius, direction, innerAngleRad, outerAngleRad,
            castsShadow ? 1 : 0);

    public static void SetPointLightPosition(int index, Vector3 position) => Native.aether_render_set_point_light_position(index, position);

    public static void SetPointLightColor(int index, Vector3 color) => Native.aether_render_set_point_light_color(index, color);

    public static void SetPointLightIntensity(int index, float intensity) => Native.aether_render_set_point_light_intensity(index, intensity);

    public static int PointLightCount => Native.aether_render_get_point_light_count();

    public static void SetSpotLightPosition(int index, Vector3 position) => Native.aether_render_set_spot_light_position(index, position);

    public static void SetSpotLightColor(int index, Vector3 color) => Native.aether_render_set_spot_light_color(index, color);

    public static void SetSpotLightIntensity(int index, float intensity) => Native.aether_render_set_spot_light_intensity(index, intensity);

    public static int SpotLightCount => Native.aether_render_get_spot_light_count();

    public static void ClearLights() => Native.aether_render_clear_lights();

    public static void SetSky(Vector3 horizon, Vector3 zenith) => Native.aether_render_set_sky(horizon, zenith);

    public static void SetSkyVoid(Vector3 color) => Native.aether_render_set_sky_void(color);
}

/// <summary>The day/night cycle driven by DayNightSystem.</summary>
public static class DayNight
{
    public static bool Enabled
    {
        get => Native.aether_daynight_get_enabled() != 0;
        set => Native.aether_daynight_set_enabled(value ? 1 : 0);
    }

    /// <summary>Time of day in hours [0, 24).</summary>
    public static float TimeOfDay
    {
        get => Native.aether_daynight_get_time();
        set => Native.aether_daynight_set_time(value);
    }

    /// <summary>Simulated seconds per real second.</summary>
    public static float TimeSpeed
    {
        get => Native.aether_daynight_get_speed();
        set => Native.aether_daynight_set_speed(value);
    }

    public static Vector3 SunDirection => Native.aether_daynight_get_sun_direction();
}
