using System.Numerics;

namespace AetherCore;

/// <summary>
/// A typed reference to a component that lives on an entity. Declare a public
/// field of a concrete IComponentRef type (e.g. <see cref="RigidBodyRef"/>) on a
/// script, and the inspector shows an entity slot that only accepts entities that
/// carry that component - dragging one links it. Reach the component's operations
/// through the wrapper's members. Persisted per-entity like any other script
/// field (the link survives save/load via the scene's entity remap).
/// </summary>
public interface IComponentRef
{
    /// <summary>ComponentCatalog name of the required component; the inspector uses
    /// it to validate which entities may be dropped onto the field.</summary>
    static abstract string ComponentType { get; }

    /// <summary>The entity carrying the referenced component.</summary>
    Entity Owner { get; }
}

/// <summary>Reference to a camera on an entity. Drop a camera entity to link it.</summary>
public readonly struct CameraRef : IComponentRef
{
    public static string ComponentType => "Camera";
    public Entity Owner { get; }
    public CameraRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    /// <summary>Make this the scene's main camera.</summary>
    public void MakeMain() => Camera.SetMain(Owner);

    public void SetTarget(Vector3 target) => Camera.SetTarget(Owner, target);
    public void SetOrbital(float yaw, float pitch, float distance) => Camera.SetOrbital(Owner, yaw, pitch, distance);
    public void SetPerspective(float fovDegrees) => Camera.SetPerspective(Owner, fovDegrees);
    public void SetOrthographic(float height) => Camera.SetOrthographic(Owner, height);

    public CameraProjection Projection => Camera.GetProjection(Owner);
    public float OrthographicHeight => Camera.GetOrthographicHeight(Owner);
    public float Yaw => Camera.GetYaw(Owner);
    public Vector3 Forward => Camera.GetForward(Owner);
    public Vector3 Right => Camera.GetRight(Owner);
}

/// <summary>Reference to an authored world-space sprite renderer.</summary>
public readonly struct SpriteRendererRef : IComponentRef
{
    public static string ComponentType => "Sprite Renderer";
    public Entity Owner { get; }
    public SpriteRendererRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    public void SetTexture(string path) => SpriteRenderer.SetTexture(Owner, path);
    public Vector4 Tint { get => SpriteRenderer.GetTint(Owner); set => SpriteRenderer.SetTint(Owner, value); }
    public Vector2 PixelSize { get => SpriteRenderer.GetPixelSize(Owner); set => SpriteRenderer.SetPixelSize(Owner, value); }
    public Vector2 Pivot { get => SpriteRenderer.GetPivot(Owner); set => SpriteRenderer.SetPivot(Owner, value); }
    public float PixelsPerUnit { get => SpriteRenderer.GetPixelsPerUnit(Owner); set => SpriteRenderer.SetPixelsPerUnit(Owner, value); }
    public int SortingLayer { get => SpriteRenderer.GetSortingLayer(Owner); set => SpriteRenderer.SetSortingLayer(Owner, value); }
    public int OrderInLayer { get => SpriteRenderer.GetOrderInLayer(Owner); set => SpriteRenderer.SetOrderInLayer(Owner, value); }
    public SpriteBlendMode BlendMode { get => SpriteRenderer.GetBlendMode(Owner); set => SpriteRenderer.SetBlendMode(Owner, value); }

    private bool GetFlag(uint flag) => (SpriteRenderer.GetFlags(Owner) & flag) != 0;
    private void SetFlag(uint flag, bool value)
    {
        uint flags = SpriteRenderer.GetFlags(Owner);
        SpriteRenderer.SetFlags(Owner, value ? flags | flag : flags & ~flag);
    }
    public bool Visible { get => GetFlag(1); set => SetFlag(1, value); }
    public bool FlipX { get => GetFlag(2); set => SetFlag(2, value); }
    public bool FlipY { get => GetFlag(4); set => SetFlag(4, value); }
    public bool PixelSnap { get => GetFlag(8); set => SetFlag(8, value); }
}

/// <summary>Reference to a deterministic fixed-step sprite animator.</summary>
public readonly struct SpriteAnimatorRef : IComponentRef
{
    public static string ComponentType => "Sprite Animator";
    public Entity Owner { get; }
    public SpriteAnimatorRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    public void SetAnimation(string path) => SpriteAnimator.SetAnimation(Owner, path);
    public void Play() => SpriteAnimator.Play(Owner);
    public void Pause() => SpriteAnimator.Pause(Owner);
    public void Restart() => SpriteAnimator.Restart(Owner);
    public bool IsPlaying => SpriteAnimator.IsPlaying(Owner);
    public uint CurrentFrame => SpriteAnimator.CurrentFrame(Owner);
    public float Speed { get => SpriteAnimator.GetSpeed(Owner); set => SpriteAnimator.SetSpeed(Owner, value); }
    public SpriteAnimationLoopMode LoopMode { get => SpriteAnimator.GetLoopMode(Owner); set => SpriteAnimator.SetLoopMode(Owner, value); }
    public bool TryPopEvent(out SpriteAnimationEvent animationEvent) => SpriteAnimator.TryPopEvent(Owner, out animationEvent);
}

/// <summary>Reference to a rigid body on an entity. Drop an entity that has a
/// Rigid Body to link it, then read/write its motion through this wrapper.</summary>
public readonly struct RigidBodyRef : IComponentRef
{
    public static string ComponentType => "Rigid Body";
    public Entity Owner { get; }
    public RigidBodyRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    public Vector3 LinearVelocity
    {
        get => Physics.GetLinearVelocity(Owner);
        set => Physics.SetLinearVelocity(Owner, value);
    }

    public Vector3 AngularVelocity
    {
        get => Physics.GetAngularVelocity(Owner);
        set => Physics.SetAngularVelocity(Owner, value);
    }

    public void AddForce(Vector3 force) => Physics.AddForce(Owner, force);
    public void AddImpulse(Vector3 impulse) => Physics.AddImpulse(Owner, impulse);
}

/// <summary>Reference to a 2D rigid body on an entity (Box2D-backed; 2D scenes
/// only). Drop an entity that has a Rigid Body 2D to link it.</summary>
public readonly struct RigidBody2DRef : IComponentRef
{
    public static string ComponentType => "Rigid Body 2D";
    public Entity Owner { get; }
    public RigidBody2DRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    public Vector2 LinearVelocity
    {
        get => Physics2D.GetLinearVelocity(Owner);
        set => Physics2D.SetLinearVelocity(Owner, value);
    }

    /// <summary>Radians per second, positive counter-clockwise.</summary>
    public float AngularVelocity
    {
        get => Physics2D.GetAngularVelocity(Owner);
        set => Physics2D.SetAngularVelocity(Owner, value);
    }

    public void AddForce(Vector2 force) => Physics2D.AddForce(Owner, force);
    public void AddImpulse(Vector2 impulse) => Physics2D.AddImpulse(Owner, impulse);
    public void AddTorque(float torque) => Physics2D.AddTorque(Owner, torque);
    public void SetGravityScale(float scale) => Physics2D.SetGravityScale(Owner, scale);
}

/// <summary>Reference to a 2D collider on an entity. Drop an entity that has a
/// Collider 2D to link it.</summary>
public readonly struct Collider2DRef : IComponentRef
{
    public static string ComponentType => "Collider 2D";
    public Entity Owner { get; }
    public Collider2DRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    /// <summary>Switch the collider between solid and trigger (sensor).</summary>
    public void SetTrigger(bool trigger) => Physics2D.SetTrigger(Owner, trigger);
}

/// <summary>Reference to a UI Text element on an entity. Drop a canvas text entity
/// to link it, then drive its string from a script through <see cref="Text"/>.</summary>
public readonly struct UiTextRef : IComponentRef
{
    public static string ComponentType => "UI Text";
    public Entity Owner { get; }
    public UiTextRef(Entity owner) => Owner = owner;
    public bool IsValid => Owner.IsValid;

    /// <summary>The displayed string; assigning it updates the label next frame.</summary>
    public string Text
    {
        get => Ui.GetText(Owner);
        set => Ui.SetText(Owner, value);
    }
}
