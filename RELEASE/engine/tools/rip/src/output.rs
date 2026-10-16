//! Writing output files without touching the ones that are already right.
//!
//! A re-rip produces the same bytes for almost every file. Leaving those
//! files alone keeps their modification times, so later steps that compare
//! times (the setup tool's waifu2x step skips PNGs older than their upscaled
//! copy) don't redo thousands of textures.

use std::io;
use std::path::Path;

/// Writes `bytes` to `path` unless the file already holds exactly them.
/// Returns true if it wrote.
pub fn write_if_changed(path: &Path, bytes: &[u8]) -> io::Result<bool> {
    // The length check first, so files that obviously differ aren't read.
    let same_length = std::fs::metadata(path)
        .is_ok_and(|meta| meta.is_file() && meta.len() == bytes.len() as u64);
    if same_length && std::fs::read(path).is_ok_and(|old| old == bytes) {
        return Ok(false);
    }
    std::fs::write(path, bytes)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unchanged_files_are_left_alone() {
        let dir = std::env::temp_dir().join(format!("rip-output-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("a.bin");
        assert!(write_if_changed(&path, b"abc").unwrap());
        let first = std::fs::metadata(&path).unwrap().modified().unwrap();
        std::thread::sleep(std::time::Duration::from_millis(20));
        assert!(!write_if_changed(&path, b"abc").unwrap());
        assert_eq!(std::fs::metadata(&path).unwrap().modified().unwrap(), first);
        // Same length, other bytes; then another length.
        assert!(write_if_changed(&path, b"abd").unwrap());
        assert!(write_if_changed(&path, b"abcd").unwrap());
        assert_eq!(std::fs::read(&path).unwrap(), b"abcd");
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
