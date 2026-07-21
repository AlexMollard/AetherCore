using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Animates the shared menu atmosphere: staggered red "watching eye" blinks.
/// The vignette and scanlines are static authored images; the base dark comes from the
/// camera clear.</summary>
public sealed class MenuShell : EntityScript
{
    private readonly Entity[] _eyes = new Entity[4];
    private readonly float[] _phase = { 0f, 3.3f, 6.7f, 9.1f };
    private readonly float[] _period = { 9f, 11f, 13f, 10f };
    private static readonly Vector4 EyeColor = new(0.416f, 0.165f, 0.165f, 1f);
    private float _t;

    public override void OnAttach()
    {
        for (int i = 0; i < _eyes.Length; i++) _eyes[i] = Scene.Find($"Eye{i}");
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        for (int i = 0; i < _eyes.Length; i++)
        {
            if (!_eyes[i].IsValid) continue;
            // Mostly closed; a brief opening near the end of each period.
            float u = ((_t + _phase[i]) % _period[i]) / _period[i];
            float open = u > 0.86f ? System.MathF.Sin((u - 0.86f) / 0.14f * System.MathF.PI) : 0f;
            Vector4 c = EyeColor;
            c.W = 0.85f * System.Math.Clamp(open, 0f, 1f);
            Ui.SetImageColor(_eyes[i], c);
        }
    }
}
