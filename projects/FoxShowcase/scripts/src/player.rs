use std::sync::atomic::{AtomicBool, Ordering};
use aethercore::{gameplay_script, ClipRef, Context, Entity, Event, PhysicsEventKind, Result, Script, Vec3};

// The interaction script sets this while a panel is open.
pub(crate) static CONTROLS_BLOCKED: AtomicBool = AtomicBool::new(false);
const SPAWN: Vec3 = Vec3 { x: 3.0, y: 0.5, z: 34.0 };
fn v(x: f32, y: f32, z: f32) -> Vec3 { Vec3 { x, y, z } }
fn angle_delta(from: f32, to: f32) -> f32 { (to - from + 180.0).rem_euclid(360.0) - 180.0 }

gameplay_script! { pub struct ExplorerController {
    pub position: Vec3 = SPAWN,
    pub grounded: bool = false,
    pub moving: bool = false,
    pub collisions: i64 = 0,
    yaw: f32 = 0.0,
    pitch: f32 = 15.0,
    distance: f32 = 5.0,
    visual: Entity = Entity::NONE,
    body: Entity = Entity::NONE,
    joints: Entity = Entity::NONE,
    camera: Entity = Entity::NONE,
    velocity: Vec3 = Vec3 { x: 0.0, y: 0.0, z: 0.0 },
    body_yaw: f32 = 0.0,
    playback: f32 = 1.0,
    jump_held: bool = false,
    respawn_held: bool = false
} }

impl Script for ExplorerController {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.visual = ctx.find_entity("Explorer visual")?;
        self.body = ctx.find_entity("Explorer body")?;
        self.joints = ctx.find_entity("Explorer joints")?;
        self.camera = ctx.find_entity("Third person camera")?;
        self.position = ctx.position(ctx.entity())?;
        Ok(())
    }

    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        let blocked = CONTROLS_BLOCKED.load(Ordering::Relaxed);
        let jump = ctx.key_down(32)?;
        let respawn = ctx.key_down(82)?;
        if !blocked && respawn && !self.respawn_held {
            // Explicit respawn only; the controller owns all normal vertical motion.
            ctx.set_position(ctx.entity(), SPAWN)?;
            ctx.set_euler(ctx.entity(), v(0.0, 0.0, 0.0))?;
            self.position = SPAWN;
            self.velocity = v(0.0, 0.0, 0.0);
        }
        self.respawn_held = respawn;

        let dt = delta.max(0.0);
        let dragging = !blocked && ctx.mouse_down(0)?;
        if dragging {
            let mouse = ctx.mouse_delta()?;
            self.yaw -= mouse.x * 0.2;
            self.pitch = (self.pitch + mouse.y * 0.2).clamp(-10.0, 65.0);
        }
        if !blocked {
            self.distance = (self.distance - ctx.scroll_delta()?.y * 0.45).clamp(2.4, 9.0);
        }
        let (sy, cy) = self.yaw.to_radians().sin_cos();
        let right = if blocked { 0.0 } else { (ctx.key_down(68)? as i32 - ctx.key_down(65)? as i32) as f32 };
        let forward = if blocked { 0.0 } else { (ctx.key_down(87)? as i32 - ctx.key_down(83)? as i32) as f32 };
        let mut direction = v(right * cy - forward * sy, 0.0, -right * sy - forward * cy);
        let length = (direction.x * direction.x + direction.z * direction.z).sqrt();
        if length > 0.0 { direction.x /= length; direction.z /= length; }
        let speed = if !blocked && (ctx.key_down(340)? || ctx.key_down(344)?) { 5.6 } else { 3.7 };
        let target_x = direction.x * speed;
        let target_z = direction.z * speed;
        let change_x = target_x - self.velocity.x;
        let change_z = target_z - self.velocity.z;
        let change = (change_x * change_x + change_z * change_z).sqrt();
        let rate = if length > 0.0 { 21.0 } else { 30.0 };
        let fraction = if change > 0.0 { (rate * dt / change).min(1.0) } else { 0.0 };
        self.velocity.x += change_x * fraction;
        self.velocity.z += change_z * fraction;
        ctx.character_move(ctx.entity(), self.velocity, 1.0)?;
        self.grounded = ctx.character_is_grounded(ctx.entity())?;
        if !blocked && jump && !self.jump_held && self.grounded { ctx.character_jump(ctx.entity(), 5.0)?; }
        self.jump_held = jump;

        let position = ctx.position(ctx.entity())?;
        let dx = position.x - self.position.x;
        let dz = position.z - self.position.z;
        let actual_speed = if dt > 0.0 { (dx * dx + dz * dz).sqrt() / dt } else { 0.0 };
        let moving = actual_speed > 0.12;
        if moving != self.moving {
            let clip = if moving { 1 } else { 0 };
            ctx.set_animation_clip(self.body, ClipRef::Index(clip), 0.18)?;
            ctx.set_animation_clip(self.joints, ClipRef::Index(clip), 0.18)?;
            self.moving = moving;
        }
        let playback = if moving { (actual_speed / 3.7).clamp(0.75, 1.35) } else { 1.0 };
        if (playback - self.playback).abs() > 0.01 {
            ctx.set_animation_speed(self.body, playback)?;
            ctx.set_animation_speed(self.joints, playback)?;
            self.playback = playback;
        }
        if moving {
            let facing = (-dx).atan2(-dz).to_degrees() + 180.0;
            let turn = angle_delta(self.body_yaw, facing);
            self.body_yaw += turn.clamp(-12.0_f32.to_degrees() * dt, 12.0_f32.to_degrees() * dt);
            ctx.set_euler(self.visual, v(0.0, self.body_yaw, 0.0))?;
        }
        self.position = position;

        let (sp, cp) = self.pitch.to_radians().sin_cos();
        let (sy, cy) = self.yaw.to_radians().sin_cos();
        let target = v(position.x, position.y + 1.35, position.z);
        let back = v(sy * cp, sp, cy * cp);
        // Start outside the capsule so the player's own inner body cannot mask walls.
        let offset = 0.6;
        let origin = v(target.x + back.x * offset, target.y + back.y * offset, target.z + back.z * offset);
        let mut distance = self.distance;
        if let Some(hit) = ctx.raycast_3d(origin, back, self.distance - offset)? {
            if hit.entity != ctx.entity() {
                distance = (offset + hit.distance - 0.2).max(0.35);
            }
        }
        let eye = v(target.x + back.x * distance, target.y + back.y * distance, target.z + back.z * distance);
        ctx.set_position(self.camera, eye)?;
        ctx.set_euler(self.camera, v(-self.pitch, self.yaw, 0.0))
    }

    fn event(&mut self, ctx: &mut Context<'_>, event: &Event) -> Result {
        let Some(kind) = event.physics_kind() else {
            return Err(aethercore::Error(aethercore::abi::AE_GAMEPLAY_UNSUPPORTED, "Explorer only handles physics events".into()));
        };
        if kind == PhysicsEventKind::CollisionEnter {
            self.collisions = self.collisions.saturating_add(1);
        }
        self.grounded = ctx.character_is_grounded(ctx.entity())?;
        Ok(())
    }
}
