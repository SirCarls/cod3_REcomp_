#include "iw_pc/client/download.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace iw_pc::client {

namespace {

ByteView text(const char* s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)); }

// strncpy(out, text, n) then out[n] = 0.
void copy_name(std::span<std::uint8_t> out, ByteView source, std::size_t n) {
    ByteView s = c_str(source);
    std::size_t k = std::min(s.size(), n);
    std::copy(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), out.begin());
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(k), out.begin() + static_cast<std::ptrdiff_t>(n), 0);
    out[n] = 0;
}

// The first warning of a call.
struct Warnings {
    std::optional<DownloadWarning> first;
    void note(DownloadWarning w) {
        if (!first) first = w;
    }
    // Queues a command, keeping its warning; the ring's error ends the call.
    Status command(ClientState& cl, ByteView command) {
        Queued q = add_reliable_command(cl, command);
        if (q.error) return q.error;
        if (q.warning) note({DownloadWarningKind::Command, *q.warning});
        return std::nullopt;
    }
    Status command(ClientState& cl, const std::string& s) {
        return command(cl, ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    }
};

Status redirect(net::Msg& msg, ClientState& cl, DownloadState& dl, DownloadHost& host, Warnings& warnings) {
    bool cod2 = cl.game == Game::CallOfDuty2;
    std::vector<std::uint8_t> home = host.home_path();
    std::array<std::uint8_t, kDownloadNameSize> name = dl.name;
    copy_name(dl.original_name, name, kOriginalDownloadNameSize - 1);
    std::array<std::uint8_t, net::kMaxStringChars> buffer{};
    ByteView url = msg.read_string(buffer);
    copy_name(dl.name, url, kDownloadNameSize - 1);
    dl.size = msg.read_long();
    dl.flags = msg.read_long();
    if (dl.flags & 2) {
        if (!host.open_url(c_str(dl.name)) && cod2) {
            ByteView u = c_str(dl.name);
            return GameError::drop("EXE_ERR_COULDNT_OPEN_URL\x15" + std::string(u.begin(), u.end()));
        }
        host.add_text(text("quit\n"));
        if (cod2) host.add_text(text("quit\n"));
        if (Status e = warnings.command(cl, text("wwwdl bbl8r"))) return e;
        if (cod2) {
            host.end_download();
            dl.www = false;
        } else {
            dl.www_aborting = true;
        }
        return std::nullopt;
    }
    host.set_size(dl.size);
    // ("Server redirected download: %s", a debug line.)
    dl.www = true;
    if (Status e = warnings.command(cl, text("wwwdl ack"))) return e;
    if (!cod2) {
        std::vector<std::uint8_t> needle{'@'};
        ByteView original = c_str(dl.original_name);
        needle.insert(needle.end(), original.begin(), original.end());
        ByteView list = c_str(dl.bad_checksums);
        if (std::search(list.begin(), list.end(), needle.begin(), needle.end()) != list.end()) {
            warnings.note({DownloadWarningKind::RefusedRedirect});
            if (Status e = warnings.command(cl, text("wwwdl fail"))) return e;
            dl.www_aborting = true;
            return std::nullopt;
        }
    }
    std::vector<std::uint8_t> path = host.os_path(home, c_str(dl.temp_name), ByteView());
    copy_name(dl.temp_name, path, kDownloadNameSize - 1);
    std::size_t n = c_str(dl.temp_name).size();
    // The trailing separator (for an empty path, the byte before: already 0).
    if (n > 0) dl.temp_name[n - 1] = 0;
    if (!host.begin_download(c_str(dl.temp_name), c_str(dl.name))) {
        if (Status e = warnings.command(cl, text("wwwdl fail"))) return e;
        if (cod2) host.end_download();
        warnings.note({DownloadWarningKind::FailedToInitialize});
        if (cod2) dl.www = false;
        else dl.www_aborting = true;
    }
    if (dl.flags & 1) {
        if (Status e = warnings.command(cl, text("wwwdl bbl8r"))) return e;
        dl.www_disconnected = true;
    }
    return std::nullopt;
}

}  // namespace

ByteView c_str(ByteView bytes) {
    auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes.first(static_cast<std::size_t>(end - bytes.begin()));
}

const char* message_format(const DownloadWarning& w) {
    switch (w.kind) {
        case DownloadWarningKind::NotRequested: return "Server sending download, but no download was requested\n";
        case DownloadWarningKind::CouldNotCreate: return "Could not create %s\n";
        case DownloadWarningKind::RefusedRedirect: return "refusing redirect to %s by server (bad checksum)\n";
        case DownloadWarningKind::FailedToInitialize: return "Failed to initialize download for '%s'\n";
        case DownloadWarningKind::Command: return net::message_format(w.command);
    }
    return "";
}

Expected<std::optional<DownloadWarning>> parse_download(net::Msg& msg, ClientState& cl, DownloadState& dl,
                                                         DownloadHost& host) {
    Game game = cl.game;
    Warnings warnings;
    std::int32_t block = msg.read_short();
    if (block == -1) {
        if (!dl.www) {
            if (Status e = redirect(msg, cl, dl, host, warnings)) return *e;
        } else {
            // A redirect is going: the server repeats it until it hears back.
            std::array<std::uint8_t, net::kMaxStringChars> buffer{};
            msg.read_string(buffer);
            msg.read_long();
            msg.read_long();
        }
        return warnings.first;
    }
    if (block == 0) {
        dl.size = msg.read_long();
        host.set_size(dl.size);
        if (dl.size < 0) {
            std::array<std::uint8_t, net::kMaxStringChars> buffer{};
            ByteView t = msg.read_string(buffer);
            return GameError::drop(std::string(t.begin(), t.end()));
        }
    }
    std::int32_t size = msg.read_short();
    std::vector<std::uint8_t> data(static_cast<std::size_t>(std::max(size, 0)), 0);
    if (size > 0) msg.read_data(data);
    if (dl.block != block) {
        // ("CL_ParseDownload: Expected block %d, got %d", a debug line.)
        if (block > dl.block) {
            if (Status e = warnings.command(cl, "retransdl " + std::to_string(dl.block))) return *e;
        }
        return warnings.first;
    }
    if (dl.file == 0) {
        if (dl.temp_name[0] == 0) {
            warnings.note({DownloadWarningKind::NotRequested});
            if (Status e = warnings.command(cl, text("stopdl"))) return *e;
            return warnings.first;
        }
        dl.file = host.open(c_str(dl.temp_name));
        if (dl.file == 0) {
            warnings.note({DownloadWarningKind::CouldNotCreate});
            if (Status e = warnings.command(cl, text("stopdl"))) return *e;
            host.next_download();
            return warnings.first;
        }
    }
    if (size != 0) host.write(data, size, dl.file);
    auto add = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    if (game == Game::CallOfDuty2) {
        dl.count = add(dl.count, size);
        host.set_count(dl.count);
        if (Status e = warnings.command(cl, "nextdl " + std::to_string(dl.block))) return *e;
        dl.block = add(dl.block, 1);
    } else {
        if (Status e = warnings.command(cl, "nextdl " + std::to_string(dl.block))) return *e;
        dl.count = add(dl.count, size);
        dl.block = add(dl.block, 1);
        host.set_count(dl.count);
    }
    if (size == 0) {
        if (dl.file != 0) {
            host.close(dl.file);
            dl.file = 0;
            host.rename(c_str(dl.temp_name), c_str(dl.name));
        }
        dl.name[0] = 0;
        dl.temp_name[0] = 0;
        host.clear_name();
        host.write_packet();
        host.write_packet();
        host.next_download();
    }
    return warnings.first;
}

Expected<std::optional<DownloadWarning>> parse_redirect(net::Msg& msg, ClientState& cl, DownloadState& dl,
                                                         DownloadHost& host) {
    Warnings warnings;
    if (Status e = redirect(msg, cl, dl, host, warnings)) return *e;
    return warnings.first;
}

}  // namespace iw_pc::client
