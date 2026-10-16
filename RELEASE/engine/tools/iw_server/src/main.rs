//! `iw-server`: a LAN game server for Call of Duty 2 1.3 and United
//! Offensive 1.51 that the games' own PC clients (the player's CB or Steam
//! copy) can find in their LAN browser and join. See `docs/PC_SERVER.md`.
//!
//! ```text
//! iw-server --game cod2 --map mp_toujane [--port 28960] [--exe <CoD2MP_s.exe>]
//!           [--hostname <name>] [--gametype dm] [--max-clients 16]
//! ```
//!
//! The game's executable is only read (its plain data: the protocol tables,
//! checked by hash); it is never run, copied or changed. Without `--exe` it
//! is looked for in `IW_PC_INPUT`, then in Steam's libraries.

#![forbid(unsafe_code)]

use std::net::{SocketAddr, UdpSocket};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use game_sources::{steam, GameId};
use iw_pc::net::netchan::NetAddress;
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::server::{Protocol, Server, ServerConfig};
use iw_pc::Game;

const USAGE: &str = "usage: iw-server --game cod2|uo --map <map> [--port <port>] [--exe <file>] \
[--hostname <name>] [--gametype <type>] [--max-clients <n>] [--motd <text>] \
[--set <key> <value>] [--config-string <index> <text>]";

/// The port both games use by default.
const DEFAULT_PORT: u16 = 28960;

struct Options {
    config: ServerConfig,
    port: u16,
    exe: Option<PathBuf>,
}

fn parse(args: &[String]) -> Result<Options, String> {
    let mut game = None;
    let mut map = None;
    let mut port = DEFAULT_PORT;
    let mut exe = None;
    let mut rest: Vec<(String, Vec<String>)> = Vec::new();
    let mut i = 0;
    let value = |i: usize| -> Result<String, String> {
        args.get(i + 1)
            .cloned()
            .ok_or_else(|| format!("{} needs a value\n{USAGE}", args[i]))
    };
    while i < args.len() {
        match args[i].as_str() {
            "--game" => {
                game = Some(match value(i)?.as_str() {
                    "cod2" => Game::CallOfDuty2,
                    "uo" | "coduo" => Game::UnitedOffensive,
                    other => return Err(format!("unknown game {other:?} (cod2 or uo)")),
                });
                i += 2;
            }
            "--map" => {
                map = Some(value(i)?);
                i += 2;
            }
            "--port" => {
                port = value(i)?.parse().map_err(|e| format!("--port: {e}"))?;
                i += 2;
            }
            "--exe" => {
                exe = Some(PathBuf::from(value(i)?));
                i += 2;
            }
            "--set" | "--config-string" => {
                let a = value(i)?;
                let b = args.get(i + 2).cloned().ok_or(USAGE)?;
                rest.push((args[i].clone(), vec![a, b]));
                i += 3;
            }
            "--hostname" | "--gametype" | "--max-clients" | "--motd" => {
                rest.push((args[i].clone(), vec![value(i)?]));
                i += 2;
            }
            "-h" | "--help" => return Err(USAGE.to_string()),
            other => return Err(format!("unknown option {other:?}\n{USAGE}")),
        }
    }
    let game = game.ok_or(USAGE)?;
    let map = map.ok_or(USAGE)?;
    let mut config = ServerConfig::new(game, &map);
    for (option, values) in rest {
        match option.as_str() {
            "--hostname" => config.hostname = values[0].clone(),
            "--gametype" => config.gametype = values[0].clone(),
            "--motd" => config.motd = values[0].clone(),
            "--max-clients" => {
                config.max_clients = values[0]
                    .parse::<usize>()
                    .map_err(|e| format!("--max-clients: {e}"))?
                    .clamp(1, 64)
            }
            "--set" => config
                .server_info
                .push((values[0].clone(), values[1].clone())),
            _ => {
                let index = values[0]
                    .parse::<usize>()
                    .map_err(|e| format!("--config-string: {e}"))?;
                config.config_strings.push((index, values[1].clone()));
            }
        }
    }
    Ok(Options { config, port, exe })
}

/// The executable to read the protocol tables from: `--exe`, else
/// `IW_PC_INPUT`, else an install Steam knows.
fn find_executable(game: Game, explicit: Option<PathBuf>) -> Option<PathBuf> {
    if explicit.is_some() {
        return explicit;
    }
    if let Some(folders) = std::env::var_os("IW_PC_INPUT") {
        if let Some(path) = protocol_data::find_executable(game, &folders) {
            return Some(path);
        }
    }
    let wanted = match game {
        Game::CallOfDuty2 => GameId::Cod2,
        Game::UnitedOffensive => GameId::CodUo,
    };
    let scan = steam::scan(&steam::find_roots(None));
    scan.installs
        .iter()
        .flat_map(|i| i.games.iter())
        .filter(|g| g.game == wanted)
        .map(|g| g.root.join(protocol_data::executable_name(game)))
        .find(|p| p.is_file())
}

fn address_of(from: &SocketAddr) -> Option<NetAddress> {
    match from {
        SocketAddr::V4(v4) => Some(NetAddress::ip(v4.ip().octets(), v4.port())),
        SocketAddr::V6(_) => None, // the games speak IPv4 only
    }
}

fn run(options: Options) -> Result<(), String> {
    let game = options.config.game;
    let path = find_executable(game, options.exe).ok_or_else(|| {
        format!(
            "{} not found: pass --exe <path to it>, or set IW_PC_INPUT to its folder",
            protocol_data::executable_name(game)
        )
    })?;
    let exe = GameExecutable::open(game, &path).map_err(|e| e.to_string())?;
    let protocol = Protocol::from_executable(&exe).map_err(|e| e.to_string())?;
    drop(exe);
    println!(
        "protocol tables read from {} ({})",
        path.display(),
        protocol_data::supported_build(game)
    );
    let socket = UdpSocket::bind(("0.0.0.0", options.port))
        .map_err(|e| format!("port {}: {e}", options.port))?;
    socket
        .set_read_timeout(Some(Duration::from_millis(5)))
        .map_err(|e| e.to_string())?;
    let seed = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(1, |d| d.subsec_nanos() ^ d.as_secs() as u32);
    let map = options.config.map.clone();
    let mut server = Server::new(options.config, protocol, seed);
    println!(
        "{} server on UDP port {}, map {map}: LAN browser or `connect <this machine's address>:{}`",
        match game {
            Game::CallOfDuty2 => "Call of Duty 2 1.3",
            Game::UnitedOffensive => "United Offensive 1.51",
        },
        options.port,
        options.port
    );
    let mut buffer = [0u8; 0x10000];
    let mut last = Instant::now();
    let mut players = 0;
    loop {
        let mut send = |to: &NetAddress, bytes: &[u8]| {
            let address = SocketAddr::from((to.ip, to.port));
            let _ = socket.send_to(bytes, address);
        };
        match socket.recv_from(&mut buffer) {
            Ok((n, from)) => {
                if let Some(address) = address_of(&from) {
                    server.packet(&address, &buffer[..n], &mut send);
                }
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(e) if e.kind() == std::io::ErrorKind::ConnectionReset => {} // Windows: an ICMP reply
            Err(e) => return Err(e.to_string()),
        }
        let now = Instant::now();
        let elapsed = now.duration_since(last).as_millis() as i32;
        if elapsed > 0 {
            last = now;
            server.frame(elapsed, &mut send);
        }
        let count = server.player_count();
        if count != players {
            println!("players: {count}");
            players = count;
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match parse(&args).and_then(run) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
