//! The SDK's shader interpreter, through `interp/xe_interp.cpp` (built by `interp/build.sh`).

use std::ffi::c_void;

/// One run's inputs (must match `XiCase` in xe_interp.cpp).
#[repr(C)]
pub struct XiCase {
    pub shader_type: u32,
    pub consts: [[f32; 4]; 512],
    pub bools: [u32; 8],
    pub loops: [u32; 32],
    pub fetch: [u32; 192],
    pub regs: [[f32; 4]; 64],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct XiExport {
    pub reg: u32,
    pub mask: u32,
    pub v: [f32; 4],
}

#[repr(C)]
pub struct XiResult {
    pub count: u32,
    pub overflow: u32,
    pub ex: [XiExport; 256],
    pub regs: [[f32; 4]; 64],
}

extern "C" {
    fn xi_new() -> *mut c_void;
    fn xi_guest(ctx: *mut c_void) -> *mut u8;
    fn xi_run(ctx: *mut c_void, ucode: *const u32, input: *const XiCase, out: *mut XiResult);
}

pub struct Interpreter {
    ctx: *mut c_void,
    guest: *mut u8,
}

impl Interpreter {
    pub fn new() -> Option<Self> {
        // SAFETY: plain C calls; a null context means the guest memory couldn't be mapped.
        let ctx = unsafe { xi_new() };
        if ctx.is_null() {
            return None;
        }
        let guest = unsafe { xi_guest(ctx) };
        Some(Interpreter { ctx, guest })
    }

    /// Writes dwords into guest physical memory at dword address `at` (the window the
    /// checker uses is small and well inside the SDK's 512 MB mapping).
    pub fn write_guest(&mut self, at: usize, words: &[u32]) {
        // SAFETY: `at + words.len()` dwords stay far below the mapping's size.
        unsafe {
            std::ptr::copy_nonoverlapping(words.as_ptr() as *const u8, self.guest.add(at * 4), words.len() * 4);
        }
    }

    pub fn run(&mut self, ucode: &[u32], input: &XiCase, out: &mut XiResult) {
        // SAFETY: the interpreter reads `ucode` within the shader's own control flow.
        unsafe { xi_run(self.ctx, ucode.as_ptr(), input, out) }
    }
}
