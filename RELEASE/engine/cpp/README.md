# The C++ version (for developers)

The PC games' decomp, **Call of Duty 2 1.3** and **United Offensive 1.51**, in C++20. The full
version is the Rust crate `engine/crates/iw_pc` (owner, 2026-10-15: "c++ is needed, but the full
versions need to be rust. c++ for developers"); this follows it function by function, with the
same names, the same `Original:` lines and the same behavior, quirks included, so that people
who work in C++ can read, use and build on the decomp.

Every function here is checked against the games' own machine code exactly like the Rust
version: the same checker runs the original in an emulator and the C++ port on the same
thousands of inputs and compares every result and every byte written.

```
python3 tools/decomp/x86/pcdecomp.py check --impl cpp            # every function, both games
python3 tools/decomp/x86/pcdecomp.py check --impl cpp Q_stricmp  # one function
```

(`IW_PC_INPUT` names the folders holding `CoD2MP_s.exe` and `CoDUOMP.exe`, as for the Rust
checks; they are only read.)

## What is here

| | |
|---|---|
| `iw_pc/base.h`, `cstr.h`, `crt.h` | bytes, games, the game's errors; C strings in fixed buffers; the C runtime pieces (`tolower`, `strncpy`, `Com_sprintf`'s rules) |
| `iw_pc/shared/` | strings, info strings, hashes, file filters, the archives' block checksums (MD4) |
| `iw_pc/fs/` | the search path's archives and the list of their pure checksums a client sends a pure server |
| `iw_pc/net/msg` | the message buffer (`msg_t`): bits, bytes, strings, angles, key-coded values |
| `iw_pc/net/huffman` | the message Huffman tree and United Offensive's adaptive coder |
| `iw_pc/net/protocol_data`, `iw_pc/sha256` | the protocol tables, read from the player's executable and checked by SHA-256 |
| `iw_pc/net/oob` | packets without a connection: words, `getinfo`/`getstatus` answers, challenges, the connect decision, the server browser entry |
| `iw_pc/net/netchan` | the connection: header, fragments, scrambling |
| `iw_pc/net/delta` | delta coding of entities, clients, the player state and player commands, both ways |
| `iw_pc/net/game_state` | what the server writes into a client's messages: its reliable commands, the snapshot's entity and client lists, the whole snapshot, the game state and its sending (the message types are here too) |
| `iw_pc/client` | the client's side of the connection: reading the server's messages (the game state, snapshots with their entities and clients, reliable commands, the whole message), the packet's arrival and packets without a connection, joining (`getchallenge`, `connect`), the system info, the client game's clock, making and sending the player's commands, reliable commands and `cp` (pure checksums), downloads, the `connect` command |
| `pc_check/` | `pc_check_cpp`, the checker's answerer (the protocol of `engine/tools/pc_check`) |
| `tests/` | unit tests that need no game files (`ctest`) |

The Rust crate also has the server (`iw_pc::server`, `iw-server`) and the client's connection driver
(`iw_pc::client::connection`, `iw-join`); they are built from the functions above and come to
C++ when they are checked as functions of their own.

## Building

```
cmake -S engine/cpp -B engine/cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/cpp/build
ctest --test-dir engine/cpp/build
```

Standard C++20 and the standard library only. Built with GCC 13 and Clang 18 on Linux, no
warnings at `-Wall -Wextra -Wpedantic -Wconversion`. Windows (MSVC 2022 or clang-cl) is meant
to work the same way but hasn't been built yet. The checks need a 64-bit build (Call of Duty
2's 24-bit x87 rounding is reproduced with `float` stores, which an x87 build with extended
precision would not give).

## Conventions (the same as the Rust crate's)

- Strings are bytes: a string argument is a `ByteView` of its characters (reading past its end
  gives 0, `cstr::at`); a string in a fixed buffer is a `ByteSpan` the size of the game's array.
- The game's `Com_Error` is a returned `GameError` (`Status`, `Expected<T>`) at the same point
  with the same level; a warning the original prints instead is a returned refusal.
- Characters are signed where the original uses `movsx`.
- No game data: the Huffman frequencies and the delta field tables are read from the player's
  executable at run time (`net/protocol_data.h`), never stored in the code.
- Quake III's source (GPL) is for understanding and names only; this is written from the Rust
  ports, which are written from the games' machine code.
