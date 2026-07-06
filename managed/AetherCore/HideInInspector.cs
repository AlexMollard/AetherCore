using System;

namespace AetherCore;

/// <summary>
/// Marks a public script field so it is NOT exposed in the inspector or
/// serialized with the scene. (Public fields of supported types are exposed by
/// default - this opts out.)
/// </summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class HideInInspectorAttribute : Attribute;
