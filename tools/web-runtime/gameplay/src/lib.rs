use aethercore::*;
gameplay_script! { pub struct BrowserMover { pub ticks: i64 = 0 } }
impl Script for BrowserMover {
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        self.ticks += 1;
        let right = ctx.key_down(262)?;
        let left = ctx.key_down(263)?;
        let mut position = ctx.position(ctx.entity())?;
        position.x += (i32::from(right) - i32::from(left)) as f32 * delta;
        ctx.set_position(ctx.entity(), position)
    }
}
#[path = "../../../../projects/AgentPipelineRust/scripts/src/reference.rs"]
mod reference;
use reference::{ReferencePlayer, ReferenceResult};
#[path = "../../../../projects/EmberRun/scripts/src/game.rs"]
mod ember;
use ember::EmberRun;
#[path = "../../../../projects/AgentPipelineRust/scripts/src/model_probe.rs"]
mod model_probe;
use model_probe::ModelProbe;
#[path = "../../../../projects/AgentPipelineRust/scripts/src/custom_pass.rs"]
mod custom_pass;
use custom_pass::CustomPassProbe;
#[path = "../../../../projects/AgentPipelineRust/fixtures3d/gameplay3d.rs"]
mod gameplay3d;
use gameplay3d::{
    Bodies3DProbe, Contacts3DProbe, Ragdoll3DProbe, Raycast3DProbe, Skin3DProbe, Spawner3DProbe,
    Transition3DProbe, TransitionResult3DProbe, Walker3DProbe,
};
export_gameplay!(BrowserMover, ReferencePlayer, ReferenceResult, EmberRun, ModelProbe, CustomPassProbe,
    Bodies3DProbe, Raycast3DProbe, Contacts3DProbe, Walker3DProbe, Ragdoll3DProbe, Skin3DProbe, Spawner3DProbe,
    Transition3DProbe, TransitionResult3DProbe);

#[cfg(feature = "automation")]
#[unsafe(no_mangle)]
pub extern "C" fn aether_browser_test_panic() { panic!("Browser automation panic fixture"); }
