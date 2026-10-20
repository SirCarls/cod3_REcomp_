//! Requests for `iw_pc::net::delta`.
//!
//! States arrive as their bytes (`s:` tokens; `s:-` is a NULL state). A
//! field arrives as `v:` with its offset and bit count (two little-endian
//! 32-bit numbers), read by the spec from the executable's own table; whole
//! tables come from the executable found through `IW_PC_INPUT`.

use std::sync::OnceLock;

use iw_pc::net::delta::{self, FieldTable, FieldTables, NetField, StructCoding, TableFlag};
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::Game;

use crate::net_netchan::with_msg;
use crate::{Answer, Args, Ret};

/// `game`'s field tables, read once from its executable.
pub(crate) fn tables(game: Game) -> Result<&'static FieldTables, String> {
    static TABLES: [OnceLock<Result<FieldTables, String>>; 2] = [OnceLock::new(), OnceLock::new()];
    let slot = &TABLES[usize::from(game == Game::UnitedOffensive)];
    slot.get_or_init(|| {
        let folders = std::env::var_os("IW_PC_INPUT").ok_or("IW_PC_INPUT is not set")?;
        let path = protocol_data::find_executable(game, &folders).ok_or_else(|| {
            format!(
                "{} not found in IW_PC_INPUT",
                protocol_data::executable_name(game)
            )
        })?;
        let exe = GameExecutable::open(game, &path).map_err(|e| e.to_string())?;
        FieldTables::from_executable(&exe).map_err(|e| e.to_string())
    })
    .as_ref()
    .map_err(Clone::clone)
}

fn field(bytes: &[u8]) -> Result<NetField, String> {
    let word = |at: usize| -> Result<i32, String> {
        let b = bytes.get(at..at + 4).ok_or("short field token")?;
        Ok(i32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    Ok(NetField {
        offset: word(0)? as u32,
        bits: word(4)?,
    })
}

/// A table argument (its name, as specs/net_delta.py's TABLES has it).
fn table<'t>(tables: &'t FieldTables, name: &[u8]) -> Result<&'t FieldTable, String> {
    Ok(match name {
        b"entity" => &tables.entity,
        b"entity12" => tables
            .entity_alternate
            .as_ref()
            .ok_or("this game has one entity table")?,
        b"client" => &tables.client,
        b"archived" => &tables.archived_entity,
        b"player" => &tables.player,
        b"hud" => &tables.hud_elem,
        b"objective" => &tables.objective,
        _ => return Err(format!("unknown table {}", String::from_utf8_lossy(name))),
    })
}

/// United Offensive's `MSG_WriteEntityTableFlag`: the callback a check of
/// `MSG_WriteDeltaStruct` may pass.
const UO_TABLE_FLAG: i32 = 0x0044_AF00;

/// A count argument (never negative in the checks).
fn count(args: &Args, i: usize) -> Result<usize, String> {
    usize::try_from(args.i32(i)).map_err(|_| "negative count".to_string())
}

/// A state argument: its bytes, or `None` for NULL.
fn state(args: &Args, i: usize) -> Option<Vec<u8>> {
    args.str(i).map(<[u8]>::to_vec)
}

/// Runs `f` on a copy of buffer argument `i` (the structure a reader fills)
/// and puts the copy back.
fn read_into<R>(
    args: &mut Args,
    i: usize,
    f: impl FnOnce(&mut Args, &mut [u8]) -> Result<R, String>,
) -> Result<R, String> {
    let mut to = args.buffer(i).ok_or("NULL to")?.clone();
    let result = f(args, &mut to)?;
    if let Some(slot) = args.buffer(i) {
        *slot = to;
    }
    Ok(result)
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    Some(match function {
        // from, to, msg, field
        "MSG_WriteDeltaField" => (|| {
            let from = state(args, 0).ok_or("NULL from")?;
            let to = state(args, 1).ok_or("NULL to")?;
            let field = field(args.bytes(3))?;
            with_msg(args, 2, game, |m| {
                delta::write_delta_field(m, &from, &to, field)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to, force
        "MSG_WriteDeltaEntity" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1).ok_or("NULL from")?;
            let to = state(args, 2);
            let force = args.i32(3) != 0;
            with_msg(args, 0, game, |m| {
                delta::write_delta_entity(m, tables, &from, to.as_deref(), force)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to, force, field count, number bits, table, [UO: callback,] change bit
        "MSG_WriteDeltaStruct" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1).ok_or("NULL from")?;
            let to = state(args, 2);
            let force = args.i32(3) != 0;
            let fields = table(tables, args.bytes(6))?
                .fields()
                .get(..count(args, 4)?)
                .ok_or("field count past the table")?;
            let (table_flag, change_bit) = match game {
                Game::CallOfDuty2 => (None, args.i32(7)),
                Game::UnitedOffensive => {
                    let flag = match args.i32(7) {
                        0 => None,
                        UO_TABLE_FLAG => Some(delta::write_entity_table_flag as TableFlag),
                        other => return Err(format!("unknown callback {other:#x}")),
                    };
                    (flag, args.i32(8))
                }
            };
            let coding = StructCoding {
                fields,
                index_bits: args.i32(5) as u32,
                change_bit: change_bit != 0,
                table_flag,
            };
            with_msg(args, 0, game, |m| {
                delta::write_delta_struct(m, &from, to.as_deref(), force, coding)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to, count
        "MSG_WriteDeltaHudElems" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1).ok_or("NULL from")?;
            let to = state(args, 2).ok_or("NULL to")?;
            let n = count(args, 3)?;
            with_msg(args, 0, game, |m| {
                delta::write_delta_hud_elems(m, tables, &from, &to, n)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to, force
        "MSG_WriteDeltaClient" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1);
            let to = state(args, 2);
            let force = args.i32(3) != 0;
            with_msg(args, 0, game, |m| {
                delta::write_delta_client(m, tables, from.as_deref(), to.as_deref(), force)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to
        "MSG_WriteDeltaPlayerstate" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1);
            let to = state(args, 2).ok_or("NULL to")?;
            with_msg(args, 0, game, |m| {
                delta::write_delta_playerstate(m, tables, from.as_deref(), &to)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to, key
        "MSG_WriteDeltaUsercmdKey" => (|| {
            let from = state(args, 1).ok_or("NULL from")?;
            let to = state(args, 2).ok_or("NULL to")?;
            let key = args.i32(3);
            with_msg(args, 0, game, |m| {
                delta::write_delta_usercmd_key(m, key, &from, &to)
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to (out), field, print flag (always 0)
        "MSG_ReadDeltaField" => (|| {
            let from = state(args, 1).ok_or("NULL from")?;
            let field = field(args.bytes(3))?;
            read_into(args, 2, |args, to| {
                with_msg(args, 0, game, |m| {
                    delta::read_delta_field(m, &from, to, field)
                })
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to (out), number
        "MSG_ReadDeltaEntity" | "MSG_ReadDeltaArchivedEntity" | "MSG_ReadDeltaClient" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1);
            let number = args.i32(3);
            let removed = read_into(args, 2, |args, to| {
                with_msg(args, 0, game, |m| match function {
                    "MSG_ReadDeltaEntity" => from
                        .as_deref()
                        .map(|from| delta::read_delta_entity(m, tables, from, to, number)),
                    "MSG_ReadDeltaArchivedEntity" => from
                        .as_deref()
                        .map(|from| delta::read_delta_archived_entity(m, tables, from, to, number)),
                    _ => Some(delta::read_delta_client(
                        m,
                        tables,
                        from.as_deref(),
                        to,
                        number,
                    )),
                })
            })?
            .ok_or("NULL from")?;
            Ok(Ret::Int(i64::from(removed)))
        })(
        ),
        // msg, from, to (out), number, field count, table
        "MSG_ReadDeltaStruct" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1).ok_or("NULL from")?;
            let number = args.i32(3);
            let table = table(tables, args.bytes(5))?;
            if count(args, 4)? != table.fields().len() {
                return Err("the checks pass whole tables".to_string());
            }
            let removed = read_into(args, 2, |args, to| {
                with_msg(args, 0, game, |m| {
                    delta::read_delta_struct(m, &from, to, number, table)
                })
            })?;
            Ok(Ret::Int(i64::from(removed)))
        })(),
        // msg, from, to (out), count
        "MSG_ReadDeltaHudElems" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1).ok_or("NULL from")?;
            let n = count(args, 3)?;
            read_into(args, 2, |args, to| {
                with_msg(args, 0, game, |m| {
                    delta::read_delta_hud_elems(m, tables, &from, to, 0, n)
                })
            })
            .map(|_| Ret::Void)
        })(),
        // msg, from, to (out)
        "MSG_ReadDeltaPlayerstate" => (|| {
            let tables = tables(game)?;
            let from = state(args, 1);
            read_into(args, 2, |args, to| {
                with_msg(args, 0, game, |m| {
                    delta::read_delta_playerstate(m, tables, from.as_deref(), to)
                })
            })
            .map(|_| Ret::Void)
        })(),
        // msg, key, from, to (out)
        "MSG_ReadDeltaUsercmdKey" => (|| {
            let key = args.i32(1);
            let from = state(args, 2).ok_or("NULL from")?;
            let mut to = args.buffer(3).ok_or("NULL to")?.clone();
            with_msg(args, 0, game, |m| {
                delta::read_delta_usercmd_key(m, key, &from, &mut to)
            })?;
            if let Some(slot) = args.buffer(3) {
                *slot = to;
            }
            Ok(Ret::Void)
        })(),
        _ => return None,
    })
}
