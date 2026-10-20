// The `connect` console command (see the Rust version, iw_pc::client::connect):
// what the client does when the player names a server to join, before any
// packet goes out. The rest of the game goes through ConnectHost; the
// originals' CD key check is the host's answer (not ported).
#pragma once

#include <span>

#include "iw_pc/base.h"
#include "iw_pc/client/client.h"

namespace iw_pc::client {

/// The port a server name without one gets (PORT_SERVER).
inline constexpr std::uint16_t kDefaultServerPort = 28960;

/// What `connect` needs from the rest of the game (see the Rust version for each).
class ConnectHost {
public:
    virtual ~ConnectHost() = default;
    virtual bool server_running() = 0;                          // sv_running
    virtual void stop_sounds() = 0;                             // SND_StopSounds(0) / S_StopAllSounds(0)
    virtual void set_setting(ByteView name, ByteView value) = 0;  // Cvar_Set2(name, value, 1); UO only
    virtual void local_server_quit() = 0;                       // CoD2: the menus' flag; UO: Com_ShutdownServer
    virtual void clear_menu_texts() = 0;                        // CoD2: the menus' two texts
    virtual void server_frame() = 0;                            // SV_Frame(0)
    virtual void disconnect() = 0;                              // CL_Disconnect (UO: (1))
    virtual void close_console() = 0;                           // Con_Close
    virtual bool resolve(ByteView name, net::NetAddress& address) = 0;  // NET_StringToAdr
    virtual bool cd_key_valid() = 0;                            // CL_CDKeyValidate (the host's answer)
    virtual void clear_key_catchers() = 0;                      // cls.keyCatchers
    virtual std::int32_t net_qport() = 0;                       // CoD2's copy of net_qport
    virtual void close_menus() = 0;                             // CoD2: UI_CloseAll, SCR_UpdateScreen
};

/// How `connect` ended (besides an error).
enum class ConnectResult { Usage, BadAddress, Connecting };

/// What the original prints for a result, as its format (nullptr: nothing but the resolved line).
const char* message_format(ConnectResult r);

/// The line `connect` prints once the name resolved.
inline constexpr const char* kResolvedFormat = "%s resolved to %i.%i.%i.%i:%i\n";

/// The `connect` command with the console's words `args` (the command first).
/// Original: CL_Connect_f at 0x004B3900 (CoD2MP_s.exe 1.3)
/// Original: CL_Connect_f at 0x00410FE0 (CoDUOMP.exe 1.51)
Expected<ConnectResult> connect_command(ClientState& cl, std::span<const ByteView> args, ConnectHost& host);

}  // namespace iw_pc::client
