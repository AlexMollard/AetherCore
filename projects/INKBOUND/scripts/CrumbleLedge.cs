using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// A shelf of rotten rock. Stand on it and it starts to go: a moment of shaking, then it drops away
/// and is gone for a while before reforming.
///
/// It turns standing still into a cost. On its own that is just a timed jump, but next to the ink it
/// asks a better question - you cannot linger to draw a careful line from here, so either draw fast
/// and rough, or spend ink building somewhere safer to draw from. It also gives the boulder somewhere
/// to fall through on cue.
/// </summary>
public sealed class CrumbleLedge : EntityScript
{
    /// <summary>Seconds of shaking after first contact before it lets go.</summary>
    public float CrumbleSeconds = 0.7f;
    /// <summary>Seconds it stays gone before reforming.</summary>
    public float ReformSeconds = 3.0f;
    /// <summary>How far it shakes while failing, in world units.</summary>
    public float ShakeAmount = 0.06f;

    private Vector3 _home;
    private Vector4 _baseTint = Vector4.One;
    private float _timer = -1.0f;   // counting down to collapse
    private float _gone = -1.0f;    // counting down to reform
    private float _t;

    public override void OnAttach()
    {
        _home = Self.Position;
        _baseTint = SpriteRenderer.GetTint(Self);
        Physics2D.EnableEvents(Self);
    }

    public override void OnCollisionEnter2D(Entity other)
    {
        if (_timer >= 0.0f || _gone >= 0.0f) { return; }
        PlayerController? player = PlayerController.Instance;
        bool weight = (player != null && other.Id == player.Self.Id) || Boulder.IsBoulder(other.Id);
        if (!weight) { return; }
        _timer = CrumbleSeconds;
    }

    public override void OnUpdate(float deltaTime)
    {
        _t += deltaTime;

        if (_gone >= 0.0f)
        {
            _gone -= deltaTime;
            if (_gone <= 0.0f)
            {
                _gone = -1.0f;
                Self.Position = _home;
                SpriteRenderer.SetVisible(Self, true);
                SpriteRenderer.SetTint(Self, _baseTint);
                Physics2D.SetTrigger(Self, false);
            }
            return;
        }

        if (_timer < 0.0f) { return; }

        _timer -= deltaTime;
        // Shudder and redden while it fails, so the warning is unmistakable.
        float shake = MathF.Sin(_t * 55.0f) * ShakeAmount;
        Self.Position = new Vector3(_home.X + shake, _home.Y, _home.Z);
        SpriteRenderer.SetTint(Self, new Vector4(1.0f, 0.55f, 0.45f, 1.0f));

        if (_timer <= 0.0f)
        {
            _timer = -1.0f;
            _gone = ReformSeconds;
            // Drop out of the world rather than destroying it, so it can come back.
            Physics2D.SetTrigger(Self, true);
            SpriteRenderer.SetVisible(Self, false);
            Self.Position = new Vector3(_home.X, _home.Y - 100.0f, _home.Z);
            Scene.Instantiate("Dust", _home);
        }
    }
}
