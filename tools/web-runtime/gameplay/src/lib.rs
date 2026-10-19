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
export_gameplay!(BrowserMover);
