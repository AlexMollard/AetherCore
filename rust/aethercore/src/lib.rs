//! Scoped gameplay API. Engine values are copied; ECS references never escape a call.
//!
//! ```compile_fail
//! use aethercore::Context;
//! fn retain<'a>(ctx: Context<'a>) -> Context<'static> { ctx }
//! ```
//! ```compile_fail
//! fn require_send<T: Send>() {}
//! require_send::<aethercore::Context<'static>>();
//! ```
//! ```compile_fail
//! use aethercore::{Context, Entity, Vec3};
//! fn borrow<'a>(ctx: &'a mut Context<'_>, e: Entity) -> &'a mut Vec3 { ctx.position(e).unwrap() }
//! ```
use abi::services::*;
use abi::*;
pub use aethercore_abi as abi;
use std::{marker::PhantomData, rc::Rc};
pub mod provider;
#[derive(Debug, Clone, PartialEq)]
pub struct Error(pub i32, pub String);
pub type Result<T = ()> = std::result::Result<T, Error>;
/// Where a custom render pass draws, matching the C# `CustomPassStage`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CustomPassStage {
    /// Before sprites and tiles; the project's own sprites draw on top.
    BehindScene = 0,
    /// After sprites and tiles but before the 2D light map, so the pass is lit and shadowed.
    OverScene = 1,
    /// After the 2D light map; the pass keeps its own brightness instead of being darkened by
    /// lighting, for things that emit light.
    EmissiveOverLight = 2,
}
const CUSTOM_PASS_MAX_VEC4: usize = 4096;
const CUSTOM_PASS_HEADER_VEC4: usize = 3;
/// Raycast payload: origin xyz, direction xyz and max distance as little-endian float32.
const RAYCAST_PAYLOAD_BYTES: usize = 28;
pub fn status(code: i32) -> Result {
    if code == AE_GAMEPLAY_OK {
        Ok(())
    } else {
        let name = match code {
            AE_GAMEPLAY_INVALID_ARGUMENT => "invalid argument",
            AE_GAMEPLAY_UNSUPPORTED => "unsupported (missing component or system)",
            AE_GAMEPLAY_INVALID_HANDLE => "invalid handle",
            AE_GAMEPLAY_NOT_READY => "not ready (engine object not created yet)",
            AE_GAMEPLAY_NO_AUTHORITY => "no authority over the entity",
            _ => "failed",
        };
        Err(Error(code, format!("gameplay operation {name}: {code}")))
    }
}
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Entity {
    world: u64,
    id: u32,
}
impl Entity {
    pub const NONE: Self = Self { world: 0, id: 0 };
    /// # Safety
    /// The identity must originate from the active engine's checked ABI.
    pub unsafe fn from_raw(raw: AetherGameplayEntity) -> Self {
        Self {
            world: raw.world,
            id: raw.id,
        }
    }
    pub fn raw(self) -> AetherGameplayEntity {
        AetherGameplayEntity {
            world: self.world,
            id: self.id,
            reserved: 0,
        }
    }
}
#[derive(Debug, Clone, PartialEq)]
pub struct ComponentRef {
    pub entity: Entity,
}
impl Default for ComponentRef {
    fn default() -> Self {
        Self {
            entity: Entity::NONE,
        }
    }
}
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct EnumValue(pub i64);
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Float(f32),
    Int(i64),
    Bool(bool),
    Vector3(Vec3),
    String(String),
    Enum(i64),
    Entity(Entity),
    Component(Entity),
}
#[derive(Clone, Copy)]
pub struct Property {
    pub name: &'static str,
    pub kind: u32,
    pub enum_names: &'static str,
    pub component_type: &'static str,
}
pub trait Field: Sized {
    const KIND: u32;
    const ENUM_NAMES: &'static str = "";
    const COMPONENT_TYPE: &'static str = "";
    fn value(&self) -> Value;
    fn assign(&mut self, value: Value) -> Result;
}
macro_rules! field {
    ($t:ty, $kind:ident, $variant:ident) => {
        impl Field for $t {
            const KIND: u32 = $kind;
            fn value(&self) -> Value {
                Value::$variant(self.clone())
            }
            fn assign(&mut self, value: Value) -> Result {
                if let Value::$variant(v) = value {
                    *self = v;
                    Ok(())
                } else {
                    Err(Error(
                        AE_GAMEPLAY_INVALID_ARGUMENT,
                        "property type mismatch".into(),
                    ))
                }
            }
        }
    };
}
field!(f32, AE_GAMEPLAY_PROPERTY_FLOAT, Float);
field!(i64, AE_GAMEPLAY_PROPERTY_INT, Int);
field!(bool, AE_GAMEPLAY_PROPERTY_BOOL, Bool);
field!(Vec3, AE_GAMEPLAY_PROPERTY_VECTOR3, Vector3);
field!(String, AE_GAMEPLAY_PROPERTY_STRING, String);
field!(Entity, AE_GAMEPLAY_PROPERTY_ENTITY, Entity);
impl Field for EnumValue {
    const KIND: u32 = AE_GAMEPLAY_PROPERTY_ENUM;
    fn value(&self) -> Value {
        Value::Enum(self.0)
    }
    fn assign(&mut self, v: Value) -> Result {
        if let Value::Enum(v) = v {
            self.0 = v;
            Ok(())
        } else {
            Err(Error(1, "expected enum".into()))
        }
    }
}
impl Field for ComponentRef {
    const KIND: u32 = AE_GAMEPLAY_PROPERTY_COMPONENT;
    fn value(&self) -> Value {
        Value::Component(self.entity)
    }
    fn assign(&mut self, v: Value) -> Result {
        if let Value::Component(v) = v {
            self.entity = v;
            Ok(())
        } else {
            Err(Error(1, "expected component".into()))
        }
    }
}
pub trait Properties: Send + 'static {
    fn descriptors() -> &'static [Property]
    where
        Self: Sized;
    fn get_property(&self, index: u32) -> Result<Value>;
    fn set_property(&mut self, index: u32, value: Value) -> Result;
}
pub trait Script: Properties {
    fn attach(&mut self, _ctx: &mut Context<'_>) -> Result {
        Ok(())
    }
    fn update(&mut self, _ctx: &mut Context<'_>, _delta: f32) -> Result {
        Ok(())
    }
    fn detach(&mut self, _ctx: &mut Context<'_>) -> Result {
        Ok(())
    }
    fn event(&mut self, _ctx: &mut Context<'_>, _event: &Event) -> Result {
        Ok(())
    }
    /// Capture script-owned UTF-8 state before a streamed region is detached.
    /// Do not serialize runtime entity handles; they are invalid after re-entry.
    fn capture_streaming_state(&mut self, _ctx: &mut Context<'_>) -> Result<String> { Ok(String::new()) }
    /// Restore the state captured for this instance after its region is recreated.
    fn restore_streaming_state(&mut self, _ctx: &mut Context<'_>, _state: &str) -> Result { Ok(()) }
}
/// A ray hit from [`Context::raycast_3d`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RayHit {
    /// The entity that owns the body that was hit; `Entity::NONE` when the body has no entity.
    pub entity: Entity,
    /// Distance along the ray from its origin to the hit point.
    pub distance: f32,
    pub point: Vec3,
    pub normal: Vec3,
}
/// A skinned animation clip, by its name or its index.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ClipRef<'a> {
    Name(&'a str),
    Index(u32),
}
/// Which contact an [`Event`] of kind `AE_GAMEPLAY_EVENT_PHYSICS` reports.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PhysicsEventKind {
    CollisionEnter,
    CollisionExit,
    TriggerEnter,
    TriggerExit,
    /// The other entity is still overlapping this one.
    Overlapping,
}
/// Connection lifecycle reported by a network event.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NetworkEventKind {
    Connected,
    Disconnected,
}
#[derive(Debug)]
pub struct Event {
    pub kind: u32,
    pub other: Entity,
    pub code: u32,
    pub flags: u32,
    pub payload: String,
}
impl Event {
    /// The new owner's connection ID and whether it is this peer, for ownership events.
    pub fn ownership(&self) -> Option<(u32, bool)> {
        (self.kind == AE_GAMEPLAY_EVENT_OWNERSHIP).then_some((self.code, self.flags & 1 != 0))
    }
    /// Returns the transport lifecycle event and its peer connection ID.
    pub fn network_kind(&self) -> Option<(NetworkEventKind, u32)> {
        if self.kind != AE_GAMEPLAY_EVENT_NETWORK {
            return None;
        }
        let kind = match self.code {
            0 => NetworkEventKind::Connected,
            1 => NetworkEventKind::Disconnected,
            _ => return None,
        };
        Some((kind, self.flags))
    }
    /// The contact kind of a physics event; `None` for other event kinds or unknown codes.
    pub fn physics_kind(&self) -> Option<PhysicsEventKind> {
        if self.kind != AE_GAMEPLAY_EVENT_PHYSICS {
            return None;
        }
        Some(match self.code {
            0 => PhysicsEventKind::CollisionEnter,
            1 => PhysicsEventKind::CollisionExit,
            2 => PhysicsEventKind::TriggerEnter,
            3 => PhysicsEventKind::TriggerExit,
            4 => PhysicsEventKind::Overlapping,
            _ => return None,
        })
    }
    /// True for a physics event from the 3D (Jolt) world, false for the 2D (Box2D) world.
    pub fn is_3d(&self) -> bool {
        self.kind == AE_GAMEPLAY_EVENT_PHYSICS && self.flags & AE_GAMEPLAY_PHYSICS_FLAG_3D != 0
    }
}
#[macro_export]
macro_rules! gameplay_script {
    ($vis:vis struct $name:ident { $($fv:vis $field:ident : $ty:ty = $default:expr),* $(,)? }) => {
        $vis struct $name { $($fv $field: $ty),* }
        impl Default for $name { fn default() -> Self { Self { $($field: $default),* } } }
        impl $crate::Properties for $name {
            fn descriptors() -> &'static [$crate::Property] { &[$($crate::Property { name: stringify!($field), kind: <$ty as $crate::Field>::KIND, enum_names: <$ty as $crate::Field>::ENUM_NAMES, component_type: <$ty as $crate::Field>::COMPONENT_TYPE }),*] }
            fn get_property(&self, index: u32) -> $crate::Result<$crate::Value> { let fields: &[&dyn $crate::provider::ReadField] = &[$(&self.$field),*]; fields.get(index as usize).map(|f| f.read()).ok_or($crate::Error(1,"property index".into())) }
            fn set_property(&mut self, index: u32, value: $crate::Value) -> $crate::Result { let fields: &mut [&mut dyn $crate::provider::WriteField] = &mut [$(&mut self.$field),*]; fields.get_mut(index as usize).ok_or($crate::Error(1,"property index".into()))?.write(value) }
        }
    };
}
pub struct Context<'a> {
    engine: &'a AetherGameplayEngineApi,
    services: Option<&'a AetherGameplayServicesApi>,
    entity: Entity,
    _scoped: PhantomData<Rc<()>>,
}
impl<'a> Context<'a> {
    /// # Safety
    /// All callback pointers must remain valid for this scope and run on the owning world thread.
    pub unsafe fn from_raw(
        engine: &'a AetherGameplayEngineApi,
        services: Option<&'a AetherGameplayServicesApi>,
        entity: Entity,
    ) -> Self {
        Self {
            engine,
            services,
            entity,
            _scoped: PhantomData,
        }
    }
    pub fn entity(&self) -> Entity {
        self.entity
    }
    pub fn validate(&self, entity: Entity) -> Result {
        let mut valid = 0;
        let f = self
            .engine
            .entityValid
            .ok_or(Error(4, "entity validation unavailable".into()))?;
        status(unsafe { f(self.engine.context, entity.raw(), &mut valid) })?;
        if valid == 0 {
            Err(Error(5, "stale entity".into()))
        } else {
            Ok(())
        }
    }
    pub fn position(&self, entity: Entity) -> Result<Vec3> {
        self.validate(entity)?;
        let mut v = AetherGameplayVec3 {
            x: 0.,
            y: 0.,
            z: 0.,
        };
        status(unsafe {
            self.engine
                .getPosition
                .ok_or(Error(4, "transform unavailable".into()))?(
                self.engine.context,
                entity.raw(),
                &mut v,
            )
        })?;
        Ok(Vec3 {
            x: v.x,
            y: v.y,
            z: v.z,
        })
    }
    pub fn set_position(&mut self, entity: Entity, v: Vec3) -> Result {
        self.validate(entity)?;
        status(unsafe {
            self.engine
                .setPosition
                .ok_or(Error(4, "transform unavailable".into()))?(
                self.engine.context,
                entity.raw(),
                AetherGameplayVec3 {
                    x: v.x,
                    y: v.y,
                    z: v.z,
                },
            )
        })
    }
    pub fn log(&self, message: &str) -> Result {
        let d = AetherGameplayDiagnostic {
            structSize: size_of::<AetherGameplayDiagnostic>() as u32,
            severity: AE_GAMEPLAY_DIAGNOSTIC_INFO,
            code: provider::utf8("RUST_SCRIPT"),
            message: provider::utf8(message),
            file: provider::utf8(""),
            line: 0,
            column: 0,
        };
        status(unsafe {
            self.engine
                .reportDiagnostic
                .ok_or(Error(4, "logging unavailable".into()))?(self.engine.context, &d)
        })
    }
    fn service(
        &mut self,
        op: u32,
        entity: Entity,
        vector: [f32; 4],
        integer: i64,
        text: &str,
        text2: &str,
    ) -> Result<(Value, String)> {
        let (out, bytes) =
            self.invoke(op, entity, vector, integer, text, text2.as_bytes(), 65536)?;
        let text = String::from_utf8(bytes).map_err(|_| Error(1, "invalid UTF-8".into()))?;
        Ok((provider::decode_value(&out, &text)?, text))
    }
    /// Update a streaming source in world space.
    pub fn streaming_set_source(&mut self, id: &str, position: Vec3, load_radius: f32, prefetch_radius: f32, priority: u32) -> Result<bool> {
        let (value, _) = self.invoke(54, Entity::NONE, [position.x, position.y, position.z, load_radius], priority as i64, id, &prefetch_radius.to_le_bytes(), 0)?;
        Ok(value.integer != 0)
    }
    pub fn streaming_remove_source(&mut self, id: &str) -> Result<bool> {
        let (value, _) = self.service(55, Entity::NONE, [0.; 4], 0, id, "")?;
        if let Value::Bool(value) = value { Ok(value) } else { Err(Error(1, "expected bool".into())) }
    }
    pub fn streaming_request(&mut self, region: &str, priority: u32, activate: bool) -> Result<u64> {
        let (value, _) = self.service(56, Entity::NONE, [if activate { 1. } else { 0. }, 0., 0., 0.], priority as i64, region, "")?;
        if let Value::Int(value) = value { Ok(value as u64) } else { Err(Error(1, "expected request id".into())) }
    }
    pub fn streaming_prefetch(&mut self, region: &str, priority: u32) -> Result<u64> { self.streaming_request(region, priority, false) }
    pub fn streaming_load(&mut self, region: &str, priority: u32) -> Result<u64> { self.streaming_request(region, priority, true) }
    pub fn streaming_cancel(&mut self, request: u64) -> Result<bool> { self.streaming_bool_request(57, request) }
    pub fn streaming_release(&mut self, request: u64) -> Result<bool> { self.streaming_bool_request(58, request) }
    fn streaming_bool_request(&mut self, op: u32, request: u64) -> Result<bool> {
        let (value, _) = self.service(op, Entity::NONE, [0.; 4], request as i64, "", "")?;
        if let Value::Bool(value) = value { Ok(value) } else { Err(Error(1, "expected bool".into())) }
    }
    pub fn streaming_state(&mut self, request: u64) -> Result<u32> {
        let (value, _) = self.service(59, Entity::NONE, [0.; 4], request as i64, "", "")?;
        if let Value::Int(value) = value { Ok(value as u32) } else { Err(Error(1, "expected state".into())) }
    }
    pub fn streaming_diagnostics(&mut self) -> Result<String> {
        Ok(self.service(60, Entity::NONE, [0.; 4], 0, "", "")?.1)
    }
    pub fn key_down(&mut self, key: i64) -> Result<bool> {
        let (v, _) = self.service(1, Entity::NONE, [0.; 4], key, "", "")?;
        if let Value::Bool(v) = v {
            Ok(v)
        } else {
            Err(Error(1, "expected bool".into()))
        }
    }
    /// Whether a mouse button is held (0 left, 1 right, 2 middle).
    pub fn mouse_down(&mut self, button: i64) -> Result<bool> {
        let (v, _) = self.service(31, Entity::NONE, [0.; 4], button, "", "")?;
        if let Value::Bool(v) = v { Ok(v) } else { Err(Error(1, "expected bool".into())) }
    }
    /// Pointer motion since the last input frame, in pixels.
    pub fn mouse_delta(&mut self) -> Result<Vec3> {
        let (v, _) = self.service(32, Entity::NONE, [0.; 4], 0, "", "")?;
        if let Value::Vector3(v) = v { Ok(v) } else { Err(Error(1, "expected vector".into())) }
    }
    /// Scroll delta in wheel steps, positive y for scrolling up.
    pub fn scroll_delta(&mut self) -> Result<Vec3> {
        let (v, _) = self.service(33, Entity::NONE, [0.; 4], 0, "", "")?;
        if let Value::Vector3(v) = v { Ok(v) } else { Err(Error(1, "expected vector".into())) }
    }
    /// Set world-space Euler rotation in degrees.
    pub fn set_euler(&mut self, entity: Entity, euler: Vec3) -> Result {
        self.service(34, entity, [euler.x, euler.y, euler.z, 0.], 0, "", "")?;
        Ok(())
    }
    pub fn set_sprite_color(&mut self, e: Entity, color: [f32; 4]) -> Result {
        self.service(2, e, color, 0, "", "")?;
        Ok(())
    }
    pub fn velocity_2d(&mut self, e: Entity) -> Result<Vec3> {
        let (v, _) = self.service(3, e, [0.; 4], 0, "", "")?;
        if let Value::Vector3(v) = v {
            Ok(v)
        } else {
            Err(Error(1, "expected vector".into()))
        }
    }
    pub fn set_velocity_2d(&mut self, e: Entity, x: f32, y: f32) -> Result {
        self.service(4, e, [x, y, 0., 0.], 0, "", "")?;
        Ok(())
    }
    pub fn find_entity(&mut self, name: &str) -> Result<Entity> {
        self.entity_service(5, name)
    }
    pub fn load_scene(&mut self, path: &str) -> Result {
        self.service(6, Entity::NONE, [0.; 4], 0, path, "")?;
        Ok(())
    }
    pub fn instantiate_prefab(&mut self, path: &str) -> Result<Entity> {
        self.entity_service(7, path)
    }
    pub fn play_audio(&mut self, path: &str) -> Result {
        self.service(8, Entity::NONE, [0.; 4], 0, path, "")?;
        Ok(())
    }
    pub fn set_ui_text(&mut self, e: Entity, text: &str) -> Result {
        self.service(9, e, [0.; 4], 0, text, "")?;
        Ok(())
    }
    pub fn read_save(&mut self, key: &str) -> Result<String> {
        Ok(self.service(10, Entity::NONE, [0.; 4], 0, key, "")?.1)
    }
    pub fn write_save(&mut self, key: &str, value: &str) -> Result {
        self.service(11, Entity::NONE, [0.; 4], 0, key, value)?;
        Ok(())
    }
    pub fn create_entity(&mut self, name: &str) -> Result<Entity> {
        self.entity_service(12, name)
    }
    pub fn destroy_entity(&mut self, e: Entity) -> Result {
        self.service(13, e, [0.; 4], 0, "", "")?;
        Ok(())
    }
    /// Registers (or replaces) the project custom render pass `name`, drawn with the project
    /// shader `shader` at `stage`. `shader` is the bare project shader stem (for example
    /// `"ink_field"`, the path under `assets/shaders` without `.slang`). On the Web the shader
    /// must be portable, that is include `include/CustomPass.slangh`, so the cook can emit WGSL;
    /// registration fails with an error when the shader is missing or the profile does not
    /// support project shaders. Re-submit the pass every frame with [`Self::submit_custom_pass`];
    /// a registered pass that is not submitted in a frame does not draw.
    pub fn register_custom_pass(
        &mut self,
        name: &str,
        shader: &str,
        stage: CustomPassStage,
    ) -> Result {
        self.service(14, Entity::NONE, [0.; 4], stage as i64, name, shader)?;
        Ok(())
    }
    /// Removes the custom render pass `name`; unknown names are a successful no-op.
    pub fn unregister_custom_pass(&mut self, name: &str) -> Result {
        self.service(15, Entity::NONE, [0.; 4], 0, name, "")?;
        Ok(())
    }
    /// Submits the pass `name` for this frame: `params`, `color0`, `color1` and the `data`
    /// float4 records are bound to the pass shader, whose layout is the project's own. Call it
    /// every frame the pass should draw. At most 4096 records are used and extra elements are
    /// dropped, exactly like the C# `CustomPass.Submit`; submitting to an unregistered name is
    /// `Ok` and draws nothing. Values are not checked here: native shaders see them verbatim,
    /// while the Web renderer rejects non-finite params, colors or data and over-budget
    /// submissions with an explicit diagnostic.
    pub fn submit_custom_pass(
        &mut self,
        name: &str,
        data: &[[f32; 4]],
        params: [f32; 4],
        color0: [f32; 4],
        color1: [f32; 4],
    ) -> Result {
        let data = &data[..data.len().min(CUSTOM_PASS_MAX_VEC4)];
        let mut payload = Vec::with_capacity((CUSTOM_PASS_HEADER_VEC4 + data.len()) * 16);
        for vec4 in [&params, &color0, &color1].into_iter().chain(data) {
            for lane in vec4 {
                payload.extend_from_slice(&lane.to_le_bytes());
            }
        }
        self.service_bytes(16, name, &payload)
    }
    /// Like `service` for operations whose `text2` is a raw binary payload and that return no
    /// value, so no result buffer is allocated.
    fn service_bytes(&mut self, op: u32, text: &str, text2: &[u8]) -> Result {
        self.invoke(op, Entity::NONE, [0.; 4], 0, text, text2, 0)?;
        Ok(())
    }
    /// Calls service `op` and returns its raw value plus the exact bytes written to the result
    /// buffer (without the trailing NUL). `capacity` is the result buffer size; 0 for operations
    /// that return no string or bytes. A non-NONE `entity` is validated first.
    #[allow(clippy::too_many_arguments)]
    fn invoke(
        &mut self,
        op: u32,
        entity: Entity,
        vector: [f32; 4],
        integer: i64,
        text: &str,
        text2: &[u8],
        capacity: usize,
    ) -> Result<(AetherGameplayPropertyValue, Vec<u8>)> {
        let api = self
            .services
            .ok_or(Error(4, "services extension unavailable".into()))?;
        if op >= 64 || api.capabilities & (1u64 << op) == 0 {
            return Err(Error(4, format!("service {op} unsupported")));
        }
        if entity != Entity::NONE {
            self.validate(entity)?;
        }
        let req = AetherGameplayServiceRequest {
            structSize: size_of::<AetherGameplayServiceRequest>() as u32,
            operation: op,
            entity: entity.raw(),
            vector,
            integer,
            text: provider::utf8(text),
            text2: AetherGameplayUtf8 {
                data: text2.as_ptr().cast(),
                length: text2.len() as u32,
            },
        };
        let mut out = provider::empty_value();
        let mut bytes = vec![0u8; capacity];
        let mut required = 0;
        let invoke = api.invoke.ok_or(Error(4, "services invoke unavailable".into()))?;
        let mut code;
        loop {
            code = unsafe {
                invoke(api.context, &req, &mut out, AetherGameplayUtf8Buffer {
                    data: if bytes.is_empty() { std::ptr::null_mut() } else { bytes.as_mut_ptr().cast() },
                    capacity: bytes.len() as u32,
                    required: &mut required,
                })
            };
            // Diagnostics is read-only, so it alone can safely retry a growing result.
            if op != 60 || code != AE_GAMEPLAY_BUFFER_TOO_SMALL || required as usize <= bytes.len() { break; }
            bytes.resize(required as usize, 0);
        }
        status(code)?;
        if required as usize > bytes.len() {
            return Err(Error(3, "service result exceeds buffer".into()));
        }
        bytes.truncate(required.saturating_sub(1) as usize);
        Ok((out, bytes))
    }
    fn vector_service(&mut self, op: u32, e: Entity) -> Result<Vec3> {
        let (out, _) = self.invoke(op, e, [0.; 4], 0, "", &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Vector3(v) => Ok(v),
            _ => Err(Error(1, "expected vector".into())),
        }
    }
    fn vec3_write(&mut self, op: u32, e: Entity, v: Vec3) -> Result {
        self.invoke(op, e, [v.x, v.y, v.z, 0.], 0, "", &[], 0)?;
        Ok(())
    }
    /// The 3D linear velocity (m/s) of the entity's Jolt rigid body. Before the body is created
    /// this is the velocity last written with [`Self::set_velocity_3d`].
    pub fn velocity_3d(&mut self, e: Entity) -> Result<Vec3> {
        self.vector_service(17, e)
    }
    /// Sets the 3D linear velocity. Before the body is created it seeds the initial velocity.
    /// Non-finite values are an `INVALID_ARGUMENT` error.
    pub fn set_velocity_3d(&mut self, e: Entity, v: Vec3) -> Result {
        self.vec3_write(18, e, v)
    }
    /// Applies a force (N) for the next physics step. Errors with `NOT_READY` (10) while the
    /// body has not been created yet instead of dropping the force.
    pub fn add_force_3d(&mut self, e: Entity, force: Vec3) -> Result {
        self.vec3_write(19, e, force)
    }
    /// Applies an impulse (N*s) at the centre of mass. Errors with `NOT_READY` (10) while the
    /// body has not been created yet.
    pub fn add_impulse_3d(&mut self, e: Entity, impulse: Vec3) -> Result {
        self.vec3_write(20, e, impulse)
    }
    /// Casts a ray against the 3D physics world. `direction` need not be normalized but must be
    /// finite and non-zero; `max_distance` must be finite and positive. `Ok(None)` is a miss.
    pub fn raycast_3d(
        &mut self,
        origin: Vec3,
        direction: Vec3,
        max_distance: f32,
    ) -> Result<Option<RayHit>> {
        let mut payload = Vec::with_capacity(RAYCAST_PAYLOAD_BYTES);
        for lane in [
            origin.x,
            origin.y,
            origin.z,
            direction.x,
            direction.y,
            direction.z,
            max_distance,
        ] {
            payload.extend_from_slice(&lane.to_le_bytes());
        }
        let (out, normal) = self.invoke(21, Entity::NONE, [0.; 4], 0, "", &payload, 16)?;
        if out.integer == 0 {
            return Ok(None);
        }
        if normal.len() != 12 {
            return Err(Error(1, "malformed raycast result".into()));
        }
        let lane = |i: usize| f32::from_le_bytes(normal[i * 4..i * 4 + 4].try_into().unwrap());
        Ok(Some(RayHit {
            entity: unsafe { Entity::from_raw(out.entity) },
            distance: out.vector[3],
            point: Vec3 {
                x: out.vector[0],
                y: out.vector[1],
                z: out.vector[2],
            },
            normal: Vec3 {
                x: lane(0),
                y: lane(1),
                z: lane(2),
            },
        }))
    }
    /// Sets the desired horizontal velocity (`direction * speed`) of the entity's character
    /// controller; it persists until the next call. Vertical motion stays with the controller.
    pub fn character_move(&mut self, e: Entity, direction: Vec3, speed: f32) -> Result {
        self.invoke(
            22,
            e,
            [direction.x, direction.y, direction.z, speed],
            0,
            "",
            &[],
            0,
        )?;
        Ok(())
    }
    /// Queues a one-shot jump at `speed` (m/s, not negative), applied when the character is
    /// grounded.
    pub fn character_jump(&mut self, e: Entity, speed: f32) -> Result {
        self.invoke(23, e, [speed, 0., 0., 0.], 0, "", &[], 0)?;
        Ok(())
    }
    pub fn character_is_grounded(&mut self, e: Entity) -> Result<bool> {
        let (out, _) = self.invoke(24, e, [0.; 4], 0, "", &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Bool(v) => Ok(v),
            _ => Err(Error(1, "expected bool".into())),
        }
    }
    /// Builds a physics ragdoll on `e` from the skeleton at the project/VFS path `skeleton`, and
    /// returns the ragdoll root (the entity `e`, which keeps its identity and becomes the pelvis).
    pub fn spawn_ragdoll(&mut self, e: Entity, skeleton: &str) -> Result<Entity> {
        let (out, _) = self.invoke(25, e, [0.; 4], 0, skeleton, &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Entity(root) => Ok(root),
            _ => Err(Error(1, "expected entity".into())),
        }
    }
    /// Plays a skinned-mesh animation clip, blending from the playing one over `fade_seconds`
    /// (0 switches outright). Errors with `INVALID_ARGUMENT` for an unknown clip.
    pub fn set_animation_clip(
        &mut self,
        e: Entity,
        clip: ClipRef<'_>,
        fade_seconds: f32,
    ) -> Result {
        let (index, name) = match clip {
            ClipRef::Index(i) => (i64::from(i), ""),
            ClipRef::Name(n) => (0, n),
        };
        if name.is_empty() && matches!(clip, ClipRef::Name(_)) {
            return Err(Error(1, "empty animation clip name".into()));
        }
        self.invoke(26, e, [fade_seconds, 0., 0., 0.], index, name, &[], 0)?;
        Ok(())
    }
    /// Sets the animation playback speed multiplier.
    pub fn set_animation_speed(&mut self, e: Entity, speed: f32) -> Result {
        self.invoke(27, e, [speed, 0., 0., 0.], 0, "", &[], 0)?;
        Ok(())
    }
    /// Seconds into the playing animation clip.
    pub fn animation_time(&mut self, e: Entity) -> Result<f32> {
        let (out, _) = self.invoke(28, e, [0.; 4], 0, "", &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Float(v) => Ok(v),
            _ => Err(Error(1, "expected float".into())),
        }
    }
    /// Index of the animation clip called `name`; `INVALID_ARGUMENT` when there is none.
    pub fn find_animation_clip(&mut self, e: Entity, name: &str) -> Result<u32> {
        let (out, _) = self.invoke(29, e, [0.; 4], 0, name, &[], 0)?;
        u32::try_from(out.integer).map_err(|_| Error(1, "clip index out of range".into()))
    }
    /// Index of the animation clip that is playing.
    pub fn current_animation_clip(&mut self, e: Entity) -> Result<u32> {
        let (out, _) = self.invoke(30, e, [0.; 4], 0, "", &[], 0)?;
        u32::try_from(out.integer).map_err(|_| Error(1, "clip index out of range".into()))
    }
    fn net_bool(&mut self, op: u32, entity: Entity, vector: [f32; 4], integer: i64, text: &str) -> Result<bool> {
        let (out, _) = self.invoke(op, entity, vector, integer, text, &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Bool(value) => Ok(value),
            _ => Err(Error(1, "expected bool".into())),
        }
    }
    /// Start a host. The connection limit counts remote clients, not the host.
    pub fn net_host(&mut self, port: u16, max_connections: u16) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_HOST, Entity::NONE, [f32::from(max_connections), 0., 0., 0.], i64::from(port), "")
    }
    pub fn net_connect(&mut self, address: &str, port: u16) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_CONNECT, Entity::NONE, [0.; 4], i64::from(port), address)
    }
    pub fn net_disconnect(&mut self) -> Result {
        self.invoke(AE_GAMEPLAY_SERVICE_NET_DISCONNECT, Entity::NONE, [0.; 4], 0, "", &[], 0)?;
        Ok(())
    }
    pub fn net_is_host(&mut self) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_IS_HOST, Entity::NONE, [0.; 4], 0, "")
    }
    pub fn net_is_client(&mut self) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_IS_CLIENT, Entity::NONE, [0.; 4], 0, "")
    }
    pub fn net_is_connected(&mut self) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_IS_CONNECTED, Entity::NONE, [0.; 4], 0, "")
    }
    pub fn net_local_connection_id(&mut self) -> Result<u32> {
        let (out, _) = self.invoke(AE_GAMEPLAY_SERVICE_NET_LOCAL_ID, Entity::NONE, [0.; 4], 0, "", &[], 0)?;
        u32::try_from(out.integer).map_err(|_| Error(1, "connection id out of range".into()))
    }
    pub fn net_spawn(&mut self, prefab: &str, position: Vec3, owner: u32) -> Result<Entity> {
        let (out, _) = self.invoke(AE_GAMEPLAY_SERVICE_NET_SPAWN, Entity::NONE, [position.x, position.y, position.z, 0.], i64::from(owner), prefab, &[], 0)?;
        match provider::decode_value(&out, "")? {
            Value::Entity(entity) => Ok(entity),
            _ => Err(Error(1, "expected entity".into())),
        }
    }
    pub fn net_despawn(&mut self, entity: Entity) -> Result {
        self.invoke(AE_GAMEPLAY_SERVICE_NET_DESPAWN, entity, [0.; 4], 0, "", &[], 0)?;
        Ok(())
    }
    pub fn net_has_authority(&mut self, entity: Entity) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_HAS_AUTHORITY, entity, [0.; 4], 0, "")
    }
    pub fn net_is_owner(&mut self, entity: Entity) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_IS_OWNER, entity, [0.; 4], 0, "")
    }
    /// A true result on a client means the request was sent, not yet approved.
    pub fn net_request_ownership(&mut self, entity: Entity) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_REQUEST_OWNERSHIP, entity, [0.; 4], 0, "")
    }
    /// A true result on a client means the request was sent, not yet approved.
    pub fn net_release_ownership(&mut self, entity: Entity) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_RELEASE_OWNERSHIP, entity, [0.; 4], 0, "")
    }
    pub fn net_set_replication_ready(&mut self, ready: bool) -> Result {
        self.invoke(AE_GAMEPLAY_SERVICE_NET_SET_REPLICATION_READY, Entity::NONE, [0.; 4], i64::from(ready), "", &[], 0)?;
        Ok(())
    }
    pub fn net_is_replication_ready(&mut self) -> Result<bool> {
        self.net_bool(AE_GAMEPLAY_SERVICE_NET_IS_REPLICATION_READY, Entity::NONE, [0.; 4], 0, "")
    }
    pub fn net_disconnect_reason(&mut self) -> Result<String> {
        let (_, text) = self.service(AE_GAMEPLAY_SERVICE_NET_DISCONNECT_REASON, Entity::NONE, [0.; 4], 0, "", "")?;
        Ok(text)
    }
    fn entity_service(&mut self, op: u32, text: &str) -> Result<Entity> {
        let (v, _) = self.service(op, Entity::NONE, [0.; 4], 0, text, "")?;
        if let Value::Entity(e) = v {
            Ok(e)
        } else {
            Err(Error(1, "expected entity".into()))
        }
    }
}

/// Declares a stable integer-backed editable enum with generated label metadata.
#[macro_export]
macro_rules! gameplay_enum {
    ($vis:vis enum $name:ident { $first:ident = $first_value:expr $(, $variant:ident = $value:expr)* $(,)? }) => {
        #[derive(Debug, Clone, Copy, PartialEq)]
        #[repr(i64)]
        $vis enum $name { $first = $first_value $(, $variant = $value)* }
        impl $crate::Field for $name {
            const KIND:u32=$crate::abi::AE_GAMEPLAY_PROPERTY_ENUM;
            const ENUM_NAMES:&'static str=concat!(stringify!($first) $(, ",", stringify!($variant))*);
            fn value(&self)->$crate::Value{$crate::Value::Enum(*self as i64)}
            fn assign(&mut self,v:$crate::Value)->$crate::Result{if let $crate::Value::Enum(v)=v { if v==$first_value{*self=Self::$first;return Ok(())} $(if v==$value{*self=Self::$variant;return Ok(())})* }Err($crate::Error(1,"unknown enum value".into()))}
        }
    };
}

/// Defines an editable component reference while preserving its stable component type.
#[macro_export]
macro_rules! gameplay_component_ref {
    ($vis:vis struct $name:ident = $component:literal;) => {
        #[derive(Debug,Clone,Copy,Default,PartialEq)]
        $vis struct $name { pub entity:$crate::Entity }
        impl $crate::Field for $name {
            const KIND:u32=$crate::abi::AE_GAMEPLAY_PROPERTY_COMPONENT;
            const COMPONENT_TYPE:&'static str=$component;
            fn value(&self)->$crate::Value{$crate::Value::Component(self.entity)}
            fn assign(&mut self,v:$crate::Value)->$crate::Result{if let $crate::Value::Component(v)=v{self.entity=v;Ok(())}else{Err($crate::Error(1,"expected component reference".into()))}}
        }
    };
}
