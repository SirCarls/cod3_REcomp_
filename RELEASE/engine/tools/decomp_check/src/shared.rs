//! Requests for `cod3_game::shared` (strings and info strings).

use cod3_game::cstr;
use cod3_game::shared::{hash, info, strings};

use crate::{game_error, Answer, Args, Ret};

fn text(args: &Args, i: usize) -> &[u8] {
    args.str(i).unwrap_or(b"")
}

pub fn dispatch(name: &str, args: &mut Args) -> Option<Answer> {
    Some(match name {
        "Q_stricmpn" => Ok(Ret::Int(
            strings::stricmpn(text(args, 0), text(args, 1), args.i32(2)).into(),
        )),
        "Q_stricmp" => Ok(Ret::Int(
            strings::stricmp(text(args, 0), text(args, 1)).into(),
        )),
        "Q_strlwr" => {
            let buffer = args.buffer(0)?;
            strings::strlwr(buffer);
            Ok(Ret::Str(Some(cstr::text(buffer).to_vec())))
        }
        "COM_GetExtension" => Ok(Ret::Str(Some(
            strings::get_extension(text(args, 0)).to_vec(),
        ))),
        "COM_DefaultExtension" => {
            strings::default_extension_cfg(args.buffer(0)?);
            Ok(Ret::Void)
        }
        "Q_strcat" => {
            let size = args.int(1) as usize;
            let src = text(args, 2).to_vec();
            let buffer = args.buffer(0)?;
            strings::strcat(&mut buffer[..size], &src)
                .map(|_| Ret::Void)
                .map_err(game_error)
        }
        "Info_ValueForKey" => match (args.str(0), args.str(1)) {
            (Some(info_text), Some(key)) => info::value_for_key(info_text, key)
                .map(|v| Ret::Str(Some(v.to_vec())))
                .map_err(game_error),
            _ => Ok(Ret::Str(Some(Vec::new()))), // the original's NULL check
        },
        "Info_RemoveKey" | "Info_RemoveKey_Big" => {
            let key = text(args, 1).to_vec();
            let buffer = args.buffer(0)?;
            let result = if name == "Info_RemoveKey" {
                info::remove_key(buffer, &key)
            } else {
                info::remove_key_big(buffer, &key)
            };
            result.map(|_| Ret::Void).map_err(game_error)
        }
        "Info_SetValueForKey" | "Info_SetValueForKey_Big" => {
            let key = text(args, 1).to_vec();
            let value = text(args, 2).to_vec();
            let buffer = args.buffer(0)?;
            let result = if name == "Info_SetValueForKey" {
                info::set_value_for_key(buffer, &key, &value)
            } else {
                info::set_value_for_key_big(buffer, &key, &value)
            };
            result.map(|_| Ret::Void).map_err(game_error)
        }
        "NameHash" => Ok(Ret::Int(hash::name_hash(text(args, 0)).into())),
        "NameHash_Init" => {
            let key = hash::name_key(text(args, 1)).to_guest_bytes();
            args.buffer(0)?.copy_from_slice(&key);
            Ok(Ret::Void)
        }
        "generateHashValue" => Ok(Ret::Int(hash::cvar_hash(text(args, 0)).into())),
        "FS_HashFileName" => Ok(Ret::Int(
            hash::file_name_hash(text(args, 0), args.int(1) as u32).into(),
        )),
        _ => return None,
    })
}
