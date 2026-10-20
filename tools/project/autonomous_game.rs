use aethercore::*;

gameplay_script! { pub struct ReferencePlayer {
    pub ticks: i64 = 0,
    pub score: i64 = 0,
    pub collision: bool = false,
    pub trigger: bool = false,
    pub spawned: Entity = Entity::NONE
} }

impl Script for ReferencePlayer {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result { ctx.log("Autonomous build 1") }
    fn update(&mut self, ctx: &mut Context<'_>, _: f32) -> Result {
        self.ticks += 1;
        ctx.set_velocity_2d(ctx.entity(), 2., 0.)?;
        if ctx.key_down(32)? {
            self.score += 1;
        }
        let counter = ctx.find_entity("Counter")?;
        ctx.set_ui_text(counter, &format!("Score {}", self.score))?;
        if self.ticks == 5 {
            self.spawned = ctx.instantiate_prefab("ReferencePickup")?;
            ctx.play_audio("project://assets/audio/reference.wav")?;
        }
        if self.ticks == 15 {
            ctx.destroy_entity(self.spawned)?;
        }
        if self.ticks == 60 {
            ctx.write_save(
                "reference_score",
                &format!("{},{},{}", self.score, self.collision, self.trigger),
            )?;
            ctx.write_save("reference_label", "caf\u{e9}\0agent")?;
            ctx.load_scene("ReferenceResult")?;
        }
        Ok(())
    }
    fn event(&mut self, _: &mut Context<'_>, event: &Event) -> Result {
        if event.kind == abi::AE_GAMEPLAY_EVENT_PHYSICS {
            if event.code == 0 {
                self.collision = true;
            }
            if event.code == 2 {
                self.trigger = true;
            }
        }
        Ok(())
    }
}

gameplay_script! { pub struct ReferenceResult {
    pub ticks: i64 = 0,
    pub score: i64 = 0,
    pub collision: bool = false,
    pub trigger: bool = false,
    pub label: String = String::new()
} }

impl Script for ReferenceResult {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        let saved = ctx.read_save("reference_score")?;
        let parts: Vec<_> = saved.split(',').collect();
        if parts.len() != 3 {
            return Err(Error(1, "Invalid reference score".into()));
        }
        self.score = parts[0]
            .parse()
            .map_err(|_| Error(1, "Invalid reference score".into()))?;
        self.collision = parts[1] == "true";
        self.trigger = parts[2] == "true";
        self.label = ctx.read_save("reference_label")?;
        let counter = ctx.find_entity("Counter")?;
        ctx.set_ui_text(counter, &format!("Saved score {}", self.score))
    }
    fn update(&mut self, _: &mut Context<'_>, _: f32) -> Result {
        self.ticks += 1;
        Ok(())
    }
}

export_gameplay!(ReferencePlayer, ReferenceResult);
