//! The SDK's host renderer interface (`rex/graphics/host_renderer.h`, SDK patch 0100), as Rust
//! sees it, and this library's side of it.
//!
//! The SDK's GPU plugin has a `host` backend: it keeps everything the game sees of its GPU (the
//! registers, the vblank interrupts, the command ring with its fences and waits) and draws nothing
//! itself. Every shader, draw, resolve and swap the game sends goes to a host renderer through
//! the table below. This library fills that table ([`crate::rr_host_renderer`]), and the game
//! hands it to the plugin when `cod3_renderer` is `rust` (`recomp/src/render/rust_renderer.cpp`).
//!
//! M1 step 2 (this version): the library counts what it is given and says so in the log every 10
//! seconds. It draws nothing yet, so the picture stays black; Rust's first picture is step 4.
//!
//! The header's rules hold here: every struct starts with `struct_size` and only the fields it
//! covers are read; every call comes on the GPU command thread, in the game's command order, and
//! what it points to is valid only during the call (the [`RexHostGpu`] from start until stop);
//! no panic unwinds out of a call.

use std::ffi::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::time::{Duration, Instant};

use crate::{log_level, RrRenderer};

/// The header's `REX_HOST_RENDERER_ABI_VERSION`: the interface version this side was written for.
pub const REX_HOST_RENDERER_ABI_VERSION: u32 = 1;

/// `RexHostShader::type`.
pub const REX_HOST_SHADER_VERTEX: u32 = 0;
pub const REX_HOST_SHADER_PIXEL: u32 = 1;

/// What the plugin gives the renderer at start (`RexHostGpu`). Valid until stop.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RexHostGpu {
    pub struct_size: u32,
    pub abi_version: u32,
    /// Guest physical memory from physical address 0.
    pub physical_memory: *mut u8,
    pub physical_memory_size: u64,
    /// The GPU's register file, `register_count` values, current during every call.
    pub registers: *const u32,
    pub register_count: u32,
    /// `VK_MAKE_API_VERSION` of the plugin's device; 0 when it has none.
    pub vulkan_api_version: u32,
    pub vulkan_instance: *mut c_void,
    pub vulkan_physical_device: *mut c_void,
    pub vulkan_device: *mut c_void,
    pub vulkan_get_instance_proc_addr: *mut c_void,
    pub vulkan_queue_family: u32,
    pub vulkan_queue_index: u32,
    /// Holds the queue (shared with the presenter) and returns the `VkQueue`.
    pub lock_queue: Option<unsafe extern "C" fn(gpu_user: *mut c_void) -> *mut c_void>,
    pub unlock_queue: Option<unsafe extern "C" fn(gpu_user: *mut c_void)>,
    pub gpu_user: *mut c_void,
}

/// A shader the game loads for the first time (`RexHostShader`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RexHostShader {
    pub struct_size: u32,
    /// [`REX_HOST_SHADER_VERTEX`] or [`REX_HOST_SHADER_PIXEL`].
    pub kind: u32,
    /// XXH3-64 of the microcode as the game wrote it: the Vulkan backend's name for it.
    pub hash: u64,
    pub guest_address: u32,
    pub dword_count: u32,
    /// `dword_count` dwords in the host's byte order.
    pub ucode: *const u32,
}

/// A draw (`RexHostDraw`); the rest of its state is in the registers.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RexHostDraw {
    pub struct_size: u32,
    pub primitive_type: u32,
    pub index_count: u32,
    pub indexed: u32,
    pub index_format: u32,
    pub index_endian: u32,
    pub index_guest_address: u32,
    pub index_buffer_bytes: u32,
    pub major_mode_explicit: u32,
    pub reserved: u32,
    pub vertex_shader: u64,
    pub pixel_shader: u64,
}

/// A swap: the game shows a picture (`RexHostSwap`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RexHostSwap {
    pub struct_size: u32,
    pub frontbuffer_address: u32,
    pub frontbuffer_width: u32,
    pub frontbuffer_height: u32,
}

/// The renderer's table (`RexHostRenderer`), given to the plugin. Any function may be `None`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RexHostRenderer {
    pub struct_size: u32,
    pub abi_version: u32,
    pub user: *mut c_void,
    pub name: *const c_char,
    pub start: Option<unsafe extern "C" fn(user: *mut c_void, gpu: *const RexHostGpu) -> i32>,
    pub stop: Option<unsafe extern "C" fn(user: *mut c_void)>,
    pub shader: Option<unsafe extern "C" fn(user: *mut c_void, shader: *const RexHostShader)>,
    pub draw: Option<unsafe extern "C" fn(user: *mut c_void, draw: *const RexHostDraw)>,
    pub resolve: Option<unsafe extern "C" fn(user: *mut c_void)>,
    pub swap: Option<unsafe extern "C" fn(user: *mut c_void, swap: *const RexHostSwap)>,
}

/// The name the plugin's log gives this renderer.
const NAME: &[u8] = b"the Rust renderer (cod3_render)\0";

/// How often the counts go to the log.
const REPORT_EVERY: Duration = Duration::from_secs(10);

/// What the host backend has handed over.
#[derive(Clone, Copy, Default)]
pub(crate) struct Counts {
    pub frames: u64,
    pub draws: u64,
    pub indexed_draws: u64,
    pub resolves: u64,
    pub vertex_shaders: u64,
    pub pixel_shaders: u64,
}

/// The renderer's host-side state, inside [`RrRenderer`].
pub(crate) struct HostState {
    /// The table the plugin was given; `user` points at the owning [`RrRenderer`].
    pub table: RexHostRenderer,
    /// From start until stop.
    pub gpu: Option<RexHostGpu>,
    pub totals: Counts,
    /// The totals and time at the last report.
    report_counts: Counts,
    report_time: Option<Instant>,
}

impl HostState {
    pub(crate) fn new() -> Self {
        HostState {
            table: RexHostRenderer {
                struct_size: std::mem::size_of::<RexHostRenderer>() as u32,
                abi_version: REX_HOST_RENDERER_ABI_VERSION,
                user: std::ptr::null_mut(),
                name: NAME.as_ptr().cast(),
                start: Some(host_start),
                stop: Some(host_stop),
                shader: Some(host_shader),
                draw: Some(host_draw),
                resolve: Some(host_resolve),
                swap: Some(host_swap),
            },
            gpu: None,
            totals: Counts::default(),
            report_counts: Counts::default(),
            report_time: None,
        }
    }
}

/// Runs `f` on the renderer behind `user`, catching a panic so it never unwinds into the plugin.
///
/// # Safety
///
/// `user` is the `user` of a table this library made: a live [`RrRenderer`] that only the GPU
/// command thread uses while the plugin has the table.
unsafe fn with_renderer<R>(
    user: *mut c_void,
    failed: R,
    f: impl FnOnce(&mut RrRenderer) -> R,
) -> R {
    if user.is_null() {
        return failed;
    }
    // SAFETY: as the caller promises.
    let renderer = unsafe { &mut *user.cast::<RrRenderer>() };
    match catch_unwind(AssertUnwindSafe(|| f(&mut *renderer))) {
        Ok(result) => result,
        Err(_) => {
            renderer.log(
                log_level::ERROR,
                "cod3_render: a call from the host backend panicked; ignored",
            );
            failed
        }
    }
}

/// Reads a struct the plugin passed, or `None` when it is null or smaller than this side's.
///
/// # Safety
///
/// `value` is null or points to a readable struct that starts with a `u32` struct_size.
unsafe fn read_struct<T: Copy>(value: *const T) -> Option<T> {
    if value.is_null() {
        return None;
    }
    // SAFETY: as the caller promises; struct_size comes first.
    let size = unsafe { *value.cast::<u32>() } as usize;
    if size < std::mem::size_of::<T>() {
        return None;
    }
    // SAFETY: the struct is at least as big as T.
    Some(unsafe { *value })
}

fn vulkan_version(version: u32) -> String {
    if version == 0 {
        return "no Vulkan device".to_owned();
    }
    format!(
        "Vulkan {}.{} device",
        (version >> 22) & 0x7F,
        (version >> 12) & 0x3FF
    )
}

unsafe extern "C" fn host_start(user: *mut c_void, gpu: *const RexHostGpu) -> i32 {
    // SAFETY: `user` is ours (the table's), `gpu` the plugin's for this call.
    unsafe {
        with_renderer(user, 0, |renderer| {
            let Some(gpu) = read_struct(gpu) else {
                renderer.log(
                    log_level::ERROR,
                    "cod3_render: the host backend's RexHostGpu is missing or too small; not started",
                );
                return 0;
            };
            if gpu.abi_version != REX_HOST_RENDERER_ABI_VERSION {
                renderer.log(
                    log_level::ERROR,
                    &format!(
                        "cod3_render: the host backend speaks interface version {}, this library {}; not started",
                        gpu.abi_version, REX_HOST_RENDERER_ABI_VERSION
                    ),
                );
                return 0;
            }
            renderer.host.gpu = Some(gpu);
            renderer.host.totals = Counts::default();
            renderer.host.report_counts = Counts::default();
            renderer.host.report_time = Some(Instant::now());
            renderer.log(
                log_level::INFO,
                &format!(
                    "cod3_render: the SDK's host backend started it ({}, {} GPU registers, {} MB of guest memory). \
                     It counts the game's GPU work for now and draws nothing: the picture stays black (M1 step 2)",
                    vulkan_version(gpu.vulkan_api_version),
                    gpu.register_count,
                    gpu.physical_memory_size >> 20
                ),
            );
            1
        })
    }
}

unsafe extern "C" fn host_stop(user: *mut c_void) {
    // SAFETY: `user` is ours.
    unsafe {
        with_renderer(user, (), |renderer| {
            let t = renderer.host.totals;
            renderer.log(
                log_level::INFO,
                &format!(
                    "cod3_render: stopped after {} frames, {} draws ({} indexed), {} resolves, {} vertex and {} pixel shaders",
                    t.frames, t.draws, t.indexed_draws, t.resolves, t.vertex_shaders, t.pixel_shaders
                ),
            );
            renderer.host.gpu = None;
        })
    }
}

unsafe extern "C" fn host_shader(user: *mut c_void, shader: *const RexHostShader) {
    // SAFETY: `user` is ours, `shader` the plugin's for this call.
    unsafe {
        with_renderer(user, (), |renderer| {
            if let Some(shader) = read_struct(shader) {
                if shader.kind == REX_HOST_SHADER_PIXEL {
                    renderer.host.totals.pixel_shaders += 1;
                } else {
                    renderer.host.totals.vertex_shaders += 1;
                }
            }
        })
    }
}

unsafe extern "C" fn host_draw(user: *mut c_void, draw: *const RexHostDraw) {
    // SAFETY: `user` is ours, `draw` the plugin's for this call.
    unsafe {
        with_renderer(user, (), |renderer| {
            if let Some(draw) = read_struct(draw) {
                renderer.host.totals.draws += 1;
                if draw.indexed != 0 {
                    renderer.host.totals.indexed_draws += 1;
                }
            }
        })
    }
}

unsafe extern "C" fn host_resolve(user: *mut c_void) {
    // SAFETY: `user` is ours.
    unsafe {
        with_renderer(user, (), |renderer| {
            renderer.host.totals.resolves += 1;
        })
    }
}

unsafe extern "C" fn host_swap(user: *mut c_void, swap: *const RexHostSwap) {
    // SAFETY: `user` is ours, `swap` the plugin's for this call.
    unsafe {
        with_renderer(user, (), |renderer| {
            let swap = read_struct(swap);
            renderer.host.totals.frames += 1;
            let now = Instant::now();
            let since = renderer.host.report_time.get_or_insert(now);
            let elapsed = now.duration_since(*since);
            if elapsed < REPORT_EVERY {
                return;
            }
            let (t, r) = (renderer.host.totals, renderer.host.report_counts);
            let frames = t.frames - r.frames;
            let per_frame =
                |total: u64, earlier: u64| (total - earlier) as f64 / frames.max(1) as f64;
            let size = swap.map_or(String::new(), |s| {
                format!(
                    ", front buffer {}x{}",
                    s.frontbuffer_width, s.frontbuffer_height
                )
            });
            let line = format!(
                "cod3_render: {:.1} s: {} frames ({:.1} a second){}; a frame: {:.0} draws, {:.1} resolves; \
                 shaders so far: {} vertex, {} pixel",
                elapsed.as_secs_f64(),
                frames,
                frames as f64 / elapsed.as_secs_f64(),
                size,
                per_frame(t.draws, r.draws),
                per_frame(t.resolves, r.resolves),
                t.vertex_shaders,
                t.pixel_shaders
            );
            renderer.log(log_level::INFO, &line);
            renderer.host.report_counts = t;
            renderer.host.report_time = Some(now);
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts_match_the_sdk_header() {
        // rex/graphics/host_renderer.h on a 64-bit build.
        if std::mem::size_of::<*const c_void>() == 8 {
            assert_eq!(std::mem::size_of::<RexHostGpu>(), 104);
            assert_eq!(std::mem::offset_of!(RexHostGpu, registers), 24);
            assert_eq!(std::mem::offset_of!(RexHostGpu, vulkan_instance), 40);
            assert_eq!(std::mem::offset_of!(RexHostGpu, vulkan_queue_family), 72);
            assert_eq!(std::mem::offset_of!(RexHostGpu, lock_queue), 80);
            assert_eq!(std::mem::size_of::<RexHostShader>(), 32);
            assert_eq!(std::mem::offset_of!(RexHostShader, ucode), 24);
            assert_eq!(std::mem::size_of::<RexHostDraw>(), 56);
            assert_eq!(std::mem::offset_of!(RexHostDraw, vertex_shader), 40);
            assert_eq!(std::mem::size_of::<RexHostSwap>(), 16);
            assert_eq!(std::mem::size_of::<RexHostRenderer>(), 72);
            assert_eq!(std::mem::offset_of!(RexHostRenderer, start), 24);
            assert_eq!(std::mem::offset_of!(RexHostRenderer, swap), 64);
        }
    }
}
