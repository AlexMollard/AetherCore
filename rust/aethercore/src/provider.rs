use super::*;
use std::{
    collections::BTreeMap,
    panic::{AssertUnwindSafe, catch_unwind},
};
pub trait ReadField {
    fn read(&self) -> Value;
}
pub trait WriteField {
    fn write(&mut self, value: Value) -> Result;
}
impl<T: Field> ReadField for T {
    fn read(&self) -> Value {
        self.value()
    }
}
impl<T: Field> WriteField for T {
    fn write(&mut self, value: Value) -> Result {
        self.assign(value)
    }
}
pub fn utf8(text: &str) -> AetherGameplayUtf8 {
    AetherGameplayUtf8 {
        data: text.as_ptr().cast(),
        length: text.len() as u32,
    }
}
pub fn empty_value() -> AetherGameplayPropertyValue {
    AetherGameplayPropertyValue {
        structSize: size_of::<AetherGameplayPropertyValue>() as u32,
        r#type: 0,
        vector: [0.; 4],
        integer: 0,
        entity: Entity::NONE.raw(),
    }
}
/// # Safety
/// The non-null view must point to length readable bytes for this call.
pub unsafe fn read_utf8<'a>(text: AetherGameplayUtf8) -> Result<&'a str> {
    if text.length == 0 {
        return Ok("");
    }
    if text.data.is_null() {
        return Err(Error(1, "null UTF-8 data".into()));
    }
    std::str::from_utf8(unsafe {
        std::slice::from_raw_parts(text.data.cast(), text.length as usize)
    })
    .map_err(|_| Error(1, "invalid UTF-8".into()))
}
pub fn decode_value(v: &AetherGameplayPropertyValue, text: &str) -> Result<Value> {
    if v.structSize < size_of::<AetherGameplayPropertyValue>() as u32 {
        return Err(Error(2, "short property value".into()));
    }
    Ok(match v.r#type {
        1 => Value::Float(v.vector[0]),
        2 => Value::Int(v.integer),
        3 => Value::Bool(v.integer != 0),
        4 => Value::Vector3(Vec3 {
            x: v.vector[0],
            y: v.vector[1],
            z: v.vector[2],
        }),
        5 => Value::String(text.into()),
        6 => Value::Enum(v.integer),
        7 => Value::Entity(unsafe { Entity::from_raw(v.entity) }),
        8 => Value::Component(unsafe { Entity::from_raw(v.entity) }),
        0 => Value::Int(0),
        _ => return Err(Error(1, "unknown property kind".into())),
    })
}
unsafe fn write_value(
    v: Value,
    out: *mut AetherGameplayPropertyValue,
    buffer: AetherGameplayUtf8Buffer,
) -> Result {
    if out.is_null() {
        return Err(Error(1, "null property output".into()));
    }
    let mut raw = empty_value();
    match v {
        Value::Float(v) => {
            raw.r#type = 1;
            raw.vector[0] = v
        }
        Value::Int(v) => {
            raw.r#type = 2;
            raw.integer = v
        }
        Value::Bool(v) => {
            raw.r#type = 3;
            raw.integer = i64::from(v)
        }
        Value::Vector3(v) => {
            raw.r#type = 4;
            raw.vector = [v.x, v.y, v.z, 0.]
        }
        Value::String(v) => {
            raw.r#type = 5;
            if buffer.required.is_null() {
                return Err(Error(1, "null required output".into()));
            }
            unsafe {
                *buffer.required = v.len() as u32 + 1;
            }
            if buffer.capacity <= v.len() as u32 || buffer.data.is_null() {
                if !buffer.data.is_null() && buffer.capacity > 0 {
                    unsafe { *buffer.data = 0 }
                }
                return Err(Error(3, "string buffer too small".into()));
            }
            unsafe {
                std::ptr::copy_nonoverlapping(v.as_ptr(), buffer.data.cast(), v.len());
                *buffer.data.add(v.len()) = 0;
            }
        }
        Value::Enum(v) => {
            raw.r#type = 6;
            raw.integer = v
        }
        Value::Entity(v) => {
            raw.r#type = 7;
            raw.entity = v.raw()
        }
        Value::Component(v) => {
            raw.r#type = 8;
            raw.entity = v.raw()
        }
    }
    unsafe { *out = raw };
    Ok(())
}
pub struct Registration {
    pub name: &'static str,
    pub properties: &'static [Property],
    pub create: fn() -> Box<dyn Script>,
}
impl Registration {
    pub fn of<T: Script + Default>(name: &'static str) -> Self {
        Self {
            name,
            properties: T::descriptors(),
            create: || Box::<T>::default(),
        }
    }
}
struct Instance {
    script: Box<dyn Script>,
    entity: Entity,
    failed: bool,
    attached: bool,
}
pub struct Runtime {
    registrations: Vec<Registration>,
    engine: Option<AetherGameplayEngineApi>,
    services: Option<AetherGameplayServicesApi>,
    instances: BTreeMap<u64, Instance>,
    next: u64,
    owner: std::thread::ThreadId,
}
impl Runtime {
    pub fn new(registrations: Vec<Registration>) -> Self {
        Self {
            registrations,
            engine: None,
            services: None,
            instances: BTreeMap::new(),
            next: 1,
            owner: std::thread::current().id(),
        }
    }
    pub fn check_thread(&self) -> Result {
        if self.owner != std::thread::current().id() {
            return Err(Error(7, "module called on wrong thread".into()));
        }
        Ok(())
    }
    fn initialized(&self) -> Result {
        if self.engine.is_some() {
            Ok(())
        } else {
            Err(Error(9, "module not initialized".into()))
        }
    }
    fn registration(&self, index: u32) -> Result<&Registration> {
        self.registrations
            .get(index as usize)
            .ok_or(Error(1, "type index".into()))
    }
    pub unsafe fn initialize(&mut self, engine: *const AetherGameplayEngineApi) -> Result {
        if engine.is_null() {
            return Err(Error(1, "null engine".into()));
        }
        if unsafe { (*engine).structSize } < size_of::<AetherGameplayEngineApi>() as u32 {
            return Err(Error(2, "short engine API".into()));
        }
        let engine = unsafe { *engine };
        if engine.abiMajor != 1 || engine.structSize < size_of::<AetherGameplayEngineApi>() as u32 {
            return Err(Error(2, "engine ABI mismatch".into()));
        }
        if engine.entityValid.is_none()
            || engine.getPosition.is_none()
            || engine.setPosition.is_none()
            || engine.reportDiagnostic.is_none()
        {
            return Err(Error(1, "missing engine callback".into()));
        }
        if self.engine.is_some() {
            return Err(Error(8, "already initialized".into()));
        }
        self.engine = Some(engine);
        Ok(())
    }
    pub unsafe fn bind_services(
        &mut self,
        major: u32,
        size: u32,
        api: *const AetherGameplayServicesApi,
    ) -> Result {
        if major != 1 || size < size_of::<AetherGameplayServicesApi>() as u32 {
            return Err(Error(2, "services ABI mismatch".into()));
        }
        if api.is_null() {
            return Err(Error(1, "null services".into()));
        }
        if unsafe { (*api).structSize } < size_of::<AetherGameplayServicesApi>() as u32 {
            return Err(Error(2, "short services API".into()));
        }
        let api = unsafe { *api };
        if api.abiMajor != 1 || api.structSize < size_of::<AetherGameplayServicesApi>() as u32 {
            return Err(Error(2, "services ABI mismatch".into()));
        }
        if api.invoke.is_none() {
            return Err(Error(1, "missing service callback".into()));
        }
        self.services = Some(api);
        Ok(())
    }
    pub fn shutdown(&mut self) -> Result {
        // Drop user objects inside individual panic boundaries so one failed destructor
        // cannot prevent cleanup of the remaining instances.
        let instances = std::mem::take(&mut self.instances);
        let mut failed = false;
        for (_, instance) in instances {
            failed |= catch_unwind(AssertUnwindSafe(|| drop(instance))).is_err();
        }
        self.engine = None;
        self.services = None;
        if failed {
            Err(Error(9, "panic during shutdown".into()))
        } else {
            Ok(())
        }
    }
    pub unsafe fn type_count(&mut self, out: *mut u32) -> Result {
        if out.is_null() {
            return Err(Error(1, "null count".into()));
        }
        unsafe { *out = self.registrations.len() as u32 };
        Ok(())
    }
    pub unsafe fn type_info(&mut self, index: u32, out: *mut AetherGameplayTypeInfo) -> Result {
        if out.is_null() {
            return Err(Error(1, "null type info".into()));
        }
        let r = self.registration(index)?;
        unsafe {
            *out = AetherGameplayTypeInfo {
                structSize: size_of::<AetherGameplayTypeInfo>() as u32,
                propertyCount: r.properties.len() as u32,
                name: utf8(r.name),
            }
        };
        Ok(())
    }
    pub unsafe fn property_info(
        &mut self,
        index: u32,
        property: u32,
        out: *mut AetherGameplayPropertyInfo,
    ) -> Result {
        if out.is_null() {
            return Err(Error(1, "null property info".into()));
        }
        let p = self
            .registration(index)?
            .properties
            .get(property as usize)
            .ok_or(Error(1, "property index".into()))?;
        unsafe {
            *out = AetherGameplayPropertyInfo {
                structSize: size_of::<AetherGameplayPropertyInfo>() as u32,
                r#type: p.kind,
                name: utf8(p.name),
                enumNames: utf8(p.enum_names),
                componentType: utf8(p.component_type),
            }
        };
        Ok(())
    }
    pub unsafe fn create(
        &mut self,
        index: u32,
        entity: AetherGameplayEntity,
        out: *mut u64,
    ) -> Result {
        self.initialized()?;
        if out.is_null() {
            return Err(Error(1, "null instance output".into()));
        }
        let entity = unsafe { Entity::from_raw(entity) };
        let engine = self.engine.as_ref().unwrap();
        let ctx = unsafe { Context::from_raw(engine, self.services.as_ref(), entity) };
        ctx.validate(entity)?;
        let script = (self.registration(index)?.create)();
        let id = self.next;
        self.next = self
            .next
            .checked_add(1)
            .ok_or(Error(9, "instance handles exhausted".into()))?;
        self.instances.insert(
            id,
            Instance {
                script,
                entity,
                failed: false,
                attached: false,
            },
        );
        unsafe { *out = id };
        Ok(())
    }
    pub fn destroy(&mut self, id: u64) -> Result {
        let instance = self
            .instances
            .remove(&id)
            .ok_or(Error(5, "instance handle".into()))?;
        drop(instance);
        Ok(())
    }
    fn invoke(&mut self, id: u64, operation: u32, delta: f32, event: Option<Event>) -> Result {
        self.initialized()?;
        let i = self
            .instances
            .get_mut(&id)
            .ok_or(Error(5, "instance handle".into()))?;
        if i.failed {
            return Err(Error(9, "instance disabled after script failure".into()));
        }
        if operation == 0 && i.attached {
            return Err(Error(8, "instance already attached".into()));
        }
        if operation != 0 && !i.attached {
            return Err(Error(9, "instance is not attached".into()));
        }
        let engine = self.engine.as_ref().unwrap();
        let mut ctx = unsafe { Context::from_raw(engine, self.services.as_ref(), i.entity) };
        // Detach may run after its owner was destroyed. Cleanup and logging remain
        // available; every Context ECS operation still validates its own handle.
        if operation != 2 {
            ctx.validate(i.entity)?;
        }
        let result = catch_unwind(AssertUnwindSafe(|| match operation {
            0 => i.script.attach(&mut ctx),
            1 => i.script.update(&mut ctx, delta),
            2 => i.script.detach(&mut ctx),
            _ => i.script.event(&mut ctx, event.as_ref().unwrap()),
        }));
        let result =
            result.unwrap_or_else(|_| Err(Error(9, "Rust script panic (unwind contained)".into())));
        if let Err(ref e) = result {
            if e.0 != AE_GAMEPLAY_UNSUPPORTED {
                i.failed = true;
                let _ = ctx.log(&e.1);
            }
        }
        if result.is_ok() {
            if operation == 0 {
                i.attached = true
            } else if operation == 2 {
                i.attached = false
            }
        }
        result
    }
    pub fn attach(&mut self, id: u64) -> Result {
        self.invoke(id, 0, 0., None)
    }
    pub fn update(&mut self, id: u64, delta: f32) -> Result {
        if !delta.is_finite() || delta < 0. {
            return Err(Error(1, "invalid delta time".into()));
        }
        self.invoke(id, 1, delta, None)
    }
    pub fn detach(&mut self, id: u64) -> Result {
        self.invoke(id, 2, 0., None)
    }
    pub unsafe fn event(&mut self, id: u64, event: *const AetherGameplayEvent) -> Result {
        if event.is_null() {
            return Err(Error(1, "null event".into()));
        }
        if unsafe { (*event).structSize } < size_of::<AetherGameplayEvent>() as u32 {
            return Err(Error(2, "short event".into()));
        }
        let event = unsafe { *event };
        if event.structSize < size_of::<AetherGameplayEvent>() as u32 {
            return Err(Error(2, "short event".into()));
        }
        if event.kind != AE_GAMEPLAY_EVENT_PHYSICS {
            return Err(Error(4, "event unsupported".into()));
        }
        self.invoke(
            id,
            3,
            0.,
            Some(Event {
                kind: event.kind,
                other: unsafe { Entity::from_raw(event.other) },
                code: event.code,
                flags: event.flags,
                payload: unsafe { read_utf8(event.payload)? }.into(),
            }),
        )
    }
    pub unsafe fn get_property(
        &mut self,
        id: u64,
        index: u32,
        out: *mut AetherGameplayPropertyValue,
        buffer: AetherGameplayUtf8Buffer,
    ) -> Result {
        let i = self
            .instances
            .get_mut(&id)
            .ok_or(Error(5, "instance handle".into()))?;
        if i.failed {
            return Err(Error(9, "instance disabled".into()));
        }
        let result = catch_unwind(AssertUnwindSafe(|| i.script.get_property(index)));
        match result {
            Ok(Ok(v)) => unsafe { write_value(v, out, buffer) },
            Ok(Err(e)) => {
                if e.0 == AE_GAMEPLAY_SCRIPT_ERROR {
                    i.failed = true;
                }
                Err(e)
            }
            Err(_) => {
                i.failed = true;
                Err(Error(9, "property getter panicked".into()))
            }
        }
    }
    pub unsafe fn set_property(
        &mut self,
        id: u64,
        index: u32,
        value: *const AetherGameplayPropertyValue,
        text: AetherGameplayUtf8,
    ) -> Result {
        if value.is_null() {
            return Err(Error(1, "null value".into()));
        }
        if unsafe { (*value).structSize } < size_of::<AetherGameplayPropertyValue>() as u32 {
            return Err(Error(2, "short property".into()));
        }
        let value = decode_value(unsafe { &*value }, unsafe { read_utf8(text)? })?;
        let i = self
            .instances
            .get_mut(&id)
            .ok_or(Error(5, "instance handle".into()))?;
        if i.failed {
            return Err(Error(9, "instance disabled".into()));
        }
        match catch_unwind(AssertUnwindSafe(|| i.script.set_property(index, value))) {
            Ok(Err(e)) if e.0 == AE_GAMEPLAY_SCRIPT_ERROR => {
                i.failed = true;
                Err(e)
            }
            Ok(v) => v,
            Err(_) => {
                i.failed = true;
                Err(Error(9, "property setter panicked".into()))
            }
        }
    }
    pub unsafe fn default_property(
        &mut self,
        index: u32,
        property: u32,
        out: *mut AetherGameplayPropertyValue,
        buffer: AetherGameplayUtf8Buffer,
    ) -> Result {
        let script = (self.registration(index)?.create)();
        unsafe { write_value(script.get_property(property)?, out, buffer) }
    }
}
pub fn boundary(f: impl FnOnce() -> Result) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => e.0,
        Err(_) => AE_GAMEPLAY_SCRIPT_ERROR,
    }
}
#[macro_export]
macro_rules! export_gameplay {
    ($($script:ident),+ $(,)?) => {
        mod __aether_gameplay_export {
            use $crate::{abi::*,abi::services::*,provider::*,Error};
            static RUNTIME: std::sync::OnceLock<std::sync::Mutex<Runtime>> = std::sync::OnceLock::new();
            fn call(context:u64,f:impl FnOnce(&mut Runtime)->$crate::Result)->i32{boundary(||{if context!=1{return Err(Error(5,"module context".into()))}let runtime=RUNTIME.get_or_init(||std::sync::Mutex::new(Runtime::new(vec![$(Registration::of::<super::$script>(stringify!($script))),+]))); let mut r=runtime.try_lock().map_err(|_|Error(8,"reentrant module call".into()))?; r.check_thread()?; let code=boundary(||f(&mut r)); $crate::status(code)})}
            unsafe extern "C" fn initialize(c:u64,e:*const AetherGameplayEngineApi)->i32{call(c,|r|unsafe{r.initialize(e)})}
            unsafe extern "C" fn shutdown(c:u64)->i32{call(c,|r|r.shutdown())}
            unsafe extern "C" fn count(c:u64,o:*mut u32)->i32{call(c,|r|unsafe{r.type_count(o)})}
            unsafe extern "C" fn info(c:u64,t:u32,o:*mut AetherGameplayTypeInfo)->i32{call(c,|r|unsafe{r.type_info(t,o)})}
            unsafe extern "C" fn prop_info(c:u64,t:u32,p:u32,o:*mut AetherGameplayPropertyInfo)->i32{call(c,|r|unsafe{r.property_info(t,p,o)})}
            unsafe extern "C" fn create(c:u64,t:u32,e:AetherGameplayEntity,o:*mut u64)->i32{call(c,|r|unsafe{r.create(t,e,o)})}
            unsafe extern "C" fn destroy(c:u64,i:u64)->i32{call(c,|r|r.destroy(i))}
            unsafe extern "C" fn attach(c:u64,i:u64)->i32{call(c,|r|r.attach(i))}
            unsafe extern "C" fn update(c:u64,i:u64,d:f32)->i32{call(c,|r|r.update(i,d))}
            unsafe extern "C" fn detach(c:u64,i:u64)->i32{call(c,|r|r.detach(i))}
            unsafe extern "C" fn get(c:u64,i:u64,p:u32,o:*mut AetherGameplayPropertyValue,b:AetherGameplayUtf8Buffer)->i32{call(c,|r|unsafe{r.get_property(i,p,o,b)})}
            unsafe extern "C" fn set(c:u64,i:u64,p:u32,v:*const AetherGameplayPropertyValue,s:AetherGameplayUtf8)->i32{call(c,|r|unsafe{r.set_property(i,p,v,s)})}
            unsafe extern "C" fn defaults(c:u64,t:u32,p:u32,o:*mut AetherGameplayPropertyValue,b:AetherGameplayUtf8Buffer)->i32{call(c,|r|unsafe{r.default_property(t,p,o,b)})}
            unsafe extern "C" fn event(c:u64,i:u64,e:*const AetherGameplayEvent)->i32{call(c,|r|unsafe{r.event(i,e)})}
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn aether_gameplay_bind_services(major:u32,size:u32,api:*const AetherGameplayServicesApi)->i32{call(1,|r|unsafe{r.bind_services(major,size,api)})}
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn aether_gameplay_query(major:u32,size:u32,out:*mut AetherGameplayModuleApi)->i32{boundary(||{
                if out.is_null(){return Err(Error(1,"null module output".into()))}
                if major!=1 || size<std::mem::size_of::<AetherGameplayModuleApi>() as u32{return Err(Error(2,"module ABI mismatch".into()))}
                unsafe{*out=AetherGameplayModuleApi{structSize:std::mem::size_of::<AetherGameplayModuleApi>() as u32,abiMajor:1,capabilities:AE_GAMEPLAY_CAP_PHYSICS,context:1,initialize:Some(initialize),shutdown:Some(shutdown),getTypeCount:Some(count),getTypeInfo:Some(info),getPropertyInfo:Some(prop_info),create:Some(create),destroy:Some(destroy),attach:Some(attach),update:Some(update),detach:Some(detach),getProperty:Some(get),setProperty:Some(set),getDefaultProperty:Some(defaults),dispatchEvent:Some(event)}};Ok(())
            })}
        }
    };
}
