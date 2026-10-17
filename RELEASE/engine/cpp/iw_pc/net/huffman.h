// Huffman coding, as the games compress their messages: Quake III's adaptive
// Huffman tree (huffman.c's algorithm, ported from the games' machine code),
// used two ways:
// * The message tree (MessageTree): built once from the byte frequencies (read
//   from the player's executable, net/protocol_data.h) and never changed;
//   both games compress a whole message at once after writing it.
// * United Offensive's adaptive coder (compress_adaptive, decompress_adaptive),
//   used for the connect packet only.
// Bits are written lowest first within a byte; a code from the root down (1
// for a right branch). A byte the coder starts is cleared first. Reads past
// the input give zeros; writes past the output are dropped.
#pragma once

#include <array>

#include "iw_pc/base.h"

namespace iw_pc::net {

class Msg;

/// Quake III's NYT ("not yet transmitted") symbol.
inline constexpr std::uint16_t kNyt = 256;

/// An adaptive Huffman tree (Quake III's huff_t, indices for pointers).
class HuffmanTree {
public:
    /// A tree holding only NYT, its root.
    /// Original: Huff_Init at 0x0043D4E0 (CoD2MP_s.exe 1.3)
    /// Original: Huff_Init at 0x004420F0 (CoDUOMP.exe 1.51)
    HuffmanTree();

    /// Counts one more `symbol`: a symbol already in the tree gets heavier;
    /// a new one splits the NYT leaf.
    /// Original: Huff_addRef at 0x0043CCC0 (CoD2MP_s.exe 1.3)
    /// Original: Huff_addRef at 0x00441870 (CoDUOMP.exe 1.51)
    void add_ref(std::uint8_t symbol);

    /// Whether `symbol` (0 to 256) has a leaf.
    bool contains(std::uint16_t symbol) const;

    /// Writes `symbol`'s code at *bit; a symbol not in the tree yet is NYT's
    /// code followed by its 8 bits, highest first.
    /// Original: Huff_transmit at 0x0043CFC0 (CoD2MP_s.exe 1.3)
    /// Original: Huff_transmit at 0x00441B70 (CoDUOMP.exe 1.51)
    void transmit(std::uint16_t symbol, ByteSpan out, std::size_t& bit) const;

    /// Reads one code at *bit and returns its symbol (NYT is 256); nullopt
    /// when a branch is missing (never in a tree built by add_ref()).
    /// Original: Huff_Receive at 0x0043CE60 (CoD2MP_s.exe 1.3)
    /// Original: Huff_Receive at 0x00441A10 (CoDUOMP.exe 1.51)
    std::optional<std::uint16_t> receive(ByteView input, std::size_t& bit) const;

private:
    friend class MessageTree;
    static constexpr std::size_t kMaxNodes = 768;
    static constexpr std::size_t kSymbols = 257;

    struct Node {
        std::uint16_t left, right, parent;
        std::uint16_t next, prev;  // the weight list (next: heavier or equal)
        std::uint16_t head;        // the slot holding the first node of this weight block
        std::uint32_t weight;
        std::uint16_t symbol;
    };

    std::array<Node, kMaxNodes> nodes_;
    std::array<std::uint16_t, kMaxNodes> slots_;  // nodePtrs: block heads, or the next free slot
    std::size_t used_nodes_;                      // blocNode
    std::size_t used_slots_;                      // blocPtrs
    std::uint16_t free_slot_;                     // freelist
    std::uint16_t root_;                          // tree
    std::uint16_t lhead_;                         // the lightest node, always NYT
    std::array<std::uint16_t, kSymbols> loc_;     // each symbol's leaf

    std::uint16_t take_slot();
    void free_slot(std::uint16_t slot);
    void new_head(std::uint16_t node);
    /// Original: Huff_swap at 0x0043CB30 (CoD2MP_s.exe 1.3)
    /// Original: Huff_swap at 0x004416E0 (CoDUOMP.exe 1.51)
    void swap(std::uint16_t node1, std::uint16_t node2);
    /// Original: Huff_swaplist at 0x0043CB80 (CoD2MP_s.exe 1.3)
    /// Original: Huff_swaplist at 0x00441730 (CoDUOMP.exe 1.51)
    void swap_list(std::uint16_t node1, std::uint16_t node2);
    /// Original: Huff_increment at 0x0043CBE0 (CoD2MP_s.exe 1.3)
    /// Original: Huff_increment at 0x00441790 (CoDUOMP.exe 1.51)
    void increment(std::uint16_t node);
    void write_code(std::uint16_t node, ByteSpan out, std::size_t& bit) const;
};

/// The message tree: built once from the byte frequencies, then used for
/// every message of a connection (both games, both directions).
class MessageTree {
public:
    /// Every byte value, 0 to 255 in order, added frequencies[value] times
    /// (a count of 0 or less adds nothing). About a million steps: do it once.
    /// Original: MSG_initHuffman at 0x00447670 (CoD2MP_s.exe 1.3)
    /// Original: MSG_initHuffman at 0x0044CF80 (CoDUOMP.exe 1.51)
    explicit MessageTree(const std::array<std::int32_t, 256>& frequencies);

    const HuffmanTree& tree() const { return tree_; }

    /// Writes `symbol`'s code at *offset (a symbol without a leaf: nothing).
    /// Original: Huff_offsetTransmit at 0x0043D160 (CoD2MP_s.exe 1.3)
    /// Original: Huff_offsetTransmit at 0x00441D10 (CoDUOMP.exe 1.51)
    void offset_transmit(std::uint16_t symbol, ByteSpan out, std::size_t& offset) const;

    /// Reads one code at *offset and returns its symbol (NYT gives 256); a
    /// missing branch gives 0 and leaves *offset.
    /// Original: Huff_offsetReceive at 0x0043CEC0 (CoD2MP_s.exe 1.3)
    /// Original: Huff_offsetReceive at 0x00441A70 (CoDUOMP.exe 1.51)
    std::uint16_t offset_receive(ByteView input, std::size_t& offset) const;

    /// Compresses a whole message from bit 0 of `out`; returns the bytes used.
    /// Original: MSG_WriteBitsCompress at 0x00444000 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteBitsCompress at 0x00449780 (CoDUOMP.exe 1.51)
    std::size_t write_bits_compress(ByteView input, ByteSpan out) const;

    /// Decompresses a whole message: decodes symbols while the bit position
    /// is inside `input` (so the padding usually decodes into one more byte);
    /// NYT decodes as 0. Returns the number decoded (bytes past `out` dropped).
    /// Original: MSG_ReadBitsCompress at 0x00444050 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadBitsCompress at 0x004497D0 (CoDUOMP.exe 1.51)
    std::size_t read_bits_compress(ByteView input, ByteSpan out) const;

private:
    HuffmanTree tree_;
};

/// Compresses msg's bytes from `offset` to cursize in place with a fresh
/// adaptive tree: two length bytes (high first), the codes, one more byte.
/// Original: Huff_Compress at 0x00441FA0 (CoDUOMP.exe 1.51)
void compress_adaptive(Msg& msg, std::size_t offset);

/// Decompresses what compress_adaptive() wrote, in place. United Offensive
/// runs this on any packet starting with "connect", from byte 12.
/// Original: Huff_Decompress at 0x00441D40 (CoDUOMP.exe 1.51)
void decompress_adaptive(Msg& msg, std::size_t offset);

}  // namespace iw_pc::net
