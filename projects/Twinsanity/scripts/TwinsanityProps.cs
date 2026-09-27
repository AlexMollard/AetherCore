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
		public Entity Crown, CrownHull; // an idol head's spiked crown (TRAINING_EXPLODING_IDOL_HEAD_CROWN)
		public float CrownLeft = -1.0f; // seconds of the crown's a001 burst left; -1 = resting
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

	// act_GLOBAL_BOMB (COM_GLOBAL_BOMB_DEFAULT) is a pushable (TwinsanityMechanics) with this fuse on
	// top. A spin primes it (s4/s6 AgentWasSpun -> s2 COM_GLOBAL_BOMB_PRIMED); so does rolling it and
	// walking off: s5 (rolling) -> s6 (stopped) -> s2 once MeToPlayerSqrDist > 20; so does touching a
	// deadly surface (s1/s4/s6 Cond125: rig logs/cannon/rigjump_boom.png, a lobbed bomb that rolled
	// down the seabed onto the drowning plane primed and blew up, one resting above it at -2.7 did
	// not, rigjump2.json). Rolled more than sqrt(2000) m from where it started (s5
	// MeToInitPosSqrDist > 2000) it primes on a 4 s fuse instead (s10). 1 s after priming (s2
	// TimeInUnit 1) COM_GLOBAL_BOMB_DAMAGED explodes it with CreateDamage radius 3 - the blast that
	// knocks the idol heads over (rig logs/beachcomplete/rig_totem_sheet.png: rolled to the
	// yellow-gem totem, Crash leaves, it goes off at the totem's mouth).
	private sealed class Bomb
	{
		public Pushable Push = null!;
		public float Fuse = -1.0f; // seconds to the explosion once primed
		public float Flash;        // COM_GLOBAL_BOMB_PRIMED's red/black cycles run so far (BombFlash)
		public bool Red;           // showing OGI 871 (red) rather than 870
		public bool Rolled;        // s5 reached: it has been pushed
		public Vector3 Home;       // where it started (s5 MeToInitPosSqrDist)
		public bool Gone;
	}

	private readonly List<Bomb> _bombs = new();
	private const float BombFuse = 1.0f;
	private const float BombFarFuse = 4.0f;    // s10 TimeInUnit 4
	private const float BombFarSqr = 2000.0f;  // s5 MeToInitPosSqrDist
	private const float BombDamageRadius = 3.0f;
	private const float BombSpinReach = 1.5f;
	private const float BombLeaveSqr = 20.0f;
	// ponytail: the spin's knock is not in the scripts. Measured in HubA (logs/traversal/bomb_rig.csv):
	// a spin beside it sends it rolling ~2.06 m before the blast; 4.5 m/s decaying at 1.4/s covers
	// 2.4 m over the 1 s fuse.
	private const float BombKickSpeed = 4.5f;
	private const float BombDrag = 1.4f;
	// COM_GLOBAL_BOMB_PRIMED loops DoAnim OGI slot 2 (871: the red half of the bomb texture, UVs
	// shifted 0.485) then slot 1 (870, black), DELAY 0.25 per state. Rig, every frame at 50 Hz
	// (logs/bombfuse/README.md): red from the moment it primes, ~13 frames red / ~15 black, a steady
	// 0.55 s cycle from the first flash to the boom on both the 1 s spin fuse and the 4 s far fuse.
	// The speed-up over the last second is ours, visual only (the user's ask): the cycle shrinks
	// linearly to BombFlashEnd at the boom. The fuse lengths are untouched.
	private const float BombFlashCycle = 0.55f;
	private const float BombFlashRed = 0.47f;       // red share of a cycle (0.26 s of 0.55)
	private const float BombFlashEnd = 0.1f;
	private const float BombFlashRamp = 1.0f;       // seconds before the boom the speed-up starts
	private const string BombRedModel = "project://assets/models/objects/act_GLOBAL_BOMB/act_GLOBAL_BOMB_1.gltf";

	// act_RIGID_CANNON (COM_RIGID_CANNON_ACTIVATED): any jump landing on its red button fires a
	// GLOBAL_BOMB from the muzzle (OGI 756 exit point 1: local (0, 2.577, 4.058)) 0.1 s after the
	// landing, with the muzzle flash (EXPLODE_1A/1C/1D) and its Sounds[0] = 63. The button sends
	// message 87 on a plain landing and 256 on a belly-flop, and the two shots differ (rig EE RAM,
	// logs/cannon/rigball*.json, fitted by fit_ball.py with drag dv/dt = -k v - g):
	//   - plain jump: subtype 7, 8.0 m/s along the barrel and 11.2 up; it lands ~7 m out and just
	//     lies there as an ordinary bomb (s12 TouchingTerrain -> s1).
	//   - belly-flop: subtype 8, primed, 24.3 m/s along and 16.6 up; it goes off on the first thing
	//     it touches (s8 TouchingTerrain -> s7): the statue 30 m out, 1.49 s after leaving.
	// Both fly under g 19.5 with a drag of 0.585/s (the fits: 19.62/0.597 and 19.36/0.573). Every
	// landing fires, with no cooldown and no ball count (rigtrig2/3.json: five in a row, and four
	// 0.95 s apart).
	private sealed class Cannonball
	{
		public Entity Model;
		public Vector3 Velocity;
		public bool Primed;       // the belly-flop shot: explodes on contact, flashing (s8 runs PRIMED)
		public float Flash;
		public bool Red;
		public Pushable Cannon = null!;
		public float Floor;       // lost below this (over a void)
	}

	private sealed class CannonShot
	{
		public Pushable Cannon = null!;
		public bool Primed;
		public float Delay;
	}

	private readonly List<Cannonball> _cannonballs = new();
	private readonly List<CannonShot> _cannonShots = new();
	private bool _wasGrounded = true;
	private bool _rose; // this airborne spell went up: a jump (AgentWasJumpedOn), not a fall or a warp
	private static readonly Vector3 CannonMuzzleLocal = new(0.0f, 2.577f, 4.058f);
	private const float CannonFireDelay = 0.1f;   // rigfire.json: landing flash 1.324 s, muzzle 1.418 s
	private const float CannonJumpSpeed = 8.0f, CannonJumpUp = 11.2f;
	private const float CannonSlamSpeed = 24.3f, CannonSlamUp = 16.6f;
	private const float CannonballGravity = 19.5f;
	private const float CannonballDrag = 0.585f;
	private const float CannonballRadius = 0.6f;  // SetLogicalRadius(0, 0.6)
	private const string CannonballModel = "project://assets/models/objects/act_GLOBAL_BOMB/act_GLOBAL_BOMB_0.gltf";
	// The idol head's crown: SpawnResidentAgent(0, 0, 8.5, ...) of object 780 on top of the head. It
	// hurts Crash standing on it (rig logs/cannon/rigcrown.json: dropped onto statue 2's crown he
	// stands at 12.649 = 1.792 + 8.5 + its 2.337 hull top and loses a mask at 0.6 s and another at
	// 2.45 s), and bursts (a001, then DestroyMe) when a blast lowers the head (rig_headhit.png).
	private const float CrownRise = 8.5f;
	private const string CrownModel = "project://assets/models/objects/TRAINING_EXPLODING_IDOL_HEAD_CROWN/TRAINING_EXPLODING_IDOL_HEAD_CROWN.gltf";
	private const string CrownHullPath = "project://assets/models/objects/TRAINING_EXPLODING_IDOL_HEAD_CROWN/TRAINING_EXPLODING_IDOL_HEAD_CROWN_hull0.gltf";

	// ponytail: the original's global progression counter (condition GlobalProgression) lives in
	// the save; the port always starts a new game, where it is 0. Upgrade path: a save system.
	private const int GlobalProgression = 0;
	// COM_WUMPA_TREE_DEFAULT s3: MeToFocusSqrDist <= 4 with Crash as the focus.
	private const float WumpaTreeShakeRadius = 2.0f;

	// Sets up a one-shot prop; false when the object is not one.
	private bool SetupOneShot(Actor a, TwInstance i)
	{
		string objectName = i.Name, model = i.Model;
		uint subtype = i.Subtype;
		string n = NameKey(objectName);
		Entity e = a.Model;
		OneShot s = new() { Actor = a };
		bool playNow = false;
		bool startDone = false;
		if (n.StartsWith("act_training_exploding_idol_head"))
		{
			s.Cue = PropCue.Explosion;
			s.ClipNames = new[] { "a001", "a002" };
			// COM_TRAINING_EXPLODING_IDOL_HEAD_DEFAULT s4: SoftFlagSet(18) spawns the crown 8.5 up.
			if ((i.Flags >> 18 & 1u) != 0)
			{
				s.Crown = World.Create();
				s.Crown.Name = objectName + " Crown";
				s.Crown.MarkTransient();
				s.Crown.AddTransform();
				s.Crown.Position = e.Position + new Vector3(0.0f, CrownRise, 0.0f);
				s.Crown.EulerDegrees = e.EulerDegrees;
				s.Crown.LoadModel(CrownModel);
				SetLooping(s.Crown, false);
				int burst = Animation.Find(s.Crown, "a001");
				if (burst >= 0)
				{
					Animation.SetClip(s.Crown, burst);
					Animation.SetTime(s.Crown, 0.0f);
					Animation.SetPlaybackSpeed(s.Crown, 0.0f);
				}
				s.CrownHull = HullBody(s.Crown, CrownHullPath);
			}
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
		else if (n.StartsWith("act_training_cave_blocker"))
		{
			// No scripts of its own: hubb trigger 2's message 87 drops it from its placement (y 8, above the
			// cave roof) onto its one key (y 0), shutting the cave to HubA behind Crash.
			LoadHulls(s, model);
			Vector3[] keys = i.Points;
			float[] floats = i.Floats;
			_blockers.Add(new Blocker
			{
				Shot = s,
				To = keys.Length > 0 ? keys[0] : e.Position,
				Speed = floats.Length > 3 ? floats[3] : 25.0f,
				Accel = floats.Length > 4 ? floats[4] : 25.0f,
			});
			return true;
		}
		else if (n.StartsWith("act_training_swinging_log") && subtype == 0)
		{
			// COM_TRAINING_SWINGING_LOG_START s0: PosWarp lifts the pivot 6.4 m (the model hangs 8.3 m below its
			// origin); subtype 0 then swings from level start (s9 -> s13 SetWobble).
			e.Position += new Vector3(0.0f, SwingLift, 0.0f);
			_swingLogs.Add(new SwingLog { Model = e, Pivot = e.Position, Yaw = e.EulerDegrees.Y });
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
				BurstCrown(s);
			}
		}
	}

	// The head's message to its crown when it drops: the crown plays a001 (its spikes fly off) and
	// DestroyMe once it ends (COM_TRAINING_EXPLODING_IDOL_HEAD_CROWN_ACTIVATED); from then on the
	// lowered head's top is safe.
	private static void BurstCrown(OneShot s)
	{
		if (!s.Crown.IsValid || s.CrownLeft >= 0.0f)
		{
			return;
		}
		if (s.CrownHull.IsValid)
		{
			s.CrownHull.Destroy();
			s.CrownHull = default;
		}
		Animation.SetTime(s.Crown, 0.0f);
		Animation.SetPlaybackSpeed(s.Crown, 1.0f);
		s.CrownLeft = Animation.ClipDuration(s.Crown);
	}

	// Crash on a crown: its spikes hurt him, one mask per hit, again once his hurt grace is over
	// (the rig's two hits 1.85 s apart, rigcrown.json).
	private void UpdateCrown(OneShot s, float dt)
	{
		if (s.CrownLeft >= 0.0f)
		{
			s.CrownLeft -= dt;
			if (s.CrownLeft < 0.0f)
			{
				s.Crown.Destroy();
				s.Crown = default;
			}
			return;
		}
		if (s.CrownHull.IsValid && _player != null && _player.Self.IsValid && CharacterController.GetGroundEntity(_player.Self) == s.CrownHull)
		{
			// From straight below: the rig leaves him standing on the spikes (no shove off the top).
			_host?.DamagePlayer(_player.Self.Position - Vector3.UnitY, DeathKind.Generic);
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
			Pushable p = b.Push;
			if (b.Fuse < 0.0f)
			{
				Vector3 away = (p.Center - crashPos) with { Y = 0.0f };
				float dist = away.Length();
				if (_player != null && _player.IsSpinning && dist < BombSpinReach && MathF.Abs(crashPos.Y - p.Center.Y) < 1.5f)
				{
					b.Fuse = BombFuse;
					p.Velocity = (dist > 0.001f ? away / dist : Vector3.UnitZ) * BombKickSpeed;
					p.FreeDamping = BombDrag;
				}
				else if (OnDeadly(p))
				{
					b.Fuse = BombFuse;
				}
				else if (b.Rolled && Vector3.DistanceSquared(p.Center, b.Home) > BombFarSqr)
				{
					b.Fuse = BombFarFuse;
				}
				else
				{
					// s6's leave rule follows s5 only, i.e. only once Crash has pushed it: a lobbed bomb
					// resting on the seabed stays put (rigjump2.json).
					b.Rolled |= p.Pushed;
					if (b.Rolled && p.Velocity == Vector3.Zero && Vector3.DistanceSquared(p.Center, crashPos) > BombLeaveSqr)
					{
						b.Fuse = BombFuse;
					}
				}
				if (b.Fuse < 0.0f)
				{
					continue;
				}
			}
			else
			{
				b.Fuse -= dt;
			}
			if (b.Fuse >= 0.0f)
			{
				BombFlash(p.Model, dt, b.Fuse, ref b.Flash, ref b.Red);
				continue;
			}
			BombBlast(p.Center, crashPos);
			b.Gone = true;
			_pushables.Remove(p);
			p.Body.Destroy();
			p.Model.Destroy();
		}
		_bombs.RemoveAll(b => b.Gone);
	}

	// One step of the primed flash (BombFlashCycle): red for the first BombFlashRed of each cycle,
	// from the frame it primes. `remaining` is the fuse left; infinity keeps the rig's steady cycle.
	private static void BombFlash(Entity model, float dt, float remaining, ref float phase, ref bool red)
	{
		float cycle = remaining < BombFlashRamp
			? BombFlashEnd + (BombFlashCycle - BombFlashEnd) * remaining / BombFlashRamp
			: BombFlashCycle;
		bool want = phase % 1.0f < BombFlashRed;
		phase += dt / cycle;
		if (want != red)
		{
			red = want;
			CrateFx.LoadState(model, red ? BombRedModel : CannonballModel);
		}
	}

	// What the bomb rests on is a deadly collision piece (the drowning plane under the sea, a pit).
	private bool OnDeadly(Pushable p)
	{
		RaycastHit hit = Physics.Raycast(p.Center, -Vector3.UnitY, p.RestHeight + 0.3f);
		if (hit.DidHit && hit.Entity == p.Body)
		{
			hit = Physics.Raycast(p.Center - new Vector3(0.0f, p.Radius + 0.02f, 0.0f), -Vector3.UnitY, 0.3f);
		}
		return hit.DidHit && _host != null && _host.IsDeadly(hit.Entity);
	}

	// COM_GLOBAL_BOMB_DAMAGED: EXPLODE_1A-1D, Sounds[3], CreateDamage radius 3 (100 damage: through
	// the masks).
	private void BombBlast(Vector3 center, Vector3 crashPos)
	{
		CrateFx.BombExploded(center);
		Log.Info($"[Twinsanity] bomb exploded at ({center.X:F2}, {center.Y:F2}, {center.Z:F2})");
		Explosion(center, BombDamageRadius);
		if (Vector3.Distance(center, crashPos + new Vector3(0.0f, 0.9f, 0.0f)) < BombDamageRadius)
		{
			_host?.DamagePlayer(center, DeathKind.Explode);
		}
	}

	private void UpdateCannons(float dt, Vector3 crashPos)
	{
		// A jump landing on the button (COM_RIGID_CANNON_BUTTON_ACTIVATED on OnLand /
		// OnGettingBodyslamAttacked): Crash comes down onto its hull. A belly-flop is the primed shot.
		bool grounded = _player != null && _player.IsGrounded;
		_rose |= !grounded && _player != null && _player.Velocity.Y > 1.0f;
		// The rig's idle case (rigtrig.json): dropped onto the button by a warp, nothing fires.
		if (grounded && !_wasGrounded && _rose && _player!.Self.IsValid)
		{
			Entity under = CharacterController.GetGroundEntity(_player.Self);
			foreach (Pushable p in _pushables)
			{
				if (p.ButtonHull.IsValid && under == p.ButtonHull)
				{
					_cannonShots.Add(new CannonShot { Cannon = p, Primed = _player.IsSlamming, Delay = CannonFireDelay });
				}
			}
		}
		_wasGrounded = grounded;
		if (grounded)
		{
			_rose = false;
		}

		foreach (CannonShot shot in _cannonShots)
		{
			shot.Delay -= dt;
			if (shot.Delay < 0.0f)
			{
				FireCannon(shot.Cannon, shot.Primed);
			}
		}
		_cannonShots.RemoveAll(s => s.Delay < 0.0f);

		foreach (Cannonball c in _cannonballs)
		{
			// dv/dt = -k v - g (the rig fit), one semi-implicit step.
			c.Velocity += (-CannonballDrag * c.Velocity - new Vector3(0.0f, CannonballGravity, 0.0f)) * dt;
			Vector3 from = c.Model.Position;
			Vector3 step = c.Velocity * dt;
			float len = step.Length();
			Vector3 to = from + step;
			bool contact = false;
			if (len > 1e-5f)
			{
				RaycastHit hit = Physics.Raycast(from, step / len, len + CannonballRadius);
				if (hit.DidHit && !IsCannonPart(c.Cannon, hit.Entity) && !(_player != null && hit.Entity == _player.Self))
				{
					contact = true;
					to = hit.Position - step / len * CannonballRadius;
				}
			}
			if (c.Primed && !contact)
			{
				foreach (OneShot s in _oneShots)
				{
					contact |= s.Cue == PropCue.Explosion && s.Actor.Alive && InBlast(to, 0.0f, s.Actor.Model.Position);
				}
			}
			c.Model.Position = to;
			if (c.Primed)
			{
				BombFlash(c.Model, dt, float.PositiveInfinity, ref c.Flash, ref c.Red);
			}
			if (to.Y < c.Floor)
			{
				c.Model.Destroy();
				c.Velocity = new Vector3(float.NaN);
				continue;
			}
			if (!contact)
			{
				continue;
			}
			c.Model.Destroy();
			c.Velocity = new Vector3(float.NaN);
			if (c.Primed)
			{
				BombBlast(to, crashPos);
			}
			else
			{
				// The lob lands and lies there as an ordinary bomb (s12 TouchingTerrain -> s1).
				TrySpawnPushable("act_GLOBAL_BOMB", CannonballModel, to, Vector3.Zero);
				Log.Info($"[Twinsanity] cannon lob landed at ({to.X:F2}, {to.Y:F2}, {to.Z:F2})");
			}
		}
		_cannonballs.RemoveAll(c => float.IsNaN(c.Velocity.X));
	}

	private void FireCannon(Pushable p, bool primed)
	{
		float yaw = p.Model.EulerDegrees.Y;
		Vector3 muzzle = p.Model.Position + Yawed(CannonMuzzleLocal, yaw);
		Vector3 forward = Yawed(Vector3.UnitZ, yaw);
		CrateFx.MuzzleFlash(muzzle);
		Log.Info($"[Twinsanity] cannon fired ({(primed ? "belly-flop" : "jump")} shot, yaw {yaw:F1})");
		TwinsanityAudio.Explosion(muzzle); // Sounds[0] = 63, the same clip as 22
		Entity ball = World.Create();
		ball.Name = "Cannonball";
		ball.MarkTransient();
		ball.AddTransform();
		ball.Position = muzzle;
		ball.LoadModel(CannonballModel);
		_cannonballs.Add(new Cannonball
		{
			Model = ball,
			Cannon = p,
			Primed = primed,
			Velocity = forward * (primed ? CannonSlamSpeed : CannonJumpSpeed) + new Vector3(0.0f, primed ? CannonSlamUp : CannonJumpUp, 0.0f),
			Floor = p.Model.Position.Y - 40.0f,
		});
	}

	private static bool IsCannonPart(Pushable p, Entity e) => e == p.Body || e == p.ButtonHull || p.Hulls.Contains(e);

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
				float lastY = s.Feet.Y;
				s.Velocity.Y -= SledGravity * dt;
				s.Feet += s.Velocity * dt;
				// Cast from last frame's height: at ~50 m/s one frame can carry the feet through the sand.
				float? ground = SledGround(new Vector3(s.Feet.X, lastY, s.Feet.Z));
				if (s.Velocity.Y < 0.0f && ground is float g && s.Feet.Y - SledTop + SledBottom <= g)
				{
					s.Flying = false;
					s.Sliding = true;
					s.Velocity = new Vector3(s.Velocity.X, 0.0f, s.Velocity.Z) * SledLandKeep;
					s.Feet.Y = g - SledBottom + SledTop;
				}
				else if (s.Feet.Y < s.Start.Y - 40.0f)
				{
					// No ground under the flight (collision missing): hand Crash back rather than
					// carry him down forever.
					e.Destroy();
					_player.RideFeet = null;
					s.Shot.Actor.Alive = false;
					continue;
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

	// Static ground under the sledge, cast from 2 m above the feet (a slope rising under a fast board
	// would otherwise put the start under the sand), passing through Crash's own capsule.
	private float? SledGround(Vector3 feet)
	{
		Vector3 from = feet + new Vector3(0.0f, 2.0f, 0.0f);
		for (int i = 0; i < 3; i++)
		{
			RaycastHit hit = Physics.Raycast(from, -Vector3.UnitY, 32.0f);
			if (!hit.DidHit)
			{
				return null;
			}
			if (_player == null || hit.Entity != _player.Self)
			{
				return hit.Position.Y;
			}
			from = hit.Position - new Vector3(0.0f, 0.02f, 0.0f);
		}
		return null;
	}

	// act_TRAINING_CAVE_BLOCKER (hubb inst 37): the stone face over the cave from HubA. The rig closes the cave
	// once Crash is inside trigger 2 (logs/hubb/rig_notes.md: he walks back into it at x -80.5 against the face,
	// 2.7 m short of its centre). Its floats are the path-platform mover's: [3] speed 25, [4] accel 25.
	// ponytail: the drop itself is unmeasured (the rig's camera faces away from the cave there); 25 m/s^2
	// capped at 25 m/s lands the 8 m drop in 0.8 s. Upgrade path: film it with a free camera.
	private sealed class Blocker
	{
		public OneShot Shot = null!;
		public Vector3 To;
		public float Speed, Accel, Velocity;
		public bool Moving, Down;
	}

	private readonly List<Blocker> _blockers = new();

	/// <summary>Trigger message 87 aimed at the instance root <paramref name="root"/>: a path crab leaves its
	/// wait (COM_GLOBAL_CRAB_INIT S11, huba trigger 1), the cave blocker drops (hubb trigger 2).</summary>
	public void Wake(Entity root)
	{
		foreach (Actor a in _actors)
		{
			if (a.Model == root && a.Kind == Behaviour.Crab && a.Critter is { Points.Count: > 1, Mode: Mode.Down } c)
			{
				c.Mode = Mode.Walk;
			}
		}
		foreach (Blocker b in _blockers)
		{
			if (!b.Moving && !b.Down && b.Shot.Actor.Model == root)
			{
				b.Moving = true;
			}
		}
	}

	// JSON adapter (TwinsanityCutscenes' position-based wakes); deleted by the wave-2 cutover.
	/// <summary>Trigger message 87 aimed at the actor placed at <paramref name="home"/>: a path crab leaves its
	/// wait (COM_GLOBAL_CRAB_INIT S11, huba trigger 1), the cave blocker drops (hubb trigger 2).</summary>
	public void Wake(Vector3 home)
	{
		WakePathCrab(home);
		foreach (Blocker b in _blockers)
		{
			if (!b.Moving && !b.Down && Horizontal(b.Shot.Actor.Home, home) < 0.5f)
			{
				b.Moving = true;
			}
		}
	}

	private void UpdateBlockers(float dt)
	{
		foreach (Blocker b in _blockers)
		{
			if (!b.Moving)
			{
				continue;
			}
			Entity e = b.Shot.Actor.Model;
			b.Velocity = MathF.Min(b.Speed, b.Velocity + b.Accel * dt);
			Vector3 to = b.To - e.Position;
			float step = b.Velocity * dt;
			if (step >= to.Length())
			{
				e.Position = b.To;
				b.Moving = false;
				b.Down = true;
				foreach (PropHull h in b.Shot.Hulls)
				{
					h.Body.Destroy();
					h.Body = HullBody(e, h.Rest);
				}
				continue;
			}
			e.Position += Vector3.Normalize(to) * step;
		}
	}

	// act_TRAINING_SWINGING_LOG, subtype 0 (hubb inst 3): COM_TRAINING_SWINGING_LOG_START s13
	// SetWobble(2.0944, 0, 0, 0.8, ...) - 2.0944 rad/s about its local x axis, amplitude 0.8 rad: a 3.0 s
	// pendulum. COM_TRAINING_SWINGING_LOG_IMPACT sends Crash message 59 on touch. Rig (logs/hubb/rig_notes.md):
	// period 2.9-3.0 s from 0.72 s samples (rig_log2_sheet.png); one hit killed Crash with an Aku mask up
	// (rig_log_hit_sheet.png), so the hit goes through the masks.
	// ponytail: the swing phase starts at level start, not at the chunk's load on the disc.
	private sealed class SwingLog
	{
		public Entity Model;
		public Vector3 Pivot;
		public float Yaw, T;
	}

	private readonly List<SwingLog> _swingLogs = new();
	private const float SwingLift = 6.4f;          // s0 PosWarp(0, 6.4, 0)
	private const float SwingRate = 2.0944f;        // SetWobble rad/s
	private const float SwingAmplitude = 0.8f;      // SetWobble rad
	// The log in the model: its axis 7.3 m under the pivot, 5.5 m long along x (mesh -2.83..2.66), radius 1.0.
	private static readonly Vector3 SwingLogA = new(-2.6f, -7.3f, 0.0f), SwingLogB = new(2.4f, -7.3f, 0.0f);
	private const float SwingLogRadius = 1.0f;

	private void UpdateSwingLogs(float dt, Vector3 crashPos)
	{
		foreach (SwingLog l in _swingLogs)
		{
			l.T += dt;
			float angle = SwingAmplitude * MathF.Sin(SwingRate * l.T);
			l.Model.EulerDegrees = new Vector3(angle * 180.0f / MathF.PI, l.Yaw, 0.0f);
			Quaternion q = Quaternion.CreateFromYawPitchRoll(l.Yaw * MathF.PI / 180.0f, angle, 0.0f);
			Vector3 a = l.Pivot + Vector3.Transform(SwingLogA, q);
			Vector3 b = l.Pivot + Vector3.Transform(SwingLogB, q);
			// Crash's body: feet + 0.4 to feet + 1.3, radius 0.4.
			if (SegmentDistance(a, b, crashPos + new Vector3(0.0f, 0.4f, 0.0f), crashPos + new Vector3(0.0f, 1.3f, 0.0f)) < SwingLogRadius + 0.4f)
			{
				_host?.DamagePlayer((a + b) * 0.5f, DeathKind.Crush);
			}
		}
	}

	// Closest distance between segments p0-p1 and q0-q1.
	private static float SegmentDistance(Vector3 p0, Vector3 p1, Vector3 q0, Vector3 q1)
	{
		Vector3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
		float a = Vector3.Dot(d1, d1), e = Vector3.Dot(d2, d2), f = Vector3.Dot(d2, r);
		float c = Vector3.Dot(d1, r), b = Vector3.Dot(d1, d2), den = a * e - b * b;
		float s = den > 1e-6f ? Math.Clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
		float t = Math.Clamp((b * s + f) / e, 0.0f, 1.0f);
		s = Math.Clamp((b * t - c) / a, 0.0f, 1.0f);
		return Vector3.Distance(p0 + d1 * s, q0 + d2 * t);
	}

	private void UpdateOneShots(float dt, Vector3 crashPos)
	{
		UpdateBombs(dt, crashPos);
		UpdateCannons(dt, crashPos);
		UpdateSleds(dt, crashPos);
		UpdateBlockers(dt);
		UpdateSwingLogs(dt, crashPos);
		foreach (OneShot s in _oneShots)
		{
			if (s.Crown.IsValid)
			{
				UpdateCrown(s, dt);
			}
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
