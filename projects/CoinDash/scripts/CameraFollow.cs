using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Side-scroller camera: smoothly tracks the player along X (never backwards
/// past the level start), holds a gentle Y band, attached to the main camera.
/// </summary>
public sealed class CameraFollow : EntityScript
{
    public float MinX = 0.0f;
    public float MaxX = 74.0f;
    public float BaseY = 2.0f;
    public float SmoothSpeed = 6.0f;

    private Entity _player;

    public override void OnAttach()
    {
        _player = Scene.Find("Player");
    }

    public override void OnUpdate(float deltaTime)
    {
        if (!_player.IsValid)
        {
            _player = Scene.Find("Player");
            return;
        }
        Vector3 target = _player.Position;
        Vector3 pos = Self.Position;
        float wantX = System.Math.Clamp(target.X, MinX, MaxX);
        float wantY = System.Math.Max(BaseY, target.Y * 0.35f + BaseY * 0.65f);
        float t = System.Math.Clamp(SmoothSpeed * deltaTime, 0.0f, 1.0f);
        pos.X += (wantX - pos.X) * t;
        pos.Y += (wantY - pos.Y) * t;
        Self.Position = pos;
    }
}
