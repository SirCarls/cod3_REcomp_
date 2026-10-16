//! Path helpers: finding a child folder or file without regard to case
//! (installs made on Windows spell `Main` and `main` either way, and Linux
//! file systems care), and checking whether one folder lies inside another.

use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

/// The child of `dir` called `name`, compared without regard to ASCII case.
/// An exact match wins; otherwise the first match in sorted order.
/// The folder is listed rather than probed with `dir.join(name)`, so the
/// result keeps the spelling on disk even on a case-insensitive file system.
pub fn child_ignoring_case(dir: &Path, name: &str) -> Option<PathBuf> {
    let mut matches: Vec<String> = fs::read_dir(dir)
        .ok()?
        .filter_map(Result::ok)
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|n| n.eq_ignore_ascii_case(name))
        .collect();
    matches.sort();
    let chosen = match matches.iter().position(|n| n == name) {
        Some(exact) => matches.swap_remove(exact),
        None => matches.into_iter().next()?,
    };
    Some(dir.join(chosen))
}

/// `fs::canonicalize`, but on Windows without the `\\?\` prefix it adds
/// (`\\?\C:\Games` becomes `C:\Games`), so paths read normally in messages
/// and in the manifest. Every resolved path goes through here, so they all
/// compare alike. Rust's standard library adds the prefix back by itself
/// where a long path needs it.
pub fn canonical(path: &Path) -> io::Result<PathBuf> {
    fs::canonicalize(path).map(without_verbatim_prefix)
}

fn without_verbatim_prefix(path: PathBuf) -> PathBuf {
    let Some(text) = path.to_str() else {
        return path;
    };
    if let Some(share) = text.strip_prefix(r"\\?\UNC\") {
        return PathBuf::from(format!(r"\\{share}"));
    }
    if let Some(rest) = text.strip_prefix(r"\\?\") {
        let bytes = rest.as_bytes();
        if bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' {
            return PathBuf::from(rest);
        }
    }
    path
}

/// Follows `relative` (folders separated by `/`) from `dir`, matching each
/// part without regard to case.
pub fn path_ignoring_case(dir: &Path, relative: &str) -> Option<PathBuf> {
    let mut path = dir.to_path_buf();
    for part in relative.split('/').filter(|p| !p.is_empty()) {
        path = child_ignoring_case(&path, part)?;
    }
    Some(path)
}

/// An absolute, clean form of `path` that works even when the end of it
/// doesn't exist yet: the longest existing part is resolved (symlinks
/// followed), the rest is appended. Refuses `..` in the part that doesn't
/// exist, since it can't be resolved safely.
pub fn resolve(path: &Path) -> io::Result<PathBuf> {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()?.join(path)
    };
    let mut existing = absolute.as_path();
    let mut missing: Vec<&std::ffi::OsStr> = Vec::new();
    loop {
        match canonical(existing) {
            Ok(resolved) => {
                let mut result = resolved;
                for part in missing.iter().rev() {
                    result.push(part);
                }
                return Ok(result);
            }
            Err(e) => {
                let Some(parent) = existing.parent() else {
                    return Err(e);
                };
                match existing.components().next_back() {
                    Some(Component::Normal(part)) => missing.push(part),
                    Some(Component::CurDir) => {}
                    _ => {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidInput,
                            format!(
                                "{} uses \"..\" in a part that doesn't exist yet",
                                path.display()
                            ),
                        ))
                    }
                }
                existing = parent;
            }
        }
    }
}

/// Whether `inner` is `outer` or lies inside it (both already resolved).
pub fn is_within(inner: &Path, outer: &Path) -> bool {
    inner.starts_with(outer)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testkit::{write_file, TempDir};

    #[test]
    fn finds_children_in_any_case() {
        let tmp = TempDir::new("paths").unwrap();
        write_file(tmp.path(), "Main/PAK0.pk3", b"x").unwrap();
        let main = child_ignoring_case(tmp.path(), "main").unwrap();
        assert!(main.ends_with("Main"));
        let pak = path_ignoring_case(tmp.path(), "MAIN/pak0.PK3").unwrap();
        assert!(pak.ends_with("Main/PAK0.pk3"));
        assert_eq!(child_ignoring_case(tmp.path(), "uo"), None);
        assert_eq!(child_ignoring_case(&tmp.path().join("nothing"), "uo"), None);
    }

    #[test]
    fn verbatim_prefix_is_dropped() {
        let simplify = |s: &str| without_verbatim_prefix(PathBuf::from(s));
        assert_eq!(
            simplify(r"\\?\C:\Games\CoD2"),
            PathBuf::from(r"C:\Games\CoD2")
        );
        assert_eq!(
            simplify(r"\\?\UNC\server\share"),
            PathBuf::from(r"\\server\share")
        );
        assert_eq!(
            simplify(r"\\?\Volume{x}\a"),
            PathBuf::from(r"\\?\Volume{x}\a")
        );
        assert_eq!(simplify("/srv/games"), PathBuf::from("/srv/games"));
    }

    #[test]
    fn resolves_paths_that_do_not_exist_yet() {
        let tmp = TempDir::new("paths").unwrap();
        let real = canonical(tmp.path()).unwrap();
        let out = resolve(&tmp.path().join("new").join("copy")).unwrap();
        assert_eq!(out, real.join("new").join("copy"));
        assert!(resolve(&tmp.path().join("new").join("..").join("x")).is_err());
        assert!(is_within(&out, &real));
        assert!(!is_within(&real, &out));
        // "Main2" is not inside "Main": the check compares whole parts.
        assert!(!is_within(Path::new("/a/Main2"), Path::new("/a/Main")));
    }
}
