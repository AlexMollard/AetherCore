use aethercore::*;

pub fn move_player(mut p: Vec3, x: f32, y: f32, speed: f32, delta: f32) -> Vec3 {
    let length = (x * x + y * y).sqrt().max(1.0);
    p.x = (p.x + x / length * speed * delta).clamp(-5.5, 5.5);
    p.y = (p.y + y / length * speed * delta).clamp(-2.8, 2.8);
    p
}

gameplay_script! { pub struct EmberRun {
    pub ticks: i64 = 0,
    pub score: i64 = 0,
    pub lives: i64 = 3,
    pub best: i64 = 0,
    pub invulnerable: i64 = 0,
    pub dash: i64 = 0,
    pub cooldown: i64 = 0,
    pub finished: bool = false,
    pub restart_held: bool = false
} }

impl EmberRun {
    fn reset(&mut self, ctx: &mut Context<'_>) -> Result {
        self.ticks = 0;
        self.score = 0;
        self.lives = 3;
        self.finished = false;
        self.invulnerable = 0;
        self.dash = 0;
        self.cooldown = 0;
        ctx.set_position(
            ctx.entity(),
            Vec3 {
                x: 0.0,
                y: -2.4,
                z: 0.0,
            },
        )?;
        for (i, (x, y)) in [(0.0, -0.9), (-3.8, 1.8), (3.8, 1.8)]
            .into_iter()
            .enumerate()
        {
            let e = ctx.find_entity(&format!("Ember{}", i))?;
            ctx.set_position(e, Vec3 { x, y, z: 0.0 })?;
        }
        Ok(())
    }
}

impl Script for EmberRun {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.best = ctx
            .read_save("ember_best")
            .unwrap_or_default()
            .parse()
            .unwrap_or(0);
        self.reset(ctx)
    }
    fn update(&mut self, ctx: &mut Context<'_>, delta: f32) -> Result {
        let restart = ctx.key_down(82)?;
        if restart && !self.restart_held {
            self.reset(ctx)?;
        }
        self.restart_held = restart;
        let hud = ctx.find_entity("Counter")?;
        if self.finished {
            return ctx.set_ui_text(
                hud,
                &format!(
                    "{} EMBERS  |  BEST {}  |  R TO RETRY",
                    self.score, self.best
                ),
            );
        }
        self.ticks += 1;
        self.invulnerable = (self.invulnerable - 1).max(0);
        self.cooldown = (self.cooldown - 1).max(0);
        self.dash = (self.dash - 1).max(0);
        if ctx.key_down(32)? && self.cooldown == 0 {
            self.dash = 12;
            self.cooldown = 72;
            self.invulnerable = self.invulnerable.max(12);
        }
        let right = ctx.key_down(68)? || ctx.key_down(262)?;
        let left = ctx.key_down(65)? || ctx.key_down(263)?;
        let up = ctx.key_down(87)? || ctx.key_down(265)?;
        let down = ctx.key_down(83)? || ctx.key_down(264)?;
        let mut player = move_player(
            ctx.position(ctx.entity())?,
            (right as i32 - left as i32) as f32,
            (up as i32 - down as i32) as f32,
            if self.dash > 0 { 10.0 } else { 3.8 },
            delta,
        );
        ctx.set_position(ctx.entity(), player)?;
        let time = self.ticks as f32 / 60.0;
        for i in 0..3 {
            let hazard = ctx.find_entity(&format!("Hazard{}", i))?;
            let p = Vec3 {
                x: (time * (0.8 + i as f32 * 0.17) + i as f32 * 2.1).sin() * 4.6,
                y: [0.4, -1.5, 1.9][i],
                z: 0.0,
            };
            ctx.set_position(hazard, p)?;
            if (player.x - p.x).powi(2) + (player.y - p.y).powi(2) < 0.48 * 0.48
                && self.invulnerable == 0
            {
                self.lives -= 1;
                self.invulnerable = 90;
                player = Vec3 {
                    x: 0.0,
                    y: -2.4,
                    z: 0.0,
                };
                ctx.set_position(ctx.entity(), player)?;
                ctx.play_audio("project://assets/ember/hit.wav")?;
            }
        }
        for i in 0..3 {
            let ember = ctx.find_entity(&format!("Ember{}", i))?;
            let p = ctx.position(ember)?;
            let pulse = 0.78 + 0.22 * (time * 4.0 + i as f32).sin().abs();
            ctx.set_sprite_color(ember, [1.0, pulse, pulse, 1.0])?;
            if (player.x - p.x).powi(2) + (player.y - p.y).powi(2) < 0.55 * 0.55 {
                self.score += 10;
                let positions = [
                    (-4.2, -2.1),
                    (4.2, 2.1),
                    (-2.0, 1.0),
                    (2.7, -1.0),
                    (0.0, 2.4),
                    (4.0, -2.0),
                    (-4.0, 2.0),
                    (0.0, -1.0),
                ];
                let (x, y) = positions[(self.score as usize / 10 + i * 3) % positions.len()];
                ctx.set_position(ember, Vec3 { x, y, z: 0.0 })?;
                ctx.play_audio("project://assets/ember/chime.wav")?;
            }
        }
        let flash = self.invulnerable > 0 && self.ticks % 10 < 5;
        ctx.set_sprite_color(
            ctx.entity(),
            if flash {
                [1.0, 1.0, 1.0, 0.35]
            } else {
                [1.0; 4]
            },
        )?;
        if self.lives <= 0 || self.ticks >= 1800 {
            self.finished = true;
            self.best = self.best.max(self.score);
            ctx.write_save("ember_best", &self.best.to_string())?;
        }
        let seconds = (1800 - self.ticks).max(0) / 60;
        ctx.set_ui_text(
            hud,
            &format!(
                "{} EMBERS  |  {} LIVES  |  {:02}s",
                self.score, self.lives, seconds
            ),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use aethercore::Vec3;
    #[test]
    fn diagonal_speed_matches_straight_and_stays_in_bounds() {
        let a = move_player(Vec3::default(), 1.0, 0.0, 4.0, 0.1);
        let b = move_player(Vec3::default(), 1.0, 1.0, 4.0, 0.1);
        assert!((a.x * a.x - (b.x * b.x + b.y * b.y)).abs() < 0.00001);
        let p = move_player(
            Vec3 {
                x: 5.4,
                y: 2.7,
                z: 0.0,
            },
            1.0,
            1.0,
            20.0,
            1.0,
        );
        assert!(p.x <= 5.5 && p.y <= 2.8);
    }
}
