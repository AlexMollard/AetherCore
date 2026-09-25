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
	// The rig shows the squash-launch giving a vertical velocity of exactly 30 (logs/mechanics/
	// rig_burst.json worm1/worm2): 9 m of rise under 50 m/s^2, which tops out level with the ledges
	// the worm columns of wumpa sit on (worm 2.33 -> ledge 12.1).
	private const float LaunchVelocity = 30.0f;

	/// <summary>Launch Crash off the worm at <paramref name="wormPos"/>. Returns false when he is not
	/// falling onto it (rising through it, or already launched this contact).</summary>
	public static bool TryLaunch(CrashPlayer player, Vector3 wormPos)
	{
		if (player.Velocity.Y > 0.0f)
		{
			return false;
		}
		player.Bounce(LaunchVelocity);
		return true;
	}
}

/// <summary>
/// Pushables: the objects whose spawn scripts give them SetContactRigid (a rigid-body contact
/// response) - act_WUMPA_NUT, act_BEACH_BALL, act_MONKEY_ROCK and act_RIGID_CANNON. Crash walking
/// into one pushes it (the behaviour scripts' IsPushingObject condition, which switches his walk
/// and run to the push clips a044/a045 - CrashPlayer.Pushing).
///
/// Measured on the rig (logs/mechanics/rig_burst.json, the nut's live centre at 0x00D54010):
///   - Contact holds Crash's feet 1.0-1.1 m from the nut's centre; the nut's centre rests 0.5615
///     above the ground (its script's SetLogicalRadius 0.56), not at the instance height.
///   - He is held up while it gets going: the nut goes from rest to his speed in ~12 frames
///     (about 40 m/s^2), then runs at his full speed (9 run, 2.5 walk) - pushing never slows him.
///   - Pushing at an angle moves it along the contact normal only.
///   - Let go, it rolls on, its speed decaying exponentially at 0.87/s on flat sand.
/// Each is a kinematic collision body the script drives, so Crash's controller is blocked by it
/// exactly as by scenery.
/// </summary>
public sealed partial class TwinsanityActors
{
	private sealed class Pushable
	{
		public Entity Model;
		public Entity Body;
		public Vector3 Center;       // body centre (world)
		public Vector3 ModelOffset;  // model origin - body centre
		public float Radius;         // collision radius
		public float RestHeight;     // centre height above the ground
		public bool Rolls;
		public float PushAccel;
		public float Damping;        // 1/s, exponential speed decay while free
		public Vector3 Velocity;     // horizontal
		public Quaternion Roll = Quaternion.Identity;
		public Quaternion Base = Quaternion.Identity;
	}

	private readonly List<Pushable> _pushables = new();

	private const float CrashRadius = 0.4f;   // Beach.scene.toml capsule
	private const float CrashHeight = 1.95f;
	// ponytail: how fast a pushed object closes its sideways offset from Crash's line (1/s). The rig
	// shows it tracking his line to ~1 cm; the rate itself is not observable, so it is set fast.
	private const float CenteringRate = 10.0f;
	// ponytail: the game's prop gravity is not in the extracted data; Crash's own AirGravity (50)
	// stands in, scaled by 5/7 for a rolling sphere. It only matters on slopes.
	private const float SlopeGravity = 50.0f * 5.0f / 7.0f;
	private const float RestSpeed = 0.05f;

	// Per family: collision radius (model extent - with our 0.4 capsule it reproduces the rig's
	// 1.0-1.1 m contact distance), centre height above ground (the script's SetLogicalRadius),
	// rolls, push acceleration (m/s^2; 80 reproduces the rig 12-frame ramp once the servo spends some of it holding the line), free-roll decay (1/s).
	private static (float Radius, float RestHeight, bool Rolls, float PushAccel, float Damping)? PushableKind(string key) => key switch
	{
		"act_wumpa_nut" => (0.65f, 0.5615f, true, 80.0f, 0.87f),
		// ponytail: the ball and rock reuse the nut's push/decay until measured; rest heights are the
		// rig's live centres (ball 0.594, rock 0.28).
		"act_beach_ball" => (0.67f, 0.594f, true, 80.0f, 0.87f),
		"act_monkey_rock" => (0.45f, 0.28f, true, 80.0f, 0.87f),
		"act_rigid_cannon" => (1.4f, 1.3f, false, 80.0f, 0.0f),
		_ => null,
	};

	private bool TrySpawnPushable(string objectName, string model, Vector3 position, Vector3 eulerDegrees)
	{
		string key = NameKey(objectName);
		var kind = PushableKind(key);
		if (kind == null)
		{
			return false;
		}
		var (radius, rest, rolls, accel, damping) = kind.Value;

		Entity e = World.Create();
		e.Name = objectName;
		e.AddTransform();
		e.EulerDegrees = eulerDegrees;
		e.LoadModel(model);

		// The balls and the nut are modelled around their centre, which the game keeps RestHeight
		// above the ground; the cannon keeps its placed height and origin.
		float ground = Ground(position, radius, position.Y + 1.0f, default, default, out _) ?? position.Y - rest;
		Vector3 center = rolls ? new Vector3(position.X, ground + rest, position.Z) : position + new Vector3(0.0f, 0.3f, 0.0f);
		Vector3 modelOffset = rolls ? Vector3.Zero : position - center;
		e.Position = center + modelOffset;

		Entity body = World.Create();
		body.Name = objectName + " Body";
		body.AddTransform();
		body.Position = center;
		Physics.AddSphereBody(body, radius, dynamic: false);
		Physics.SetMotionType(body, PhysicsMotionType.Kinematic);

		_pushables.Add(new Pushable
		{
			Model = e,
			Body = body,
			Center = center,
			ModelOffset = modelOffset,
			Radius = radius,
			RestHeight = center.Y - ground,
			Rolls = rolls,
			PushAccel = accel,
			Damping = damping,
			Base = Quaternion.CreateFromYawPitchRoll(eulerDegrees.Y * MathF.PI / 180.0f, eulerDegrees.X * MathF.PI / 180.0f, eulerDegrees.Z * MathF.PI / 180.0f),
		});
		return true;
	}

	private void UpdatePushables(float dt, CrashPlayer player)
	{
		if (!player.Self.IsValid || dt <= 0.0f)
		{
			return;
		}
		bool pushing = false;
		Vector3 feet = player.Self.Position;
		Vector3 flatVel = player.Velocity with { Y = 0.0f };
		float speed = flatVel.Length();

		foreach (Pushable p in _pushables)
		{
			Vector3 toObj = (p.Center - feet) with { Y = 0.0f };
			float dist = toObj.Length();
			bool overlapY = feet.Y < p.Center.Y + p.Radius * 0.8f && feet.Y + CrashHeight > p.Center.Y - p.Radius;
			bool pushed = false;
			if (dist < p.Radius + CrashRadius + 0.2f && dist > 1e-3f && overlapY && player.IsGrounded && speed > 0.5f)
			{
				Vector3 n = toObj / dist;
				if (Vector3.Dot(flatVel, n) > speed * 0.5f)
				{
					// The rig keeps the pushed object dead ahead of him, at his speed: drive it at the
					// point his line meets contact distance (servo, closed at CenteringRate). The
					// speed only ever rises here, up to what he pushes at - when he brakes, turns or
					// lets go, the object keeps its speed and coasts (rig: releasing at 9.5 leaves it
					// coasting at 0.87/s; it never follows his braking back down).
					Vector3 dir = flatVel / speed;
					Vector3 hold = feet + dir * (p.Radius + CrashRadius + 0.05f);
					Vector3 delta = flatVel + (hold - p.Center) * CenteringRate - p.Velocity;
					delta.Y = 0.0f;
					float max = p.PushAccel * dt;
					p.Velocity += delta.Length() > max ? Vector3.Normalize(delta) * max : delta;
					// It never outruns him, and an excess (a slope kick, a teleport) bleeds off.
					float v = p.Velocity.Length();
					if (v < speed)
					{
						p.Velocity = Vector3.Normalize(p.Velocity + dir * 0.01f) * speed;
					}
					else if (v > speed + 0.5f)
					{
						p.Velocity *= MathF.Max(speed, v - p.PushAccel * dt) / v;
					}
					pushed = pushing = true;
				}
			}
			if (!pushed)
			{
				p.Velocity *= p.Rolls ? MathF.Exp(-p.Damping * dt) : 0.0f;
			}
			Step(p, dt, player.Self);
		}
		player.Pushing = pushing;
	}

	private void Step(Pushable p, float dt, Entity crash)
	{
		float? groundHere = Ground(p.Center, p.Radius, p.Center.Y + 1.0f, p.Body, crash, out Vector2 grad);
		if (p.Rolls && groundHere != null)
		{
			// Downhill pull on a rolling sphere. Gradient is clamped: a rim ray that clipped
			// something tall (a character, a crate) must not fling the object.
			grad = Vector2.Clamp(grad, new Vector2(-0.5f), new Vector2(0.5f));
			p.Velocity -= new Vector3(grad.X, 0.0f, grad.Y) * SlopeGravity * dt;
		}
		if (p.Velocity.LengthSquared() < RestSpeed * RestSpeed)
		{
			p.Velocity = Vector3.Zero;
			return;
		}
		Vector3 step = p.Velocity * dt;
		float len = step.Length();
		Vector3 dir = step / len;
		// Stop at scenery: probe ahead from just outside the body so the ray cannot hit it.
		RaycastHit wall = Physics.Raycast(p.Center + dir * (p.Radius + 0.02f), dir, len + 0.05f);
		if (wall.DidHit && wall.Entity != p.Body && wall.Normal.Y < 0.7f)
		{
			p.Velocity = Vector3.Zero;
			return;
		}
		Vector3 next = p.Center + step;
		float? ground = Ground(next, p.Radius, next.Y + 1.0f, p.Body, crash, out _);
		if (ground == null || ground.Value < p.Center.Y - p.RestHeight - 1.0f)
		{
			// ponytail: no falling sim - a pushable stops at a drop instead of going over it.
			p.Velocity = Vector3.Zero;
			return;
		}
		next.Y = ground.Value + p.RestHeight;
		p.Center = next;
		p.Body.Position = next;
		p.Model.Position = next + p.ModelOffset;
		if (p.Rolls)
		{
			Vector3 axis = Vector3.Normalize(Vector3.Cross(Vector3.UnitY, dir));
			p.Roll = Quaternion.Normalize(Quaternion.Concatenate(p.Roll, Quaternion.CreateFromAxisAngle(axis, len / p.Radius)));
			p.Model.EulerDegrees = EngineEuler(Matrix4x4.CreateFromQuaternion(Quaternion.Concatenate(p.Base, p.Roll)));
		}
	}

	// Ground under a body: rays down just outside its rim, +x -x +z -z. A hit on the body itself (the
	// kinematic body trails its target by a physics step) or anything above its centre (Crash's
	// capsule) is not ground. Returns the highest hit and the height gradient (dh/dx, dh/dz).
	// ponytail: four rim samples - a ball resting on a crest between them reads slightly low; upgrade
	// to a shape cast that can filter the body out.
	private static float? Ground(Vector3 center, float radius, float fromY, Entity self, Entity crash, out Vector2 grad)
	{
		float r = radius + 0.05f;
		Span<float> h = stackalloc float[4];
		Span<Vector2> offsets = stackalloc Vector2[] { new(r, 0), new(-r, 0), new(0, r), new(0, -r) };
		float? best = null;
		for (int i = 0; i < 4; i++)
		{
			h[i] = float.NaN;
			RaycastHit hit = Physics.Raycast(new Vector3(center.X + offsets[i].X, fromY, center.Z + offsets[i].Y), -Vector3.UnitY, fromY - center.Y + radius + 1.5f);
			bool ignored = hit.Entity == self || hit.Entity == crash;
			if (hit.DidHit && !ignored && hit.Position.Y < center.Y)
			{
				h[i] = hit.Position.Y;
				best = best == null ? h[i] : MathF.Max(best.Value, h[i]);
			}
		}
		grad = new Vector2(
			float.IsNaN(h[0]) || float.IsNaN(h[1]) ? 0.0f : (h[0] - h[1]) / (2.0f * r),
			float.IsNaN(h[2]) || float.IsNaN(h[3]) ? 0.0f : (h[2] - h[3]) / (2.0f * r));
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
