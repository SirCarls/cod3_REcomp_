//! Texture sets: TEX items without GPU data that list other textures.
//!
//! Character archives use them for interchangeable heads and bodies
//! (`c_uspriv_ng_head_text` lists `c_uspriv_head1_c`, `c_uspriv_head2_c`,
//! ...). A material names the set; the game picks one of its textures.
//!
//! Object data (offsets from the object start):
//!
//! ```text
//! +0x00  zeros                  (width and height in a real texture)
//! +0x10  name reference         the set's own name
//! +0x18  u32 kind               4 for a set (0x4000 in a real texture)
//! +0x24  u32 count
//! +0x28  pointer                the list: one field per texture, each an
//!                               import by name or a pointer to a local TEX
//! ```

use crate::{linked_texture, TextureRef};
use kapf::apkf::{Chunk, Item};

const KIND_OFFSET: usize = 0x18;
const COUNT_OFFSET: usize = 0x24;
const LIST_OFFSET: usize = 0x28;
/// Value of the kind field in a texture set.
const KIND_SET: u32 = 4;
/// More textures than any retail set has (the most seen is 6).
const MAX_TEXTURES: u32 = 64;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TextureSet {
    pub name: String,
    /// The textures, in the order the set lists them.
    pub textures: Vec<TextureRef>,
}

/// Reads `item` as a texture set. Returns `None` for a real texture (one
/// with GPU data) or anything else that doesn't look like a set.
pub fn parse(chunk: &Chunk, item: &Item) -> Option<TextureSet> {
    if item.gpu.is_some_and(|(_, size)| size > 0) {
        return None;
    }
    let at = item.object.0;
    if chunk.u32_at(at + KIND_OFFSET)? != KIND_SET {
        return None;
    }
    let count = chunk.u32_at(at + COUNT_OFFSET)?.min(MAX_TEXTURES) as usize;
    let list = chunk.pointer(at + LIST_OFFSET)?;
    Some(TextureSet {
        name: item.name.clone(),
        textures: (0..count)
            .filter_map(|i| linked_texture(chunk, list + 4 * i))
            .collect(),
    })
}
