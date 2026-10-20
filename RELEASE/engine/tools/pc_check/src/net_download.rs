//! Requests for `iw_pc::client::download`: a download message
//! (specs/net_download.py). The host is the check's: each call is logged as
//! the original's stand-ins record them, and answers as they do.

use iw_pc::client::download::{self, c_str, DownloadHost, DownloadState};
use iw_pc::Game;

use crate::net_client::{load, store, CallLog, Logged, Regions};
use crate::net_netchan::with_msg;
use crate::{game_error, Answer, Args, Ret};

/// Where United Offensive keeps the download's numbers in clc (offsets from
/// its start, 0x04DC8860; specs/net_download.py).
const UO_FILE: usize = 0x2_0148;
const UO_BLOCK: usize = 0x2_0150;
const UO_COUNT: usize = 0x2_0154;
const UO_SIZE: usize = 0x2_0158;
const UO_FLAGS: usize = 0x2_015C;
const UO_WWW: usize = 0x2_0560;
const UO_ABORTING: usize = 0x2_0564;
const UO_BAD: usize = 0x2_0968;
/// Call of Duty 2's download block (0x00966DAC): after the three names.
const COD2_FLAGS: usize = 0x248;
const COD2_WWW: usize = 0x24C;
const COD2_FILE: usize = 0x250;
const COD2_BLOCK: usize = 0x254;
const COD2_SIZE: usize = 0x258;
/// Its menus' block: size, count, name.
const UI_SIZE: usize = 0x10;
const UI_COUNT: usize = 0x14;
const UI_NAME: usize = 0x1C;

fn get(b: &[u8], at: usize) -> Result<i32, String> {
    b.get(at..at + 4)
        .map(|w| i32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        .ok_or_else(|| "short download region".to_string())
}

fn put(b: &mut [u8], at: usize, value: i32) -> Result<(), String> {
    b.get_mut(at..at + 4)
        .ok_or("short download region")?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}

/// The names (both games, the start of the download block): the
/// disconnected flag, then the name, the temporary name, the original name.
fn read_names(dl: &mut DownloadState, b: &[u8]) -> Result<(), String> {
    if b.len() < 0x244 {
        return Err("short download block".to_string());
    }
    dl.www_disconnected = get(b, 0)? != 0;
    dl.name.copy_from_slice(&b[4..0x104]);
    dl.temp_name.copy_from_slice(&b[0x104..0x204]);
    dl.original_name.copy_from_slice(&b[0x204..0x244]);
    Ok(())
}

fn write_names(dl: &DownloadState, b: &mut [u8]) -> Result<(), String> {
    put(b, 0, i32::from(dl.www_disconnected))?;
    b[4..0x104].copy_from_slice(&dl.name);
    b[0x104..0x204].copy_from_slice(&dl.temp_name);
    b[0x204..0x244].copy_from_slice(&dl.original_name);
    Ok(())
}

/// What the stand-ins answer (HostAnswers): four numbers, then the home path.
struct Answers {
    handle: i32,
    begin: i32,
    shell: i32,
    window: i32,
    home: Option<Vec<u8>>,
}

fn answers(b: &[u8]) -> Result<Answers, String> {
    let w = |i: usize| -> Result<i32, String> {
        b.get(4 * i..4 * i + 4)
            .map(|x| i32::from_be_bytes([x[0], x[1], x[2], x[3]]))
            .ok_or_else(|| "short answers token".to_string())
    };
    let home = match b.get(16) {
        Some(1) => Some(b[17..].to_vec()),
        Some(0) => None,
        _ => return Err("bad answers token".to_string()),
    };
    Ok(Answers {
        handle: w(0)?,
        begin: w(1)?,
        shell: w(2)?,
        window: w(3)?,
        home,
    })
}

/// The check's host: the stand-ins' answers, their calls logged; Call of
/// Duty 2's menus' numbers written into their block.
struct CheckHost {
    game: Game,
    answers: Answers,
    ui: Vec<u8>,
    log: CallLog,
}

impl CheckHost {
    fn cvar_value(&mut self, name: &[u8], value: i32) {
        self.log.call(
            "Cvar_SetValue",
            &[Logged::S(name), Logged::I((value as f32).to_bits() as i32)],
        );
    }

    fn ui_word(&mut self, at: usize, value: i32) {
        if let Some(w) = self.ui.get_mut(at..at + 4) {
            w.copy_from_slice(&value.to_le_bytes());
        }
    }
}

impl DownloadHost for CheckHost {
    fn open(&mut self, temp_name: &[u8]) -> i32 {
        self.log
            .call("FS_SV_FOpenFileWrite", &[Logged::S(temp_name)]);
        self.answers.handle
    }

    fn write(&mut self, data: &[u8], length: i32, file: i32) {
        self.log.call(
            "FS_Write",
            &[Logged::S(data), Logged::I(length), Logged::I(file)],
        );
    }

    fn close(&mut self, file: i32) {
        self.log.call("FS_FCloseFile", &[Logged::I(file)]);
    }

    fn rename(&mut self, from: &[u8], to: &[u8]) {
        self.log
            .call("FS_SV_Rename", &[Logged::S(from), Logged::S(to)]);
    }

    fn write_packet(&mut self) {
        self.log.call("CL_WritePacket", &[]);
    }

    fn next_download(&mut self) {
        self.log.call("CL_NextDownload", &[]);
    }

    fn set_size(&mut self, size: i32) {
        match self.game {
            Game::CallOfDuty2 => self.ui_word(UI_SIZE, size),
            Game::UnitedOffensive => self.cvar_value(b"cl_downloadSize", size),
        }
    }

    fn set_count(&mut self, count: i32) {
        match self.game {
            Game::CallOfDuty2 => self.ui_word(UI_COUNT, count),
            Game::UnitedOffensive => self.cvar_value(b"cl_downloadCount", count),
        }
    }

    fn clear_name(&mut self) {
        match self.game {
            Game::CallOfDuty2 => {
                if let Some(c) = self.ui.get_mut(UI_NAME) {
                    *c = 0;
                }
            }
            Game::UnitedOffensive => self.log.call(
                "Cvar_Set2",
                &[Logged::S(b"cl_downloadName"), Logged::S(b""), Logged::I(1)],
            ),
        }
    }

    fn home_path(&mut self) -> Vec<u8> {
        let label = match self.game {
            Game::CallOfDuty2 => "Dvar_FindVar",
            Game::UnitedOffensive => "Cvar_FindVar",
        };
        self.log.call(label, &[Logged::S(b"fs_homepath")]);
        self.answers.home.clone().unwrap_or_default()
    }

    fn os_path(&mut self, base: &[u8], game: &[u8], qpath: &[u8]) -> Vec<u8> {
        self.log.call(
            "FS_BuildOSPath",
            &[
                Logged::S(base),
                Logged::S(game),
                Logged::S(qpath),
                Logged::I(0),
            ],
        );
        // The check's FS_BuildOSPath: base/game/qpath, at most 255 characters.
        let mut path = [base, b"/", game, b"/", qpath].concat();
        path.truncate(255);
        path
    }

    fn open_url(&mut self, url: &[u8]) -> bool {
        match self.game {
            Game::CallOfDuty2 => {
                self.log.call(
                    "ShellExecuteA",
                    &[Logged::S(b"open"), Logged::S(url), Logged::I(9)],
                );
                if self.answers.shell == 0 {
                    return false;
                }
                self.log.call("GetForegroundWindow", &[]);
                if self.answers.window != 0 {
                    self.log.call(
                        "ShowWindow",
                        &[Logged::I(self.answers.window), Logged::I(3)],
                    );
                }
                true
            }
            Game::UnitedOffensive => {
                self.log
                    .call("Sys_OpenURL", &[Logged::S(url), Logged::I(1)]);
                true
            }
        }
    }

    fn add_text(&mut self, text: &[u8]) {
        self.log.call("Cbuf_AddText", &[Logged::S(text)]);
    }

    fn begin_download(&mut self, local: &[u8], remote: &[u8]) -> bool {
        self.log
            .call("DL_BeginDownload", &[Logged::S(local), Logged::S(remote)]);
        self.answers.begin != 0
    }

    fn end_download(&mut self) {
        self.log.call("DL_EndDownload", &[]);
    }
}

/// CL_ParseDownload and Call of Duty 2's CL_ParseDownloadRedirect: (msg,
/// clc before the channel, the download block, the menus' block, the
/// stand-ins' answers, the call log) -> the first warning.
fn answer(game: Game, args: &mut Args, redirect: bool) -> Answer {
    let regions = Regions {
        cl: None,
        clc: Some(1),
    };
    let mut cl = load(game, args, regions)?;
    let mut dl = DownloadState::default();
    let block = args.region(2).clone();
    read_names(&mut dl, &block)?;
    let ui = args.buffer(3).ok_or("NULL menus' block")?.clone();
    match game {
        Game::CallOfDuty2 => {
            dl.flags = get(&block, COD2_FLAGS)?;
            dl.www = get(&block, COD2_WWW)? != 0;
            dl.file = get(&block, COD2_FILE)?;
            dl.block = get(&block, COD2_BLOCK)?;
            dl.size = get(&block, COD2_SIZE)?;
            dl.count = get(&ui, UI_COUNT)?;
        }
        Game::UnitedOffensive => {
            let clc = args.region(1);
            dl.file = get(clc, UO_FILE)?;
            dl.block = get(clc, UO_BLOCK)?;
            dl.count = get(clc, UO_COUNT)?;
            dl.size = get(clc, UO_SIZE)?;
            dl.flags = get(clc, UO_FLAGS)?;
            dl.www = get(clc, UO_WWW)? != 0;
            dl.www_aborting = get(clc, UO_ABORTING)? != 0;
            dl.bad_checksums = c_str(clc.get(UO_BAD..).unwrap_or(&[])).to_vec();
        }
    }
    let mut host = CheckHost {
        game,
        answers: answers(args.bytes(4))?,
        ui,
        log: CallLog::default(),
    };
    let result = with_msg(args, 0, game, |m| {
        if redirect {
            download::parse_redirect(m, &mut cl, &mut dl, &mut host)
        } else {
            download::parse_download(m, &mut cl, &mut dl, &mut host)
        }
    })?;
    // (The log and the changed state stand also when the call ends in an error.)
    store(&cl, args, regions)?;
    let block = args.region(2);
    write_names(&dl, block)?;
    match game {
        Game::CallOfDuty2 => {
            put(block, COD2_FLAGS, dl.flags)?;
            put(block, COD2_WWW, i32::from(dl.www))?;
            put(block, COD2_FILE, dl.file)?;
            put(block, COD2_BLOCK, dl.block)?;
            put(block, COD2_SIZE, dl.size)?;
        }
        Game::UnitedOffensive => {
            let clc = args.region(1);
            put(clc, UO_FILE, dl.file)?;
            put(clc, UO_BLOCK, dl.block)?;
            put(clc, UO_COUNT, dl.count)?;
            put(clc, UO_SIZE, dl.size)?;
            put(clc, UO_FLAGS, dl.flags)?;
            put(clc, UO_WWW, i32::from(dl.www))?;
            put(clc, UO_ABORTING, i32::from(dl.www_aborting))?;
        }
    }
    *args.buffer(3).ok_or("NULL menus' block")? = host.ui;
    *args.buffer(5).ok_or("NULL call log")? = host.log.0;
    let warning = result.map_err(game_error)?;
    Ok(Ret::Printed(
        warning.map(|w| w.message_format().as_bytes().to_vec()),
    ))
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    match function {
        "CL_ParseDownload" => Some(answer(game, args, false)),
        "CL_ParseDownloadRedirect" if game == Game::CallOfDuty2 => Some(answer(game, args, true)),
        _ => None,
    }
}
