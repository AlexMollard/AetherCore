use std::sync::atomic::Ordering;
use aethercore::{gameplay_script, Context, Entity, Result, Script, Vec3};
use crate::player::CONTROLS_BLOCKED;

const SAVE_KEY: &str = "fox_world_sites";
const CHIME: &str = "project://assets/audio/ruin-chime.wav";
const TRAVEL_CONTROLS: &str = "Drag orbit   Wheel zoom   R respawn";
const SITES: [(&str, f32, f32, &str); 5] = [
    ("Trailhead journal", 1.0, 30.0, "Follow the narrow trails through the valley. Deer graze in the western meadow; wolves shelter in the eastern woodland. Read the four distant field notes to complete your journal."),
    ("Pond field notes", -30.0, 37.0, "The pond collects water below the meadow. Its muddy banks, stone crossing and shaded trees form a quiet habitat. Watch the grazing deer beyond the shore."),
    ("Sanctuary relic", -5.0, -16.0, "Within the paved courtyard, a carved weight hangs from an old stone frame. F sends a stone toward it; G returns the resting stone. Escape closes these notes."),
    ("Woodland shrine", 46.0, -25.0, "The eastern glade shelters the wolves. Wind moves the foliage above the narrow forest trail. Follow the foothill path north to the warm mineral outcrops."),
    ("Coastal lookout", -47.0, 25.0, "The old watchpost faces the sunset across the sea. Sand replaces the meadow near the wet shoreline. The lookout and sanctuary belong to the same forgotten route."),
];
const JOURNAL_CHILDREN: &[&str] = &["Journal page", "Journal footer", "Journal switch", "Journal switch label", "Journal slider", "Journal slider label", "Journal map caption", "Journal map", "Journal atlas heading", "Journal body", "Journal progress", "Journal title", "Journal chapter", "Journal background", "Journal backdrop"];
const STAMP_LABELS: [&str; 5] = ["I", "II", "III", "IV", "V"];
fn v(x: f32, y: f32, z: f32) -> Vec3 { Vec3 { x, y, z } }

gameplay_script! { pub struct RuinExperiment {
    actor: Entity = Entity::NONE,
    stone: Entity = Entity::NONE,
    status: Entity = Entity::NONE,
    progress: Entity = Entity::NONE,
    guide: Entity = Entity::NONE,
    launched: Entity = Entity::NONE,
    location: Entity = Entity::NONE,
    stamp_one: Entity = Entity::NONE,
    stamp_two: Entity = Entity::NONE,
    stamp_three: Entity = Entity::NONE,
    stamp_four: Entity = Entity::NONE,
    stamp_five: Entity = Entity::NONE,
    modal: Entity = Entity::NONE,
    modal_body: Entity = Entity::NONE,
    nearest: i64 = -1,
    pub modal_site: i64 = -1,
    pub visited: i64 = 0,
    e_held: bool = false,
    f_held: bool = false,
    g_held: bool = false
} }

impl RuinExperiment {
    fn update_progress(&self, ctx: &mut Context<'_>) -> Result {
        ctx.set_ui_text(self.progress, &format!("{} OF 5 FIELD NOTES", (self.visited as u32).count_ones()))?;
        for (index, stamp) in [self.stamp_one, self.stamp_two, self.stamp_three, self.stamp_four, self.stamp_five].iter().enumerate() {
            ctx.set_ui_text(*stamp, if self.visited & (1 << index) != 0 { STAMP_LABELS[index] } else { "" })?;
        }
        Ok(())
    }

    fn close(&mut self, ctx: &mut Context<'_>) -> Result {
        for name in JOURNAL_CHILDREN {
            let child = ctx.find_entity(name)?;
            ctx.destroy_entity(child)?;
        }
        ctx.destroy_entity(self.modal)?;
        self.modal = Entity::NONE;
        self.modal_body = Entity::NONE;
        self.modal_site = -1;
        CONTROLS_BLOCKED.store(false, Ordering::Relaxed);
        ctx.set_ui_text(self.guide, TRAVEL_CONTROLS)?;
        Ok(())
    }

    fn open(&mut self, ctx: &mut Context<'_>, site: usize) -> Result {
        self.modal = ctx.instantiate_prefab("FieldJournal")?;
        self.modal_body = ctx.find_entity("Journal body")?;
        self.modal_site = site as i64;
        self.visited |= 1 << site;
        let saved = ctx.write_save(SAVE_KEY, &self.visited.to_string()).is_ok();
        ctx.play_audio(CHIME)?;
        let title = ctx.find_entity("Journal title")?;
        ctx.set_ui_text(title, SITES[site].0)?;
        ctx.set_ui_text(self.modal_body, SITES[site].3)?;
        let progress = ctx.find_entity("Journal progress")?;
        ctx.set_ui_text(progress, &format!("{}/5 places recorded  |  {}", (self.visited as u32).count_ones(), if saved { "save queued" } else { "save unavailable" }))?;
        let page = ctx.find_entity("Journal page")?;
        ctx.set_ui_text(page, &format!("{:02} / 05", site + 1))?;
        self.update_progress(ctx)?;
        ctx.set_ui_text(self.status, "[ Esc ]  Return to the valley")?;
        ctx.set_ui_text(self.location, "Field journal")?;
        ctx.set_ui_text(self.guide, "Journal open   Tab / arrows adjust   Esc return")?;
        CONTROLS_BLOCKED.store(true, Ordering::Relaxed);
        Ok(())
    }
}

impl Script for RuinExperiment {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.actor = ctx.find_entity("Explorer")?;
        self.stone = ctx.find_entity("Ruin resting stone")?;
        self.status = ctx.find_entity("Ruin status")?;
        self.progress = ctx.find_entity("Journal progress HUD")?;
        self.guide = ctx.find_entity("Exploration guide")?;
        self.location = ctx.find_entity("Interaction location")?;
        self.stamp_one = ctx.find_entity("Discovery stamp 0")?;
        self.stamp_two = ctx.find_entity("Discovery stamp 1")?;
        self.stamp_three = ctx.find_entity("Discovery stamp 2")?;
        self.stamp_four = ctx.find_entity("Discovery stamp 3")?;
        self.stamp_five = ctx.find_entity("Discovery stamp 4")?;
        self.visited = ctx.read_save(SAVE_KEY).ok().and_then(|s| s.parse::<i64>().ok()).filter(|n| (0..32).contains(n)).unwrap_or(0);
        CONTROLS_BLOCKED.store(false, Ordering::Relaxed);
        self.update_progress(ctx)?;
        Ok(())
    }

    fn update(&mut self, ctx: &mut Context<'_>, _: f32) -> Result {
        let e = ctx.key_down(69)?;
        let f = ctx.key_down(70)?;
        let g = ctx.key_down(71)?;
        if self.modal != Entity::NONE {
            if ctx.key_down(256)? {
                self.close(ctx)?;
                self.nearest = -2;
            } else if self.modal_site == 2 {
                if f && !self.f_held {
                    if self.launched != Entity::NONE { ctx.destroy_entity(self.launched)?; }
                    let hit = ctx.raycast_3d(v(-5.0, 4.0, -16.0), v(0.0, -1.0, 0.0), 5.0)?;
                    self.launched = ctx.instantiate_prefab("RuinStone")?;
                    ctx.set_position(self.launched, v(-2.8, 1.0, -14.65))?;
                    ctx.set_velocity_3d(self.launched, v(0.0, 1.0, -3.5))?;
                    ctx.play_audio(CHIME)?;
                    ctx.set_ui_text(self.modal_body, if matches!(hit, Some(h) if h.entity == self.stone) {
                        "The resting stone is still in its place. A second stone travels toward the hanging relic; G returns the resting stone."
                    } else { "A stone travels toward the hanging relic. The resting stone has moved; G returns it to its place." })?;
                }
                if g && !self.g_held {
                    if self.launched != Entity::NONE { ctx.destroy_entity(self.launched)?; self.launched = Entity::NONE; }
                    ctx.set_position(self.stone, v(-5.0, 0.45, -16.0))?;
                    ctx.set_euler(self.stone, v(0.0, 0.0, 0.0))?;
                    ctx.set_velocity_3d(self.stone, v(0.0, 0.0, 0.0))?;
                    ctx.set_ui_text(self.modal_body, SITES[2].3)?;
                }
            }
        } else {
            let p = ctx.position(self.actor)?;
            let mut nearest = -1;
            let mut best = 4.5 * 4.5;
            for (index, (_, x, z, _)) in SITES.iter().enumerate() {
                let d = (p.x - x).powi(2) + (p.z - z).powi(2);
                if d < best { nearest = index as i64; best = d; }
            }
            if nearest != self.nearest {
                self.nearest = nearest;
                ctx.set_ui_text(self.status, if nearest >= 0 { "[ E ]  Read field notes" } else { "Follow the field-note trail" })?;
                ctx.set_ui_text(self.location, if nearest >= 0 { SITES[nearest as usize].0 } else { "Explore the valley" })?;
            }
            if e && !self.e_held && nearest >= 0 { self.open(ctx, nearest as usize)?; }
        }
        self.e_held = e;
        self.f_held = f;
        self.g_held = g;
        Ok(())
    }
}
