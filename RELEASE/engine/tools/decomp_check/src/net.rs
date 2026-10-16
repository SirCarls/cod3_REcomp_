//! Requests for `cod3_game::net` (network messages and addresses).

use cod3_game::net::adr::{self, NetAdr, NETADR_SIZE};
use cod3_game::net::msg::Msg;

use crate::{game_error, Answer, Args, MsgArg, Ret};

/// Runs `f` on the message the checker sent, then keeps the fields it left
/// for the reply (the data buffer is changed in place).
fn on_message<R>(m: &mut MsgArg, f: impl FnOnce(&mut Msg<'_>) -> R) -> R {
    let mut msg = Msg {
        overflowed: m.overflowed,
        data: &mut m.data,
        maxsize: m.maxsize,
        cursize: m.cursize,
        readcount: m.readcount,
    };
    let result = f(&mut msg);
    let (overflowed, maxsize, cursize, readcount) =
        (msg.overflowed, msg.maxsize, msg.cursize, msg.readcount);
    m.overflowed = overflowed;
    m.maxsize = maxsize;
    m.cursize = cursize;
    m.readcount = readcount;
    result
}

/// A `netadr_t` argument (`v:`, the game's 20 bytes).
fn address(args: &Args, i: usize) -> NetAdr {
    let bytes: &[u8; NETADR_SIZE] = args.bytes(i).try_into().expect("a netadr_t is 20 bytes");
    NetAdr::from_guest_bytes(bytes)
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(match name {
        "MSG_WriteString" => {
            // The original writes an empty string for a NULL pointer.
            let text = args.str(1).unwrap_or(b"").to_vec();
            on_message(args.msg(0), |msg| msg.write_string(&text))
                .map(|()| Ret::Void)
                .map_err(game_error)
        }
        "MSG_ReadString" => Ok(Ret::Str(Some(on_message(args.msg(0), |msg| {
            msg.read_string().to_vec()
        })))),
        "NET_CompareAdrSigned" => Ok(Ret::Int(
            adr::compare_signed(&address(args, 0), &address(args, 1)).into(),
        )),
        "NET_AdrToString" => Ok(Ret::Str(Some(
            adr::adr_to_string(&address(args, 0)).into_bytes(),
        ))),
        _ => return None,
    })
}
