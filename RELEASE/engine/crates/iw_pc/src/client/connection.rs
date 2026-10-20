//! A client connection, put together from the ported client pieces: it
//! joins a server of the original PC games (a CB or Steam copy's dedicated
//! server, or this crate's [`crate::server::Server`]) and keeps a player
//! in the game, the way the originals' client frame does. It plays no game:
//! there is no client game, no map and no input, so its player stands where
//! the server puts it and sends empty commands. It is what a Rust client
//! builds on, and a way to check that one can join.
//!
//! The pieces, in the order a frame runs them (each checked against the
//! games' own, see `tools/decomp/x86/specs/net_client.py`):
//!
//! * [`super::check_for_resend`]: `getchallenge`, then `connect`, while joining;
//! * [`super::packet_event`] / [`super::connectionless_packet`]: what comes in
//!   (the challenge, the connect answer, the server's messages, which
//!   [`super::parse_server_message`] reads);
//! * [`super::system_info_changed`] after a game state;
//! * [`super::set_cgame_time`]: the clock, which the first snapshot starts;
//! * [`super::create_new_commands`] with [`super::finish_move`],
//!   [`super::ready_to_send_packet`] and [`super::write_packet`]: the
//!   player's commands.
//!
//! What the originals do between them and this leaves out or does more
//! simply: after a game state, `CL_DownloadsComplete` (downloads; loading
//! the map and the client game, `CL_InitCGame`, which sets the states 6 and
//! 7 / 4 and 5; then `cp` with the archives' checksums,
//! [`super::send_pure_checksums`], and three messages at once) is cut to the
//! states, `cp` and the three messages;
//! server commands go through [`super::get_server_command`] (config strings
//! applied, `w` ends the connection) and the rest are handed out
//! ([`Event::ServerCommand`]); the connection itself starts as `CL_Connect_f`
//! leaves it ([`Connection::connect`]).

use super::download::{self, DownloadHost, DownloadState};
use super::{
    check_for_resend, connectionless_packet, create_new_commands, finish_move, get_server_command,
    packet_event, playing_states, ready_to_send_packet, send_pure_checksums, set_cgame_time,
    system_info_changed, write_packet, ClientPacket, ClientState, ClockParts, ClockSettings,
    InputTiming, PacketOutcome, Resend, ResendSettings, SendSettings, ServerCommand,
    ServerMessageParts, SystemInfo, SYSTEM_INFO,
};
use crate::net::delta::{self, FieldTables};
use crate::net::huffman::MessageTree;
use crate::net::msg::Msg;
use crate::net::netchan::NetAddress;
use crate::Game;

/// The player's side of joining.
#[derive(Debug, Clone)]
pub struct JoinSettings {
    /// The player's settings as an info string (`\name\...\rate\...`): the
    /// connect text adds `protocol`, `challenge` and `qport`.
    pub userinfo: Vec<u8>,
    /// Call of Duty 2's `getchallenge` carries a hash of the player's CD key
    /// (`CL_BuildMd5StrFromCDKey`, not ported: a server on the local network
    /// only keeps it, as the player's PunkBuster id). Empty: none.
    pub key_hash: Vec<u8>,
    /// `net_lanauthorize`.
    pub lan_authorize: bool,
    /// The machine's own IPv4 addresses (for what counts as the LAN).
    pub local_addresses: Vec<[u8; 4]>,
    /// `cl_maxpackets` (30 in both games).
    pub max_packets: i32,
    /// `cl_packetdup`, `cl_nodelta`.
    pub send: SendSettings,
    /// The clock's settings (`timescale` and the others).
    pub clock: ClockSettings,
    /// The archives the player's game has loaded and used, for the pure
    /// checksums sent after each game state (`cp`). Empty: none used, which
    /// a pure Call of Duty 2 server accepts (`cp @ <feed>`); a pure United
    /// Offensive server wants its client game's and user interface's
    /// archives (docs/PC_MULTIPLAYER.md, section 8).
    pub archives: Vec<crate::fs::LoadedArchive>,
    /// `cl_timeout`, `cl_connectTimeout` and the pause settings.
    pub timeout: super::TimeoutSettings,
}

impl JoinSettings {
    /// A player called `name` with the games' default rate.
    pub fn new(name: &str) -> JoinSettings {
        JoinSettings {
            userinfo: format!("\\name\\{name}\\rate\\25000\\snaps\\20").into_bytes(),
            key_hash: Vec::new(),
            lan_authorize: false,
            local_addresses: Vec::new(),
            max_packets: 30,
            send: SendSettings::default(),
            clock: ClockSettings::default(),
            archives: Vec::new(),
            timeout: super::TimeoutSettings::default(),
        }
    }
}

/// What happened, for the caller.
#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    /// `getchallenge` went out (`authorize`: the original would also ask the
    /// key server first; this doesn't).
    ChallengeSent { authorize: bool },
    /// The server's challenge arrived.
    Challenged(i32),
    /// `connect` went out.
    ConnectSent,
    /// The server took the player (`connectResponse`).
    Connected,
    /// A game state: the server's settings for the client to take.
    GameState(SystemInfo),
    /// The first snapshot with the player in the game: playing.
    Active,
    /// A new snapshot (its server time).
    Snapshot(i32),
    /// A reliable command from the server for the client game (its
    /// sequence number and text; [`super::get_server_command`] has applied
    /// config strings and map restarts already).
    ServerCommand(i32, Vec<u8>),
    /// A config string changed (its index); the system info's
    /// ([`super::SYSTEM_INFO`]) comes with the settings it holds.
    ConfigString(usize, Option<SystemInfo>),
    /// `print` (the text).
    Print(Vec<u8>),
    /// The server redirected a download to a web address and asked for it
    /// to be opened (the originals open it with the shell and quit): not
    /// done. Call of Duty 2 drops the connection then, as the original does
    /// when the address can't be opened; United Offensive goes on.
    OpenUrlRefused(Vec<u8>),
    /// The connection ended: the server's `w` command (its reason) or
    /// `disconnect` packet, its `error`, or the game's error in a message.
    Disconnected(String),
}

/// The game-state follow-ups (the client's `ServerMessageParts`).
struct Parts<'a> {
    game_state: bool,
    download: &'a mut DownloadState,
    /// A download message's error (the connection drops).
    error: Option<crate::GameError>,
    /// A web address the server asked to open.
    url: Option<Vec<u8>>,
}

/// What this client does for downloads: it asks for none (so a server's
/// blocks get `stopdl`, as in the originals), starts no web downloads, and
/// opens no web pages.
struct NoDownloads<'a> {
    url: &'a mut Option<Vec<u8>>,
}

impl DownloadHost for NoDownloads<'_> {
    fn open(&mut self, _temp_name: &[u8]) -> i32 {
        0
    }
    fn write(&mut self, _data: &[u8], _length: i32, _file: i32) {}
    fn close(&mut self, _file: i32) {}
    fn rename(&mut self, _from: &[u8], _to: &[u8]) {}
    fn write_packet(&mut self) {}
    fn next_download(&mut self) {}
    fn set_size(&mut self, _size: i32) {}
    fn set_count(&mut self, _count: i32) {}
    fn clear_name(&mut self) {}
    fn home_path(&mut self) -> Vec<u8> {
        Vec::new()
    }
    fn os_path(&mut self, base: &[u8], game: &[u8], qpath: &[u8]) -> Vec<u8> {
        [base, b"/", game, b"/", qpath].concat()
    }
    fn open_url(&mut self, url: &[u8]) -> bool {
        *self.url = Some(url.to_vec());
        false
    }
    fn add_text(&mut self, _text: &[u8]) {}
    fn begin_download(&mut self, _local: &[u8], _remote: &[u8]) -> bool {
        false
    }
    fn end_download(&mut self) {}
}

impl ServerMessageParts for Parts<'_> {
    fn game_state_parsed(&mut self, _cl: &mut ClientState) -> bool {
        self.game_state = true;
        true
    }
    fn download(&mut self, msg: &mut Msg, cl: &mut ClientState) -> bool {
        let mut host = NoDownloads { url: &mut self.url };
        match download::parse_download(msg, cl, self.download, &mut host) {
            Ok(_) => true,
            Err(e) => {
                self.error = Some(e);
                false
            }
        }
    }
}

/// The clock's caller parts: no demos, no activeAction.
struct Clock {
    active: bool,
}

impl ClockParts for Clock {
    fn became_active(&mut self, _cl: &mut ClientState) {
        self.active = true;
    }
    fn read_demo_message(&mut self, _cl: &mut ClientState) -> bool {
        false
    }
    fn time_demo_frame(&mut self, _cl: &mut ClientState) -> bool {
        false
    }
}

/// One connection to one server.
pub struct Connection {
    pub cl: ClientState,
    pub settings: JoinSettings,
    tree: MessageTree,
    tables: FieldTables,
    timing: InputTiming,
    /// The last server command handed out.
    commands_seen: i32,
    /// The words of the last server command (`Cmd_Argv`).
    tokens: Box<crate::net::oob::Tokens>,
    /// A big config string being put together.
    big_config_string: Box<[u8; super::BIG_CONFIG_STRING_SIZE]>,
    /// The settings' changed flags (the originals' `dvar_modifiedFlags`; 2:
    /// the player's settings, sent again by [`super::check_userinfo`]).
    modified_flags: i32,
    /// Downloads (none asked for; see `NoDownloads`).
    download: DownloadState,
    disconnected: bool,
}

/// The driver's side of the `connect` command: the address it was given,
/// nothing else running, no console, no menus, no CD key.
struct DriverConnect {
    server: NetAddress,
    qport: i32,
}

impl super::connect::ConnectHost for DriverConnect {
    fn server_running(&mut self) -> bool {
        false
    }
    fn stop_sounds(&mut self) {}
    fn set_setting(&mut self, _name: &[u8], _value: &[u8]) {}
    fn local_server_quit(&mut self) {}
    fn clear_menu_texts(&mut self) {}
    fn server_frame(&mut self) {}
    fn disconnect(&mut self) {}
    fn close_console(&mut self) {}
    fn resolve(&mut self, _name: &[u8], address: &mut NetAddress) -> bool {
        *address = self.server;
        true
    }
    fn cd_key_valid(&mut self) -> bool {
        true
    }
    fn clear_key_catchers(&mut self) {}
    fn net_qport(&mut self) -> i32 {
        self.qport
    }
    fn close_menus(&mut self) {}
}

impl Connection {
    /// A client of `game` with the protocol's tables (read from the
    /// player's executable).
    pub fn new(
        game: Game,
        tree: MessageTree,
        tables: FieldTables,
        settings: JoinSettings,
    ) -> Connection {
        Connection {
            cl: ClientState::new(game),
            settings,
            tree,
            tables,
            timing: InputTiming::default(),
            commands_seen: 0,
            tokens: Box::default(),
            big_config_string: Box::new([0; super::BIG_CONFIG_STRING_SIZE]),
            modified_flags: 0,
            download: DownloadState::default(),
            disconnected: false,
        }
    }

    /// Starts joining `server` (resolved already) as the `connect` command
    /// does ([`super::connect::connect_command`], with the address as the
    /// name, nothing else running, the console and menus left out): the
    /// server's last message cleared, its address (port 28960 when none),
    /// sending `getchallenge` at once (a loopback or bot address skips to
    /// `connect`), the qport. The originals' CD key check is not made: this
    /// client sends no CD key.
    pub fn connect(&mut self, server: NetAddress, qport: i32, now: i32) {
        let [a, b, c, d] = server.ip;
        let name = match server.port {
            0 => format!("{a}.{b}.{c}.{d}"),
            port => format!("{a}.{b}.{c}.{d}:{port}"),
        };
        let mut host = DriverConnect { server, qport };
        let args: [&[u8]; 2] = [b"connect", name.as_bytes()];
        // (Only a failed CD key check errs, and the driver's passes.)
        let _ = super::connect::connect_command(&mut self.cl, &args, &mut host);
        let cl = &mut self.cl;
        cl.qport = qport;
        cl.realtime = now;
        cl.sim_time = now;
        self.disconnected = false;
        self.commands_seen = cl.server_command_sequence;
    }

    /// New settings for the player (`\\name\\...`): sent to the server
    /// in the next frame once joining, as `userinfo` (the originals do it
    /// when a setting with the user-info flag changes).
    pub fn set_userinfo(&mut self, userinfo: Vec<u8>) {
        self.settings.userinfo = userinfo;
        self.modified_flags |= 2;
    }

    /// Whether the connection ended.
    pub fn disconnected(&self) -> bool {
        self.disconnected
    }

    /// Whether the player is in the game.
    pub fn active(&self) -> bool {
        self.cl.connection_state == playing_states(self.cl.game).2
    }

    fn end(&mut self, why: String, events: &mut Vec<Event>) {
        self.disconnected = true;
        self.cl.connection_state = 0;
        events.push(Event::Disconnected(why));
    }

    /// A packet from `from` (`data`, as it arrived) at client time `now`;
    /// `send` gets anything to send back.
    pub fn packet(
        &mut self,
        from: &NetAddress,
        data: &[u8],
        now: i32,
        send: &mut dyn FnMut(&NetAddress, &[u8]),
    ) -> Vec<Event> {
        let mut events = Vec::new();
        if self.disconnected {
            return events;
        }
        self.cl.realtime = now;
        let game = self.cl.game;
        let mut buffer = data.to_vec();
        buffer.resize(data.len() + 64, 0);
        let mut msg = Msg::new(game, &mut buffer);
        msg.cursize = data.len();
        let mut parts = Parts {
            game_state: false,
            download: &mut self.download,
            error: None,
            url: None,
        };
        let outcome = match packet_event(
            &mut self.cl,
            from,
            &mut msg,
            &self.tree,
            &self.tables,
            &mut parts,
        ) {
            Ok(outcome) => outcome,
            Err(e) => {
                self.end(format!("{e}"), &mut events);
                return events;
            }
        };
        let Parts {
            game_state: got_game_state,
            error,
            url,
            ..
        } = parts;
        if let Some(url) = url {
            events.push(Event::OpenUrlRefused(url));
        }
        if let Some(e) = error {
            self.end(format!("{e}"), &mut events);
            return events;
        }
        match outcome {
            PacketOutcome::Connectionless => {
                match connectionless_packet(&mut self.cl, from, &mut msg, send) {
                    ClientPacket::ChallengeTaken => {
                        events.push(Event::Challenged(self.cl.challenge))
                    }
                    ClientPacket::Connected => events.push(Event::Connected),
                    ClientPacket::Print => {
                        let text = &self.cl.server_message;
                        events.push(Event::Print(text[..crate::cstr::len(text)].to_vec()));
                    }
                    ClientPacket::ServerError(text) => {
                        self.end(String::from_utf8_lossy(&text).into_owned(), &mut events);
                    }
                    ClientPacket::Disconnect => self.end("disconnect".into(), &mut events),
                    ClientPacket::NeedCdKey => {
                        self.end("EXE_AWAITINGCDKEYAUTH".into(), &mut events)
                    }
                    _ => {}
                }
            }
            PacketOutcome::Read { .. } => {
                if got_game_state {
                    self.game_state(&mut events, send);
                }
                if self.cl.new_snapshots != 0 {
                    events.push(Event::Snapshot(self.cl.snap.server_time));
                }
                self.server_commands(&mut events);
            }
            _ => {}
        }
        events
    }

    /// After a game state: the system info, then (no downloads, no map to
    /// load) loading, primed, the pure checksums (both games send them to
    /// every server; one that isn't pure ignores them) and three messages
    /// at once (`CL_DownloadsComplete`).
    fn game_state(&mut self, events: &mut Vec<Event>, send: &mut dyn FnMut(&NetAddress, &[u8])) {
        let info = match system_info_changed(&mut self.cl, false) {
            Ok(info) => info.unwrap_or_default(),
            Err(e) => {
                self.end(format!("{e}"), events);
                return;
            }
        };
        events.push(Event::GameState(info));
        let (_, primed, _) = playing_states(self.cl.game);
        self.cl.connection_state = primed;
        self.commands_seen = self.cl.server_command_sequence;
        // The fake checksum (`fs_fakeChkSum`) is the originals' mark of a
        // file read from outside any archive on a pure server: never here.
        let feed = self.cl.checksum_feed;
        let paths = crate::fs::search_path_for_feed(&self.settings.archives, feed);
        if let Err(e) = send_pure_checksums(&mut self.cl, &paths, feed, 0) {
            self.end(format!("{e}"), events);
            return;
        }
        for _ in 0..3 {
            if let Err(e) = write_packet(&mut self.cl, &self.tree, &self.settings.send, send) {
                self.end(format!("{e}"), events);
                return;
            }
        }
    }

    /// The server commands that arrived, in order, through
    /// [`get_server_command`] as the originals' client game asks for them:
    /// config strings and map restarts applied, `w` ends the connection,
    /// the rest goes to the caller.
    fn server_commands(&mut self, events: &mut Vec<Event>) {
        while self.commands_seen < self.cl.server_command_sequence {
            self.commands_seen += 1;
            let number = self.commands_seen;
            let text = self.cl.server_command(number).to_vec();
            let command = get_server_command(
                &mut self.cl,
                number,
                &mut self.tokens,
                &mut self.big_config_string,
            );
            match command {
                Err(e) => {
                    self.end(format!("{e}"), events);
                    return;
                }
                Ok(ServerCommand::Disconnected { reason, .. }) => {
                    let why = reason.map_or("EXE_SERVER_DISCONNECTED".to_string(), |r| {
                        String::from_utf8_lossy(&r).into_owned()
                    });
                    self.end(why, events);
                    return;
                }
                Ok(ServerCommand::ConfigString(Some(index))) => {
                    let mut info = None;
                    if index == SYSTEM_INFO {
                        match system_info_changed(&mut self.cl, false) {
                            Ok(changed) => info = changed,
                            Err(e) => {
                                self.end(format!("{e}"), events);
                                return;
                            }
                        }
                    }
                    events.push(Event::ConfigString(index, info));
                }
                Ok(ServerCommand::Endless) => {
                    self.end(
                        "a server command the original never finishes".to_string(),
                        events,
                    );
                    return;
                }
                Ok(ServerCommand::Kept) | Ok(ServerCommand::ConfigString(None)) => {}
                Ok(ServerCommand::ClientGame) | Ok(ServerCommand::MapRestart) => {
                    events.push(Event::ServerCommand(number, text));
                }
            }
        }
    }

    /// One client frame at time `now` (milliseconds): changed settings,
    /// the time-out, resend while joining, the clock, the player's command,
    /// the message to the server when it's time.
    pub fn frame(&mut self, now: i32, send: &mut dyn FnMut(&NetAddress, &[u8])) -> Vec<Event> {
        let mut events = Vec::new();
        if self.disconnected {
            return events;
        }
        self.cl.realtime = now;
        self.cl.sim_time = now;
        let paused = self.settings.timeout.client_paused;
        let info = self.settings.userinfo.clone();
        // (A warning, a cut command, is the original's print only.)
        if let Some(Err(e)) =
            super::check_userinfo(&mut self.cl, paused, &mut self.modified_flags, &info)
        {
            self.end(format!("{e}"), &mut events);
            return events;
        }
        if let Err(e) = super::check_timeout(&mut self.cl, &self.settings.timeout) {
            self.end(format!("{e}"), &mut events);
            return events;
        }
        let local = self.settings.local_addresses.clone();
        let resend = ResendSettings {
            userinfo: &self.settings.userinfo,
            key_hash: &self.settings.key_hash,
            lan_authorize: self.settings.lan_authorize,
            local_addresses: &local,
        };
        match check_for_resend(&mut self.cl, &resend, send) {
            Resend::Challenge { authorize } => events.push(Event::ChallengeSent { authorize }),
            Resend::Connect => events.push(Event::ConnectSent),
            Resend::Nothing => {}
        }
        let mut clock = Clock { active: false };
        if let Err(e) = set_cgame_time(&mut self.cl, &self.settings.clock, &mut clock) {
            self.end(format!("{e}"), &mut events);
            return events;
        }
        if clock.active {
            events.push(Event::Active);
        }
        let connected = match self.cl.game {
            Game::CallOfDuty2 => 5,
            Game::UnitedOffensive => 3,
        };
        if self.cl.connection_state < connected {
            return events;
        }
        let game = self.cl.game;
        create_new_commands(&mut self.cl, now, &mut self.timing, &mut |cl, _| {
            // No input: an empty command, finished as the originals finish one.
            let mut cmd = vec![0u8; delta::usercmd_size(game)];
            finish_move(cl, &mut cmd);
            cmd
        });
        let mut max_packets = self.settings.max_packets;
        if ready_to_send_packet(&self.cl, false, &local, &mut max_packets) {
            if let Err(e) = write_packet(&mut self.cl, &self.tree, &self.settings.send, send) {
                self.end(format!("{e}"), &mut events);
            }
        }
        self.settings.max_packets = max_packets;
        events
    }
}
