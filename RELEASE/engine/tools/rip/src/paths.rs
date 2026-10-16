//! Turning names from game files into safe output paths.
//!
//! Game files name things with '/' or '\\' separators on every platform, and
//! the names are untrusted input: nothing they say may write outside the
//! output folder. Output paths are built with `Path::join`, so they use the
//! right separator on Linux and Windows.

use std::path::{Component, Path, PathBuf};

/// Joins an asset path from a game file onto `root`, refusing anything that
/// could escape it (absolute paths, `..`, drive prefixes).
pub fn safe_join(root: &Path, asset_path: &str) -> Option<PathBuf> {
    let mut out = root.to_path_buf();
    for part in asset_path.split(['/', '\\']) {
        if part.is_empty() || part == "." {
            continue;
        }
        let p = Path::new(part);
        match p.components().next() {
            Some(Component::Normal(_)) if p.components().count() == 1 => out.push(part),
            _ => return None,
        }
    }
    (out != root).then_some(out)
}

/// Resource names become file names: keep letters, digits and a few marks.
pub fn sanitize(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || "._-$".contains(c) {
                c
            } else {
                '_'
            }
        })
        .collect();
    if s.is_empty() || s.starts_with('.') {
        format!("unnamed{s}")
    } else if is_windows_device_name(&s) {
        // "con.png" would be the console on Windows, not a file.
        format!("_{s}")
    } else {
        s
    }
}

/// Names Windows keeps for devices in every folder (with any extension):
/// CON, PRN, AUX, NUL, COM1-9 and LPT1-9.
fn is_windows_device_name(name: &str) -> bool {
    let stem = name.split('.').next().unwrap_or("").to_ascii_lowercase();
    match stem.as_str() {
        "con" | "prn" | "aux" | "nul" => true,
        _ => {
            stem.len() == 4
                && (stem.starts_with("com") || stem.starts_with("lpt"))
                && stem.as_bytes()[3].is_ascii_digit()
        }
    }
}

/// A label for an archive: its path under `root` without the extension, with
/// '/' separators ("sp/saint_lo/chars"). Falls back to the file stem.
pub fn archive_label(root: Option<&Path>, file: &Path) -> String {
    let relative = root
        .and_then(|r| file.strip_prefix(r).ok())
        .unwrap_or_else(|| Path::new(file.file_name().unwrap_or_default()));
    let mut parts: Vec<String> = relative
        .components()
        .filter_map(|c| match c {
            Component::Normal(s) => Some(sanitize(&s.to_string_lossy())),
            _ => None,
        })
        .collect();
    if let Some(last) = parts.last_mut() {
        if let Some(stem) = Path::new(last.as_str()).file_stem() {
            *last = stem.to_string_lossy().into_owned();
        }
    }
    if parts.is_empty() {
        "archive".into()
    } else {
        parts.join("/")
    }
}

/// `../` repeated once per folder level of `label`, plus one: the way back
/// from `<out>/<kind>/<label>/` to `<out>/`, for relative links in files.
pub fn up_to_root(label: &str) -> String {
    "../".repeat(label.split('/').count() + 1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_join_refuses_escapes() {
        let root = Path::new("out");
        assert_eq!(
            safe_join(root, "a/b.gsc"),
            Some(root.join("a").join("b.gsc"))
        );
        assert_eq!(safe_join(root, "a\\..\\..\\x"), None);
        assert_eq!(safe_join(root, ""), None);
        assert_eq!(safe_join(root, "/etc/passwd"), Some(root.join("etc").join("passwd")));
        let drive = if cfg!(windows) {
            None
        } else {
            Some(root.join("C:").join("x"))
        };
        assert_eq!(safe_join(root, "C:/x"), drive);
    }

    #[test]
    fn sanitize_keeps_names_plain() {
        assert_eq!(sanitize("../../x"), "unnamed.._.._x");
        assert_eq!(sanitize("a/b\\c:d"), "a_b_c_d");
        assert_eq!(sanitize("con"), "_con");
        assert_eq!(sanitize("COM1.dds"), "_COM1.dds");
        assert_eq!(sanitize("console"), "console");
        assert_eq!(sanitize("com10"), "com10");
    }

    #[test]
    fn labels_and_links() {
        let root = Path::new("iso");
        let file = root.join("sp").join("saint_lo").join("chars.cod");
        assert_eq!(archive_label(Some(root), &file), "sp/saint_lo/chars");
        assert_eq!(archive_label(None, &file), "chars");
        assert_eq!(up_to_root("sp/saint_lo/chars"), "../../../../");
    }
}
