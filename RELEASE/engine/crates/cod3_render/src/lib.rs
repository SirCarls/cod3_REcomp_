//! The Rust renderer's library: what the game links, and the C ABI it calls.
//!
//! The plan is in `dev/agents/rust-renderer/` (DESIGN.md, CRITIQUE.md, M0-ANSWERS.md): the
//! game's draws are taken from the SDK's own command processor (its `host` backend hands each
//! shader, draw, resolve and swap to this library, [`host`]) and drawn here with Vulkan, so the
//! emulated Xbox 360 GPU (EDRAM, the render-target and texture caches) no longer runs.
//!
//! - M0: the skeleton. The library builds as a static library for Linux and Windows; the game
//!   links it into both programs and checks the ABI at start.
//! - M1 step 2 (this version): the host backend's table ([`rr_host_renderer`]). With
//!   `cod3_renderer` set to `rust` the GPU emulation is off and every draw comes here; the
//!   library counts them and draws nothing yet.
//!
//! The boundary rules, kept from the first version on:
//! - Every struct that crosses it starts with `struct_size` (and `abi_version` where it is
//!   created); a mismatch is refused with a log line, never guessed at.
//! - No Rust panic crosses it: every exported function catches unwinding and returns the
//!   function's "failed" value instead.
//! - Memory never changes owner across it: what Rust allocates, Rust frees (`rr_destroy`).
//!
//! The C side of the ABI is `include/cod3_render.h`; the two must say the same thing, and the
//! tests below check the struct layout the header promises.

pub mod host;

use std::ffi::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// The ABI this library speaks. Raise it whenever an exported function or struct changes, and
/// the header's `RR_ABI_VERSION` with it. 2: [`rr_host_renderer`].
pub const ABI_VERSION: u32 = 2;

/// The library's name and version, NUL-terminated for C.
const VERSION_TEXT: &str = concat!("cod3_render ", env!("CARGO_PKG_VERSION"), "\0");

/// Log levels of [`RrHostApi::log`] (the header's `RR_LOG_*`).
pub mod log_level {
    pub const ERROR: u32 = 0;
    pub const WARNING: u32 = 1;
    pub const INFO: u32 = 2;
    pub const DEBUG: u32 = 3;
}

/// The game's log, as the library sees it: `message` is UTF-8, `length` bytes, not
/// NUL-terminated. It may be called from any thread.
pub type RrLogFn =
    unsafe extern "C" fn(user: *mut c_void, level: u32, message: *const c_char, length: usize);

/// What the game gives the library (`RrHostApi` in the header). Grows at its end only; the
/// library reads only the fields `struct_size` covers.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RrHostApi {
    /// `sizeof(RrHostApi)` as the game was built.
    pub struct_size: u32,
    /// The ABI the game was built against.
    pub abi_version: u32,
    /// Passed back unchanged to every callback.
    pub user: *mut c_void,
    /// The game's log; may be null (the library is then silent).
    pub log: Option<RrLogFn>,
}

/// One renderer, made by [`rr_create`] and freed by [`rr_destroy`]. Opaque to the game.
pub struct RrRenderer {
    api: RrHostApi,
    /// The host backend's side ([`rr_host_renderer`]).
    pub(crate) host: host::HostState,
}

impl RrRenderer {
    pub(crate) fn log(&self, level: u32, message: &str) {
        if let Some(log) = self.api.log {
            // SAFETY: the game promised in rr_create that `log` and `user` stay valid until
            // rr_destroy, and the message is a live &str for the duration of the call.
            unsafe { log(self.api.user, level, message.as_ptr().cast(), message.len()) };
        }
    }
}

/// Why `rr_create` refused a host description; said in the game's log when it can be.
fn check_host(host: &RrHostApi) -> Result<(), String> {
    let needed = std::mem::size_of::<RrHostApi>();
    if (host.struct_size as usize) < needed {
        return Err(format!(
            "the game's RrHostApi is {} bytes, this library needs {needed}",
            host.struct_size
        ));
    }
    if host.abi_version != ABI_VERSION {
        return Err(format!(
            "the game speaks ABI {}, this library ABI {ABI_VERSION}",
            host.abi_version
        ));
    }
    Ok(())
}

/// The ABI this library speaks ([`ABI_VERSION`]).
#[no_mangle]
pub extern "C" fn rr_abi_version() -> u32 {
    ABI_VERSION
}

/// The library's name and version as a NUL-terminated string that lives as long as the program.
#[no_mangle]
pub extern "C" fn rr_version() -> *const c_char {
    VERSION_TEXT.as_ptr().cast()
}

/// Makes a renderer for the game described by `host`, or returns null (with a log line when
/// `host` has a usable log) if `host` is missing, too small or for another ABI.
///
/// # Safety
///
/// `host` is null or points to a readable `RrHostApi` of at least `struct_size` bytes. Its `log`
/// callback and `user` pointer stay valid until the renderer is given to [`rr_destroy`].
#[no_mangle]
pub unsafe extern "C" fn rr_create(host: *const RrHostApi) -> *mut RrRenderer {
    if host.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: the caller promises `host` points to a readable RrHostApi; the fields read here
    // (struct_size, abi_version) come first, so even an older, smaller one is read in bounds.
    let (struct_size, abi_version) = unsafe { ((*host).struct_size, (*host).abi_version) };
    let api = if struct_size as usize >= std::mem::size_of::<RrHostApi>() {
        // SAFETY: as above, and struct_size covers the whole struct.
        unsafe { *host }
    } else {
        RrHostApi {
            struct_size,
            abi_version,
            user: std::ptr::null_mut(),
            log: None,
        }
    };
    let made = catch_unwind(AssertUnwindSafe(|| {
        let renderer = Box::new(RrRenderer {
            api,
            host: host::HostState::new(),
        });
        if let Err(why) = check_host(&api) {
            renderer.log(
                log_level::ERROR,
                &format!("cod3_render: refused to start: {why}"),
            );
            return std::ptr::null_mut();
        }
        renderer.log(
            log_level::DEBUG,
            &format!("cod3_render: made (ABI {ABI_VERSION})"),
        );
        let renderer = Box::into_raw(renderer);
        // SAFETY: just made; the table's calls get it back as their `user`.
        unsafe { (*renderer).host.table.user = renderer.cast() };
        renderer
    }));
    made.unwrap_or(std::ptr::null_mut())
}

/// The table the SDK's host backend takes (`GpuCreateInfo::host_renderer`, the SDK's
/// `rex/graphics/host_renderer.h`), or null for a null renderer. It lives as long as the
/// renderer.
///
/// # Safety
///
/// `renderer` is null or a live pointer from [`rr_create`]. Once the table is handed to the
/// plugin, only the plugin's GPU command thread calls into this renderer, and it isn't given to
/// [`rr_destroy`] while the plugin may still call.
#[no_mangle]
pub unsafe extern "C" fn rr_host_renderer(
    renderer: *mut RrRenderer,
) -> *const host::RexHostRenderer {
    if renderer.is_null() {
        return std::ptr::null();
    }
    // SAFETY: as the caller promises; only the table's address is taken.
    unsafe { std::ptr::addr_of!((*renderer).host.table) }
}

/// Frees a renderer made by [`rr_create`]. Null does nothing.
///
/// # Safety
///
/// `renderer` is null or a pointer [`rr_create`] returned that wasn't destroyed yet; it isn't used
/// again afterwards, and the host backend no longer calls it.
#[no_mangle]
pub unsafe extern "C" fn rr_destroy(renderer: *mut RrRenderer) {
    if renderer.is_null() {
        return;
    }
    // A panic here would only leak the renderer; the game goes on either way.
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller promises it came from rr_create and isn't used again.
        let renderer = unsafe { Box::from_raw(renderer) };
        renderer.log(log_level::DEBUG, "cod3_render: freed");
    }));
}

#[cfg(test)]
mod tests {
    use super::host::*;
    use super::*;
    use std::sync::Mutex;

    /// Lines the test log callback received.
    static LINES: Mutex<Vec<(u32, String)>> = Mutex::new(Vec::new());

    unsafe extern "C" fn test_log(
        _user: *mut c_void,
        level: u32,
        message: *const c_char,
        len: usize,
    ) {
        // SAFETY: the library passes a live UTF-8 slice of `len` bytes.
        let bytes = unsafe { std::slice::from_raw_parts(message.cast::<u8>(), len) };
        LINES
            .lock()
            .unwrap()
            .push((level, String::from_utf8_lossy(bytes).into_owned()));
    }

    fn host() -> RrHostApi {
        RrHostApi {
            struct_size: std::mem::size_of::<RrHostApi>() as u32,
            abi_version: ABI_VERSION,
            user: std::ptr::null_mut(),
            log: Some(test_log),
        }
    }

    #[test]
    fn layout_matches_the_header() {
        // include/cod3_render.h: uint32_t, uint32_t, void*, function pointer.
        let pointer = std::mem::size_of::<*const c_void>();
        assert_eq!(std::mem::size_of::<RrHostApi>(), 8 + 2 * pointer);
        assert_eq!(std::mem::offset_of!(RrHostApi, user), 8);
        assert_eq!(std::mem::offset_of!(RrHostApi, log), 8 + pointer);
    }

    #[test]
    fn version_is_c_text() {
        assert_eq!(rr_abi_version(), ABI_VERSION);
        // SAFETY: rr_version returns a static NUL-terminated string.
        let text = unsafe { std::ffi::CStr::from_ptr(rr_version()) };
        assert!(text.to_str().unwrap().starts_with("cod3_render "));
    }

    #[test]
    fn create_and_destroy() {
        let host = host();
        // SAFETY: a valid host description on the stack, alive for the whole test.
        let renderer = unsafe { rr_create(&host) };
        assert!(!renderer.is_null());
        // SAFETY: made by rr_create just above, destroyed once.
        unsafe { rr_destroy(renderer) };
        let lines = LINES.lock().unwrap();
        assert!(lines
            .iter()
            .any(|(level, line)| *level == log_level::DEBUG && line.contains("made")));
    }

    #[test]
    fn refuses_another_abi_or_a_short_struct() {
        let mut other_abi = host();
        other_abi.abi_version = ABI_VERSION + 1;
        let mut short = host();
        short.struct_size = 4;
        // SAFETY: valid host descriptions on the stack; null is allowed.
        unsafe {
            assert!(rr_create(&other_abi).is_null());
            assert!(rr_create(&short).is_null());
            assert!(rr_create(std::ptr::null()).is_null());
            assert!(rr_host_renderer(std::ptr::null_mut()).is_null());
            rr_destroy(std::ptr::null_mut());
        }
        let lines = LINES.lock().unwrap();
        assert!(lines
            .iter()
            .any(|(level, line)| *level == log_level::ERROR && line.contains("ABI")));
    }

    /// The host backend's calls, as the plugin makes them, reach the counts.
    #[test]
    fn host_table_counts_what_it_is_given() {
        let host = host();
        // SAFETY: a valid host description, alive for the whole test.
        let renderer = unsafe { rr_create(&host) };
        // SAFETY: made just above.
        let table = unsafe { *rr_host_renderer(renderer) };
        assert_eq!(table.abi_version, REX_HOST_RENDERER_ABI_VERSION);
        assert_eq!(table.user, renderer.cast());
        let registers = vec![0u32; 0x5003];
        let gpu = RexHostGpu {
            struct_size: std::mem::size_of::<RexHostGpu>() as u32,
            abi_version: REX_HOST_RENDERER_ABI_VERSION,
            physical_memory: std::ptr::null_mut(),
            physical_memory_size: 0x2000_0000,
            registers: registers.as_ptr(),
            register_count: registers.len() as u32,
            vulkan_api_version: 0,
            vulkan_instance: std::ptr::null_mut(),
            vulkan_physical_device: std::ptr::null_mut(),
            vulkan_device: std::ptr::null_mut(),
            vulkan_get_instance_proc_addr: std::ptr::null_mut(),
            vulkan_queue_family: 0,
            vulkan_queue_index: 0,
            lock_queue: None,
            unlock_queue: None,
            gpu_user: std::ptr::null_mut(),
        };
        let ucode = [0u32; 6];
        let shader = RexHostShader {
            struct_size: std::mem::size_of::<RexHostShader>() as u32,
            kind: REX_HOST_SHADER_PIXEL,
            hash: 1,
            guest_address: 0x1000,
            dword_count: ucode.len() as u32,
            ucode: ucode.as_ptr(),
        };
        let draw = RexHostDraw {
            struct_size: std::mem::size_of::<RexHostDraw>() as u32,
            primitive_type: 4,
            index_count: 6,
            indexed: 1,
            index_format: 0,
            index_endian: 1,
            index_guest_address: 0x2000,
            index_buffer_bytes: 12,
            major_mode_explicit: 0,
            reserved: 0,
            vertex_shader: 2,
            pixel_shader: 1,
        };
        let swap = RexHostSwap {
            struct_size: std::mem::size_of::<RexHostSwap>() as u32,
            frontbuffer_address: 0x0100_0000,
            frontbuffer_width: 1280,
            frontbuffer_height: 720,
        };
        // SAFETY: the table's own functions with its own user, as the plugin calls them, each with
        // a struct alive for the call.
        unsafe {
            assert_eq!(table.start.unwrap()(table.user, &gpu), 1);
            table.shader.unwrap()(table.user, &shader);
            table.draw.unwrap()(table.user, &draw);
            table.draw.unwrap()(table.user, &draw);
            table.resolve.unwrap()(table.user);
            table.swap.unwrap()(table.user, &swap);
            let counts = (*renderer).host.totals;
            assert_eq!(
                (
                    counts.draws,
                    counts.indexed_draws,
                    counts.resolves,
                    counts.frames
                ),
                (2, 2, 1, 1)
            );
            assert_eq!((counts.vertex_shaders, counts.pixel_shaders), (0, 1));
            table.stop.unwrap()(table.user);
            // A short struct is ignored, not read past its end.
            let mut short = draw;
            short.struct_size = 8;
            table.draw.unwrap()(table.user, &short);
            assert_eq!((*renderer).host.totals.draws, 2);
            rr_destroy(renderer);
        }
        let lines = LINES.lock().unwrap();
        assert!(lines
            .iter()
            .any(|(_, line)| line.contains("host backend started it")));
        assert!(lines
            .iter()
            .any(|(_, line)| line.contains("stopped after 1 frames")));
    }
}
