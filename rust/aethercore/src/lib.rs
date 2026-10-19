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
pub fn status(code: i32) -> Result {
    if code == AE_GAMEPLAY_OK {
        Ok(())
    } else {
        Err(Error(code, format!("gameplay operation failed: {code}")))
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
        Err(Error(AE_GAMEPLAY_UNSUPPORTED, "event unsupported".into()))
    }
}
#[derive(Debug)]
pub struct Event {
    pub kind: u32,
    pub other: Entity,
    pub code: u32,
    pub flags: u32,
    pub payload: String,
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
        let api = self
            .services
            .ok_or(Error(4, "services extension unavailable".into()))?;
        if api.capabilities & (1u64 << op) == 0 {
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
            text2: provider::utf8(text2),
        };
        let mut out = provider::empty_value();
        let mut bytes = vec![0u8; 65536];
        let mut required = 0;
        let code = unsafe {
            api.invoke
                .ok_or(Error(4, "services invoke unavailable".into()))?(
                api.context,
                &req,
                &mut out,
                AetherGameplayUtf8Buffer {
                    data: bytes.as_mut_ptr().cast(),
                    capacity: bytes.len() as u32,
                    required: &mut required,
                },
            )
        };
        status(code)?;
        if required as usize > bytes.len() {
            return Err(Error(3, "service result exceeds buffer".into()));
        }
        let len = required.saturating_sub(1) as usize;
        let text = String::from_utf8(bytes[..len].to_vec())
            .map_err(|_| Error(1, "invalid UTF-8".into()))?;
        Ok((provider::decode_value(&out, &text)?, text))
    }
    pub fn key_down(&mut self, key: i64) -> Result<bool> {
        let (v, _) = self.service(1, Entity::NONE, [0.; 4], key, "", "")?;
        if let Value::Bool(v) = v {
            Ok(v)
        } else {
            Err(Error(1, "expected bool".into()))
        }
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
