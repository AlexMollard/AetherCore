using System;
using System.Collections.Generic;
using System.Numerics;
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
		public int Next;
		public float Remaining = -1.0f; // seconds left of the clip playing now; -1 = resting
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
	private bool SetupOneShot(Actor a, string objectName, uint subtype)
	{
		string n = NameKey(objectName);
		Entity e = a.Model;
		OneShot s = new() { Actor = a };
		bool playNow = false;
		bool startDone = false;
		if (n.StartsWith("act_training_exploding_idol_head"))
		{
			s.Cue = PropCue.Explosion;
			s.Clips = Clips(e, "a001", "a002");
		}
		else if (n.StartsWith("act_training_falling_log"))
		{
			s.Cue = PropCue.Explosion;
			s.Clips = Clips(e, "a001");
			startDone = subtype == 10;
		}
		else if (n.StartsWith("act_seapillar"))
		{
			s.Cue = PropCue.None; // rises only by progression
			s.Clips = Clips(e, "a001");
			startDone = GlobalProgression >= 2;
		}
		else if (n.StartsWith("act_wumpa_tree") || n.StartsWith("old_act_wumpa_tree"))
		{
			s.Cue = PropCue.Proximity;
			if (subtype == 20)
			{
				s.Clips = Clips(e, "a001");
				playNow = true;
			}
			else if (subtype is 0 or 1 or 2 or 3)
			{
				s.Clips = Clips(e, subtype == 1 ? "a005" : subtype == 2 ? "a004" : "a002");
			}
			// Other subtypes (10-12, the farmer cutscene trees) wait for a cutscene message.
		}
		else if (n.StartsWith("act_generic_grey_stone_door") || n.StartsWith("act_tiki_mon"))
		{
			// COM_GENERIC_GREY_STONE_DOOR_DEFAULT (a001) and COM_TIKI_MON_INIT (a007) play their clip
			// at spawn with DoAnim flags 0x20FF1 / 0xA0FF1: loop nibble (bits 12-15) 0 = play once,
			// the same as every one-shot above, while every idle loop in the hub scripts has it set
			// (chicken 0x3FF1, butterfly 0x5FF1, worm 0x2FF1 - logs/triggers/dump-loops.txt).
			s.Clips = Clips(e, n.StartsWith("act_tiki_mon") ? "a007" : "a001");
			playNow = true;
		}
		else if (n.StartsWith("act_global_bomb"))
		{
			_bombs.Add(new Bomb { Actor = a });
			return true;
		}
		else
		{
			return false;
		}

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

	private void UpdateOneShots(float dt, Vector3 crashPos)
	{
		UpdateBombs(dt, crashPos);
		UpdateCannons(dt, crashPos);
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
		Animation.SetClip(e, s.Clips[s.Next++]);
		Animation.SetTime(e, 0.0f);
		Animation.SetPlaybackSpeed(e, 1.0f);
		s.Remaining = Animation.ClipDuration(e);
	}

	private static int[] Clips(Entity e, params string[] names)
	{
		var list = new List<int>();
		foreach (string name in names)
		{
			int idx = Animation.Find(e, name);
			if (idx >= 0)
			{
				list.Add(idx);
			}
		}
		return list.ToArray();
	}
}
