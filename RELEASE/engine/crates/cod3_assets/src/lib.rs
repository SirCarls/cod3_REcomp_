//! Call of Duty 3 resource objects that tie the others together:
//!
//! - [`material`]: materials (APKF type `MAT`): a shader effect plus its
//!   parameters, including the textures;
//! - [`model`]: models (APKF type `MLTI`, the `.xemesh` files): which
//!   materials, meshes and skeleton make up one model;
//! - [`texture_set`]: TEX items that list interchangeable textures.
//!
//! All read their object data through [`kapf::apkf::Chunk`], which follows
//! pointers inside the chunk and names imported from other chunks. Layouts
//! are in docs/formats/cod3.md.

#![forbid(unsafe_code)]

pub mod material;
pub mod model;
pub mod texture_set;

use kapf::apkf::{name_hash, Chunk, Link};

/// The name of whatever a field links to: the item at a local pointer, an
/// imported resource's name, or a name record.
pub fn linked_name(chunk: &Chunk, field: usize) -> Option<String> {
    match chunk.link(field) {
        Link::Local(at) => chunk.item_at(at).map(|(_, item)| item.name.clone()),
        Link::Import { name, .. } | Link::Name(name) => Some(name),
        Link::None => None,
    }
}

/// A texture a material or a texture set refers to: by name as the files
/// show it (cut to 27 characters) and by the hash of the full name, which is
/// what tells `brick$us_dirty_broken_wall_c` from `..._n` when both are cut
/// to `brick$us_dirty_broken_wall_`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TextureRef {
    pub name: String,
    pub hash: u32,
}

/// The texture a field links to: a TEX item in this chunk, or one imported
/// by name hash from another.
pub fn linked_texture(chunk: &Chunk, field: usize) -> Option<TextureRef> {
    match chunk.link(field) {
        Link::Local(at) => chunk.item_at(at).map(|(_, item)| TextureRef {
            name: item.name.clone(),
            hash: item.name_hash,
        }),
        Link::Import {
            name, name_hash, ..
        } => Some(TextureRef {
            name,
            hash: name_hash,
        }),
        // A name record without a hash of its own: hashed like the game does.
        Link::Name(name) => Some(TextureRef {
            hash: name_hash(&name),
            name,
        }),
        Link::None => None,
    }
}
