//! Requests for `iw_pc::net::game_state`: what the server writes into a
//! client's messages (specs/net_game_state.py).

use iw_pc::net::game_state::{self, GameState, GameStateClient, GameStateLink, ServerCommands};
use iw_pc::net::msg::Msg;
use iw_pc::Game;

use crate::net_client::{CallLog, Logged};
use crate::net_netchan::with_msg;
use crate::{Answer, Args, Ret};

fn word(b: &[u8], at: usize) -> Result<i32, String> {
    b.get(at..at + 4)
        .map(|w| i32::from_be_bytes([w[0], w[1], w[2], w[3]]))
        .ok_or_else(|| "short client token".to_string())
}

/// A client's reliable commands from its token: sequence, acknowledge, then
/// each slot's text (u16 length, bytes).
fn client_commands(game: Game, data: &[u8]) -> Result<(i32, i32, Vec<Vec<u8>>), String> {
    let sequence = word(data, 0)?;
    let acknowledge = word(data, 4)?;
    let ring = iw_pc::net::netchan::max_reliable_commands(game);
    let mut texts = Vec::with_capacity(ring);
    let mut at = 8;
    for _ in 0..ring {
        let n = data
            .get(at..at + 2)
            .map(|b| usize::from(u16::from_be_bytes([b[0], b[1]])))
            .ok_or("short client token")?;
        texts.push(
            data.get(at + 2..at + 2 + n)
                .ok_or("short client token")?
                .to_vec(),
        );
        at += 2 + n;
    }
    Ok((sequence, acknowledge, texts))
}

/// The old and the new list from a ring token (specs/net_game_state.py
/// RingLists): the old count (u32 big-endian), then the states.
/// States, each its own bytes.
type States = Vec<Vec<u8>>;

fn ring_lists(data: &[u8], size: usize) -> Result<(States, States), String> {
    let old_count = word(data, 0)? as usize;
    let states: Vec<Vec<u8>> = data[4..].chunks(size).map(<[u8]>::to_vec).collect();
    if old_count > states.len() || states.iter().any(|s| s.len() != size) {
        return Err("bad ring token".to_string());
    }
    let new = states[old_count..].to_vec();
    let mut old = states;
    old.truncate(old_count);
    Ok((old, new))
}

/// sv.svEntities' stride in each game (the baseline is at the start of each entry).
fn baseline_stride(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 0x174,
        Game::UnitedOffensive => 0x180,
    }
}

/// A snapshot writer's client from its token (specs/net_game_state.py
/// SnapshotClientBlock): five numbers, then per frame four numbers and the
/// player state.
struct ClientToken {
    state: i32,
    active: i32,
    delta: i32,
    rate: i32,
    outgoing: i32,
    frames: Vec<(Vec<u8>, [i32; 4])>,
}

fn client_token(data: &[u8], ps_size: usize) -> Result<ClientToken, String> {
    let mut at = 20;
    let mut frames = Vec::with_capacity(32);
    for _ in 0..32 {
        let numbers = [
            word(data, at)?,
            word(data, at + 4)?,
            word(data, at + 8)?,
            word(data, at + 12)?,
        ];
        let n = word(data, at + 16)? as usize;
        let ps = data.get(at + 20..at + 20 + n).ok_or("short client token")?;
        frames.push((
            if n == 0 {
                vec![0; ps_size]
            } else {
                ps.to_vec()
            },
            numbers,
        ));
        at += 20 + n;
    }
    Ok(ClientToken {
        state: word(data, 0)?,
        active: word(data, 4)?,
        delta: word(data, 8)?,
        rate: word(data, 12)?,
        outgoing: word(data, 16)?,
        frames,
    })
}

/// The check's connection (specs/net_game_state.py): each fragment sent is
/// one of `left`; a message too big for one packet leaves `after` of them.
/// Both calls are logged as the original's stand-ins record them.
struct CheckLink {
    log: CallLog,
    left: i32,
    after: i32,
}

impl GameStateLink for CheckLink {
    fn transmit_next_fragment(&mut self) -> bool {
        self.log.call("Netchan_TransmitNextFragment", &[]);
        self.left -= 1;
        self.left > 0
    }

    fn send_message(&mut self, msg: &Msg) -> bool {
        let end = msg.cursize.min(msg.data.len());
        self.log.call(
            "SV_SendMessageToClient",
            &[
                Logged::I(i32::from(msg.overflowed)),
                Logged::S(&msg.data[..end]),
            ],
        );
        if self.after > 0 {
            self.left = self.after;
            true
        } else {
            false
        }
    }
}

/// The config strings from their token (index u16, length u32, text each),
/// all 2048 (the rest empty).
fn config_strings(data: &[u8]) -> Result<Vec<Vec<u8>>, String> {
    let mut strings = vec![Vec::new(); game_state::MAX_CONFIG_STRINGS];
    let mut at = 0;
    while at < data.len() {
        let head = data.get(at..at + 6).ok_or("short config string token")?;
        let index = usize::from(u16::from_be_bytes([head[0], head[1]]));
        let n = u32::from_be_bytes([head[2], head[3], head[4], head[5]]) as usize;
        let text = data
            .get(at + 6..at + 6 + n)
            .ok_or("short config string token")?;
        *strings.get_mut(index).ok_or("config string index")? = text.to_vec();
        at += 6 + n;
    }
    Ok(strings)
}

pub fn dispatch(game: Game, function: &str, args: &mut Args) -> Option<Answer> {
    Some(match function {
        // msg, client, the rings, the baselines, sv.time, the map-restart bit,
        // svs.nextSnapshotEntities, sv_padPackets
        "SV_WriteSnapshotToClient" => (|| {
            let tables = crate::net_delta::tables(game)?;
            let ps_size = iw_pc::net::delta::player_state_size(game);
            let token = client_token(&args.buffer(1).ok_or("NULL client")?.clone(), ps_size)?;
            let rings = args.bytes(2).to_vec();
            let (entity_count, client_count) =
                (word(&rings, 0)? as usize, word(&rings, 4)? as usize);
            let entity_size = iw_pc::client::entity_state_size(game);
            let client_size = iw_pc::client::CLIENT_STATE_SIZE;
            let entity_end = 8 + entity_count * entity_size;
            if rings.len() != entity_end + client_count * client_size {
                return Err("bad rings token".to_string());
            }
            let entities: Vec<&[u8]> = rings[8..entity_end].chunks(entity_size).collect();
            let clients: Vec<&[u8]> = rings[entity_end..].chunks(client_size).collect();
            let baselines = args.region(3).clone();
            let stride = baseline_stride(game);
            let baseline = |n: i32| -> &[u8] {
                let at = (n.clamp(0, 1023) as usize) * stride;
                &baselines[at..at + entity_size]
            };
            let frames: Vec<game_state::SnapshotFrame> = token
                .frames
                .iter()
                .map(|(ps, n)| game_state::SnapshotFrame {
                    player: ps,
                    num_entities: n[0],
                    num_clients: n[1],
                    first_entity: n[2],
                    first_client: n[3],
                })
                .collect();
            let mut client = game_state::SnapshotClient {
                state: token.state,
                active: token.active != 0,
                delta_message: token.delta,
                rate_delayed: token.rate != 0,
                outgoing_sequence: token.outgoing,
                frames: &frames,
            };
            let rings = game_state::SnapshotRings {
                entities: &entities,
                clients: &clients,
                next_entities: args.i32(6),
                baseline: &baseline,
            };
            let (time, bit, padding) = (args.i32(4), args.i32(5), args.i32(7));
            with_msg(args, 0, game, |m| {
                game_state::write_snapshot_to_client(
                    m,
                    tables,
                    &mut client,
                    &rings,
                    time,
                    bit,
                    padding,
                )
            })?;
            // The "in the game" word is all of the client the writer changes.
            *args.buffer(1).ok_or("NULL client")? = i32::from(client.active).to_be_bytes().to_vec();
            Ok(Ret::Void)
        })(),
        // msg, four numbers (the lists' places, as each game passes them), the lists, the baselines
        "SV_EmitPacketEntities" => (|| {
            let tables = crate::net_delta::tables(game)?;
            let size = iw_pc::client::entity_state_size(game);
            let (old, new) = ring_lists(args.bytes(5), size)?;
            let baselines = args.region(6).clone();
            let stride = baseline_stride(game);
            let baseline = |n: i32| -> &[u8] {
                let at = (n.clamp(0, 1023) as usize) * stride;
                &baselines[at..at + size]
            };
            let old_refs: Vec<&[u8]> = old.iter().map(Vec::as_slice).collect();
            let new_refs: Vec<&[u8]> = new.iter().map(Vec::as_slice).collect();
            with_msg(args, 0, game, |m| {
                game_state::write_packet_entities(m, tables, &old_refs, &new_refs, &baseline)
            })?;
            Ok(Ret::Void)
        })(),
        // msg, four numbers, the lists
        "SV_EmitPacketClients" => (|| {
            let tables = crate::net_delta::tables(game)?;
            let (old, new) = ring_lists(args.bytes(5), iw_pc::client::CLIENT_STATE_SIZE)?;
            let old_refs: Vec<&[u8]> = old.iter().map(Vec::as_slice).collect();
            let new_refs: Vec<&[u8]> = new.iter().map(Vec::as_slice).collect();
            with_msg(args, 0, game, |m| {
                game_state::write_packet_clients(m, tables, &old_refs, &new_refs)
            })?;
            Ok(Ret::Void)
        })(),
        // client, sv.configstrings, the baselines, sv.checksumFeed, timescale's integer,
        // the call log -> the first warning; the client's changed fields
        "SV_SendClientGameState" => (|| {
            let tables = crate::net_delta::tables(game)?;
            let token = args.buffer(0).ok_or("NULL client")?.clone();
            let n = |i: usize| word(&token, 4 * i);
            let (state, fragments, outgoing, last_command, number, after) =
                (n(0)?, n(1)?, n(2)?, n(3)?, n(4)?, n(5)?);
            let (sequence, acknowledge, texts) =
                client_commands(game, &token[24.min(token.len())..])?;
            let slices: Vec<&[u8]> = texts.iter().map(Vec::as_slice).collect();
            let strings = config_strings(args.bytes(1))?;
            let string_refs: Vec<&[u8]> = strings.iter().map(Vec::as_slice).collect();
            let baselines = args.region(2).clone();
            let size = iw_pc::client::entity_state_size(game);
            let stride = baseline_stride(game);
            let bases: Vec<&[u8]> = (0..1024)
                .map(|k| baselines.get(k * stride..k * stride + size).unwrap_or(&[]))
                .collect();
            let state_in = GameState {
                last_client_command: last_command,
                commands: ServerCommands {
                    acknowledged: acknowledge,
                    sequence,
                    texts: &slices,
                },
                config_strings: &string_refs,
                baselines: &bases,
                client_num: number,
                checksum_feed: args.i32(3),
            };
            // (Whatever pureAuthentic, gamestateMessageNum and reliableSent held: all three are set.)
            let mut client = GameStateClient {
                state,
                unsent_fragments: fragments > 0,
                outgoing_sequence: outgoing,
                ..GameStateClient::default()
            };
            let mut link = CheckLink {
                log: CallLog::default(),
                left: fragments,
                after,
            };
            let mut buffer = vec![0u8; game_state::game_state_buffer_size(game)];
            let result = game_state::send_client_game_state(
                game,
                tables,
                &mut client,
                &state_in,
                args.i32(4),
                &mut buffer,
                &mut link,
            );
            let mut out = Vec::with_capacity(20);
            for w in [
                client.state,
                client.pure_authentic,
                client.gamestate_message_num,
                i32::from(client.unsent_fragments),
                client.reliable_sent,
            ] {
                out.extend_from_slice(&w.to_be_bytes());
            }
            *args.buffer(0).ok_or("NULL client")? = out;
            *args.buffer(5).ok_or("NULL call log")? = link.log.0;
            Ok(Ret::Printed(
                result.err().map(|e| e.message_format().as_bytes().to_vec()),
            ))
        })(),
        // msg, client -> the first warning; the client's last-sent number
        "SV_UpdateServerCommandsToClient" => (|| {
            let client = args.buffer(1).ok_or("NULL client")?.clone();
            let (sequence, acknowledge, texts) = client_commands(game, &client)?;
            let slices: Vec<&[u8]> = texts.iter().map(Vec::as_slice).collect();
            let commands = ServerCommands {
                acknowledged: acknowledge,
                sequence,
                texts: &slices,
            };
            let result = with_msg(args, 0, game, |m| {
                game_state::write_server_commands(m, &commands)
            })?;
            // (The original then records the commands as sent: reliableSent = reliableSequence.)
            *args.buffer(1).ok_or("NULL client")? = sequence.to_be_bytes().to_vec();
            Ok(Ret::Printed(
                result.err().map(|e| e.message_format().as_bytes().to_vec()),
            ))
        })(),
        _ => return None,
    })
}
