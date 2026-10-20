//! Requests for `iw_pc::client::connect`: the `connect` console command
//! (specs/net_connect.py). The host is the check's: each call is logged as
//! the original's stand-ins record them and answers as they do; Call of Duty
//! 2's menus' block and key catchers are written as the original writes them.

use iw_pc::client::connect::{self, ConnectHost, ConnectResult, RESOLVED_FORMAT};
use iw_pc::client::{ClientState, SERVER_MESSAGE_SIZE, SERVER_NAME_SIZE};
use iw_pc::net::netchan::NetAddress;
use iw_pc::Game;

use crate::net_client::{canonical_address, guest_address, CallLog, Logged};
use crate::net_netchan::{address_bytes, address_from};
use crate::{game_error, Answer, Args, Ret};

/// Offsets in the clc part the check passes (from `clc.serverAddress`).
const CONNECT_TIME: usize = 0x14;
const CONNECT_COUNT: usize = 0x18;
const SERVER_MESSAGE: usize = 0x1C;
/// Call of Duty 2's menus' block: the server-quit flag and the two texts.
const UI_QUIT: usize = 0xDD;
const UI_TEXTS: [usize; 2] = [0x5C, 0x9C];

fn get(b: &[u8], at: usize) -> Result<i32, String> {
    b.get(at..at + 4)
        .map(|w| i32::from_le_bytes([w[0], w[1], w[2], w[3]]))
        .ok_or_else(|| "short region".to_string())
}

fn put(b: &mut [u8], at: usize, value: i32) -> Result<(), String> {
    b.get_mut(at..at + 4)
        .ok_or("short region")?
        .copy_from_slice(&value.to_le_bytes());
    Ok(())
}

struct CheckHost {
    game: Game,
    running: bool,
    resolved: Option<NetAddress>,
    key: bool,
    qport: i32,
    ui: Vec<u8>,
    keys: Vec<u8>,
    key_catchers: Option<i32>,
    log: CallLog,
}

impl ConnectHost for CheckHost {
    fn server_running(&mut self) -> bool {
        self.running
    }

    fn stop_sounds(&mut self) {
        let label = match self.game {
            Game::CallOfDuty2 => "SND_StopSounds",
            Game::UnitedOffensive => "S_StopAllSounds",
        };
        self.log.call(label, &[Logged::I(0)]);
    }

    fn set_setting(&mut self, name: &[u8], value: &[u8]) {
        self.log.call(
            "Cvar_Set2",
            &[Logged::S(name), Logged::S(value), Logged::I(1)],
        );
    }

    fn local_server_quit(&mut self) {
        match self.game {
            Game::CallOfDuty2 => {
                if let Some(b) = self.ui.get_mut(UI_QUIT) {
                    *b = 1;
                }
            }
            Game::UnitedOffensive => self
                .log
                .call("Com_ShutdownServer", &[Logged::S(b"EXE_SERVERQUIT")]),
        }
    }

    fn clear_menu_texts(&mut self) {
        for at in UI_TEXTS {
            if let Some(b) = self.ui.get_mut(at) {
                *b = 0;
            }
        }
    }

    fn server_frame(&mut self) {
        self.log.call("SV_Frame", &[Logged::I(0)]);
    }

    fn disconnect(&mut self) {
        match self.game {
            Game::CallOfDuty2 => self.log.call("CL_Disconnect", &[]),
            Game::UnitedOffensive => self.log.call("CL_Disconnect", &[Logged::I(1)]),
        }
    }

    fn close_console(&mut self) {
        self.log.call("Con_Close", &[]);
    }

    fn resolve(&mut self, name: &[u8], address: &mut NetAddress) -> bool {
        self.log.call("NET_StringToAdr", &[Logged::S(name)]);
        match self.resolved {
            Some(a) => {
                *address = a;
                true
            }
            None => false,
        }
    }

    fn cd_key_valid(&mut self) -> bool {
        self.log.call("CL_CDKeyValidate", &[]);
        self.key
    }

    fn clear_key_catchers(&mut self) {
        match self.game {
            Game::CallOfDuty2 => {
                // The word at 0x0096B654 and the byte after it.
                for b in self.keys.iter_mut().take(5) {
                    *b = 0;
                }
            }
            Game::UnitedOffensive => self.key_catchers = Some(0),
        }
    }

    fn net_qport(&mut self) -> i32 {
        self.qport
    }

    fn close_menus(&mut self) {
        self.log.call("UI_CloseAll", &[]);
        self.log.call("SCR_UpdateScreen", &[]);
    }
}

/// CL_Connect_f: (argc, the second word, sv_running, the menus' block, cls,
/// clc from the server's address, cls.servername, [Call of Duty 2: the key
/// catchers, net_qport,] the stand-ins' answers, the call log) -> what it
/// printed.
fn answer(game: Game, args: &mut Args) -> Answer {
    let cod2 = game == Game::CallOfDuty2;
    let (answers_at, log_at) = if cod2 { (9, 10) } else { (7, 8) };
    let answers = args.bytes(answers_at).to_vec();
    let resolved = match answers.first() {
        Some(0) => None,
        Some(1) => Some(address_from(answers.get(1..21).ok_or("short answers")?)?),
        _ => return Err("bad answers token".to_string()),
    };
    let key = *answers.last().ok_or("short answers")? != 0;

    let mut cl = ClientState::new(game);
    let cls = args.region(4).clone();
    cl.connection_state = get(&cls, 0)?;
    if cod2 {
        cl.qport = get(&cls, 4)?;
    }
    let clc = args.region(5).clone();
    let mut address = [0u8; 20];
    address.copy_from_slice(clc.get(..20).ok_or("short clc")?);
    cl.server_address = address_from(&canonical_address(game, &address))?;
    cl.connect_time = get(&clc, CONNECT_TIME)?;
    cl.connect_packet_count = get(&clc, CONNECT_COUNT)?;
    cl.server_message.copy_from_slice(
        clc.get(SERVER_MESSAGE..SERVER_MESSAGE + SERVER_MESSAGE_SIZE)
            .ok_or("short clc")?,
    );
    cl.server_name.copy_from_slice(
        args.region(6)
            .get(..SERVER_NAME_SIZE)
            .ok_or("short server name")?,
    );

    let mut host = CheckHost {
        game,
        running: args.i32(2) & if cod2 { 0xFF } else { -1 } != 0,
        resolved,
        key,
        qport: if cod2 { args.i32(8) } else { 0 },
        ui: args.buffer(3).cloned().unwrap_or_default(),
        keys: if cod2 {
            args.region(7).clone()
        } else {
            Vec::new()
        },
        key_catchers: None,
        log: CallLog::default(),
    };
    let argc = usize::try_from(args.i32(0)).unwrap_or(0);
    let second = args.text(1).to_vec();
    let words: Vec<&[u8]> = (0..argc)
        .map(|i| {
            if i == 1 {
                second.as_slice()
            } else {
                b"connect".as_slice()
            }
        })
        .collect();
    let result = connect::connect_command(&mut cl, &words, &mut host);

    // What the call changed (also when it ends in an error).
    let cls = args.region(4);
    put(cls, 0, cl.connection_state)?;
    if cod2 {
        put(cls, 4, cl.qport)?;
    } else if let Some(v) = host.key_catchers {
        put(cls, 4, v)?;
    }
    let clc = args.region(5);
    let address = guest_address(game, &address_bytes(&cl.server_address));
    clc[..20].copy_from_slice(&address);
    put(clc, CONNECT_TIME, cl.connect_time)?;
    put(clc, CONNECT_COUNT, cl.connect_packet_count)?;
    clc[SERVER_MESSAGE..SERVER_MESSAGE + SERVER_MESSAGE_SIZE].copy_from_slice(&cl.server_message);
    args.region(6)[..SERVER_NAME_SIZE].copy_from_slice(&cl.server_name);
    if cod2 {
        *args.region(7) = host.keys;
    }
    if let Some(ui) = args.buffer(3) {
        *ui = host.ui;
    }
    *args.buffer(log_at).ok_or("NULL call log")? = host.log.0;

    let printed: Vec<&[u8]> = match result.map_err(game_error)? {
        ConnectResult::Connecting => vec![RESOLVED_FORMAT.as_bytes()],
        other => vec![other.message_format().unwrap_or("").as_bytes()],
    };
    Ok(Ret::Printed(Some(printed.join(&0u8))))
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    match function {
        "CL_Connect_f" => Some(answer(game, args)),
        _ => None,
    }
}
