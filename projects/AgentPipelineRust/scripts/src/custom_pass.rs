use aethercore::*;

// Shared by the native comparison project and the browser fixture provider: three custom passes, one per
// stage, re-submitted every tick with fixed data (no time dependence) so native and Web must agree exactly.
// Layout per rectangle (custom_pass_fixture.slang): [min.x, min.y, max.x, max.y], [border, 0, 0, 0].
const SHADER: &str = "custom_pass_fixture";
const BORDER: f32 = 0.25;

// Behind the sprite: lies partly under the white sprite at (-3..-1, 1..3).
const BEHIND_RECT: [f32; 4] = [-2.0, 1.5, 0.5, 2.5];
// Over the sprite layer, below the lamp: the mirror image (about y = 0) of the sprite.
const OVER_RECT: [f32; 4] = [-3.0, -3.0, -1.0, -1.0];
// Emissive: one rectangle outside the lamp radius and one on top of the lamp.
const EMISSIVE_DARK_RECT: [f32; 4] = [2.5, -3.25, 3.75, -2.25];
const EMISSIVE_LIT_RECT: [f32; 4] = [-2.125, -0.5, -0.875, 0.5];

fn aabb(rects: &[[f32; 4]]) -> [f32; 4] {
    rects.iter().fold([f32::MAX, f32::MAX, f32::MIN, f32::MIN], |a, r| {
        [a[0].min(r[0]), a[1].min(r[1]), a[2].max(r[2]), a[3].max(r[3])]
    })
}

fn records(rects: &[[f32; 4]]) -> Vec<[f32; 4]> {
    rects.iter().flat_map(|r| [*r, [BORDER, 0.0, 0.0, 0.0]]).collect()
}

// `active = false` (BrowserCustomPassOff) leaves the pass unsubmitted, so nothing draws.
// Negative fixtures: a non-empty `shader` overrides the registered shader stem (a missing cooked shader must fail
// explicitly) and `flood` adds that many extra full-size (4096 float4) submissions per tick (the Web per-frame
// budget must reject the frame before GPU submission).
gameplay_script! { pub struct CustomPassProbe { pub active: bool = true, pub shader: String = String::new(), pub flood: i64 = 0 } }
impl CustomPassProbe {
    fn submit(ctx: &mut Context<'_>, name: &str, rects: &[[f32; 4]], fill: [f32; 4], border: [f32; 4]) -> Result {
        ctx.submit_custom_pass(name, &records(rects), aabb(rects), fill, border)
    }
    fn stem(&self) -> &str {
        if self.shader.is_empty() { SHADER } else { &self.shader }
    }
}
impl Script for CustomPassProbe {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        if !self.active {
            return Ok(());
        }
        ctx.register_custom_pass("fixture_behind", self.stem(), CustomPassStage::BehindScene)?;
        ctx.register_custom_pass("fixture_over", self.stem(), CustomPassStage::OverScene)?;
        ctx.register_custom_pass("fixture_emissive", self.stem(), CustomPassStage::EmissiveOverLight)
    }
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        if !self.active {
            return Ok(());
        }
        Self::submit(ctx, "fixture_behind", &[BEHIND_RECT], [0.15, 0.85, 0.25, 1.0], [0.9, 0.2, 0.8, 1.0])?;
        Self::submit(ctx, "fixture_over", &[OVER_RECT], [1.0, 1.0, 1.0, 1.0], [1.0, 0.5, 0.1, 1.0])?;
        Self::submit(
            ctx,
            "fixture_emissive",
            &[EMISSIVE_DARK_RECT, EMISSIVE_LIT_RECT],
            [0.2, 0.5, 1.0, 1.0],
            [1.0, 0.9, 0.2, 1.0],
        )?;
        let big = vec![[0.0f32; 4]; 4096];
        for _ in 0..self.flood.clamp(0, 60) {
            ctx.submit_custom_pass("fixture_over", &big, [0.0; 4], [0.0; 4], [0.0; 4])?;
        }
        Ok(())
    }
    fn detach(&mut self, ctx: &mut Context<'_>) -> Result {
        if !self.active {
            return Ok(());
        }
        ctx.unregister_custom_pass("fixture_behind")?;
        ctx.unregister_custom_pass("fixture_over")?;
        ctx.unregister_custom_pass("fixture_emissive")
    }
}
