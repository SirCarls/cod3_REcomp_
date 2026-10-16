//! Network addresses: `netadr_t`, and comparing and printing them.
//!
//! The game's structure is 20 bytes. `NET_GetLoopPacket` zeroes all of them
//! before it sets the type, `NET_CompareAdrSigned` reads the fields at these
//! offsets, and `Netchan_Process` passes one by value in three registers:
//!
//! | Offset | Field | What it is |
//! |---|---|---|
//! | +0 | `type` | what kind of address ([`NA_LOOPBACK`], [`NA_IP`], [`NA_IPX`] ...) |
//! | +4 | `ip` | an IPv4 address, first byte first |
//! | +8 | `ipx` | an IPX network (4 bytes) and node (6 bytes) |
//! | +0x12 | `port` | the port, in network byte order |
//!
//! It is Quake III's layout. The type numbers the code uses are Quake III's
//! too; the others (0 bot, 1 bad, 3 broadcast, 6 IPX broadcast in Quake
//! III) get no special treatment from the functions here.

/// `NA_LOOPBACK`: the game talking to itself (the server of a local game).
pub const NA_LOOPBACK: i32 = 2;
/// `NA_IP`: an IPv4 address and port.
pub const NA_IP: i32 = 4;
/// `NA_IPX`: an IPX address and port.
pub const NA_IPX: i32 = 5;

/// The size of a `netadr_t` in the game.
pub const NETADR_SIZE: usize = 20;

/// `netadr_t`: a network address.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NetAdr {
    /// +0: `type`, a plain `int` in the game; any value can arrive here.
    pub kind: i32,
    /// +4: the IPv4 address, first byte first (`127.0.0.1` is `[127, 0, 0, 1]`).
    pub ip: [u8; 4],
    /// +8: the IPX network (4 bytes) and node (6 bytes).
    pub ipx: [u8; 10],
    /// +0x12: the port number. The game keeps it in network byte order,
    /// which is the Xbox 360's own, so this is the number itself.
    pub port: u16,
}

impl NetAdr {
    /// Reads an address as the game keeps it in memory (20 bytes, big-endian).
    pub fn from_guest_bytes(bytes: &[u8; NETADR_SIZE]) -> Self {
        let mut ip = [0; 4];
        let mut ipx = [0; 10];
        ip.copy_from_slice(&bytes[4..8]);
        ipx.copy_from_slice(&bytes[8..18]);
        NetAdr {
            kind: i32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            ip,
            ipx,
            port: u16::from_be_bytes([bytes[18], bytes[19]]),
        }
    }

    /// The address as the game keeps it in memory.
    pub fn to_guest_bytes(&self) -> [u8; NETADR_SIZE] {
        let mut bytes = [0; NETADR_SIZE];
        bytes[..4].copy_from_slice(&self.kind.to_be_bytes());
        bytes[4..8].copy_from_slice(&self.ip);
        bytes[8..18].copy_from_slice(&self.ipx);
        bytes[18..].copy_from_slice(&self.port.to_be_bytes());
        bytes
    }
}

/// The difference of the first ports or bytes that differ, as the game
/// orders addresses of the same type: the port first, then the address.
fn port_then_bytes(a_port: u16, b_port: u16, a: &[u8], b: &[u8]) -> i32 {
    if a_port != b_port {
        return i32::from(a_port) - i32::from(b_port);
    }
    a.iter()
        .zip(b)
        .map(|(&x, &y)| i32::from(x) - i32::from(y))
        .find(|&difference| difference != 0)
        .unwrap_or(0)
}

/// Orders two addresses: 0 when they are the same address, else a number
/// whose sign says which comes first.
///
/// Addresses of different types differ by their type numbers (`a.kind -
/// b.kind`). IP and IPX addresses compare their ports first, as numbers,
/// then their address bytes, first to last; the result is the first
/// difference. Two loopback addresses are the same; so are two addresses of
/// the same other type (the original prints "bad address type" for those,
/// a developer print compiled out of the game's build). Bytes a type
/// doesn't use are ignored (the IPX bytes of an IP address ...).
///
/// The original takes pointers; Quake III's `NET_CompareAdr` only said
/// whether two addresses were equal.
///
/// Original: `NET_CompareAdrSigned` at 0x82453E28 (multiplayer: 0x82484208)
pub fn compare_signed(a: &NetAdr, b: &NetAdr) -> i32 {
    if a.kind != b.kind {
        return a.kind.wrapping_sub(b.kind);
    }
    match a.kind {
        NA_IP => port_then_bytes(a.port, b.port, &a.ip, &b.ip),
        NA_IPX => port_then_bytes(a.port, b.port, &a.ipx, &b.ipx),
        _ => 0,
    }
}

/// The address as text: `loopback`, `1.2.3.4:28960` for an IP address, and
/// for every other type the IPX form, `0a0b0c0d.0102030405ff:28960` (the
/// network, a dot, the node, in two lowercase hex digits a byte).
///
/// The port is printed as a signed 16-bit number, as the original's `%i`
/// of `BigShort(port)` does: ports from 32768 up come out negative
/// (`40000` prints as `-25536`). Quake III printed it unsigned (`%hu`), and
/// printed `bot` for type 0, which CoD3 prints in the IPX form.
///
/// The original formats into a static 64-byte buffer (the longest text is
/// 28 characters) and returns it; the port returns the text.
///
/// Original: `NET_AdrToString` at 0x82451DB0 (multiplayer: 0x8247EE88)
pub fn adr_to_string(adr: &NetAdr) -> String {
    // BigShort is a no-op on the big-endian 360; `%i` gets it as a short.
    let port = adr.port as i16;
    match adr.kind {
        NA_LOOPBACK => "loopback".to_string(),
        NA_IP => {
            let [a, b, c, d] = adr.ip;
            format!("{a}.{b}.{c}.{d}:{port}")
        }
        _ => {
            let [n0, n1, n2, n3, h0, h1, h2, h3, h4, h5] = adr.ipx;
            format!(
                "{n0:02x}{n1:02x}{n2:02x}{n3:02x}.{h0:02x}{h1:02x}{h2:02x}{h3:02x}{h4:02x}{h5:02x}:{port}"
            )
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ip(a: [u8; 4], port: u16) -> NetAdr {
        NetAdr {
            kind: NA_IP,
            ip: a,
            port,
            ..NetAdr::default()
        }
    }

    #[test]
    fn prints_addresses_like_the_game() {
        assert_eq!(adr_to_string(&ip([127, 0, 0, 1], 28960)), "127.0.0.1:28960");
        assert_eq!(
            adr_to_string(&ip([10, 0, 0, 255], 40000)),
            "10.0.0.255:-25536"
        );
        let loopback = NetAdr {
            kind: NA_LOOPBACK,
            ..ip([1, 2, 3, 4], 5)
        };
        assert_eq!(adr_to_string(&loopback), "loopback");
        let ipx = NetAdr {
            kind: NA_IPX,
            ipx: [0x0A, 0x0B, 0x0C, 0x0D, 1, 2, 3, 4, 5, 0xFF],
            port: 28960,
            ..NetAdr::default()
        };
        assert_eq!(adr_to_string(&ipx), "0a0b0c0d.0102030405ff:28960");
    }

    #[test]
    fn orders_by_type_then_port_then_address() {
        let a = ip([10, 0, 0, 1], 28960);
        assert_eq!(compare_signed(&a, &a), 0);
        assert_eq!(compare_signed(&a, &ip([10, 0, 0, 1], 28961)), -1);
        assert_eq!(compare_signed(&a, &ip([9, 200, 0, 1], 28960)), 1);
        assert_eq!(compare_signed(&a, &ip([10, 0, 0, 250], 28960)), -249);
        let other = NetAdr { kind: 1, ..a };
        assert_eq!(compare_signed(&a, &other), 3);
        assert_eq!(
            compare_signed(
                &other,
                &NetAdr {
                    kind: 1,
                    ..ip([1; 4], 1)
                }
            ),
            0
        );
        let low = NetAdr {
            kind: i32::MIN,
            ..a
        };
        assert_eq!(compare_signed(&low, &NetAdr { kind: 1, ..a }), i32::MAX);
    }

    #[test]
    fn keeps_the_games_bytes() {
        let a = NetAdr {
            kind: NA_IPX,
            ip: [1, 2, 3, 4],
            ipx: [5, 6, 7, 8, 9, 10, 11, 12, 13, 14],
            port: 0xABCD,
        };
        let bytes = a.to_guest_bytes();
        assert_eq!(&bytes[..4], &[0, 0, 0, 5]);
        assert_eq!(&bytes[18..], &[0xAB, 0xCD]);
        assert_eq!(NetAdr::from_guest_bytes(&bytes), a);
    }
}
