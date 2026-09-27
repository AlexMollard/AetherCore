using System;
using System.Collections.Generic;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>
/// Wumpa fruit pickups: spawning, the idle motion, collection and the burst a broken crate
/// releases. Behaviour measured against the original on the rig (logs/wumpa/):
/// - Idle: the fruit spins about its local Y at 450 deg/s while bobbing 0.125 units on a sine
///   locked to the spin phase (base y + 0.125 * sin(phase)); the render matrices were sampled
///   from EE RAM at ~27 Hz (logs/wumpa/rig-wumpa-samples.txt). The phase differs per instance;
///   here it is derived from the position. The engine mirrors game space, so the visible spin
///   runs the opposite way to the game's +450 deg/s.
/// - Collection (logs/beachfeel/HANDOFF.md, rigpick.py): the fruit reacts once its origin comes
///   within 2.0 of a vertical segment from Crash's feet to a metre above them, then flies to his
///   feet with v += (40 - v) * 0.17 per 50 Hz frame (1.9 m in 5 frames), then to the HUD's wumpa
///   counter (COM_MOBILEWUMPA_PICKUP's CA_PickUpWumpa; the counter is visible in rig captures) and
///   only then increments it - TwinsanityHud pops the counter on the change.
/// - Crates release real wumpa objects (BASICCRATE's object list names object 1): each appears at
///   the crate base + (U±0.8, 0.35, U±0.8) and idles there, bob starting downward - no pop-out
///   (Command_CreateCrateContents_Run, rig strips logs/beachfeel/rig_strip_*.png).
/// - The model's mesh already sits 0.61-1.41 above its origin, and the rig renders the origin at
///   instance + bob (rig-wumpa-samples.txt: -18.991, 0.01..0.14, -27.777 for the instance at y 0.01).
/// </summary>
public sealed class TwinsanityWumpa
{
	private sealed class Fruit
	{
		public Entity Model;
		public Vector3 Base;      // resting position (origin at the fruit's base)
		public float Phase;       // spin/bob angle, degrees
		public State St = State.Idle;
		public float Speed;       // homing speed
		public float FlyT;        // seconds in the HUD fly
		public Vector3 FlyFrom;
	}

	private enum State
	{
		Idle,
		Homing,
		ToHud,
	}

	// Measured off the rig: 450 deg/s spin, 0.125-unit bob on the same phase; the pickup capsule
	// and the homing law are from logs/beachfeel/HANDOFF.md.
	private const float SpinRate = 450.0f;
	private const float BobAmplitude = 0.125f;
	private const float PickupRadius = 2.0f;
	private const float PickupHeight = 1.0f;   // the capsule's segment: feet to feet + 1
	private const float HomingTopSpeed = 40.0f;
	private const float HomingRate = 9.316f;   // -ln(1 - 0.17) * 50: the per-frame 0.17 in continuous time
	private const float CollectDistance = 0.25f;
	private const float SpillSpread = 0.8f;
	private const float SpillHeight = 0.35f;
	private const float FlyDuration = 0.25f;

	private static readonly System.Random s_spill = new(0x5911);

	private readonly List<Fruit> _fruits = new();
	private readonly Action<int> _collect;

	public TwinsanityWumpa(Action<int> collect)
	{
		_collect = collect;
	}

	public int Count => _fruits.Count;

	/// <summary>Places one idle fruit. `position` is the level instance's origin, where the
	/// model's origin sits (the mesh itself is modelled 0.6-1.4 above it), bobbing around it.</summary>
	public void Spawn(string modelPath, Vector3 position, float yawDegrees)
	{
		var f = new Fruit
		{
			Model = SpawnModel(modelPath, position, yawDegrees),
			Base = position,
		};
		f.Phase = PhaseFor(position);
		_fruits.Add(f);
	}

	/// <summary>Adopts a fruit the bake already placed: the entity carries the model at the
	/// instance origin, so the bind state is derived from it exactly as a spawn would.</summary>
	public void Bind(Entity existing, Vector3 position)
	{
		var f = new Fruit
		{
			Model = existing,
			Base = position,
		};
		f.Phase = PhaseFor(position);
		_fruits.Add(f);
	}

	/// <summary>A broken crate releases `count` fruits scattered around `crateBase` (the crate's
	/// origin), idling where they appear.</summary>
	public void Burst(string modelPath, Vector3 crateBase, int count)
	{
		for (int i = 0; i < count; i++)
		{
			Vector3 at = crateBase + new Vector3(
				(s_spill.NextSingle() * 2.0f - 1.0f) * SpillSpread,
				SpillHeight,
				(s_spill.NextSingle() * 2.0f - 1.0f) * SpillSpread);
			_fruits.Add(new Fruit { Model = SpawnModel(modelPath, at, 0.0f), Base = at });
		}
	}

	// The pickup capsule: the fruit origin within PickupRadius of the segment feet..feet + 1.
	private static bool InReach(Vector3 fruit, Vector3 feet)
	{
		float up = Math.Clamp(fruit.Y - feet.Y, 0.0f, PickupHeight);
		return Vector3.DistanceSquared(fruit, feet + new Vector3(0.0f, up, 0.0f)) < PickupRadius * PickupRadius;
	}

	public void Update(float deltaTime, Vector3 crashFeet)
	{
		float blend = MathF.Exp(-HomingRate * deltaTime);
		for (int i = _fruits.Count - 1; i >= 0; i--)
		{
			Fruit f = _fruits[i];
			switch (f.St)
			{
				case State.Idle:
				{
					f.Phase -= SpinRate * deltaTime; // engine space mirrors the game's +450 deg/s
					Vector3 origin = f.Base + new Vector3(0.0f, BobAmplitude * MathF.Sin(f.Phase * MathF.PI / 180.0f), 0.0f);
					if (InReach(origin, crashFeet))
					{
						f.St = State.Homing;
						f.Base = origin;
						f.Speed = 0.0f;
					}
					break;
				}
				case State.Homing:
				{
					f.Speed = HomingTopSpeed - (HomingTopSpeed - f.Speed) * blend;
					Vector3 to = crashFeet - f.Base;
					float d = to.Length();
					float stepLen = f.Speed * deltaTime;
					if (d - stepLen < CollectDistance)
					{
						f.Base += d > 0.0f ? to * (MathF.Max(0.0f, d - CollectDistance) / d) : Vector3.Zero;
						f.St = State.ToHud;
						f.FlyFrom = f.Base;
						f.FlyT = 0.0f;
						break;
					}
					f.Base += to * (stepLen / d);
					break;
				}
				case State.ToHud:
				{
					f.FlyT += deltaTime;
					Vector3 target = HudTarget();
					float t = MathF.Min(1.0f, f.FlyT / FlyDuration);
					f.Base = Vector3.Lerp(f.FlyFrom, target, t * t); // ease-in, like the original's snap to the counter
					if (f.FlyT >= FlyDuration || Vector3.DistanceSquared(f.Base, target) < 0.05f)
					{
						_collect(1);
						f.Model.Destroy();
						_fruits.RemoveAt(i);
						continue;
					}
					break;
				}
			}

			if (f.St == State.Idle)
			{
				f.Model.Position = f.Base + new Vector3(0.0f, BobAmplitude * MathF.Sin(f.Phase * MathF.PI / 180.0f), 0.0f);
				f.Model.EulerDegrees = new Vector3(0.0f, -f.Phase, 0.0f);
			}
			else
			{
				f.Model.Position = f.Base;
			}
		}
	}

	// The counter sits in the HUD's top-left; a fixed point ahead of the camera towards it
	// gives the fruit a screen path that matches without needing the canvas projection.
	// ponytail: approximate HUD fly target - upgrade = unproject TwinsanityHud.WumpaCounterScreen
	// through the camera once the canvas size is reachable from scripts.
	private static Vector3 HudTarget()
	{
		Entity cam = Camera.Main;
		Vector3 forward = Camera.GetForward(cam);
		Vector3 right = Camera.GetRight(cam);
		Vector3 up = Vector3.Cross(right, forward);
		return cam.Position + forward * 2.2f - right * 0.8f + up * 0.8f;
	}

	// Distinct idle phases per instance, as on the rig; derived from the position so a
	// reload reproduces the same field of fruit.
	private static float PhaseFor(Vector3 position)
	{
		return (position.X * 73.0f + position.Z * 131.0f) % 360.0f;
	}

	private static Entity SpawnModel(string modelPath, Vector3 position, float yawDegrees)
	{
		Entity e = World.Create();
		e.Name = "Wumpa";
		e.AddTransform();
		e.Position = position;
		e.EulerDegrees = new Vector3(0.0f, yawDegrees, 0.0f);
		e.LoadModel(modelPath);
		// The original lays a soft blob under each fruit (logs/wumpa/rig/idle_00.png), never a
		// sun-projected silhouette. Cast into the shadow maps, a fruit hovering a metre up threw a
		// detached shadow that a distant cascade quantised into a dark blocky speck - rows of them
		// along every wumpa trail (logs/artifacts/specks_sheet.png).
		for (int i = 0; i < e.ChildCount; i++)
		{
			MeshRenderer.SetCastShadows(e.GetChild(i), false);
		}
		return e;
	}
}
