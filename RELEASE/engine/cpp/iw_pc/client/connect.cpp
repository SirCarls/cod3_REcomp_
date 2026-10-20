#include "iw_pc/client/connect.h"

#include <algorithm>
#include <cstring>

namespace iw_pc::client {

namespace {

ByteView text(const char* s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)); }

// Up to the first NUL.
ByteView c_string(ByteView bytes) {
    auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    return bytes.first(static_cast<std::size_t>(end - bytes.begin()));
}

}  // namespace

const char* message_format(ConnectResult r) {
    switch (r) {
        case ConnectResult::Usage: return "usage: connect [server]\n";
        case ConnectResult::BadAddress: return "Bad server address\n";
        case ConnectResult::Connecting: return nullptr;
    }
    return nullptr;
}

Expected<ConnectResult> connect_command(ClientState& cl, std::span<const ByteView> args, ConnectHost& host) {
    if (args.size() != 2) return ConnectResult::Usage;
    bool cod2 = cl.game == Game::CallOfDuty2;
    host.stop_sounds();
    if (!cod2) host.set_setting(text("r_uiFullScreen"), text("0"));
    if (!cl.server_message.empty()) cl.server_message[0] = 0;
    ByteView server = args[1];
    ByteView typed = c_string(server);
    ByteView localhost = text("localhost");
    if (host.server_running() && std::ranges::equal(typed, localhost)) {
        host.local_server_quit();
    }
    if (cod2) host.clear_menu_texts();
    else host.set_setting(text("sv_killserver"), text("1"));
    host.server_frame();
    host.disconnect();
    host.close_console();

    // cls.servername: strncpy(name, server, 255), the last byte 0.
    std::size_t n = std::min(typed.size(), kServerNameSize - 1);
    std::fill(cl.server_name.begin(), cl.server_name.end(), std::uint8_t{0});
    std::copy(typed.begin(), typed.begin() + static_cast<std::ptrdiff_t>(n), cl.server_name.begin());
    std::vector<std::uint8_t> name(typed.begin(), typed.begin() + static_cast<std::ptrdiff_t>(n));

    if (!host.resolve(name, cl.server_address)) {
        cl.connection_state = 0;
        if (!cod2) host.set_setting(text("ui_dl_running"), text("0"));
        return ConnectResult::BadAddress;
    }
    if (cl.server_address.port == 0) cl.server_address.port = kDefaultServerPort;
    bool local = cl.server_address.kind == net::AddressType::Loopback || cl.server_address.kind == net::AddressType::Bot;
    if (!local && !host.cd_key_valid()) return GameError::drop("EXE_ERR_INVALID_CD_KEY");
    cl.connection_state = local ? (cod2 ? 4 : 2) : (cod2 ? 3 : 1);
    host.clear_key_catchers();
    cl.connect_time = -99999;
    cl.connect_packet_count = 0;
    if (cod2) {
        cl.qport = host.net_qport();
        host.close_menus();
    } else {
        host.set_setting(text("cl_currentServerAddress"), typed);  // the name as typed
    }
    return ConnectResult::Connecting;
}

}  // namespace iw_pc::client
