// Downloads from the server (svc_download): the blocks of a file the client
// asked for, or the server's redirect to a web address (see the Rust
// version, iw_pc::client::download, for the whole story; and its note on the
// shell: a server's flag 2 makes the originals open its address and quit).
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "iw_pc/base.h"
#include "iw_pc/client/client.h"
#include "iw_pc/net/msg.h"

namespace iw_pc::client {

inline constexpr std::size_t kDownloadNameSize = 256;         // downloadName, downloadTempName
inline constexpr std::size_t kOriginalDownloadNameSize = 64;  // originalDownloadName

/// The client's download (C strings in fixed buffers, as the originals keep them).
struct DownloadState {
    std::int32_t block = 0;   // the block expected next
    std::int32_t size = 0;    // the file's size
    std::int32_t count = 0;   // the bytes so far (CoD2: the menus' number)
    std::int32_t file = 0;    // the file being written (0: none)
    std::array<std::uint8_t, kDownloadNameSize> name{};       // after a redirect, its web address
    std::array<std::uint8_t, kDownloadNameSize> temp_name{};  // after a redirect, its path
    std::array<std::uint8_t, kOriginalDownloadNameSize> original_name{};
    std::int32_t flags = 0;   // the redirect's: 1 disconnect while downloading, 2 open the address and quit
    bool www = false;              // a redirected download is going
    bool www_aborting = false;     // United Offensive: the redirect failed or was refused
    bool www_disconnected = false; // flag 1
    std::vector<std::uint8_t> bad_checksums;  // United Offensive: "@name" each
};

/// A C string in a buffer: up to its first NUL.
ByteView c_str(ByteView bytes);

/// What a download does outside the client's state (see the Rust version).
class DownloadHost {
public:
    virtual ~DownloadHost() = default;
    virtual std::int32_t open(ByteView temp_name) = 0;                     // FS_SV_FOpenFileWrite
    virtual void write(ByteView data, std::int32_t length, std::int32_t file) = 0;  // FS_Write
    virtual void close(std::int32_t file) = 0;                             // FS_FCloseFile
    virtual void rename(ByteView from, ByteView to) = 0;                   // FS_SV_Rename
    virtual void write_packet() = 0;                                       // CL_WritePacket
    virtual void next_download() = 0;                                      // CL_NextDownload
    virtual void set_size(std::int32_t size) = 0;                          // the menus' numbers
    virtual void set_count(std::int32_t count) = 0;
    virtual void clear_name() = 0;
    virtual std::vector<std::uint8_t> home_path() = 0;                     // fs_homepath's text
    virtual std::vector<std::uint8_t> os_path(ByteView base, ByteView game, ByteView qpath) = 0;  // FS_BuildOSPath
    virtual bool open_url(ByteView url) = 0;                               // the shell; see the Rust version
    virtual void add_text(ByteView text) = 0;                              // Cbuf_AddText
    virtual bool begin_download(ByteView local, ByteView remote) = 0;      // DL_BeginDownload
    virtual void end_download() = 0;                                       // CoD2: ends the web download
};

/// A warning the original prints.
enum class DownloadWarningKind { NotRequested, CouldNotCreate, RefusedRedirect, FailedToInitialize, Command };
struct DownloadWarning {
    DownloadWarningKind kind;
    net::ReliableCommandWarning command{};  // for Command
};
/// Its format (both games, the same text).
const char* message_format(const DownloadWarning& w);

/// Reads a download message: a block of the file, or a redirect (see the Rust version).
/// Original: CL_ParseDownload at 0x004140B0 (CoD2MP_s.exe 1.3; msg in esi)
/// Original: CL_ParseDownload at 0x00418F90 (CoDUOMP.exe 1.51; msg in ecx)
Expected<std::optional<DownloadWarning>> parse_download(net::Msg& msg, ClientState& cl, DownloadState& dl,
                                                         DownloadHost& host);

/// A redirect (block -1, no redirected download going; see the Rust version).
/// Original: CL_ParseDownloadRedirect at 0x00413E70 (CoD2MP_s.exe 1.3; msg in eax; our name)
/// (United Offensive has the same code inside CL_ParseDownload.)
Expected<std::optional<DownloadWarning>> parse_redirect(net::Msg& msg, ClientState& cl, DownloadState& dl,
                                                         DownloadHost& host);

}  // namespace iw_pc::client
