//! What the server tells clients about itself: its settings, and the config
//! strings a client needs to load the map.
//!
//! # The config strings sent
//!
//! | Index | What | Evidence |
//! |---|---|---|
//! | 0 | server info (`\key\value` pairs): host name, map, game type, protocol, slots | `CS_SERVERINFO`, Quake III's; read by the client's `CL_ServerInfoPacket`, `CL_DownloadsComplete` (`mapname`, `g_gametype`), the client game (`sv_maxclients`, `sv_hostname`) |
//! | 1 | system info: `sv_serverid`, `sv_pure`, the archive lists, `sv_cheats` | `CS_SYSTEMINFO`; every key read by Call of Duty 2's `CL_SystemInfoChanged` |
//! | 2 | the game version, `cod` | `CG_Init` stops with "Client/Server game mismatch" unless it is `cod` (both games' client game) |
//! | 3, 4, 11 | ambient sound (empty), map message (empty), north yaw (`0`) | set by the game module's world spawn |
//! | 13 | the level's start time | set by the game module's world spawn |
//! | 14 | the message of the day | set by the game module's world spawn |
//!
//! Indices 3 to 14 are Call of Duty 2's (`CS_AMBIENT` ... `CS_MOTD`); United
//! Offensive's are taken to be the same until checked in its game module.
//! Everything else a full game sets (models, sounds, effects, menus, teams)
//! comes from the game module, which this server doesn't run yet.

use crate::Game;

/// Extra `\key\value` pairs for the server info string, beyond the ones
/// [`ServerConfig`] fills.
pub type ServerInfoKeys = Vec<(String, String)>;

/// The server's settings.
#[derive(Debug, Clone)]
pub struct ServerConfig {
    pub game: Game,
    /// The map to load, e.g. `mp_toujane` (the client loads it from its own files).
    pub map: String,
    /// The name in server lists (`sv_hostname`).
    pub hostname: String,
    /// The game type, e.g. `dm`, `tdm`, `sd` (`g_gametype`).
    pub gametype: String,
    /// Player slots (`sv_maxclients`).
    pub max_clients: usize,
    /// Server frames per second (`sv_fps`; 20 in both games).
    pub fps: u32,
    /// The message of the day (`CS_MOTD`).
    pub motd: String,
    /// More server info keys, or replacements for the default ones.
    pub server_info: ServerInfoKeys,
    /// Config strings to set or replace, by index.
    pub config_strings: Vec<(usize, String)>,
}

impl ServerConfig {
    /// Defaults for `game` on `map`.
    pub fn new(game: Game, map: &str) -> ServerConfig {
        ServerConfig {
            game,
            map: map.to_string(),
            hostname: "Rust server".to_string(),
            gametype: "dm".to_string(),
            max_clients: 16,
            fps: 20,
            motd: String::new(),
            server_info: Vec::new(),
            config_strings: Vec::new(),
        }
    }

    /// The player movement type the server gives players while it runs no
    /// game module: free-flying spectators (`PM_SPECTATOR`, 4 in Call of
    /// Duty 2; assumed the same in United Offensive).
    pub fn spectator_pm_type(&self, _game: Game) -> i32 {
        4
    }

    /// The server info string (`CS_SERVERINFO`).
    pub fn server_info(&self, game: Game) -> Vec<u8> {
        let (protocol, version, game_name) = match game {
            Game::CallOfDuty2 => ("118", "1.3", "Call of Duty 2"),
            Game::UnitedOffensive => ("22", "1.51", "CoD:United Offensive"),
        };
        let max_clients = self.max_clients.to_string();
        let mut pairs: Vec<(&str, &str)> = vec![
            ("g_gametype", &self.gametype),
            ("gamename", game_name),
            ("mapname", &self.map),
            ("protocol", protocol),
            ("shortversion", version),
            ("sv_allowAnonymous", "0"),
            ("sv_disableClientConsole", "0"),
            ("sv_floodProtect", "1"),
            ("sv_hostname", &self.hostname),
            ("sv_maxclients", &max_clients),
            ("sv_maxPing", "0"),
            ("sv_minPing", "0"),
            ("sv_privateClients", "0"),
            ("sv_punkbuster", "0"),
            ("sv_pure", "0"),
            ("sv_voice", "0"),
        ];
        for (key, value) in &self.server_info {
            match pairs.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(key)) {
                Some(pair) => pair.1 = value,
                None => pairs.push((key, value)),
            }
        }
        info_string(&pairs)
    }

    /// The system info string (`CS_SYSTEMINFO`).
    pub fn system_info(&self, game: Game, server_id: i32) -> Vec<u8> {
        let id = server_id.to_string();
        let keys: &[(&str, &str)] = match game {
            Game::CallOfDuty2 => &[
                ("sv_cheats", "0"),
                ("sv_iwdNames", ""),
                ("sv_iwds", ""),
                ("sv_pure", "0"),
                ("sv_referencedIwdNames", ""),
                ("sv_referencedIwds", ""),
            ],
            Game::UnitedOffensive => &[
                ("sv_cheats", "0"),
                ("sv_pakNames", ""),
                ("sv_paks", ""),
                ("sv_pure", "0"),
                ("sv_referencedPakNames", ""),
                ("sv_referencedPaks", ""),
            ],
        };
        let mut pairs: Vec<(&str, &str)> = keys.to_vec();
        pairs.push(("sv_serverid", &id));
        info_string(&pairs)
    }

    /// Every config string the server sets (index, text).
    pub fn config_strings(
        &self,
        game: Game,
        server_id: i32,
        start_time: i32,
    ) -> Vec<(usize, Vec<u8>)> {
        let mut strings = vec![
            (0, self.server_info(game)),
            (1, self.system_info(game, server_id)),
            (2, b"cod".to_vec()),
            (11, b"0".to_vec()),
            (13, start_time.to_string().into_bytes()),
            (14, self.motd.clone().into_bytes()),
        ];
        for (index, text) in &self.config_strings {
            match strings.iter_mut().find(|(i, _)| i == index) {
                Some(slot) => slot.1 = text.clone().into_bytes(),
                None => strings.push((*index, text.clone().into_bytes())),
            }
        }
        strings
    }
}

/// `\key\value\key\value`; backslashes, quotes and semicolons in either are
/// dropped (the games refuse them in info strings).
fn info_string(pairs: &[(&str, &str)]) -> Vec<u8> {
    let clean = |s: &str| -> Vec<u8> {
        s.bytes()
            .filter(|&c| c != b'\\' && c != b'"' && c != b';' && c >= b' ')
            .collect()
    };
    let mut out = Vec::new();
    for (key, value) in pairs {
        out.push(b'\\');
        out.extend(clean(key));
        out.push(b'\\');
        out.extend(clean(value));
    }
    out
}
