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
