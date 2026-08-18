using System.Numerics;

namespace AetherCore;

// Typed wrappers for the engine's Behaviors components - the little transform and
// material drivers that BehaviorSystem ticks. Each is a thin veneer over
// ComponentAccess, so the field names live in exactly one place and the compiler
// checks the property names instead of the runtime checking strings.
//
// These follow the IComponentRef pattern, so a script can also declare one as a public
// field and get a drop-validated entity slot in the Inspector:
//
//     public SpinRef Turntable;   // drag any entity carrying a Spin onto it
//
// Reads are properties, writes are Set* METHODS - deliberately, not for lack of taste.
// These have to be structs to work as serialized inspector fields, and C# refuses to
// invoke a property setter on a struct returned by a property: `Self.Spin.Degrees = v`
// is CS1612, "cannot modify the return value". A settable property would compile only
// when you had first copied the ref into a local, so the obvious one-liner would fail
// with an error that explains nothing. `Self.Spin.SetDegreesPerSecond(v)` always works.
//
// All of them are safe on an entity that does not carry the component: reads return the
// declared fallback, writes return false.

/// <summary>Constant rotation, in degrees per second per axis.</summary>
public readonly struct SpinRef : IComponentRef
{
    public static string ComponentType => "Spin";
    public Entity Owner { get; }
    public SpinRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public Vector3 DegreesPerSecond => C.GetVector3("euler_deg_per_sec");
    public bool SetDegreesPerSecond(Vector3 value) => C.SetVector3("euler_deg_per_sec", value);
}

/// <summary>Vertical sine bob around the entity's authored position.</summary>
public readonly struct BobRef : IComponentRef
{
    public static string ComponentType => "Bob";
    public Entity Owner { get; }
    public BobRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public float Amplitude => C.GetFloat("amplitude");
    public bool SetAmplitude(float value) => C.SetFloat("amplitude", value);
    public float Frequency => C.GetFloat("frequency");
    public bool SetFrequency(float value) => C.SetFloat("frequency", value);
    public float Phase => C.GetFloat("phase");
    public bool SetPhase(float value) => C.SetFloat("phase", value);
}

/// <summary>Circular orbit around a world-space centre.</summary>
public readonly struct OrbitRef : IComponentRef
{
    public static string ComponentType => "Orbit";
    public Entity Owner { get; }
    public OrbitRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public Vector3 Center => C.GetVector3("center");
    public bool SetCenter(Vector3 value) => C.SetVector3("center", value);
    public float Radius => C.GetFloat("radius");
    public bool SetRadius(float value) => C.SetFloat("radius", value);
    public float DegreesPerSecond => C.GetFloat("speed_deg");
    public bool SetDegreesPerSecond(float value) => C.SetFloat("speed_deg", value);
    public float AngleDegrees => C.GetFloat("angle_deg");
    public bool SetAngleDegrees(float value) => C.SetFloat("angle_deg", value);
    public float YawOffsetDegrees => C.GetFloat("yaw_offset_deg");
    public bool SetYawOffsetDegrees(float value) => C.SetFloat("yaw_offset_deg", value);
    public float Height => C.GetFloat("height");
    public bool SetHeight(float value) => C.SetFloat("height", value);
}

/// <summary>Sine pulse applied to the entity's scale.</summary>
public readonly struct ScalePulseRef : IComponentRef
{
    public static string ComponentType => "Scale Pulse";
    public Entity Owner { get; }
    public ScalePulseRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public float Amplitude => C.GetFloat("amplitude");
    public bool SetAmplitude(float value) => C.SetFloat("amplitude", value);
    public float Frequency => C.GetFloat("frequency");
    public bool SetFrequency(float value) => C.SetFloat("frequency", value);
    public float Phase => C.GetFloat("phase");
    public bool SetPhase(float value) => C.SetFloat("phase", value);
}

/// <summary>Oscillates the material's emissive colour between two values.</summary>
public readonly struct MaterialPulseRef : IComponentRef
{
    public static string ComponentType => "Material Pulse";
    public Entity Owner { get; }
    public MaterialPulseRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public Vector3 EmissiveA => C.GetVector3("emissive_a");
    public bool SetEmissiveA(Vector3 value) => C.SetVector3("emissive_a", value);
    public Vector3 EmissiveB => C.GetVector3("emissive_b");
    public bool SetEmissiveB(Vector3 value) => C.SetVector3("emissive_b", value);
    public float Frequency => C.GetFloat("frequency");
    public bool SetFrequency(float value) => C.SetFloat("frequency", value);
}

/// <summary>Turns the entity to face a world-space point each frame.</summary>
public readonly struct LookAtRef : IComponentRef
{
    public static string ComponentType => "Look At";
    public Entity Owner { get; }
    public LookAtRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Add() => C.Add();
    public bool Remove() => C.Remove();

    public Vector3 Target => C.GetVector3("target");
    public bool SetTarget(Vector3 value) => C.SetVector3("target", value);
    public bool KeepUpright => C.GetBool("keep_upright");
    public bool SetKeepUpright(bool value) => C.SetBool("keep_upright", value);
}

/// <summary>
/// How an animation clip's root motion is applied. Attached by the animation system
/// rather than by hand, so this wrapper reads and toggles but does not add.
/// </summary>
public readonly struct RootMotionRef : IComponentRef
{
    public static string ComponentType => "Root Motion";
    public Entity Owner { get; }
    public RootMotionRef(Entity owner) => Owner = owner;

    private ComponentAccess C => new(Owner, ComponentType);
    public bool Exists => C.Exists;
    public bool Remove() => C.Remove();

    public bool Enabled => C.GetBool("enabled");
    public bool SetEnabled(bool value) => C.SetBool("enabled", value);
    public bool ApplyToTransform => C.GetBool("apply_to_transform");
    public bool SetApplyToTransform(bool value) => C.SetBool("apply_to_transform", value);
    public bool ApplyToPhysics => C.GetBool("apply_to_physics");
    public bool SetApplyToPhysics(bool value) => C.SetBool("apply_to_physics", value);
}
