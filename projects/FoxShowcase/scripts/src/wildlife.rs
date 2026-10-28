use aethercore::{gameplay_script, ClipRef, Context, Result, Script, Vec3};

// The landscape's analytic height function, shared with assets/models/sanctuary/terrain_height.py.
fn terrain_height(x: f32, z: f32) -> f32 {
    if x <= -64.0 { return -0.65; }
    if x < -56.0 { return -0.65 + (x + 64.0) * (0.55 / 8.0); }
    if x < -45.0 { return -0.1 + (x + 56.0) * (0.4 / 11.0); }
    if x.abs() <= 20.0 && z.abs() <= 20.0 { return 0.0; }
    let fade = ((x.abs().max(z.abs()) - 20.0) / 10.0).clamp(0.0, 1.0);
    let rolling = fade * 0.30 * (x * 0.12).sin() * (z * 0.10).cos();
    let rise = ((-z - 35.0) / 25.0).clamp(0.0, 1.0);
    let rise = rise * rise * (3.0 - 2.0 * rise);
    let mut peaks = 0.0;
    if rise > 0.0 {
        for (px, pz, wx, wz, amplitude) in [(-34.0, -75.0, 18.0, 22.0, 28.0), (0.0, -76.0, 21.0, 24.0, 31.0), (38.0, -73.0, 19.0, 22.0, 29.0)] {
            peaks += amplitude * (-((x - px) / wx).powi(2) - ((z - pz) / wz).powi(2)).exp();
        }
    }
    let inland = rolling + rise * (peaks + 1.5 + 1.4 * (x * 0.14 + z * 0.09).sin().abs());
    let pond_radius_squared = ((x + 30.0).powi(2) + (z - 42.0).powi(2)) / 30.25;
    let inland = inland - 0.7 * (1.0 - pond_radius_squared).max(0.0);
    if x < -40.0 {
        let blend = (x + 45.0) / 5.0;
        0.3 * (1.0 - blend) + inland * blend
    } else { inland }
}

const DEER: &[&str] = &["Meadow deer walking part 0", "Meadow deer walking part 1", "Meadow deer walking part 2", "Meadow deer walking part 3", "Meadow deer walking part 4", "Meadow deer walking part 5", "Meadow deer walking part 6"];
const WOLF: &[&str] = &["Woodland wolf walking part 0", "Woodland wolf walking part 1", "Woodland wolf walking part 2", "Woodland wolf walking part 3"];
const FOX: &[&str] = &["Fox part 0"];

gameplay_script! { pub struct WildlifeWalker {
    pub center_x: f32 = 0.0,
    pub center_z: f32 = 0.0,
    pub radius_x: f32 = 1.0,
    pub radius_z: f32 = 1.0,
    pub phase: f32 = 0.0,
    pub speed: f32 = 1.0,
    pub species: i64 = 0,
    pub cycle: f32 = 0.0,
    pub travelling: bool = true,
    pub position: Vec3 = Vec3 { x: 0.0, y: 0.0, z: 0.0 }
} }

impl Script for WildlifeWalker {
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        // Ten seconds of travel, then three of grazing/surveying. Split at phase
        // boundaries so even a long frame never moves during the resting interval.
        let mut remaining = delta.max(0.0);
        while remaining > 0.0 {
            let travelling = self.cycle < 10.0;
            let end = if travelling { 10.0 } else { 13.0 };
            let step = remaining.min(end - self.cycle);
            if travelling {
                let (sin, cos) = self.phase.sin_cos();
                let tangent = ((self.radius_x * sin).powi(2) + (self.radius_z * cos).powi(2)).sqrt();
                if tangent > 0.0 {
                    self.phase = (self.phase + step * self.speed / tangent).rem_euclid(std::f32::consts::TAU);
                }
            }
            self.cycle += step;
            remaining -= step;
            if self.cycle >= 13.0 { self.cycle = 0.0; }
        }
        let travelling = self.cycle < 10.0;
        if travelling != self.travelling {
            let (parts, clip) = match self.species {
                0 => (DEER, if travelling { 12 } else { 3 }),
                1 => (WOLF, if travelling { 11 } else { 5 }),
                _ => (FOX, if travelling { 1 } else { 0 }),
            };
            for name in parts {
                let part = ctx.find_entity(name)?;
                ctx.set_animation_clip(part, ClipRef::Index(clip), 0.2)?;
            }
            self.travelling = travelling;
        }
        let (sin, cos) = self.phase.sin_cos();
        let x = self.center_x + self.radius_x * cos;
        let z = self.center_z + self.radius_z * sin;
        self.position = Vec3 { x, y: terrain_height(x, z) + 0.04, z };
        // All three source skeletons face +Z: head lies ahead of the tail.
        let yaw = (-self.radius_x * sin).atan2(self.radius_z * cos).to_degrees();
        ctx.set_position(ctx.entity(), self.position)?;
        ctx.set_euler(ctx.entity(), Vec3 { x: 0.0, y: yaw, z: 0.0 })
    }
}

#[cfg(test)]
mod tests {
    use super::terrain_height;
    #[test]
    fn habitat_height_matches_landscape_samples() {
        for (x, z, expected) in [(-22.0, 16.0, 0.00084239), (-27.0, 30.0, -0.02917961), (52.0, 5.0, -0.01136607), (51.0, -10.0, -0.02633358), (-2.0, 24.0, 0.02103365)] {
            assert!((terrain_height(x, z) - expected).abs() < 0.00001);
        }
    }
}
