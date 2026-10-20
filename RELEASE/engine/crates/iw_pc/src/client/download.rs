//! Downloads from the server (`svc_download`): the blocks of a file the
//! client asked for (`nextdl`, `stopdl`, `retransdl`), or the server's
//! redirect to a web address (`wwwdl ack`, `wwwdl fail`, `wwwdl bbl8r`).
//!
//! A block is its number (block 0 also carries the file's size), its length
//! and its bytes; the client acknowledges each with `nextdl <n>`, asks again
//! with `retransdl <n>` for one it missed, and takes a zero-length block as
//! the end: the file is closed, renamed from its temporary name, and the
//! next download starts. Block -1 is a redirect: the file's web address, its
//! size and two flags (1: the server disconnects the client while it
//! downloads; 2: open the address in a browser and quit).
//!
//! What the original does to the file system, the console, the menus and
//! the web download is the caller's ([`DownloadHost`]).
//!
//! # Opening a web page
//!
//! A server that sets flag 2 makes the originals open its address with the
//! system's shell (`ShellExecuteA` "open": the default browser, or whatever
//! program handles the address) and quit the game. The port only tells the
//! host ([`DownloadHost::open_url`]); a client should ask the player first,
//! or refuse.
//!
//! # United Offensive's error text
//!
//! When block 0 carries a negative size, the string after it is the error
//! the client drops with. Call of Duty 2 passes it to `Com_Error` as a `%s`
//! argument; United Offensive passes it as the format itself, so a server's
//! `%` sequences are formatted with whatever is on the stack. The port keeps
//! the text as it is.

use crate::client::{add_reliable_command, ClientState};
use crate::net::msg::{Msg, ReliableCommandWarning, MAX_STRING_CHARS};
use crate::{Game, GameError};

/// The size of the download names (`cls.downloadName`, `cls.downloadTempName`).
pub const NAME_SIZE: usize = 256;
/// The size of the name kept from before a redirect (`cls.originalDownloadName`).
pub const ORIGINAL_NAME_SIZE: usize = 64;

/// The client's download (`clc.download*`, `cls.download*`; the redirect's
/// flags). The names are C strings in fixed buffers, as the originals keep
/// them: clearing one zeroes only its first byte.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DownloadState {
    /// The block expected next (`clc.downloadBlock`).
    pub block: i32,
    /// The file's size, from block 0 or the redirect (`clc.downloadSize`).
    pub size: i32,
    /// The bytes written so far (`clc.downloadCount`; Call of Duty 2 keeps
    /// it only in the menus' numbers, [`DownloadHost::set_count`]).
    pub count: i32,
    /// The file being written (`clc.download`: [`DownloadHost::open`]'s
    /// handle; 0: none).
    pub file: i32,
    /// The file's name on the server; after a redirect, its web address.
    pub name: [u8; NAME_SIZE],
    /// Where it is written until complete; after a redirect, its path.
    pub temp_name: [u8; NAME_SIZE],
    /// The name before a redirect replaced it.
    pub original_name: [u8; ORIGINAL_NAME_SIZE],
    /// The redirect's flags (1: the server disconnects the client while it
    /// downloads; 2: open the address and quit).
    pub flags: i32,
    /// A redirected download is going: more block -1 messages are skipped
    /// (`clc.bWWWDl`).
    pub www: bool,
    /// United Offensive: the redirect failed or was refused; nothing more is
    /// done with it until a new game state (`clc.bWWWDlAborting`).
    pub www_aborting: bool,
    /// The server will disconnect the client while it downloads (flag 1;
    /// `cls.bWWWDlDisconnected`).
    pub www_disconnected: bool,
    /// United Offensive: files whose download failed its checksum, each as
    /// `@name` (`clc.badChecksumList`): a redirect to one is refused.
    pub bad_checksums: Vec<u8>,
}

impl Default for DownloadState {
    fn default() -> Self {
        DownloadState {
            block: 0,
            size: 0,
            count: 0,
            file: 0,
            name: [0; NAME_SIZE],
            temp_name: [0; NAME_SIZE],
            original_name: [0; ORIGINAL_NAME_SIZE],
            flags: 0,
            www: false,
            www_aborting: false,
            www_disconnected: false,
            bad_checksums: Vec::new(),
        }
    }
}

/// A C string in a buffer: up to its first NUL.
pub fn c_str(bytes: &[u8]) -> &[u8] {
    &bytes[..bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len())]
}

/// `strncpy(out, text, n)` then `out[n] = 0`: the text's first `n` bytes
/// (up to its NUL), the rest of `out[..n]` zeroed, and one more zero.
fn copy_name(out: &mut [u8], text: &[u8], n: usize) {
    let text = c_str(text);
    let k = text.len().min(n);
    out[..k].copy_from_slice(&text[..k]);
    out[k..n].fill(0);
    out[n] = 0;
}

/// What a download does outside the client's state.
pub trait DownloadHost {
    /// Opens the file to write (`FS_SV_FOpenFileWrite`): a handle, 0 when it
    /// can't.
    fn open(&mut self, temp_name: &[u8]) -> i32;
    /// Writes a block (`FS_Write`; `length` as the original passes it: -1
    /// for a block whose length couldn't be read, with no bytes).
    fn write(&mut self, data: &[u8], length: i32, file: i32);
    /// Closes the file (`FS_FCloseFile`).
    fn close(&mut self, file: i32);
    /// Renames the finished file (`FS_SV_Rename`).
    fn rename(&mut self, from: &[u8], to: &[u8]);
    /// Sends a packet now (`CL_WritePacket`).
    fn write_packet(&mut self);
    /// Starts the next download, if any (`CL_NextDownload`).
    fn next_download(&mut self);
    /// The menus' numbers: the size (United Offensive: `cl_downloadSize`;
    /// Call of Duty 2: its download block's size).
    fn set_size(&mut self, size: i32);
    /// The bytes so far (United Offensive: `cl_downloadCount`; Call of Duty
    /// 2: the block's count, also [`DownloadState::count`]).
    fn set_count(&mut self, count: i32);
    /// The download's name is cleared (United Offensive: `cl_downloadName`
    /// set to ""; Call of Duty 2: its block's name).
    fn clear_name(&mut self);
    /// The `fs_homepath` setting's text ("" when there is none).
    fn home_path(&mut self) -> Vec<u8>;
    /// An operating system path (`FS_BuildOSPath(base, game, qpath)`).
    fn os_path(&mut self, base: &[u8], game: &[u8], qpath: &[u8]) -> Vec<u8>;
    /// Opens `url` with the system's shell and brings up the game's window
    /// (Call of Duty 2: `ShellExecuteA`, then the window in front
    /// maximized; United Offensive: `Sys_OpenURL`). Returns whether it
    /// opened; Call of Duty 2 drops the connection when not ("couldn't open
    /// URL"). See the module documentation.
    fn open_url(&mut self, url: &[u8]) -> bool;
    /// Adds a console command (`Cbuf_AddText`).
    fn add_text(&mut self, text: &[u8]);
    /// Starts the web download (`DL_BeginDownload(local, remote)`); false
    /// when it can't.
    fn begin_download(&mut self, local: &[u8], remote: &[u8]) -> bool;
    /// Call of Duty 2: ends the web download (its connection handle).
    fn end_download(&mut self);
}

/// A warning the original prints (and then goes on, or returns).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DownloadWarning {
    /// "Server sending download, but no download was requested" (`stopdl` sent).
    NotRequested,
    /// "Could not create %s" (`stopdl` sent, the next download started).
    CouldNotCreate,
    /// United Offensive: "refusing redirect to %s by server (bad checksum)".
    RefusedRedirect,
    /// "Failed to initialize download for '%s'" (`wwwdl fail` sent).
    FailedToInitialize,
    /// A command's warning from [`add_reliable_command`] (never for the
    /// commands this sends).
    Command(ReliableCommandWarning),
}

impl DownloadWarning {
    /// The format of the warning (both games, the same text).
    pub fn message_format(self) -> &'static str {
        match self {
            DownloadWarning::NotRequested => {
                "Server sending download, but no download was requested\n"
            }
            DownloadWarning::CouldNotCreate => "Could not create %s\n",
            DownloadWarning::RefusedRedirect => {
                "refusing redirect to %s by server (bad checksum)\n"
            }
            DownloadWarning::FailedToInitialize => "Failed to initialize download for '%s'\n",
            DownloadWarning::Command(w) => w.message_format(),
        }
    }
}

/// The first warning of a call.
#[derive(Default)]
struct Warnings(Option<DownloadWarning>);

impl Warnings {
    fn note(&mut self, warning: DownloadWarning) {
        self.0.get_or_insert(warning);
    }

    /// Queues a command, keeping its warning.
    fn command(&mut self, cl: &mut ClientState, text: &[u8]) -> Result<(), GameError> {
        if let Err(w) = add_reliable_command(cl, text)? {
            self.note(DownloadWarning::Command(w));
        }
        Ok(())
    }
}

/// Reads a download message (`svc_download`): a block of the file being
/// downloaded, or a redirect ([`parse_redirect`]). A block that isn't the
/// one expected is dropped (a later one is asked for again with
/// `retransdl`); otherwise the file is opened if it isn't yet, the block
/// written, acknowledged with `nextdl` and counted, and a zero-length block
/// finishes the file: closed, renamed, the names cleared, two packets sent
/// at once (so that the server sees the last `nextdl` before the client
/// spends time loading) and the next download started. Block 0 carries the
/// file's size; a negative one drops the connection with the text after it.
/// Returns the first warning printed.
///
/// Original: `CL_ParseDownload` at 0x004140B0 (CoD2MP_s.exe 1.3; msg in esi)
/// Original: `CL_ParseDownload` at 0x00418F90 (CoDUOMP.exe 1.51; msg in ecx)
pub fn parse_download(
    msg: &mut Msg,
    cl: &mut ClientState,
    dl: &mut DownloadState,
    host: &mut dyn DownloadHost,
) -> Result<Option<DownloadWarning>, GameError> {
    let game = cl.game;
    let mut warnings = Warnings::default();
    let block = msg.read_short();
    if block == -1 {
        if !dl.www {
            redirect(msg, cl, dl, host, &mut warnings)?;
        } else {
            // A redirect is going: the server repeats it until it hears back.
            let mut text = [0u8; MAX_STRING_CHARS];
            msg.read_string(&mut text);
            msg.read_long();
            msg.read_long();
        }
        return Ok(warnings.0);
    }
    if block == 0 {
        dl.size = msg.read_long();
        host.set_size(dl.size);
        if dl.size < 0 {
            let mut text = [0u8; MAX_STRING_CHARS];
            let text = msg.read_string(&mut text);
            return Err(GameError::drop(String::from_utf8_lossy(text).into_owned()));
        }
    }
    let size = msg.read_short();
    let mut data = vec![0u8; size.max(0) as usize];
    if size > 0 {
        msg.read_data(&mut data);
    }
    if dl.block != block {
        // ("CL_ParseDownload: Expected block %d, got %d", a debug line.)
        if block > dl.block {
            warnings.command(cl, format!("retransdl {}", dl.block).as_bytes())?;
        }
        return Ok(warnings.0);
    }
    if dl.file == 0 {
        if dl.temp_name[0] == 0 {
            warnings.note(DownloadWarning::NotRequested);
            warnings.command(cl, b"stopdl")?;
            return Ok(warnings.0);
        }
        dl.file = host.open(c_str(&dl.temp_name));
        if dl.file == 0 {
            warnings.note(DownloadWarning::CouldNotCreate);
            warnings.command(cl, b"stopdl")?;
            host.next_download();
            return Ok(warnings.0);
        }
    }
    if size != 0 {
        host.write(&data, size, dl.file);
    }
    if game == Game::CallOfDuty2 {
        dl.count = dl.count.wrapping_add(size);
        host.set_count(dl.count);
        warnings.command(cl, format!("nextdl {}", dl.block).as_bytes())?;
        dl.block = dl.block.wrapping_add(1);
    } else {
        warnings.command(cl, format!("nextdl {}", dl.block).as_bytes())?;
        dl.count = dl.count.wrapping_add(size);
        dl.block = dl.block.wrapping_add(1);
        host.set_count(dl.count);
    }
    if size == 0 {
        if dl.file != 0 {
            host.close(dl.file);
            dl.file = 0;
            host.rename(c_str(&dl.temp_name), c_str(&dl.name));
        }
        dl.name[0] = 0;
        dl.temp_name[0] = 0;
        host.clear_name();
        host.write_packet();
        host.write_packet();
        host.next_download();
    }
    Ok(warnings.0)
}

/// A redirect (block -1, no redirected download going): the server's web
/// address replaces the name (the name kept as the original name), with the
/// size and the flags. Flag 2: the address is opened ([`DownloadHost::
/// open_url`]), the game quits and the client says `wwwdl bbl8r`.
/// Otherwise the client acknowledges (`wwwdl ack`), builds the temporary
/// file's path under `fs_homepath` (its last character dropped) and starts
/// the web download, or says `wwwdl fail`; flag 1 then makes it say `wwwdl
/// bbl8r` and expect to be disconnected. United Offensive first refuses an
/// original name in its bad-checksum list (`wwwdl fail`).
///
/// Original: `CL_ParseDownloadRedirect` at 0x00413E70 (CoD2MP_s.exe 1.3; msg in eax; our name)
///
/// (United Offensive has the same code inside `CL_ParseDownload`.)
pub fn parse_redirect(
    msg: &mut Msg,
    cl: &mut ClientState,
    dl: &mut DownloadState,
    host: &mut dyn DownloadHost,
) -> Result<Option<DownloadWarning>, GameError> {
    let mut warnings = Warnings::default();
    redirect(msg, cl, dl, host, &mut warnings)?;
    Ok(warnings.0)
}

fn redirect(
    msg: &mut Msg,
    cl: &mut ClientState,
    dl: &mut DownloadState,
    host: &mut dyn DownloadHost,
    warnings: &mut Warnings,
) -> Result<(), GameError> {
    let cod2 = cl.game == Game::CallOfDuty2;
    let home = host.home_path();
    let name = dl.name;
    copy_name(&mut dl.original_name, &name, ORIGINAL_NAME_SIZE - 1);
    let mut text = [0u8; MAX_STRING_CHARS];
    let url = msg.read_string(&mut text);
    copy_name(&mut dl.name, url, NAME_SIZE - 1);
    dl.size = msg.read_long();
    dl.flags = msg.read_long();
    if dl.flags & 2 != 0 {
        if !host.open_url(c_str(&dl.name)) && cod2 {
            let url = String::from_utf8_lossy(c_str(&dl.name)).into_owned();
            return Err(GameError::drop(format!(
                "EXE_ERR_COULDNT_OPEN_URL\x15{url}"
            )));
        }
        host.add_text(b"quit\n");
        if cod2 {
            host.add_text(b"quit\n");
        }
        warnings.command(cl, b"wwwdl bbl8r")?;
        if cod2 {
            host.end_download();
            dl.www = false;
        } else {
            dl.www_aborting = true;
        }
        return Ok(());
    }
    host.set_size(dl.size);
    // ("Server redirected download: %s", a debug line.)
    dl.www = true;
    warnings.command(cl, b"wwwdl ack")?;
    if !cod2 {
        let mut needle = b"@".to_vec();
        needle.extend_from_slice(c_str(&dl.original_name));
        let list = c_str(&dl.bad_checksums);
        if list.windows(needle.len()).any(|w| w == needle.as_slice()) {
            warnings.note(DownloadWarning::RefusedRedirect);
            warnings.command(cl, b"wwwdl fail")?;
            dl.www_aborting = true;
            return Ok(());
        }
    }
    let path = host.os_path(&home, c_str(&dl.temp_name), b"");
    copy_name(&mut dl.temp_name, &path, NAME_SIZE - 1);
    let n = c_str(&dl.temp_name).len();
    // The trailing separator: the byte before the end, which for an empty
    // path is the last byte of the name before it in memory (already 0).
    if n > 0 {
        dl.temp_name[n - 1] = 0;
    }
    if !host.begin_download(c_str(&dl.temp_name), c_str(&dl.name)) {
        warnings.command(cl, b"wwwdl fail")?;
        if cod2 {
            host.end_download();
        }
        warnings.note(DownloadWarning::FailedToInitialize);
        if cod2 {
            dl.www = false;
        } else {
            dl.www_aborting = true;
        }
    }
    if dl.flags & 1 != 0 {
        warnings.command(cl, b"wwwdl bbl8r")?;
        dl.www_disconnected = true;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client::{max_reliable_commands, SERVER_COMMAND_SIZE};

    /// A host that records what it is asked.
    #[derive(Default)]
    struct Recorder {
        calls: Vec<String>,
        open: i32,
        shell: bool,
    }

    impl DownloadHost for Recorder {
        fn open(&mut self, temp_name: &[u8]) -> i32 {
            self.calls
                .push(format!("open {}", String::from_utf8_lossy(temp_name)));
            self.open
        }
        fn write(&mut self, data: &[u8], length: i32, file: i32) {
            self.calls
                .push(format!("write {} {length} {file}", data.len()));
        }
        fn close(&mut self, file: i32) {
            self.calls.push(format!("close {file}"));
        }
        fn rename(&mut self, from: &[u8], to: &[u8]) {
            let (f, t) = (String::from_utf8_lossy(from), String::from_utf8_lossy(to));
            self.calls.push(format!("rename {f} {t}"));
        }
        fn write_packet(&mut self) {
            self.calls.push("packet".into());
        }
        fn next_download(&mut self) {
            self.calls.push("next".into());
        }
        fn set_size(&mut self, size: i32) {
            self.calls.push(format!("size {size}"));
        }
        fn set_count(&mut self, count: i32) {
            self.calls.push(format!("count {count}"));
        }
        fn clear_name(&mut self) {
            self.calls.push("clear".into());
        }
        fn home_path(&mut self) -> Vec<u8> {
            b"home".to_vec()
        }
        fn os_path(&mut self, base: &[u8], game: &[u8], qpath: &[u8]) -> Vec<u8> {
            [base, b"/", game, b"/", qpath].concat()
        }
        fn open_url(&mut self, url: &[u8]) -> bool {
            self.calls
                .push(format!("url {}", String::from_utf8_lossy(url)));
            self.shell
        }
        fn add_text(&mut self, text: &[u8]) {
            self.calls
                .push(format!("text {}", String::from_utf8_lossy(text).trim()));
        }
        fn begin_download(&mut self, local: &[u8], remote: &[u8]) -> bool {
            let (l, r) = (
                String::from_utf8_lossy(local),
                String::from_utf8_lossy(remote),
            );
            self.calls.push(format!("begin {l} {r}"));
            true
        }
        fn end_download(&mut self) {
            self.calls.push("end".into());
        }
    }

    fn message(parts: &[&[u8]]) -> Vec<u8> {
        parts.concat()
    }

    fn last_command(cl: &ClientState) -> String {
        let ring = max_reliable_commands(cl.game);
        let slot = (cl.reliable_sequence as u32 as usize % ring) * SERVER_COMMAND_SIZE;
        String::from_utf8_lossy(c_str(
            &cl.reliable_commands[slot..slot + SERVER_COMMAND_SIZE],
        ))
        .into_owned()
    }

    fn named(name: &[u8]) -> [u8; NAME_SIZE] {
        let mut out = [0; NAME_SIZE];
        out[..name.len()].copy_from_slice(name);
        out
    }

    fn run(
        game: Game,
        data: &[u8],
        dl: &mut DownloadState,
        host: &mut Recorder,
    ) -> (ClientState, Result<Option<DownloadWarning>, GameError>) {
        let mut cl = ClientState::new(game);
        let mut buffer = data.to_vec();
        buffer.resize(data.len() + 64, 0);
        let mut msg = Msg::new(game, &mut buffer);
        msg.cursize = data.len();
        let result = parse_download(&mut msg, &mut cl, dl, host);
        (cl, result)
    }

    #[test]
    fn a_file_in_two_blocks() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let mut dl = DownloadState {
                name: named(b"maps/mp/x.iwd"),
                temp_name: named(b"maps/mp/x.iwd.tmp"),
                ..DownloadState::default()
            };
            let mut host = Recorder {
                open: 7,
                ..Recorder::default()
            };
            // Block 0: the size (5), then 5 bytes.
            let first = message(&[
                &0i16.to_le_bytes(),
                &5i32.to_le_bytes(),
                &5i16.to_le_bytes(),
                b"abcde",
            ]);
            let (cl, result) = run(game, &first, &mut dl, &mut host);
            assert_eq!(result.unwrap(), None);
            assert_eq!(last_command(&cl), "nextdl 0");
            assert_eq!((dl.block, dl.count, dl.size, dl.file), (1, 5, 5, 7));
            // Block 1: empty, the end.
            let end = message(&[&1i16.to_le_bytes(), &0i16.to_le_bytes()]);
            let (cl, result) = run(game, &end, &mut dl, &mut host);
            assert_eq!(result.unwrap(), None);
            assert_eq!(last_command(&cl), "nextdl 1");
            assert_eq!((dl.file, dl.name[0], dl.temp_name[0]), (0, 0, 0));
            let tail: Vec<&str> = host
                .calls
                .iter()
                .map(String::as_str)
                .skip_while(|c| *c != "close 7")
                .collect();
            assert_eq!(
                tail,
                [
                    "close 7",
                    "rename maps/mp/x.iwd.tmp maps/mp/x.iwd",
                    "clear",
                    "packet",
                    "packet",
                    "next"
                ]
            );
        }
    }

    #[test]
    fn a_block_nobody_asked_for() {
        let mut dl = DownloadState::default();
        let mut host = Recorder::default();
        let data = message(&[
            &0i16.to_le_bytes(),
            &5i32.to_le_bytes(),
            &0i16.to_le_bytes(),
        ]);
        let (cl, result) = run(Game::CallOfDuty2, &data, &mut dl, &mut host);
        assert_eq!(result.unwrap(), Some(DownloadWarning::NotRequested));
        assert_eq!(last_command(&cl), "stopdl");
    }

    #[test]
    fn a_later_block_asks_again() {
        let mut dl = DownloadState {
            block: 3,
            ..DownloadState::default()
        };
        let mut host = Recorder::default();
        let data = message(&[&5i16.to_le_bytes(), &0i16.to_le_bytes()]);
        let (cl, _) = run(Game::UnitedOffensive, &data, &mut dl, &mut host);
        assert_eq!(last_command(&cl), "retransdl 3");
        assert_eq!(dl.block, 3);
    }

    #[test]
    fn a_web_page_not_opened() {
        let redirect = message(&[
            &(-1i16).to_le_bytes(),
            b"http://example.invalid/x\0",
            &1i32.to_le_bytes(),
            &2i32.to_le_bytes(),
        ]);
        // Call of Duty 2 drops the connection when the address can't be opened.
        let mut host = Recorder::default();
        let (_, result) = run(
            Game::CallOfDuty2,
            &redirect,
            &mut DownloadState::default(),
            &mut host,
        );
        assert!(result.is_err());
        assert_eq!(host.calls, ["url http://example.invalid/x"]);
        // United Offensive goes on: quit, `wwwdl bbl8r`, nothing more until a new game state.
        let mut host = Recorder::default();
        let mut dl = DownloadState::default();
        let (cl, result) = run(Game::UnitedOffensive, &redirect, &mut dl, &mut host);
        assert_eq!(result.unwrap(), None);
        assert_eq!(last_command(&cl), "wwwdl bbl8r");
        assert!(dl.www_aborting);
    }

    #[test]
    fn a_redirect_starts_a_web_download() {
        let mut dl = DownloadState {
            name: named(b"x.iwd"),
            temp_name: named(b"x.iwd.tmp"),
            ..DownloadState::default()
        };
        let mut host = Recorder::default();
        let redirect = message(&[
            &(-1i16).to_le_bytes(),
            b"http://example.invalid/x.iwd\0",
            &9i32.to_le_bytes(),
            &0i32.to_le_bytes(),
        ]);
        let (cl, result) = run(Game::CallOfDuty2, &redirect, &mut dl, &mut host);
        assert_eq!(result.unwrap(), None);
        assert_eq!(last_command(&cl), "wwwdl ack");
        assert!(dl.www);
        assert_eq!(c_str(&dl.original_name), b"x.iwd");
        // home/x.iwd.tmp/ with the last character dropped.
        assert_eq!(c_str(&dl.temp_name), b"home/x.iwd.tmp");
        assert_eq!(
            host.calls.last().unwrap(),
            "begin home/x.iwd.tmp http://example.invalid/x.iwd"
        );
    }
}
