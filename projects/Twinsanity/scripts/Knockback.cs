using System;
using System.Numerics;

namespace AetherGame;

/// <summary>
/// One knockback arc: a launch away from whatever hit the body (a blast centre, Crash, a
/// hazard), then gravity, an optional bounce, and a tumble while airborne. Measured on the rig
/// (logs/gameplay/knockback_sheet.md): creature deaths from a spin or a blast and Crash's hurt
/// recoil all share this shape and differ only in the numbers.
/// </summary>
public readonly record struct KnockArc(
	float Speed,      // horizontal launch speed away from the source, m/s
	float Lift,       // vertical launch speed, m/s
	float Gravity,    // m/s^2 while airborne (the scripts' cmd 127 SetGravity 35)
	float Bounce,     // vertical speed kept on the first landing (0: stops dead)
	float BounceKeep, // horizontal speed kept on that bounce
	float Tumble,     // tumble rate while airborne, rad/s (the scripts' cmd 12 spin)
	float Linger,     // seconds the body lies after coming to rest before it pops (CrateFx.CreaturePop) and goes
	float Duration);  // for a ground shove (Lift 0): seconds the push lasts, then it stops dead

public static class Knockback
{
	// Coop chicken, spun or caught in a blast (rig_chicken_spin.csv, rig_blast1.csv): 12.5 m/s
	// straight away from the hit, 8.0 m/s up under 35 m/s^2 (apex 0.93 m, 0.46 s in the air), no
	// bounce; it pops into feathers the moment it lands (COM_GLOBAL_CHICKEN_HIT waits on landing,
	// then CHICKEN_POP: SEAGULLPOP + ENEMY_POP2; rig rig_pop_chicken: gone by the next frame).
	public static readonly KnockArc Chicken = new(12.5f, 8.0f, 35.0f, 0.0f, 0.0f, 41.89f, 0.0f, 0.0f);

	// Crab (and the other COM_GENERIC_CREATURE_DAMAGED_SPIN creatures), spun (rig_crab_spin.csv):
	// 9.1 m/s away, 9.35 m/s up (apex 1.25 m, 0.53 s), one bounce keeping 0.46 of its drop speed
	// (a 0.26 m hop) and 0.63 of its run, then it lies 0.6 s (DAMAGED_SPIN S7, DELAY 0.6) and pops
	// (DEAD_BODYPOP; rig rig_pop_crab: the puff ~1.5 s after the hit).
	public static readonly KnockArc Creature = new(9.1f, 9.35f, 35.0f, 0.46f, 0.63f, 25.13f, 0.6f, 0.0f);

	// Crash hurt through a mask (rig_hurt_piranha.csv, rig_crab_spin.csv): a flat shove, 4.97 m/s
	// straight away from the hazard for 31 frames (0.517 s, 2.57 m), no lift, then a dead stop.
	public static readonly KnockArc CrashHurt = new(4.97f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.517f);

	/// <summary>Launch velocity for a body at <paramref name="at"/> hit from <paramref name="from"/>;
	/// <paramref name="fallback"/> (horizontal) is used when the two share a column.</summary>
	public static Vector3 Launch(Vector3 from, Vector3 at, KnockArc arc, Vector3 fallback)
	{
		Vector3 away = new(at.X - from.X, 0.0f, at.Z - from.Z);
		away = away.LengthSquared() > 1e-6f ? Vector3.Normalize(away) : fallback;
		return away * arc.Speed + new Vector3(0.0f, arc.Lift, 0.0f);
	}

	/// <summary>Advances an airborne body one step. <paramref name="bounced"/> starts false.
	/// Returns true once it has come to rest on <paramref name="groundY"/>.</summary>
	public static bool Step(ref Vector3 pos, ref Vector3 vel, ref bool bounced, KnockArc arc, float groundY, float dt)
	{
		vel.Y -= arc.Gravity * dt;
		pos += vel * dt;
		if (pos.Y > groundY || vel.Y > 0.0f)
		{
			return false;
		}
		pos.Y = groundY;
		if (!bounced && arc.Bounce > 0.0f)
		{
			bounced = true;
			vel = new Vector3(vel.X * arc.BounceKeep, -vel.Y * arc.Bounce, vel.Z * arc.BounceKeep);
			return false;
		}
		vel = Vector3.Zero;
		return true;
	}
}
