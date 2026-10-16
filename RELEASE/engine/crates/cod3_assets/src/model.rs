//! Models (APKF type `MLTI`, version 100; the `.xemesh` items).
//!
//! A model lists the resources that make it up, in groups by type:
//!
//! ```text
//! +0x00  u32 group count         2 (MAT, MESH) or 3 (MAT, MESH, SKEL)
//! +0x04  pointer                 the group table
//! group table entries, 12 bytes:
//!   +0  u32 type                 four-character code, byte-reversed
//!   +4  u32 count
//!   +8  pointer                  the members: 8 bytes each,
//!                                a name reference and a pointer to the item
//! ```
//!
//! The meshes of a model are its levels of detail. Each MESH also lists the
//! lower levels and their switch distances itself (see cod3_mesh).

use crate::linked_name;
use kapf::apkf::{four_cc_name, Chunk, Item};

const GROUP_COUNT_OFFSET: usize = 0x00;
const GROUP_TABLE_OFFSET: usize = 0x04;
const GROUP_ENTRY_SIZE: usize = 12;
const MEMBER_SIZE: usize = 8;
/// More groups or members than any retail model has.
const MAX_GROUPS: u32 = 16;
const MAX_MEMBERS: u32 = 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Group {
    /// "MAT", "MESH", "SKEL".
    pub type_name: String,
    /// Item names, as the APKF directory names them.
    pub members: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Model {
    pub name: String,
    pub groups: Vec<Group>,
}

impl Model {
    pub fn members_of(&self, type_name: &str) -> &[String] {
        self.groups
            .iter()
            .find(|g| g.type_name == type_name)
            .map(|g| g.members.as_slice())
            .unwrap_or(&[])
    }
}

pub fn parse(chunk: &Chunk, item: &Item) -> Model {
    let at = item.object.0;
    let mut model = Model {
        name: item.name.clone(),
        groups: Vec::new(),
    };
    let (Some(count), Some(table)) = (
        chunk.u32_at(at + GROUP_COUNT_OFFSET),
        chunk.pointer(at + GROUP_TABLE_OFFSET),
    ) else {
        return model;
    };
    for g in 0..count.min(MAX_GROUPS) as usize {
        let entry = table + g * GROUP_ENTRY_SIZE;
        let (Some(type_code), Some(member_count)) = (chunk.u32_at(entry), chunk.u32_at(entry + 4))
        else {
            break;
        };
        let Some(members_at) = chunk.pointer(entry + 8) else {
            continue;
        };
        let members = (0..member_count.min(MAX_MEMBERS) as usize)
            .filter_map(|m| linked_name(chunk, members_at + m * MEMBER_SIZE + 4))
            .collect();
        model.groups.push(Group {
            type_name: four_cc_name(type_code),
            members,
        });
    }
    model
}
