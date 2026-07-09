using System;
using System.Numerics;
using AetherCore;

namespace AetherGame.Systems;

/// <summary>Spawns a ring of animated foxes.</summary>
public static class FoxSystem
{
    public static void SpawnFoxes(int count, float radius, TagId foxTag)
    {
        if (count <= 0)
        {
            return;
        }
        float step = 6.2832f / count;
        for (int i = 0; i < count; ++i)
        {
            float angle = i * step;
            var pos = new Vector3(MathF.Cos(angle) * radius, 0.0f, MathF.Sin(angle) * radius);
            float yaw = angle * 57.2958f + 90.0f;

            Entity fox = World.Create();
            fox.AddTransform();
            fox.SetTransform(pos, new Vector3(0.0f, yaw, 0.0f), new Vector3(0.05f, 0.05f, 0.05f));
            fox.LoadModel("project://assets/models/Fox/Fox.mesh");

            // Play the Run clip, desynchronized per fox.
            Animation.SetClip(fox, 2);
            Animation.SetPlaybackSpeed(fox, 0.7f);
            Animation.SetTime(fox, i * 0.3f);

            Tags.Add(fox, foxTag);
        }
    }
}
