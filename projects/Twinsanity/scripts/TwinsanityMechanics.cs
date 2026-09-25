using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// The earth worm's squash-launch (act_EARTH_WORM OnLand, COM_EARTH_WORM_SQUASHLAUNCH): when Crash
/// lands on a popped-up worm the worm plays its squash clip (slot 0x0C) and the script runs
/// ApplyVelocity(gravity 50, distance Y 10) on him - a launch that tops out 10 m above the take-off
/// under 50 m/s^2, which is Crash's own AirGravity. The worm actor (TwinsanityActors.UpdateWorm)
/// decides when Crash lands on it and calls TryLaunch.
/// </summary>
public static class MechanicsWorm
{
	private const float LaunchGravity = 50.0f;
	private const float LaunchHeight = 10.0f;

	/// <summary>Launch Crash off the worm at <paramref name="wormPos"/>. Returns false when he is not
	/// falling onto it (rising through it, or already launched this contact).</summary>
	public static bool TryLaunch(CrashPlayer player, Vector3 wormPos)
	{
		if (player.Velocity.Y > 0.0f)
		{
			return false;
		}
		player.Bounce(MathF.Sqrt(2.0f * LaunchGravity * LaunchHeight));
		return true;
	}
}

/// <summary>
/// Pushables: the objects whose spawn scripts give them SetContactRigid - act_WUMPA_NUT,
/// act_BEACH_BALL, act_MONKEY_ROCK and act_RIGID_CANNON. Crash walking into one pushes it ahead of
/// him (the behaviour scripts' IsPushingObject condition, which switches his walk/run to the push
/// clips a044/a045); balls and the nut roll on after he lets go, the cannon stops dead.
/// Each is a kinematic collision body the script drives, so Crash's controller is blocked by it
/// exactly as by scenery and follows it at the push speed.
/// </summary>
public sealed partial class TwinsanityActors
{
	private sealed class Pushable
	{
		public Entity Model;
		public Entity Body;
		public Vector3 Center;       // body centre (world)
		public Vector3 ModelOffset;  // model origin - body centre
		public float Radius;         // collision / contact radius
		public float GroundOffset;   // centre height above the ground below it
		public bool Rolls;
		public float RollDecel;
		public float PushRatio;
		public Vector3 Velocity;     // horizontal
		public Quaternion Roll = Quaternion.Identity;
		public Quaternion Base = Quaternion.Identity;
	}

	private readonly List<Pushable> _pushables = new();

	private const float CrashRadius = 0.4f;   // Beach.scene.toml capsule
	private const float CrashHeight = 1.95f;

	// Per family: collision radius (model bounds), rolls, roll-on deceleration (m/s^2), push speed
	// as a fraction of Crash's. Measured on the rig - see logs/mechanics/.
	private static (float Radius, bool Rolls, float RollDecel, float PushRatio)? PushableKind(string key) => key switch
	{
		"act_wumpa_nut" => (0.65f, true, 6.0f, 1.0f),
		"act_beach_ball" => (0.67f, true, 4.0f, 1.0f),
		"act_monkey_rock" => (0.46f, true, 8.0f, 1.0f),
		"act_rigid_cannon" => (1.4f, false, 0.0f, 1.0f),
		_ => null,
	};

	private bool TrySpawnPushable(string objectName, string model, Vector3 position, Vector3 eulerDegrees)
	{
		var kind = PushableKind(NameKey(objectName));
		if (kind == null)
		{
			return false;
		}
		var (radius, rolls, decel, ratio) = kind.Value;
		bool beachBall = model.Contains("_1.gltf", StringComparison.Ordinal) && NameKey(objectName) == "act_beach_ball";
		if (beachBall)
		{
			radius = 0.48f; // the small ball variant (act_BEACH_BALL1_1)
		}

		Entity e = World.Create();
		e.Name = objectName;
		e.AddTransform();
		e.Position = position;
		e.EulerDegrees = eulerDegrees;
		e.LoadModel(model);

		// Balls and the nut are modelled around their centre; the cannon's origin sits at its axle,
		// so its collision sphere is raised to cover the carriage.
		Vector3 center = rolls ? position : position + new Vector3(0.0f, 0.3f, 0.0f);
		Entity body = World.Create();
		body.Name = objectName + " Body";
		body.AddTransform();
		body.Position = center;
		Physics.AddSphereBody(body, radius, dynamic: false);
		Physics.SetMotionType(body, PhysicsMotionType.Kinematic);

		float ground = GroundBelow(center, radius, center.Y + 2.0f) ?? (center.Y - radius);
		_pushables.Add(new Pushable
		{
			Model = e,
			Body = body,
			Center = center,
			ModelOffset = position - center,
			Radius = radius,
			GroundOffset = center.Y - ground,
			Rolls = rolls,
			RollDecel = decel,
			PushRatio = ratio,
			Base = Quaternion.CreateFromYawPitchRoll(eulerDegrees.Y * MathF.PI / 180.0f, eulerDegrees.X * MathF.PI / 180.0f, eulerDegrees.Z * MathF.PI / 180.0f),
		});
		return true;
	}

	private void UpdatePushables(float dt, CrashPlayer player)
	{
		bool pushing = false;
		if (!player.Self.IsValid)
		{
			return;
		}
		Vector3 feet = player.Self.Position;
		Vector3 crashVel = player.Velocity;
		Vector3 flatVel = new(crashVel.X, 0.0f, crashVel.Z);
		float speed = flatVel.Length();

		foreach (Pushable p in _pushables)
		{
			Vector3 toObj = p.Center - feet;
			toObj.Y = 0.0f;
			float dist = toObj.Length();
			float contact = p.Radius + CrashRadius + 0.15f;
			bool overlapY = feet.Y < p.Center.Y + p.Radius * 0.8f && feet.Y + CrashHeight > p.Center.Y - p.Radius;
			if (dist < contact && dist > 1e-3f && overlapY && player.IsGrounded && speed > 0.5f)
			{
				Vector3 n = toObj / dist;
				float along = Vector3.Dot(flatVel, n);
				if (along > speed * 0.5f)
				{
					pushing = true;
					p.Velocity = n * along * p.PushRatio;
				}
			}
			else if (p.Velocity != Vector3.Zero)
			{
				float v = p.Velocity.Length();
				float nv = p.Rolls ? MathF.Max(0.0f, v - p.RollDecel * dt) : 0.0f;
				p.Velocity = nv > 0.0f ? p.Velocity * (nv / v) : Vector3.Zero;
			}
			if (p.Velocity == Vector3.Zero)
			{
				continue;
			}
			Move(p, p.Velocity * dt);
		}
		player.Pushing = pushing;
	}

	private void Move(Pushable p, Vector3 step)
	{
		float len = step.Length();
		Vector3 dir = step / len;
		// Stop at scenery: probe ahead from just outside the body so the ray cannot hit it.
		RaycastHit wall = Physics.Raycast(p.Center + dir * (p.Radius + 0.02f), dir, len + 0.05f);
		if (wall.DidHit && wall.Entity != p.Body)
		{
			p.Velocity = Vector3.Zero;
			return;
		}
		Vector3 next = p.Center + step;
		float? ground = GroundBelow(next, p.Radius, next.Y + 1.0f, p.Body);
		if (ground == null)
		{
			// Nothing below within reach (a drop): ponytail - no falling sim; it stops at the edge.
			p.Velocity = Vector3.Zero;
			return;
		}
		next.Y = ground.Value + p.GroundOffset;
		p.Center = next;
		if (p.Rolls)
		{
			Vector3 axis = Vector3.Normalize(Vector3.Cross(Vector3.UnitY, dir));
			p.Roll = Quaternion.Normalize(Quaternion.Concatenate(p.Roll, Quaternion.CreateFromAxisAngle(axis, len / p.Radius)));
		}
		p.Body.Position = next;
		p.Model.Position = next + p.ModelOffset;
		if (p.Rolls)
		{
			p.Model.EulerDegrees = EngineEuler(Matrix4x4.CreateFromQuaternion(Quaternion.Concatenate(p.Base, p.Roll)));
		}
	}

	// Ground height under a body: rays down just outside its rim. A hit on the body itself (the
	// kinematic body trails its target by a physics step) or anything above its centre (Crash's
	// capsule) is not ground. ponytail: four rim samples, highest wins - a ball resting on a crest
	// between them reads slightly low; upgrade to a shape cast that can filter the body out.
	private static float? GroundBelow(Vector3 center, float radius, float fromY, Entity self = default)
	{
		float? best = null;
		float r = radius + 0.05f;
		Span<Vector3> offsets = stackalloc Vector3[] { new(r, 0, 0), new(-r, 0, 0), new(0, 0, r), new(0, 0, -r) };
		foreach (Vector3 o in offsets)
		{
			RaycastHit hit = Physics.Raycast(new Vector3(center.X + o.X, fromY, center.Z + o.Z), -Vector3.UnitY, fromY - center.Y + radius + 1.5f);
			if (hit.DidHit && hit.Entity != self && hit.Position.Y < center.Y && (best == null || hit.Position.Y > best.Value))
			{
				best = hit.Position.Y;
			}
		}
		return best;
	}

	// Row-vector rotation -> the engine's Euler degrees (same convention as TwinsanityLevel.EulerOf).
	private static Vector3 EngineEuler(Matrix4x4 m)
	{
		const float deg = 180.0f / MathF.PI;
		float x = MathF.Asin(Math.Clamp(-m.M32, -1.0f, 1.0f));
		float y = MathF.Atan2(m.M31, m.M33);
		float z = MathF.Atan2(m.M12, m.M22);
		return new Vector3(x, y, z) * deg;
	}
}
