//! Probes for the 3D native-vs-browser gameplay parity fixtures (resources/web/reference/scenes/
//! Browser*3D*.scene.toml). Every observation is a published script property, so the parity harness
//! compares it at each checkpoint tick. projects/AgentPipelineCSharp/fixtures3d/Gameplay3D.cs publishes
//! the same properties with the same behaviour. These live outside `scripts/` on purpose: the 2D
//! production publisher scans every script under `scripts/`, and the project's 2D reference must keep
//! publishing; tools/web-runtime/gameplay-parity-checks.py injects this file into its staged project.
use aethercore::*;

const HUMAN: &str = "project://assets/models/Human/Human.gltf";

fn v3(x: f32, y: f32, z: f32) -> Vec3 {
    Vec3 { x, y, z }
}

// Dynamic bodies driven by script velocity, force and impulse; hinge and fixed joints.
gameplay_script! { pub struct Bodies3DProbe {
    pub ticks: i64 = 0,
    pub slider_set: Vec3 = Vec3::default(),
    pub slider_v30: Vec3 = Vec3::default(),
    pub pusher_v30: Vec3 = Vec3::default(),
    pub anchor_v10: Vec3 = Vec3::default(),
    pub anchor_v12: Vec3 = Vec3::default(),
    pub pendulum_v30: Vec3 = Vec3::default(),
    pub welded_v60: Vec3 = Vec3::default()
} }

impl Script for Bodies3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        let tick = self.ticks;
        self.ticks += 1;
        let slider = ctx.find_entity("Slider")?;
        let pusher = ctx.find_entity("Pusher")?;
        let anchor = ctx.find_entity("Anchor box")?;
        match tick {
            0 => {
                ctx.set_velocity_3d(slider, v3(3., 0., 0.))?;
                self.slider_set = ctx.velocity_3d(slider)?;
            }
            10 => {
                self.anchor_v10 = ctx.velocity_3d(anchor)?;
                ctx.add_impulse_3d(anchor, v3(0., 30., 0.))?;
            }
            12 => self.anchor_v12 = ctx.velocity_3d(anchor)?,
            30 => {
                self.slider_v30 = ctx.velocity_3d(slider)?;
                self.pusher_v30 = ctx.velocity_3d(pusher)?;
                let pendulum = ctx.find_entity("Pendulum")?;
                self.pendulum_v30 = ctx.velocity_3d(pendulum)?;
            }
            60 => {
                let welded = ctx.find_entity("Welded box")?;
                self.welded_v60 = ctx.velocity_3d(welded)?;
            }
            _ => {}
        }
        if tick < 20 {
            ctx.add_force_3d(pusher, v3(100., 0., 0.))?;
        }
        Ok(())
    }
}

/// One raycast result: distance (-1 on a miss), hit point, surface normal, and whether the hit
/// entity (or the absence of one) is the expected one.
struct Cast {
    distance: f32,
    point: Vec3,
    normal: Vec3,
    ok: bool,
}

fn cast(ctx: &mut Context<'_>, origin: Vec3, direction: Vec3, expect: &str) -> Result<Cast> {
    let hit = ctx.raycast_3d(origin, direction, 20.)?;
    let expected = if expect.is_empty() { Entity::NONE } else { ctx.find_entity(expect)? };
    Ok(match hit {
        Some(hit) => Cast {
            distance: hit.distance,
            point: hit.point,
            normal: hit.normal,
            ok: !expect.is_empty() && hit.entity == expected,
        },
        None => Cast { distance: -1., point: Vec3::default(), normal: Vec3::default(), ok: expect.is_empty() },
    })
}

// Script raycasts against static and dynamic bodies.
gameplay_script! { pub struct Raycast3DProbe {
    pub ticks: i64 = 0,
    pub floor_distance: f32 = -2.0,
    pub floor_point: Vec3 = Vec3::default(),
    pub floor_normal: Vec3 = Vec3::default(),
    pub floor_ok: bool = false,
    pub wall_distance: f32 = -2.0,
    pub wall_point: Vec3 = Vec3::default(),
    pub wall_normal: Vec3 = Vec3::default(),
    pub wall_ok: bool = false,
    pub ball_distance: f32 = -2.0,
    pub ball_point: Vec3 = Vec3::default(),
    pub ball_normal: Vec3 = Vec3::default(),
    pub ball_ok: bool = false,
    pub diamond_distance: f32 = -2.0,
    pub diamond_point: Vec3 = Vec3::default(),
    pub diamond_normal: Vec3 = Vec3::default(),
    pub diamond_ok: bool = false,
    pub oblique_distance: f32 = -2.0,
    pub oblique_point: Vec3 = Vec3::default(),
    pub oblique_normal: Vec3 = Vec3::default(),
    pub oblique_ok: bool = false,
    pub miss_distance: f32 = -2.0,
    pub miss_ok: bool = false,
    pub dropper_distance: f32 = -2.0,
    pub dropper_point: Vec3 = Vec3::default(),
    pub dropper_normal: Vec3 = Vec3::default(),
    pub dropper_ok: bool = false,
    pub rest_distance: f32 = -2.0,
    pub rest_ok: bool = false
} }

impl Script for Raycast3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        let tick = self.ticks;
        self.ticks += 1;
        match tick {
            1 => {
                let c = cast(ctx, v3(2., 5., 2.), v3(0., -1., 0.), "Floor")?;
                (self.floor_distance, self.floor_point, self.floor_normal, self.floor_ok) =
                    (c.distance, c.point, c.normal, c.ok);
                let c = cast(ctx, v3(0., 1., 0.), v3(1., 0., 0.), "Wall")?;
                (self.wall_distance, self.wall_point, self.wall_normal, self.wall_ok) =
                    (c.distance, c.point, c.normal, c.ok);
                let c = cast(ctx, v3(0., 1.5, 0.3), v3(-1., 0., 0.), "Ball")?;
                (self.ball_distance, self.ball_point, self.ball_normal, self.ball_ok) =
                    (c.distance, c.point, c.normal, c.ok);
                let c = cast(ctx, v3(0.5, 1., 0.), v3(0., 0., -1.), "Diamond")?;
                (self.diamond_distance, self.diamond_point, self.diamond_normal, self.diamond_ok) =
                    (c.distance, c.point, c.normal, c.ok);
                let c = cast(ctx, v3(-3., 3., 0.), v3(0.707_106_77, -0.707_106_77, 0.), "Floor")?;
                (self.oblique_distance, self.oblique_point, self.oblique_normal, self.oblique_ok) =
                    (c.distance, c.point, c.normal, c.ok);
                let c = cast(ctx, v3(0., 10., 0.), v3(0., 1., 0.), "")?;
                (self.miss_distance, self.miss_ok) = (c.distance, c.ok);
            }
            31 => {
                let c = cast(ctx, v3(8., 10., 0.), v3(0., -1., 0.), "Dropper")?;
                (self.dropper_distance, self.dropper_point, self.dropper_normal, self.dropper_ok) =
                    (c.distance, c.point, c.normal, c.ok);
            }
            119 => {
                let c = cast(ctx, v3(8., 10., 0.), v3(0., -1., 0.), "Dropper")?;
                (self.rest_distance, self.rest_ok) = (c.distance, c.ok);
            }
            _ => {}
        }
        Ok(())
    }
}

// Counts the 3D contact events delivered to the entity carrying it.
gameplay_script! { pub struct Contacts3DProbe {
    pub ticks: i64 = 0,
    pub collision_enter: i64 = 0,
    pub collision_exit: i64 = 0,
    pub trigger_enter: i64 = 0,
    pub trigger_exit: i64 = 0,
    pub events_2d: i64 = 0,
    pub touched_floor: bool = false
} }

impl Script for Contacts3DProbe {
    fn update(&mut self, _ctx: &mut Context<'_>, _delta: f32) -> Result {
        self.ticks += 1;
        Ok(())
    }

    fn event(&mut self, ctx: &mut Context<'_>, event: &Event) -> Result {
        let Some(kind) = event.physics_kind() else {
            return Ok(());
        };
        if !event.is_3d() {
            self.events_2d += 1;
            return Ok(());
        }
        match kind {
            PhysicsEventKind::CollisionEnter => {
                self.collision_enter += 1;
                self.touched_floor |= event.other == ctx.find_entity("Floor")?;
            }
            PhysicsEventKind::CollisionExit => self.collision_exit += 1,
            PhysicsEventKind::TriggerEnter => self.trigger_enter += 1,
            PhysicsEventKind::TriggerExit => self.trigger_exit += 1,
            PhysicsEventKind::Overlapping => {}
        }
        Ok(())
    }
}

// Character controller driven by arrow keys (move) and space (jump).
gameplay_script! { pub struct Walker3DProbe {
    pub ticks: i64 = 0,
    pub grounded: bool = false,
    pub grounded_ticks: i64 = 0,
    pub airborne_ticks: i64 = 0,
    pub jumps: i64 = 0,
    pub max_y: f32 = -100.0,
    pub space_was_down: bool = false
} }

impl Script for Walker3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        self.ticks += 1;
        let me = ctx.entity();
        let mut direction = Vec3::default();
        if ctx.key_down(262)? {
            direction.x += 1.;
        }
        if ctx.key_down(263)? {
            direction.x -= 1.;
        }
        if ctx.key_down(265)? {
            direction.z -= 1.;
        }
        if ctx.key_down(264)? {
            direction.z += 1.;
        }
        ctx.character_move(me, direction, 3.)?;
        let space = ctx.key_down(32)?;
        if space && !self.space_was_down {
            ctx.character_jump(me, 5.)?;
            self.jumps += 1;
        }
        self.space_was_down = space;
        self.grounded = ctx.character_is_grounded(me)?;
        if self.grounded {
            self.grounded_ticks += 1;
        } else {
            self.airborne_ticks += 1;
        }
        self.max_y = self.max_y.max(ctx.position(me)?.y);
        Ok(())
    }

    fn event(&mut self, _ctx: &mut Context<'_>, _event: &Event) -> Result {
        Ok(())
    }
}

// Builds a ragdoll on its entity at tick 0 and checks a second spawn is refused.
gameplay_script! { pub struct Ragdoll3DProbe {
    pub ticks: i64 = 0,
    pub spawned: bool = false,
    pub root_is_self: bool = false,
    pub respawn_refused: bool = false
} }

impl Script for Ragdoll3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        let tick = self.ticks;
        self.ticks += 1;
        if tick == 0 {
            let me = ctx.entity();
            let root = ctx.spawn_ragdoll(me, HUMAN);
            self.spawned = root.is_ok();
            self.root_is_self = matches!(root, Ok(root) if root == me);
            self.respawn_refused = ctx.spawn_ragdoll(me, HUMAN).is_err();
        }
        Ok(())
    }

    fn event(&mut self, _ctx: &mut Context<'_>, _event: &Event) -> Result {
        Ok(())
    }
}

// Skinned clip switch, cross-fade and speed changes on the Human; publishes the live clip and time.
gameplay_script! { pub struct Skin3DProbe {
    pub ticks: i64 = 0,
    pub ready_tick: i64 = -1,
    pub clip: i64 = -1,
    pub clip_time: f32 = -1.0,
    pub walk_index: i64 = -1
} }

impl Script for Skin3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        let tick = self.ticks;
        self.ticks += 1;
        let me = ctx.entity();
        if self.ready_tick < 0 {
            // The model's clips become available once its asynchronous load completes.
            if ctx.set_animation_clip(me, ClipRef::Index(0), 0.).is_err() {
                return Ok(());
            }
            self.ready_tick = tick;
        }
        match tick - self.ready_tick {
            10 => {
                self.walk_index = i64::from(ctx.find_animation_clip(me, "Walk")?);
                ctx.set_animation_clip(me, ClipRef::Name("Walk"), 0.)?;
            }
            25 => ctx.set_animation_clip(me, ClipRef::Name("mixamo.com"), 0.5)?,
            40 => ctx.set_animation_speed(me, 2.)?,
            70 => ctx.set_animation_speed(me, 0.5)?,
            _ => {}
        }
        self.clip = i64::from(ctx.current_animation_clip(me)?);
        self.clip_time = ctx.animation_time(me)?;
        Ok(())
    }
}

// Instantiates 3D prefabs from script, places them, destroys one and instantiates another.
gameplay_script! { pub struct Spawner3DProbe {
    pub ticks: i64 = 0,
    pub spawned_count: i64 = 0,
    pub destroyed_count: i64 = 0,
    pub first: Entity = Entity::NONE,
    pub second: Entity = Entity::NONE,
    pub third: Entity = Entity::NONE
} }

impl Script for Spawner3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        self.ticks += 1;
        match self.ticks {
            5 => {
                self.first = ctx.instantiate_prefab("Crate3D")?;
                ctx.set_position(self.first, v3(-1., 4., 0.))?;
                self.spawned_count += 1;
            }
            15 => {
                self.second = ctx.instantiate_prefab("Barrel3D")?;
                ctx.set_position(self.second, v3(1.5, 6., 0.))?;
                self.spawned_count += 1;
            }
            25 => {
                ctx.destroy_entity(self.first)?;
                self.destroyed_count += 1;
            }
            40 => {
                self.third = ctx.instantiate_prefab("Crate3D")?;
                ctx.set_position(self.third, v3(0., 3., 0.))?;
                self.spawned_count += 1;
            }
            _ => {}
        }
        Ok(())
    }
}

// Lets a body fall for 20 ticks, saves a marker and loads the second 3D scene.
gameplay_script! { pub struct Transition3DProbe { pub ticks: i64 = 0 } }

impl Script for Transition3DProbe {
    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        self.ticks += 1;
        if self.ticks == 20 {
            ctx.write_save("transition3d_marker", &format!("left after {} ticks", self.ticks))?;
            ctx.load_scene("BrowserTransition3DResult")?;
        }
        Ok(())
    }

    fn event(&mut self, _ctx: &mut Context<'_>, _event: &Event) -> Result {
        Ok(())
    }
}

// Runs in the destination scene: reads the pre-transition save and tracks its sphere.
gameplay_script! { pub struct TransitionResult3DProbe {
    pub ticks: i64 = 0,
    pub marker: String = String::new(),
    pub arrival_y: f32 = -100.0
} }

impl Script for TransitionResult3DProbe {
    fn attach(&mut self, ctx: &mut Context<'_>) -> Result {
        self.marker = ctx.read_save("transition3d_marker")?;
        Ok(())
    }

    fn update(&mut self, ctx: &mut Context<'_>, _delta: f32) -> Result {
        self.ticks += 1;
        self.arrival_y = ctx.position(ctx.entity())?.y;
        Ok(())
    }

    fn event(&mut self, _ctx: &mut Context<'_>, _event: &Event) -> Result {
        Ok(())
    }
}
