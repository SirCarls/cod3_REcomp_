//! Call of Duty 3 meshes (APKF type `MESH`, version 2) to glTF 2.0.
//!
//! Object data (offsets from the object start; "pointer" fields are followed
//! with [`kapf::apkf::Chunk`]):
//!
//! ```text
//! +0x00  name reference           "<mesh>.<model>"
//! +0x08  u32 surface count
//! +0x0C  pointer                  surface table, 8 bytes per surface:
//!                                   u32 0, pointer to the surface
//! +0x14  u32 lower LOD count
//! +0x18  pointer                  LOD table, 8 bytes per level:
//!                                   pointer to the MESH, f32 switch distance
//! +0x20  f32[4]                   bounding sphere (center, radius)
//! ```
//!
//! A surface is one draw with one material:
//!
//! ```text
//! +0x00  f32[4] bounding sphere, +0x10 f32[3] box min, +0x20 f32[3] box max
//! +0x34  pointer                  the material (MAT, same chunk)
//! +0x38  pointer, u32             bone palette and its size (skinned meshes)
//! +0x40  pointer                  D3D vertex buffer header
//! +0x48  u32 vertex count
//! +0x50  pointer                  D3D index buffer header
//! +0x58  u32 index count          16-bit, triangle list
//! +0x60  pointer                  vertex declaration: u32 stride, pointer to
//!                                 the elements
//! ```
//!
//! The D3D headers start with Common (`0x00200001` vertex buffer,
//! `0x20200002` index buffer) and hold, at +0x18, the vertex fetch constant
//! (dword0: offset in the mesh's GPU data, dword1 bits 2-25: size in dwords)
//! or the index data's offset and size. All GPU data is big-endian.
//!
//! Vertex declarations are Xbox 360 D3DVERTEXELEMENT9 arrays (12 bytes: u16
//! stream, u16 offset, u32 type, u8 method, usage, usage index, pad), ended by
//! stream 0x00FF. Types seen (Xbox 360 D3DDECLTYPE values):
//!
//! | value        | type      | used for                       |
//! |--------------|-----------|--------------------------------|
//! | `0x002A23B9` | FLOAT3    | position                       |
//! | `0x002A2187` | DEC3N     | normal, tangent, binormal      |
//! | `0x002C23A5` | FLOAT2    | texture coordinates            |
//! | `0x00182886` | D3DCOLOR  | vertex color                   |
//! | `0x001A2286` | UBYTE4    | blend indices                  |
//! | `0x001A2086` | UBYTE4N   | blend weights                  |
//!
//! Checked on 162 meshes (236 surfaces): every vertex count times the stride
//! equals the vertex buffer size, and every index count times 2 equals the
//! index buffer size.
//!
//! Output: positions, normals and the first UV set; Y-up (CoD3 is Z-up).
//! Skinning and further UV sets come later.

#![forbid(unsafe_code)]

use kapf::apkf::{Chunk, Item, Link};
use std::fmt;

const SURFACE_COUNT_OFFSET: usize = 0x08;
const SURFACE_TABLE_OFFSET: usize = 0x0C;
const SURFACE_ENTRY_SIZE: usize = 8;
const LOD_COUNT_OFFSET: usize = 0x14;
const LOD_TABLE_OFFSET: usize = 0x18;
const LOD_ENTRY_SIZE: usize = 8;

const SURFACE_MATERIAL_OFFSET: usize = 0x34;
const SURFACE_VERTEX_BUFFER_OFFSET: usize = 0x40;
const SURFACE_VERTEX_COUNT_OFFSET: usize = 0x48;
const SURFACE_INDEX_BUFFER_OFFSET: usize = 0x50;
const SURFACE_INDEX_COUNT_OFFSET: usize = 0x58;
const SURFACE_DECLARATION_OFFSET: usize = 0x60;

const VERTEX_BUFFER_COMMON: u32 = 0x0020_0001;
const INDEX_BUFFER_COMMON: u32 = 0x2020_0002;
/// The fetch constant / data address follows Common, ReferenceCount, Fence,
/// ReadFence, Identifier and BaseFlush.
const D3D_HEADER_DATA_OFFSET: usize = 0x18;

const DECL_ELEMENT_SIZE: usize = 12;
/// Stream value of the D3DDECL_END element.
const DECL_END_STREAM: u32 = 0x00FF;
const MAX_DECL_ELEMENTS: usize = 32;
/// More surfaces or LODs than any retail mesh has (the most seen is 7).
const MAX_SURFACES: u32 = 256;
const MAX_LODS: u32 = 16;

const DECL_FLOAT3: u32 = 0x002A_23B9;
const DECL_DEC3N: u32 = 0x002A_2187;
const DECL_FLOAT2: u32 = 0x002C_23A5;

const USAGE_POSITION: u8 = 0;
const USAGE_NORMAL: u8 = 3;
const USAGE_TEXCOORD: u8 = 5;

#[derive(Debug)]
pub enum Error {
    NoSurfaces,
    /// A pointer the layout needs is missing or points outside the chunk.
    BadPointer(&'static str),
    /// A D3D header does not start with the expected Common value.
    BadHeader(&'static str),
    Truncated(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NoSurfaces => write!(f, "the mesh has no surfaces"),
            Error::BadPointer(what) => write!(f, "missing or bad pointer to the {what}"),
            Error::BadHeader(what) => write!(f, "unexpected {what} header"),
            Error::Truncated(what) => write!(f, "mesh data ends inside the {what}"),
        }
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy)]
struct Element {
    offset: usize,
    decl_type: u32,
    usage: u8,
    usage_index: u8,
}

/// One draw: vertices, triangle-list indices and the material it uses.
#[derive(Debug, Default, Clone)]
pub struct Surface {
    /// The MAT item's name (same chunk), if the surface has one.
    pub material: Option<String>,
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
}

#[derive(Debug, Default, Clone)]
pub struct Mesh {
    pub name: String,
    pub surfaces: Vec<Surface>,
    /// Lower levels of detail: (MESH item name, switch distance).
    pub lods: Vec<(String, f32)>,
}

fn be32(d: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(d[at..at + 4].try_into().unwrap())
}

fn f32_be(d: &[u8], at: usize) -> f32 {
    f32::from_bits(be32(d, at))
}

/// DEC3N: three signed 10-bit fields, normalized to [-1, 1].
fn dec3n(v: u32) -> [f32; 3] {
    let field = |shift: u32| {
        let raw = ((v >> shift) & 0x3FF) as i32;
        let signed = if raw & 0x200 != 0 { raw - 0x400 } else { raw };
        (signed as f32 / 511.0).max(-1.0)
    };
    [field(0), field(10), field(20)]
}

/// CoD3 is Z-up; glTF is Y-up.
fn to_y_up(p: [f32; 3]) -> [f32; 3] {
    [p[0], p[2], -p[1]]
}

fn item_name(chunk: &Chunk, at: usize) -> Option<String> {
    chunk.item_at(at).map(|(_, item)| item.name.clone())
}

/// Reads the vertex declaration that `decl` (stride, pointer) describes.
fn declaration(chunk: &Chunk, decl: usize) -> Result<(usize, Vec<Element>), Error> {
    let stride = chunk
        .u32_at(decl)
        .ok_or(Error::Truncated("vertex declaration"))? as usize;
    let mut at = chunk
        .pointer(decl + 4)
        .ok_or(Error::BadPointer("vertex elements"))?;
    let mut elements = Vec::new();
    while elements.len() < MAX_DECL_ELEMENTS {
        let head = chunk
            .u32_at(at)
            .ok_or(Error::Truncated("vertex declaration"))?;
        if head >> 16 == DECL_END_STREAM {
            break;
        }
        let packed = chunk.u32_at(at + 8).unwrap_or(0).to_be_bytes();
        elements.push(Element {
            offset: (head & 0xFFFF) as usize,
            decl_type: chunk.u32_at(at + 4).unwrap_or(0),
            usage: packed[1],
            usage_index: packed[2],
        });
        at += DECL_ELEMENT_SIZE;
    }
    Ok((stride, elements))
}

/// (offset, size) of the data a D3D buffer header describes, in the mesh's
/// GPU data.
fn buffer_range(chunk: &Chunk, header: usize, is_vertex: bool) -> Result<(usize, usize), Error> {
    let (common, what) = if is_vertex {
        (VERTEX_BUFFER_COMMON, "vertex buffer")
    } else {
        (INDEX_BUFFER_COMMON, "index buffer")
    };
    if chunk.u32_at(header) != Some(common) {
        return Err(Error::BadHeader(what));
    }
    let word0 = chunk
        .u32_at(header + D3D_HEADER_DATA_OFFSET)
        .ok_or(Error::Truncated(what))?;
    let word1 = chunk
        .u32_at(header + D3D_HEADER_DATA_OFFSET + 4)
        .ok_or(Error::Truncated(what))?;
    Ok(if is_vertex {
        // Vertex fetch constant: address in dwords (low 2 bits are the
        // type), size in dwords in bits 2-25.
        (
            (word0 & !3) as usize,
            (((word1 >> 2) & 0x00FF_FFFF) * 4) as usize,
        )
    } else {
        (word0 as usize, word1 as usize)
    })
}

fn decode_surface(chunk: &Chunk, surface: usize, gpu: &[u8]) -> Result<Surface, Error> {
    let pointer = |offset: usize, what| {
        chunk
            .pointer(surface + offset)
            .ok_or(Error::BadPointer(what))
    };
    let material = match chunk.link(surface + SURFACE_MATERIAL_OFFSET) {
        Link::Local(at) => item_name(chunk, at),
        Link::Import { name, .. } => Some(name),
        _ => None,
    };
    let (stride, elements) =
        declaration(chunk, pointer(SURFACE_DECLARATION_OFFSET, "declaration")?)?;
    let (vb_at, vb_len) = buffer_range(
        chunk,
        pointer(SURFACE_VERTEX_BUFFER_OFFSET, "vertex buffer")?,
        true,
    )?;
    let (ib_at, ib_len) = buffer_range(
        chunk,
        pointer(SURFACE_INDEX_BUFFER_OFFSET, "index buffer")?,
        false,
    )?;
    let vertex_count = chunk
        .u32_at(surface + SURFACE_VERTEX_COUNT_OFFSET)
        .unwrap_or(0) as usize;
    let index_count = chunk
        .u32_at(surface + SURFACE_INDEX_COUNT_OFFSET)
        .unwrap_or(0) as usize;

    let vb = gpu
        .get(vb_at..vb_at + vb_len)
        .ok_or(Error::Truncated("vertex buffer"))?;
    let ib = gpu
        .get(ib_at..ib_at + ib_len)
        .ok_or(Error::Truncated("index buffer"))?;
    let vertex_count = vertex_count.min(vb.len().checked_div(stride).unwrap_or(0));
    let index_count = index_count.min(ib.len() / 2) / 3 * 3;

    let find = |usage: u8, ty: u32| {
        elements
            .iter()
            .find(|e| e.usage == usage && e.usage_index == 0 && e.decl_type == ty)
            .copied()
    };
    let position = find(USAGE_POSITION, DECL_FLOAT3);
    let normal = find(USAGE_NORMAL, DECL_DEC3N);
    let uv = find(USAGE_TEXCOORD, DECL_FLOAT2);

    let mut out = Surface {
        material,
        ..Default::default()
    };
    for v in 0..vertex_count {
        let base = v * stride;
        if let Some(e) = position {
            let o = base + e.offset;
            out.positions.push(to_y_up([
                f32_be(vb, o),
                f32_be(vb, o + 4),
                f32_be(vb, o + 8),
            ]));
        }
        if let Some(e) = normal {
            out.normals.push(to_y_up(dec3n(be32(vb, base + e.offset))));
        }
        if let Some(e) = uv {
            let o = base + e.offset;
            out.uvs.push([f32_be(vb, o), f32_be(vb, o + 4)]);
        }
    }
    out.indices = ib[..index_count * 2]
        .chunks_exact(2)
        .map(|c| u16::from_be_bytes([c[0], c[1]]) as u32)
        .collect();
    if out.indices.iter().any(|&i| i as usize >= vertex_count) {
        return Err(Error::Truncated(
            "index buffer (index past the last vertex)",
        ));
    }
    Ok(out)
}

/// Decodes the MESH `item` of `chunk`.
pub fn decode(chunk: &Chunk, item: &Item) -> Result<Mesh, Error> {
    let at = item.object.0;
    let gpu = chunk.gpu(item).ok_or(Error::Truncated("GPU data"))?;
    let count = chunk.u32_at(at + SURFACE_COUNT_OFFSET).unwrap_or(0);
    if count == 0 {
        return Err(Error::NoSurfaces);
    }
    let table = chunk
        .pointer(at + SURFACE_TABLE_OFFSET)
        .ok_or(Error::BadPointer("surface table"))?;
    let mut mesh = Mesh {
        name: item.name.clone(),
        ..Default::default()
    };
    for i in 0..count.min(MAX_SURFACES) as usize {
        let surface = chunk
            .pointer(table + i * SURFACE_ENTRY_SIZE + 4)
            .ok_or(Error::BadPointer("surface"))?;
        mesh.surfaces.push(decode_surface(chunk, surface, gpu)?);
    }
    let lod_count = chunk.u32_at(at + LOD_COUNT_OFFSET).unwrap_or(0);
    if let Some(lod_table) = chunk.pointer(at + LOD_TABLE_OFFSET) {
        for i in 0..lod_count.min(MAX_LODS) as usize {
            let entry = lod_table + i * LOD_ENTRY_SIZE;
            if let Some(name) = chunk.pointer(entry).and_then(|p| item_name(chunk, p)) {
                mesh.lods
                    .push((name, chunk.f32_at(entry + 4).unwrap_or(0.0)));
            }
        }
    }
    Ok(mesh)
}

/// A glTF material: names of the image files for its textures, relative to
/// the .gltf file, and the original parameters as JSON for `extras`.
#[derive(Debug, Default, Clone)]
pub struct GltfMaterial {
    pub name: String,
    pub base_color_uri: Option<String>,
    pub normal_uri: Option<String>,
    /// A JSON object, written as the material's `extras` (may be empty).
    pub extras_json: String,
}

/// JSON string escaping for names written into the glTF file.
pub fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// glTF 2.0 constants.
const GLTF_ARRAY_BUFFER: u32 = 34962;
const GLTF_ELEMENT_ARRAY_BUFFER: u32 = 34963;
const GLTF_FLOAT: u32 = 5126;
const GLTF_UNSIGNED_INT: u32 = 5125;
const GLTF_TRIANGLES: u32 = 4;

/// A glTF 2.0 file (`.gltf` JSON plus a `.bin` buffer). `materials` maps a
/// MAT item name to its glTF material; surfaces whose material is not in it
/// get none.
pub fn to_gltf(mesh: &Mesh, materials: &[GltfMaterial], bin_file_name: &str) -> (String, Vec<u8>) {
    let mut bin = Vec::new();
    let mut views = Vec::new();
    let mut accessors = Vec::new();
    let mut primitives = Vec::new();

    let mut push_view = |bin: &mut Vec<u8>, bytes: &[u8], target: u32| -> usize {
        while bin.len() % 4 != 0 {
            bin.push(0);
        }
        let offset = bin.len();
        bin.extend_from_slice(bytes);
        views.push(format!(
            r#"{{"buffer":0,"byteOffset":{offset},"byteLength":{},"target":{target}}}"#,
            bytes.len()
        ));
        views.len() - 1
    };
    let float_bytes = |v: &[f32]| v.iter().flat_map(|f| f.to_le_bytes()).collect::<Vec<u8>>();

    for surface in &mesh.surfaces {
        if surface.positions.is_empty() || surface.indices.is_empty() {
            continue;
        }
        let mut attributes = Vec::new();

        let (mut min, mut max) = ([f32::MAX; 3], [f32::MIN; 3]);
        for p in &surface.positions {
            for k in 0..3 {
                min[k] = min[k].min(p[k]);
                max[k] = max[k].max(p[k]);
            }
        }
        let flat: Vec<f32> = surface.positions.iter().flatten().copied().collect();
        let view = push_view(&mut bin, &float_bytes(&flat), GLTF_ARRAY_BUFFER);
        accessors.push(format!(
            r#"{{"bufferView":{view},"componentType":{GLTF_FLOAT},"count":{},"type":"VEC3","min":[{},{},{}],"max":[{},{},{}]}}"#,
            surface.positions.len(), min[0], min[1], min[2], max[0], max[1], max[2]
        ));
        attributes.push(format!(r#""POSITION":{}"#, accessors.len() - 1));

        if surface.normals.len() == surface.positions.len() {
            let flat: Vec<f32> = surface.normals.iter().flatten().copied().collect();
            let view = push_view(&mut bin, &float_bytes(&flat), GLTF_ARRAY_BUFFER);
            accessors.push(format!(
                r#"{{"bufferView":{view},"componentType":{GLTF_FLOAT},"count":{},"type":"VEC3"}}"#,
                surface.normals.len()
            ));
            attributes.push(format!(r#""NORMAL":{}"#, accessors.len() - 1));
        }
        if surface.uvs.len() == surface.positions.len() {
            let flat: Vec<f32> = surface.uvs.iter().flatten().copied().collect();
            let view = push_view(&mut bin, &float_bytes(&flat), GLTF_ARRAY_BUFFER);
            accessors.push(format!(
                r#"{{"bufferView":{view},"componentType":{GLTF_FLOAT},"count":{},"type":"VEC2"}}"#,
                surface.uvs.len()
            ));
            attributes.push(format!(r#""TEXCOORD_0":{}"#, accessors.len() - 1));
        }
        let index_bytes: Vec<u8> = surface
            .indices
            .iter()
            .flat_map(|i| i.to_le_bytes())
            .collect();
        let view = push_view(&mut bin, &index_bytes, GLTF_ELEMENT_ARRAY_BUFFER);
        accessors.push(format!(
            r#"{{"bufferView":{view},"componentType":{GLTF_UNSIGNED_INT},"count":{},"type":"SCALAR"}}"#,
            surface.indices.len()
        ));
        let material = surface
            .material
            .as_ref()
            .and_then(|m| materials.iter().position(|g| &g.name == m))
            .map(|i| format!(r#","material":{i}"#))
            .unwrap_or_default();
        primitives.push(format!(
            r#"{{"attributes":{{{}}},"indices":{}{material},"mode":{GLTF_TRIANGLES}}}"#,
            attributes.join(","),
            accessors.len() - 1
        ));
    }

    // Images, textures and materials. Each distinct image file is listed once.
    let mut images: Vec<String> = Vec::new();
    let mut texture_index = |uri: &str| -> usize {
        match images.iter().position(|u| u == uri) {
            Some(i) => i,
            None => {
                images.push(uri.to_string());
                images.len() - 1
            }
        }
    };
    let mut material_json = Vec::new();
    for m in materials {
        let mut pbr = vec![r#""metallicFactor":0"#.to_string()];
        if let Some(uri) = &m.base_color_uri {
            pbr.push(format!(
                r#""baseColorTexture":{{"index":{}}}"#,
                texture_index(uri)
            ));
        }
        let mut fields = vec![
            format!(r#""name":{}"#, json_string(&m.name)),
            format!(r#""pbrMetallicRoughness":{{{}}}"#, pbr.join(",")),
        ];
        if let Some(uri) = &m.normal_uri {
            fields.push(format!(
                r#""normalTexture":{{"index":{}}}"#,
                texture_index(uri)
            ));
        }
        if !m.extras_json.is_empty() {
            fields.push(format!(r#""extras":{}"#, m.extras_json));
        }
        material_json.push(format!("{{{}}}", fields.join(",")));
    }
    let images_json: Vec<String> = images
        .iter()
        .map(|uri| format!(r#"{{"uri":{}}}"#, json_string(uri)))
        .collect();
    let textures_json: Vec<String> = (0..images.len())
        .map(|i| format!(r#"{{"source":{i}}}"#))
        .collect();

    let name = json_string(&mesh.name);
    let mut json = format!(
        r#"{{"asset":{{"version":"2.0","generator":"cod3-waw rip"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0,"name":{name}}}],"meshes":[{{"name":{name},"primitives":[{}]}}],"buffers":[{{"uri":{},"byteLength":{}}}],"bufferViews":[{}],"accessors":[{}]"#,
        primitives.join(","),
        json_string(bin_file_name),
        bin.len(),
        views.join(","),
        accessors.join(",")
    );
    if !material_json.is_empty() {
        json.push_str(&format!(r#","materials":[{}]"#, material_json.join(",")));
    }
    if !images_json.is_empty() {
        json.push_str(&format!(
            r#","images":[{}],"textures":[{}]"#,
            images_json.join(","),
            textures_json.join(",")
        ));
    }
    json.push('}');
    (json, bin)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dec3n_extremes() {
        assert_eq!(dec3n(0x1FF), [1.0, 0.0, 0.0]);
        assert_eq!(dec3n(0x201 << 10), [0.0, -1.0, 0.0]);
    }

    #[test]
    fn json_escaping() {
        assert_eq!(json_string(r#"a"b\c"#), r#""a\"b\\c""#);
    }

    #[test]
    fn gltf_lists_each_image_once() {
        let surface = Surface {
            material: Some("m".into()),
            positions: vec![[0.0; 3], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
            indices: vec![0, 1, 2],
            ..Default::default()
        };
        let mesh = Mesh {
            name: "x".into(),
            surfaces: vec![surface],
            lods: vec![],
        };
        let material = GltfMaterial {
            name: "m".into(),
            base_color_uri: Some("t.png".into()),
            normal_uri: Some("t.png".into()),
            extras_json: String::new(),
        };
        let (json, bin) = to_gltf(&mesh, &[material], "x.bin");
        assert_eq!(json.matches(r#""uri":"t.png""#).count(), 1);
        assert!(json.contains(r#""material":0"#));
        assert_eq!(bin.len(), 36 + 12);
    }
}
