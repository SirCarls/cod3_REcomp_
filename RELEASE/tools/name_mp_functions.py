#!/usr/bin/env python3
"""Builds a first symbol map for Call of Duty 3's multiplayer program,
codmp_xenonf.xex, with the generic passes of tools/name_functions.py (import
thunks, register helpers, self-naming strings, RTTI, command tables, asset
callbacks, XAPI wrappers, dvar globals) seeded with the few multiplayer
addresses checked by hand below. The single-player tool's "known" and
"curated" lists are default.xex addresses and are not used here.

    python3 tools/name_mp_functions.py [codmp.pe] [--imports codmp.imports]
                                       [--config config/codmp.toml]
                                       [--q3-src <Quake-III-Arena checkout>]
                                       [-o symbols_mp.toml] [--ppcx-names codmp.pe.names]
                                       [--names-toml names_mp.toml] [--avoid <file>]

Inputs are made the same way as for default.xex: xex_dump.py (the image) and
xex_imports.py (the listing). With the campaign's image and symbol map
(--campaign-pe, --campaign-symbols; found next to the image and in
recomp/config by default) the functions that have exactly the same
instructions as a named campaign function, and only one match on each side,
get its name, and the globals they address at the same instruction get the
campaign's global names (`pass_same_as_campaign`). --names-toml also writes the codegen's [names]
table (SDK patch 0011) the way tools/gen_codegen_names.py does: known/high
names only, import thunks and helpers left out, names on the --avoid lists
suffixed _guest, duplicated names dropped.
"""
from __future__ import annotations

import argparse
import collections
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import name_functions as nf  # noqa: E402  (the single-player tool, used as a library)

# Checked by hand in the multiplayer image (dist/dev/agents/multiplayer/NOTES.md
# has the evidence). Same roles as the single-player names in addresses.h.
KNOWN_MP_FUNCTIONS = [
    (0x82322010, "setjmp", "high", "CRT setjmp: same body as default.xex 0x82351CC0 (mflr r0, mfcr r4, "
     "stfd f14..); seven callers"),
    (0x8231E400, "longjmp", "high", "CRT longjmp: byte-identical to default.xex 0x8234EEC0"),
    (0x824C5298, "ScriptThread_Run", "high", "default.xex ScriptThread_Run 0x824A6150: same length (0x358), "
     "all 214 primary opcodes equal; setjmp call at +0x8C, resume at +0x25C, blr at +0x330"),
    (0x823911E8, "ScriptThread_CallEntry", "high", "the call at ScriptThread_Run+0x154 (0x824C53EC), where "
     "default.xex calls ScriptThread_CallEntry 0x823C0B98; the same four instructions (the entry functor's "
     "slot 2 through ctr)"),
    (0x82141E40, "Jobs_WakeWorkers", "high", "PulseEvent on the job workers' event 0x82A8B89C (its only "
     "NtPulseEvent call); the same three instructions as default.xex 0x8212A100 (game/job_wake.cpp)"),
    (0x825447E0, "Com_Error", "high", "the same 8 instructions as default.xex Com_Error 0x825239A0 (a call "
     "to DbgBreakPoint in this build; one other function has that shape too) and 110 callers against the "
     "campaign's 112"),
    (0x8248A388, "Cmd_AddCommand", "high", "'Cmd_AddCommand: %s already defined'"),
    (0x8248A490, "Cmd_AddButtonCommand", "medium", "second body with 'Cmd_AddCommand: %s already defined' "
     "(default.xex: 0x8246CAA0 next to Cmd_AddCommand)"),
    (0x8248A598, "Cmd_AddServerCommand", "high", "'Cmd_AddServerCommand: %s already defined'"),
    (0x8254F458, "Cvar_Get", "high", "'Cvar_Get: NULL parameter'"),
    (0x825583F8, "Com_Init", "high", "'--- Common Initialization Complete ---'; registers sv_cheats"),
    (0x82415AA0, "MP_ReadLaunchData", "high", "XamLoaderGetLaunchDataSize / XamLoaderGetLaunchData, then "
     "copies the 664-byte controls block (li r5,664) the single-player program stored"),
    # Controls and profiles (game/mp/controls_mp.cpp, 2026-10-19).
    (0x82507AD8, "Controls_ExecLayoutFiles", "high", "layout 0-3 in r3: Cbuf_AddText(va('exec %s_<a-d>.cfg', "
     "'xenon')) then va('exec mp_%s_<a-d>.cfg'), then Cbuf_Execute (the campaign's 0x824E1BD0 runs only the "
     "first)"),
    (0x824178E0, "Profile_SettingsReadDone", "high", "a profile settings read's completion (callback at "
     "0x82417B08): status at request+8, profile at +0x154; on success assigns the 6984-byte block to "
     "0x82C6B730 + profile * 6984 (0x824E6098), copies its first 664 bytes to 0x82C9BD40 and calls the "
     "callback at 0x82BCCA8C"),
    (0x824E6098, "ProfileSettings__Assign", "high", "member-wise copy of a 6984-byte profile settings block "
     "(0x824DA350 for the first 664 bytes, then XMemCpy of 0x170 at +0x29C and +0x40C, ...); "
     "Profile_SettingsReadDone's assignment"),
    # The title's network library: one wrapper per NetDll_ import (caller 1 =
    # title). The thin ones (NetDll_socket_wrapper ...) are named by the
    # generic passes; these three have a body of their own (.pdata entries).
    (0x8222C678, "NetDll_XNetStartup_wrapper", "high", "XamGetSystemVersion, XexGetModuleHandle / "
     "XexGetProcedureAddress, then the only call to NetDll_XNetStartup (0x8222C714)"),
    (0x8222C838, "NetDll_XNetQosLookup_wrapper", "high", "the only call to NetDll_XNetQosLookup "
     "(0x8222C88C)"),
    (0x8222C8B8, "NetDll_WSAStartup_wrapper", "high", "XamGetSystemVersion, XexGetModuleHandle / "
     "XexGetProcedureAddress, then the only call to NetDll_WSAStartup (0x8222C954)"),
    (0x8222CA38, "XSessionCreate", "high", "XamSessionCreateHandle then XMsgStartIORequest(0xFB, 0xB0010)"),
    (0x8222CBC8, "XSessionDelete", "high", "XMsgStartIORequest(0xFB, 0xB0011)"),
    (0x8222CC78, "XSessionJoinRemote", "medium", "XMsgStartIORequest(0xFB, 0xB0012), first of two"),
    (0x8222CD20, "XSessionJoinLocal", "medium", "XMsgStartIORequest(0xFB, 0xB0012), second of two"),
    (0x8222CDC8, "XSessionLeaveRemote", "medium", "XMsgStartIORequest(0xFB, 0xB0013), first of two"),
    (0x8222CE70, "XSessionLeaveLocal", "medium", "XMsgStartIORequest(0xFB, 0xB0013), second of two"),
    (0x8222CF18, "XSessionArbitrationRegister", "high", "XMsgStartIORequest(0xFB, 0xB001A)"),
    (0x8222CFF0, "XSessionStart", "high", "XMsgStartIORequest(0xFB, 0xB0014)"),
    (0x8222D088, "XSessionEnd", "high", "XMsgStartIORequest(0xFB, 0xB0015)"),
    (0x8222D138, "XSessionSearch", "high", "XMsgStartIORequest(0xFB, 0xB0016)"),
    (0x8222D228, "XSessionSearchByID", "high", "XMsgStartIORequest(0xFB, 0xB001B)"),
    (0x8222D310, "XSessionWriteStats", "high", "XMsgStartIORequest(0xFB, 0xB0025)"),
    (0x82312C50, "XUserWriteAchievements", "high", "XMsgStartIORequest(0xFB, 0xB0008)"),
    (0x82312CD0, "XUserSetContextEx", "high", "XMsgStartIORequest(0xFB, 0xB0006)"),
    (0x82312D70, "XUserSetPropertyEx", "high", "XMsgStartIORequest(0xFB, 0xB0007)"),
    (0x82312EC0, "XUserReadStats", "high", "XMsgStartIORequest(0xFB, 0xB0021)"),
    (0x82312B30, "XLaunchNewImage", "high", "tail call to XamLoaderLaunchTitle (the only one)"),
    (0x823140C8, "_xex_entry", "high", "the XEX entry point; calls XamLoaderTerminateTitle twice"),
    # The network layers (2026-10-13; evidence in the Rust decomp's helper notes,
    # dev/agents/rust-decomp/protocol/): the loopback-only Quake III layer (MSG,
    # netchan, svc/clc) and the Demonware peer messages (bdBitBuffer, MPPeer).
    # docs/MP_PROTOCOL.md describes both.
    (0x820D9770, "bdBitBuffer::writeBool", "high",
     "writeDataType(1) then 1 bit (0xFF or 0)"),
    (0x820D9888, "bdBitBuffer::writeInt32", "high",
     "writeDataType(7) then 32 bits of the byte-swapped value"),
    (0x820D98F0, "bdBitBuffer::writeUInt32", "high",
     "writeDataType(8) then 32 bits of the byte-swapped value"),
    (0x820D99E0, "bdBitBuffer::writeFloat32", "medium",
     "writeDataType(13) then 32 bits of the float; writeRangedFloat32 falls back to it"),
    (0x820D9B18, "bdBitBuffer::readUInt8", "medium",
     "readDataType(3) then readBits(8); returns success; used by the 0x23 handler for the player index"),
    (0x821AEF80, "bdBitBuffer::readBits", "medium",
     "called by the typed readers with (dst, numBits) (mirror of writeBits)"),
    (0x821AF0B8, "bdBitBuffer::readDataType", "medium",
     "called first by the typed readers with the type id (mirror of writeDataType)"),
    (0x821AF670, "bdBitBuffer::readRangedFloat32", "medium",
     "called with the same (lo, hi, prec) constants as the writeRangedFloat32 calls it mirrors (0x823D7FF8, 0x823D8140)"),
    (0x821AFB08, "bdBitBuffer::writeBits", "high",
     "writes N bits LSB-first at the bit write position (+0x14), updates max (+0x18); called by every writer"),
    (0x821AFCE8, "bdBitBuffer::ctor", "high",
     "string 'bdBitBuffer::bdBitBuffer(const unsigned int,const bool)'; writes the 1-bit header (typeChecked)"),
    (0x821AFF20, "bdBitBuffer::writeRangedUInt32", "high",
     "string 'void __cdecl bdBitBuffer::writeRangedUInt32(const unsigned int,const unsigned int,const unsigned int,const bool)'"),
    (0x821B0068, "bdBitBuffer::writeDataType", "high",
     "if typeChecked (+0x21) tail-calls writeRangedUInt32(type,0,31,false); callers pass 1,2,3,5,7,8,11,12,13,15,19"),
    (0x821B0088, "bdBitBuffer::writeRangedInt32", "high",
     "string 'void __cdecl bdBitBuffer::writeRangedInt32(const int,const int,const int)'"),
    (0x821B01C8, "bdBitBuffer::writeRangedFloat32", "high",
     "string 'void __cdecl bdBitBuffer::writeRangedFloat32(const float,const float,const float,const float)'"),
    (0x821B1A78, "bdBitOperations::highBitNumber", "high",
     "string 'unsigned int __cdecl bdBitOperations::highBitNumber(unsigned int)'"),
    (0x821B1DD0, "bdMessage::getType", "high",
     "lbz r3,8(r3); used by bdSession::accept and MPPlayerManager::vfunc_1 to switch on the type"),
    (0x821B1E38, "bdMessage::ctor", "high",
     "stores vtable 0x8202F6B4, refcount 0, type byte at +8, typeChecked at +0x10; called after bdMemory allocate(0x18) with the message type in r4 by every game message sender"),
    (0x821B1E60, "bdMessage::ctor_payload", "medium",
     "same vtable and type byte, plus payload buffers sized in bits (slwi r5,r27,3); used by the receive path 0x821B9EB0 and type 0x26 senders"),
    (0x821B1FB0, "bdMessage::getPayload", "medium",
     "creates the 0x24-byte bdBitBuffer at msg+0xC with the msg+0x10 typeChecked flag if missing, returns a reference"),
    (0x821BF728, "bdSession::sendHost", "high",
     "string 'bool __cdecl bdSession::sendHost(class bdReference<class bdMessage>,const bool)'"),
    (0x821BFE30, "bdSession::sendAll", "medium",
     "loops over the session's connections (count +0x1C) calling vtable slot 2 (msg, reliable); only caller MPPeer::SendAll"),
    (0x821BFF08, "bdSession::getPeerIndex", "medium",
     "searches the connection list for the given connection, writes its index; used by bdSession::accept before accepting a game message"),
    (0x821C27C8, "bdSession::handleJoinRequest", "high",
     "string 'void __cdecl bdSession::handleJoinRequest(...)'; type 0x0A in accept"),
    (0x821C2948, "bdSession::handleJoinReply", "high",
     "string 'void __cdecl bdSession::handleJoinReply(...)'; type 0x0B in accept"),
    (0x821C3028, "bdSession::handleConsistencyMessage", "high",
     "string 'void __cdecl bdSession::handleConsistencyMessage(...)'; type 0x0C in accept"),
    (0x821C3510, "bdSession::accept", "high",
     "string 'bool __cdecl bdSession::accept(const class bdReceivedMessage &)'; dispatches types 0x0A/0x0B/0x0C, logs others except 0x23/0x4C"),
    (0x82312698, "XMemCpy", "medium",
     "memcpy(dst,src,n) returning dst: under 16 bytes ld/std, words, bytes; from 16 bytes dcbt plus VMX128 lvsl/vperm; forward copy (overlap smears, run in the oracle); 161 callers incl. NET_SendLoopPacket, ..."),
    (0x823D75C8, "MSGBB_ReadAngles2", "medium",
     "called by 0x823F78B0 at the angles position in both branches"),
    (0x823D7768, "MSGBB_WriteAngles2", "medium",
     "two angles as ANGLE2SHORT (x 182.044) in 16 bits each, writeDataType(5)"),
    (0x823D7FF8, "MSGBB_ReadOrigin", "medium",
     "3 x readRangedFloat32(-8191, 8191, 1), stops on the first failure (flag 0x82B793E6)"),
    (0x823D80C8, "MSGBB_WriteOrigin_copy", "medium",
     "makes the same three writeRangedFloat32(v, -8191, 8191, 1) calls as 0x823D8228 (a second copy; callers not traced)"),
    (0x823D8140, "MSGBB_ReadCoordDelta", "medium",
     "readRangedFloat32(-512, 512, 1) guarded by the global read-failed flag 0x82B793E6"),
    (0x823D81D0, "MSGBB_WriteCoordDelta", "medium",
     "writeRangedFloat32(new - base, -512, 512, 1)"),
    (0x823D8228, "MSGBB_WriteOrigin", "medium",
     "3 x writeRangedFloat32(v, -8191, 8191, 1); -8191 is the static float 0x82C9DD00 set by 0x8259BE90"),
    (0x823D8F08, "MPPeer::SendAll", "high",
     "string 'SEND ALL failed\\n'; bdSession(+0x5998)::sendAll(msg, reliable); 47 callers that each build one message type"),
    (0x823DCCA8, "MPPeer::SendToPlayer", "medium",
     "sends a message on the player slot's connection (+4) after bdSession::getPeerIndex finds it; vtable slot 2 (msg, reliable)"),
    (0x823DCD88, "MPPeer::SendToRemotePlayers", "medium",
     "loops the 24 player slots (0x310 bytes at this+0x1010), skips local (0x823D8FF0), unready (+0x46) and one excluded slot, calls SendToPlayer"),
    (0x823DCF10, "MPPeer::SendToAllRemote", "medium",
     "wrapper: SendToRemotePlayers(this, msg, no exclusion, reliable)"),
    (0x823E5978, "MPPeer::SendByProximity", "medium",
     "sends to remote slots whose 'near enemy' test (distance < 512, team check 0x82BA13F8) equals the argument, once per console (bit mask)"),
    (0x823E6770, "MPPlayer::WriteViewUpdate", "medium",
     "payload of type 0x24 (and appended to 0x4C): hasAngles bit, 2 angles, ranged 0..3, weapon byte"),
    (0x823EF2B0, "MSGBB_ReadVelocity", "medium",
     "called by 0x823F78B0 at the velocity position in both branches"),
    (0x823F20C0, "MSGBB_WriteVelocity", "medium",
     "bool hasXY, 8-bit yaw byte, speed as ranged float (-1024,1024,1), z as ranged float (-1024,1024,1)"),
    (0x823F2288, "MPPeer::WriteVehicleUpdate", "medium",
     "payload body of type 0x4C (origin, two velocities, angles, ranged floats/ints); 'vehicle' is a guess"),
    (0x823F2E20, "MPPlayer::WritePositionUpdate", "medium",
     "full or delta player state (origin, velocity, angles, weapon, flags) against the baseline at slot +0x100..+0x13C; called only by MPPeer::SendPlayerUpdates for type 0x23"),
    (0x823F78B0, "MPPlayer::ReadPositionUpdate", "medium",
     "mirror of 0x823F2E20 (3 bools, then readOrigin/readVelocity/readAngles or 8 bools and deltas); called by the 0x23 handler"),
    (0x823F9528, "MPPeer::HandlePositionUpdate", "medium",
     "handler for type 0x23 in the table; reads the player index byte, ignores local players, calls 0x823F78B0 then 0x823EF898"),
    (0x82403840, "MPPeer::SendLocalPlayerUpdates", "medium",
     "per frame (via 0x82404580); 1000/250/125 ms timers per local client; builds message types 0x23, 0x24, 0x4C"),
    (0x82405D50, "MPPlayerManager::ctor", "high",
     "stores MPPlayerManager::vftable/_4/_8; only caller MPPeer::ctor; fills the message handler table at this+0x8210+type*8"),
    (0x8247EFA8, "Netchan_Setup", "medium",
     "memset(chan,0,0xC30), +0 sock, +8 address (20 bytes), +0x1C qport, +0x20 incoming 0, +0x24 outgoing 1 (Q3 Netchan_Setup shape); no callers"),
    (0x8247F0D8, "MSG_ReadLong", "high",
     "reads 4 bytes BE at data+readcount, readcount += 4, returns -1 if readcount > cursize; same code inlined in CL_ParseServerMessage and CL_ParseCommandString; no callers (inlined everywhere)"),
    (0x8247F130, "MSG_ReadShort", "high",
     "2 bytes BE, readcount += 2, -1 past the end, else lhz (unsigned); no callers"),
    (0x8247F170, "MSG_ReadByte", "high",
     "1 byte, readcount += 1, returns 0xFF past the end (same inline pattern in CL_ParseServerMessage 0x8252905C); no callers"),
    (0x8247F1A0, "MSG_WriteLong", "high",
     "4 bytes BE at data+cursize, cursize += 4, no overflow check (same inline pattern in SV_SendClientSnapshot); no callers"),
    (0x8247F1E8, "MSG_WriteShort", "high",
     "low 16 bits BE at data+cursize, cursize += 2, no check; no callers"),
    (0x8247F218, "MSG_WriteData", "high",
     "memcpy (0x82312698) to data+cursize, cursize += len; no callers"),
    (0x8247F268, "MSG_WriteByte", "high",
     "data[cursize++] = r4, no check; no callers"),
    (0x8247F280, "MSG_WriteZeroByte", "medium",
     "data[cursize++] = 0; name is descriptive (no Q3 equivalent found); no callers"),
    (0x8247F2A0, "MSG_BeginReading", "medium",
     "readcount = 0 only; no callers"),
    (0x8247F2B0, "MSG_Init", "medium",
     "zeroes msg_t (5 words), data = r4, maxsize = 0xC00 fixed, memset(data,0,0xC00); no callers"),
    (0x82489560, "NET_OutOfBandPrint", "high",
     "writes four 0xFF bytes, vsprintf (0x8231A8A0), NET_SendPacket; only caller SV_PacketEvent with 'disconnect'"),
    (0x82522B10, "CL_FinishMove", "high",
     "cmd.serverTime = cl+0x610, weapon = cl+0x654 & 0xFF, angles = (int)(viewangles*182.044+0.5) & 0xFFFF, five floats from cl+0x670..0x680; same shape as campaign 0x82509A90"),
    (0x82522C38, "CL_CmdButtons", "high",
     "ORs 1<<i into cmd->buttons (+4) for each of 16 kbuttons (stride 0x18 from 0x82B53F68) active or wasPressed and clears wasPressed; extra bits 0x8000 and 0x200; same shape as campaign 0x82509BB8"),
    (0x82524000, "CL_SetUserCmdFloats", "medium",
     "stores f1..f5 into cl+0x670..0x680, which CL_FinishMove copies into usercmd +0x1C..+0x2C; no direct callers (syscall table); name descriptive"),
    (0x82524040, "CL_SetUserCmdValue", "medium",
     "sets cl+0x654 (copied into cmd.weapon by CL_FinishMove), cl+0x658 = 0, cl+0x660 = float; Q3 CL_SetUserCmdValue shape; no direct callers (syscall table)"),
    (0x82524940, "CL_Netchan_Transmit", "medium",
     "appends byte 3 (clc_EOF) to msg then b Netchan_Transmit; unreferenced"),
    (0x82525088, "CL_WritePacket", "high",
     "writes serverId, serverMessageSequence, serverCommandSequence, then (2, seq, MSG_WriteString) per unacked reliable command and 3 (clc_EOF), then Netchan_Transmit(&clc.netchan); cl_showSend '%i '; campaign 0x8250AE88"),
    (0x82525338, "CL_KeyMove", "medium",
     "second step of CL_CreateCmd; sits right after CL_WritePacket as campaign CL_KeyMove 0x8250B108 does; reads in_buttons (0x82B53E20) and edits cmd->buttons stance bits"),
    (0x82528258, "CL_ParseSnapshot", "high",
     "called by CL_ParseServerMessage for svc 7 (0x825290B8); reads 4-byte BE time + 1 byte into a 0x610-byte snapshot, copies the player state from (*0x82BCC7FC)[client+1]->+0x200 with 0x820B1630, stores into cl.snap ..."),
    (0x8252B578, "CL_GamepadMove", "medium",
     "third step of CL_CreateCmd; writes cmd forwardmove (+0x18) and rightmove (+0x19) as i8 clamped to -128..127 from the stick (campaign calls the matching function CL_MouseMove)"),
    (0x8252C7A8, "CL_CreateCmd", "high",
     "memset(cmd,0,0x30); cl_freeze check; calls CL_CmdButtons, 0x82525338, 0x8252B578, cl_viewPitchCompensate/cl_viewYawCompensate, 90-degree pitch clamp, then CL_FinishMove"),
    (0x8252C9E0, "CL_CreateNewCommands", "high",
     "increments cl.cmdNumber (clientActive+0x1298) and copies CL_CreateCmd's 0x30-byte result into cl.cmds[n & 63] (clientActive+0x698); called by CL_Frame 0x8252E4B0 and CL_SendCmd; same role as campaign 0x82510980"),
    (0x8252F930, "SV_FreeAcknowledgedReliableCommands", "medium",
     "walks back from (cl->reliableAcknowledge-1)&63, memsets each slot's buffer and clears its pointer (+0xC) and length (+0x10) until NULL or reliableAcknowledge&63; called first by SV_ExecuteClientMessage"),
    (0x8252FDB0, "SV_Netchan_Process", "medium",
     "addi r3,r3,0x750 (client netchan); b Netchan_Process; unreferenced"),
    (0x8252FDB8, "SV_Netchan_Transmit", "medium",
     "appends byte 8 to msg, r3 = client+0x750, b Netchan_Transmit; unreferenced"),
    (0x82530098, "SV_InitReliableCommands", "medium",
     "cl+4 = 0x6000, cl+8 = Z_Malloc(0x6000) pool, cl+0xC and cl+0x10 = Z_Malloc(0x100) (64 pointers, 64 lengths), cl+0x14 = pool; called by SV_DirectConnect with 0x40"),
    (0x82531370, "SV_PacketEvent", "high",
     "reads u16 qport at bytes 4-5, finds the svs.clients slot with that index, Netchan_Process(cl+0x750) then SV_ExecuteClientMessage; else NET_OutOfBandPrint 'disconnect'; campaign 0x82513B80"),
    (0x825321C8, "SV_ConnectLocalClient", "medium",
     "builds netadr {type 2 (loopback), 0...} and calls SV_DirectConnect; called from sub_824046D8 <- HandlePlayerSetup; name descriptive"),
    (0x82593518, "ClientThink", "medium",
     "copies client pers.cmd (+0x790) to oldcmd (+0x7C0), then SV_GetUsercmd(clientActiveIndex, &pers.cmd), then the think step; Q3 ClientThink shape"),
    # Demonware's bit buffer, beside the methods the generic passes name from
    # their assertion strings. Type numbers follow the tags each one writes
    # (writeBool 1, writeInt32 7, writeUInt32 8, writeFloat32 13 are named).
    (0x820D97D8, "bdBitBuffer::writeInt16", "medium", "writeDataType(5), then the value's two bytes swapped and writeBits(16)"),
    (0x820D9830, "bdBitBuffer::writeUInt16", "medium", "writeDataType(6), then the value's two bytes swapped and writeBits(16)"),
    (0x820D9958, "bdBitBuffer::writeUInt64", "medium", "writeDataType(10), then the 64-bit value's bytes swapped and writeBits(64)"),
    (0x820D9A48, "bdBitBuffer::readBool", "high", "readDataType(1), readBits(1) into a byte, *out = byte != 0"),
    (0x820D9AC8, "bdBitBuffer::readBoolValue", "medium", "readBool into a local and returns it (false when the read fails)"),
    (0x820D9D08, "bdBitBuffer::readInt32", "high", "readDataType(7), readBits(32), bytes swapped into *out"),
    (0x820D9DA0, "bdBitBuffer::readUInt32", "high", "readDataType(8), readBits(32), bytes swapped into *out; reads the range of a tagged readRangedUInt32"),
    (0x820D9EF0, "bdBitBuffer::readFloat32", "high", "readDataType(13), readBits(32), bytes swapped into *out; reads the range of a tagged readRangedFloat32"),
    (0x821AF1E0, "bdBitBuffer::readRangedUInt32", "high", "'bool __cdecl bdBitBuffer::readRangedUInt32(unsigned' and 'Range error. Expected: (%u,%u)'"),
    (0x821AF430, "bdBitBuffer::readRangedInt32", "high", "the signed twin of readRangedUInt32 (cmpw, readDataType(11), readInt32 for the range)"),
    (0x820D9B88, "bdBitBuffer::readInt8", "medium", "readDataType(2), readBits(8) straight into *out"),
    (0x820D9BF8, "bdBitBuffer::readInt16", "medium", "readDataType(5), readBits(16), bytes swapped into *out"),
    (0x820D9C80, "bdBitBuffer::readUInt16", "medium", "readDataType(6), readBits(16), bytes swapped into *out"),
    (0x820D9E38, "bdBitBuffer::readUInt64", "medium", "readDataType(10), readBits(64), bytes swapped into *out"),
    # The message fields on top of the bit buffer (Quake III's MSG_Write*/MSG_Read*
    # family on a bdBitBuffer; the helpers' MSGBB_ names). Each is checked
    # against its Rust port in cod3_game::net::fields.
    (0x823D7540, "MSGBB_WriteAngle", "medium", "one angle: fctiwz(angle * 182.044), writeDataType(5) and 16 bits (an inline writeInt16)"),
    (0x823D7698, "MSGBB_WriteAngles2_copy", "medium", "the same instructions as MSGBB_WriteAngles2"),
    (0x823D7838, "MSGBB_ReadVector", "medium", "three readFloat32s, each after a good one, status in 0x82B793E6"),
    (0x823D78F0, "MSGBB_WriteVector", "medium", "three writeFloat32s"),
    (0x823D7950, "MSGBB_ReadAngles3", "medium", "three readInt16s times 360/65536, status in 0x82B793E6"),
    (0x823D7A60, "MSGBB_WriteAngles3", "medium", "three angles of 16 bits, like MSGBB_WriteAngle"),
    (0x823D7B78, "MSGBB_WriteAngles3_copy", "medium", "the same instructions as MSGBB_WriteAngles3"),
    (0x823D7C90, "MSGBB_ReadDir", "medium", "readInt8, then the normal from the 162-entry table at 0x825FC698 (zero past it)"),
    (0x823D7D20, "MSGBB_WriteDir", "medium", "DirToByte, writeDataType(2) and 8 bits"),
    (0x823D7D88, "MSGBB_WriteDir_copy", "medium", "the same instructions as MSGBB_WriteDir"),
    (0x823D7DF0, "MSGBB_ReadOriginInt", "medium", "three readRangedInt32(-8191, 0x1FFF) as floats, status in 0x82B793E6"),
    (0x823D7F30, "MSGBB_WriteOriginInt", "medium", "three writeRangedInt32(fctiwz(c), -8191, 0x1FFF)"),
    (0x82545998, "DirToByte", "high", "Quake III's DirToByte: the largest dot product with the 162 normals at 0x825FC698, 0 for NULL"),
    (0x82550478, "vectoyaw_signed", "medium", "yaw in degrees from an arcsine series (no atan2), not wrapped; 0x82550680 adds 360 below 0"),
    (0x82550680, "vectoyaw", "medium", "the yaw of 0x82550478 wrapped into [0, 360)"),
    (0x823F7638, "MPPlayer::ReadViewUpdate", "medium", "reads message 0x24 (bool, MSGBB_ReadAngles2, readRangedInt32(0, 3), readInt8); called by its handler 0x823F9440"),
    (0x823F9440, "MPPeer::HandleViewUpdate", "medium", "the 0x24 entry of the MPPlayerManager's handler table; finds the sender's MPPlayer and calls 0x823F7638"),
    (0x823F6048, "MPPeer::ReadVehicleUpdate", "medium", "reads message 0x4C after its index: MSGBB_ReadOrigin, two MSGBB_ReadVelocity, MSGBB_ReadAngles3, four ranged floats, three ranged ints"),
    (0x823F8658, "MPPeer::HandleVehicleUpdate", "medium", "the 0x4C entry of the MPPlayerManager's handler table; reads the index, calls 0x823F6048, then the 'driven' bit and MPPlayer::ReadViewUpdate"),
    (0x820DA148, "AngleWrap180", "medium", "out = in outside [-180, 180] ? fmod(in + 180, 360.0) - 180 : in (float out, float in)"),
    # The cheat commands (god, noclip, notarget, ufo, give, setviewpos,
    # jumptonode) come from the game's command table at 0x825FE9D0 (generic
    # command pass), so they are not listed here.
]


# D-form instructions (addi, addis, ori, oris, the loads and stores): their
# 16-bit immediate or displacement changes between the two programs when it
# is part of a global's address; the opcode and registers don't.
D_FORM = {14, 15, 24, 25} | set(range(32, 56)) | {58, 62}
# Of those, the ones whose immediate can be the low half of an address (lis rX + this, rX).
ADDRESS_USERS = {14} | set(range(32, 56)) | {58, 62}
SKELETON_MIN = 6  # instructions; shorter functions look alike too easily


def skeleton(img, start: int, end: int) -> tuple:
    """A function's instructions with branch targets and D-form immediates
    left out: the same function in both programs has the same skeleton."""
    out = []
    for a in range(start, end, 4):
        w = img.u32(a)
        op = w >> 26
        if op == 18:
            w &= 0xFC000003
        elif op == 16:
            w &= 0xFFFF0003
        elif op in D_FORM:
            w &= 0xFFFF0000
        out.append(w)
    return tuple(out)


def addressed_globals(img, start: int, end: int) -> dict:
    """{instruction index: address} for `lis rX, hi` followed by a D-form use of rX."""
    high, out = {}, {}
    for i, a in enumerate(range(start, end, 4)):
        w = img.u32(a)
        op, rt, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            high[rt] = imm << 16
        elif op in ADDRESS_USERS and ra in high:
            out[i] = (high[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
    return out


def pass_same_as_campaign(prog, syms, campaign_pe: Path, campaign_symbols: Path, report) -> None:
    """Names from default.xex for functions with the same skeleton, unique in both programs."""
    cimg = nf.Image(campaign_pe)
    cfuncs = nf.find_functions(cimg, {})
    with open(campaign_symbols, "rb") as f:
        table = tomllib.load(f)
    cnames = {e["address"]: e for e in table.get("function", [])}
    cglobals = {e["address"]: e for e in table.get("global", [])}

    def by_skeleton(img, funcs):
        out = collections.defaultdict(list)
        for start, end in funcs:
            if (end - start) // 4 >= SKELETON_MIN:
                out[skeleton(img, start, end)].append((start, end))
        return out

    ours, theirs = by_skeleton(prog.img, prog.funcs), by_skeleton(cimg, cfuncs)
    pairs = [(t[0], o[0]) for k, t in theirs.items() if len(t) == 1 and len(o := ours.get(k, ())) == 1]
    named = 0
    votes = collections.defaultdict(collections.Counter)
    for (cstart, cend), (start, end) in pairs:
        entry = cnames.get(cstart)
        # C++ methods are left to this program's own RTTI pass: their names
        # carry vtable slot numbers, which differ between the two programs.
        if entry and not entry["name"].startswith("__") and "::" not in entry["name"]:
            confidence = "high" if entry.get("confidence") in ("known", "high") else "medium"
            if syms.add(start, entry["name"], confidence,
                        f"same instructions as default.xex 0x{cstart:08X} ({entry['name']}), "
                        "the only such function in either program", "campaign"):
                named += 1
        theirs_at, ours_at = addressed_globals(cimg, cstart, cend), addressed_globals(prog.img, start, end)
        for i, address in theirs_at.items():
            if i in ours_at:
                votes[address][ours_at[i]] += 1
    globals_named = 0
    for address, entry in cglobals.items():
        seen = votes.get(address)
        if seen and len(seen) == 1:
            ours_address, count = seen.most_common(1)[0]
            confidence = "high" if count >= 2 and entry.get("confidence") in ("known", "high") else "medium"
            if syms.add(ours_address, entry["name"], confidence,
                        f"default.xex's {entry['name']} (0x{address:08X}), addressed by the same instruction "
                        f"of {count} matching function(s)", "campaign", kind="global"):
                globals_named += 1
    report["campaign"] = (len(pairs), named, globals_named)


def write_names_toml(path: Path, syms, avoid: set) -> tuple[int, int]:
    """The codegen's [names] table, with tools/gen_codegen_names.py's rules."""
    chosen = []
    for s in syms.functions():
        if s.confidence not in ("known", "high"):
            continue
        name = s.cname
        if name.startswith("__") or s.source in ("imports", "helpers") or not nf.IDENT.match(name):
            continue
        if name in avoid:
            name += "_guest"
        chosen.append((s.address, name))
    counts = collections.Counter(n for _, n in chosen)
    dupes = {n for n, c in counts.items() if c > 1}
    chosen = sorted((a, n) for a, n in chosen if n not in dupes)
    lines = [
        "# Generated by tools/name_mp_functions.py: the codegen's readable names for",
        "# codmp_xenonf.xex (SDK patch 0011). Edit the tool, not this file.",
        f"# {len(chosen)} functions; {len(dupes)} names given to more than one address left out.",
        "",
        "[names]",
    ] + [f'0x{a:08X} = "{n}"' for a, n in chosen]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(chosen), len(dupes)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pe", nargs="?", default=str(HERE.parent / "xex" / "codmp.pe"))
    ap.add_argument("--imports", help="xex_imports.py listing (default: the image with .imports)")
    ap.add_argument("--config", help="codegen config whose [functions] table adds function starts")
    ap.add_argument("--q3-src", help="Quake III Arena source checkout for the string fingerprints")
    ap.add_argument("--campaign-pe", help="default.xex's image (default: default.pe next to the image)")
    ap.add_argument("--campaign-symbols", help="default.xex's symbol map (default: recomp/config/symbols.toml)")
    ap.add_argument("-o", "--out", help="symbol map to write (default: <image folder>/symbols_mp.toml)")
    ap.add_argument("--ppcx-names", help="also write 'ADDRESS NAME' lines for ppcx.py")
    ap.add_argument("--names-toml", help="also write the codegen [names] table")
    ap.add_argument("--avoid", type=Path, action="append", default=[],
                    help="names to avoid in --names-toml (one per line; may repeat)")
    a = ap.parse_args(argv)

    pe = Path(a.pe)
    if not pe.is_file():
        sys.exit(f"no image at {pe}")
    imports_path = Path(a.imports) if a.imports else pe.with_suffix(".imports")
    out = Path(a.out) if a.out else pe.parent / "symbols_mp.toml"

    img = nf.Image(pe)
    extra = nf.read_extra_functions(a.config)
    prog = nf.Program(img, extra)
    print(f"{len(prog.funcs)} functions ({len(img.pdata())} from .pdata, {len(extra)} from the config)")

    syms = nf.SymbolTable()
    warnings, report = [], {}
    for addr, name, conf, ev in KNOWN_MP_FUNCTIONS:
        if addr not in prog.start_set:
            warnings.append(f"{name} 0x{addr:08X} is not a function start here")
        syms.add(addr, name, conf, ev, "known" if conf == "high" else "curated")
    imports = nf.read_imports(imports_path)
    if not imports:
        warnings.append(f"no imports listing at {imports_path}: import thunks left unnamed")
    thunks = nf.pass_imports(prog, syms, imports)
    report["helpers"] = nf.pass_save_restore_helpers(prog, syms)
    campaign_pe = Path(a.campaign_pe) if a.campaign_pe else pe.parent / "default.pe"
    campaign_symbols = (Path(a.campaign_symbols) if a.campaign_symbols else
                        HERE.parent / "COD_3_WAW_Setup-tool" / "recomp" / "config" / "symbols.toml")
    if campaign_pe.is_file() and campaign_symbols.is_file():
        pass_same_as_campaign(prog, syms, campaign_pe, campaign_symbols, report)
        pairs, named, globals_named = report["campaign"]
        print(f"same as the campaign: {pairs} functions match one to one; {named} named, "
              f"{globals_named} globals")
    else:
        warnings.append(f"no campaign image or symbol map ({campaign_pe}, {campaign_symbols}): "
                        "names shared with default.xex left out")
    q3 = nf.read_q3_source(a.q3_src) if a.q3_src else None
    nf.pass_self_naming(prog, syms, report, nf.q3_string_owners(q3) if q3 else None)
    if q3:
        nf.pass_q3_strings(prog, syms, q3, report)
    nf.pass_rtti(prog, syms, report)
    nf.pass_commands(prog, syms, report)
    nf.pass_asset_types(prog, syms, report)
    nf.pass_xapi(prog, syms, thunks, report)
    nf.pass_dvar_globals(prog, syms, report)
    nf.pass_trivial(prog, syms, thunks, report)

    funcs, globs = syms.functions(), syms.globals()
    by_conf = collections.Counter(s.confidence for s in funcs)
    by_src = collections.Counter(s.source for s in funcs)
    header = [
        "Call of Duty 3 (Xbox 360) codmp_xenonf.xex symbol map (multiplayer program).",
        "Generated by tools/name_mp_functions.py; edit the tool, not this file.",
        f"Image: {pe.name}, {len(prog.funcs)} functions, of which {len(funcs)} named here; "
        f"{len(globs)} globals.",
        "confidence: known = import thunks and addresses checked by hand (KNOWN_MP_FUNCTIONS);",
        "  high = direct evidence (RTTI, command registration, a string naming the function);",
        "  medium = one weaker clue (see evidence).",
        "",
        "functions by confidence: " + ", ".join(f"{k} {by_conf[k]}" for k in ("known", "high", "medium")),
        "functions by evidence: " + ", ".join(f"{k} {v}" for k, v in by_src.most_common()),
    ]
    nf.write_toml(out, syms, header)
    print(f"wrote {out}: {len(funcs)} functions, {len(globs)} globals")
    print("  by confidence: " + ", ".join(f"{k} {by_conf[k]}" for k in ("known", "high", "medium")))
    print("  by evidence:   " + ", ".join(f"{k} {v}" for k, v in by_src.most_common()))
    if a.ppcx_names:
        nf.write_ppcx_names(a.ppcx_names, syms)
        print(f"wrote {a.ppcx_names}")
    if a.names_toml:
        from gen_codegen_names import CPP_KEYWORDS  # the same keyword list as for default.xex
        avoid = set(CPP_KEYWORDS)
        for p in a.avoid:
            avoid.update(n.strip() for n in p.read_text(encoding="utf-8").split() if n.strip())
        n, d = write_names_toml(Path(a.names_toml), syms, avoid)
        print(f"wrote {a.names_toml}: {n} names ({d} duplicates left out)")
    for w in warnings:
        print("warning:", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
