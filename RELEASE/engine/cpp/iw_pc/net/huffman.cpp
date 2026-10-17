#include "iw_pc/net/huffman.h"

#include <cstring>
#include <vector>

#include "iw_pc/net/msg.h"

namespace iw_pc::net {

namespace {

constexpr std::uint16_t kInternal = 257;  // the symbol of a node that isn't a leaf
constexpr std::uint16_t kNone = 0xFFFF;   // no node (the original's NULL)
constexpr std::size_t kAdaptiveBuffer = 0x10000;  // United Offensive's work buffer

std::uint8_t byte_at(ByteView bytes, std::size_t index) { return index < bytes.size() ? bytes[index] : 0; }

// Quake III's get_bit.
std::uint8_t get_bit(ByteView input, std::size_t bit) { return (byte_at(input, bit >> 3) >> (bit & 7)) & 1; }

// Quake III's add_bit: a byte is cleared when its first bit is written.
void add_bit(ByteSpan out, std::size_t& bit, std::uint8_t value) {
    std::size_t at = bit >> 3;
    if (at < out.size()) {
        if ((bit & 7) == 0) out[at] = 0;
        out[at] = static_cast<std::uint8_t>(out[at] | ((value & 1) << (bit & 7)));
    }
    ++bit;
}

void copy_into(ByteSpan data, std::size_t at, ByteView bytes) {
    if (at >= data.size()) return;
    std::size_t n = std::min(bytes.size(), data.size() - at);
    if (n > 0) std::memmove(data.data() + at, bytes.data(), n);
}

}  // namespace

HuffmanTree::HuffmanTree() {
    nodes_.fill(Node{kNone, kNone, kNone, kNone, kNone, kNone, 0, 0});
    slots_.fill(kNone);
    used_nodes_ = 1;
    used_slots_ = 0;
    free_slot_ = kNone;
    root_ = 0;
    lhead_ = 0;
    loc_.fill(kNone);
    nodes_[0].symbol = kNyt;
    loc_[kNyt] = 0;
}

std::uint16_t HuffmanTree::take_slot() {
    if (free_slot_ == kNone) return static_cast<std::uint16_t>(used_slots_++);
    std::uint16_t slot = free_slot_;
    free_slot_ = slots_[slot];
    return slot;
}

void HuffmanTree::free_slot(std::uint16_t slot) {
    slots_[slot] = free_slot_;
    free_slot_ = slot;
}

void HuffmanTree::new_head(std::uint16_t node) {
    std::uint16_t slot = take_slot();
    nodes_[node].head = slot;
    slots_[slot] = node;
}

void HuffmanTree::swap(std::uint16_t node1, std::uint16_t node2) {
    std::uint16_t parent1 = nodes_[node1].parent;
    std::uint16_t parent2 = nodes_[node2].parent;
    if (parent1 != kNone) {
        if (nodes_[parent1].left == node1) nodes_[parent1].left = node2;
        else nodes_[parent1].right = node2;
    } else {
        root_ = node2;
    }
    if (parent2 != kNone) {
        if (nodes_[parent2].left == node2) nodes_[parent2].left = node1;
        else nodes_[parent2].right = node1;
    } else {
        root_ = node1;
    }
    nodes_[node1].parent = parent2;
    nodes_[node2].parent = parent1;
}

void HuffmanTree::swap_list(std::uint16_t node1, std::uint16_t node2) {
    Node& a = nodes_[node1];
    Node& b = nodes_[node2];
    std::swap(a.next, b.next);
    std::swap(a.prev, b.prev);
    if (a.next == node1) a.next = node2;
    if (b.next == node2) b.next = node1;
    if (a.next != kNone) nodes_[a.next].prev = node1;
    if (b.next != kNone) nodes_[b.next].prev = node2;
    if (a.prev != kNone) nodes_[a.prev].next = node1;
    if (b.prev != kNone) nodes_[b.prev].next = node2;
}

void HuffmanTree::increment(std::uint16_t node) {
    if (node == kNone) return;
    std::uint16_t next = nodes_[node].next;
    if (next != kNone && nodes_[next].weight == nodes_[node].weight) {
        std::uint16_t leader = slots_[nodes_[node].head];
        if (leader != nodes_[node].parent) swap(leader, node);
        swap_list(leader, node);
    }
    std::uint16_t prev = nodes_[node].prev;
    std::uint16_t head = nodes_[node].head;
    if (prev != kNone && nodes_[prev].weight == nodes_[node].weight) slots_[head] = prev;
    else free_slot(head);
    ++nodes_[node].weight;
    next = nodes_[node].next;
    if (next != kNone && nodes_[next].weight == nodes_[node].weight) nodes_[node].head = nodes_[next].head;
    else new_head(node);
    std::uint16_t parent = nodes_[node].parent;
    if (parent != kNone) {
        increment(parent);
        parent = nodes_[node].parent;
        if (nodes_[node].prev == parent) {
            swap_list(node, parent);
            std::uint16_t h = nodes_[node].head;
            if (slots_[h] == node) slots_[h] = parent;
        }
    }
}

void HuffmanTree::add_ref(std::uint8_t symbol) {
    std::uint16_t leaf = loc_[symbol];
    if (leaf != kNone) {
        increment(leaf);
        return;
    }
    std::uint16_t lhead = lhead_;
    auto internal = static_cast<std::uint16_t>(used_nodes_ + 1);
    auto new_leaf = static_cast<std::uint16_t>(used_nodes_);
    used_nodes_ += 2;

    // The new internal node goes into the weight list after NYT.
    std::uint16_t after = nodes_[lhead].next;
    nodes_[internal].symbol = kInternal;
    nodes_[internal].weight = 1;
    nodes_[internal].next = after;
    if (after != kNone) {
        nodes_[after].prev = internal;
        if (nodes_[after].weight == 1) nodes_[internal].head = nodes_[after].head;
        else new_head(internal);
    } else {
        new_head(internal);
    }
    nodes_[lhead].next = internal;
    nodes_[internal].prev = lhead;

    // Then the new leaf, between NYT and the internal node.
    after = nodes_[lhead].next;
    nodes_[new_leaf].symbol = symbol;
    nodes_[new_leaf].weight = 1;
    nodes_[new_leaf].next = after;
    if (after != kNone) {
        nodes_[after].prev = new_leaf;
        if (nodes_[after].weight == 1) {
            nodes_[new_leaf].head = nodes_[after].head;
        } else {
            // Never reached (`after` is the internal node, weight 1). The
            // original stores the internal node in the new slot here.
            std::uint16_t slot = take_slot();
            nodes_[new_leaf].head = slot;
            slots_[slot] = internal;
        }
    } else {
        new_head(new_leaf);
    }
    nodes_[lhead].next = new_leaf;
    nodes_[new_leaf].prev = lhead;
    nodes_[new_leaf].left = kNone;
    nodes_[new_leaf].right = kNone;

    // The internal node takes NYT's place: NYT on its left, the leaf on its right.
    std::uint16_t parent = nodes_[lhead].parent;
    if (parent != kNone) {
        if (nodes_[parent].left == lhead) nodes_[parent].left = internal;
        else nodes_[parent].right = internal;
    } else {
        root_ = internal;
    }
    nodes_[internal].right = new_leaf;
    nodes_[internal].left = lhead;
    nodes_[internal].parent = parent;
    nodes_[lhead].parent = internal;
    nodes_[new_leaf].parent = internal;
    loc_[symbol] = new_leaf;
    increment(parent);
}

bool HuffmanTree::contains(std::uint16_t symbol) const { return symbol < kSymbols && loc_[symbol] != kNone; }

// Quake III's send(node, NULL): the path from the root, at *bit. The length
// is counted first, then the bits are placed from the leaf up.
void HuffmanTree::write_code(std::uint16_t node, ByteSpan out, std::size_t& bit) const {
    std::size_t length = 0;
    for (std::uint16_t walk = node; nodes_[walk].parent != kNone; walk = nodes_[walk].parent) ++length;
    std::size_t start = bit;
    // As add_bit does: every byte the code starts is cleared.
    for (std::size_t boundary = (start + 7) / 8 * 8; boundary < start + length; boundary += 8) {
        if ((boundary >> 3) < out.size()) out[boundary >> 3] = 0;
    }
    std::size_t position = start + length;
    for (std::uint16_t walk = node; nodes_[walk].parent != kNone;) {
        --position;
        std::uint16_t parent = nodes_[walk].parent;
        if (nodes_[parent].right == walk && (position >> 3) < out.size()) {
            out[position >> 3] = static_cast<std::uint8_t>(out[position >> 3] | (1u << (position & 7)));
        }
        walk = parent;
    }
    bit = start + length;
}

void HuffmanTree::transmit(std::uint16_t symbol, ByteSpan out, std::size_t& bit) const {
    if (symbol < kSymbols && loc_[symbol] != kNone) {
        write_code(loc_[symbol], out, bit);
        return;
    }
    write_code(loc_[kNyt], out, bit);
    for (int shift = 7; shift >= 0; --shift) add_bit(out, bit, static_cast<std::uint8_t>(symbol >> shift));
}

std::optional<std::uint16_t> HuffmanTree::receive(ByteView input, std::size_t& bit) const {
    std::uint16_t node = root_;
    while (node != kNone && nodes_[node].symbol == kInternal) {
        std::uint8_t branch = get_bit(input, bit);
        ++bit;
        node = branch ? nodes_[node].right : nodes_[node].left;
    }
    if (node == kNone) return std::nullopt;
    return nodes_[node].symbol;
}

MessageTree::MessageTree(const std::array<std::int32_t, 256>& frequencies) {
    for (std::size_t symbol = 0; symbol < 256; ++symbol) {
        for (std::int32_t k = 0; k < frequencies[symbol]; ++k) tree_.add_ref(static_cast<std::uint8_t>(symbol));
    }
}

void MessageTree::offset_transmit(std::uint16_t symbol, ByteSpan out, std::size_t& offset) const {
    if (symbol < HuffmanTree::kSymbols && tree_.loc_[symbol] != kNone) tree_.write_code(tree_.loc_[symbol], out, offset);
}

std::uint16_t MessageTree::offset_receive(ByteView input, std::size_t& offset) const {
    std::size_t bit = offset;
    auto symbol = tree_.receive(input, bit);
    if (!symbol) return 0;
    offset = bit;
    return *symbol;
}

std::size_t MessageTree::write_bits_compress(ByteView input, ByteSpan out) const {
    std::size_t bit = 0;
    for (std::uint8_t byte : input) tree_.write_code(tree_.loc_[byte], out, bit);
    return (bit + 7) >> 3;
}

std::size_t MessageTree::read_bits_compress(ByteView input, ByteSpan out) const {
    std::size_t bits = input.size() * 8;
    std::size_t bit = 0;
    std::size_t count = 0;
    while (bit < bits) {
        std::size_t start = bit;
        std::uint16_t symbol = offset_receive(input, bit);
        if (count < out.size()) out[count] = static_cast<std::uint8_t>(symbol);
        ++count;
        if (bit == start) break;  // a tree without codes (zero counts): the original never ends
    }
    return count;
}

void compress_adaptive(Msg& msg, std::size_t offset) {
    if (offset >= msg.cursize) return;
    std::size_t size = msg.cursize - offset;
    HuffmanTree tree;
    std::vector<std::uint8_t> work(kAdaptiveBuffer, 0);
    work[0] = static_cast<std::uint8_t>(size >> 8);
    work[1] = static_cast<std::uint8_t>(size);
    std::size_t bit = 16;
    for (std::size_t i = offset; i < msg.cursize; ++i) {
        std::uint8_t byte = byte_at(msg.data, i);
        tree.transmit(byte, work, bit);
        tree.add_ref(byte);
    }
    bit += 8;
    std::size_t length = bit >> 3;
    msg.cursize = offset + length;
    copy_into(msg.data, offset, ByteView(work).first(std::min(length, kAdaptiveBuffer)));
}

void decompress_adaptive(Msg& msg, std::size_t offset) {
    if (offset >= msg.cursize) return;
    std::size_t size = msg.cursize - offset;
    HuffmanTree tree;
    std::vector<std::uint8_t> work(kAdaptiveBuffer, 0);
    ByteView input = offset < msg.data.size() ? ByteView(msg.data).subspan(offset) : ByteView{};
    std::size_t declared = static_cast<std::size_t>(byte_at(input, 0)) * 256 + byte_at(input, 1);
    std::size_t length = std::min(declared, msg.maxsize > offset ? msg.maxsize - offset : 0);
    std::size_t bit = 16;
    for (std::size_t k = 0; k < length; ++k) {
        if ((bit >> 3) > size) {
            work[k] = 0;
            break;
        }
        std::uint16_t symbol = tree.receive(input, bit).value_or(0);
        if (symbol == kNyt) {
            symbol = 0;
            for (int i = 0; i < 8; ++i) {
                symbol = static_cast<std::uint16_t>((symbol << 1) | get_bit(input, bit));
                ++bit;
            }
        }
        work[k] = static_cast<std::uint8_t>(symbol);
        tree.add_ref(static_cast<std::uint8_t>(symbol));
    }
    msg.cursize = offset + length;
    copy_into(msg.data, offset, ByteView(work).first(length));
}

}  // namespace iw_pc::net
