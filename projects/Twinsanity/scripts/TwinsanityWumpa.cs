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
/// - Collection: once Crash's chest comes within ~1.6 units the fruit homes to him at high
///   speed (measured ~18 u/s over ~0.1 s), then flies to the HUD's wumpa counter
///   (COM_MOBILEWUMPA_PICKUP's CA_PickUpWumpa; the counter is visible in rig captures) and only
///   then increments it - TwinsanityHud pops the counter on the change.
/// - Crates release real wumpa objects (BASICCRATE's object list names object 1) that pop out
///   ballistically, settle, and join the idle motion.
/// </summary>
public sealed class TwinsanityWumpa
{
	private sealed class Fruit
	{
		public Entity Model;
		public Vector3 Base;      // resting position (origin at the fruit's base)
		public float Phase;       // spin/bob angle, degrees
		public State St = State.Idle;
		public Vector3 Velocity;  // ballistic pop-out from a crate
		public float GroundY;     // where a burst fruit settles
		public int Bounces;
		public float FlyT;        // seconds in the HUD fly
		public Vector3 FlyFrom;
	}

	private enum State
	{
		Idle,
		Ballistic,
		Homing,
		ToHud,
	}

	// Measured off the rig: 450 deg/s spin, 0.125-unit bob on the same phase, homing starts
	// inside ~1.6 units and covers the gap in about a tenth of a second.
	private const float SpinRate = 450.0f;
	private const float BobAmplitude = 0.125f;
	private const float HomingRadius = 1.6f;
	private const float HomingSpeed = 18.0f;
	private const float ChestHeight = 0.9f;   // where homed fruit meets Crash's body
	private const float BurstGravity = 22.0f; // the project's 2.5 gravity scale on ~8.8 m/s^2
	private const float FlyDuration = 0.25f;

	private readonly List<Fruit> _fruits = new();
	private readonly Action<int> _collect;

	public TwinsanityWumpa(Action<int> collect)
	{
		_collect = collect;
	}

	public int Count => _fruits.Count;

	/// <summary>Places one idle fruit. `position` is the level instance's origin; the model sits
	/// about a unit above it and bobs around that.</summary>
	public void Spawn(string modelPath, Vector3 position, float yawDegrees)
	{
		var f = new Fruit
		{
			Model = SpawnModel(modelPath, position, yawDegrees),
			Base = position + new Vector3(0.0f, 1.0f, 0.0f),
		};
		f.Phase = PhaseFor(position);
		_fruits.Add(f);
	}

	/// <summary>A broken crate releases `count` fruits that pop out and settle around `crateBase`
	/// (the crate's bottom-centre) before joining the idle motion.</summary>
	public void Burst(string modelPath, Vector3 crateBase, int count)
	{
		for (int i = 0; i < count; i++)
		{
			float a = MathF.PI * 2.0f * i / count + 0.5f;
			var f = new Fruit
			{
				Model = SpawnModel(modelPath, crateBase + new Vector3(0.0f, 0.9f, 0.0f), a * 57.3f),
				Base = crateBase,
				GroundY = crateBase.Y,
				St = State.Ballistic,
				Velocity = new Vector3(MathF.Cos(a) * 3.5f, 6.5f + 0.7f * (i % 3), MathF.Sin(a) * 3.5f),
				Bounces = 2,
			};
			_fruits.Add(f);
		}
	}

	public void Update(float deltaTime, Vector3 crashFeet)
	{
		Vector3 chest = crashFeet + new Vector3(0.0f, ChestHeight, 0.0f);
		for (int i = _fruits.Count - 1; i >= 0; i--)
		{
			Fruit f = _fruits[i];
			switch (f.St)
			{
				case State.Ballistic:
				{
					f.Velocity.Y -= BurstGravity * deltaTime;
					f.Base += f.Velocity * deltaTime;
					if (f.Base.Y <= f.GroundY && f.Velocity.Y < 0.0f)
					{
						f.Base.Y = f.GroundY;
						if (--f.Bounces <= 0)
						{
							f.St = State.Idle;
							f.Velocity = Vector3.Zero;
						}
						else
						{
							f.Velocity.Y *= -0.4f;
							f.Velocity.X *= 0.6f;
							f.Velocity.Z *= 0.6f;
						}
					}
					break;
				}
				case State.Idle:
				{
					f.Phase -= SpinRate * deltaTime; // engine space mirrors the game's +450 deg/s
					Vector2 toCrash = new(crashFeet.X - f.Base.X, crashFeet.Z - f.Base.Z);
					if (toCrash.LengthSquared() < HomingRadius * HomingRadius)
					{
						f.St = State.Homing;
					}
					break;
				}
				case State.Homing:
				{
					Vector3 to = chest - f.Base;
					float d = to.Length();
					if (d < 0.4f)
					{
						f.St = State.ToHud;
						f.FlyFrom = f.Base;
						f.FlyT = 0.0f;
						break;
					}
					f.Base += to * MathF.Min(1.0f, HomingSpeed * deltaTime / d);
					// Homed fruit holds its pose instead of continuing the idle spin.
					f.Model.Position = f.Base;
					continue;
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

			if (f.St is State.Idle or State.Ballistic)
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
		return e;
	}
}
