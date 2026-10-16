//! Requests for the PC games' ports (`cod2mp.*`, `uomp.*`).
//!
//! Arguments come in the C prototype's order, whatever registers the
//! original takes them in (the specs in `tools/decomp/x86/specs/` place them).

use iw_pc::shared::{filter, hash, info, strings};
use iw_pc::{cstr, Game};

use crate::{game_error, Answer, Args, Ret};

fn int(value: bool) -> Ret {
    Ret::Int(value.into())
}

/// The string a function leaves in its buffer argument (what it returns as a pointer).
fn text_of(buffer: &[u8]) -> Ret {
    Ret::Str(Some(cstr::text(buffer).to_vec()))
}

/// Writes `text` and a terminator at the start of a buffer argument, as
/// the originals do for an output they fill (`COM_StripExtension`).
fn write_out(args: &mut Args, i: usize, text: &[u8]) -> Result<(), String> {
    let out = args.buffer(i).ok_or("NULL output")?;
    if cstr::write_at(out, 0, text) {
        Ok(())
    } else {
        Err("output buffer too small".to_string())
    }
}

fn printed(result: Result<(), info::Refusal>, game: Game) -> Ret {
    Ret::Printed(
        result
            .err()
            .map(|refusal| refusal.message_format(game).as_bytes().to_vec()),
    )
}

pub fn dispatch(image: &str, function: &str, args: &mut Args) -> Option<Answer> {
    let game = if image == "cod2mp" {
        Game::CallOfDuty2
    } else {
        Game::UnitedOffensive
    };
    let cod2 = game == Game::CallOfDuty2;
    // The network code's ports answer in their own modules.
    if let Some(answer) = crate::net_msg::dispatch(game, function, args) {
        return Some(answer);
    }
    if let Some(answer) = crate::net_oob::dispatch(game, function, args) {
        return Some(answer);
    }
    if let Some(answer) = crate::net_delta::dispatch(game, function, args) {
        return Some(answer);
    }
    Some(match function {
        // --- strings -----------------------------------------------------------
        "Q_stricmpn" => Ok(Ret::Int(
            strings::stricmpn(args.text(0), args.text(1), args.i32(2)).into(),
        )),
        "Q_strncmp" => Ok(Ret::Int(
            strings::strncmp(args.text(0), args.text(1), args.i32(2)).into(),
        )),
        "Q_stricmp" if cod2 => Ok(Ret::Int(
            strings::stricmp(args.text(0), args.text(1)).into(),
        )),
        "Q_stricmp" => Ok(Ret::Int(
            strings::stricmp_nullable(args.str(0), args.str(1)).into(),
        )),
        "Q_strcmp" if cod2 => Ok(Ret::Int(strings::strcmp(args.text(0), args.text(1)).into())),
        "sub_0044FAA0" if !cod2 => Ok(Ret::Int(
            strings::stricmp_equal_or_minus_one(args.text(0), args.text(1)).into(),
        )),
        "Q_strncpyz" => {
            let size = args.int(2) as usize;
            let src = args.text(1).to_vec();
            let dest = args.buffer(0)?;
            strings::strncpyz(&mut dest[..size], &src);
            Ok(Ret::Void)
        }
        "Q_strlwr" | "Q_strupr" | "Q_CleanStr" => {
            let buffer = args.buffer(0)?;
            match function {
                "Q_strlwr" => strings::strlwr(buffer),
                "Q_strupr" => strings::strupr(buffer),
                _ => strings::clean_str(buffer),
            }
            Ok(text_of(buffer))
        }
        "Q_strcat" => {
            let size = args.int(1) as usize;
            let src = args.text(2).to_vec();
            let dest = args.buffer(0)?;
            strings::strcat(&mut dest[..size], &src)
                .map(|_| Ret::Void)
                .map_err(game_error)
        }
        "Q_PrintStrlen" => Ok(Ret::Int(strings::print_strlen(args.text(0)).into())),
        "COM_SkipPath" => Ok(Ret::Str(Some(strings::skip_path(args.text(0)).to_vec()))),
        "Com_GetExtensionSubString" if cod2 => Ok(Ret::Str(Some(
            strings::extension_with_dot(args.text(0)).to_vec(),
        ))),
        "COM_StripExtension" => {
            let kept = if cod2 {
                strings::strip_extension(args.text(0)).to_vec()
            } else {
                strings::strip_extension_at_first_dot(args.text(0)).to_vec()
            };
            write_out(args, 1, &kept).map(|_| Ret::Void)
        }
        "COM_DefaultExtension" => {
            let max_size = args.int(1) as usize;
            let extension = args.text(2).to_vec();
            strings::default_extension(args.buffer(0)?, max_size, &extension);
            Ok(Ret::Void)
        }
        // --- info strings ------------------------------------------------------
        "Info_ValueForKey" => match (args.str(0), args.str(1)) {
            (Some(text), Some(key)) => info::value_for_key(text, key)
                .map(|v| Ret::Str(Some(v.to_vec())))
                .map_err(game_error),
            _ => Ok(Ret::Str(Some(Vec::new()))), // the original's NULL check
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
        "Info_Validate" => Ok(int(info::validate(args.text(0)))),
        "Info_SetValueForKey" | "Info_SetValueForKey_Big" => {
            let key = args.text(1).to_vec();
            let value = args.text(2).to_vec();
            let buffer = args.buffer(0)?;
            let result = if function == "Info_SetValueForKey" {
                info::set_value_for_key(buffer, &key, &value)
            } else {
                info::set_value_for_key_big(game, buffer, &key, &value)
            };
            Ok(printed(result, game))
        }
        // --- hashes ------------------------------------------------------------
        "FS_HashFileName" => Ok(Ret::Int(
            hash::file_name_hash(args.text(0), args.int(1) as u32).into(),
        )),
        "generateHashValue" => match args.str(0) {
            Some(name) => Ok(Ret::Int(hash::cvar_hash(name).into())),
            None => Err(game_error(iw_pc::GameError::drop(
                "null name in generateHashValue",
            ))),
        },
        "Com_HashKey" => Ok(Ret::Int(hash::hash_key(args.text(0), args.i32(1)).into())),
        // --- filters -----------------------------------------------------------
        "Com_Filter" | "Com_FilterPath" => {
            let (pattern, name, case_sensitive) = (args.text(0), args.text(1), args.int(2) != 0);
            let matched = match (function, cod2) {
                ("Com_Filter", true) => filter::filter_cod2(pattern, name, case_sensitive),
                ("Com_Filter", false) => filter::filter(pattern, name, case_sensitive),
                (_, true) => filter::filter_path_cod2(pattern, name, case_sensitive),
                (_, false) => filter::filter_path(pattern, name, case_sensitive),
            };
            Ok(int(matched))
        }
        _ => return None,
    })
}
