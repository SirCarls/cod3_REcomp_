//! Reading the machine on Windows, with the Win32 functions declared here
//! (no extra crate):
//!
//! - `GetLogicalProcessorInformationEx(RelationAll)`: cores and their logical
//!   CPUs, each core's `EfficiencyClass` (Windows 10 and later; the highest
//!   class is the P-cores), and the L3 caches with the CPUs sharing them;
//! - `GetProcessAffinityMask`: the CPUs this process may use;
//! - `GlobalMemoryStatusEx`: memory;
//! - `RtlGetVersion` from ntdll (it reports the real version, where
//!   `GetVersionEx` doesn't without a manifest), and `wine_get_version`, an
//!   export only Wine's ntdll has;
//! - the CPU's name from `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0`
//!   (`ProcessorNameString`).
//!
//! Processor group 0 only: Windows keeps a process in one group unless it
//! asks for more, and machines with more than 64 logical CPUs are rare.
//!
//! The record layouts below follow Microsoft's headers (`winnt.h`) for 64-bit
//! Windows; `tools/check_budgets.cpp` notes how they were checked against
//! MinGW's.

use std::collections::BTreeSet;
use std::ffi::c_void;

use crate::{restrict, Profile};

type Bool = i32;
type Handle = *mut c_void;
type HKey = *mut c_void;

const RELATION_PROCESSOR_CORE: u32 = 0;
const RELATION_CACHE: u32 = 2;
const RELATION_ALL: u32 = 0xFFFF;
const KEY_READ: u32 = 0x20019;
const REG_SZ: u32 = 1;
// HKEY_LOCAL_MACHINE is the predefined handle value 0x80000002, sign-extended.
const HKEY_LOCAL_MACHINE: HKey = 0x8000_0002_u32 as i32 as isize as HKey;

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: Relationship (u32), Size (u32),
// then the record for that relationship.
const RECORD_HEADER: usize = 8;
// PROCESSOR_RELATIONSHIP: Flags (u8), EfficiencyClass (u8), Reserved[20],
// GroupCount (u16) at 22, GroupMask[] (GROUP_AFFINITY, 8-byte aligned) at 24.
const PROCESSOR_EFFICIENCY_CLASS: usize = 1;
const PROCESSOR_GROUP_COUNT: usize = 22;
const PROCESSOR_GROUP_MASK: usize = 24;
// CACHE_RELATIONSHIP: Level (u8), Associativity (u8), LineSize (u16),
// CacheSize (u32), Type (u32), Reserved[18], GroupCount (u16) at 30,
// GroupMask (GROUP_AFFINITY) at 32.
const CACHE_LEVEL: usize = 0;
const CACHE_GROUP_MASK: usize = 32;
// GROUP_AFFINITY: Mask (KAFFINITY, pointer-sized) at 0, Group (u16) at 8.
const AFFINITY_GROUP: usize = 8;

#[repr(C)]
struct MemoryStatusEx {
    length: u32,
    memory_load: u32,
    total_phys: u64,
    avail_phys: u64,
    total_page_file: u64,
    avail_page_file: u64,
    total_virtual: u64,
    avail_virtual: u64,
    avail_extended_virtual: u64,
}

#[repr(C)]
struct OsVersionInfoW {
    size: u32,
    major: u32,
    minor: u32,
    build: u32,
    platform: u32,
    service_pack: [u16; 128],
}

#[link(name = "kernel32")]
extern "system" {
    fn GetLogicalProcessorInformationEx(relation: u32, buffer: *mut u8, length: *mut u32) -> Bool;
    fn GetCurrentProcess() -> Handle;
    fn GetProcessAffinityMask(
        process: Handle,
        process_mask: *mut usize,
        system_mask: *mut usize,
    ) -> Bool;
    fn GlobalMemoryStatusEx(status: *mut MemoryStatusEx) -> Bool;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
}

#[link(name = "advapi32")]
extern "system" {
    fn RegOpenKeyExW(
        key: HKey,
        sub_key: *const u16,
        options: u32,
        access: u32,
        result: *mut HKey,
    ) -> i32;
    fn RegQueryValueExW(
        key: HKey,
        name: *const u16,
        reserved: *mut u32,
        kind: *mut u32,
        data: *mut u8,
        size: *mut u32,
    ) -> i32;
    fn RegCloseKey(key: HKey) -> i32;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn mask_cpus(mask: usize) -> Vec<usize> {
    (0..usize::BITS as usize)
        .filter(|cpu| mask & (1usize << cpu) != 0)
        .collect()
}

fn read_u16(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(at..at + 2)?.try_into().ok()?))
}

fn read_u32(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
}

fn read_mask(bytes: &[u8], at: usize) -> Option<usize> {
    const WIDTH: usize = std::mem::size_of::<usize>();
    Some(usize::from_le_bytes(
        bytes.get(at..at + WIDTH)?.try_into().ok()?,
    ))
}

/// The OS version, and the Wine version when there is one.
fn os_version() -> (String, bool) {
    let ntdll_name = wide("ntdll.dll");
    // SAFETY: plain Win32 calls with valid, NUL-terminated arguments; the
    // function pointers are only called when found, with the signatures
    // ntdll documents (RtlGetVersion) or Wine defines (wine_get_version).
    unsafe {
        let ntdll = GetModuleHandleW(ntdll_name.as_ptr());
        if ntdll.is_null() {
            return ("Windows".into(), false);
        }
        let mut os = "Windows".to_string();
        let get_version = GetProcAddress(ntdll, c"RtlGetVersion".as_ptr().cast());
        if !get_version.is_null() {
            let get_version: extern "system" fn(*mut OsVersionInfoW) -> i32 =
                std::mem::transmute(get_version);
            let mut info: OsVersionInfoW = std::mem::zeroed();
            info.size = std::mem::size_of::<OsVersionInfoW>() as u32;
            if get_version(&mut info) == 0 {
                os = format!("Windows {}.{}.{}", info.major, info.minor, info.build);
            }
        }
        let wine_version = GetProcAddress(ntdll, c"wine_get_version".as_ptr().cast());
        if wine_version.is_null() {
            return (os, false);
        }
        let wine_version: extern "C" fn() -> *const std::ffi::c_char =
            std::mem::transmute(wine_version);
        let version = wine_version();
        let version = if version.is_null() {
            "?".to_string()
        } else {
            std::ffi::CStr::from_ptr(version)
                .to_string_lossy()
                .into_owned()
        };
        (format!("{os} (Wine {version})"), true)
    }
}

fn cpu_model() -> String {
    let path = wide(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
    let value = wide("ProcessorNameString");
    let mut name = [0u16; 128];
    // SAFETY: the key is closed on every path after a successful open; the
    // value is read into a buffer whose size in bytes is passed, minus one
    // character kept for a terminating NUL.
    unsafe {
        let mut key: HKey = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.as_ptr(), 0, KEY_READ, &mut key) != 0 {
            return String::new();
        }
        let mut kind = 0u32;
        let mut size = ((name.len() - 1) * 2) as u32;
        let status = RegQueryValueExW(
            key,
            value.as_ptr(),
            std::ptr::null_mut(),
            &mut kind,
            name.as_mut_ptr().cast(),
            &mut size,
        );
        RegCloseKey(key);
        if status != 0 || kind != REG_SZ {
            return String::new();
        }
    }
    let end = name.iter().position(|&c| c == 0).unwrap_or(name.len());
    String::from_utf16_lossy(&name[..end]).trim().to_string()
}

pub(crate) fn read() -> Profile {
    let (os, under_wine) = os_version();
    let mut p = Profile {
        os,
        under_wine,
        cpu_model: cpu_model(),
        ..Profile::default()
    };

    let (mut process_mask, mut system_mask) = (0usize, 0usize);
    // SAFETY: GetCurrentProcess returns a pseudo handle that needs no closing;
    // both masks are valid out-pointers.
    unsafe {
        GetProcessAffinityMask(GetCurrentProcess(), &mut process_mask, &mut system_mask);
    }
    p.logical_cpus = system_mask.count_ones() as usize;
    let allowed: BTreeSet<usize> = mask_cpus(process_mask).into_iter().collect();
    p.allowed_cpus = allowed.iter().copied().collect();

    let mut length = 0u32;
    // SAFETY: the first call only reports the size needed; the second fills a
    // buffer of exactly that size.
    let buffer = unsafe {
        GetLogicalProcessorInformationEx(RELATION_ALL, std::ptr::null_mut(), &mut length);
        let mut buffer = vec![0u8; length as usize];
        if length == 0
            || GetLogicalProcessorInformationEx(RELATION_ALL, buffer.as_mut_ptr(), &mut length) == 0
        {
            buffer.clear();
        }
        buffer.truncate(length as usize);
        buffer
    };

    // Each record: (relationship, the record's body).
    let mut records = Vec::new();
    let mut offset = 0;
    while let (Some(relation), Some(size)) =
        (read_u32(&buffer, offset), read_u32(&buffer, offset + 4))
    {
        let size = size as usize;
        if size < RECORD_HEADER || offset + size > buffer.len() {
            break;
        }
        records.push((relation, &buffer[offset + RECORD_HEADER..offset + size]));
        offset += size;
    }

    // Hybrid CPUs: P-cores have the highest efficiency class.
    let max_class = records
        .iter()
        .filter(|(relation, _)| *relation == RELATION_PROCESSOR_CORE)
        .filter_map(|(_, body)| body.get(PROCESSOR_EFFICIENCY_CLASS).copied())
        .max()
        .unwrap_or(0);
    let mut cores = Vec::new();
    let mut caches = Vec::new();
    for (relation, body) in records {
        match relation {
            RELATION_PROCESSOR_CORE => {
                if read_u16(body, PROCESSOR_GROUP_COUNT).unwrap_or(0) == 0
                    || read_u16(body, PROCESSOR_GROUP_MASK + AFFINITY_GROUP) != Some(0)
                {
                    continue;
                }
                let Some(mask) = read_mask(body, PROCESSOR_GROUP_MASK) else {
                    continue;
                };
                let cpus = mask_cpus(mask);
                let class = body.get(PROCESSOR_EFFICIENCY_CLASS).copied().unwrap_or(0);
                if max_class > 0 && class < max_class {
                    p.efficiency_cpus
                        .extend(cpus.iter().copied().filter(|c| allowed.contains(c)));
                }
                cores.push(cpus);
            }
            RELATION_CACHE => {
                if body.get(CACHE_LEVEL) != Some(&3)
                    || read_u16(body, CACHE_GROUP_MASK + AFFINITY_GROUP) != Some(0)
                {
                    continue;
                }
                if let Some(mask) = read_mask(body, CACHE_GROUP_MASK) {
                    caches.push(mask_cpus(mask));
                }
            }
            _ => {}
        }
    }
    p.efficiency_cpus.sort_unstable();
    p.efficiency_cpus.dedup();
    p.cores = restrict(&cores, &allowed);
    p.l3_groups = restrict(&caches, &allowed);

    // SAFETY: the structure's size is set as the function requires.
    unsafe {
        let mut memory: MemoryStatusEx = std::mem::zeroed();
        memory.length = std::mem::size_of::<MemoryStatusEx>() as u32;
        if GlobalMemoryStatusEx(&mut memory) != 0 {
            p.ram_total_bytes = memory.total_phys;
            p.ram_available_bytes = memory.avail_phys;
        }
    }
    p
}
