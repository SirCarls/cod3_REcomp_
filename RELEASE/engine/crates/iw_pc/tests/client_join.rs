//! The client connection (`iw_pc::client::connection`, the ported client
//! pieces put together) joins this crate's server, as it would a CB copy's:
//! challenge, connect, game state, first snapshot, then it stays in the
//! game for a while, both sides keeping time in 10 ms steps and every packet
//! going through the real bytes.
//!
//! Needs the games' executables for the protocol tables: set `IW_PC_INPUT`
//! to the folders holding `CoD2MP_s.exe` / `CoDUOMP.exe` (skipped otherwise).

use iw_pc::client::connection::{Connection, Event, JoinSettings};
use iw_pc::net::netchan::NetAddress;
use iw_pc::net::protocol_data::{self, GameExecutable};
use iw_pc::server::{Protocol, Server, ServerConfig};
use iw_pc::Game;

fn protocol(game: Game) -> Option<Protocol> {
    let folders = std::env::var_os("IW_PC_INPUT")?;
    let path = protocol_data::find_executable(game, &folders)?;
    let exe = GameExecutable::open(game, &path).ok()?;
    Some(Protocol::from_executable(&exe).expect("protocol tables"))
}

fn join(game: Game) {
    let Some(protocol) = protocol(game) else {
        eprintln!("IW_PC_INPUT not set or no executable: {game:?} join skipped");
        return;
    };
    let (tree, tables) = (protocol.tree.clone(), protocol.tables.clone());
    let mut config = ServerConfig::new(game, "mp_test");
    config.hostname = "test server".into();
    let mut server = Server::new(config, protocol, 777);
    // Off the LAN, so the client's packet rate applies.
    let server_address = NetAddress::ip([203, 0, 113, 10], 28960);
    let client_address = NetAddress::ip([198, 51, 100, 20], 28961);
    let mut settings = JoinSettings::new("Rusty");
    settings.key_hash = b"0123456789abcdef0123456789abcdef".to_vec();
    let mut client = Connection::new(game, tree, tables, settings);

    let mut to_server: Vec<Vec<u8>> = Vec::new();
    let mut to_client: Vec<Vec<u8>> = Vec::new();
    let mut seen = Vec::new();
    let start = 100_000;
    client.connect(server_address, 0x4D2, start);
    let mut snapshots = 0;
    let mut last_snapshot_time = 0;
    for step in 0..400 {
        let now = start + step * 10;
        // The client's frame, then what it sent reaches the server.
        for e in client.frame(now, &mut |to, bytes| {
            assert_eq!(*to, server_address);
            to_server.push(bytes.to_vec());
        }) {
            seen.push(e);
        }
        for p in to_server.drain(..) {
            server.packet(&client_address, &p, &mut |to, bytes| {
                assert_eq!(*to, client_address);
                to_client.push(bytes.to_vec());
            });
        }
        server.frame(10, &mut |to, bytes| {
            assert_eq!(*to, client_address);
            to_client.push(bytes.to_vec());
        });
        for p in std::mem::take(&mut to_client) {
            for e in client.packet(&server_address, &p, now, &mut |_, bytes| {
                to_server.push(bytes.to_vec())
            }) {
                if let Event::Snapshot(time) = e {
                    assert!(time > last_snapshot_time, "snapshots in order");
                    last_snapshot_time = time;
                    snapshots += 1;
                }
                seen.push(e);
            }
        }
        assert!(!client.disconnected(), "disconnected: {seen:?}");
    }
    let kinds: Vec<&Event> = seen
        .iter()
        .filter(|e| !matches!(e, Event::Snapshot(_)))
        .collect();
    let position = |want: &dyn Fn(&Event) -> bool| kinds.iter().position(|e| want(e));
    let challenged = position(&|e| matches!(e, Event::Challenged(_))).expect("challenge");
    let connected = position(&|e| matches!(e, Event::Connected)).expect("connected");
    let game_state = position(&|e| matches!(e, Event::GameState(_))).expect("game state");
    let active = position(&|e| matches!(e, Event::Active)).expect("active");
    assert!(
        challenged < connected && connected < game_state && game_state < active,
        "{kinds:?}"
    );
    let Event::GameState(info) = kinds[game_state] else {
        unreachable!()
    };
    assert!(info.settings.iter().any(|(k, _)| k == b"sv_serverid"));
    assert!(client.active());
    assert_eq!(client.cl.server_id, server_id(&client));
    assert_eq!(
        server.active_count(),
        1,
        "the server has the player in the game"
    );
    // 4 s in the game at 20 snapshots a second.
    assert!(snapshots > 40, "{snapshots} snapshots");
    // The client's clock follows the server's (within a frame or two).
    let lag = last_snapshot_time - client.cl.server_time;
    assert!(
        (-100..=100).contains(&lag),
        "client time {} vs snapshot {last_snapshot_time}",
        client.cl.server_time
    );
}

fn server_id(client: &Connection) -> i32 {
    let info = client.cl.game_state.get(iw_pc::client::SYSTEM_INFO);
    iw_pc::net::oob::atol(iw_pc::shared::info::value_for_key(info, b"sv_serverid").unwrap())
}

#[test]
fn call_of_duty_2_client_joins() {
    join(Game::CallOfDuty2);
}

#[test]
fn united_offensive_client_joins() {
    join(Game::UnitedOffensive);
}
