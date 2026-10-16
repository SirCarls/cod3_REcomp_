# Security

This page says what the port protects you from and how, and how to report a
problem.

## What is protected

**Files someone else gives you can't run code or reach outside the game's
folders.** That covers a shared `cod3.toml` or console config, a user
texture pack, a map pack download and, once multiplayer exists, network
data.

- **The game is recompiled ahead of time.** Its code is fixed when you build
  it. Nothing it reads can add new code, and damaged game memory can't make
  the program jump somewhere new.
- **The game's files stay in the game's folders.** A file name the game
  uses becomes a path on your computer only when it's a plain name: no `..`,
  no drive letter, no separators. Renaming can't move a file out of its
  folder, and the game's own files (`assets/`) can't be changed at all.
- **Settings files are checked.** The console config (`userdata/cod3_config.cfg`)
  keeps only `seta`/`set` lines with a valid name and a short, printable
  value, never `sv_cheats`, and has a size limit. `cod3.toml` can't load a
  graphics plugin from another folder, pass a command line to the game, make
  the game's files writable, or move the log file. Text values are escaped when the settings are
  saved, so a value can't add settings of its own.
- **Texture packs are untrusted input.** Every line of a pack's `index.tsv`
  is checked; file names must be plain names inside the pack (links that lead
  outside are refused); every DDS header is checked before anything is
  loaded, and the file must be exactly as long as its header says. A broken
  file is skipped with a log line.
- **Console file names stay put.** `condump` writes only a plain `.txt` name
  inside `userdata/`.

## Cheats

- Cheat commands (`god`, `noclip`, `give` ...) and cheat-protected dvars (C
  in `cvarlist`, for example `timescale`) work only while `sv_cheats` is 1.
  The game checks this itself; the port adds a clear message.
- `sv_cheats` is changed only with the `sv_cheats` (or `cheats`) command,
  typed in the console or given in `cod3_autoexec`, never from the console
  config. The game's own command line isn't used at all, so `+set` can't
  slip a cheat in at start. While cheats are on, and until they are off and
  reset, a **CHEATS USED** mark stays on screen.
- In a multiplayer match only the host can change `sv_cheats`, and every
  player is told. Other players can never run cheat commands or commands
  that start, restart or end the match.

## Textures in multiplayer

During a match the HD texture pack is off, and from your own user texture
pack only a fixed list of world insignia (flags, banners and the like, never
anything a soldier or vehicle wears) is used. Those pictures may not be more
see-through than the originals. So a texture can't make walls, smoke or
bushes see-through or make other players stand out. Replaced textures only
change what you see on your own screen.

## The setup tool

- It never runs World at War, a patch or a mod installer. Installers are
  unpacked as archives, and archive entries that would land outside their
  folder are refused.
- Every download needs your approval first, uses HTTPS and is checked
  before it's used: against a fixed SHA-256, the publisher's own checksum or
  signature, or a pinned commit or lock file (the SDK source, the ripper's
  Rust libraries).
- It never deletes your Call of Duty 3 disc image, your World at War folder
  or anything outside its own folders. The work folder is emptied after a
  build, so the tool only uses a folder it made: a folder that already holds
  other files is refused.
- Commands are started directly, never through a shell, so file names can't
  turn into commands.

## Limits

This isn't anti-cheat. Anyone can change their own copy of the game.
Fairness in multiplayer comes from the host deciding what happens in the
match.

## Reporting a problem

Please report security problems privately: use **Report a vulnerability** on
the repository's **Security** tab on GitHub. If that isn't available, open an
issue that only says you have a security report, without the details, and
we'll arrange a private way to send them.

A good report says what you did, what happened, which version or commit you
used, and on which system (Windows or Linux). Please give us time to fix it
before you publish.
