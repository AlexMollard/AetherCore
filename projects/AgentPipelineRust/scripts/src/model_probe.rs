use aethercore::*;

// Shared by the native comparison project and browser fixture provider so
// authored camera/model motion follows exactly the same safe SDK calls.
gameplay_script! { pub struct ModelProbe {} }
impl Script for ModelProbe {
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        let mut position = ctx.position(ctx.entity())?;
        position.x += delta;
        ctx.set_position(ctx.entity(), position)
    }
}
