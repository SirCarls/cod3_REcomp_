//! Helpers for tests that build fake installs: a temporary folder that
//! deletes itself, and a writer for tiny ZIP archives (stored entries, no
//! compression). Used by this crate's tests and by `newcopy`'s; other
//! crates may use them to test code that reads a new copy.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

/// A fresh folder under the system temp folder, removed (with everything in
/// it) when dropped.
pub struct TempDir {
    path: PathBuf,
}

impl TempDir {
    /// Makes a folder with a unique name that starts with `label`.
    pub fn new(label: &str) -> io::Result<TempDir> {
        static COUNTER: AtomicU32 = AtomicU32::new(0);
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let base = std::env::temp_dir();
        loop {
            let n = COUNTER.fetch_add(1, Ordering::Relaxed);
            let path = base.join(format!(
                "game_sources-{label}-{}-{nanos}-{n}",
                std::process::id()
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(TempDir { path }),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Writes `contents` to `dir/relative`, making folders on the way.
pub fn write_file(dir: &Path, relative: &str, contents: &[u8]) -> io::Result<PathBuf> {
    let path = dir.join(relative);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&path, contents)?;
    Ok(path)
}

/// CRC-32 (the ZIP/zlib polynomial 0xEDB88320), bit by bit: test files are tiny.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in data {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

/// A valid ZIP archive holding `entries` (name, contents), stored without
/// compression.
pub fn stored_zip(entries: &[(&str, &[u8])]) -> Vec<u8> {
    stored_zip_with_comment(entries, b"")
}

/// Like [`stored_zip`], with an archive comment after the end record.
pub fn stored_zip_with_comment(entries: &[(&str, &[u8])], comment: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut directory = Vec::new();
    for (name, data) in entries {
        let offset = out.len() as u32;
        let crc = crc32(data);
        let size = data.len() as u32;
        let name_len = name.len() as u16;
        // Local file header.
        out.extend_from_slice(b"PK\x03\x04");
        out.extend_from_slice(&20u16.to_le_bytes()); // version needed
        out.extend_from_slice(&0u16.to_le_bytes()); // flags
        out.extend_from_slice(&0u16.to_le_bytes()); // method: stored
        out.extend_from_slice(&0u16.to_le_bytes()); // time
        out.extend_from_slice(&0x21u16.to_le_bytes()); // date: 1980-01-01
        out.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&size.to_le_bytes());
        out.extend_from_slice(&size.to_le_bytes());
        out.extend_from_slice(&name_len.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes()); // extra length
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(data);
        // Central directory entry.
        directory.extend_from_slice(b"PK\x01\x02");
        directory.extend_from_slice(&20u16.to_le_bytes()); // version made by
        directory.extend_from_slice(&20u16.to_le_bytes()); // version needed
        directory.extend_from_slice(&0u16.to_le_bytes()); // flags
        directory.extend_from_slice(&0u16.to_le_bytes()); // method
        directory.extend_from_slice(&0u16.to_le_bytes()); // time
        directory.extend_from_slice(&0x21u16.to_le_bytes()); // date
        directory.extend_from_slice(&crc.to_le_bytes());
        directory.extend_from_slice(&size.to_le_bytes());
        directory.extend_from_slice(&size.to_le_bytes());
        directory.extend_from_slice(&name_len.to_le_bytes());
        directory.extend_from_slice(&0u16.to_le_bytes()); // extra length
        directory.extend_from_slice(&0u16.to_le_bytes()); // comment length
        directory.extend_from_slice(&0u16.to_le_bytes()); // disk
        directory.extend_from_slice(&0u16.to_le_bytes()); // internal attributes
        directory.extend_from_slice(&0u32.to_le_bytes()); // external attributes
        directory.extend_from_slice(&offset.to_le_bytes());
        directory.extend_from_slice(name.as_bytes());
    }
    let directory_offset = out.len() as u32;
    out.extend_from_slice(&directory);
    let count = entries.len() as u16;
    out.extend_from_slice(b"PK\x05\x06");
    out.extend_from_slice(&0u16.to_le_bytes()); // this disk
    out.extend_from_slice(&0u16.to_le_bytes()); // directory disk
    out.extend_from_slice(&count.to_le_bytes());
    out.extend_from_slice(&count.to_le_bytes());
    out.extend_from_slice(&(directory.len() as u32).to_le_bytes());
    out.extend_from_slice(&directory_offset.to_le_bytes());
    out.extend_from_slice(&(comment.len() as u16).to_le_bytes());
    out.extend_from_slice(comment);
    out
}

/// A tiny valid archive whose one entry names the archive, so every fake
/// archive has different contents (and a different SHA-256).
pub fn fake_archive(name: &str) -> Vec<u8> {
    stored_zip(&[(&format!("{name}.txt"), name.as_bytes())])
}

/// Archive names in the owner's real installs (folder as the game spells
/// it, then file name): Call of Duty 1.5 + United Offensive 1.51b in one
/// folder, and Call of Duty 2 1.3.
pub fn real_archive_names(game: &str) -> Vec<String> {
    let mut names = Vec::new();
    match game {
        "cod" | "coduo" => {
            for i in "0123456789ab".chars() {
                names.push(format!("Main/pak{i}.pk3"));
            }
            for i in 0..=5 {
                names.push(format!("Main/localized_english_pak{i}.pk3"));
            }
            if game == "coduo" {
                for i in 0..=7 {
                    names.push(format!("uo/pakuo{i:02}.pk3"));
                }
                for i in 0..=2 {
                    names.push(format!("uo/localized_english_pakuo{i:02}.pk3"));
                }
                names.push("uo/uomappack00.pk3".to_string());
            }
        }
        "cod2" => {
            for i in 0..=15 {
                names.push(format!("main/iw_{i:02}.iwd"));
            }
            for i in 0..=11 {
                names.push(format!("main/localized_english_iw{i:02}.iwd"));
            }
        }
        _ => {}
    }
    names
}

/// Writes a fake install of `game` ("cod", "coduo" or "cod2") into `dir`:
/// every archive of the owner's real install as a tiny ZIP, plus the
/// version files and stand-ins for the executables (a few text bytes, so a
/// test can prove they are never copied).
pub fn fake_install(dir: &Path, game: &str) -> io::Result<()> {
    for relative in real_archive_names(game) {
        let name = relative.rsplit('/').next().unwrap_or(&relative);
        write_file(dir, &relative, &fake_archive(name))?;
    }
    let executable_stub = b"not a real program; tests check it is never copied";
    match game {
        "cod" | "coduo" => {
            write_file(
                dir,
                "version.inf",
                b"[Version Info]\r\nExtVersion=1.5\r\nIntVersion=5.3\r\n\r\n[Product Info]\r\nProduct=Call of Duty\r\n",
            )?;
            write_file(dir, "localization.txt", b"english\r\n")?;
            write_file(dir, "CoDSP.exe", executable_stub)?;
            write_file(dir, "CoDMP.exe", executable_stub)?;
            write_file(dir, "Main/game_mp_x86.dll", executable_stub)?;
            if game == "coduo" {
                write_file(
                    dir,
                    "uo/version.inf",
                    b"[Version Info]\r\nExtVersion=1.51b\r\nIntVersion=2.3\r\n",
                )?;
                write_file(dir, "CoDUOSP.exe", executable_stub)?;
                write_file(dir, "CoDUOMP.exe", executable_stub)?;
                write_file(dir, "uo/uo_game_mp_x86.dll", executable_stub)?;
            }
        }
        "cod2" => {
            write_file(
                dir,
                "version.inf",
                b"[Version Info]\r\nExtVersion=1.3\r\nIntVersion=2.3\r\n",
            )?;
            write_file(dir, "localization.txt", b"english\r\n")?;
            write_file(dir, "CoD2SP_s.exe", executable_stub)?;
            write_file(dir, "CoD2MP_s.exe", executable_stub)?;
            write_file(dir, "mss32.dll", executable_stub)?;
        }
        _ => {}
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn temp_dirs_are_unique_and_removed() {
        let kept;
        {
            let a = TempDir::new("kit").unwrap();
            let b = TempDir::new("kit").unwrap();
            assert_ne!(a.path(), b.path());
            write_file(a.path(), "x/y.txt", b"z").unwrap();
            kept = a.path().to_path_buf();
        }
        assert!(!kept.exists());
    }
}
