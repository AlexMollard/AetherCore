use aethercore::{abi::*, provider::utf8};
use std::sync::Mutex;
static DIAGNOSTICS: Mutex<Vec<String>> = Mutex::new(Vec::new());
unsafe extern "C" fn valid(_: u64, e: AetherGameplayEntity, out: *mut u32) -> i32 {
    if e.world != 11 {
        return AE_GAMEPLAY_WRONG_WORLD;
    }
    unsafe { *out = u32::from(e.id == 42) };
    0
}
unsafe extern "C" fn position(
    _: u64,
    _: AetherGameplayEntity,
    out: *mut AetherGameplayVec3,
) -> i32 {
    unsafe {
        *out = AetherGameplayVec3 {
            x: 0.,
            y: 0.,
            z: 0.,
        }
    };
    0
}
unsafe extern "C" fn set_position(_: u64, _: AetherGameplayEntity, _: AetherGameplayVec3) -> i32 {
    0
}
unsafe extern "C" fn diagnostic(_: u64, d: *const AetherGameplayDiagnostic) -> i32 {
    let d = unsafe { &*d };
    let text =
        unsafe { std::slice::from_raw_parts(d.message.data.cast(), d.message.length as usize) };
    DIAGNOSTICS
        .lock()
        .unwrap()
        .push(String::from_utf8_lossy(text).into_owned());
    0
}
fn main() {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "normal".into());
    assert!(["normal", "error", "unwind", "abort"].contains(&mode.as_str()));
    if mode != "normal" {
        unsafe { std::env::set_var("AETHER_RUST_FIXTURE_FAILURE", &mode) }
    }
    unsafe {
        let mut api: AetherGameplayModuleApi = std::mem::zeroed();
        assert_eq!(
            aether_gameplay::fixture_query(1, size_of_val(&api) as u32, &mut api),
            0
        );
        let engine = AetherGameplayEngineApi {
            structSize: size_of::<AetherGameplayEngineApi>() as u32,
            abiMajor: 1,
            capabilities: 0,
            context: 0,
            entityValid: Some(valid),
            getPosition: Some(position),
            setPosition: Some(set_position),
            reportDiagnostic: Some(diagnostic),
        };
        assert_eq!(api.initialize.unwrap()(api.context, &engine), 0);
        let mut id = 0;
        assert_eq!(
            api.create.unwrap()(
                api.context,
                1,
                AetherGameplayEntity {
                    world: 11,
                    id: 42,
                    reserved: 0
                },
                &mut id
            ),
            0
        );
        assert_eq!(api.attach.unwrap()(api.context, id), 0);
        eprintln!("PROBE_ENTER_UPDATE {mode}");
        let first = api.update.unwrap()(api.context, id, 0.25);
        if mode == "normal" {
            assert_eq!(first, 0)
        } else {
            assert_eq!(first, AE_GAMEPLAY_SCRIPT_ERROR);
            assert_eq!(
                api.update.unwrap()(api.context, id, 0.25),
                AE_GAMEPLAY_SCRIPT_ERROR
            );
            let messages = DIAGNOSTICS.lock().unwrap();
            let needle = if mode == "error" {
                "recoverable error"
            } else {
                "unwind contained"
            };
            assert!(
                messages.iter().any(|m| m.contains(needle)),
                "missing diagnostic: {messages:?}"
            );
        }
        let mut value = aethercore::provider::empty_value();
        let buffer = AetherGameplayUtf8Buffer {
            data: std::ptr::null_mut(),
            capacity: 0,
            required: std::ptr::null_mut(),
        };
        if mode == "normal" {
            assert_eq!(
                api.getProperty.unwrap()(api.context, id, 0, &mut value, buffer),
                0
            );
            assert_eq!(value.integer, 1);
            assert_eq!(api.detach.unwrap()(api.context, id), 0)
        } else {
            assert_eq!(
                api.getProperty.unwrap()(api.context, id, 0, &mut value, buffer),
                AE_GAMEPLAY_SCRIPT_ERROR
            )
        }
        assert_eq!(api.destroy.unwrap()(api.context, id), 0);
        assert_eq!(api.shutdown.unwrap()(api.context), 0);
        let _ = utf8("");
        println!("PROBE_PASSED {mode}");
    }
}
