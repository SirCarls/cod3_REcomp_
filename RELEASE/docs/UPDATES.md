# Updates of the setup tool

The setup tool can update itself: the project folder you unzipped (the setup
tool, the recomp project, the SDK patches, the Rust engine's source, the docs
and the tools) to the newest release. It never updates anything else: your
discs (`input/`), the built game (`output/`), the work folder (`work/`), the
settings (`config/`) and `dev/` are never touched. After an update, press
**Build** again to rebuild the game with it.

## For players

- **Nothing is checked until you allow it.** The first time the setup tool
  starts it asks: *Check for new versions of the setup tool?* Your answer is
  kept in `config/updates.json`; change it on the **About** page (*Check for
  updates when the setup tool starts*), or on the command line with
  `update --allow` / `update --deny`. (Typing `update` on the command line,
  or pressing **Check now**, checks that once, whatever the setting.)
- **A check** reads two small files from the project's GitHub page
  (`raw.githubusercontent.com`; GitHub sees your IP address, as with any
  download). Nothing else is sent.
- **Nothing is downloaded until you say so.** When there is a newer version, a
  note says so, with what's new. **Update** lists what will be downloaded, from
  where and into which folder, and waits for **Download the update**.
- **It's put in place when the setup tool starts again** (**Restart now**, or
  next time), before the rest of the tool is loaded. The files it replaces are
  kept in `config/update/previous/`: **About > Undo the last update** puts them
  back. If the update is cut off halfway (a power cut, a closed window), the
  next start puts the earlier files back first, then tries again. If the new
  version's code doesn't even load, it's undone at once. `update --undo` works
  even when the rest of the tool doesn't start.
- On the command line: `sh setup.sh update` (or `setup.bat update`) checks,
  shows what would change and asks; `--check` only looks, `--yes` doesn't ask,
  `--undo` goes back.
- The launcher copy in a game folder (`output/launcher/`) doesn't update itself;
  it's written again by the next build.

## Why an update can't be faked

- **Every release is signed** on the project owner's own PC with an Ed25519 key
  that never leaves it (it is not on GitHub, in CI or with any helper). The
  release lists every file with its size and SHA-256
  (`COD_3_WAW_Setup-tool/release.json`); the signature
  (`release.json.sig`) covers that list.
- **The setup tool trusts only the keys it came with:**
  `COD_3_WAW_Setup-tool/c3w/update_keys.json`. GitHub only carries the files:
  if the repository, a mirror or the connection were taken over, an update
  without a valid signature is refused, with a warning.
- **The signature is checked first**, before the list is read; then every
  downloaded file must match its size and SHA-256 before it's written anywhere.
- **No going back by trickery:** only a version newer than your copy's (and not
  older than the newest one seen) is offered, so an old, signed release can't
  be replayed to bring back a fixed problem.
- **Only plain paths inside the project folder:** plain ASCII names that
  Windows allows (no reserved names like `CON`, no short names like `COD_3_~1`),
  no `..`, no absolute paths, no links or junctions on the way, never `input/`,
  `output/`, `work/`, `config/`, `dev/` or `.git` (in any letter case).
- **New code never checks itself:** the copy you have checks and downloads the
  update; the new files run only after they are all checked and in place.
- **Changing the keys takes a second, offline key** (the recovery key, kept off
  the PC): a release that changes `update_keys.json` must be signed with it.
  If the signing key is lost or stolen, the recovery key moves everyone who
  hasn't installed a bad release to a new key. (Like any software update, a
  release signed with the signing key can change any of the tool's code, so
  the signing key and its passphrase are what must stay safe.)
- The ed25519 code is RFC 8032's (`c3w/ed25519.py`), with the RFC's test
  vectors and an OpenSSL cross-check in `tests/test_update.py`.

What it can't protect against: someone who has the owner's signing key and
passphrase, a release the owner signs without noticing a bad change, a
tampered first download (compare the key fingerprints below with the ones on
the project page), anyone who can already write into your project folder, and
someone who controls your connection keeping you on the version you have
(they can't make you install anything, but they can hide that a new one
exists).

## For the project owner: making a release

In the folder you publish (your GitHub clone's `RELEASE/`):

1. **Once:** `python3 tools/sign_release.py keygen`. It asks for two
   passphrases and writes two keys into your own settings folder
   (`~/.config/cod3-release-keys/`), never into the project:
   - `signing-key.json` signs every release;
   - `recovery-key.json` only replaces the signing key: copy it to a USB stick
     and delete it from the PC.
   It writes the public keys and the repository's name into
   `COD_3_WAW_Setup-tool/c3w/update_keys.json` and prints their fingerprints.
2. **Each release:** bump `TOOL_VERSION` in `c3w/pipeline.py` (as always), then
   `python3 tools/sign_release.py sign --push`. It takes every file git
   publishes in this folder, refuses private key files, game data and
   personal paths (`tools/check_repo.py`), asks before publishing files that
   are new to git, and hashes each file as players get it (as git gives it out,
   line endings by `.gitattributes`). Then it writes `release.json` and its
   signature, commits exactly those files, tags `v<version>`, checks every
   file at the tag the way a player's setup tool and a download will, checks
   that the key players have now signed it, and pushes: not when anything the
   push would publish (earlier commits too) holds a key file, and it asks
   before publishing earlier commits of yours. If a check fails, the commit
   and the tag are taken back. The release
   notes are the `TOOL_VERSION` comment's entry for that version (or
   `--notes "..."`).
3. **Check a folder** at any time: `python3 tools/sign_release.py verify`.

Players' setup tools read `release.json` from the repository's default branch
and the files from the tag, so commits pushed after a release (unsigned) are
never offered as an update.

**If the signing key is lost or stolen:** make a new one, put its public key
into `update_keys.json` in place of the old one, and sign that release with
the recovery key (`sign --recovery`); setup tools accept a key change only with
the recovery key's signature.

## Files

| File | What it is |
|---|---|
| `COD_3_WAW_Setup-tool/c3w/update.py` | Checks, downloads and applies updates. |
| `COD_3_WAW_Setup-tool/c3w/ed25519.py` | Ed25519 signatures (RFC 8032). |
| `COD_3_WAW_Setup-tool/c3w/gui_updates.py` | The question at the first start, the note, About > Updates. |
| `COD_3_WAW_Setup-tool/c3w/update_keys.json` | The release keys and where releases come from. |
| `COD_3_WAW_Setup-tool/release.json`, `.sig` | This copy's release: its files and their signature. |
| `tools/sign_release.py` | Makes the keys; signs, commits and tags a release. |
| `config/updates.json` | Your choice about checks, and the newest version seen. |
| `config/update/pending/`, `previous/` | A downloaded update waiting for the next start; the files the last one replaced. |
