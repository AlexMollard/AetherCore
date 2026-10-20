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
export_gameplay!(BrowserMover, ReferencePlayer, ReferenceResult);

#[cfg(feature = "automation")]
#[unsafe(no_mangle)]
pub extern "C" fn aether_browser_test_panic() { panic!("Browser automation panic fixture"); }
