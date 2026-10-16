//! Call of Duty 3 (Xbox 360) `.cod` archives: splitting, textures, meshes with
//! materials, and layout checks.
//!
//! Output layout (all under the chosen output folder):
//!
//! ```text
//! textures/<name>.dds   GPU-ready base level (DXT1/3/5, DXN, A8R8G8B8)
//! textures/<name>.png   the same, decoded to RGBA for glTF viewers and Unreal
//! textures/hd_keys.tsv  every texture instance's key for the HD pack (`hd`)
//! meshes/<label>/<mesh>.gltf + .bin
//! materials/<label>.json   every material's parameters and every model's parts
//! ```
//!
//! Files whose content didn't change are not rewritten, so their
//! modification times stay (see `output`).
//!
//! `<label>` is the archive's path under the input folder without the
//! extension ("sp/saint_lo/chars"), or just its name for single files.
//! Texture names are shared by all archives, as the game resolves imported
//! textures by name.

use crate::hd::{self, KeyLine, KeyList};
use crate::jobs::Claims;
use crate::output::write_if_changed;
use crate::paths::{sanitize, up_to_root};
use cod3_assets::material::{self, Material, TextureKind, TextureRole, Value};
use cod3_assets::{model, texture_set};
use cod3_mesh::{json_string, GltfMaterial};
use kapf::apkf::{Chunk, Link};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::path::Path;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

/// Texture object layout (offsets from the object start):
/// +0x00 u32 width, u32 height; +0x10 a 0x1C-byte resource header;
/// +0x2C the Xbox 360 D3D texture header (Common, ReferenceCount, Fence,
/// ReadFence, Identifier, BaseFlush, MipFlush); +0x48 the Xenos fetch
/// constant (6 words), which ends the object.
const TEX_WIDTH_OFFSET: usize = 0x00;
const TEX_HEIGHT_OFFSET: usize = 0x04;
const TEX_D3D_HEADER_OFFSET: usize = 0x2C;
const TEX_FETCH_OFFSET: usize = 0x48;
const TEX_FETCH_WORDS: usize = 6;
/// D3D Common value of a texture header.
const D3D_COMMON_TEXTURE: u32 = 0x0020_0003;
/// Section of `frontend.cod` holding one "dynamic" texture. About 45 MB of
/// data (probably more frames of the animated menu background) follow its
/// tables, so the "tables end at the chunk end" rule doesn't apply there.
const DYNAMIC_TEXTURE_SECTION: &str = "DTEX";

/// Calls `visit` with the section name and chunk for every chunk of the
/// archive that has an APKF directory.
fn for_each_chunk(file: &Path, mut visit: impl FnMut(&str, &Chunk) -> Result<()>) -> Result<()> {
    let bytes = std::fs::read(file)?;
    let archive = kapf::Archive::parse(&bytes)?;
    for section in &archive.sections {
        for entry in &section.chunks {
            let data = archive.read_chunk(entry)?;
            if let Ok(chunk) = Chunk::parse(&data) {
                visit(&section.name, &chunk)?;
            }
        }
    }
    Ok(())
}

/// True for a TEX item that has pixel data (texture sets have none).
fn has_pixels(item: &kapf::apkf::Item) -> bool {
    item.gpu.is_some_and(|(_, size)| size > 0)
}

/// Splits an archive into its resources:
/// `<out_dir>/<archive>/<TYPE>/<name>.object.bin` and `.gpu.bin`. Sections
/// without an APKF directory are written whole as `<section>.<n>.bin`.
pub fn split(file: &Path, out_dir: &Path) -> Result<()> {
    let bytes = std::fs::read(file)?;
    let archive = kapf::Archive::parse(&bytes)?;
    let stem = file
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("archive");
    let dir = out_dir.join(sanitize(stem));
    std::fs::create_dir_all(&dir)?;
    println!(
        "{}: KAPF version {:#x}, built by {:?}, {} sections",
        file.display(),
        archive.version,
        archive.builder,
        archive.sections.len()
    );
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    for section in &archive.sections {
        let label = if section.name.is_empty() {
            "main"
        } else {
            section.name.as_str()
        };
        for (i, entry) in section.chunks.iter().enumerate() {
            let data = archive.read_chunk(entry)?;
            let Ok(chunk) = Chunk::parse(&data) else {
                std::fs::write(dir.join(format!("{}.{i}.bin", sanitize(label))), &data)?;
                continue;
            };
            for t in &chunk.directory.types {
                let type_dir = dir.join(sanitize(&t.type_name()));
                std::fs::create_dir_all(&type_dir)?;
                for item in &t.items {
                    *counts.entry(t.type_name()).or_default() += 1;
                    let base = sanitize(&item.name);
                    if let Some(object) = chunk.object(item) {
                        std::fs::write(type_dir.join(format!("{base}.object.bin")), object)?;
                    }
                    if let Some(gpu) = chunk.gpu(item) {
                        std::fs::write(type_dir.join(format!("{base}.gpu.bin")), gpu)?;
                    }
                }
            }
        }
    }
    for (t, n) in &counts {
        println!("  {t:>10}: {n}");
    }
    Ok(())
}

/// Writes every unpacked chunk as `<out_dir>/<archive>/<section>.<n>.bin`.
pub fn chunks(file: &Path, out_dir: &Path) -> Result<()> {
    let bytes = std::fs::read(file)?;
    let archive = kapf::Archive::parse(&bytes)?;
    let stem = file
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("archive");
    let dir = out_dir.join(sanitize(stem));
    std::fs::create_dir_all(&dir)?;
    for section in &archive.sections {
        let label = if section.name.is_empty() {
            "main"
        } else {
            section.name.as_str()
        };
        for (i, entry) in section.chunks.iter().enumerate() {
            std::fs::write(
                dir.join(format!("{}.{i}.bin", sanitize(label))),
                archive.read_chunk(entry)?,
            )?;
        }
    }
    Ok(())
}

fn fetch_constant(chunk: &Chunk, object_at: usize) -> Option<xenos::FetchConstant> {
    let mut words = [0u32; TEX_FETCH_WORDS];
    for (i, w) in words.iter_mut().enumerate() {
        *w = chunk.u32_at(object_at + TEX_FETCH_OFFSET + 4 * i)?;
    }
    Some(xenos::FetchConstant::parse(words))
}

/// Writes the archive's textures to `<out_dir>/textures/` as DDS and PNG.
/// Textures another archive already wrote (by name, through `claims`) are
/// skipped, but every instance the HD texture pack could replace still adds
/// its key to `keys`, and a picture under that name that no instance saved
/// yet is saved as `<name>__<key>.png` for the pack (see hd.rs). The
/// archive's materials tell `keys` what each texture holds (the kind of
/// the parameters that use it, by name hash), and its texture sets which
/// textures they stand for. Returns a short report.
pub fn textures(file: &Path, out_dir: &Path, claims: &Claims, keys: &KeyList) -> Result<String> {
    let dir = out_dir.join("textures");
    std::fs::create_dir_all(&dir)?;
    let (mut written, mut unchanged, mut shared, mut sets) = (0usize, 0usize, 0usize, 0usize);
    // Other pictures under a name another instance saved (written now).
    let mut variants = 0usize;
    let mut skipped: BTreeMap<String, usize> = BTreeMap::new();
    for_each_chunk(file, |_, chunk| {
        for item in chunk.items_of("MAT") {
            for param in material::parse(chunk, item).params {
                if let Value::Texture(Some(texture)) = param.value {
                    keys.add_use(texture.hash, TextureKind::of_param(&param.name));
                }
            }
        }
        for item in chunk.items_of("TEX") {
            if !has_pixels(item) {
                sets += 1;
                if let Some(set) = texture_set::parse(chunk, item) {
                    keys.add_set(
                        item.name_hash,
                        set.textures.into_iter().map(|t| t.hash).collect(),
                    );
                }
                continue;
            }
            let (Some(gpu), Some(fetch)) = (chunk.gpu(item), fetch_constant(chunk, item.object.0))
            else {
                *skipped.entry("outside the chunk".into()).or_default() += 1;
                continue;
            };
            let claimed = claims.claim(&item.name);
            let wants_key = xenos::hd_replaceable(&fetch);
            if !claimed && !wants_key {
                shared += 1;
                continue;
            }
            let linear = match xenos::base_level_linear(&fetch, gpu) {
                Ok(linear) => linear,
                Err(e) => {
                    if claimed {
                        *skipped.entry(e.to_string()).or_default() += 1;
                    } else {
                        shared += 1;
                    }
                    continue;
                }
            };
            let base = sanitize(&item.name);
            if wants_key {
                let key = xenos::hd_key(&linear);
                // The PNG that holds this instance's picture: the one saved
                // under the name, or, for another picture under the same
                // name (each level has its own lightmaps, fog ramp ...), a
                // file of its own. A picture already saved by another
                // instance is found again when hd_keys.tsv is written.
                let file = if claimed {
                    keys.claim_picture(key);
                    base.clone()
                } else if !hd::keep_original(&base, fetch.width, fetch.height)
                    && keys.claim_picture(key)
                {
                    let file = hd::variant_file(&base, key);
                    match xenos::rgba8(&fetch, &linear) {
                        Ok(rgba) => {
                            let png = png_write::encode_rgba(fetch.width, fetch.height, &rgba);
                            if write_if_changed(&dir.join(format!("{file}.png")), &png)? {
                                variants += 1;
                            }
                            file
                        }
                        Err(e) => {
                            *skipped.entry(e.to_string()).or_default() += 1;
                            String::new()
                        }
                    }
                } else {
                    String::new()
                };
                keys.add(KeyLine {
                    name: base.clone(),
                    key,
                    width: fetch.width,
                    height: fetch.height,
                    format: fetch.format.to_xenos(),
                    file,
                    kind: hd::Kind::Unknown,
                    name_hash: item.name_hash,
                });
            }
            if !claimed {
                shared += 1;
                continue;
            }
            let mut changed = false;
            if let Ok(dds) = xenos::dds_file(&fetch, &linear) {
                changed |= write_if_changed(&dir.join(format!("{base}.dds")), &dds)?;
            }
            match xenos::rgba8(&fetch, &linear) {
                Ok(rgba) => {
                    let png = png_write::encode_rgba(fetch.width, fetch.height, &rgba);
                    changed |= write_if_changed(&dir.join(format!("{base}.png")), &png)?;
                    if changed {
                        written += 1;
                    } else {
                        unchanged += 1;
                    }
                }
                Err(e) => *skipped.entry(e.to_string()).or_default() += 1,
            }
        }
        Ok(())
    })?;
    let mut report = format!(
        "{}: {written} textures written, {unchanged} unchanged, {shared} already written by \
         another archive ({variants} of them another picture, saved separately for the HD pack), \
         {sets} texture sets\n",
        file.display()
    );
    for (why, n) in skipped {
        let _ = writeln!(report, "  skipped {n}: {why}");
    }
    Ok(report)
}

/// Textures to look for: by name (as in `textures/`, sanitized) or by key
/// (the 16 hex digits of hd_keys.tsv).
#[derive(Default)]
pub struct Wanted {
    pub names: BTreeSet<String>,
    pub keys: BTreeSet<u64>,
}

impl Wanted {
    /// Each argument is a key if it is 16 hex digits, else a name.
    pub fn from_args(args: &[String]) -> Self {
        let mut wanted = Wanted::default();
        for arg in args {
            match u64::from_str_radix(arg, 16) {
                Ok(key) if arg.len() == 16 => {
                    wanted.keys.insert(key);
                }
                _ => {
                    wanted.names.insert(arg.clone());
                }
            }
        }
        wanted
    }
}

/// `rip cod3-locate`: where the wanted textures are in an archive. One line
/// per instance, tab-separated: archive label, section (`main` for the
/// unnamed one), chunk number in the section, the chunk's offset in the
/// file and whether it is packed (NCH/LZO, so the texture's bytes are only
/// at these offsets once the chunk is unpacked, as `cod3-chunks` writes
/// it), the TEX object's offset and the pixel data's offset and size in the
/// unpacked chunk, the size, the Xenos format and the key (`-` for a texture
/// the HD pack can't replace). The game loads chunks to addresses it picks
/// at run time, so the key, not an address, identifies a texture in memory.
pub fn locate(file: &Path, label: &str, wanted: &Wanted) -> Result<String> {
    let bytes = std::fs::read(file)?;
    let archive = kapf::Archive::parse(&bytes)?;
    let mut report = String::new();
    for section in &archive.sections {
        let section_name = if section.name.is_empty() {
            "main"
        } else {
            section.name.as_str()
        };
        for (number, entry) in section.chunks.iter().enumerate() {
            let data = archive.read_chunk(entry)?;
            let Ok(chunk) = Chunk::parse(&data) else {
                continue;
            };
            for item in chunk.items_of("TEX").filter(|i| has_pixels(i)) {
                let (Some(gpu), Some(fetch)) =
                    (chunk.gpu(item), fetch_constant(&chunk, item.object.0))
                else {
                    continue;
                };
                let name = sanitize(&item.name);
                let key = xenos::hd_replaceable(&fetch)
                    .then(|| xenos::base_level_linear(&fetch, gpu).ok())
                    .flatten()
                    .map(|linear| xenos::hd_key(&linear));
                let by_key = key.is_some_and(|k| wanted.keys.contains(&k));
                if !wanted.names.contains(&name) && !by_key {
                    continue;
                }
                let (gpu_at, gpu_len) = item.gpu.unwrap_or_default();
                let _ = writeln!(
                    report,
                    "{label}\t{section_name}\tchunk {number}\tfile offset {:#x}{}\t\
                     object {:#x}\tpixels {gpu_at:#x} ({gpu_len} bytes)\t{name}\t{}x{}\t\
                     format {}\tkey {}",
                    entry.file_offset,
                    if entry.is_packed() { " (packed)" } else { "" },
                    item.object.0,
                    fetch.width,
                    fetch.height,
                    fetch.format.to_xenos(),
                    key.map_or("-".to_string(), |k| format!("{k:016x}")),
                );
            }
        }
    }
    Ok(report)
}

/// A material's parameters as a JSON object (for glTF `extras` and the
/// materials file).
fn material_json(m: &Material) -> String {
    let params: Vec<String> = m
        .params
        .iter()
        .map(|p| {
            let value = match &p.value {
                Value::Int(v) => v.to_string(),
                Value::Float(v) if v.is_finite() => v.to_string(),
                Value::Float(_) => "null".into(),
                Value::Float4(v) => format!(
                    "[{}]",
                    v.iter()
                        .map(|f| if f.is_finite() {
                            f.to_string()
                        } else {
                            "null".into()
                        })
                        .collect::<Vec<_>>()
                        .join(",")
                ),
                Value::Texture(Some(t)) => json_string(&t.name),
                Value::Texture(None) => "null".into(),
                Value::Unknown { type_id, size } => {
                    format!(r#"{{"unknown_type":{type_id},"size":{size}}}"#)
                }
            };
            format!("{}:{value}", json_string(&p.name))
        })
        .collect();
    format!(
        r#"{{"class":{},"effect":{},"params":{{{}}}}}"#,
        json_string(&m.class),
        m.effect
            .as_deref()
            .map(json_string)
            .unwrap_or_else(|| "null".into()),
        params.join(",")
    )
}

/// Writes the archive's meshes as glTF to `<out_dir>/meshes/<label>/`, with
/// their materials and links to `<out_dir>/textures/<name>.png`, and every
/// material and model to `<out_dir>/materials/<label>.json`.
///
/// Texture names the meshes use are added to `used_textures`, so a whole-disc
/// run can report links to textures it never found.
pub fn meshes(
    file: &Path,
    label: &str,
    out_dir: &Path,
    used_textures: &std::sync::Mutex<BTreeSet<String>>,
) -> Result<String> {
    let mesh_dir =
        crate::paths::safe_join(&out_dir.join("meshes"), label).ok_or("bad archive label")?;
    std::fs::create_dir_all(&mesh_dir)?;
    let up = up_to_root(label);
    let (mut written, mut materials_seen) = (0usize, 0usize);
    let mut skipped: BTreeMap<String, usize> = BTreeMap::new();
    let mut materials_json = Vec::new();
    let mut models_json = Vec::new();

    // Texture sets of the whole archive first: a material may name a set
    // from another chunk. glTF gets the set's first texture.
    let mut set_first_texture: BTreeMap<String, String> = BTreeMap::new();
    for_each_chunk(file, |_, chunk| {
        for item in chunk.items_of("TEX") {
            if let Some(set) = texture_set::parse(chunk, item) {
                if let Some(first) = set.textures.first() {
                    set_first_texture.insert(set.name, first.name.clone());
                }
            }
        }
        Ok(())
    })?;

    for_each_chunk(file, |_, chunk| {
        // Materials of this chunk, by item name.
        let materials: BTreeMap<String, Material> = chunk
            .items_of("MAT")
            .map(|item| (item.name.clone(), material::parse(chunk, item)))
            .collect();
        materials_seen += materials.len();
        for m in materials.values() {
            materials_json.push(format!("{}:{}", json_string(&m.name), material_json(m)));
        }
        for item in chunk.items_of("MLTI") {
            let model = model::parse(chunk, item);
            let groups: Vec<String> = model
                .groups
                .iter()
                .map(|g| {
                    let members: Vec<String> = g.members.iter().map(|m| json_string(m)).collect();
                    format!("{}:[{}]", json_string(&g.type_name), members.join(","))
                })
                .collect();
            models_json.push(format!(
                "{}:{{{}}}",
                json_string(&model.name),
                groups.join(",")
            ));
        }

        for item in chunk.items_of("MESH") {
            let mesh = match cod3_mesh::decode(chunk, item) {
                Ok(mesh) => mesh,
                Err(e) => {
                    *skipped.entry(e.to_string()).or_default() += 1;
                    continue;
                }
            };
            let mut gltf_materials = Vec::new();
            for name in mesh.surfaces.iter().filter_map(|s| s.material.as_ref()) {
                if gltf_materials
                    .iter()
                    .any(|g: &GltfMaterial| &g.name == name)
                {
                    continue;
                }
                let Some(m) = materials.get(name) else {
                    continue;
                };
                let uri = |role| {
                    m.texture_for(role).map(|t| {
                        let t = set_first_texture.get(t).map(String::as_str).unwrap_or(t);
                        used_textures
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .insert(t.to_string());
                        format!("{up}textures/{}.png", sanitize(t))
                    })
                };
                gltf_materials.push(GltfMaterial {
                    name: name.clone(),
                    base_color_uri: uri(TextureRole::BaseColor),
                    normal_uri: uri(TextureRole::Normal),
                    extras_json: material_json(m),
                });
            }
            let base = sanitize(&item.name);
            let bin_name = format!("{base}.bin");
            let (json, bin) = cod3_mesh::to_gltf(&mesh, &gltf_materials, &bin_name);
            write_if_changed(&mesh_dir.join(format!("{base}.gltf")), json.as_bytes())?;
            write_if_changed(&mesh_dir.join(bin_name), &bin)?;
            written += 1;
        }
        Ok(())
    })?;

    if !materials_json.is_empty() || !models_json.is_empty() {
        let path = crate::paths::safe_join(&out_dir.join("materials"), &format!("{label}.json"))
            .ok_or("bad archive label")?;
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = format!(
            "{{\"materials\":{{{}}},\n\"models\":{{{}}}}}\n",
            materials_json.join(",\n"),
            models_json.join(",\n")
        );
        write_if_changed(&path, json.as_bytes())?;
    }
    let mut report = format!(
        "{}: {written} meshes, {materials_seen} materials\n",
        file.display()
    );
    for (why, n) in skipped {
        let _ = writeln!(report, "  skipped {n} meshes: {why}");
    }
    Ok(report)
}

/// Where an offset in a chunk lies, for problem reports: inside an item's
/// object or GPU data, just past an item's end, or between items.
fn describe_offset(chunk: &Chunk, at: usize) -> String {
    let mut nearest_end: Option<(usize, String)> = None;
    for t in &chunk.directory.types {
        for item in &t.items {
            let (start, len) = item.object;
            if (start..start + len).contains(&at) {
                return format!("{} {} +{:#x}", t.type_name(), item.name, at - start);
            }
            if let Some((gpu_start, gpu_len)) = item.gpu {
                if (gpu_start..gpu_start + gpu_len).contains(&at) {
                    return format!(
                        "{} {} GPU data +{:#x}",
                        t.type_name(),
                        item.name,
                        at - gpu_start
                    );
                }
            }
            let end = start + len;
            if end <= at && nearest_end.as_ref().map_or(true, |(e, _)| end > *e) {
                nearest_end = Some((end, format!("{} {}", t.type_name(), item.name)));
            }
        }
    }
    match nearest_end {
        Some((end, name)) => format!("{:#x} ({:#x} past the end of {name})", at, at - end),
        None => format!("{at:#x} (before the first item)"),
    }
}

/// Checks the archive against everything the format notes claim, so a run
/// over the whole disc shows where the layout rules don't hold. Returns the
/// report and whether every check passed.
pub fn check(file: &Path) -> Result<(String, bool)> {
    #[derive(Default)]
    struct Count {
        passed: usize,
        total: usize,
    }
    impl Count {
        fn add(&mut self, ok: bool) {
            self.total += 1;
            self.passed += ok as usize;
        }
    }
    let (mut tables, mut tex_headers, mut tex_sizes, mut sets, mut own_names, mut pointers) =
        <(Count, Count, Count, Count, Count, Count)>::default();
    let mut surfaces = Count::default();
    let mut notes = Vec::new();
    let mut problems = Vec::new();
    let mut chunk_index = 0;
    for_each_chunk(file, |section, chunk| {
        let dir = &chunk.directory;
        if section == DYNAMIC_TEXTURE_SECTION {
            notes.push(format!(
                "chunk {chunk_index} ({section}): {:#x} bytes follow the tables",
                chunk.data.len().saturating_sub(dir.end_of_tables)
            ));
        } else {
            tables.add(dir.end_of_tables == chunk.data.len());
        }
        if section != DYNAMIC_TEXTURE_SECTION && dir.end_of_tables != chunk.data.len() {
            problems.push(format!(
                "chunk {chunk_index}: tables end at {:#x}, chunk is {:#x} bytes",
                dir.end_of_tables,
                chunk.data.len()
            ));
        }
        for item in chunk.items_of("TEX").filter(|i| !has_pixels(i)) {
            let ok = texture_set::parse(chunk, item).is_some_and(|s| !s.textures.is_empty());
            sets.add(ok);
            if !ok {
                problems.push(format!(
                    "chunk {chunk_index}: TEX {} has no pixels and isn't a set",
                    item.name
                ));
            }
        }
        for item in chunk.items_of("TEX").filter(|i| has_pixels(i)) {
            let at = item.object.0;
            let ok = chunk.u32_at(at + TEX_D3D_HEADER_OFFSET) == Some(D3D_COMMON_TEXTURE);
            tex_headers.add(ok);
            if !ok {
                problems.push(format!(
                    "chunk {chunk_index}: TEX {} has no D3D header",
                    item.name
                ));
                continue;
            }
            if let Some(fetch) = fetch_constant(chunk, at) {
                tex_sizes.add(
                    chunk.u32_at(at + TEX_WIDTH_OFFSET) == Some(fetch.width)
                        && chunk.u32_at(at + TEX_HEIGHT_OFFSET) == Some(fetch.height),
                );
            }
        }
        for type_name in ["MAT", "MESH"] {
            for item in chunk.items_of(type_name) {
                let ok = chunk.link(item.object.0) == Link::Name(item.name.clone());
                own_names.add(ok);
                if !ok {
                    problems.push(format!(
                        "chunk {chunk_index}: {type_name} {} doesn't start with its name",
                        item.name
                    ));
                }
            }
        }
        for &field in &dir.pointer_fields {
            if let Link::Local(target) = chunk.link(field) {
                let ok = chunk.item_at(target).is_some();
                pointers.add(ok);
                if !ok {
                    problems.push(format!(
                        "chunk {chunk_index}: pointer in {} -> {}",
                        describe_offset(chunk, field),
                        describe_offset(chunk, target)
                    ));
                }
            }
        }
        for item in chunk.items_of("MESH") {
            let ok = cod3_mesh::decode(chunk, item).is_ok();
            surfaces.add(ok);
            if !ok {
                problems.push(format!(
                    "chunk {chunk_index}: MESH {} doesn't decode",
                    item.name
                ));
            }
        }
        chunk_index += 1;
        Ok(())
    })?;
    let all = [
        &tables,
        &tex_headers,
        &tex_sizes,
        &sets,
        &own_names,
        &pointers,
        &surfaces,
    ];
    let ok = all.iter().all(|c| c.passed == c.total);
    let mut report = format!(
        "{}: {} — {chunk_index} chunks\n",
        file.display(),
        if ok { "OK" } else { "PROBLEMS" }
    );
    for (what, c) in [
        ("tables end at the chunk end", &tables),
        ("texture D3D headers", &tex_headers),
        ("texture sizes match the fetch constant", &tex_sizes),
        ("texture sets read", &sets),
        ("materials/meshes start with their name", &own_names),
        ("pointers land inside an item", &pointers),
        ("meshes decode", &surfaces),
    ] {
        let _ = writeln!(report, "  {what}: {}/{}", c.passed, c.total);
    }
    for n in &notes {
        let _ = writeln!(report, "  note: {n}");
    }
    for p in problems.iter().take(10) {
        let _ = writeln!(report, "  ! {p}");
    }
    if problems.len() > 10 {
        let _ = writeln!(report, "  ! ... {} more", problems.len() - 10);
    }
    Ok((report, ok))
}
