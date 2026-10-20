//! The `connect` console command: what the client does when the player
//! names a server to join (`connect <server>`), before any packet goes out:
//! the local game stopped, the name resolved, the connection reset so that
//! the next frame sends `getchallenge` ([`super::check_for_resend`]).
//!
//! What it does to the rest of the game (sounds, the local server, the
//! console, the menus, name resolution, the settings) goes through
//! [`ConnectHost`]. The CD key check the originals make before joining a
//! server on the network is the host's answer
//! ([`ConnectHost::cd_key_valid`]); it is not ported (the Rust client sends
//! no CD key).

use super::ClientState;
use crate::net::netchan::{AddressType, NetAddress};
use crate::{Game, GameError};

/// The port a server name without one gets (`PORT_SERVER`).
pub const DEFAULT_SERVER_PORT: u16 = 28960;

/// What `connect` needs from the rest of the game. The originals call these
/// in the order [`connect_command`] documents.
pub trait ConnectHost {
    /// Whether this computer runs a server (the `sv_running` setting).
    fn server_running(&mut self) -> bool;
    /// Every sound stopped (Call of Duty 2 `SND_StopSounds(0)`, United
    /// Offensive `S_StopAllSounds(0)`).
    fn stop_sounds(&mut self);
    /// A setting changed (`Cvar_Set2(name, value, 1)`; United Offensive
    /// only).
    fn set_setting(&mut self, name: &[u8], value: &[u8]);
    /// Joining `localhost` while this computer runs a server. Call of Duty
    /// 2: the menus' "server quit" flag set; United Offensive: the local
    /// game shut down with "EXE_SERVERQUIT" (its `0x00439DA0`, ours
    /// `Com_ShutdownServer`).
    fn local_server_quit(&mut self);
    /// Call of Duty 2: the menus' two connection texts emptied.
    fn clear_menu_texts(&mut self);
    /// One server frame (`SV_Frame(0)`): a local server told to stop does.
    fn server_frame(&mut self);
    /// The current connection ended (`CL_Disconnect`; United Offensive
    /// passes 1).
    fn disconnect(&mut self);
    /// The console closed (`Con_Close`).
    fn close_console(&mut self);
    /// `name` resolved into `address` (`NET_StringToAdr`); false: not a
    /// server address (`address` is then the host's to leave or clear).
    fn resolve(&mut self, name: &[u8], address: &mut NetAddress) -> bool;
    /// Whether the player's CD key passes the game's own check
    /// (`CL_CDKeyValidate`; asked only for a server on the network).
    fn cd_key_valid(&mut self) -> bool;
    /// Which parts of the game take the player's keys cleared (Quake III's
    /// `cls.keyCatchers`: Call of Duty 2's word at 0x0096B654 and byte at
    /// 0x0096B658, United Offensive's word at 0x04AD3D64).
    fn clear_key_catchers(&mut self);
    /// The client's `net_qport` setting (Call of Duty 2 keeps a copy for
    /// the connect text; United Offensive reads the setting itself later).
    fn net_qport(&mut self) -> i32;
    /// Call of Duty 2: every menu closed (`UI_CloseAll`) and the screen
    /// drawn once (`SCR_UpdateScreen`).
    fn close_menus(&mut self);
}

/// How `connect` ended (besides an error).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectResult {
    /// Not one argument: "usage: connect [server]" printed, nothing done.
    Usage,
    /// The name is no server address: "Bad server address" printed, the
    /// connection state 0.
    BadAddress,
    /// Joining: the next frame sends `getchallenge` (or `connect`, for a
    /// loopback or bot address).
    Connecting,
}

impl ConnectResult {
    /// What the original prints, as its format.
    pub fn message_format(self) -> Option<&'static str> {
        match self {
            ConnectResult::Usage => Some("usage: connect [server]\n"),
            ConnectResult::BadAddress => Some("Bad server address\n"),
            ConnectResult::Connecting => None,
        }
    }
}

/// The line `connect` prints once the name resolved.
pub const RESOLVED_FORMAT: &str = "%s resolved to %i.%i.%i.%i:%i\n";

/// The `connect` command with the console's words `args` (the command
/// itself first): one argument, the server's name or address.
///
/// In order: every sound stopped (United Offensive: also `r_uiFullScreen`
/// 0), the server's last message cleared (its first byte), a local server
/// told to quit when the name is exactly `localhost` and one runs (United
/// Offensive then sets `sv_killserver` 1), one server frame, the
/// connection ended, the console closed, the name kept (`cls.servername`,
/// at most 255 bytes) and resolved: when it doesn't, the connection state
/// is 0 (United Offensive: `ui_dl_running` 0). An address without a port
/// gets [`DEFAULT_SERVER_PORT`]; it is printed ([`RESOLVED_FORMAT`]). A
/// server on the network (not loopback, not a bot) needs a valid CD key,
/// else the error `EXE_ERR_INVALID_CD_KEY` (dropped). Then the state is
/// "challenging" for loopback and bot addresses, "connecting" for the rest
/// (Call of Duty 2 4 / 3, United Offensive 2 / 1), the key catchers cleared,
/// the connect time -99999 (send at once), no connect packets yet; Call of
/// Duty 2 copies `net_qport`, closes the menus and draws the screen, United
/// Offensive sets `cl_currentServerAddress` to the name as typed.
///
/// Original: `CL_Connect_f` at 0x004B3900 (CoD2MP_s.exe 1.3)
/// Original: `CL_Connect_f` at 0x00410FE0 (CoDUOMP.exe 1.51)
pub fn connect_command(
    cl: &mut ClientState,
    args: &[&[u8]],
    host: &mut dyn ConnectHost,
) -> Result<ConnectResult, GameError> {
    if args.len() != 2 {
        return Ok(ConnectResult::Usage);
    }
    let cod2 = cl.game == Game::CallOfDuty2;
    host.stop_sounds();
    if !cod2 {
        host.set_setting(b"r_uiFullScreen", b"0");
    }
    if let Some(first) = cl.server_message.first_mut() {
        *first = 0;
    }
    let server = args[1];
    // (The name is a C string: up to its first NUL.)
    let typed = &server[..server.iter().position(|&c| c == 0).unwrap_or(server.len())];
    if host.server_running() && typed == b"localhost" {
        host.local_server_quit();
    }
    if cod2 {
        host.clear_menu_texts();
    } else {
        host.set_setting(b"sv_killserver", b"1");
    }
    host.server_frame();
    host.disconnect();
    host.close_console();

    // cls.servername: strncpy(name, server, 255), the last byte 0.
    let n = typed.len().min(super::SERVER_NAME_SIZE - 1);
    cl.server_name.fill(0);
    cl.server_name[..n].copy_from_slice(&typed[..n]);
    let name = cl.server_name[..n].to_vec();

    if !host.resolve(&name, &mut cl.server_address) {
        cl.connection_state = 0;
        if !cod2 {
            host.set_setting(b"ui_dl_running", b"0");
        }
        return Ok(ConnectResult::BadAddress);
    }
    if cl.server_address.port == 0 {
        cl.server_address.port = DEFAULT_SERVER_PORT;
    }
    let local = matches!(
        cl.server_address.kind,
        AddressType::Loopback | AddressType::Bot
    );
    if !local && !host.cd_key_valid() {
        return Err(GameError::drop("EXE_ERR_INVALID_CD_KEY"));
    }
    let (connecting, challenging) = if cod2 { (3, 4) } else { (1, 2) };
    cl.connection_state = if local { challenging } else { connecting };
    host.clear_key_catchers();
    cl.connect_time = -99999;
    cl.connect_packet_count = 0;
    if cod2 {
        cl.qport = host.net_qport();
        host.close_menus();
    } else {
        // (The name as typed, not the 255-byte copy.)
        host.set_setting(b"cl_currentServerAddress", typed);
    }
    Ok(ConnectResult::Connecting)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Log {
        calls: Vec<String>,
        resolves: Option<NetAddress>,
        key: bool,
        running: bool,
    }

    impl ConnectHost for Log {
        fn server_running(&mut self) -> bool {
            self.running
        }
        fn stop_sounds(&mut self) {
            self.calls.push("sounds".into());
        }
        fn set_setting(&mut self, name: &[u8], value: &[u8]) {
            self.calls.push(format!(
                "set {} {}",
                String::from_utf8_lossy(name),
                String::from_utf8_lossy(value)
            ));
        }
        fn local_server_quit(&mut self) {
            self.calls.push("quit".into());
        }
        fn clear_menu_texts(&mut self) {
            self.calls.push("texts".into());
        }
        fn server_frame(&mut self) {
            self.calls.push("frame".into());
        }
        fn disconnect(&mut self) {
            self.calls.push("disconnect".into());
        }
        fn close_console(&mut self) {
            self.calls.push("console".into());
        }
        fn resolve(&mut self, _name: &[u8], address: &mut NetAddress) -> bool {
            match self.resolves {
                Some(a) => {
                    *address = a;
                    true
                }
                None => false,
            }
        }
        fn cd_key_valid(&mut self) -> bool {
            self.calls.push("key".into());
            self.key
        }
        fn clear_key_catchers(&mut self) {}
        fn net_qport(&mut self) -> i32 {
            0x1234
        }
        fn close_menus(&mut self) {
            self.calls.push("menus".into());
        }
    }

    #[test]
    fn joins_a_network_server_with_the_default_port() {
        let mut cl = ClientState::new(Game::CallOfDuty2);
        let mut host = Log {
            resolves: Some(NetAddress::ip([192, 168, 0, 2], 0)),
            key: true,
            ..Log::default()
        };
        let r = connect_command(&mut cl, &[b"connect", b"lan-box"], &mut host).unwrap();
        assert_eq!(r, ConnectResult::Connecting);
        assert_eq!(cl.connection_state, 3);
        assert_eq!(cl.server_address.port, DEFAULT_SERVER_PORT);
        assert_eq!(cl.connect_time, -99999);
        assert_eq!(cl.qport, 0x1234);
        assert_eq!(&cl.server_name[..8], b"lan-box\0");
        assert!(host.calls.contains(&"key".to_string()));
    }

    #[test]
    fn a_bad_cd_key_drops_and_loopback_needs_none() {
        let mut cl = ClientState::new(Game::UnitedOffensive);
        let mut host = Log {
            resolves: Some(NetAddress::ip([10, 0, 0, 1], 28961)),
            ..Log::default()
        };
        assert!(connect_command(&mut cl, &[b"connect", b"x"], &mut host).is_err());
        let mut loopback = NetAddress::ip([127, 0, 0, 1], 0);
        loopback.kind = AddressType::Loopback;
        host.resolves = Some(loopback);
        host.calls.clear();
        let r = connect_command(&mut cl, &[b"connect", b"localhost"], &mut host).unwrap();
        assert_eq!(r, ConnectResult::Connecting);
        assert_eq!(cl.connection_state, 2);
        assert!(!host.calls.contains(&"key".to_string()));
        assert_eq!(
            host.calls.last().unwrap(),
            "set cl_currentServerAddress localhost"
        );
    }

    #[test]
    fn usage_and_bad_address() {
        let mut cl = ClientState::new(Game::UnitedOffensive);
        let mut host = Log::default();
        assert_eq!(
            connect_command(&mut cl, &[b"connect"], &mut host).unwrap(),
            ConnectResult::Usage
        );
        assert!(host.calls.is_empty());
        cl.connection_state = 5;
        assert_eq!(
            connect_command(&mut cl, &[b"connect", b"nowhere"], &mut host).unwrap(),
            ConnectResult::BadAddress
        );
        assert_eq!(cl.connection_state, 0);
        assert_eq!(host.calls.last().unwrap(), "set ui_dl_running 0");
    }
}
