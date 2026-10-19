use aethercore::*;
#[test]
fn generated_fields_round_trip_and_reject_wrong_types() {
    gameplay_script! { pub struct Sample { pub speed: f32 = 2.5, pub label: String = String::from("hé\0llo"), pub target: Entity = Entity::NONE, pub point: Vec3 = Vec3::default(), pub count: i64 = 4, pub enabled: bool = true } }
    impl Script for Sample {}
    let mut sample = Sample::default();
    assert_eq!(Sample::descriptors()[0].name, "speed");
    assert_eq!(
        sample.get_property(1).unwrap(),
        Value::String("hé\0llo".into())
    );
    assert!(sample.set_property(0, Value::Bool(false)).is_err());
    sample.set_property(0, Value::Float(8.0)).unwrap();
    assert_eq!(sample.get_property(0).unwrap(), Value::Float(8.0));
}

#[test]
fn exported_provider_contains_panics_disables_instance_and_cleans_up() {
    unsafe {
        let mut api = std::mem::MaybeUninit::<abi::AetherGameplayModuleApi>::zeroed().assume_init();
        assert_eq!(
            __aether_gameplay_export::aether_gameplay_query(2, size_of_val(&api) as u32, &mut api),
            2
        );
        assert!(api.create.is_none());
        assert_eq!(
            __aether_gameplay_export::aether_gameplay_query(1, size_of_val(&api) as u32, &mut api),
            0
        );
        let engine = engine();
        assert_eq!(api.initialize.unwrap()(1, &engine), 0);
        let count_fn = api.getTypeCount.unwrap();
        assert_eq!(
            std::thread::spawn(move || {
                let mut count = 0;
                count_fn(1, &mut count)
            })
            .join()
            .unwrap(),
            abi::AE_GAMEPLAY_WRONG_THREAD
        );
        let mut id = 0;
        assert_eq!(
            api.create.unwrap()(
                1,
                0,
                Entity::from_raw(abi::AetherGameplayEntity {
                    world: 5,
                    id: 8,
                    reserved: 0
                })
                .raw(),
                &mut id
            ),
            0
        );
        assert_eq!(api.attach.unwrap()(1, id), 0);
        assert_eq!(api.update.unwrap()(1, id, 0.1), 9);
        assert_eq!(api.update.unwrap()(1, id, 0.1), 9);
        assert_eq!(api.detach.unwrap()(1, id), abi::AE_GAMEPLAY_SCRIPT_ERROR);
        assert_eq!(api.destroy.unwrap()(1, id), 0);
        assert_eq!(api.destroy.unwrap()(1, id), 5);
        assert_eq!(api.shutdown.unwrap()(1), 0);
    }
}
gameplay_script! { pub struct PanicScript { pub count: i64 = 0 } }
impl Script for PanicScript {
    fn update(&mut self, _: &mut Context<'_>, _: f32) -> Result {
        self.count += 1;
        panic!("fixture panic")
    }
}
aethercore::export_gameplay!(PanicScript);
unsafe extern "C" fn valid(_: u64, e: abi::AetherGameplayEntity, out: *mut u32) -> i32 {
    if e.world != 5 {
        return 6;
    }
    unsafe { *out = u32::from(e.id == 8) };
    0
}
unsafe extern "C" fn get(
    _: u64,
    _: abi::AetherGameplayEntity,
    out: *mut abi::AetherGameplayVec3,
) -> i32 {
    unsafe {
        *out = abi::AetherGameplayVec3 {
            x: 1.,
            y: 2.,
            z: 3.,
        }
    };
    0
}
unsafe extern "C" fn set(_: u64, _: abi::AetherGameplayEntity, _: abi::AetherGameplayVec3) -> i32 {
    0
}
unsafe extern "C" fn report(_: u64, _: *const abi::AetherGameplayDiagnostic) -> i32 {
    0
}
fn engine() -> abi::AetherGameplayEngineApi {
    abi::AetherGameplayEngineApi {
        structSize: size_of::<abi::AetherGameplayEngineApi>() as u32,
        abiMajor: 1,
        capabilities: 0,
        context: 0,
        entityValid: Some(valid),
        getPosition: Some(get),
        setPosition: Some(set),
        reportDiagnostic: Some(report),
    }
}
#[test]
fn scoped_access_rejects_stale_and_wrong_world_and_unsupported_services() {
    unsafe {
        let engine = engine();
        let e = Entity::from_raw(abi::AetherGameplayEntity {
            world: 5,
            id: 8,
            reserved: 0,
        });
        let mut ctx = Context::from_raw(&engine, None, e);
        assert_eq!(
            ctx.position(e).unwrap(),
            Vec3 {
                x: 1.,
                y: 2.,
                z: 3.
            }
        );
        let stale = Entity::from_raw(abi::AetherGameplayEntity {
            world: 5,
            id: 9,
            reserved: 0,
        });
        assert_eq!(ctx.position(stale).unwrap_err().0, 5);
        let wrong = Entity::from_raw(abi::AetherGameplayEntity {
            world: 6,
            id: 8,
            reserved: 0,
        });
        assert_eq!(ctx.set_position(wrong, Vec3::default()).unwrap_err().0, 6);
        assert_eq!(ctx.key_down(65).unwrap_err().0, 4);
    }
}

#[test]
fn enum_and_component_declarations_supply_metadata_and_validate_values() {
    gameplay_enum! {pub enum Direction{Left=0,Right=1}}
    gameplay_component_ref! {pub struct SpriteRef = "SpriteRenderer2D";}
    gameplay_script! {pub struct Rich{pub direction:Direction=Direction::Left,pub sprite:SpriteRef=SpriteRef::default()}}
    impl Script for Rich {}
    assert_eq!(Rich::descriptors()[0].enum_names, "Left,Right");
    assert_eq!(Rich::descriptors()[1].component_type, "SpriteRenderer2D");
    let mut rich = Rich::default();
    assert!(rich.set_property(0, Value::Enum(9)).is_err());
    rich.set_property(0, Value::Enum(1)).unwrap();
    assert_eq!(rich.get_property(0).unwrap(), Value::Enum(1));
}
#[test]
fn extension_layout_matches_native_contract() {
    use abi::services::*;
    use std::mem::{offset_of, size_of};
    assert_eq!(offset_of!(AetherGameplayServiceRequest, entity), 8);
    assert_eq!(offset_of!(AetherGameplayServiceRequest, vector), 24);
    assert_eq!(offset_of!(AetherGameplayServiceRequest, integer), 40);
    assert_eq!(offset_of!(AetherGameplayServiceRequest, text), 48);
    assert_eq!(size_of::<AetherGameplayServiceRequest>(), 80);
    assert_eq!(offset_of!(AetherGameplayServicesApi, invoke), 24);
    assert_eq!(size_of::<AetherGameplayServicesApi>(), 32);
}

#[test]
fn failed_instances_do_not_call_user_property_callbacks() {
    unsafe {
        let mut r =
            provider::Runtime::new(vec![provider::Registration::of::<PanicScript>("panic")]);
        r.initialize(&engine()).unwrap();
        let mut id = 0;
        r.create(
            0,
            abi::AetherGameplayEntity {
                world: 5,
                id: 8,
                reserved: 0,
            },
            &mut id,
        )
        .unwrap();
        r.attach(id).unwrap();
        assert!(r.update(id, 0.1).is_err());
        let mut out = provider::empty_value();
        assert_eq!(
            r.get_property(id, 0, &mut out, empty_buffer())
                .unwrap_err()
                .0,
            9
        );
        r.destroy(id).unwrap();
    }
}
fn empty_buffer() -> abi::AetherGameplayUtf8Buffer {
    abi::AetherGameplayUtf8Buffer {
        data: std::ptr::null_mut(),
        capacity: 0,
        required: std::ptr::null_mut(),
    }
}
#[test]
fn defaults_and_string_buffer_preserve_unicode_nul_and_no_partial_writes() {
    unsafe {
        gameplay_script! {pub struct Text{pub text:String="水\0😀".into()}}
        impl Script for Text {}
        let mut r = provider::Runtime::new(vec![provider::Registration::of::<Text>("Text")]);
        let mut out = provider::empty_value();
        let mut required = 0;
        let mut bytes = [99u8; 16];
        let mut buffer = abi::AetherGameplayUtf8Buffer {
            data: bytes.as_mut_ptr().cast(),
            capacity: 2,
            required: &mut required,
        };
        assert_eq!(r.default_property(0, 0, &mut out, buffer).unwrap_err().0, 3);
        assert_eq!(bytes[0], 0);
        assert_eq!(bytes[1], 99);
        assert_eq!(required, 9);
        buffer.capacity = 16;
        r.default_property(0, 0, &mut out, buffer).unwrap();
        assert_eq!(&bytes[..8], "水\0😀".as_bytes());
        assert_eq!(bytes[8], 0);
    }
}

#[test]
fn recoverable_property_error_disables_script_but_cleanup_succeeds() {
    #[derive(Default)]
    struct FailingProperty;
    impl Properties for FailingProperty {
        fn descriptors() -> &'static [Property] {
            &[]
        }
        fn get_property(&self, _: u32) -> Result<Value> {
            Err(Error(9, "property failed".into()))
        }
        fn set_property(&mut self, _: u32, _: Value) -> Result {
            Err(Error(9, "property failed".into()))
        }
    }
    impl Script for FailingProperty {}
    unsafe {
        let mut r = provider::Runtime::new(vec![provider::Registration::of::<FailingProperty>(
            "failing",
        )]);
        r.initialize(&engine()).unwrap();
        let mut id = 0;
        r.create(
            0,
            abi::AetherGameplayEntity {
                world: 5,
                id: 8,
                reserved: 0,
            },
            &mut id,
        )
        .unwrap();
        r.attach(id).unwrap();
        let mut out = provider::empty_value();
        assert_eq!(
            r.get_property(id, 0, &mut out, empty_buffer())
                .unwrap_err()
                .0,
            9
        );
        assert_eq!(r.update(id, 0.1).unwrap_err().0, 9);
        r.destroy(id).unwrap();
        r.shutdown().unwrap();
    }
}

#[test]
fn detach_runs_cleanup_after_owner_destroyed_but_ecs_access_stays_checked() {
    use std::sync::atomic::{AtomicBool, Ordering};
    unsafe extern "C" fn lifetime_valid(
        context: u64,
        _: abi::AetherGameplayEntity,
        out: *mut u32,
    ) -> i32 {
        let alive = unsafe { &*(context as *const AtomicBool) };
        unsafe { *out = u32::from(alive.load(Ordering::SeqCst)) };
        0
    }
    gameplay_script! {pub struct DetachProbe{pub detached:bool=false,pub access_error:i64=0}}
    impl Script for DetachProbe {
        fn detach(&mut self, ctx: &mut Context<'_>) -> Result {
            self.detached = true;
            self.access_error = i64::from(ctx.position(ctx.entity()).unwrap_err().0);
            ctx.log("stale owner cleanup completed")
        }
    }
    unsafe {
        let alive = AtomicBool::new(true);
        let mut engine = engine();
        engine.context = &alive as *const AtomicBool as u64;
        engine.entityValid = Some(lifetime_valid);
        let mut r = provider::Runtime::new(vec![provider::Registration::of::<DetachProbe>(
            "DetachProbe",
        )]);
        r.initialize(&engine).unwrap();
        let mut id = 0;
        r.create(
            0,
            abi::AetherGameplayEntity {
                world: 5,
                id: 8,
                reserved: 0,
            },
            &mut id,
        )
        .unwrap();
        r.attach(id).unwrap();
        alive.store(false, Ordering::SeqCst);
        r.detach(id).unwrap();
        let mut out = provider::empty_value();
        r.get_property(id, 0, &mut out, empty_buffer()).unwrap();
        assert_eq!(out.integer, 1);
        r.get_property(id, 1, &mut out, empty_buffer()).unwrap();
        assert_eq!(out.integer, i64::from(abi::AE_GAMEPLAY_INVALID_HANDLE));
        r.destroy(id).unwrap();
        r.shutdown().unwrap();
    }
}
