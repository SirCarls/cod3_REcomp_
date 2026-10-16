//! World at War fastfiles (PC and Xbox 360).

use crate::paths::safe_join;
use std::path::Path;

/// Lists a WaW fastfile (PC or Xbox 360) and writes its raw files (GSC/CSC scripts) to
/// `<out_dir>/rawfiles/`.
pub fn fastfile(file: &Path, out_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let named = |e: &dyn std::fmt::Display| format!("{}: {e}", file.display());
    let bytes = std::fs::read(file).map_err(|e| named(&e))?;
    let zone = ff_t4::Zone::from_fastfile(&bytes).map_err(|e| named(&e))?;
    println!(
        "{}: {} zone, {} bytes, {} script strings, {} assets",
        file.display(),
        zone.platform.name(),
        zone.data.len(),
        zone.script_strings.len(),
        zone.assets.len()
    );
    for (t, n) in zone.census() {
        let name = zone.type_name(t);
        println!("  {name:>16} ({t:2}): {n}");
    }
    let raw = zone.scan_raw_files();
    let dir = out_dir.join("rawfiles");
    for rf in &raw {
        let Some(path) = safe_join(&dir, &rf.name) else {
            eprintln!("  skipped unsafe name {:?}", rf.name);
            continue;
        };
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(&path, &rf.data)?;
    }
    println!("  wrote {} raw files to {}", raw.len(), dir.display());
    Ok(())
}
