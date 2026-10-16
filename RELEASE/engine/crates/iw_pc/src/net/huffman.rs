//! Huffman coding, as the games compress their messages: Quake III's
//! adaptive Huffman tree (`huffman.c`'s algorithm, ported here from the
//! games' machine code), used two ways.
//!
//! * **The message tree** ([`MessageTree`]): built once from a table of byte
//!   frequencies (the same 256 numbers as Quake III's, read from the
//!   player's executable by [`protocol_data`](super::protocol_data)) by
//!   adding every byte to an adaptive tree as many times as its frequency
//!   says, then never changed. Unlike Quake III, both games compress a whole
//!   message in one go after writing it uncompressed
//!   ([`MessageTree::write_bits_compress`]) and decompress it whole on
//!   arrival ([`MessageTree::read_bits_compress`]).
//! * **United Offensive's adaptive coder** ([`compress_adaptive`],
//!   [`decompress_adaptive`]): a fresh tree that learns as it codes, used for
//!   the `connect` packet only (Call of Duty 2 sends `connect` uncompressed).
//!
//! Bits are written lowest bit first within a byte; a code is written from
//! the root of the tree down (a 1 bit for a right branch). A byte the coder
//! starts writing is cleared first; bits after the last one written keep
//! what was there.
//!
//! The tree is Quake III's structure with indices for pointers: up to 768
//! nodes, a doubly linked list of the nodes by weight, and one shared slot
//! per block of equal weights holding the block's first node. The two games'
//! tree code is the same machine code (`CoD2MP_s.exe` 0x0043CA80-0x0043CEC0
//! and `CoDUOMP.exe` 0x00441630-0x00441A70 instruction for instruction).
//!
//! # Memory past the buffers
//!
//! The originals don't bound their reads and writes: a code that runs past
//! the end of the input reads whatever memory follows (zeros here), and the
//! output is as long as the input makes it. Here reads past the input give
//! zeros and writes past the output are dropped; the functions still return
//! the length the original would have produced.

use super::msg::Msg;

/// Quake III's `NYT` ("not yet transmitted"): the leaf that stands for every
/// symbol not in an adaptive tree yet. Coding it is followed by the symbol's
/// 8 bits.
pub const NYT: u16 = 256;
/// Quake III's `INTERNAL_NODE`: the symbol of a node that isn't a leaf.
const INTERNAL: u16 = 257;
/// No node (the original's NULL).
const NONE: u16 = u16::MAX;
/// Nodes in a tree (Quake III's `nodeList`): a root, NYT and two per symbol fit.
const MAX_NODES: usize = 768;
/// Symbols: the 256 byte values and NYT.
const SYMBOLS: usize = 257;
/// The size of United Offensive's work buffer for the adaptive coder (on
/// its stack): the longest compressed or decompressed message.
const ADAPTIVE_BUFFER: usize = 0x10000;

/// A node of the tree (Quake III's `node_t`, indices for pointers).
#[derive(Debug, Clone, Copy)]
struct Node {
    left: u16,
    right: u16,
    parent: u16,
    /// The next node in the weight list (heavier or equal).
    next: u16,
    /// The previous node in the weight list.
    prev: u16,
    /// The slot holding the first node of this node's block of equal weights.
    head: u16,
    weight: u32,
    symbol: u16,
}

const ZERO_NODE: Node = Node {
    left: NONE,
    right: NONE,
    parent: NONE,
    next: NONE,
    prev: NONE,
    head: NONE,
    weight: 0,
    symbol: 0,
};

/// An adaptive Huffman tree (Quake III's `huff_t`): starts with the NYT leaf
/// only, and every [`add_ref`](Self::add_ref) of a symbol updates the codes.
/// About 17 KB, no allocation.
#[derive(Clone)]
pub struct HuffmanTree {
    nodes: [Node; MAX_NODES],
    /// `nodePtrs`: each slot holds the first node of a block of equal
    /// weights; a free slot holds the next free slot.
    slots: [u16; MAX_NODES],
    /// `blocNode`: nodes handed out.
    used_nodes: usize,
    /// `blocPtrs`: slots handed out (before any was freed).
    used_slots: usize,
    /// `freelist`: the first free slot.
    free_slot: u16,
    /// `tree`: the root.
    root: u16,
    /// `lhead`: the lightest node, always NYT.
    lhead: u16,
    /// `loc`: each symbol's leaf.
    loc: [u16; SYMBOLS],
}

impl Default for HuffmanTree {
    fn default() -> Self {
        Self::new()
    }
}

/// Byte `index` of `bytes`, or 0 past the end.
#[inline]
fn byte_at(bytes: &[u8], index: usize) -> u8 {
    bytes.get(index).copied().unwrap_or(0)
}

/// Quake III's `get_bit`: the bit at bit position `bit`.
#[inline]
fn get_bit(input: &[u8], bit: usize) -> u8 {
    (byte_at(input, bit >> 3) >> (bit & 7)) & 1
}

/// Quake III's `add_bit`: writes one bit at position `*bit` (a byte is
/// cleared when its first bit is written) and moves on.
#[inline]
fn add_bit(out: &mut [u8], bit: &mut usize, value: u8) {
    if let Some(byte) = out.get_mut(*bit >> 3) {
        if *bit & 7 == 0 {
            *byte = 0;
        }
        *byte |= (value & 1) << (*bit & 7);
    }
    *bit += 1;
}

impl HuffmanTree {
    /// A tree holding only NYT, its root.
    ///
    /// Original: `Huff_Init` at 0x0043D4E0 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_Init` at 0x004420F0 (CoDUOMP.exe 1.51)
    pub fn new() -> HuffmanTree {
        let mut tree = HuffmanTree {
            nodes: [ZERO_NODE; MAX_NODES],
            slots: [NONE; MAX_NODES],
            used_nodes: 1,
            used_slots: 0,
            free_slot: NONE,
            root: 0,
            lhead: 0,
            loc: [NONE; SYMBOLS],
        };
        tree.nodes[0].symbol = NYT;
        tree.loc[usize::from(NYT)] = 0;
        tree
    }

    fn node(&self, index: u16) -> &Node {
        &self.nodes[usize::from(index)]
    }

    fn node_mut(&mut self, index: u16) -> &mut Node {
        &mut self.nodes[usize::from(index)]
    }

    /// Quake III's `get_ppnode`: a free slot.
    fn take_slot(&mut self) -> u16 {
        if self.free_slot == NONE {
            let slot = self.used_slots as u16;
            self.used_slots += 1;
            slot
        } else {
            let slot = self.free_slot;
            self.free_slot = self.slots[usize::from(slot)];
            slot
        }
    }

    /// Quake III's `free_ppnode`.
    fn free_slot(&mut self, slot: u16) {
        self.slots[usize::from(slot)] = self.free_slot;
        self.free_slot = slot;
    }

    /// A new slot holding `node`, as `node`'s block head.
    fn new_head(&mut self, node: u16) {
        let slot = self.take_slot();
        self.node_mut(node).head = slot;
        self.slots[usize::from(slot)] = node;
    }

    /// Quake III's `swap`: exchanges two subtrees' places in the tree (in
    /// this order, so two siblings stay where they are).
    ///
    /// Original: `Huff_swap` at 0x0043CB30 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_swap` at 0x004416E0 (CoDUOMP.exe 1.51)
    fn swap(&mut self, node1: u16, node2: u16) {
        let parent1 = self.node(node1).parent;
        let parent2 = self.node(node2).parent;
        if parent1 != NONE {
            if self.node(parent1).left == node1 {
                self.node_mut(parent1).left = node2;
            } else {
                self.node_mut(parent1).right = node2;
            }
        } else {
            self.root = node2;
        }
        if parent2 != NONE {
            if self.node(parent2).left == node2 {
                self.node_mut(parent2).left = node1;
            } else {
                self.node_mut(parent2).right = node1;
            }
        } else {
            self.root = node1;
        }
        self.node_mut(node1).parent = parent2;
        self.node_mut(node2).parent = parent1;
    }

    /// Quake III's `swaplist`: exchanges two nodes' places in the weight list.
    ///
    /// Original: `Huff_swaplist` at 0x0043CB80 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_swaplist` at 0x00441730 (CoDUOMP.exe 1.51)
    fn swap_list(&mut self, node1: u16, node2: u16) {
        let next1 = self.node(node1).next;
        self.node_mut(node1).next = self.node(node2).next;
        self.node_mut(node2).next = next1;
        let prev1 = self.node(node1).prev;
        self.node_mut(node1).prev = self.node(node2).prev;
        self.node_mut(node2).prev = prev1;
        if self.node(node1).next == node1 {
            self.node_mut(node1).next = node2;
        }
        if self.node(node2).next == node2 {
            self.node_mut(node2).next = node1;
        }
        let (next1, next2) = (self.node(node1).next, self.node(node2).next);
        if next1 != NONE {
            self.node_mut(next1).prev = node1;
        }
        if next2 != NONE {
            self.node_mut(next2).prev = node2;
        }
        let (prev1, prev2) = (self.node(node1).prev, self.node(node2).prev);
        if prev1 != NONE {
            self.node_mut(prev1).next = node1;
        }
        if prev2 != NONE {
            self.node_mut(prev2).next = node2;
        }
    }

    /// Quake III's `increment`: adds one to a node's weight, keeping the
    /// tree a Huffman tree (the node moves to the end of its weight block
    /// first), then does the same for its parent. Recursive, as the
    /// original; the depth is the tree's height.
    ///
    /// Original: `Huff_increment` at 0x0043CBE0 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_increment` at 0x00441790 (CoDUOMP.exe 1.51)
    fn increment(&mut self, node: u16) {
        if node == NONE {
            return;
        }
        let next = self.node(node).next;
        if next != NONE && self.node(next).weight == self.node(node).weight {
            let leader = self.slots[usize::from(self.node(node).head)];
            if leader != self.node(node).parent {
                self.swap(leader, node);
            }
            self.swap_list(leader, node);
        }
        let prev = self.node(node).prev;
        let head = self.node(node).head;
        if prev != NONE && self.node(prev).weight == self.node(node).weight {
            self.slots[usize::from(head)] = prev;
        } else {
            self.free_slot(head);
        }
        self.node_mut(node).weight += 1;
        let next = self.node(node).next;
        if next != NONE && self.node(next).weight == self.node(node).weight {
            self.node_mut(node).head = self.node(next).head;
        } else {
            self.new_head(node);
        }
        let parent = self.node(node).parent;
        if parent != NONE {
            self.increment(parent);
            let parent = self.node(node).parent;
            if self.node(node).prev == parent {
                self.swap_list(node, parent);
                let head = usize::from(self.node(node).head);
                if self.slots[head] == node {
                    self.slots[head] = parent;
                }
            }
        }
    }

    /// Counts one more `symbol`: a symbol already in the tree gets heavier;
    /// a new one splits the NYT leaf into a node holding NYT and the
    /// symbol's new leaf.
    ///
    /// Original: `Huff_addRef` at 0x0043CCC0 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_addRef` at 0x00441870 (CoDUOMP.exe 1.51)
    pub fn add_ref(&mut self, symbol: u8) {
        let leaf = self.loc[usize::from(symbol)];
        if leaf != NONE {
            self.increment(leaf);
            return;
        }
        let lhead = self.lhead;
        let internal = (self.used_nodes + 1) as u16;
        let new_leaf = self.used_nodes as u16;
        self.used_nodes += 2;

        // The new internal node goes into the weight list after NYT.
        let after = self.node(lhead).next;
        {
            let node = self.node_mut(internal);
            node.symbol = INTERNAL;
            node.weight = 1;
            node.next = after;
        }
        if after != NONE {
            self.node_mut(after).prev = internal;
            if self.node(after).weight == 1 {
                self.node_mut(internal).head = self.node(after).head;
            } else {
                self.new_head(internal);
            }
        } else {
            self.new_head(internal);
        }
        self.node_mut(lhead).next = internal;
        self.node_mut(internal).prev = lhead;

        // Then the new leaf, between NYT and the internal node.
        let after = self.node(lhead).next;
        {
            let node = self.node_mut(new_leaf);
            node.symbol = u16::from(symbol);
            node.weight = 1;
            node.next = after;
        }
        if after != NONE {
            self.node_mut(after).prev = new_leaf;
            if self.node(after).weight == 1 {
                self.node_mut(new_leaf).head = self.node(after).head;
            } else {
                // Never reached (`after` is the internal node, weight 1). The
                // original stores the internal node in the new slot here.
                let slot = self.take_slot();
                self.node_mut(new_leaf).head = slot;
                self.slots[usize::from(slot)] = internal;
            }
        } else {
            self.new_head(new_leaf);
        }
        self.node_mut(lhead).next = new_leaf;
        self.node_mut(new_leaf).prev = lhead;
        self.node_mut(new_leaf).left = NONE;
        self.node_mut(new_leaf).right = NONE;

        // The internal node takes NYT's place in the tree: NYT on its left,
        // the new leaf on its right.
        let parent = self.node(lhead).parent;
        if parent != NONE {
            if self.node(parent).left == lhead {
                self.node_mut(parent).left = internal;
            } else {
                self.node_mut(parent).right = internal;
            }
        } else {
            self.root = internal;
        }
        {
            let node = self.node_mut(internal);
            node.right = new_leaf;
            node.left = lhead;
            node.parent = parent;
        }
        self.node_mut(lhead).parent = internal;
        self.node_mut(new_leaf).parent = internal;
        self.loc[usize::from(symbol)] = new_leaf;
        self.increment(parent);
    }

    /// Whether `symbol` (0 to 256, NYT included) has a leaf.
    pub fn contains(&self, symbol: u16) -> bool {
        self.loc
            .get(usize::from(symbol))
            .is_some_and(|&leaf| leaf != NONE)
    }

    /// Quake III's `send(node, NULL)`: writes `node`'s code, the path from
    /// the root, at `*bit`. Linear in the code's length: the length is
    /// counted first, then the bits are placed from the leaf up.
    fn write_code(&self, node: u16, out: &mut [u8], bit: &mut usize) {
        let mut length = 0;
        let mut walk = node;
        while self.node(walk).parent != NONE {
            length += 1;
            walk = self.node(walk).parent;
        }
        let start = *bit;
        // As add_bit does: every byte the code starts is cleared.
        let mut boundary = start.next_multiple_of(8);
        while boundary < start + length {
            if let Some(byte) = out.get_mut(boundary >> 3) {
                *byte = 0;
            }
            boundary += 8;
        }
        let mut position = start + length;
        let mut walk = node;
        while self.node(walk).parent != NONE {
            position -= 1;
            let parent = self.node(walk).parent;
            if self.node(parent).right == walk {
                if let Some(byte) = out.get_mut(position >> 3) {
                    *byte |= 1 << (position & 7);
                }
            }
            walk = parent;
        }
        *bit = start + length;
    }

    /// Writes `symbol`'s code at `*bit`; a symbol not in the tree yet is
    /// NYT's code followed by its 8 bits, highest first.
    ///
    /// Original: `Huff_transmit` at 0x0043CFC0 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_transmit` at 0x00441B70 (CoDUOMP.exe 1.51)
    pub fn transmit(&self, symbol: u16, out: &mut [u8], bit: &mut usize) {
        match self.loc.get(usize::from(symbol)) {
            Some(&leaf) if leaf != NONE => self.write_code(leaf, out, bit),
            _ => {
                self.write_code(self.loc[usize::from(NYT)], out, bit);
                for shift in (0..8).rev() {
                    add_bit(out, bit, (symbol >> shift) as u8);
                }
            }
        }
    }

    /// Reads one code at `*bit`, from the root down, and returns its symbol
    /// (NYT is 256); `None` when a branch is missing (never in a tree built
    /// by [`add_ref`](Self::add_ref)), `*bit` then past the bits read.
    ///
    /// Original: `Huff_Receive` at 0x0043CE60 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_Receive` at 0x00441A10 (CoDUOMP.exe 1.51)
    pub fn receive(&self, input: &[u8], bit: &mut usize) -> Option<u16> {
        let mut node = self.root;
        while node != NONE && self.node(node).symbol == INTERNAL {
            let branch = get_bit(input, *bit);
            *bit += 1;
            node = if branch != 0 {
                self.node(node).right
            } else {
                self.node(node).left
            };
        }
        (node != NONE).then(|| self.node(node).symbol)
    }
}

/// The message tree: built once from the byte frequencies, then used for
/// every message of a connection (both games, both directions).
#[derive(Clone)]
pub struct MessageTree {
    tree: HuffmanTree,
}

impl MessageTree {
    /// Builds the tree the games build at start: every byte value, 0 to 255
    /// in order, added `frequencies[value]` times (a count of 0 or less adds
    /// nothing). The games build a compressor and a decompressor this way;
    /// they come out identical, so one tree serves both. About a million
    /// steps for the games' table: do it once.
    ///
    /// Original: `MSG_initHuffman` at 0x00447670 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_initHuffman` at 0x0044CF80 (CoDUOMP.exe 1.51)
    pub fn from_frequencies(frequencies: &[i32; 256]) -> MessageTree {
        let mut tree = HuffmanTree::new();
        for (symbol, &count) in frequencies.iter().enumerate() {
            for _ in 0..count.max(0) {
                tree.add_ref(symbol as u8);
            }
        }
        MessageTree { tree }
    }

    /// The tree itself (for [`HuffmanTree::transmit`] and
    /// [`HuffmanTree::receive`]).
    pub fn tree(&self) -> &HuffmanTree {
        &self.tree
    }

    /// Writes `symbol`'s code (0 to 256: every byte and NYT are in the
    /// message tree) at `*offset`, a bit position in `out`, and moves
    /// `*offset` past it. A symbol without a leaf (only in a tree built from
    /// zero counts) writes nothing (the original would fail).
    ///
    /// Original: `Huff_offsetTransmit` at 0x0043D160 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_offsetTransmit` at 0x00441D10 (CoDUOMP.exe 1.51)
    pub fn offset_transmit(&self, symbol: u16, out: &mut [u8], offset: &mut usize) {
        if let Some(&leaf) = self.tree.loc.get(usize::from(symbol)) {
            if leaf != NONE {
                self.tree.write_code(leaf, out, offset);
            }
        }
    }

    /// Reads one code at `*offset` and returns its symbol (NYT's code
    /// gives 256), moving `*offset` past it. A missing branch (never in a
    /// tree built from the games' table) gives 0 and leaves `*offset`.
    ///
    /// Original: `Huff_offsetReceive` at 0x0043CEC0 (CoD2MP_s.exe 1.3)
    /// Original: `Huff_offsetReceive` at 0x00441A70 (CoDUOMP.exe 1.51)
    pub fn offset_receive(&self, input: &[u8], offset: &mut usize) -> u16 {
        let mut bit = *offset;
        match self.tree.receive(input, &mut bit) {
            Some(symbol) => {
                *offset = bit;
                symbol
            }
            None => 0,
        }
    }

    /// Compresses a whole message: every byte's code, one after the other
    /// from bit 0 of `out`. Returns the bytes used (the bits rounded up); the
    /// last byte's unused high bits are 0. `out` needs at most as many bytes
    /// as the longest code has bits times `input.len()`, over 8, plus one.
    ///
    /// Original: `MSG_WriteBitsCompress` at 0x00444000 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_WriteBitsCompress` at 0x00449780 (CoDUOMP.exe 1.51)
    pub fn write_bits_compress(&self, input: &[u8], out: &mut [u8]) -> usize {
        let mut bit = 0;
        for &byte in input {
            self.tree
                .write_code(self.tree.loc[usize::from(byte)], out, &mut bit);
        }
        (bit + 7) >> 3
    }

    /// Decompresses a whole message: decodes symbols while the bit position
    /// is inside `input`, so the padding bits at the end usually decode into
    /// one more byte (a code that runs past the end reads zeros). NYT's code
    /// decodes as a 0 byte. Returns the number of bytes decoded; bytes past
    /// the end of `out` are dropped (the original has no limit: give `out`
    /// 8 bytes per input byte to be sure).
    ///
    /// Original: `MSG_ReadBitsCompress` at 0x00444050 (CoD2MP_s.exe 1.3)
    /// Original: `MSG_ReadBitsCompress` at 0x004497D0 (CoDUOMP.exe 1.51)
    pub fn read_bits_compress(&self, input: &[u8], out: &mut [u8]) -> usize {
        let bits = input.len() * 8;
        let mut bit = 0;
        let mut count = 0;
        while bit < bits {
            let start = bit;
            let symbol = self.offset_receive(input, &mut bit);
            if let Some(slot) = out.get_mut(count) {
                *slot = symbol as u8;
            }
            count += 1;
            if bit == start {
                break; // a tree without codes (zero counts): the original never ends
            }
        }
        count
    }
}

/// Compresses `msg`'s bytes from `offset` to `cursize` in place with a fresh
/// adaptive tree: two bytes of length (high byte first), each byte's code
/// (a new byte as NYT and its 8 bits) learned as it goes, and one more byte.
/// `cursize` becomes the compressed end. Nothing happens when `offset` is
/// at or past `cursize`.
///
/// The original doesn't check `maxsize` (the output can be longer than the
/// input) and copies one byte past its last code; when the codes end on a
/// byte boundary, that byte is whatever its stack held, 0 here. Messages up
/// to 64 KB compressed are handled (the original's work buffer); bytes
/// beyond are dropped. The work buffer and the tree (about 80 KB) are on the
/// stack: nothing is allocated.
///
/// Original: `Huff_Compress` at 0x00441FA0 (CoDUOMP.exe 1.51)
pub fn compress_adaptive(msg: &mut Msg, offset: usize) {
    if offset >= msg.cursize {
        return;
    }
    let size = msg.cursize - offset;
    let mut tree = HuffmanTree::new();
    let mut work = [0u8; ADAPTIVE_BUFFER];
    work[0] = (size >> 8) as u8;
    work[1] = size as u8;
    let mut bit = 16;
    for i in offset..msg.cursize {
        let byte = byte_at(msg.data, i);
        tree.transmit(u16::from(byte), &mut work, &mut bit);
        tree.add_ref(byte);
    }
    bit += 8;
    let length = bit >> 3;
    msg.cursize = offset + length;
    copy_into(msg.data, offset, &work[..length.min(ADAPTIVE_BUFFER)]);
}

/// Decompresses what [`compress_adaptive`] wrote, in place: reads the
/// length (cut to what fits between `offset` and `maxsize`), then decodes
/// that many bytes with a fresh adaptive tree. Decoding stops early when the
/// position passes the end of the compressed bytes; the byte there and the
/// rest of the length read as 0 (the original leaves its stack's old bytes
/// there). `cursize` becomes `offset` plus the length. Nothing happens when
/// `offset` is at or past `cursize`.
///
/// United Offensive runs this on any packet starting with `connect`, from
/// byte 12 (after `\xFF\xFF\xFF\xFFconnect `).
///
/// Original: `Huff_Decompress` at 0x00441D40 (CoDUOMP.exe 1.51)
pub fn decompress_adaptive(msg: &mut Msg, offset: usize) {
    if offset >= msg.cursize {
        return;
    }
    let size = msg.cursize - offset;
    let mut tree = HuffmanTree::new();
    let mut work = [0u8; ADAPTIVE_BUFFER];
    let input = msg.data.get(offset..).unwrap_or(&[]);
    let declared = usize::from(byte_at(input, 0)) * 256 + usize::from(byte_at(input, 1));
    let length = declared.min(msg.maxsize.saturating_sub(offset));
    let mut bit = 16;
    for slot in work.iter_mut().take(length) {
        if bit >> 3 > size {
            *slot = 0;
            break;
        }
        let mut symbol = tree.receive(input, &mut bit).unwrap_or(0);
        if symbol == NYT {
            symbol = 0;
            for _ in 0..8 {
                symbol = (symbol << 1) | u16::from(get_bit(input, bit));
                bit += 1;
            }
        }
        *slot = symbol as u8;
        tree.add_ref(symbol as u8);
    }
    msg.cursize = offset + length;
    copy_into(msg.data, offset, &work[..length]);
}

/// Copies `bytes` to `data[at..]`, as far as `data` goes.
fn copy_into(data: &mut [u8], at: usize, bytes: &[u8]) {
    let end = data.len().min(at.saturating_add(bytes.len()));
    if at < end {
        data[at..end].copy_from_slice(&bytes[..end - at]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Game;

    /// Frequencies with the shape of a message table (zeros and small
    /// numbers common, every byte present), made up for the tests.
    fn made_up_frequencies() -> [i32; 256] {
        let mut frequencies = [0i32; 256];
        for (i, f) in frequencies.iter_mut().enumerate() {
            *f = 1 + ((i as i32 * 7919) % 97) + if i < 16 { 400 - 20 * i as i32 } else { 0 };
        }
        frequencies
    }

    #[test]
    fn message_tree_round_trips() {
        let tree = MessageTree::from_frequencies(&made_up_frequencies());
        for symbol in 0..=NYT {
            assert!(tree.tree().contains(symbol));
            let mut out = [0u8; 8];
            let mut bit = 3;
            tree.offset_transmit(symbol, &mut out, &mut bit);
            let mut back = 3;
            assert_eq!(tree.offset_receive(&out, &mut back), symbol);
            assert_eq!(back, bit);
        }
        let message: Vec<u8> = (0..300u32).map(|i| (i * i % 251) as u8).collect();
        let mut packed = [0u8; 1200];
        let n = tree.write_bits_compress(&message, &mut packed);
        let mut unpacked = [0u8; 2400];
        let m = tree.read_bits_compress(&packed[..n], &mut unpacked);
        assert!(m >= message.len());
        assert_eq!(&unpacked[..message.len()], &message[..]);
    }

    #[test]
    fn codes_follow_the_weights() {
        // The heaviest symbol gets the shortest code.
        let mut frequencies = [1i32; 256];
        frequencies[b'e' as usize] = 1000;
        let tree = MessageTree::from_frequencies(&frequencies);
        let length = |symbol: u16| {
            let mut out = [0u8; 8];
            let mut bit = 0;
            tree.offset_transmit(symbol, &mut out, &mut bit);
            bit
        };
        assert!((0..256).all(|s| length(s) >= length(u16::from(b'e'))));
        assert!(length(u16::from(b'e')) <= 2);
    }

    #[test]
    fn adaptive_round_trip() {
        let text = b"\xFF\xFF\xFF\xFFconnect \"\\cg_predictItems\\1\\name\\Player\\rate\\25000\\protocol\\22\"";
        let mut buffer = [0u8; 400];
        buffer[..text.len()].copy_from_slice(text);
        let mut msg = Msg::new(Game::UnitedOffensive, &mut buffer);
        msg.cursize = text.len();
        compress_adaptive(&mut msg, 12);
        assert_ne!(&msg.data[12..text.len()], &text[12..]);
        let compressed = msg.cursize;
        decompress_adaptive(&mut msg, 12);
        assert_eq!(msg.cursize, text.len());
        assert_eq!(&msg.data[..text.len()], &text[..]);
        assert!(compressed > 12);
    }

    /// The games' own trees, when `IW_PC_INPUT` names the folder(s) holding
    /// the executables: both build the same tree, quickly enough, and every
    /// code is short.
    #[test]
    fn message_tree_of_the_games() {
        use crate::net::protocol_data::{find_executable, GameExecutable};
        let Some(folders) = std::env::var_os("IW_PC_INPUT") else {
            eprintln!("IW_PC_INPUT is not set: skipping the games' message trees");
            return;
        };
        let mut lengths = Vec::new();
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let Some(path) = find_executable(game, &folders) else {
                continue;
            };
            let frequencies = GameExecutable::open(game, &path)
                .unwrap()
                .huffman_frequencies()
                .unwrap();
            let started = std::time::Instant::now();
            let tree = MessageTree::from_frequencies(&frequencies);
            eprintln!("{game:?}: message tree built in {:?}", started.elapsed());
            let code_lengths: Vec<usize> = (0..=NYT)
                .map(|symbol| {
                    let mut out = [0u8; 8];
                    let mut bit = 0;
                    tree.offset_transmit(symbol, &mut out, &mut bit);
                    bit
                })
                .collect();
            eprintln!(
                "{game:?}: codes of {} to {} bits",
                code_lengths.iter().min().unwrap(),
                code_lengths.iter().max().unwrap()
            );
            lengths.push(code_lengths);
        }
        if lengths.len() == 2 {
            assert_eq!(lengths[0], lengths[1]);
        }
    }

    #[test]
    fn first_symbols_are_spelled_out() {
        // An empty tree's root is NYT itself: no code bits, then the 8 bits.
        let tree = HuffmanTree::new();
        let mut out = [0u8; 2];
        let mut bit = 0;
        tree.transmit(0xA5, &mut out, &mut bit);
        assert_eq!((bit, out[0]), (8, 0b1010_0101u8.reverse_bits()));
        let mut read = 0;
        assert_eq!(tree.receive(&out, &mut read), Some(NYT));
        assert_eq!(read, 0);
    }
}
