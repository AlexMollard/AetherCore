using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>
/// One-shot props: objects whose behaviour script plays a clip once in reaction to an event and
/// then holds its end pose (DoAnim with no loop), never as an idle loop. Each rests on the first
/// frame of its first clip until its cue, as the original shows the unanimated model.
/// Scripts dumped with logs/triggers/dump.ps1 (logs/triggers/dump.txt):
/// - act_TRAINING_EXPLODING_IDOL_HEAD (the totems): COM_TRAINING_EXPLODING_IDOL_HEAD_DEFAULT waits
///   for message 169 from its TRIGGER part, which sends it when damaged by an explosion; plays
///   a001, then a002 on the next explosion.
/// - act_TRAINING_FALLING_LOG (the falling tree): COM_TRAINING_FALLING_LOG_DAMAGED plays a001 when
///   damaged by an explosion (the beach one stands 2 m from a TNT crate); subtype 10 starts down.
/// - act_SEAPILLAR (rising pillars): COM_SEAPILLAR_DEFAULT plays a001 once global progression
///   reaches 2.
/// - act_WUMPA_TREE: COM_WUMPA_TREE_DEFAULT plays a001 at spawn for subtype 20, otherwise shakes
///   once (a002, a005 for subtype 1, a004 for subtype 2) when Crash comes within 2 m.
/// </summary>
public sealed partial class TwinsanityActors
{
	private enum PropCue { None, Explosion, Proximity }

	private sealed class OneShot
	{
		public Actor Actor = null!;
		public PropCue Cue;
		public int[] Clips = Array.Empty<int>(); // played in order, one per cue
		public string[] ClipNames = Array.Empty<string>();
		public int Next;
		public float Remaining = -1.0f; // seconds left of the clip playing now; -1 = resting
		public string Playing = "";
		public readonly List<PropHull> Hulls = new();
	}

	// One of the object's disc collision hulls (GI_CollisionData, exported by tw-extract to
	// <model>.hulls.json): a static convex body at the rest pose, swapped for the pose the prop
	// holds once a clip that moves it ends - the fallen beach tree is the bridge Crash walks.
	private sealed class PropHull
	{
		public Entity Body;
		public string Rest = "";
		public readonly Dictionary<string, string> Clips = new();
	}

	private readonly List<OneShot> _oneShots = new();

	// act_GLOBAL_BOMB (COM_GLOBAL_BOMB_DEFAULT): a spin primes it (s4 AgentWasSpun -> s2
	// COM_GLOBAL_BOMB_PRIMED), and 1 s later (s2 TimeInUnit 1) COM_GLOBAL_BOMB_DAMAGED explodes it
	// with CreateDamage radius 3 - the explosion that knocks the idol heads over.
	private sealed class Bomb
	{
		public Actor Actor = null!;
		public float Fuse = -1.0f; // seconds to the explosion once primed
		public Vector3 Velocity;
	}

	private readonly List<Bomb> _bombs = new();
	private const float BombFuse = 1.0f;
	private const float BombDamageRadius = 3.0f;
	private const float BombSpinReach = 1.5f;
	// ponytail: the bomb is a rigid body the spin knocks away; its launch speed and rolling drag
	// are not in the scripts. Tuned to the rig; no collision while rolling (flat ground only).
	private const float BombKickSpeed = 8.0f;
	private const float BombDrag = 2.0f;

	// act_RIGID_CANNON: belly-flopping its red button fires a GLOBAL_BOMB (the cannon's object list
	// holds it) that COM_GLOBAL_BOMB_DEFAULT subtype 7 launches with cmd193(.., 8.0, .., 20.0) and that
	// explodes where it lands (s12 TouchingTerrain) - on the CannonPuzzle idol heads.
	private sealed class Cannonball
	{
		public Entity Model;
		public Vector3 Velocity;
		public float Floor; // explode when it falls below this height
	}

	private readonly List<Cannonball> _cannonballs = new();
	private bool _slamHandled;
	// ponytail: cmd193's 8.0 / 20.0 read as launch up-speed / forward speed (m/s); gravity, the
	// muzzle offset and the button reach are not in the scripts. Upgrade path: measure a shot on
	// the rig (the camera faces inland from the cannon, so it needs a free-camera capture).
	private const float CannonUpSpeed = 8.0f;
	private const float CannonForwardSpeed = 20.0f;
	private const float CannonballGravity = 9.8f;
	private const float CannonButtonReach = 2.0f;
	private const string CannonballModel = "project://assets/models/objects/act_GLOBAL_BOMB/act_GLOBAL_BOMB_0.gltf";

	// ponytail: the original's global progression counter (condition GlobalProgression) lives in
	// the save; the port always starts a new game, where it is 0. Upgrade path: a save system.
	private const int GlobalProgression = 0;
	// COM_WUMPA_TREE_DEFAULT s3: MeToFocusSqrDist <= 4 with Crash as the focus.
	private const float WumpaTreeShakeRadius = 2.0f;

	// Sets up a one-shot prop; false when the object is not one.
	private bool SetupOneShot(Actor a, string objectName, uint subtype, string model)
	{
		string n = NameKey(objectName);
		Entity e = a.Model;
		OneShot s = new() { Actor = a };
		bool playNow = false;
		bool startDone = false;
		if (n.StartsWith("act_training_exploding_idol_head"))
		{
			s.Cue = PropCue.Explosion;
			s.ClipNames = new[] { "a001", "a002" };
		}
		else if (n.StartsWith("act_training_falling_log"))
		{
			s.Cue = PropCue.Explosion;
			s.ClipNames = new[] { "a001" };
			startDone = subtype == 10;
		}
		else if (n.StartsWith("act_seapillar"))
		{
			s.Cue = PropCue.None; // rises only by progression
			s.ClipNames = new[] { "a001" };
			startDone = GlobalProgression >= 2;
		}
		else if (n.StartsWith("act_wumpa_tree") || n.StartsWith("old_act_wumpa_tree"))
		{
			s.Cue = PropCue.Proximity;
			if (subtype == 20)
			{
				s.ClipNames = new[] { "a001" };
				playNow = true;
			}
			else if (subtype is 0 or 1 or 2 or 3)
			{
				s.ClipNames = new[] { subtype == 1 ? "a005" : subtype == 2 ? "a004" : "a002" };
			}
			// Other subtypes (10-12, the farmer cutscene trees) wait for a cutscene message.
		}
		else if (n.StartsWith("act_generic_grey_stone_door") || n.StartsWith("act_tiki_mon"))
		{
			// COM_GENERIC_GREY_STONE_DOOR_DEFAULT (a001) and COM_TIKI_MON_INIT (a007) play their clip
			// at spawn with DoAnim flags 0x20FF1 / 0xA0FF1: loop nibble (bits 12-15) 0 = play once,
			// the same as every one-shot above, while every idle loop in the hub scripts has it set
			// (chicken 0x3FF1, butterfly 0x5FF1, worm 0x2FF1 - logs/triggers/dump-loops.txt).
			s.ClipNames = new[] { n.StartsWith("act_tiki_mon") ? "a007" : "a001" };
			playNow = true;
		}
		else if (n.StartsWith("act_global_bomb"))
		{
			_bombs.Add(new Bomb { Actor = a });
			return true;
		}
		else if (objectName.Equals("act_EARTH_NATIVE_SLEDGE", StringComparison.OrdinalIgnoreCase))
		{
			// The rigid body settles onto its chute, which is rigid-only collision (surface 25, not
			// exported), so it rests where the rig measured it rather than at its instance height.
			e.Position += SledSettle;
			LoadHulls(s, model);
			_sleds.Add(new Sled { Shot = s, Start = e.Position, Yaw = e.EulerDegrees.Y });
			// ponytail: the ride replays the beach chute's rig track, so only the beach sledge rides;
			// the bossarea copy (act_EARTH_NATIVE_SLEDGE1) is an ordinary solid prop until measured.
			return true;
		}
		else
		{
			return false;
		}

		s.ClipNames = Array.FindAll(s.ClipNames, c => Animation.Find(e, c) >= 0);
		s.Clips = Array.ConvertAll(s.ClipNames, c => Animation.Find(e, c));
		LoadHulls(s, model);
		SetLooping(e, false);
		// Rest on frame 0 of the first cue clip (or of the model's first clip when the prop has no
		// cue here, e.g. the farmer-cutscene wumpa trees): the original shows it unanimated.
		int restClip = s.Clips.Length > 0 ? s.Clips[0] : a.IdleClip;
		if (restClip >= 0)
		{
			Animation.SetClip(e, restClip);
			Animation.SetTime(e, 0.0f);
			Animation.SetPlaybackSpeed(e, 0.0f);
		}
		if (s.Clips.Length == 0)
		{
			_oneShots.Add(s);
			return true;
		}
		if (startDone)
		{
			// Already played before Crash arrived: hold the end pose.
			Animation.SetTime(e, Animation.ClipDuration(e));
			s.Next = s.Clips.Length;
			HoldHulls(s, s.ClipNames[^1]);
		}
		else if (playNow)
		{
			PlayOnce(s);
		}
		_oneShots.Add(s);
		return true;
	}

	/// <summary>
	/// An explosion (TNT / Nitro crate) at <paramref name="center"/>: every explosion-cued prop
	/// within <paramref name="radius"/> plays its next clip once.
	/// </summary>
	public void Explosion(Vector3 center, float radius)
	{
		foreach (OneShot s in _oneShots)
		{
			if (s.Cue == PropCue.Explosion && s.Actor.Alive && InBlast(center, radius, s.Actor.Model.Position))
			{
				PlayOnce(s);
			}
		}
	}

	// Explosion-cued props are tall (idol head: joint1 at 5.5 m, +-2.4 m; tree ~16 m), so a blast
	// counts along the prop's upright extent: within PropReach + radius sideways, above its base.
	private const float PropReach = 2.5f;
	private const float PropHeight = 8.0f;

	private static bool InBlast(Vector3 center, float radius, Vector3 prop)
	{
		Vector3 d = center - prop;
		return new Vector2(d.X, d.Z).Length() < PropReach + radius && d.Y > -radius && d.Y < PropHeight + radius;
	}

	private void UpdateBombs(float dt, Vector3 crashPos)
	{
		foreach (Bomb b in _bombs)
		{
			if (!b.Actor.Alive)
			{
				continue;
			}
			Entity e = b.Actor.Model;
			Vector3 p = e.Position;
			if (b.Fuse < 0.0f)
			{
				Vector3 away = p - crashPos;
				away.Y = 0.0f;
				float dist = away.Length();
				if (_player != null && _player.IsSpinning && dist < BombSpinReach && MathF.Abs(crashPos.Y - p.Y) < 1.5f)
				{
					b.Fuse = BombFuse;
					b.Velocity = (dist > 0.001f ? away / dist : Vector3.UnitZ) * BombKickSpeed;
				}
				continue;
			}
			e.Position = p + b.Velocity * dt;
			b.Velocity *= MathF.Max(0.0f, 1.0f - BombDrag * dt);
			b.Fuse -= dt;
			if (b.Fuse >= 0.0f)
			{
				continue;
			}
			Vector3 center = e.Position + new Vector3(0.0f, 0.5f, 0.0f);
			CrateFx.Exploded(e.Position, 5);
			Explosion(center, BombDamageRadius);
			if (Vector3.Distance(center, crashPos + new Vector3(0.0f, 0.9f, 0.0f)) < BombDamageRadius)
			{
				_host?.DamagePlayer(center, DeathKind.Explode);
			}
			b.Actor.Alive = false;
			e.Destroy();
		}
		_bombs.RemoveAll(b => !b.Actor.Alive);
	}

	private void UpdateCannons(float dt, Vector3 crashPos)
	{
		// One shot per belly-flop landing on the cannon (COM_RIGID_CANNON_BUTTON_ACTIVATED on
		// OnGettingBodyslamAttacked / OnLand).
		bool slamLanded = _player != null && _player.IsSlamming && _player.IsGrounded;
		if (!slamLanded)
		{
			_slamHandled = false;
		}
		else if (!_slamHandled)
		{
			_slamHandled = true;
			foreach (Pushable p in _pushables)
			{
				Vector3 origin = p.Model.Position;
				Vector3 flat = crashPos - origin;
				flat.Y = 0.0f;
				if (NameKey(p.Model.Name) != "act_rigid_cannon" || flat.Length() > CannonButtonReach || crashPos.Y < origin.Y + 1.0f)
				{
					continue;
				}
				float yaw = p.Model.EulerDegrees.Y * MathF.PI / 180.0f;
				Vector3 forward = new(MathF.Sin(yaw), 0.0f, MathF.Cos(yaw));
				Entity ball = World.Create();
				ball.Name = "Cannonball";
				ball.MarkTransient();
				ball.AddTransform();
				ball.Position = origin + forward * 3.1f + new Vector3(0.0f, 2.3f, 0.0f); // barrel mouth
				ball.LoadModel(CannonballModel);
				_cannonballs.Add(new Cannonball
				{
					Model = ball,
					Velocity = forward * CannonForwardSpeed + new Vector3(0.0f, CannonUpSpeed, 0.0f),
					Floor = origin.Y - 3.0f,
				});
			}
		}

		foreach (Cannonball c in _cannonballs)
		{
			c.Velocity.Y -= CannonballGravity * dt;
			Vector3 p = c.Model.Position + c.Velocity * dt;
			c.Model.Position = p;
			bool hit = p.Y < c.Floor;
			foreach (OneShot s in _oneShots)
			{
				if (s.Cue == PropCue.Explosion && InBlast(p, 0.0f, s.Actor.Model.Position))
				{
					hit = true;
				}
			}
			if (!hit)
			{
				continue;
			}
			CrateFx.Exploded(p - new Vector3(0.0f, 0.5f, 0.0f), 5);
			Explosion(p, BombDamageRadius);
			c.Model.Destroy();
			c.Velocity = new Vector3(float.NaN);
		}
		_cannonballs.RemoveAll(c => float.IsNaN(c.Velocity.X));
	}

	// act_EARTH_NATIVE_SLEDGE (COM_EARTH_NATIVE_SLEDGE_DEFAULT): a rigid body on a rigid-only chute
	// that starts sliding once Crash stands on it, carries him down the chute, off the ramp and over
	// the water to the clear-gem island, stops, then breaks (DoParticle/DoSound) and DestroyMe.
	// Measured on the rig (logs/traversal/sled_rig.csv, 60 Hz): he stands 0.55 above the settled
	// sledge's origin, rides straight along its facing, leaves the ramp 1.7 s in at 49.3 m/s forward
	// and 19.05 m/s up, flies under 50.4 m/s^2, keeps 0.43 of his speed on landing, then brakes at
	// 34 m/s^2 and stands on the stopped sledge ~1.6 s before it breaks.
	private sealed class Sled
	{
		public OneShot Shot = null!; // the model and its disc hulls
		public Vector3 Start;        // settled origin
		public float Yaw;
		public float T = -1.0f;      // seconds into the ride; -1 = waiting for Crash
		public Vector3 Feet;
		public Vector3 Velocity;
		public bool Flying, Sliding;
		public float Hold;
	}

	private readonly List<Sled> _sleds = new();
	private static readonly Vector3 SledSettle = new(0.0f, -0.63f, 0.0f); // rig feet 17.71 - hull top 0.55 - instance 17.79
	private const float SledTop = 0.55f;      // hull top above the origin: where Crash's feet ride
	private const float SledBottom = -0.05f;  // hull bottom
	private const float SledHalfWidth = 1.26f, SledHalfLength = 2.01f;
	// ponytail: the chute is rigid-only collision the port does not simulate, so its run (t, drop,
	// forward) replays the rig's feet track at 0.1 s; upgrade path: export surface 25 as a
	// rigid-only body and slide the sledge on it.
	private static readonly (float T, float Drop, float Forward)[] SledChute =
	{
		(0.0f, 0.00f, 0.00f), (0.1f, -0.02f, 0.13f), (0.2f, -0.04f, 0.34f), (0.3f, -0.27f, 0.71f),
		(0.4f, -0.46f, 1.19f), (0.5f, -0.63f, 1.75f), (0.6f, -1.01f, 2.43f), (0.7f, -1.73f, 3.39f),
		(0.8f, -2.49f, 4.79f), (0.9f, -3.47f, 6.52f), (1.0f, -4.52f, 8.61f), (1.1f, -5.72f, 11.14f),
		(1.2f, -7.13f, 14.11f), (1.3f, -8.81f, 17.64f), (1.4f, -10.77f, 21.90f), (1.5f, -12.55f, 26.32f),
		(1.6f, -13.97f, 30.82f), (1.7f, -13.88f, 35.27f),
	};
	private const float SledLaunchForward = 49.3f, SledLaunchUp = 19.05f, SledGravity = 50.4f;
	private const float SledLandKeep = 0.43f, SledBrake = 34.0f, SledBreakAfter = 1.6f;

	private void UpdateSleds(float dt, Vector3 crashPos)
	{
		if (_player == null)
		{
			return;
		}
		foreach (Sled s in _sleds)
		{
			Entity e = s.Shot.Actor.Model;
			float yaw = s.Yaw * MathF.PI / 180.0f;
			Vector3 forward = new(MathF.Sin(yaw), 0.0f, MathF.Cos(yaw));
			if (s.T < 0.0f)
			{
				// Mount: Crash standing on the board (inside its hull footprint, feet on its top).
				Vector3 d = crashPos - s.Start;
				float along = Vector3.Dot(d, forward);
				float side = d.X * forward.Z - d.Z * forward.X;
				if (!_player.IsGrounded || MathF.Abs(along) > SledHalfLength || MathF.Abs(side) > SledHalfWidth
					|| MathF.Abs(d.Y - SledTop) > 0.3f)
				{
					continue;
				}
				s.T = 0.0f;
				foreach (PropHull h in s.Shot.Hulls)
				{
					h.Body.Destroy();
				}
				s.Shot.Hulls.Clear();
			}
			s.T += dt;
			Vector3 feet0 = s.Start + new Vector3(0.0f, SledTop, 0.0f);
			Vector3 before = s.Feet;
			if (s.T <= SledChute[^1].T)
			{
				int i = Math.Min((int)(s.T / 0.1f), SledChute.Length - 2);
				float f = (s.T - SledChute[i].T) / (SledChute[i + 1].T - SledChute[i].T);
				float drop = SledChute[i].Drop + (SledChute[i + 1].Drop - SledChute[i].Drop) * f;
				float ahead = SledChute[i].Forward + (SledChute[i + 1].Forward - SledChute[i].Forward) * f;
				s.Feet = feet0 + forward * ahead + new Vector3(0.0f, drop, 0.0f);
			}
			else if (!s.Flying && !s.Sliding)
			{
				s.Flying = true;
				s.Feet = feet0 + forward * SledChute[^1].Forward + new Vector3(0.0f, SledChute[^1].Drop, 0.0f);
				s.Velocity = forward * SledLaunchForward + new Vector3(0.0f, SledLaunchUp, 0.0f);
			}
			else if (s.Flying)
			{
				s.Velocity.Y -= SledGravity * dt;
				s.Feet += s.Velocity * dt;
				float? ground = SledGround(s.Feet);
				if (s.Velocity.Y < 0.0f && ground is float g && s.Feet.Y - SledTop + SledBottom <= g)
				{
					s.Flying = false;
					s.Sliding = true;
					s.Velocity = new Vector3(s.Velocity.X, 0.0f, s.Velocity.Z) * SledLandKeep;
					s.Feet.Y = g - SledBottom + SledTop;
				}
			}
			else if (s.Velocity.LengthSquared() > 0.0f)
			{
				float speed = MathF.Max(0.0f, s.Velocity.Length() - SledBrake * dt);
				s.Velocity = speed > 0.0f ? Vector3.Normalize(s.Velocity) * speed : Vector3.Zero;
				s.Feet += s.Velocity * dt;
				if (SledGround(s.Feet) is float g)
				{
					s.Feet.Y = g - SledBottom + SledTop;
				}
			}
			else if ((s.Hold += dt) >= SledBreakAfter)
			{
				// COM_EARTH_NATIVE_SLEDGE s2: the board breaks up and is destroyed; Crash stands.
				// ponytail: its break particles (DoParticle 0xD2/0xD3) and sound are not ported.
				e.Destroy();
				_player.RideFeet = null;
				s.Shot.Actor.Alive = false;
				continue;
			}
			// Pitch the board along its travel (nose down the chute, up off the ramp).
			Vector3 v = s.T <= SledChute[^1].T && dt > 0.0f ? (s.Feet - before) / dt : s.Velocity;
			float pitch = s.T < dt * 1.5f || s.Sliding ? 0.0f : MathF.Atan2(v.Y, MathF.Max(1.0f, MathF.Sqrt(v.X * v.X + v.Z * v.Z))) * 180.0f / MathF.PI;
			e.Position = s.Feet - new Vector3(0.0f, SledTop, 0.0f);
			e.EulerDegrees = new Vector3(-pitch, s.Yaw, 0.0f);
			_player.RideFeet = s.Feet;
		}
		_sleds.RemoveAll(s => !s.Shot.Actor.Alive);
	}

	// Static ground under the sledge, cast from just under Crash's feet so it cannot hit him.
	private static float? SledGround(Vector3 feet)
	{
		RaycastHit hit = Physics.Raycast(feet - new Vector3(0.0f, SledTop - 0.1f, 0.0f), -Vector3.UnitY, 30.0f);
		return hit.DidHit ? hit.Position.Y : null;
	}

	private void UpdateOneShots(float dt, Vector3 crashPos)
	{
		UpdateBombs(dt, crashPos);
		UpdateCannons(dt, crashPos);
		UpdateSleds(dt, crashPos);
		foreach (OneShot s in _oneShots)
		{
			if (s.Remaining >= 0.0f)
			{
				s.Remaining -= dt;
				if (s.Remaining < 0.0f)
				{
					// Hold the last frame.
					Animation.SetTime(s.Actor.Model, Animation.ClipDuration(s.Actor.Model));
					Animation.SetPlaybackSpeed(s.Actor.Model, 0.0f);
					HoldHulls(s, s.Playing);
				}
				continue;
			}
			if (s.Cue == PropCue.Proximity && s.Next < s.Clips.Length
				&& Vector3.DistanceSquared(crashPos, s.Actor.Model.Position) <= WumpaTreeShakeRadius * WumpaTreeShakeRadius)
			{
				PlayOnce(s);
			}
		}
	}

	private static void PlayOnce(OneShot s)
	{
		// A clip still playing is not restarted; spent props ignore further cues.
		if (s.Remaining >= 0.0f || s.Next >= s.Clips.Length)
		{
			return;
		}
		Entity e = s.Actor.Model;
		s.Playing = s.ClipNames[s.Next];
		Animation.SetClip(e, s.Clips[s.Next++]);
		Animation.SetTime(e, 0.0f);
		Animation.SetPlaybackSpeed(e, 1.0f);
		s.Remaining = Animation.ClipDuration(e);
	}

	private static void LoadHulls(OneShot s, string model)
	{
		string? text = Assets.ReadText(model.Substring(0, model.Length - ".gltf".Length) + ".hulls.json");
		if (text == null)
		{
			return;
		}
		using JsonDocument doc = JsonDocument.Parse(text);
		foreach (JsonElement row in doc.RootElement.GetProperty("hulls").EnumerateArray())
		{
			PropHull h = new() { Rest = row.GetProperty("rest").GetString()! };
			foreach (JsonProperty clip in row.GetProperty("clips").EnumerateObject())
			{
				h.Clips[clip.Name] = clip.Value.GetString()!;
			}
			h.Body = HullBody(s.Actor.Model, h.Rest);
			s.Hulls.Add(h);
		}
	}

	// Move each hull the clip moved to the pose the prop now holds.
	private static void HoldHulls(OneShot s, string clip)
	{
		foreach (PropHull h in s.Hulls)
		{
			if (h.Clips.TryGetValue(clip, out string? path))
			{
				h.Body.Destroy();
				h.Body = HullBody(s.Actor.Model, path);
			}
		}
	}

	private static Entity HullBody(Entity model, string path)
	{
		Entity b = World.Create();
		b.Name = model.Name + " hull";
		b.MarkTransient();
		b.AddTransform();
		b.Position = model.Position;
		b.EulerDegrees = model.EulerDegrees;
		Physics.AddConvexHullBody(b, path, dynamic: false);
		return b;
	}
}
