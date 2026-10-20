//! `iw-join`: joins a Call of Duty 2 1.3 or United Offensive 1.51 server
//! with the Rust client's network code (`iw_pc::client::connection`) and
//! stays in the game as a player that stands still, printing what happens.
//! It is the client side of cross-play's first check: a CB copy's
//! dedicated server (or `iw-server`) should list the player and keep it.
//! See `docs/PC_MULTIPLAYER.md`.
//!
//! ```text
//! iw-join --game cod2|uo --server <address>[:port] [--name <name>]
//!         [--seconds <n>] [--exe <file>] [--qport <n>] [--key-hash <32 hex>]
//! ```
//!
//! The game's executable is only read (the protocol tables, checked by
//! hash); it is never run, copied or changed. Without `--exe` it is looked
//! for in `IW_PC_INPUT`, then in Steam's libraries. Call of Duty 2 sends a
//! hash of the CD key with `getchallenge`, which a server on the local
//! network only keeps (as the player's PunkBuster id): `iw-join` makes one
//! up from the name unless `--key-hash` gives one. It doesn't do the key
//! server's authorization, so servers off the LAN that check keys won't
//! take it.

#![forbid(unsafe_code)]

use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use game_sources::{steam, GameId};
use iw_pc::client::connection::{Connection, Event, JoinSettings};
use iw_pc::net::netchan::NetAddress;
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::server::Protocol;
use iw_pc::Game;

const USAGE: &str = "usage: iw-join --game cod2|uo --server <address>[:port] [--name <name>] \
[--seconds <n>] [--exe <file>] [--qport <n>] [--key-hash <32 hex digits>]";

struct Options {
    game: Game,
    server: String,
    name: String,
    seconds: u64,
    exe: Option<PathBuf>,
    qport: Option<i32>,
    key_hash: Option<String>,
}

fn parse(args: &[String]) -> Result<Options, String> {
    let mut game = None;
    let mut server = None;
    let mut name = "RustPlayer".to_string();
    let mut seconds = 60;
    let mut exe = None;
    let mut qport = None;
    let mut key_hash = None;
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
            }
            "--server" => server = Some(value(i)?),
            "--name" => name = value(i)?,
            "--seconds" => seconds = value(i)?.parse().map_err(|e| format!("--seconds: {e}"))?,
            "--exe" => exe = Some(PathBuf::from(value(i)?)),
            "--qport" => qport = Some(value(i)?.parse().map_err(|e| format!("--qport: {e}"))?),
            "--key-hash" => {
                let hash = value(i)?;
                if hash.len() != 32 || !hash.bytes().all(|c| c.is_ascii_hexdigit()) {
                    return Err("--key-hash: 32 hex digits".into());
                }
                key_hash = Some(hash);
            }
            "-h" | "--help" => return Err(USAGE.to_string()),
            other => return Err(format!("unknown option {other:?}\n{USAGE}")),
        }
        i += 2;
    }
    Ok(Options {
        game: game.ok_or(USAGE)?,
        server: server.ok_or(USAGE)?,
        name,
        seconds,
        exe,
        qport,
        key_hash,
    })
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

/// A made-up key hash for `name`: 32 hex digits (64-bit FNV-1a, twice).
fn made_up_key_hash(name: &str) -> String {
    let fnv = |seed: u64| {
        name.bytes().fold(seed, |h, c| {
            (h ^ u64::from(c)).wrapping_mul(0x0000_0100_0000_01B3)
        })
    };
    format!(
        "{:016x}{:016x}",
        fnv(0xCBF2_9CE4_8422_2325),
        fnv(0x6C62_272E_07BB_0142)
    )
}

fn resolve(server: &str) -> Result<NetAddress, String> {
    let with_port = if server
        .rsplit_once(':')
        .is_some_and(|(_, p)| p.parse::<u16>().is_ok())
    {
        server.to_string()
    } else {
        format!("{server}:28960")
    };
    let address = with_port
        .to_socket_addrs()
        .map_err(|e| format!("{server}: {e}"))?
        .find_map(|a| match a {
            SocketAddr::V4(v4) => Some(v4),
            SocketAddr::V6(_) => None,
        })
        .ok_or_else(|| format!("{server}: no IPv4 address (the games speak IPv4 only)"))?;
    Ok(NetAddress::ip(address.ip().octets(), address.port()))
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

fn run(options: Options) -> Result<(), String> {
    let game = options.game;
    let path = find_executable(game, options.exe).ok_or_else(|| {
        format!(
            "{} not found: pass --exe <path to it>, or set IW_PC_INPUT to its folder",
            protocol_data::executable_name(game)
        )
    })?;
    let exe = GameExecutable::open(game, &path).map_err(|e| e.to_string())?;
    let protocol = Protocol::from_executable(&exe).map_err(|e| e.to_string())?;
    drop(exe);
    let server = resolve(&options.server)?;
    let socket = UdpSocket::bind(("0.0.0.0", 0)).map_err(|e| e.to_string())?;
    socket
        .set_read_timeout(Some(Duration::from_millis(2)))
        .map_err(|e| e.to_string())?;
    let mut settings = JoinSettings::new(&options.name);
    if game == Game::CallOfDuty2 {
        settings.key_hash = options
            .key_hash
            .unwrap_or_else(|| made_up_key_hash(&options.name))
            .into_bytes();
    }
    let started = Instant::now();
    let qport = options
        .qport
        .unwrap_or((started.elapsed().subsec_nanos() & 0xFFFF) as i32);
    let mut client = Connection::new(game, protocol.tree, protocol.tables, settings);
    let [a, b, c, d] = server.ip;
    println!(
        "joining {a}.{b}.{c}.{d}:{} as {:?} (qport {qport})",
        server.port, options.name
    );
    // The client's clock starts where the games' does: a few seconds in.
    let clock = |at: Instant| 10_000 + at.duration_since(started).as_millis() as i32;
    client.connect(server, qport, clock(Instant::now()));
    let mut buffer = [0u8; 0x20000];
    let mut snapshots = 0u64;
    let mut active_since = None;
    let mut last_frame = Instant::now();
    let deadline = started + Duration::from_secs(options.seconds);
    let report = |events: Vec<Event>, snapshots: &mut u64, active_since: &mut Option<Instant>| {
        for e in events {
            match e {
                Event::ChallengeSent { authorize } => println!(
                    "getchallenge sent{}",
                    if authorize {
                        " (the original would also ask the key server)"
                    } else {
                        ""
                    }
                ),
                Event::Challenged(n) => println!("challenge {n}"),
                Event::ConnectSent => println!("connect sent"),
                Event::Connected => println!("connected"),
                Event::GameState(info) => {
                    let settings: Vec<String> = info
                        .settings
                        .iter()
                        .map(|(k, v)| format!("{}={}", text(k), text(v)))
                        .collect();
                    println!("game state; server settings: {}", settings.join(" "));
                }
                Event::Active => {
                    println!("in the game");
                    *active_since = Some(Instant::now());
                }
                Event::Snapshot(time) => {
                    *snapshots += 1;
                    if *snapshots % 200 == 1 {
                        println!("snapshot {snapshots} (server time {time})");
                    }
                }
                Event::ServerCommand(n, command) => {
                    println!("server command {n}: {}", text(&command))
                }
                Event::ConfigString(index, _) => println!("config string {index} changed"),
                Event::Print(message) => println!("print: {}", text(&message)),
                Event::OpenUrlRefused(url) => {
                    println!("the server asked to open {} (not opened)", text(&url))
                }
                Event::Disconnected(why) => println!("disconnected: {why}"),
            }
        }
    };
    while Instant::now() < deadline && !client.disconnected() {
        let mut send = |to: &NetAddress, bytes: &[u8]| {
            let _ = socket.send_to(bytes, SocketAddr::from((to.ip, to.port)));
        };
        match socket.recv_from(&mut buffer) {
            Ok((n, SocketAddr::V4(from))) => {
                let from = NetAddress::ip(from.ip().octets(), from.port());
                let events = client.packet(&from, &buffer[..n], clock(Instant::now()), &mut send);
                report(events, &mut snapshots, &mut active_since);
            }
            Ok(_) => {}
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(e) => return Err(e.to_string()),
        }
        // A client frame every 10 ms or so (the games run theirs at the frame rate).
        if last_frame.elapsed() >= Duration::from_millis(10) {
            last_frame = Instant::now();
            let events = client.frame(clock(last_frame), &mut send);
            report(events, &mut snapshots, &mut active_since);
        }
    }
    match (client.disconnected(), active_since) {
        (false, Some(since)) => {
            println!(
                "still in the game after {:.1} s, {snapshots} snapshots: done",
                since.elapsed().as_secs_f64()
            );
            Ok(())
        }
        (false, None) => Err("didn't get into the game in time".into()),
        (true, _) => Err("the connection ended".into()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let options = match parse(&args) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };
    match run(options) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("iw-join: {e}");
            ExitCode::FAILURE
        }
    }
}
