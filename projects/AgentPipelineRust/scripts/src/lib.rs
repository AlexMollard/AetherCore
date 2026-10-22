mod reference;
use reference::{ReferencePlayer, ReferenceResult};

use aethercore::*;
gameplay_enum! { pub enum Mode { Idle = 0, Moving = 1, Complete = 2 } }
gameplay_component_ref! { pub struct RigidBody2DRef = "Rigid Body 2D"; }
gameplay_script! { pub struct Probe {
    pub ticks: i64 = 0,
    pub attached: bool = false,
    pub detached: bool = false,
    pub events: i64 = 0,
    pub label: String = "héllo Rust".into(),
    pub speed: f32 = 2.0,
    pub target: Entity = Entity::NONE,
    pub offset: Vec3 = Vec3::default(),
    pub mode: Mode = Mode::Idle,
    pub component: RigidBody2DRef = RigidBody2DRef::default()
} }
impl Script for Probe {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.attached = true;
        ctx.log("Rust Probe attached")
    }
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        match std::env::var("AETHER_RUST_FIXTURE_FAILURE").as_deref() {
            Ok("error") => {
                return Err(Error(
                    abi::AE_GAMEPLAY_SCRIPT_ERROR,
                    "Rust fixture recoverable error".into(),
                ));
            }
            Ok("unwind") => panic!("Rust fixture unwind panic"),
            Ok("abort") => std::process::abort(),
            _ => {}
        }
        self.ticks += 1;
        let mut position = ctx.position(ctx.entity())?;
        position.x += self.speed * delta;
        ctx.set_position(ctx.entity(), position)
    }
    fn detach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.detached = true;
        ctx.log("Rust Probe detached")
    }
    fn event(&mut self, _: &mut Context<'_>, event: &Event) -> Result {
        if event.kind == abi::AE_GAMEPLAY_EVENT_PHYSICS {
            self.events += 1;
            Ok(())
        } else {
            Err(Error(4, "event unsupported".into()))
        }
    }
}
gameplay_script! { pub struct Player {
    pub speed: f32 = 3.0,
    pub ticks: i64 = 0,
    pub collisions: i64 = 0,
    pub label: String = "Rust Player".into(),
    pub score: i64 = 0,
    pub counter: Entity = Entity::NONE,
    pub spawned: Entity = Entity::NONE,
    pub exercise_services: bool = false
} }
impl Script for Player {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        ctx.log("Rust Player attached")
    }
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        self.ticks += 1;
        if self.ticks == 60 {
            ctx.log("Rust Player reached 60 updates")?;
        }
        let mut p = ctx.position(ctx.entity())?;
        p.x += self.speed * delta;
        ctx.set_position(ctx.entity(), p)?;
        if self.exercise_services {
            if ctx.key_down(32)? {
                self.score += 1;
            }
            ctx.set_sprite_color(ctx.entity(), [0.2, 0.8, 1., 1.])?;
            ctx.set_velocity_2d(ctx.entity(), self.speed, 0.)?;
            let _ = ctx.velocity_2d(ctx.entity())?;
            if self.counter != Entity::NONE {
                ctx.set_ui_text(self.counter, &format!("Score {}", self.score))?;
            }
            if self.ticks == 1 {
                self.spawned = ctx.create_entity("RustSpawned")?;
                ctx.write_save("rust_score", &self.score.to_string())?;
            }
            if self.ticks == 2 && self.spawned != Entity::NONE {
                ctx.destroy_entity(self.spawned)?;
                self.spawned = Entity::NONE;
            }
        }
        Ok(())
    }
    fn event(&mut self, _: &mut Context<'_>, _: &Event) -> Result {
        self.collisions += 1;
        Ok(())
    }
}
mod model_probe;
pub use model_probe::ModelProbe;
mod custom_pass;
pub use custom_pass::CustomPassProbe;
#[cfg(not(feature = "wrong-abi"))]
aethercore::export_gameplay!(Player, Probe, ReferencePlayer, ReferenceResult, ModelProbe, CustomPassProbe);
#[cfg(feature = "wrong-abi")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_gameplay_query(
    _: u32,
    _: u32,
    _: *mut abi::AetherGameplayModuleApi,
) -> i32 {
    abi::AE_GAMEPLAY_ABI_MISMATCH
}

/// Fixture-only forwarding entry used by the bounded subprocess failure probe.
pub unsafe fn fixture_query(major: u32, size: u32, out: *mut abi::AetherGameplayModuleApi) -> i32 {
    #[cfg(not(feature = "wrong-abi"))]
    unsafe {
        __aether_gameplay_export::aether_gameplay_query(major, size, out)
    }
    #[cfg(feature = "wrong-abi")]
    unsafe {
        aether_gameplay_query(major, size, out)
    }
}
