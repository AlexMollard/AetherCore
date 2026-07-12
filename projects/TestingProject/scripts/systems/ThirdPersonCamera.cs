using AetherCore;

namespace AetherGame.Systems;

/// <summary>Keeps an orbit camera's target locked to a followed entity.</summary>
public static class ThirdPersonCamera
{
    public static void Update(Entity camera, Entity target)
    {
        if (!camera.IsValid || !target.IsValid)
        {
            return;
        }
        Camera.SetTarget(camera, target.Position);
    }
}
