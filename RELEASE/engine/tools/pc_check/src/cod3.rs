//! `cod3.*` requests: Call of Duty 3's ports (`cod3_game`) answering the PC
//! specs' questions, so `pcdecomp.py check --against cod3` shows which
//! functions the three games share and which differ. The arguments are
//! the PC specs' (the same C prototypes).

use cod3_game::cstr;
use cod3_game::shared::{hash, info, strings};

use crate::{game_error, Answer, Args, Ret};

pub fn dispatch(function: &str, args: &mut Args) -> Option<Answer> {
    Some(match function {
        "Q_stricmpn" => Ok(Ret::Int(
            strings::stricmpn(args.text(0), args.text(1), args.i32(2)).into(),
        )),
        "Q_stricmp" | "sub_0044FAA0" => Ok(Ret::Int(
            strings::stricmp(args.text(0), args.text(1)).into(),
        )),
        "Q_strlwr" => {
            let buffer = args.buffer(0)?;
            strings::strlwr(buffer);
            Ok(Ret::Str(Some(cstr::text(buffer).to_vec())))
        }
        "Q_strcat" => {
            let size = args.int(1) as usize;
            let src = args.text(2).to_vec();
            let dest = args.buffer(0)?;
            strings::strcat(&mut dest[..size], &src)
                .map(|_| Ret::Void)
                .map_err(game_error)
        }
        "Info_ValueForKey" => match (args.str(0), args.str(1)) {
            (Some(text), Some(key)) => info::value_for_key(text, key)
                .map(|v| Ret::Str(Some(v.to_vec())))
                .map_err(game_error),
            _ => Ok(Ret::Str(Some(Vec::new()))),
        },
        "Info_RemoveKey" | "Info_RemoveKey_Big" => {
            let key = args.text(1).to_vec();
            let buffer = args.buffer(0)?;
            let result = if function == "Info_RemoveKey" {
                info::remove_key(buffer, &key)
            } else {
                info::remove_key_big(buffer, &key)
            };
            result.map(|_| Ret::Void).map_err(game_error)
        }
        "Info_SetValueForKey" | "Info_SetValueForKey_Big" => {
            // Call of Duty 3 stops with an error where the PC games print a
            // warning: a success compares, a refusal shows as a difference.
            let key = args.text(1).to_vec();
            let value = args.text(2).to_vec();
            let buffer = args.buffer(0)?;
            let result = if function == "Info_SetValueForKey" {
                info::set_value_for_key(buffer, &key, &value)
            } else {
                info::set_value_for_key_big(buffer, &key, &value)
            };
            result.map(|_| Ret::Printed(None)).map_err(game_error)
        }
        "FS_HashFileName" => Ok(Ret::Int(
            hash::file_name_hash(args.text(0), args.int(1) as u32).into(),
        )),
        "generateHashValue" => match args.str(0) {
            Some(name) => Ok(Ret::Int(hash::cvar_hash(name).into())),
            None => Err(game_error(cod3_game::GameError::drop(
                "null name in generateHashValue",
            ))),
        },
        _ => return None,
    })
}
