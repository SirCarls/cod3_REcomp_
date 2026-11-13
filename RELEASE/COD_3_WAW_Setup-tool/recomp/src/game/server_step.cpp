// cod3 - [acct2-main] 2026-10-26 The campaign's game at a fixed rate (cod3_server_hz, 60 a second),
// whatever the frame rate, with the moving things drawn smoothly between its updates.
//
// Owner 2026-10-25 23:30: "also collision needs to be that of 60fps."; asked which collision:
// "Player vs world, All game collision"; 23:50 "yes please keep going" (smoothing first). 23:52: "mp
// should be able to have 120hz network servers" (the rate is a setting, up to 240; multiplayer is
// its own program, see dev/agents/BOARD.md).
//
// The campaign runs its whole game frame once per drawn frame: Com_Frame -> SV_Frame (0x82518E40,
// r3 = the frame's milliseconds) -> 0x825173F8 -> G_RunFrame (0x8256D3E0). Everything the game
// moves there - the AI, grenades and rockets, doors and other movers, vehicles, physics objects -
// moves and collides in steps of the frame's time, so above 60 fps they behave differently (the
// physics library alone already has fixed steps: physics_step.cpp). The player's own input is not
// in it: Com_Frame -> 0x82511B28 -> SV_ClientThink (0x82512338) -> ClientThink, once per usercmd,
// and the player's movement has its own fixed steps (movement/hooks_sp.cpp, cod3_move_hz).
//
// Here SV_Frame runs in whole updates of 1/cod3_server_hz seconds that end at fixed times of the
// game's clock (16, 17, 17 ms at 60, as the game's own milliseconds at 60 fps): none, one or
// several in a drawn frame. Time is never lost or added (the game's speed is unchanged); a frame
// longer than 6 updates (a level load, a long hitch) runs as one update of all its time, as the
// game did.
//
// Drawing between updates. The campaign's client game works on the server's own entities (the
// player's is compared by pointer in CG_CalcEntityLerpPositions 0x8243CD70): for each one it sets
// the drawn origin (entity +112) from its trajectory (+20): stationary -> its base, a trajectory
// (grenades, rockets) -> evaluated at the client's time, which needs nothing more, and
// TR_INTERPOLATE (1: actors, vehicles, physics objects; 0x82438C90) -> a plain copy of the
// server's current origin (+336). Those would move in steps above the rate, so after the game's
// own code the drawn origin of a TR_INTERPOLATE entity is put between its origins at the last two
// updates, by how far the game's clock is into the next update (one update behind at most, as
// the player's camera). The server's values are put back before the next server frame. [claude-9b]
// The drawn angles (+128, 0x82438B68) the same way when the game leaves them as the update set them
// (vehicles turning, soldiers), and in a vehicle the view's angles (cod3_server_smooth_view). 0 turns
// it all off: one update a frame with the frame's time, as the game does.
// [claude-9b-think] 2026-10-27 UNTESTED Not every entity is drawn from +112 / +128. CG_AddCEntity (its switch) picks a
// draw by the entity's type (byte +0) and each fills the entity's scene node (+548, made by 0x824721C8; the refEntity
// at node +8: axis node +28 / +40 / +52, origin node +68) that the renderer draws (0x8242E450; a vehicle's model by
// 0x8242CB68, its bones relative to that matrix). Types 0, 2, 3, 4, 5, 10, 11, 13 (and 7 when flagged) take +112 / +128;
// type 14 (a vehicle: 0x82555668 sets it with the vehicle's data at +540) and 15 take the server's +336 / +352
// (0x82442338), and type 1 (players and soldiers, 0x8244B610) its +336 with its own +352, or the +352 of the vehicle it
// rides (+16 bit 0x00100000, the vehicle's handle at +432). So vehicles and the people on them were drawn at the last
// update's pose in every frame whatever the smoothing did: cod3_server_smooth_body (PlaceVehicleBody and
// PlaceRiderBody below).

#include "game/server_step.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>  // [acct2-cod3-motion] std::memcpy (cod3_debug_drawn_trace_diff)
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // [acct2-cod3-motion] cod3_debug_drawn_trace_diff: which guest pages can be read
#include <rex/system/xmemory.h>

#include "game/guest.h"
#include "game/movement/step_view.h"  // [claude-9b] the aim and lean drawn between steps
#include "game/ride_bones.h"  // [claude-9b-think] cod3_blend_ride_bones: the ridden vehicle's turret and gun

// [claude-9b] 2026-10-26 23:20: off by default again (owner 23:16: "People and various other entities
// are flickering"). Above 60 fps, frames without an update drew fewer things (bench serverhz1: 3228
// -> 2958 draws a frame in blkbrn, 1086 -> 940 in saint_lo), the smoothing never ran (its log: "0
// things drawn between updates") and 6 of its runs ended in a crash. Back on once those are fixed.
// [claude-9b] 2026-10-27 02:20: on again at 60 (owner 00:14: "start on the 60hz. This is priority
// one"). The crash was the local client's packet queue overflowing (SV_SendClientMessages and the
// NET_SendLoopPacket hook), the missing things the animated models' draw flag (cod3_server_step_draw);
// the smoothing runs. Runs c9b-draw-on/-long (blkbrn 50 s and 131 s) and c9b-draw-stlo (saint_lo):
// no errors or crashes, as many models drawn in frames with and without an update.
REXCVAR_DEFINE_INT32(cod3_server_hz, 60, "cod3/Game",
                     "Game updates a second in the campaign, whatever the frame rate: the AI, "
                     "grenades, doors, vehicles and everything else that moves and collides behaves "
                     "as at that rate (60 = the game at 60 fps), drawn smoothly between updates at "
                     "higher frame rates. 0 = one update a frame with the frame's time, as the game "
                     "does")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_server_smooth, true, "cod3/Game",
                    "Draw moving things (soldiers, vehicles, physics objects) between the game's "
                    "updates above cod3_server_hz frames a second. Off: they move in steps of the "
                    "update rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b] 2026-10-27 The view in a vehicle (owner 10-27 02:34: "First person within vehicles has a LOT
// of judder/jerky movement. The mission where you start in a tank's turret then get out has a mission
// you can use to test. Also cars and bikes seem to be effected as well"). Run c9b-veh-cam1 (blkbrn, the
// tank's turret, ~300 fps): turning the turret, the camera turned 0.70 deg and moved 3.9 units in each
// frame with a game update and not at all in the 3-4 frames between (driving straight was smooth
// already: 0.9 a frame in both, PlayerViewOffset). In a vehicle 0x824505E8 places the camera by the
// player's view angles (client +172; in the turret also its place around the turret, 0x82446598), and
// the vehicle sets those in the game's update. Now each update's view angles are kept, and a frame in
// a vehicle draws the view between the last two, as the vehicle itself is drawn (one update behind at
// most), at any frame rate. Only while the angles change in the updates alone: a view the player turns
// in every frame (walking, a free look) is left as it is.
// [claude-9b] 2026-10-27 see CG_CalcEntityLerpPositions below.
REXCVAR_DEFINE_BOOL(cod3_server_smooth_stationary, true, "cod3/Game",
                    "With cod3_server_smooth, also draw between the game's updates the parts the game moves by "
                    "their base position each update (a tank's turret and hatches, things linked to a vehicle), "
                    "so they stay on what carries them; off: they move in steps of the update rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_server_smooth_view, true, "cod3/Game",
                    "With cod3_server_hz and cod3_server_smooth, in a vehicle (a tank's turret, a car, a "
                    "bike) the view turns smoothly between the game's updates, as the vehicle is drawn; "
                    "off: it turns in steps of the update rate (judder above it)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b-think] 2026-10-28 UNTESTED (owner 06:43: "Camera rotation while on turrets (MG42, or the manning the turret
// mission) is locked to 60hz"). The view smoothing above ran only in a vehicle (RidesVehicle): a tank's turret is a seat,
// an MG42 or a flak gun is not. On a mounted gun the player's view angles (client +172) are set in the game's updates
// alone, as in a vehicle (the mouse turns the client's angles every frame; the camera turned once an update, so the update
// turns the view). Now the same smoothing also runs while the playerState says the player works a mounted gun (ps +208
// & 0x6000, as fov.cpp's vehicle_view reads it; movement/NOTES.md: turret 0x2000 / 0x4000). Like the tank's turret it
// is drawn between the last two updates (one update behind at most); a view that turns between updates is left as it
// is (ViewAnglesToDraw's own test), so this changes nothing where the game already turns it every frame.
REXCVAR_DEFINE_BOOL(cod3_server_smooth_turret, true, "cod3/Game",
                    "With cod3_server_smooth_view, on a mounted gun (an MG42, a flak gun, a crane) the view turns "
                    "smoothly between the game's updates too; off: it turns in steps of the update rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b-think] 2026-10-27 UNTESTED The first-person view on a vehicle the player drives or rides (owner 09:56:
// nightdrop's jeep, the driver's seat, ~200 fps: the world and the jeep's inside judder). 0x824505E8 places the camera
// at the seat as the last update left it, so the camera jumped once an update while the jeep is drawn between its last
// two updates (one update behind at most, CG_CalcEntityLerpPositions' hook below). SmoothView (movement/hooks_sp.cpp)
// tried the movement steps' offset first: in a vehicle Pmove does not move the player, so it was a zero offset that
// still counted, and the update offset (PlayerViewOffset) was never reached. Now on a ridden vehicle the camera is put
// on the vehicle as it is drawn: the vehicle's own server origin and angles at the last two updates (kept after each
// update, as AdvanceView does), the camera moved and turned with it by Back(), the ridden vehicle drawn from the same
// pose; a view the player turns (free look) is turned back by the part of the update's turn not drawn yet. The tank's
// turret camera (mode 4) and third person are left as before.
// [claude-9b] 2026-10-27 11:55 it was off by default until it reached the picture; 14:55 on again: with
// cod3_server_smooth_body 1 (the vehicle's scene node on the same pose) claude-9b-test's frame captures of the hostage
// ride show the interior still (0.6-1.0 px a frame median, no 60 Hz pattern; body 0: +90/-45/-45 px).
REXCVAR_DEFINE_INT32(cod3_server_smooth_ride, 1, "cod3/Game",
                     "With cod3_server_hz and cod3_server_smooth, in first person on a vehicle the player drives or "
                     "rides (a jeep, a car, a bike) the camera stays on the vehicle as it is drawn between the game's "
                     "updates, and a view the player turns (free look) turns with it. 0: off (the camera moves in "
                     "steps of the update rate); 1: on, the free look turned by the view's turn in the update")
    .range(0, 1)  // [claude-9b-think] 2 (the vehicle's yaw alone) dropped: ride4 found it uneven
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b-think] 2026-10-27 UNTESTED (claude-9b-test: the hostage level's ride runs in camera mode 8, where the
// ride did nothing). The vehicle cameras of 0x824505E8 (mode from 0x82447D48) are built from the ridden vehicle's
// server pose, its origin (+336) and angles (+352), as the game left them in the update: 2 / 3 the driver's own and
// third-person views (0x8244FC00 / 0x82450088, the angles +352), 6 / 7 / 8 / 13 a seat's tag (0x824457F0: the tag's
// world matrix by 0x8254EC98 -> 0x8254D2A8 = AnglesToAxis(+352) at +336 with the update's bones; 8 a passenger's seat,
// 6 the seat after the driver's), 12 the seat the update put the player in (ps origin). The ride moves and turns each
// of these with the vehicle as it is drawn. Bit n: camera mode n. The default leaves out the turrets (4, 5: the view
// angles there are drawn between updates already, cod3_server_smooth_view) and third person off a vehicle (1).
REXCVAR_DEFINE_INT32(cod3_server_smooth_ride_cams, 12749,  // modes 0, 2, 3, 6, 7, 8, 12, 13
                     "cod3/Game",
                     "With cod3_server_smooth_ride, the camera modes of the game's view setup it applies to, one bit "
                     "each (bit n: mode n; 0 the eye, 2/3 a driver's views, 6/7/8/13 a seat's camera, 12 linked to a "
                     "vehicle; 4/5 the turrets are left out)")
    .range(0, (1 << 23) - 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b-think] 2026-10-27 UNTESTED ride6 (claude-9b-test's pictures of the hostage ride: the jeep's inside jumped
// once an update with the ride on as with it off). The camera is placed on the ride's pose (RideViewOffset); the
// vehicle was drawn from that pose only where the game draws it at its update position (a plain copy of +336 / +32),
// and otherwise by whatever drew it (the game's own placing, or the other smoothing). On: the ridden vehicle is drawn
// from the camera's pose whatever its trajectory type and whatever the game drew, so the two agree by construction.
REXCVAR_DEFINE_BOOL(cod3_server_smooth_ride_vehicle, true, "cod3/Game",
                    "With cod3_server_smooth_ride, the vehicle the player rides is drawn from the same pose the "
                    "camera is placed on, whatever way the game itself draws it, so its inside stays still on the "
                    "screen; off: only where the game draws it at its update position")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-ocr] 2026-11-07 (owner 13:18: "The camera or NPCs can be jittering depending on the vehicle. It's related to
// our fps changes. saint lo intro car ride has it. really bad."). Run acct2-ocr-jit1 (saint_lo from the start, ~280
// fps): the intro ride's camera is mode 15 of 0x82447D48 the whole way, and the player rides no vehicle there
// (RiddenVehicle 0), so none of the view smoothing above applies. Each second the camera turned 0.4-2.0 degrees and
// moved up to 0.8 units in each of the 60 frames with a game update and not at all in the 200-230 frames between,
// while the jeep and its riders are drawn between updates (cod3_server_smooth_body): a 60 Hz judder against them.
// The modes set here: after 0x824505E8, a camera the game left exactly as the last update set it is drawn between
// its places at the last two updates, by Back(), as the moving things are (one update behind at most). A camera that
// changed since the update (the player turns it, or the game moves it every frame) is left as it is, and so is a cut.
// [acct2-cod3-motion] 2026-11-07 Run acct2-cod3-motion-hostage-250-v: mode 21 (0x82447D48 returns it while the
// playerState's pm_type, cg +0x24 -> +0x34, is above 3: the player killed) moved the camera in update frames alone (137
// moves, 128 of them with an update; 329 frames it stood still while the game's camera moved): the death camera stepped
// at 60 Hz until the level restarted. Mode 21 is in the default now (the same drawing as mode 15).
REXCVAR_DEFINE_INT32(cod3_server_smooth_cams, (1 << 15) | (1 << 21), "cod3/Game",
                     "With cod3_server_hz and cod3_server_smooth, the camera modes of the game's view setup whose "
                     "camera the game sets in its updates alone (a scripted ride, such as Saint-Lo's opening drive; "
                     "the camera after the player is killed) drawn between the updates, one bit each (bit n: mode n; "
                     "default modes 15 and 21); 0: off (the camera moves in steps of the update rate there)")
    .range(0, (1 << 23) - 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-ocr] 2026-11-07 (owner 13:18, the vehicle jitter; 13:55: "Keep going with bug checks"). Run
// acct2-ocr-veh-crssrds (crssrds from the start, uncapped, ~170 fps): about 45 s in, for 5 s in camera mode 12 of
// 0x82447D48 (picked when the playerState's +36 is 1: the player linked to something, it seems) with no ridden vehicle,
// the camera moved 1.5-1.9 units a frame in the 60 frames a second with a game update and 0.02-0.06 in the 100-150
// between, while it turned in every frame (the player's free look). That camera is at the playerState's origin, which
// moved in the updates alone, and nothing drew it between them (SmoothView, movement/hooks_sp.cpp, and the vehicle
// smoothing above need a ridden vehicle or a player moved by the movement steps). The modes set here: after SmoothView, if
// it moved nothing and the player rides no vehicle, the camera's place is drawn between its places at the last two
// updates, by Back() (one update behind at most, as the moving things); its angles are left as they are. Not a
// camera cod3_server_smooth_cams drew this frame, and not a cut.
REXCVAR_DEFINE_INT32(cod3_server_smooth_linked, 1 << 12, "cod3/Game",
                     "With cod3_server_hz and cod3_server_smooth, the camera modes of the game's view setup in which "
                     "the player is carried by something the game moves in its updates (linked to it, not riding a "
                     "vehicle, as about 45 s into Crossroads) whose camera place is drawn between the updates, one bit "
                     "each (bit n: mode n; default mode 12); 0: off (the camera moves in steps of the update rate "
                     "there)")
    .range(0, (1 << 23) - 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] 2026-11-07 (owner 20:31, St-Bert's intro at 250 fps: "still doesn't look smooth. The npc's jolt
// around"). Run acct2-cod3-motion-stbert-trace1 (cod3_debug_view_trace, camera mode 12 the whole drive): the playerState,
// the player's entity and the game's camera move only in frames with an update (4.6-5.0 units each), and
// cod3_server_smooth_linked drew the camera between them evenly (its move a frame follows the frame's milliseconds) --
// except in the frames where SmoothView moved the camera itself (1 to 7 a second, its update offset): those cleared the
// linked camera's history, so the next frame started a new camera at the game's place, a whole update ahead (one jump
// of ~5 units), and it then stood still until the next update (two or three frames). The whole picture jumped forward
// and stopped several times a second while the riders and the trucks around moved evenly: the "jolting". On: the
// history is kept from the game's own camera place (as 0x824505E8 left it) also in the frames SmoothView draws, so
// nothing starts over; those frames are still drawn as SmoothView placed them (the same update offset).
REXCVAR_DEFINE_BOOL(cod3_server_smooth_linked_keep, true, "cod3/Game",
                    "With cod3_server_smooth_linked, its camera keeps its place at the last two updates through frames "
                    "the movement's view smoothing draws, so it never starts over in the middle of a ride (off: it "
                    "starts over after each such frame: a jump of one update, then a stop)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] 2026-11-07 Runs acct2-cod3-motion-stbert-250/1000-after (cod3_debug_server_step): with the place
// drawn evenly, St-Bert's truck ride still turned 0.26-0.38 degrees a frame in the 60 frames a second with an update and
// not at all in the 100-180 between, for 4 s while the truck takes a bend (the game turns the linked player's view in
// its updates; cod3_server_smooth_linked drew the place alone, and the vehicle view smoothing needs a ridden vehicle).
// On: in those modes the camera's angles are drawn between their values at the last two updates too, by Back(), when
// they changed in the updates alone (a view the player turns between updates is left as it is), and its axis turned
// with them; not a cut (90 degrees or more in an update).
REXCVAR_DEFINE_BOOL(cod3_server_smooth_linked_turn, true, "cod3/Game",
                    "With cod3_server_smooth_linked, its camera also turns smoothly between the game's updates when "
                    "the game turns it (a ride taking a bend); off: it turns in steps of the update rate")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b-think] 2026-10-27 UNTESTED ride9 (claude-9b-test's ride8, the hostage ride at 180 fps: the trace passed,
// the camera on the ride's pose, but the frame captures showed the jeep's inside +90 / -45 / -45 px each 60 Hz cycle,
// ride 0 as 1). The renderer draws a vehicle where its scene node says, and the vehicles' draw fills that from the
// server's origin and angles (+336 / +352, see the note at the top), never from the drawn +112 / +128 that the ride and
// the other smoothing set: the body stayed at the update's pose while the camera moved between updates. Now the node of
// a vehicle (and of anyone riding one) is moved after the game filled it, by the vehicle's pose between its last two
// updates (the ride's pose for the ridden one with cod3_server_smooth_ride, the same pose the camera is on). The node is
// the game's per-entity draw record, filled again by the next frame's draw; nothing of the game's own state is changed.
REXCVAR_DEFINE_INT32(cod3_server_smooth_body, 1, "cod3/Game",
                     "With cod3_server_hz and cod3_server_smooth, where vehicles and the people the game draws at "
                     "their update position are drawn. 0: at the last game update's pose, as the game does (they move "
                     "in steps of the update rate); 1: vehicles and the people riding them between the vehicle's last "
                     "two updates, the ridden one on the camera's pose; 2: also soldiers and other players between "
                     "their own last two updates")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] 2026-11-07 (owner 18:59: 500 fps uncapped; 10-26: "I wanna make sure these fixes work on ANY
// framerate"). The game's clock counts whole milliseconds (Com_Frame's msec from its millisecond clock), while the
// frame limiter (frame_rate.cpp) paces frames in real time: at 144 fps (6.94 ms) the frames get 7, 7, 7, ... 6 ms, at
// 240 fps 4, 4, 4, 4, 4, 5, at 300 fps 3, 3, 4, and uncapped anything. Everything drawn between updates (Back()) and
// the camera between movement steps moved by those whole milliseconds, up to 20-50 % more or less in one frame than
// in the next while the frames reach the screen evenly: a fine judder at many rates (none at 60, 125, 250, 500, 1000).
// On: the drawing uses the game's clock plus the part of a millisecond it hasn't counted yet (the real time since the
// last frames, less their milliseconds: always within one millisecond), so things move by the real time between
// frames. Only how frames between updates are drawn; the game's clock, its updates and the usercmds are unchanged.
REXCVAR_DEFINE_BOOL(cod3_server_draw_subms, true, "cod3/Game",
                    "Draw moving things and the camera between the game's updates by the real time to a fraction of a "
                    "millisecond (the game's clock counts whole milliseconds, which made them move unevenly at frame "
                    "rates that aren't a whole number of milliseconds a frame); off: by whole milliseconds")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b] 2026-10-27
REXCVAR_DEFINE_BOOL(cod3_server_client_time, true, "cod3/Game",
                    "Above cod3_server_hz frames a second, the client game's clock runs on between the "
                    "game's updates (effects, the view and the weapon move every frame); off: it stands "
                    "still until the next update, as before (things flickered)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b] 2026-10-27 The flicker above the update rate (owner 10-26 23:16 "People and various
// other entities are flickering", 10-27 00:57 "entities are flickering"; 01:35: "the problem seems
// related to the glitched corpse animation fix. I think something similar is being applied here?").
// It is the same kind of thing: a part of the game's update that a drawn frame needs. The renderer
// draws an animated model (a scene node's model, node +92) only while bit 0 of the model's +196 is
// set (0x8242E450, before 0x8242D080 / 0x8242CB68), and each draw clears it (0x8242D080, 0x8242CB68,
// 0x8242D868, 0x8242AED8, 0x82427D18). The last call of G_RunFrame, 0x8253F438, sets it on the model
// (entity +508) of every entity in the animated list (0x82A50E24: soldiers, vehicles, anything with
// bones). So a model was drawn once per game update: a drawn frame without one left out every
// soldier and vehicle (the three tanks of blkbrn in about half of the screenshots at ~140 fps and 1
// of 15 at 60; the client game, the renderer's view test and the bones were the same in both kinds
// of frame: runs c9b-flick-*, c9b-cull, c9b-bones). Now such a frame runs that call alone, as the
// corpse fix runs the ragdolls' pose part alone between physics steps: the models are drawn in the
// last update's pose (their bones do not change between updates: the animation runs in G_RunFrame).
REXCVAR_DEFINE_BOOL(cod3_server_step_draw, true, "cod3/Game",
                    "With cod3_server_hz, frames without a game update still draw the soldiers, "
                    "vehicles and other animated models, in the last update's pose; off: those frames "
                    "leave them out (they flicker above the update rate)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [rexglue-native npc] 2026-11-09 (owner 15:37: "NPC animations and skeletons can get malformed at low fps. This can't
// be FPS based"; 15:38: Mace, the soldier against the tank at the start, around 38 fps, emulated and native rendering).
// [rexglue-native npc] Bit 0 of an animated model's +196 also tells the next game update whether the model was drawn
// [rexglue-native npc] since the last one: G_RunFrame's tail sets it (0x8253F438) and a draw clears it. The update
// [rexglue-native npc] reads it for the animation's detail (0x82421AA8: still set and entity +636 bit 1 clear ->
// [rexglue-native npc] 0x820C2568 with class 5, the coarsest, for models nobody saw) and in the game code that uses
// [rexglue-native npc] the skeleton only for models on screen (Actor_UpdateEyeInformation, 0x823D5308, 0x823E54C8,
// [rexglue-native npc] 0x82410B20, 0x82550068, and the "drawn?" answers 0x82463D88 and 0x8249DED0). On the console
// [rexglue-native npc] every update came after a draw. Below cod3_server_hz frames a second a frame runs two or more
// [rexglue-native npc] updates back to back (GameUpdates), so each update after a frame's first saw every model as
// [rexglue-native npc] not drawn: the coarsest animation detail (the frame then draws that update's skeleton, its
// [rexglue-native npc] coarse bones next to older ones: "malformed") and the off-screen paths above. With this on,
// [rexglue-native npc] those updates see the models the last frame drew as drawn again, as the console's would;
// [rexglue-native npc] models it didn't draw keep the bit. A frame with one update or none is unchanged.
// [acct2-bro] 2026-11-12 rn60c: on by default. The coarsest class is also the soldiers' simpler model (three
// [acct2-bro] surfaces for the detailed one's six), so with this off every soldier in view changed model for one
// [acct2-bro] frame whenever a frame ran two updates (any hitch): the owner's "a brief frame where a soldier's pose
// [acct2-bro] malforms" (11-11 15:32). Measured in Saint-Lo's camp with live pictures saved (which slow those
// [acct2-bro] frames): dev/agents/rexglue-native/RENDER-NOTES.md, 11-12, "the soldiers that flash for one frame".
REXCVAR_DEFINE_BOOL(cod3_anim_lod_between_updates, true, "cod3",
                    "When a frame runs two or more game updates (below cod3_server_hz frames a second), each update "
                    "after the first sees the soldiers and other animated models the last frame drew as drawn, as on "
                    "the console (their animation detail and the game's on-screen checks); off: as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native npc] 2026-11-09 so a run's screenshots can be matched to the frames that ran two or more updates.
REXCVAR_DEFINE_INT32(cod3_debug_frame_updates, 0, "cod3",
                     "Log one line for each of the next N drawn frames of a level: the game updates it ran and, for "
                     "the updates after its first, how many animated models got the coarsest animation detail "
                     "(0: off; setting it again starts again)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native npc] 2026-11-09 (main: is the soldier against the tank drawn malformed below 60 frames a second?
// [rexglue-native npc] a still every few seconds misses a fault that lasts one frame) Read only: every bone of the
// [rexglue-native npc] animated models near the player, frame by frame, for limb lengths and jumps (PoseBonesLines).
REXCVAR_DEFINE_INT32(cod3_debug_pose_bones, 0, "cod3",
                     "Log for each of the next N drawn frames of a level the bones of the 3 animated models nearest "
                     "the player (each bone's origin in its model's own space) and the frame's game updates (0: off; "
                     "setting it again starts again)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native npc] 2026-11-09 (main: the soldier against the tank at mace2's start, in world space, at 38 and at 60:
// [rexglue-native npc] does his place on the tank jump in frames with two updates, do his root and limbs come apart?)
// [rexglue-native npc] Read only (PoseWorldLines).
REXCVAR_DEFINE_INT32(cod3_debug_pose_bones_near, 0, "cod3",
                     "With cod3_debug_pose_bones: 0 the 3 animated models nearest the player, in their model's space; "
                     "-1 the vehicle nearest the player, N > 0 the entity numbered N: that entity and one animated "
                     "model near it (cod3_debug_pose_bones_pick) in world space, once a drawn frame: their origins, "
                     "angles and scene nodes, the model's place in the entity's frame, and its bones placed by its node")
    .range(-1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native-bugs] 2026-11-11 nl1 (owner 07:29: "st lo ride car jitter isn't in-sync with the player": in his
// [rexglue-native-bugs] recording the truck's bed and benches shake against the view while the scenery outside does not).
// [rexglue-native-bugs] Which entities draw a vehicle and what is inside it, and from what: the game's draw for each type
// [rexglue-native-bugs] fills an entity's scene node from its server origin (+336), from the drawn origin (+112, which the
// [rexglue-native-bugs] smoothing here sets between updates) or from its trajectory's base (+32). Read only (NearEntitiesLines).
REXCVAR_DEFINE_INT32(cod3_debug_near_entities, 0, "cod3",
                     "Log once a second every entity the client game draws within N units of the camera: its number, "
                     "type, trajectory type, flags, bones, server / drawn / scene-node origins, which of them the node "
                     "was drawn from, and how often the node moved in frames with and without a game update (0: off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(cod3_debug_pose_bones_pick, 0, "cod3",
                     "With cod3_debug_pose_bones_near: the model traced: 0 the animated model nearest that entity when "
                     "the trace starts; N > 0 the entity numbered N")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_server_step_sync, true, "cod3/Game",
                     "When a frame runs more than one game update (below cod3_server_hz frames a "
                     "second, or after a hitch), wait for the game's jobs of one update before the "
                     "next starts, as a drawn frame between them would; off: they start at once")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_server_step, false, "cod3",
                    "Log once a second the game updates, the frames and the things drawn between "
                    "updates (cod3_server_hz)");

// [claude-9b] 2026-10-27 (owner 03:27: "movement is still fucked. Now it doesn't matter the fps when you move
// you bounce"): one log line a drawn frame while the player moves, to see what the camera does.
REXCVAR_DEFINE_INT32(cod3_debug_view_trace, 0, "cod3",
                     "Log the camera and the player's position in every drawn frame while the player "
                     "moves, for this many frames (0: off)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [claude-9b] 2026-10-27 (owner 04:23: "In saint lo the NPC on the crate when doing his idle animation will act as if
// there's two states of being, one where he's sitting and one where he's making some motions"): what each animated model
// is drawn with, frame by frame (dev/agents/claude-9b-think/FINDINGS-NPCPOSE.md, Q4). A model's bones are one buffer
// (model +0x70), written by the game update; the renderer only reads them. This hashes the bones each drawn frame and at
// each update's end.
REXCVAR_DEFINE_INT32(cod3_debug_pose, 0, "cod3",
                     "Log once a second the animated models drawn in a pose (their bones) that goes back to the one "
                     "before last (A-B-A), poses that changed in frames without a game update or since the update, "
                     "and models that jumped in place; -1: on; N: also one line a drawn frame (300) for entity N-1")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;

constexpr uint32_t kGentityTable = 0x82A2AB40;       // *(it) + 4 * (client + 1): the player's entity
constexpr uint32_t kActiveLocalClient = 0x82A2A1C8;  // the player index the game uses
constexpr uint32_t kCgTime = 0x82A95244;             // the client game's time (CG_DrawActiveFrame)

constexpr uint32_t kEntFlags = 16;
constexpr uint32_t kEntPosType = 20;        // trajectory type of the position
constexpr uint32_t kEntDrawnOrigin = 112;   // where the client game draws it (vec3)
constexpr uint32_t kEntServerOrigin = 336;  // the server's current origin (vec3)
constexpr uint32_t kMovedFlag = 0x40000000;  // set by the game when the drawn origin changes
constexpr int32_t kTrInterpolate = 1;
constexpr int32_t kTrStationary = 0;        // [claude-9b] drawn at its trajectory's base (+32)
constexpr uint32_t kEntTrBase = 32;         // [claude-9b] the position trajectory's base (vec3)
constexpr uint32_t kEntNumber = 500;        // [claude-9b] the entity's number

constexpr int kMaxTicksPerFrame = 6;
constexpr float kMaxSmoothedMove = 128.0f;  // more in one update: a teleport, not smoothed
// [claude-9b] 2026-10-27 the angles (cod3_server_smooth_view and the drawn angles)
constexpr uint32_t kEntDrawnAngles = 128;    // the angles the client game draws it with (0x82438B68)
constexpr uint32_t kEntClient = 528;         // the player's client
constexpr uint32_t kEntVehicleInfo = 540;    // a vehicle's own data (0x82447D48 tests it)
constexpr uint32_t kClientViewAngles = 172;  // the view's angles: 0x824505E8 places the camera by them
constexpr uint32_t kClientVehicle = 1148;    // the handle of the vehicle the player rides
constexpr uint32_t kClientPsFlags = 208;     // [claude-9b-think] the playerState's flags (fov.cpp kPsFlags 0xD0)
constexpr uint32_t kPsOnAGun = 0x6000;       // [claude-9b-think] working a mounted gun (fov.cpp kOnAGun)
constexpr float kMaxSmoothedTurn = 90.0f;    // more in one update (degrees): a cut, not smoothed
// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_ride
constexpr uint32_t kEntAnglesPrev = 72;      // the angles' trajectory base: 0x82438B68 blends from it
constexpr uint32_t kEntAnglesNow = 84;       // to these by cg +2800 (the update's angles; checked in the trace)
// [claude-9b-think] 2026-10-27 the entity's server angles (r.currentAngles, pitch / yaw / roll at +352 / +356 / +360):
// 0x8254D2A8 places a tag by AnglesToAxis(+352) at the origin +336, and the vehicle cameras read them (0x8244F8E0
// +356 / +360). The ride's angles, so the camera and the drawn vehicle turn as the game's own cameras see it. (The
// ride read +84 first: claude-9b-test's hostage ride had 0 there in every frame, and the update's yaw at +76, the
// vector at +72, which 0x82438B68 draws with cg +2800 at 0.)
constexpr uint32_t kEntServerAngles = 352;
constexpr uint32_t kClientOrigin = 0;        // the playerState's origin (the seat, in a vehicle)
constexpr uint32_t kClientDeltaAngles = 88;  // delta_angles: whole short angles in ints
// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_body: the scene node the renderer draws an entity from.
constexpr uint32_t kEntSceneNode = 548;      // made by 0x824721C8 (260 bytes); its refEntity is at node +8
constexpr uint32_t kNodeLightOrigin = 16;    // the lighting origin (the origin raised a little)
constexpr uint32_t kNodeAxis = 28;           // forward, left, up (3 x vec3, as AnglesToAxis 0x82532B18)
constexpr uint32_t kNodeOrigin = 68;         // where the model is drawn (0x8242E450's matrix: axis rows, then this)
constexpr uint32_t kNodeOldOrigin = 80;      // a second copy of the origin
constexpr uint32_t kEntOnVehicleFlag = 0x00100000;  // flags (+16): rides a vehicle (0x8244B610 turns it by the vehicle's)
constexpr uint32_t kEntVehicleRidden = 432;  // its handle (as the entity handles: index 14 bits, generation above)

// [claude-9b] 2026-10-27 (owner 10-27 00:14: "start on the 60hz. This is priority one").
// The server frame's milliseconds: SV_Frame stores its r3 at +160 (and 0 at +164) on every path, and
// the client reads it as the frame's time: CL_AdjustAngles (stick and key turning), the
// IN_*AnimWeight commands and CL_Frame (0x82511B28). With updates of 1/hz s it held the last
// update's 16-17 ms on every frame, also the frames without an update, so the stick turned 4 times
// as fast at 240 fps: after the updates it gets the frame's own time again, as the game set it.
constexpr uint32_t kSvFrameMsec = 0x829C3F08 + 160;
// The client game's entity list and the game's level (level_locals) values CG_AddPacketEntities
// draws by (0x8244E678): it draws an entity of the list when its +548 object's +256 equals
// level +164, which SV_SendClientMessages (vmMain 21) sets to level.time (+156) and
// SV_BuildClientSnapshot (0x82513A40) writes into the entities it sends. Diagnostics only.
constexpr uint32_t kEntListCount = 0x82A2DAD0 + 0x18204;
constexpr uint32_t kEntList = 0x82A35CD4;
constexpr uint32_t kLevelSnapTime = 0x82A4E790 + 164;

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// Update k ends at floor(k * 1000 / hz) ms of the game clock.
int64_t TickEnd(int64_t k, int hz) { return k * 1000 / hz; }
// The last update ending at or before t.
int64_t TickAtOrBefore(int64_t t, int hz) { return ((t + 1) * hz - 1) / 1000; }

struct Clock {
  int hz = -1;
  int64_t time = 0;      // the game clock: the frames' milliseconds added up
  int64_t tick = 0;      // the last update done (its index)
  uint64_t updates = 0;  // updates done (counts every SV_Frame run here)
};
Clock g_clock;

void ReadVec(uint8_t* base, uint32_t a, float v[3]) {
  for (int i = 0; i < 3; ++i) v[i] = g::ReadF32(base, a + 4 * i);
}
void WriteVec(uint8_t* base, uint32_t a, const float v[3]) {
  for (int i = 0; i < 3; ++i) g::WriteF32(base, a + 4 * i, v[i]);
}
bool Same(const float a[3], const float b[3]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }
float Dist2(const float a[3], const float b[3]) {
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return x * x + y * y + z * z;
}

// An entity's server origin at the last two updates it was seen in.
struct History {
  uint64_t update = 0;
  bool valid = false;
  int32_t number = -1;  // [claude-9b] the entity's number: a slot used again starts a new history
  float prev[3] = {};
  float cur[3] = {};
  // [claude-9b] its drawn angles at the last two updates
  uint64_t angles_update = 0;
  bool angles_valid = false;
  float angles_prev[3] = {};
  float angles_cur[3] = {};
};
std::unordered_map<uint32_t, History> g_history;  // by entity address (entities numbered past the table below)
History g_player;                                 // the player's entity (for a ridden vehicle's view)

// [acct2-cod3-motion] 2026-11-07 (acct2-bro's profile of St-Bert, native renderer, uncapped: the map's lookups took
// 1.8 % of the game's main thread) The histories in a flat table by the entity's slot: its number (+500) is its handle,
// the slot's index in the low 14 bits and a generation above (32972 = generation 2, index 204), as EntityOfHandle reads
// handles. The map above only past the table. A slot keeps the entity address it is for and starts over for another (and
// the callers start over for another number).
struct HistorySlot {
  uint32_t ent = 0;
  History h;
};
constexpr int32_t kHistorySlots = 4096;  // as many as the entity handles (EntityOfHandle)
std::vector<HistorySlot> g_history_flat(kHistorySlots);
History& HistoryOf(uint8_t* base, uint32_t ent) {
  const int32_t index = g::ReadS32(base, ent + kEntNumber) & 0x3FFF;
  if (index >= kHistorySlots) return g_history[ent];
  HistorySlot& s = g_history_flat[static_cast<size_t>(index)];
  if (s.ent != ent) {
    s.ent = ent;
    s.h = History{};
  }
  return s.h;
}

// [claude-9b] 2026-10-27 The player's view angles after the last two updates (cod3_server_smooth_view),
// and the last update in whose interval they changed in a frame without an update (turned by the
// player every frame: not smoothed).
struct ViewHistory {
  uint64_t update = 0;
  bool valid = false;
  float prev[3] = {};
  float cur[3] = {};
  bool turned_between = false;
  uint64_t turned_between_at = 0;
};
ViewHistory g_view;

// [claude-9b-think] 2026-10-27 UNTESTED The vehicle the player rides at the last two updates (cod3_server_smooth_ride),
// kept after each update: its server origin (+336) and angles (+84), the turn the update gave the player's view, and
// how far the update moved the playerState's origin (the seat; the trace only).
struct RideHistory {
  uint64_t update = 0;
  bool have = false;   // cur is the vehicle's at `update`
  bool valid = false;  // prev and cur are of two updates in a row, near enough to draw between
  uint32_t ent = 0;
  int32_t number = -1;
  float origin_prev[3] = {}, origin_cur[3] = {};
  float angles_prev[3] = {}, angles_cur[3] = {};
  float view_turn[3] = {};   // the view angles' turn in the last update (degrees)
  float delta_turn[3] = {};  // delta_angles' turn in it (degrees; the trace)
  float seat_jump[3] = {};   // the playerState origin's move in it
};
RideHistory g_ride;
// The player's view angles, delta_angles and origin before the update running now.
struct RideBefore {
  bool have = false;
  float view[3] = {};
  int32_t delta[3] = {};
  float origin[3] = {};
};
RideBefore g_ride_before;
// cod3_debug_view_trace: what the ride did this frame (-1 riding, nothing moved; 0 not riding; 1 moved), the camera's
// offset and the vehicle's drawn yaw.
int g_ride_trace_state = 0;
float g_ride_trace_off[3] = {};
float g_ride_trace_yaw = 0.0f;
bool g_ride_trace_drawn = false;  // the ridden vehicle was drawn from the ride's pose
bool g_ride_trace_angles_ok = false;  // [claude-9b-think] and its angles too (RideDrawVehicle's check)
int g_ride_trace_draw_why = 5;  // [claude-9b-think] for the ride's vehicle: 0 drawn, 2 not ridden, 3 no pose, 4 drawn
                                // moved; 5 not called for it (smoothing off); 6 drawn over the game's own place
// [claude-9b-think] 2026-10-27 UNTESTED ride6, cod3_debug_view_trace: the picture's two sides of the ridden vehicle.
float g_ride_trace_scam[3] = {};    // the camera when the vehicle was placed (inside RE_RenderScene: the scene's)
bool g_ride_trace_scam_have = false;
int g_ride_trace_vpath = -1;        // how it was placed: 0 the game's, 1 the other smoothing, 2 the ride; -1 not
float g_ride_trace_b308 = 0.0f;     // how far 0x82448308 moved the camera after SmoothView
uint32_t g_ride_trace_v286 = 0;     // its view's +286 flags (bit 0: the view follows a bone, 0x824411D8)
float g_ride_trace_rel_last[3] = {};  // the last frame's camera in the vehicle's drawn frame, for its step
bool g_ride_trace_rel_have = false;
// [claude-9b-think] 2026-10-27 UNTESTED ride9, cod3_debug_view_trace: the ridden vehicle's scene node this frame (body:
// -1 not drawn by the vehicles' draw, 0 left at the update's pose, 1 moved to its own pose between updates, 2 to the
// ride's pose), and the camera in the node's frame last frame (the step on the screen, as the renderer draws it).
int g_ride_trace_body = -1;
float g_ride_trace_nrel_last[3] = {};
bool g_ride_trace_nrel_have = false;

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_body: an entity's server origin (+336) and angles (+352) at
// its last two updates, kept by its first draw after each update.
struct BodyHistory {
  int32_t number = -1;
  uint64_t update = 0;
  bool valid = false;
  float origin_prev[3] = {}, origin_cur[3] = {};
  float angles_prev[3] = {}, angles_cur[3] = {};
};
std::unordered_map<uint32_t, BodyHistory> g_body;  // by entity address

// Drawn origins changed this frame, to put back before the next server frame.
struct Saved {
  uint32_t ent;
  float origin[3];
  float drawn[3];  // [claude-9b] what was written: put back only while it is still there
};
std::vector<Saved> g_saved;
std::vector<Saved> g_saved_angles;  // [claude-9b] the same for the drawn angles

// The debug log.
struct Counts {
  int frames = 0, updates = 0, cg_time_moved = 0, smoothed = 0, redrawn = 0, turned = 0, views = 0;
  int bodies = 0;  // [claude-9b-think] scene nodes moved by cod3_server_smooth_body
  int32_t last_cg_time = 0;
  int64_t since = 0;
};
Counts g_counts;

// [claude-9b] 2026-10-27 What the client game draws in frames without a game update against frames
// with one (cod3_debug_server_step): the owner sees people and other things flicker above 60 fps
// and a bench drew fewer things then. [0] frames without an update, [1] frames with one or more.
struct DrawCounts {
  uint64_t frames = 0, multi = 0;                 // frames, of them with 2 or more updates
  uint64_t cg_frames = 0, cg_time_moved = 0;      // CG_DrawActiveFrame calls, its time changed
  uint64_t lists = 0, list_items = 0, linked = 0, stamped = 0, snap_time_moved = 0;
  uint64_t cents = 0, cents_by_type[16] = {};     // CG_AddCEntity calls, by entity type (+0)
  uint64_t refents = 0;                           // RE_AddRefEntityToScene calls
  uint64_t lerps = 0, lerp_interp = 0, lerp_moved = 0;  // CG_CalcEntityLerpPositions
};
DrawCounts g_draw[2];
int g_kind = 1;               // the current frame's kind (set in SV_Frame)
int g_draw_updates = 1;       // [claude-9b] the current frame's updates (cod3_debug_view_trace)
int g_es_msec = 0;            // [acct2-cod3-motion] the current frame's milliseconds (cod3_debug_entity_steps)
uint64_t g_es_refents = 0;    // [acct2-cod3-motion] RE_AddRefEntityToScene calls (cod3_debug_entity_steps)
int32_t g_last_cg_time = 0;   // CG_DrawActiveFrame's r3 last time
uint32_t g_last_snap_time = 0;
bool DebugOn() { return REXCVAR_GET(cod3_debug_server_step); }

// [claude-9b] 2026-10-27 Set for the updates of a frame before its last: their SV_Frame skips
// SV_SendClientMessages, so a frame sends one snapshot, as the game's SV_Frame does however many
// server frames it runs. With up to kMaxTicksPerFrame updates in one frame each sent one, and more
// than 4 overflowed the local client's packet queue (see the NET_SendLoopPacket hook).
bool g_skip_send = false;
uint64_t g_sends_skipped = 0, g_loop_full = 0;
// [claude-9b] 2026-10-27 The animated models the renderer drew (cod3_debug_server_step), by the kind
// of frame ([0] without an update, [1] with): of the animated list's entities with a model, how many
// were ready to draw after the frame's updates (bit 0 of model +196, see cod3_server_step_draw) and
// how many of those the frame drew (the bit cleared by the next frame's start; out of view keeps it).
struct ModelCounts {
  uint64_t frames = 0, models = 0, armed = 0, drawn = 0;
};
ModelCounts g_models[2];
uint64_t g_armed_after = 0;   // ready to draw after the last frame's updates
int g_armed_kind = -1;        // that frame's kind (-1: none counted yet)
constexpr uint32_t kAnimatedList = 0x82A50E24;  // +0 the handles, +8 how many (0x8253F438)
constexpr uint32_t kEntityHandles = 0x82A2DAD0 + 512;  // a handle's low 14 bits * 8: entity, generation
// Counts the animated list's models (as 0x8253F438 walks it) and those ready to draw. after = false
// at the frame's start (before its updates): the last frame drew those no longer ready.
void CountModels(uint8_t* base, bool after) {
  const uint32_t handles = g::ReadU32(base, kAnimatedList);
  const int32_t count = std::min<int32_t>(g::ReadS32(base, kAnimatedList + 8), 4096);
  uint64_t models = 0, armed = 0;
  for (int32_t i = 0; ValidGuest(handles) && i < count; ++i) {
    const uint32_t h = g::ReadU32(base, handles + 4 * i);
    const uint32_t index = h & 0x3FFF;
    if (index >= 4096) continue;
    const uint32_t slot = kEntityHandles + 8 * index;
    if (g::ReadU32(base, slot + 4) != (h >> 14)) continue;
    const uint32_t ent = g::ReadU32(base, slot);
    if (!ValidGuest(ent)) continue;
    const uint32_t model = g::ReadU32(base, ent + 508);
    if (!ValidGuest(model)) continue;
    ++models;
    if (g::ReadU32(base, model + 196) & 1) ++armed;
  }
  if (!after) {
    if (g_armed_kind >= 0 && g_armed_after >= armed) g_models[g_armed_kind].drawn += g_armed_after - armed;
    g_armed_kind = -1;
    return;
  }
  ModelCounts& m = g_models[g_kind];
  ++m.frames;
  m.models += models;
  m.armed += armed;
  g_armed_after = armed;
  g_armed_kind = g_kind;
}

// [rexglue-native npc] 2026-11-09 cod3_anim_lod_between_updates (see its definition): the animated models the last
// [rexglue-native npc] frame drew (bit 0 of +196 clear at the frame's start, before its first update), by handle and
// [rexglue-native npc] model, and the bit cleared on them again before each further update of the frame (only where the
// [rexglue-native npc] handle still names the same entity with the same model, so a model freed by an update is
// [rexglue-native npc] never written).
struct DrawnModel {
  uint32_t handle, model;
};
std::vector<DrawnModel> g_drawn_models;  // game thread
uint64_t g_redrawn_marks = 0;            // cod3_debug_server_step: models seen as drawn again, since the last line
// [rexglue-native npc] cod3_debug_server_step: the animation detail the updates chose (0x82421AA8, below), by the
// [rexglue-native npc] update's place in its frame ([0] the first, [1] a later one): models, and of them the coarsest
// [rexglue-native npc] (class 5: not drawn since the last update).
bool g_update_later = false;
uint64_t g_lod_models[2] = {}, g_lod_coarse[2] = {};

// [rexglue-native npc] The entity a handle of the animated list names, or 0 (as CountModels resolves it).
uint32_t AnimatedEntity(uint8_t* base, uint32_t h) {
  const uint32_t index = h & 0x3FFF;
  if (index >= 4096) return 0;
  const uint32_t slot = kEntityHandles + 8 * index;
  if (g::ReadU32(base, slot + 4) != (h >> 14)) return 0;
  const uint32_t ent = g::ReadU32(base, slot);
  return ValidGuest(ent) ? ent : 0;
}

void NoteDrawnModels(uint8_t* base) {  // [rexglue-native npc]
  g_drawn_models.clear();
  const uint32_t handles = g::ReadU32(base, kAnimatedList);
  const int32_t count = std::min<int32_t>(g::ReadS32(base, kAnimatedList + 8), 4096);
  for (int32_t i = 0; ValidGuest(handles) && i < count; ++i) {
    const uint32_t h = g::ReadU32(base, handles + 4 * i);
    const uint32_t ent = AnimatedEntity(base, h);
    const uint32_t model = ent ? g::ReadU32(base, ent + 508) : 0;
    if (!ValidGuest(model) || (g::ReadU32(base, model + 196) & 1)) continue;  // not drawn: keeps its bit
    g_drawn_models.push_back({h, model});
  }
}

void MarkDrawnAgain(uint8_t* base) {  // [rexglue-native npc]
  for (const DrawnModel& d : g_drawn_models) {
    const uint32_t ent = AnimatedEntity(base, d.handle);
    if (!ent || g::ReadU32(base, ent + 508) != d.model) continue;
    g::WriteU32(base, d.model + 196, g::ReadU32(base, d.model + 196) & ~1u);
    ++g_redrawn_marks;
  }
}

// [rexglue-native npc] cod3_debug_frame_updates: the frame lines still to log, and this frame's coarsest models in
// [rexglue-native npc] its updates after the first (counted by 0x82421AA8's hook while lines are logged).
int32_t g_frame_lines_left = 0, g_frame_lines_asked = 0;
uint64_t g_frame_number = 0;
uint32_t g_frame_coarse_later = 0;
bool FrameLinesOn() { return g_frame_lines_left > 0; }
void FrameUpdatesLine(int64_t updates, int msec) {
  const int32_t asked = REXCVAR_GET(cod3_debug_frame_updates);
  if (asked != g_frame_lines_asked) g_frame_lines_left = g_frame_lines_asked = asked;
  ++g_frame_number;
  if (g_frame_lines_left <= 0) return;
  --g_frame_lines_left;
  REXLOG_INFO("cod3: server step: frame {}: {} game updates ({} ms); coarsest models in its later updates {} "
              "(cod3_anim_lod_between_updates {})",
              g_frame_number, updates, msec, g_frame_coarse_later,
              REXCVAR_GET(cod3_anim_lod_between_updates) ? "on" : "off");
  g_frame_coarse_later = 0;
}

// [claude-9b] 2026-10-27 The camera between updates (cod3_debug_server_step; owner 02:34: "First person
// within vehicles has a LOT of judder/jerky movement ... cars and bikes seem to be effected as
// well"). At each frame's start the last frame's camera is still in the client game's refdef
// (origin cg +4480, angles cg +4544; cg 0x82A98D90 + client x 5840, written by 0x824505E8, whose
// mode 0x82447D48 picks how). Per kind of frame: how far the camera moved and turned. A camera
// moving in the game's update steps moves only in the frames with an update.
constexpr uint32_t kCgBase = 0x82A98D90, kCgSize = 5840, kCgViewOrigin = 4480, kCgViewAngles = 4544;
struct CameraCounts {
  uint64_t frames = 0, still = 0, turned_none = 0;
  double moved = 0.0, turned = 0.0;
  uint64_t modes[24] = {};
};
CameraCounts g_camera[2];
float g_cam_last[6] = {};
bool g_cam_have = false;
uint32_t g_cam_mode = 0;  // the last mode 0x82447D48 returned
uint64_t g_upcam_blended = 0;  // [acct2-ocr] frames whose camera cod3_server_smooth_cams drew between updates
bool g_upcam_drawn = false;  // [acct2-ocr] cod3_server_smooth_cams drew this frame's camera (smooth_linked leaves it)
uint64_t g_linked_blended = 0, g_linked_moved = 0;  // [acct2-ocr] cod3_server_smooth_linked: drawn; SmoothView moved
uint64_t g_linked_turned = 0;  // [acct2-cod3-motion] cod3_server_smooth_linked_turn: frames its angles were drawn
// [acct2-cod3-motion] cod3_debug_view_trace: what cod3_server_smooth_linked did this frame (-1 not its mode or off,
// 0 left the camera, 1 drew it between updates, 2 SmoothView had moved it, 3 a new camera or a cut)
int g_linked_trace = -1;
float g_linked_trace_off[3] = {};
bool g_upcam_trace = false;  // cod3_server_smooth_cams drew this frame's camera
// [acct2-cod3-motion] cod3_server_smooth_linked_keep: the camera's place as 0x824505E8 left it this frame
float g_game_cam[3] = {};
bool g_game_cam_have = false;
float AngleDiff(float a, float b) {
  float d = std::fmod(a - b, 360.0f);
  if (d > 180.0f) d -= 360.0f;
  if (d < -180.0f) d += 360.0f;
  return std::fabs(d);
}
void CountCamera(uint8_t* base) {
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client > 3) return;
  const uint32_t cg = kCgBase + client * kCgSize;
  float now[6];
  for (int i = 0; i < 3; ++i) {
    now[i] = g::ReadF32(base, cg + kCgViewOrigin + 4 * i);
    now[3 + i] = g::ReadF32(base, cg + kCgViewAngles + 4 * i);
  }
  if (g_cam_have) {
    // The move into the last drawn frame, by that frame's kind: g_kind isn't updated yet. (It used the
    // kind saved one call earlier, so each move was counted under the frame before's kind.)
    CameraCounts& c = g_camera[g_kind];
    const float dx = now[0] - g_cam_last[0], dy = now[1] - g_cam_last[1], dz = now[2] - g_cam_last[2];
    const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float turned = AngleDiff(now[3], g_cam_last[3]) + AngleDiff(now[4], g_cam_last[4]);
    if (moved < 64.0f && turned < 45.0f) {  // not a cut
      ++c.frames;
      c.moved += moved;
      c.turned += turned;
      if (moved < 0.001f) ++c.still;
      if (turned < 0.0001f) ++c.turned_none;
      ++c.modes[g_cam_mode < 23 ? g_cam_mode : 23];
    }
  }
  for (int i = 0; i < 6; ++i) g_cam_last[i] = now[i];
  g_cam_have = true;
}


// [claude-9b] 2026-10-27 The client game's clock between updates. CG_DrawActiveFrame(serverTime) gets
// the server's time (level.time, the last update's), so above the update rate cg.time stood still in
// the frames between updates (diagnostic run c9b-shzdiag-blkbrn: "its time moved in 0.00" in every
// frame without an update, with the same 424 entities drawn): everything the client game moves by
// its frame time (cg +0 = cg.time - cg.oldTime, 0x82A95240) stopped for those frames. Now it gets the
// last update's time plus how far the game's clock is into the next update (at most one update
// ahead, never backwards, and nothing added while the game's time does not move: pause, menus).
constexpr uint32_t kLevelTime = 0x82A4E790 + 156;  // level.time (vmMain 19 returns it)
bool g_time_advancing = false;  // the last update moved level.time
int32_t g_cg_last_in = 0, g_cg_last_out = 0;

int Hz() { return REXCVAR_GET(cod3_server_hz); }

// [claude-9b] How far (ms) the game's clock is into the next update while the game's time moves: the
// client's clocks run on by it between updates (at most one update, 0 while paused or in menus).
int32_t Lead() {
  const int hz = g_clock.hz;
  if (hz <= 0 || !g_time_advancing) return 0;
  const int64_t step = TickEnd(g_clock.tick + 1, hz) - TickEnd(g_clock.tick, hz);
  return static_cast<int32_t>(std::clamp<int64_t>(g_clock.time - TickEnd(g_clock.tick, hz), 0, step));
}

// How much of the last update is not drawn yet: 1 right after it, 0 when the next is due.
// [acct2-cod3-motion] cod3_server_draw_subms: the real time since the clock started counting, less the milliseconds
// the game counted (within one millisecond either way while both clocks run together; 0 after a stall).
struct SubMs {
  bool have = false;
  std::chrono::steady_clock::time_point last{};
  double acc = 0.0;  // milliseconds
};
SubMs g_subms;
void SubMsFrame(int msec) {
  const auto now = std::chrono::steady_clock::now();
  if (g_subms.have) {
    const double real_ms = std::chrono::duration<double, std::milli>(now - g_subms.last).count();
    if (real_ms > 250.0 || msec > 250 || msec < 0)
      g_subms.acc = 0.0;  // a level load, a long hitch: start again
    else
      g_subms.acc = std::clamp(g_subms.acc + real_ms - double(msec), -0.999, 0.999);
  }
  g_subms.last = now;
  g_subms.have = true;
}
double SubMsFraction() { return REXCVAR_GET(cod3_server_draw_subms) ? g_subms.acc : 0.0; }

float Back() {
  const int hz = g_clock.hz;
  if (hz <= 0) return 0.0f;
  const int64_t last = TickEnd(g_clock.tick, hz), next = TickEnd(g_clock.tick + 1, hz);
  if (next <= last) return 0.0f;
  // [acct2-cod3-motion] with the part of a millisecond the game's clock hasn't counted (cod3_server_draw_subms)
  const float into = float((double(g_clock.time - last) + SubMsFraction()) / double(next - last));
  return 1.0f - std::clamp(into, 0.0f, 1.0f);
}

// A new server origin for an entity's history; true when it can be drawn between the two.
bool Advance(History& h, const float now[3]) {
  if (h.update != g_clock.updates) {
    if (h.valid && h.update + 1 == g_clock.updates) {
      for (int i = 0; i < 3; ++i) h.prev[i] = h.cur[i];
    } else {
      for (int i = 0; i < 3; ++i) h.prev[i] = now[i];  // missed updates: nothing to draw between
    }
    for (int i = 0; i < 3; ++i) h.cur[i] = now[i];
    h.update = g_clock.updates;
    h.valid = true;
  } else if (!Same(now, h.cur)) {
    for (int i = 0; i < 3; ++i) h.prev[i] = h.cur[i] = now[i];  // moved since the update
  }
  return !Same(h.prev, h.cur) && Dist2(h.prev, h.cur) < kMaxSmoothedMove * kMaxSmoothedMove;
}

uint32_t PlayerEntity(uint8_t* base) {
  const uint32_t table = g::ReadU32(base, kGentityTable);
  if (!ValidGuest(table)) return 0;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client > 3) return 0;
  return g::ReadU32(base, table + 4 * (client + 1));
}

// [rexglue-native npc] 2026-11-09 cod3_debug_pose_bones: after a frame's game updates (the bones the frame will draw:
// [rexglue-native npc] only updates write them), one line for each of the 3 animated models nearest the player
// [rexglue-native npc] (within 1000 units): the frame (as cod3_debug_frame_updates numbers it), its updates, the
// [rexglue-native npc] entity's number and distance, and each bone's origin in the model's own space ([model +0x70]
// [rexglue-native npc] +0x30, 0x40 bytes a bone, the origin row at +0x30, model +0xAB of them: ride_bones.cpp's layout).
// [rexglue-native npc] Reads only; the limb lengths and jumps are worked out from the log afterwards.
int32_t g_bone_lines_left = 0, g_bone_lines_asked = 0;
void PoseBonesLines(uint8_t* base, int64_t updates) {
  if (REXCVAR_GET(cod3_debug_pose_bones_near) != 0) return;  // [rexglue-native npc] world space: PoseWorldLines
  const int32_t asked = REXCVAR_GET(cod3_debug_pose_bones);
  if (asked != g_bone_lines_asked) g_bone_lines_left = g_bone_lines_asked = asked;
  if (g_bone_lines_left <= 0) return;
  const uint32_t player = PlayerEntity(base);
  const uint32_t handles = g::ReadU32(base, kAnimatedList);
  if (!ValidGuest(player) || !ValidGuest(handles)) return;
  --g_bone_lines_left;
  float eye[3];
  ReadVec(base, player + kEntServerOrigin, eye);
  struct Near {
    float d2;
    uint32_t ent, model;
  };
  Near nearest[3] = {{1e12f, 0, 0}, {1e12f, 0, 0}, {1e12f, 0, 0}};
  const int32_t count = std::min<int32_t>(g::ReadS32(base, kAnimatedList + 8), 4096);
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t ent = AnimatedEntity(base, g::ReadU32(base, handles + 4 * i));
    if (!ent || ent == player) continue;
    const uint32_t model = g::ReadU32(base, ent + 508);
    if (!ValidGuest(model) || !ValidGuest(g::ReadU32(base, model + 0x70))) continue;
    float at[3];
    ReadVec(base, ent + kEntServerOrigin, at);
    const float d2 = Dist2(at, eye);
    if (d2 > 1000.0f * 1000.0f) continue;
    for (int k = 0; k < 3; ++k) {
      if (d2 >= nearest[k].d2) continue;
      for (int m = 2; m > k; --m) nearest[m] = nearest[m - 1];
      nearest[k] = {d2, ent, model};
      break;
    }
  }
  for (const Near& n : nearest) {
    if (!n.ent) continue;
    const uint32_t buf = g::ReadU32(base, n.model + 0x70);
    const uint32_t bones = std::min<uint32_t>(g::ReadU8(base, n.model + 0xAB), 200);
    std::string text;
    text.reserve(bones * 26);
    for (uint32_t b = 0; b < bones; ++b) {
      float o[3];
      ReadVec(base, buf + 0x30 + 0x40 * b + 0x30, o);
      text += fmt::format(" {:.2f},{:.2f},{:.2f}", o[0], o[1], o[2]);
    }
    REXLOG_INFO("cod3: pose bones: frame {} updates {} entity {} at {:.0f} units, {} bones:{}", g_frame_number, updates,
                g::ReadS32(base, n.ent + kEntNumber), std::sqrt(n.d2), bones, text);
  }
}

// [rexglue-native npc] 2026-11-09 cod3_debug_pose_bones_near (see the setting). Logged after the client game's frame
// [rexglue-native npc] (CG_DrawActiveFrame's hook), when each entity's scene node (+548: axis node +28 / +40 / +52,
// [rexglue-native npc] origin node +68) holds where the renderer draws it (the game's draw, then
// [rexglue-native npc] cod3_server_smooth_body's move) and the bones are those the frame drew (only updates write them).
// [rexglue-native npc] A bone's world place: its origin in model space along the node's axis rows, plus the node's origin
// [rexglue-native npc] (0x8242E450's matrix). The center and the model traced are chosen once and kept; the choice is
// [rexglue-native npc] logged once with the candidates, and with where the traced entity, its actor (+0x214) and the
// [rexglue-native npc] structures their words point at hold the center's address or number (a link), and where the center
// [rexglue-native npc] holds the traced one's. Reads only.
void AnglesToAxis(const float a[3], float axis[3][3]);  // [rexglue-native npc] defined below
uint32_t EntityOfHandle(uint8_t* base, uint32_t h);      // [rexglue-native npc] defined below
bool GuestReadable(uint32_t a, uint32_t bytes);          // [rexglue-native npc] defined below
constexpr uint32_t kPwModel = 508;               // [rexglue-native npc] the entity's animated model (CountModels)
constexpr uint32_t kPwSceneFlags = 0x278;        // [rexglue-native npc] 0x40000000: a scripted scene's actor
constexpr uint32_t kPwActor = 0x214;             // [rexglue-native npc] its actor (actor +0 = the entity)
constexpr uint32_t kPwModelStamp = 0xAC;         // [rexglue-native npc] the update counter when its animation last ran
constexpr uint32_t kPwModelArmed = 0xC4;         // [rexglue-native npc] bit 0: not drawn since the last update
constexpr uint32_t kPwAnimCounter = 0x82A2ABC0;  // [rexglue-native npc] the animation's update counter
constexpr uint32_t kPwNodeStamp = 256;           // [rexglue-native npc] = level +164: in this frame's draw list
constexpr uint32_t kPwEntScan = 0x400, kPwActorScan = 0xB00, kPwPointedScan = 0x40;  // [rexglue-native npc]
struct PoseWorld {  // [rexglue-native npc]
  int32_t asked = 0, left = 0, center_ask = 0, pick = 0, tries = 0, settle = 0;
  uint32_t center = 0, target = 0;
  int32_t center_number = 0, target_number = 0;
  bool done = false;
};
PoseWorld g_pw;  // [rexglue-native npc]

struct PwPlace {  // [rexglue-native npc]
  float server[3] = {}, angles[3] = {}, drawn[3] = {}, drawn_angles[3] = {}, node[3] = {}, axis[3][3] = {};
  bool node_ok = false, listed = false;
};
void PwRead(uint8_t* base, uint32_t ent, PwPlace& p) {  // [rexglue-native npc]
  ReadVec(base, ent + kEntServerOrigin, p.server);
  ReadVec(base, ent + kEntServerAngles, p.angles);
  ReadVec(base, ent + kEntDrawnOrigin, p.drawn);
  ReadVec(base, ent + kEntDrawnAngles, p.drawn_angles);
  const uint32_t node = g::ReadU32(base, ent + kEntSceneNode);
  p.node_ok = ValidGuest(node);
  if (!p.node_ok) return;
  ReadVec(base, node + kNodeOrigin, p.node);
  for (int a = 0; a < 3; ++a) ReadVec(base, node + kNodeAxis + 12 * a, p.axis[a]);
  p.listed = g::ReadU32(base, node + kPwNodeStamp) == g::ReadU32(base, kLevelSnapTime);
}
std::string PwVec(const float v[3], int digits = 2) {  // [rexglue-native npc]
  return fmt::format("{:.{}f},{:.{}f},{:.{}f}", v[0], digits, v[1], digits, v[2], digits);
}
// [rexglue-native npc] p in the frame of origin o and axis rows ax: its parts along each row.
void PwInFrame(const float p[3], const float o[3], const float ax[3][3], float out[3]) {
  const float d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
  for (int k = 0; k < 3; ++k) out[k] = d[0] * ax[k][0] + d[1] * ax[k][1] + d[2] * ax[k][2];
}
// [rexglue-native npc] The number of the vehicle it rides (+16 bit 0x00100000, the handle at +432); -1: none.
int32_t PwRides(uint8_t* base, uint32_t ent) {
  if (!(g::ReadU32(base, ent + kEntFlags) & kEntOnVehicleFlag)) return -1;
  const uint32_t veh = EntityOfHandle(base, g::ReadU32(base, ent + kEntVehicleRidden));
  return veh ? g::ReadS32(base, veh + kEntNumber) : 0;
}
std::string PwDescribe(uint8_t* base, uint32_t ent, const float from[3]) {  // [rexglue-native npc]
  float at[3];
  ReadVec(base, ent + kEntServerOrigin, at);
  const uint32_t model = g::ReadU32(base, ent + kPwModel);
  const int bones =
      ValidGuest(model) && ValidGuest(g::ReadU32(base, model + 0x70)) ? int(g::ReadU8(base, model + 0xAB)) : 0;
  float angles[3];  // [rexglue-native npc] 2026-11-09 second pass: the angles and the model too
  ReadVec(base, ent + kEntServerAngles, angles);
  return fmt::format("entity {} type {} at {:.0f} units ({:.0f} {:.0f} {:.0f}) angles {:.0f} {:.0f} {:.0f} flags {:08x} "
                     "rides {} scene {:08x} {:08x} model {:08x} bones {}",
                     g::ReadS32(base, ent + kEntNumber), int(g::ReadU8(base, ent)), std::sqrt(Dist2(at, from)), at[0],
                     at[1], at[2], angles[0], angles[1], angles[2], g::ReadU32(base, ent + kEntFlags),
                     PwRides(base, ent), g::ReadU32(base, ent + kPwSceneFlags), g::ReadU32(base, ent + kPwSceneFlags + 4),
                     model, bones);
}
// [rexglue-native npc] Where [at, at + bytes) holds `addr` or `number`, itself or in the first words of a structure one
// [rexglue-native npc] of its words points at (the guest's heaps or the image's data, readable pages only).
std::string PwLinks(uint8_t* base, uint32_t at, uint32_t bytes, uint32_t addr, int32_t number) {
  if (!ValidGuest(at) || !GuestReadable(at, bytes)) return " (not readable)";
  std::string out;
  for (uint32_t off = 0; off < bytes && out.size() < 1500; off += 4) {
    const uint32_t v = g::ReadU32(base, at + off);
    if (v == addr) {
      out += fmt::format(" +0x{:X}=address", off);
    } else if (number > 0 && v == uint32_t(number)) {
      out += fmt::format(" +0x{:X}=number", off);
    } else if (ValidGuest(v) && v >= 0x40000000u && !(v >= 0x82000000u && v < 0x825C0000u) && v != at &&
               GuestReadable(v, kPwPointedScan)) {
      for (uint32_t k = 0; k < kPwPointedScan; k += 4) {
        const uint32_t w = g::ReadU32(base, v + k);
        if (w == addr)
          out += fmt::format(" +0x{:X}->+0x{:X}=address", off, k);
        else if (number > 0 && w == uint32_t(number))
          out += fmt::format(" +0x{:X}->+0x{:X}=number", off, k);
      }
    }
  }
  return out.empty() ? std::string(" none") : out;
}

// [rexglue-native npc] Chooses the center and the model traced (logged once); false while there is none yet.
bool PwChoose(uint8_t* base, uint32_t player) {
  float eye[3];
  ReadVec(base, player + kEntServerOrigin, eye);
  const uint32_t handles = g::ReadU32(base, kAnimatedList);
  const int32_t count = std::min<int32_t>(g::ReadS32(base, kAnimatedList + 8), 4096);
  const uint32_t listed = std::min<uint32_t>(g::ReadU32(base, kEntListCount), 0x4000);
  uint32_t center = 0;
  float best = 1e30f;
  std::string vehicles;
  int found = 0;
  for (uint32_t i = 0; i < listed; ++i) {  // the client game's entities
    const uint32_t e = g::ReadU32(base, kEntList + 4 * i);
    if (!ValidGuest(e) || e == player) continue;
    bool is = false;
    if (g_pw.center_ask < 0) {
      const int type = g::ReadU8(base, e);
      is = type == 14 || type == 15;
      if (is && found++ < 12) vehicles += fmt::format(" [{}]", PwDescribe(base, e, eye));
    } else {
      is = g::ReadS32(base, e + kEntNumber) == g_pw.center_ask;
    }
    float at[3];
    ReadVec(base, e + kEntServerOrigin, at);
    if (is && Dist2(at, eye) < best) {
      best = Dist2(at, eye);
      center = e;
    }
  }
  if (!center && g_pw.center_ask > 0) {  // not in the client game's list: the animated list
    for (int32_t i = 0; ValidGuest(handles) && i < count; ++i) {
      const uint32_t e = AnimatedEntity(base, g::ReadU32(base, handles + 4 * i));
      if (e && g::ReadS32(base, e + kEntNumber) == g_pw.center_ask) center = e;
    }
  }
  if (!center) {
    if (g_pw.tries++ % 600 == 0)
      REXLOG_INFO("cod3: pose world: frame {}: no {} yet ({} entities in the client game's list)", g_frame_number,
                  g_pw.center_ask < 0 ? std::string("vehicle") : fmt::format("entity {}", g_pw.center_ask), listed);
    return false;
  }
  float from[3];
  ReadVec(base, center + kEntServerOrigin, from);
  struct Near {
    float d2;
    uint32_t ent;
  };
  Near nearest[6] = {{1e30f, 0}, {1e30f, 0}, {1e30f, 0}, {1e30f, 0}, {1e30f, 0}, {1e30f, 0}};
  uint32_t target = 0;
  for (int32_t i = 0; ValidGuest(handles) && i < count; ++i) {
    const uint32_t e = AnimatedEntity(base, g::ReadU32(base, handles + 4 * i));
    if (!e || e == center || e == player) continue;
    const uint32_t model = g::ReadU32(base, e + kPwModel);
    if (!ValidGuest(model) || !ValidGuest(g::ReadU32(base, model + 0x70))) continue;
    if (g_pw.pick > 0 && g::ReadS32(base, e + kEntNumber) == g_pw.pick) target = e;
    float at[3];
    ReadVec(base, e + kEntServerOrigin, at);
    const float d2 = Dist2(at, from);
    for (int k = 0; k < 6; ++k) {
      if (d2 >= nearest[k].d2) continue;
      for (int m = 5; m > k; --m) nearest[m] = nearest[m - 1];
      nearest[k] = {d2, e};
      break;
    }
  }
  if (g_pw.pick == 0) target = nearest[0].ent;
  if (!target) {
    if (g_pw.tries++ % 600 == 0)
      REXLOG_INFO("cod3: pose world: frame {}: no animated model {} yet", g_frame_number,
                  g_pw.pick > 0 ? fmt::format("numbered {}", g_pw.pick) : std::string("near the center"));
    return false;
  }
  std::string near_list;
  for (const Near& n : nearest)
    if (n.ent) near_list += fmt::format(" [{}]", PwDescribe(base, n.ent, from));
  g_pw.center = center;
  g_pw.target = target;
  g_pw.center_number = g::ReadS32(base, center + kEntNumber);
  g_pw.target_number = g::ReadS32(base, target + kEntNumber);
  REXLOG_INFO("cod3: pose world: frame {}: center ({}): {}; the vehicles in the client game's list ({}):{}",
              g_frame_number, g_pw.center_ask < 0 ? "the vehicle nearest the player" : "the entity asked for",
              PwDescribe(base, center, eye), found, vehicles.empty() ? std::string(" none") : vehicles);
  REXLOG_INFO("cod3: pose world: the animated models nearest the center:{}", near_list);
  // [rexglue-native npc] 2026-11-09 second pass (npc6: the tank beside the soldiers at mace2's start is no vehicle entity
  // [rexglue-native npc] and no animated model near them): every entity of the client game's list within 600 units of
  // [rexglue-native npc] the center (the player's too), whatever its type, so the tank can be named by its number.
  std::string around;
  int around_n = 0;
  for (uint32_t i = 0; i < listed && around_n < 40; ++i) {
    const uint32_t e = g::ReadU32(base, kEntList + 4 * i);
    if (!ValidGuest(e) || e == center) continue;
    float at[3];
    ReadVec(base, e + kEntServerOrigin, at);
    if (Dist2(at, from) > 600.0f * 600.0f) continue;
    ++around_n;
    around += fmt::format(" [{}{}]", e == player ? "the player: " : "", PwDescribe(base, e, from));
  }
  REXLOG_INFO("cod3: pose world: the client game's entities within 600 units of the center ({}):{}", around_n, around);
  REXLOG_INFO("cod3: pose world: traced ({}): {}", g_pw.pick > 0 ? "the entity asked for" : "the nearest",
              PwDescribe(base, target, from));
  const uint32_t actor = g::ReadU32(base, target + kPwActor);
  REXLOG_INFO("cod3: pose world: where the traced entity holds the center ({:08x}, number {}):{}; its actor ({:08x}):{}; "
              "where the center holds the traced one ({:08x}, number {}):{}",
              center, g_pw.center_number, PwLinks(base, target, kPwEntScan, center, g_pw.center_number), actor,
              ValidGuest(actor) ? PwLinks(base, actor, kPwActorScan, center, g_pw.center_number) : std::string(" none"),
              target, g_pw.target_number, PwLinks(base, center, kPwEntScan, target, g_pw.target_number));
  return true;
}

// [rexglue-native npc] Once a drawn frame, after the client game's frame (CG_DrawActiveFrame's hook).
void PoseWorldLines(uint8_t* base) {
  const int32_t center_ask = REXCVAR_GET(cod3_debug_pose_bones_near);
  const int32_t asked = REXCVAR_GET(cod3_debug_pose_bones);
  const int32_t pick = REXCVAR_GET(cod3_debug_pose_bones_pick);
  if (center_ask == 0 || asked <= 0) {
    if (g_pw.asked) g_pw = PoseWorld{};
    return;
  }
  if (asked != g_pw.asked || center_ask != g_pw.center_ask || pick != g_pw.pick) {  // set again: start again
    g_pw = PoseWorld{};
    g_pw.asked = g_pw.left = asked;
    g_pw.center_ask = center_ask;
    g_pw.pick = pick;
  }
  if (g_pw.done || g_pw.left <= 0) return;
  const uint32_t player = PlayerEntity(base);
  if (!ValidGuest(player) || !ValidGuest(g::ReadU32(base, kAnimatedList))) return;
  // [rexglue-native npc] chosen after the level's first 30 drawn frames: the scripts place the soldiers in its first
  // [rexglue-native npc] updates (npc4: jumps of 22-60 units in frames 534-550, the level's first 20 frames)
  if (!g_pw.center && ++g_pw.settle <= 30) return;
  if (!g_pw.center && !PwChoose(base, player)) return;
  if (g::ReadS32(base, g_pw.center + kEntNumber) != g_pw.center_number ||
      g::ReadS32(base, g_pw.target + kEntNumber) != g_pw.target_number) {
    REXLOG_INFO("cod3: pose world: frame {}: entity {} or {} is gone; the trace ends", g_frame_number,
                g_pw.center_number, g_pw.target_number);
    g_pw.done = true;
    return;
  }
  --g_pw.left;
  PwPlace c, t;
  PwRead(base, g_pw.center, c);
  PwRead(base, g_pw.target, t);
  float axis[3][3];
  AnglesToAxis(c.angles, axis);
  float off_server[3], off_node[3] = {};
  PwInFrame(t.server, c.server, axis, off_server);
  if (c.node_ok && t.node_ok) PwInFrame(t.node, c.node, c.axis, off_node);
  const uint32_t model = g::ReadU32(base, g_pw.target + kPwModel);
  const uint32_t buf = ValidGuest(model) ? g::ReadU32(base, model + 0x70) : 0;
  const uint32_t n = ValidGuest(buf) && t.node_ok ? std::min<uint32_t>(g::ReadU8(base, model + 0xAB), 200) : 0;
  std::string text = fmt::format(
      "frame {} updates {} back {:.3f} counter {} | center {} server {} angles {} drawn {} node {} axis {} {} {} "
      "listed {} | model {} server {} angles {} drawn {} drawn_angles {} node {} axis {} {} {} listed {} flags {:08x} "
      "rides {} stamp {} undrawn {} | offset server {} node {} | {} bones:",
      g_frame_number, g_draw_updates, Back(), g::ReadU32(base, kPwAnimCounter), g_pw.center_number, PwVec(c.server),
      PwVec(c.angles), PwVec(c.drawn), PwVec(c.node), PwVec(c.axis[0], 4), PwVec(c.axis[1], 4), PwVec(c.axis[2], 4),
      int(c.listed), g_pw.target_number, PwVec(t.server), PwVec(t.angles), PwVec(t.drawn), PwVec(t.drawn_angles),
      PwVec(t.node), PwVec(t.axis[0], 4), PwVec(t.axis[1], 4), PwVec(t.axis[2], 4), int(t.listed),
      g::ReadU32(base, g_pw.target + kEntFlags), PwRides(base, g_pw.target),
      ValidGuest(model) ? g::ReadU32(base, model + kPwModelStamp) : 0u,
      ValidGuest(model) ? (g::ReadU32(base, model + kPwModelArmed) & 1u) : 0u, PwVec(off_server), PwVec(off_node), n);
  text.reserve(text.size() + n * 26);
  for (uint32_t b = 0; b < n; ++b) {
    float o[3], w[3];
    ReadVec(base, buf + 0x30 + 0x40 * b + 0x30, o);
    for (int k = 0; k < 3; ++k) w[k] = o[0] * t.axis[0][k] + o[1] * t.axis[1][k] + o[2] * t.axis[2][k] + t.node[k];
    text += fmt::format(" {:.2f},{:.2f},{:.2f}", w[0], w[1], w[2]);
  }
  REXLOG_INFO("cod3: pose world: {}", text);
}

// [rexglue-native-bugs] 2026-11-11 nl1 cod3_debug_near_entities (see the setting): once a drawn frame, after the client
// [rexglue-native-bugs] game's frame. Each entity near the camera: its scene node's place, frame by frame, to count the
// [rexglue-native-bugs] frames it moved in with and without a game update (a thing drawn only at its update pose moves in
// [rexglue-native-bugs] update frames alone); once a second (game time) one line each, nearest first.
struct NearTrack {
  float node[3] = {};
  bool have = false;
  uint32_t moved_update = 0, moved_between = 0, frames_update = 0, frames_between = 0;
  int64_t seen = 0;  // the game time it was last near the camera
};
std::unordered_map<int32_t, NearTrack> g_near;  // by entity number
int64_t g_near_next = 0;

const char* NodeFrom(const float node[3], const float server[3], const float drawn[3], const float trbase[3]) {
  if (Same(node, server)) return "server";
  if (Same(node, drawn)) return "drawn";
  if (Same(node, trbase)) return "base";
  return "other";
}

void NearEntitiesLines(uint8_t* base) {
  const int32_t radius = REXCVAR_GET(cod3_debug_near_entities);
  if (radius <= 0) {
    if (!g_near.empty()) g_near.clear();
    return;
  }
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  if (local > 3) return;
  float cam[3];
  ReadVec(base, kCgBase + local * kCgSize + kCgViewOrigin, cam);
  const bool update_frame = (g_kind ? g_draw_updates : 0) > 0;
  const bool log_now = g_clock.time >= g_near_next;
  struct Line {
    float d2;
    std::string text;
  };
  std::vector<Line> lines;
  const uint32_t listed = std::min<uint32_t>(g::ReadU32(base, kEntListCount), 0x4000);
  for (uint32_t i = 0; i < listed; ++i) {
    const uint32_t e = g::ReadU32(base, kEntList + 4 * i);
    if (!ValidGuest(e)) continue;
    float server[3];
    ReadVec(base, e + kEntServerOrigin, server);
    const float d2 = Dist2(server, cam);
    if (d2 > float(radius) * float(radius)) continue;
    const uint32_t node = g::ReadU32(base, e + kEntSceneNode);
    if (!ValidGuest(node)) continue;
    const int32_t number = g::ReadS32(base, e + kEntNumber);
    float node_o[3];
    ReadVec(base, node + kNodeOrigin, node_o);
    NearTrack& t = g_near[number];
    if (t.have) {
      if (!Same(node_o, t.node)) ++(update_frame ? t.moved_update : t.moved_between);
      ++(update_frame ? t.frames_update : t.frames_between);
    }
    for (int k = 0; k < 3; ++k) t.node[k] = node_o[k];
    t.have = true;
    t.seen = g_clock.time;
    if (!log_now) continue;
    float drawn[3], trbase[3];
    ReadVec(base, e + kEntDrawnOrigin, drawn);
    ReadVec(base, e + kEntTrBase, trbase);
    const uint32_t model = g::ReadU32(base, e + kPwModel);
    const uint32_t bones = ValidGuest(model) && ValidGuest(g::ReadU32(base, model + 0x70)) ? g::ReadU8(base, model + 0xAB) : 0;
    lines.push_back(Line{d2, fmt::format("num {} type {} tr {} flags {:08x} rides {} bones {} dist {:.0f} server {} "
                                         "drawn {} node {} from {} off {:.2f} moved {}/{} with an update, {}/{} without",
                                         number, g::ReadU8(base, e), g::ReadS32(base, e + kEntPosType),
                                         g::ReadU32(base, e + kEntFlags), PwRides(base, e), bones, std::sqrt(d2),
                                         PwVec(server), PwVec(drawn), PwVec(node_o),
                                         NodeFrom(node_o, server, drawn, trbase), std::sqrt(Dist2(node_o, server)),
                                         t.moved_update, t.frames_update, t.moved_between, t.frames_between)});
    t.moved_update = t.moved_between = t.frames_update = t.frames_between = 0;
  }
  if (!log_now) return;
  g_near_next = g_clock.time + 1000;
  for (auto it = g_near.begin(); it != g_near.end();)  // forget what left the camera's surroundings
    it = g_clock.time - it->second.seen > 5000 ? g_near.erase(it) : std::next(it);
  std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.d2 < b.d2; });
  REXLOG_INFO("cod3: near entities: time {} camera {} mode {} player {}: {} within {} units", g_clock.time, PwVec(cam),
              g_cam_mode, g::ReadS32(base, PlayerEntity(base) + kEntNumber), lines.size(), radius);
  for (size_t k = 0; k < lines.size() && k < 48; ++k) REXLOG_INFO("cod3: near entity {}", lines[k].text);
}

// [claude-9b] 2026-10-27 The game's values back, each only while it still holds the value drawn (game
// code that moved or turned the entity since, a script's teleport or an animation's alignment, keeps
// its own: owner 10-27 03:10 / 03:11, the Saint-Lo guide stuck at the crate above 60 fps, "this fix
// will likely need to be applied any time an npc needs to be in a particular position"). Called before
// any of the frame's game code runs: the frame's systems (0x823FFC10), its usercmds and its updates.
void PutBackOne(uint8_t* base, const Saved& s, uint32_t at) {
  float now[3];
  ReadVec(base, at, now);
  if (Same(now, s.drawn)) WriteVec(base, at, s.origin);
}
void PutBackDrawn(uint8_t* base) {
  cod3::movement::EndDrawnStepValues(base);  // [claude-9b] 2026-10-27 the aim and lean (movement/hooks_sp.cpp)
  cod3::ride_bones::Restore(base);  // [claude-9b-think] cod3_blend_ride_bones: the update's turret and gun bones back
  for (const Saved& s : g_saved) PutBackOne(base, s, s.ent + kEntDrawnOrigin);
  g_saved.clear();
  for (const Saved& s : g_saved_angles) PutBackOne(base, s, s.ent + kEntDrawnAngles);
  g_saved_angles.clear();
}

void Forget() {
  g_history.clear();
  for (HistorySlot& s : g_history_flat) s = HistorySlot{};  // [acct2-cod3-motion]
  g_player = History{};
  g_view = ViewHistory{};
  g_ride = RideHistory{};         // [claude-9b-think]
  cod3::ride_bones::Forget();     // [claude-9b-think] cod3_blend_ride_bones
  g_ride_before = RideBefore{};
  g_body.clear();                 // [claude-9b-think] cod3_server_smooth_body
  g_saved.clear();
  g_saved_angles.clear();
}

// [claude-9b] 2026-10-27 The shortest turn from one angle to another, in degrees (-180, 180].
float Turn(float from, float to) {
  float d = std::fmod(to - from, 360.0f);
  if (d > 180.0f) d -= 360.0f;
  if (d <= -180.0f) d += 360.0f;
  return d;
}

// Angles a part `back` of the way from cur back to prev, each the short way round; false when a turn
// is too big for one update (a cut).
bool BlendAngles(const float prev[3], const float cur[3], float back, float out[3]) {
  for (int i = 0; i < 3; ++i) {
    const float d = Turn(prev[i], cur[i]);
    if (std::fabs(d) >= kMaxSmoothedTurn) return false;
    out[i] = cur[i] - d * back;
  }
  return true;
}

// The player's client, or 0.
uint32_t PlayerClient(uint8_t* base) {
  const uint32_t player = PlayerEntity(base);
  if (!ValidGuest(player)) return 0;
  const uint32_t client = g::ReadU32(base, player + kEntClient);
  return ValidGuest(client) ? client : 0;
}

// True while the player rides a vehicle: the client's vehicle handle names a live entity with a
// vehicle's data, as 0x82447D48 tests before it picks a vehicle's camera.
// [claude-9b-think] 2026-10-27 UNTESTED the test as before, giving the vehicle (0: none).
uint32_t RiddenVehicle(uint8_t* base, uint32_t client) {
  const uint32_t h = g::ReadU32(base, client + kClientVehicle);
  const uint32_t index = h & 0x3FFF;
  if (index >= 4096) return 0;
  const uint32_t slot = kEntityHandles + 8 * index;
  if (g::ReadU32(base, slot + 4) != (h >> 14)) return 0;
  const uint32_t ent = g::ReadU32(base, slot);
  return ValidGuest(ent) && ValidGuest(g::ReadU32(base, ent + kEntVehicleInfo)) ? ent : 0;
}
bool RidesVehicle(uint8_t* base, uint32_t client) { return RiddenVehicle(base, client) != 0; }

// After an update: the player's view angles as it left them.
void AdvanceView(uint8_t* base) {
  const uint32_t client = PlayerClient(base);
  if (!client) {
    g_view.valid = false;
    return;
  }
  float now[3];
  ReadVec(base, client + kClientViewAngles, now);
  const bool next = g_view.valid && g_view.update + 1 == g_clock.updates;
  for (int i = 0; i < 3; ++i) {
    g_view.prev[i] = next ? g_view.cur[i] : now[i];
    g_view.cur[i] = now[i];
  }
  g_view.update = g_clock.updates;
  g_view.valid = true;
}

// The view angles to draw this frame (cod3_server_smooth_view): false to leave them. `at` is where the
// game keeps them and `now` their value.
bool ViewAnglesToDraw(uint8_t* base, uint32_t& at, float now[3], float out[3]) {
  if (g_clock.hz <= 0 || !REXCVAR_GET(cod3_server_smooth) || !REXCVAR_GET(cod3_server_smooth_view)) return false;
  const uint32_t client = PlayerClient(base);
  if (!client || !g_view.valid || g_view.update != g_clock.updates) return false;
  at = client + kClientViewAngles;
  ReadVec(base, at, now);
  if (!Same(now, g_view.cur)) {  // turned since the update: by the player, every frame
    g_view.turned_between = true;
    g_view.turned_between_at = g_clock.updates;
    return false;
  }
  if (g_view.turned_between && g_clock.updates - g_view.turned_between_at <= 1) return false;
  // [claude-9b-think] 2026-10-28 UNTESTED a mounted gun too (cod3_server_smooth_turret).
  const bool on_gun = REXCVAR_GET(cod3_server_smooth_turret) && (g::ReadU32(base, client + kClientPsFlags) & kPsOnAGun);
  if (!(on_gun || RidesVehicle(base, client)) || Same(g_view.prev, g_view.cur)) return false;
  const float back = Back();
  if (back <= 0.0f) return false;
  return BlendAngles(g_view.prev, g_view.cur, back, out);
}

// The drawn angles of an entity after the game's own code (cod3_server_smooth): between the angles it
// had at the last two updates while the game leaves them as the update set them (0x82438B68 blends
// them by cg +2800; where that moves them between updates, they are left to it).
void SmoothDrawnAngles(uint8_t* base, uint32_t ent, History& h, float back) {
  float drawn[3];
  ReadVec(base, ent + kEntDrawnAngles, drawn);
  if (h.angles_update != g_clock.updates) {  // the first frame since an update: that update's angles
    const bool next = h.angles_valid && h.angles_update + 1 == g_clock.updates;
    for (int i = 0; i < 3; ++i) {
      h.angles_prev[i] = next ? h.angles_cur[i] : drawn[i];
      h.angles_cur[i] = drawn[i];
    }
    h.angles_update = g_clock.updates;
    h.angles_valid = true;
  } else if (!Same(drawn, h.angles_cur)) {  // the game turned it since the update: its own blend
    for (int i = 0; i < 3; ++i) h.angles_prev[i] = h.angles_cur[i] = drawn[i];
    return;
  }
  if (back <= 0.0f || Same(h.angles_prev, h.angles_cur)) return;
  float out[3];
  if (!BlendAngles(h.angles_prev, h.angles_cur, back, out)) return;
  g_saved_angles.push_back(Saved{ent, {drawn[0], drawn[1], drawn[2]}, {out[0], out[1], out[2]}});
  WriteVec(base, ent + kEntDrawnAngles, out);
  g::WriteU32(base, ent + kEntFlags, g::ReadU32(base, ent + kEntFlags) | kMovedFlag);
  ++g_counts.turned;
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_ride: the ridden vehicle and its pose at the last two
// updates. The ride is on while the setting is (with cod3_server_hz and cod3_server_smooth).
bool RideOn() {
  return g_clock.hz > 0 && REXCVAR_GET(cod3_server_smooth) && REXCVAR_GET(cod3_server_smooth_ride) > 0;
}

// [claude-9b-think] 2026-10-27 UNTESTED A camera mode of 0x82447D48 the ride applies to (cod3_server_smooth_ride_cams).
bool RideCameraMode(uint32_t mode) {
  return mode < 23 && ((uint32_t(REXCVAR_GET(cod3_server_smooth_ride_cams)) >> mode) & 1u) != 0;
}

// Before an update: the player's view angles, delta_angles and origin, for the turn and the move the update gives them.
void RideBeforeUpdate(uint8_t* base) {
  g_ride_before.have = false;
  const uint32_t client = PlayerClient(base);
  // [claude-9b-think] kept with the ride off too (cod3_debug_view_trace compares 0 with 1); used only while it is on.
  if (!client || g_clock.hz <= 0 || !RidesVehicle(base, client)) return;
  ReadVec(base, client + kClientViewAngles, g_ride_before.view);
  for (int i = 0; i < 3; ++i) g_ride_before.delta[i] = g::ReadS32(base, client + kClientDeltaAngles + 4 * i);
  ReadVec(base, client + kClientOrigin, g_ride_before.origin);
  g_ride_before.have = true;
}

// After an update (next to AdvanceView): the ridden vehicle's origin and angles as the update left them.
void AdvanceRide(uint8_t* base) {
  const uint32_t client = PlayerClient(base);
  const uint32_t veh = client && g_clock.hz > 0 ? RiddenVehicle(base, client) : 0;  // [claude-9b-think] any setting
  if (!veh) {
    g_ride.have = g_ride.valid = false;
    g_ride.ent = 0;
    g_ride_before.have = false;
    return;
  }
  float origin[3], angles[3];
  ReadVec(base, veh + kEntServerOrigin, origin);
  ReadVec(base, veh + kEntServerAngles, angles);  // [claude-9b-think] was +84 (the client's blend end)
  const int32_t number = g::ReadS32(base, veh + kEntNumber);
  const bool next = g_ride.have && g_ride.ent == veh && g_ride.number == number && g_ride.update + 1 == g_clock.updates;
  for (int i = 0; i < 3; ++i) {
    g_ride.origin_prev[i] = next ? g_ride.origin_cur[i] : origin[i];
    g_ride.angles_prev[i] = next ? g_ride.angles_cur[i] : angles[i];
    g_ride.origin_cur[i] = origin[i];
    g_ride.angles_cur[i] = angles[i];
  }
  bool close_enough = Dist2(g_ride.origin_prev, g_ride.origin_cur) < kMaxSmoothedMove * kMaxSmoothedMove;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(Turn(g_ride.angles_prev[i], g_ride.angles_cur[i])) >= kMaxSmoothedTurn) close_enough = false;
  // The view's turn in the update: its angles' own turn, or (where the update set only delta_angles, which the
  // frame's usercmd then adds to the view before the frame is drawn) delta_angles' turn.
  for (int i = 0; i < 3; ++i) g_ride.view_turn[i] = g_ride.delta_turn[i] = g_ride.seat_jump[i] = 0.0f;
  if (g_ride_before.have && next) {
    float view[3], seat[3];
    ReadVec(base, client + kClientViewAngles, view);
    ReadVec(base, client + kClientOrigin, seat);
    for (int i = 0; i < 3; ++i) {
      const int32_t delta = g::ReadS32(base, client + kClientDeltaAngles + 4 * i);
      const int16_t dd = int16_t(uint16_t(uint32_t(delta - g_ride_before.delta[i])));
      g_ride.delta_turn[i] = float(dd) * (360.0f / 65536.0f);
      const float turn = Turn(g_ride_before.view[i], view[i]);
      g_ride.view_turn[i] = turn != 0.0f ? turn : g_ride.delta_turn[i];
      if (std::fabs(g_ride.view_turn[i]) >= kMaxSmoothedTurn) g_ride.view_turn[i] = 0.0f;  // a cut
      g_ride.seat_jump[i] = seat[i] - g_ride_before.origin[i];
    }
  }
  g_ride_before.have = false;
  g_ride.ent = veh;
  g_ride.number = number;
  g_ride.update = g_clock.updates;
  g_ride.have = true;
  g_ride.valid = next && close_enough;
}

// The ridden vehicle's pose to draw this frame: between its last two updates by Back(), as the other moving things.
// False when it isn't the vehicle of the last update's ride or the game moved it since the update.
bool RideDrawnPose(uint8_t* base, uint32_t veh, float origin[3], float angles[3]) {
  if (!RideOn() || !g_ride.valid || g_ride.ent != veh || g_ride.update != g_clock.updates) return false;
  float now[3];
  ReadVec(base, veh + kEntServerOrigin, now);
  if (!Same(now, g_ride.origin_cur)) return false;
  const float back = Back();
  for (int i = 0; i < 3; ++i) {
    origin[i] = g_ride.origin_cur[i] + (g_ride.origin_prev[i] - g_ride.origin_cur[i]) * back;
    angles[i] = g_ride.angles_cur[i] - Turn(g_ride.angles_prev[i], g_ride.angles_cur[i]) * back;
  }
  return true;
}

// Forward, left and up of angles (pitch, yaw, roll in degrees), as the game's AnglesToAxis.
void AnglesToAxis(const float a[3], float axis[3][3]) {
  constexpr float kRad = 3.14159265358979f / 180.0f;
  const float sp = std::sin(a[0] * kRad), cp = std::cos(a[0] * kRad);
  const float sy = std::sin(a[1] * kRad), cy = std::cos(a[1] * kRad);
  const float sr = std::sin(a[2] * kRad), cr = std::cos(a[2] * kRad);
  axis[0][0] = cp * cy;                 axis[0][1] = cp * sy;                 axis[0][2] = -sp;
  axis[1][0] = sr * sp * cy - cr * sy;  axis[1][1] = sr * sp * sy + cr * cy;  axis[1][2] = sr * cp;
  axis[2][0] = cr * sp * cy + sr * sy;  axis[2][1] = cr * sp * sy - sr * cy;  axis[2][2] = cr * cp;
}

// The view angles to draw on a ridden vehicle when ViewAnglesToDraw leaves them (the player turns the view every
// frame): the game's angles turned back by the part of the update's turn not drawn yet, so the view turns with the
// vehicle as it is drawn and the player's own turning is kept: the view's own turn in the update. [claude-9b-think]
// (Setting 2, the vehicle's yaw turn alone, is dropped: claude-9b-test's ride4 found its view turn uneven, the
// update frames 1.5-2.1x the others; the view's measured turn is all of what the update did to it.)
bool RideViewAngles(uint8_t* base, uint32_t& at, float now[3], float out[3]) {
  if (!RideOn() || !REXCVAR_GET(cod3_server_smooth_view)) return false;
  const uint32_t client = PlayerClient(base);
  // [claude-9b-think] the last frame's camera mode (this frame's is picked inside 0x824505E8): the ride's modes only.
  if (!client || !RideCameraMode(g_cam_mode) || RiddenVehicle(base, client) != g_ride.ent) return false;
  if (!g_ride.valid || g_ride.update != g_clock.updates) return false;
  float turn[3] = {0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 3; ++i) turn[i] = g_ride.view_turn[i];
  if (turn[0] == 0.0f && turn[1] == 0.0f && turn[2] == 0.0f) return false;
  const float back = Back();
  if (back <= 0.0f) return false;
  at = client + kClientViewAngles;
  ReadVec(base, at, now);
  for (int i = 0; i < 3; ++i) out[i] = now[i] - turn[i] * back;
  return true;
}

// The ridden vehicle drawn from the ride's pose (CG_CalcEntityLerpPositions' hook), so it and the camera agree;
// false to leave it to the other smoothing.
// [claude-9b-think] 2026-10-27 ride4 had drawn 0 in every frame: the hook called it for TR_INTERPOLATE only (the
// hostage ride's vehicle is most likely drawn at its base, +32: the trace's ptype says), and it left a vehicle the game
// drew anywhere but at its update position. Where the other smoothing draws it, that is the same blend of the same
// values (its base is its origin at each update, its drawn angles the update's, y352 = y72), so this makes the two
// agree by construction; where the game places it itself, only this does. g_ride_trace_draw_why: why not.
// [claude-9b-think] 2026-10-27 UNTESTED ride6: called before the hook's trajectory-type test, so for any type; where
// the game drew it somewhere else than its update position, drawn from the ride's pose anyway with
// cod3_server_smooth_ride_vehicle (reason 6), since the camera is placed on that pose.
// [claude-9b-think] 2026-10-27 ride9: a vehicle (type 14) is not drawn from what this writes (+112 / +128): its draw
// takes +336 / +352 (0x82442338), which is why ride8's pictures kept the body at the update's pose although the trace
// (made from +112) passed. The body is placed by cod3_server_smooth_body (PlaceVehicleBody); this is kept for whatever
// else reads the drawn origin.
bool RideDrawVehicle(uint8_t* base, uint32_t ent, int32_t pos_type) {
  if (!RideOn() || ent != g_ride.ent) return false;  // (no reason set: the hook calls this for every thing drawn)
  const bool stationary = pos_type == kTrStationary;
  const uint32_t client = PlayerClient(base);
  if (!client || RiddenVehicle(base, client) != ent) return g_ride_trace_draw_why = 2, false;
  float origin[3], angles[3];
  if (!RideDrawnPose(base, ent, origin, angles)) return g_ride_trace_draw_why = 3, false;
  float drawn[3], now[3];
  ReadVec(base, ent + kEntDrawnOrigin, drawn);
  ReadVec(base, ent + (stationary ? kEntTrBase : kEntServerOrigin), now);
  // Not the plain copy the game makes: left (as the other smoothing does) with cod3_server_smooth_ride_vehicle off.
  if (!Same(drawn, now)) {
    if (!REXCVAR_GET(cod3_server_smooth_ride_vehicle)) return g_ride_trace_draw_why = 4, false;
    g_ride_trace_draw_why = 6;
  } else {
    g_ride_trace_draw_why = 0;
  }
  g_saved.push_back(Saved{ent, {drawn[0], drawn[1], drawn[2]}, {origin[0], origin[1], origin[2]}});
  WriteVec(base, ent + kEntDrawnOrigin, origin);
  float game_angles[3];
  ReadVec(base, ent + kEntDrawnAngles, game_angles);
  // [claude-9b-think] 2026-10-27 Only while the game draws it at (about) the ride's angles of the last update: the
  // ride read +84 at first, which is 0 for the hostage level's vehicle (claude-9b-test), and would have drawn it at
  // yaw 0. Its drawn angles are 0x82438B68's blend of +72 (pitch +72, yaw +76, roll +80: the update's) toward +84 by
  // cg +2800, 0 in the campaign; the ride's are the server's +352, the same values at the update.
  bool angles_ok = true;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(Turn(game_angles[i], g_ride.angles_cur[i])) >
        std::fabs(Turn(g_ride.angles_prev[i], g_ride.angles_cur[i])) + 1.0f)
      angles_ok = false;
  if (angles_ok) {
    g_saved_angles.push_back(
        Saved{ent, {game_angles[0], game_angles[1], game_angles[2]}, {angles[0], angles[1], angles[2]}});
    WriteVec(base, ent + kEntDrawnAngles, angles);
  } else {  // [claude-9b-think] the drawn angles as for any other moving thing (the hook's own path)
    History& h = HistoryOf(base, ent);  // [acct2-cod3-motion] the flat table
    const int32_t number = g::ReadS32(base, ent + kEntNumber);
    if (h.number != number) {
      h = History{};
      h.number = number;
    }
    SmoothDrawnAngles(base, ent, h, Back());
  }
  g::WriteU32(base, ent + kEntFlags, g::ReadU32(base, ent + kEntFlags) | kMovedFlag);
  g_ride_trace_drawn = true;
  g_ride_trace_angles_ok = angles_ok;
  g_ride_trace_yaw = angles_ok ? angles[1] : game_angles[1];
  ++g_counts.smoothed;
  return true;
}

// [claude-9b-think] 2026-10-27 UNTESTED Angles (pitch, yaw, roll) of an axis (forward, left, up), each the nearest to
// `near_to`'s (AnglesToAxis' inverse).
void AxisToAngles(const float axis[3][3], const float near_to[3], float out[3]) {
  constexpr float kDeg = 180.0f / 3.14159265358979f;
  const float* f = axis[0];
  const float yaw = std::atan2(f[1], f[0]);
  const float pitch = std::atan2(-f[2], std::sqrt(f[0] * f[0] + f[1] * f[1]));
  const float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
  const float left0[3] = {-sy, cy, 0.0f}, up0[3] = {sp * cy, sp * sy, cp};
  const float* l = axis[1];
  const float roll = std::atan2(l[0] * up0[0] + l[1] * up0[1] + l[2] * up0[2], l[0] * left0[0] + l[1] * left0[1]);
  const float a[3] = {pitch * kDeg, yaw * kDeg, roll * kDeg};
  for (int i = 0; i < 3; ++i) out[i] = near_to[i] + Turn(near_to[i], a[i]);
}

// [claude-9b-think] 2026-10-27 UNTESTED After 0x824505E8 on a ridden vehicle: a camera turned by the vehicle's pose of
// the update (a seat's tag, 0x824457F0 without free look; a driver's view by the vehicle's angles) is turned as the
// vehicle is drawn, its angles (cg +4544) and its axis (+4496 forward, +4508 left, +4520 up, from the angles at the
// end of 0x824505E8). A camera turned by the player's view angles (the eye, the seat, free look: those are drawn
// between updates by ViewAnglesToDraw / RideViewAngles) is left. Its place moves in SmoothView (RideViewOffset).
constexpr uint32_t kCgViewAxis = 4496;  // forward, left, up (3 x vec3)
// The player's entity +916: non-zero, the seat camera follows the view angles (0x824457F0 tests it for zero).
// [claude-9b-think] ride4: a pointer (0xAA344D30), set in every frame of the hostage ride: that seat's camera takes the
// player's view angles (RideViewAngles draws those between updates), so this turn rightly does nothing there.
constexpr uint32_t kEntSeatFreeLook = 916;
bool g_ride_trace_turned = false;      // cod3_debug_view_trace
// The seat cameras (0x824457F0): 6 / 13 the first seat tag, 7 the same raised, 8 a passenger's tag. The driver's
// views (2, 3) are not turned here until a trace shows where their angles come from (the trace's cam / view yaw).
bool TagCameraMode(uint32_t mode) { return mode == 6 || mode == 7 || mode == 8 || mode == 13; }
void RideTurnCamera(uint8_t* base) {
  g_ride_trace_turned = false;
  if (!RideOn() || !RideCameraMode(g_cam_mode) || !TagCameraMode(g_cam_mode)) return;
  const uint32_t client = PlayerClient(base);
  const uint32_t veh = client ? RiddenVehicle(base, client) : 0;
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  if (!veh || local > 3) return;
  // 0x824457F0 takes the player's view angles instead of the tag's when the player's entity +916 is set (a seat
  // that lets the player look around): those are drawn between updates already (RideViewAngles).
  const uint32_t player = PlayerEntity(base);
  if (!ValidGuest(player) || g::ReadU32(base, player + kEntSeatFreeLook) != 0) return;
  const uint32_t cg = kCgBase + local * kCgSize;
  float cam_angles[3], view[3];
  ReadVec(base, cg + kCgViewAngles, cam_angles);
  ReadVec(base, client + kClientViewAngles, view);
  bool from_view = true;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(Turn(view[i], cam_angles[i])) > 0.01f) from_view = false;
  if (from_view) return;
  float origin[3], angles[3];
  if (!RideDrawnPose(base, veh, origin, angles)) return;
  float cur_axis[3][3], drawn_axis[3][3], cam_axis[3][3], out_axis[3][3];
  AnglesToAxis(g_ride.angles_cur, cur_axis);
  AnglesToAxis(angles, drawn_axis);
  for (int a = 0; a < 3; ++a) ReadVec(base, cg + kCgViewAxis + 12 * a, cam_axis[a]);
  // Each axis vector in the vehicle's frame of the update, put back out in the frame it is drawn in.
  for (int a = 0; a < 3; ++a) {
    float local_v[3];
    for (int i = 0; i < 3; ++i)
      local_v[i] = cur_axis[i][0] * cam_axis[a][0] + cur_axis[i][1] * cam_axis[a][1] + cur_axis[i][2] * cam_axis[a][2];
    for (int k = 0; k < 3; ++k)
      out_axis[a][k] = local_v[0] * drawn_axis[0][k] + local_v[1] * drawn_axis[1][k] + local_v[2] * drawn_axis[2][k];
  }
  float out[3];
  AxisToAngles(out_axis, cam_angles, out);
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(out[i]) || std::fabs(Turn(cam_angles[i], out[i])) >= kMaxSmoothedTurn) return;
  for (int a = 0; a < 3; ++a)
    for (int k = 0; k < 3; ++k)
      if (!std::isfinite(out_axis[a][k])) return;
  WriteVec(base, cg + kCgViewAngles, out);
  for (int a = 0; a < 3; ++a) WriteVec(base, cg + kCgViewAxis + 12 * a, out_axis[a]);
  g_ride_trace_turned = true;
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_body (see the setting and the note at the top of the file).
int BodySetting() {
  if (g_clock.hz <= 0 || !REXCVAR_GET(cod3_server_smooth)) return 0;
  return REXCVAR_GET(cod3_server_smooth_body);
}

// An entity handle's entity (0: none), as RiddenVehicle and the game's draws read them.
uint32_t EntityOfHandle(uint8_t* base, uint32_t h) {
  const uint32_t index = h & 0x3FFF;
  if (index >= 4096) return 0;
  const uint32_t slot = kEntityHandles + 8 * index;
  if (g::ReadU32(base, slot + 4) != (h >> 14)) return 0;
  const uint32_t ent = g::ReadU32(base, slot);
  return ValidGuest(ent) ? ent : 0;
}

// Where an entity the game draws at its server pose is drawn this frame. `cur` its server origin (+336) and angles
// (+352), which the game's draw filled its scene node from; `out` the pose to draw. 2: the ride's pose (the ridden
// vehicle with cod3_server_smooth_ride: the pose the camera is placed on); 1: between its last two updates by Back(), as
// the other moving things; 0: leave it (no update before, nothing moved, a cut, or the game moved it since the update).
int BodyPose(uint8_t* base, uint32_t ent, float cur_o[3], float cur_a[3], float out_o[3], float out_a[3]) {
  float now_o[3], now_a[3];
  ReadVec(base, ent + kEntServerOrigin, now_o);
  ReadVec(base, ent + kEntServerAngles, now_a);
  const int32_t number = g::ReadS32(base, ent + kEntNumber);
  BodyHistory& h = g_body[ent];
  if (h.number != number) {  // another entity in this slot now
    h = BodyHistory{};
    h.number = number;
  }
  if (!h.valid || h.update != g_clock.updates) {  // the first draw since an update: that update's pose
    const bool next = h.valid && h.update + 1 == g_clock.updates;
    for (int i = 0; i < 3; ++i) {
      h.origin_prev[i] = next ? h.origin_cur[i] : now_o[i];
      h.angles_prev[i] = next ? h.angles_cur[i] : now_a[i];
      h.origin_cur[i] = now_o[i];
      h.angles_cur[i] = now_a[i];
    }
    h.update = g_clock.updates;
    h.valid = true;
  } else if (!Same(now_o, h.origin_cur) || !Same(now_a, h.angles_cur)) {  // moved by the game since: its own place
    for (int i = 0; i < 3; ++i) {
      h.origin_prev[i] = h.origin_cur[i] = now_o[i];
      h.angles_prev[i] = h.angles_cur[i] = now_a[i];
    }
    return 0;
  }
  for (int i = 0; i < 3; ++i) {
    cur_o[i] = now_o[i];
    cur_a[i] = now_a[i];
  }
  if (RideOn() && ent == g_ride.ent && Same(now_a, g_ride.angles_cur) && RideDrawnPose(base, ent, out_o, out_a))
    return 2;
  const float back = Back();
  if (back <= 0.0f || (Same(h.origin_prev, h.origin_cur) && Same(h.angles_prev, h.angles_cur))) return 0;
  if (Dist2(h.origin_prev, h.origin_cur) >= kMaxSmoothedMove * kMaxSmoothedMove) return 0;
  if (!BlendAngles(h.angles_prev, h.angles_cur, back, out_a)) return 0;
  for (int i = 0; i < 3; ++i) out_o[i] = h.origin_cur[i] + (h.origin_prev[i] - h.origin_cur[i]) * back;
  return 1;
}

// A body's move from one pose (origin, angles) to another: a point p goes to `to` + d (p - `from`), a direction v to d v.
struct BodyMove {
  float from[3], to[3];
  float d[3][3];
};
void MakeBodyMove(const float from_o[3], const float from_a[3], const float to_o[3], const float to_a[3], BodyMove& m) {
  float fa[3][3], ta[3][3];
  AnglesToAxis(from_a, fa);
  AnglesToAxis(to_a, ta);
  // A vector's parts along the old axes (fa v) put back out along the new ones: d = ta^T fa.
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) m.d[r][c] = ta[0][r] * fa[0][c] + ta[1][r] * fa[1][c] + ta[2][r] * fa[2][c];
  for (int i = 0; i < 3; ++i) {
    m.from[i] = from_o[i];
    m.to[i] = to_o[i];
  }
}
void TurnBy(const BodyMove& m, const float v[3], float out[3]) {
  for (int r = 0; r < 3; ++r) out[r] = m.d[r][0] * v[0] + m.d[r][1] * v[1] + m.d[r][2] * v[2];
}
void MoveBy(const BodyMove& m, const float p[3], float out[3]) {
  const float rel[3] = {p[0] - m.from[0], p[1] - m.from[1], p[2] - m.from[2]};
  TurnBy(m, rel, out);
  for (int i = 0; i < 3; ++i) out[i] += m.to[i];
}

// Moves a scene node the game's draw just filled (its origins and axis); false when something isn't a number.
bool MoveNode(uint8_t* base, uint32_t node, const BodyMove& m) {
  float in[6][3], out[6][3];
  ReadVec(base, node + kNodeLightOrigin, in[0]);
  ReadVec(base, node + kNodeOrigin, in[1]);
  ReadVec(base, node + kNodeOldOrigin, in[2]);
  for (int a = 0; a < 3; ++a) ReadVec(base, node + kNodeAxis + 12 * a, in[3 + a]);
  for (int k = 0; k < 3; ++k) MoveBy(m, in[k], out[k]);
  for (int a = 0; a < 3; ++a) TurnBy(m, in[3 + a], out[3 + a]);
  for (const auto& v : out)
    for (float f : v)
      if (!std::isfinite(f)) return false;
  WriteVec(base, node + kNodeLightOrigin, out[0]);
  WriteVec(base, node + kNodeOrigin, out[1]);
  WriteVec(base, node + kNodeOldOrigin, out[2]);
  for (int a = 0; a < 3; ++a) WriteVec(base, node + kNodeAxis + 12 * a, out[3 + a]);
  return true;
}

// The entity's scene node when the game's draw has just filled it from the server origin (its origin is a copy of
// +336); 0 otherwise (the draw left it out this frame, or it isn't the draw this assumes).
uint32_t FilledNode(uint8_t* base, uint32_t ent) {
  const uint32_t node = g::ReadU32(base, ent + kEntSceneNode);
  if (!ValidGuest(node)) return 0;
  float node_o[3], server_o[3];
  ReadVec(base, node + kNodeOrigin, node_o);
  ReadVec(base, ent + kEntServerOrigin, server_o);
  return Same(node_o, server_o) ? node : 0;
}

// After the vehicles' draw (0x82442338, types 14 and 15): the vehicle's node moved to the pose to draw. Returns how
// (BodyPose), 0 when left.
int PlaceVehicleBody(uint8_t* base, uint32_t ent) {
  if (BodySetting() < 1 || !ValidGuest(ent)) return 0;
  const uint32_t node = FilledNode(base, ent);
  if (!node) return 0;
  float cur_o[3], cur_a[3], out_o[3], out_a[3];
  const int how = BodyPose(base, ent, cur_o, cur_a, out_o, out_a);
  if (!how) return 0;
  BodyMove m;
  MakeBodyMove(cur_o, cur_a, out_o, out_a, m);
  if (!MoveNode(base, node, m)) return 0;
  ++g_counts.bodies;
  return how;
}

// After the people's draw (0x8244B610, type 1): one riding a vehicle (turned by the vehicle's +352 there) moved with the
// vehicle as it is drawn; with setting 2 anyone else (not the player) between their own last two updates.
int PlaceRiderBody(uint8_t* base, uint32_t ent) {
  const int setting = BodySetting();
  if (setting < 1 || !ValidGuest(ent)) return 0;
  const uint32_t node = FilledNode(base, ent);
  if (!node) return 0;
  float cur_o[3], cur_a[3], out_o[3], out_a[3];
  int how = 0;
  if (g::ReadU32(base, ent + kEntFlags) & kEntOnVehicleFlag) {
    const uint32_t veh = EntityOfHandle(base, g::ReadU32(base, ent + kEntVehicleRidden));
    if (!veh || veh == ent) return 0;
    how = BodyPose(base, veh, cur_o, cur_a, out_o, out_a);
  } else {
    // The player's draw sets its +352 from the view (0x8244B610) and is seldom drawn (third person): left.
    if (setting < 2 || ent == PlayerEntity(base)) return 0;
    how = BodyPose(base, ent, cur_o, cur_a, out_o, out_a);
  }
  if (!how) return 0;
  BodyMove m;
  MakeBodyMove(cur_o, cur_a, out_o, out_a, m);
  if (!MoveNode(base, node, m)) return 0;
  ++g_counts.bodies;
  return how;
}

void DebugLog(uint8_t* base) {
  if (!REXCVAR_GET(cod3_debug_server_step)) return;
  ++g_counts.frames;
  const int32_t t = g::ReadS32(base, kCgTime);
  if (t != g_counts.last_cg_time) ++g_counts.cg_time_moved;
  g_counts.last_cg_time = t;
  if (g_clock.time - g_counts.since < 1000) return;
  REXLOG_INFO("cod3: server step: {} game updates of 1/{} s in {} frames over {} ms; the client game's "
              "time moved in {} frames; {} things drawn between updates, {} turned between them; {} frames "
              "without an update drew the animated models again; {} frames drew the view in a vehicle "
              "between updates; {} vehicles and people on them drawn between updates (cod3_server_smooth_body)",
              g_counts.updates, g_clock.hz, g_counts.frames, g_clock.time - g_counts.since,
              g_counts.cg_time_moved, g_counts.smoothed, g_counts.turned, g_counts.redrawn, g_counts.views,
              g_counts.bodies);
  // [claude-9b] the two kinds of frames side by side, as averages a frame.
  for (int k = 1; k >= 0; --k) {
    const DrawCounts& d = g_draw[k];
    if (!d.frames) continue;
    const double f = double(d.frames);
    std::string types;
    for (int t = 0; t < 16; ++t)
      if (d.cents_by_type[t]) types += fmt::format(" t{}={:.1f}", t, d.cents_by_type[t] / f);
    REXLOG_INFO("cod3: server step: frames {} an update: {} ({} with 2 or more); client game frames {:.2f} a frame, its time "
                "moved in {:.2f}; entity list {:.1f} items, {:.1f} linked, {:.1f} sent this update, the "
                "send time moved in {:.2f}; drawn entities {:.1f} ({}), scene entities {:.1f}; positions "
                "{:.1f}, interpolated {:.1f}, moving {:.1f}",
                k ? "with" : "without", d.frames, d.multi, d.cg_frames / f, d.cg_time_moved / f, d.list_items / f,
                d.linked / f, d.stamped / f, d.snap_time_moved / f, d.cents / f, types, d.refents / f,
                d.lerps / f, d.lerp_interp / f, d.lerp_moved / f);
  }
  g_draw[0] = DrawCounts{};
  g_draw[1] = DrawCounts{};
  if (g_sends_skipped || g_loop_full)
    REXLOG_INFO("cod3: server step: snapshot sends skipped (not a frame's last update): {}; loopback "
                "queue full: {}",
                g_sends_skipped, g_loop_full);
  g_sends_skipped = g_loop_full = 0;
  for (int k = 1; k >= 0; --k) {
    const ModelCounts& m = g_models[k];
    if (!m.frames) continue;
    REXLOG_INFO("cod3: server step: animated models, frames {} an update: {:.1f} a frame, {:.1f} ready to "
                "draw, {:.1f} drawn",
                k ? "with" : "without", double(m.models) / m.frames, double(m.armed) / m.frames,
                double(m.drawn) / m.frames);
  }
  g_models[0] = ModelCounts{};
  g_models[1] = ModelCounts{};
  // [rexglue-native npc] 2026-11-09 the animation detail by the update's place in its frame (0x82421AA8's hook).
  if (g_lod_models[0] || g_lod_models[1])
    REXLOG_INFO("cod3: server step: animation detail: a frame's first updates {} models, {} of them coarsest (not "
                "drawn since the last update); later updates {} models, {} coarsest; {} seen as drawn again "
                "(cod3_anim_lod_between_updates {})",
                g_lod_models[0], g_lod_coarse[0], g_lod_models[1], g_lod_coarse[1], g_redrawn_marks,
                REXCVAR_GET(cod3_anim_lod_between_updates) ? "on" : "off");
  g_lod_models[0] = g_lod_models[1] = g_lod_coarse[0] = g_lod_coarse[1] = 0;
  g_redrawn_marks = 0;
  for (int k = 1; k >= 0; --k) {
    const CameraCounts& c = g_camera[k];
    if (!c.frames) continue;
    std::string modes;
    for (int m = 0; m < 24; ++m)
      if (c.modes[m]) modes += fmt::format(" {}x{}", m, c.modes[m]);
    REXLOG_INFO("cod3: server step: camera, frames {} an update: {}, still in {}, not turning in {}; it moved "
                "{:.2f} and turned {:.3f} deg a frame; modes{}",
                k ? "with" : "without", c.frames, c.still, c.turned_none, c.moved / c.frames, c.turned / c.frames,
                modes);
  }
  g_camera[0] = CameraCounts{};
  g_camera[1] = CameraCounts{};
  if (g_upcam_blended)  // [acct2-ocr]
    REXLOG_INFO("cod3: server step: camera drawn between updates in {} frames (cod3_server_smooth_cams)",
                g_upcam_blended);
  g_upcam_blended = 0;
  if (g_linked_blended || g_linked_moved || g_linked_turned)  // [acct2-ocr]
    REXLOG_INFO("cod3: server step: camera place drawn between updates in {} frames (cod3_server_smooth_linked); "
                "SmoothView moved it in {}; turned between updates in {} (cod3_server_smooth_linked_turn)",
                g_linked_blended, g_linked_moved, g_linked_turned);
  g_linked_blended = g_linked_moved = g_linked_turned = 0;
  g_counts.since = g_clock.time;
  g_counts.frames = g_counts.updates = g_counts.cg_time_moved = g_counts.smoothed = g_counts.redrawn = 0;
  g_counts.turned = g_counts.views = g_counts.bodies = 0;
}

}  // namespace

namespace cod3::server_step {

// [acct2-cod3-motion] 2026-11-07 (server_step.h)
float DrawTimeFraction() { return float(SubMsFraction()); }

bool PlayerViewOffset(uint8_t* base, float out[3]) {
  if (Hz() <= 0 || !REXCVAR_GET(cod3_server_smooth) || !g_player.valid) return false;
  const uint32_t ent = PlayerEntity(base);
  if (!ValidGuest(ent)) return false;
  float now[3];
  ReadVec(base, ent + kEntServerOrigin, now);
  // Only while the player is where the last update left it (a vehicle or a script moved it in
  // that update; walking moves it every usercmd, which the movement steps smooth instead).
  if (!Same(now, g_player.cur) || g_player.update != g_clock.updates || Same(g_player.prev, g_player.cur) ||
      Dist2(g_player.prev, g_player.cur) >= kMaxSmoothedMove * kMaxSmoothedMove)
    return false;
  const float back = Back();
  for (int i = 0; i < 3; ++i) out[i] = (g_player.prev[i] - g_player.cur[i]) * back;
  return back > 0.0f;
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_ride (see the setting).
int RideViewOffset(uint8_t* base, const float cam[3], float out[3]) {
  g_ride_trace_state = 0;
  if (!RideOn()) return 0;
  const uint32_t client = PlayerClient(base);
  const uint32_t veh = client ? RiddenVehicle(base, client) : 0;
  if (!veh) return 0;
  // [claude-9b-think] the vehicle cameras (cod3_server_smooth_ride_cams); the turrets' (4, 5) and third person as
  // before.
  if (!RideCameraMode(g_cam_mode)) return 0;
  g_ride_trace_state = -1;
  float origin[3], angles[3];
  if (!RideDrawnPose(base, veh, origin, angles)) return -1;
  // The camera as the game placed it, relative to the vehicle at the last update, put on the vehicle as drawn.
  float cur_axis[3][3], drawn_axis[3][3];
  AnglesToAxis(g_ride.angles_cur, cur_axis);
  AnglesToAxis(angles, drawn_axis);
  float rel[3], local[3];
  for (int i = 0; i < 3; ++i) rel[i] = cam[i] - g_ride.origin_cur[i];
  for (int i = 0; i < 3; ++i) local[i] = cur_axis[i][0] * rel[0] + cur_axis[i][1] * rel[1] + cur_axis[i][2] * rel[2];
  float len2 = 0.0f;
  for (int k = 0; k < 3; ++k) {
    const float placed = origin[k] + local[0] * drawn_axis[0][k] + local[1] * drawn_axis[1][k] + local[2] * drawn_axis[2][k];
    out[k] = placed - cam[k];
    if (!std::isfinite(out[k])) return -1;
    len2 += out[k] * out[k];
  }
  if (len2 >= kMaxSmoothedMove * kMaxSmoothedMove) return -1;
  for (int i = 0; i < 3; ++i) g_ride_trace_off[i] = out[i];
  g_ride_trace_state = 1;
  return 1;
}

bool PlayerRides(uint8_t* base) {
  if (!RideOn()) return false;
  const uint32_t client = PlayerClient(base);
  return client && RidesVehicle(base, client);
}

}  // namespace cod3::server_step

// [claude-9b] 2026-10-27 Waits for all of the game's queued jobs (0x8212A110 with -1, the wait the
// main thread does every frame; job_wait.cpp). Two runs of the blkbrn checkpoint with updates of
// 1/60 s crashed in the same place: SV_LinkEntity -> 0x8246F080 took a node from the world tree's
// free list (0x829F1E90, 0x824648B8) whose link was 0x3C, a node freed while still in use, and the
// runs at 30 fps (two updates in every frame) crashed in G_RunFrame's animation queue
// (0x824F38E8 -> 0x8216E680 on a freed object). The game's update queues jobs (0x82129948) that the
// drawn frame after it waits for; two updates in one frame had the second start while the first's
// jobs still ran.
REX_EXTERN(sub_8212A110);
static void WaitForJobs(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.s64 = -1;
  sub_8212A110(ctx, base);
  ctx.r3.u64 = r3;
}

// [claude-9b] 2026-10-27 The game's own systems that Com_Frame (0x82536DD0) runs before SV_Frame, each
// with the frame's time (f1 = seconds, from the frame's time block 0x825F4E08: +4 msec, +12 seconds,
// +16 a constant / msec): 0x823FFC10(msec), the task lists 0x8251E3A0 (animation updates, scripted
// vehicles on splines ...), the sound events 0x8248A958, 0x82418410, 0x82519FF8, the spline vehicles'
// spawner 0x82521C70 (DroneSplineTask), 0x8252CAB0 and 0x825642A0. They are game code: at 60 fps the
// console ran them once per game update. With updates of 1/hz s they ran in every drawn frame and
// SV_Frame only in some, and the blkbrn checkpoint crashed on the world tree's free list at 80 s in
// every run (never with one update a frame; not the smoothing, not two updates in a frame): the
// animation updates queue requests (0x824F39A0, 820-byte records with room for 200 items each, at
// 0x829C91F8, count 0x82A2A228) that only the update empties (0x824F38E8 from 0x82563FC0), and the
// world tree's globals (0x829F1DE8) come right after that queue. Now the whole block runs in each
// update, in the game's order, with the update's time: 0x823FFC10's hook runs the updates and the
// later calls of the frame return at once. cod3_server_step_systems false: only SV_Frame steps.
// [claude-9b] 2026-10-27 03:15: on by default (owner 02:56 / 03:10: the Saint-Lo tutorial's guide stuck
// at the crate and taking the player's rifle, a softlock, "only on higher than 60 fps"). Off, those
// systems ran once per drawn frame and the game's update 0-6 times, so the work they hand to the update
// (the animation requests, the notetrack notifies, the entity deaths, the deferred notifies) no longer
// came one batch per update as on the console: at ~300 fps five frames' worth a game update, at 30 fps
// one for two updates (run c9b-test-stlo-hz60-fps30 also crashed in the update's animation queue,
// 0x824F38E8, as on 10-26). On: each update runs them first, in the console's order, at any frame rate.
REXCVAR_DEFINE_BOOL(cod3_server_step_systems, true, "cod3/Game",
                    "With cod3_server_hz, the game's own systems that run before each game update "
                    "(animation updates, scripted vehicles, sound events ...) run with the updates, "
                    "not in every drawn frame; off: only the server's update steps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
constexpr uint32_t kFrameTime = 0x825F4E08;   // the frame's time block (Com_Frame)
constexpr uint32_t kFrameTimeK = 0x82062F68;  // the constant Com_Frame divides by msec for +16
bool g_block_ran = false;  // this frame's systems and SV_Frame already ran (in 0x823FFC10's hook)
}  // namespace

REX_EXTERN(__imp__sub_823FFC10);
REX_EXTERN(__imp__sub_8251E3A0);
REX_EXTERN(__imp__sub_8248A958);
REX_EXTERN(__imp__sub_82418410);
REX_EXTERN(__imp__sub_82519FF8);
REX_EXTERN(__imp__sub_82521C70);
REX_EXTERN(__imp__sub_8252CAB0);
REX_EXTERN(__imp__sub_825642A0);
REX_EXTERN(__imp__SV_Frame);
REX_EXTERN(__imp__sub_8253F438);  // G_RunFrame's last call: the animated models ready to draw once

namespace {
// [claude-9b] 2026-10-27 The last update ran G_RunFrame to its end (0x8253F438): the level is running
// and its animated models were made ready. A frame without an update makes them ready again only
// then (not in the menus, where SV_Frame runs no G_RunFrame, nor while a level is shut down).
bool g_game_armed = false;
}  // namespace

namespace {

// One game update's systems, as Com_Frame calls them, with the update's time.
void RunSystems(PPCContext& ctx, uint8_t* base, int tick_msec) {
  const double dt = double(float(tick_msec) * 0.001f);
  g::WriteU32(base, kFrameTime + 4, uint32_t(std::max(tick_msec, 1)));
  g::WriteF32(base, kFrameTime + 12, float(dt));
  g::WriteF32(base, kFrameTime + 16, g::ReadF32(base, kFrameTimeK) / float(tick_msec));
  ctx.r3.s64 = tick_msec;
  __imp__sub_823FFC10(ctx, base);
  ctx.r3.u64 = 0x82A56738;
  ctx.f1.f64 = dt;
  __imp__sub_8251E3A0(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, 0x82A2AB38);
  ctx.f1.f64 = dt;
  __imp__sub_8248A958(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, 0x82A2ACFC);
  ctx.f1.f64 = dt;
  __imp__sub_82418410(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, 0x82A2A1A8);
  ctx.f1.f64 = dt;
  __imp__sub_82519FF8(ctx, base);
  ctx.r3.u64 = g::ReadU32(base, 0x82A2A1A0);
  ctx.f1.f64 = dt;
  __imp__sub_82521C70(ctx, base);
  __imp__sub_8252CAB0(ctx, base);
  __imp__sub_825642A0(ctx, base);
}

void ClockChange(int hz) {
  if (hz == g_clock.hz) return;
  if (hz > 0)
    REXLOG_INFO("cod3: server step: the campaign's game in updates of 1/{} s, whatever the frame rate "
                "(cod3_server_hz)",
                hz);
  else
    REXLOG_INFO("cod3: server step: one game update a frame with the frame's time, as the game does "
                "(cod3_server_hz 0)");
  g_clock = Clock{};
  g_clock.hz = hz;
  Forget();
}

// The frame's game updates (hz > 0, msec > 0): none, one or several of 1/hz s; with systems, each one
// runs the game's systems first (as Com_Frame does) and then SV_Frame.
void PoseFrameEnd(uint8_t* base);  // [claude-9b] cod3_debug_pose (below)

void GameUpdates(PPCContext& ctx, uint8_t* base, int hz, int msec, bool systems) {
  PutBackDrawn(base);
  SubMsFrame(msec);  // [acct2-cod3-motion] cod3_server_draw_subms
  if (DebugOn()) {
    CountModels(base, false);
    CountCamera(base);  // the last frame's camera, by that frame's kind (g_kind not updated yet)
  }
  // The frame's time block as Com_Frame set it, put back after the updates for the rest of the frame.
  const uint32_t frame_msec = g::ReadU32(base, kFrameTime + 4);
  const float frame_dt = g::ReadF32(base, kFrameTime + 12);
  const float frame_inv = g::ReadF32(base, kFrameTime + 16);
  g_clock.time += msec;
  const int64_t due = TickAtOrBefore(g_clock.time, hz);
  int64_t n = due - g_clock.tick;
  const bool long_frame = n > kMaxTicksPerFrame;
  if (long_frame) n = 1;  // a level load, a long hitch: one update with all of its time, as the game
  // [rexglue-native npc] 2026-11-09 cod3_anim_lod_between_updates: the models the last frame drew, for the updates
  // [rexglue-native npc] after this frame's first.
  const bool keep_drawn = n > 1 && REXCVAR_GET(cod3_anim_lod_between_updates);
  if (keep_drawn) NoteDrawnModels(base);
  for (int64_t i = 0; i < n; ++i) {
    if (i > 0 && REXCVAR_GET(cod3_server_step_sync)) WaitForJobs(ctx, base);
    if (i > 0 && keep_drawn) MarkDrawnAgain(base);  // [rexglue-native npc] as a draw between them would have
    g_update_later = i > 0;                         // [rexglue-native npc] cod3_debug_server_step's LOD counts
    const int tick_msec = static_cast<int>(long_frame ? TickEnd(due, hz) - TickEnd(g_clock.tick, hz)
                                                      : TickEnd(g_clock.tick + 1, hz) - TickEnd(g_clock.tick, hz));
    const uint32_t level_time = g::ReadU32(base, kLevelTime);
    g_skip_send = !long_frame && i + 1 < n;  // only the frame's last update sends the snapshots
    RideBeforeUpdate(base);  // [claude-9b-think] cod3_server_smooth_ride
    if (systems) RunSystems(ctx, base, tick_msec);
    ctx.r3.s64 = tick_msec;
    g_game_armed = false;
    __imp__SV_Frame(ctx, base);
    g_skip_send = false;
    g_time_advancing = g::ReadU32(base, kLevelTime) != level_time;
    ++g_counts.updates;
    if (long_frame) {
      g_clock.tick = due;
      g_clock.updates += 2;  // nothing to draw between: the histories start again
      break;
    }
    ++g_clock.tick;
    ++g_clock.updates;
    const uint32_t player = PlayerEntity(base);
    if (ValidGuest(player)) {
      float now[3];
      ReadVec(base, player + kEntServerOrigin, now);
      Advance(g_player, now);
    }
    AdvanceView(base);  // [claude-9b] cod3_server_smooth_view
    AdvanceRide(base);  // [claude-9b-think] cod3_server_smooth_ride
  }
  g_update_later = false;  // [rexglue-native npc]
  FrameUpdatesLine(n, msec);  // [rexglue-native npc] cod3_debug_frame_updates
  PoseBonesLines(base, n);    // [rexglue-native npc] cod3_debug_pose_bones
  if (n == 0 && g_game_armed && REXCVAR_GET(cod3_server_step_draw)) {
    // [claude-9b] no update this frame: its animated models ready to draw (cod3_server_step_draw).
    __imp__sub_8253F438(ctx, base);
    ++g_counts.redrawn;
  }
  if (systems) {
    g::WriteU32(base, kFrameTime + 4, frame_msec);
    g::WriteF32(base, kFrameTime + 12, frame_dt);
    g::WriteF32(base, kFrameTime + 16, frame_inv);
  }
  // [claude-9b] the frame's own time for the client, as SV_Frame left it in the game (see kSvFrameMsec).
  g::WriteU32(base, kSvFrameMsec, static_cast<uint32_t>(msec));
  g::WriteU32(base, kSvFrameMsec + 4, 0);
  g_kind = n > 0 ? 1 : 0;
  g_draw_updates = static_cast<int>(n);  // [claude-9b] cod3_debug_view_trace
  g_es_msec = msec;  // [acct2-cod3-motion] cod3_debug_entity_steps
  if (DebugOn()) {
    CountModels(base, true);
    ++g_draw[g_kind].frames;
    if (n > 1) ++g_draw[g_kind].multi;
  }
  DebugLog(base);
  PoseFrameEnd(base);  // [claude-9b] cod3_debug_pose
}

bool SystemsStep() { return Hz() > 0 && REXCVAR_GET(cod3_server_step_systems); }

}  // namespace

// 0x823FFC10(msec): the first of the frame's game systems; with cod3_server_hz and
// cod3_server_step_systems, the frame's game updates run from here.
REX_HOOK_RAW(sub_823FFC10) {
  const int msec = ctx.r3.s32;
  g_block_ran = false;
  PutBackDrawn(base);  // [claude-9b] before the frame's first game code
  if (!SystemsStep() || msec <= 0) {
    __imp__sub_823FFC10(ctx, base);
    return;
  }
  const int hz = Hz();
  ClockChange(hz);
  GameUpdates(ctx, base, hz, msec, true);
  g_block_ran = true;
}

// The rest of the frame's systems: already run in the updates when g_block_ran.
#define COD3_SYSTEM_HOOK(name)          \
  REX_HOOK_RAW(name) {                  \
    if (g_block_ran) return;            \
    __imp__##name(ctx, base);           \
  }
COD3_SYSTEM_HOOK(sub_8251E3A0)
COD3_SYSTEM_HOOK(sub_8248A958)
COD3_SYSTEM_HOOK(sub_82418410)
COD3_SYSTEM_HOOK(sub_82519FF8)
COD3_SYSTEM_HOOK(sub_82521C70)
COD3_SYSTEM_HOOK(sub_8252CAB0)
COD3_SYSTEM_HOOK(sub_825642A0)
#undef COD3_SYSTEM_HOOK

// SV_Frame(int msec): the server's frame, the game's update in it.
REX_HOOK_RAW(SV_Frame) {
  if (g_block_ran) {  // run with the systems in 0x823FFC10's hook this frame
    g_block_ran = false;
    return;
  }
  const int hz = Hz();
  ClockChange(hz);
  const int msec = ctx.r3.s32;
  if (hz <= 0 || msec <= 0) {
    PutBackDrawn(base);
    __imp__SV_Frame(ctx, base);
    ++g_clock.updates;
    if (msec > 0) g_clock.time += msec;  // only for the debug log's second (hz 0)
    g_kind = 1;
    g_es_msec = msec;  // [acct2-cod3-motion] cod3_debug_entity_steps
    if (DebugOn()) {
      ++g_draw[1].frames;
      DebugLog(base);
    }
    PoseFrameEnd(base);  // [claude-9b] cod3_debug_pose with cod3_server_hz 0 too
    return;
  }
  GameUpdates(ctx, base, hz, msec, false);
}

// [claude-9b] 2026-10-27 0x82447D48(view): how 0x824505E8 places the camera (a switch of 23 ways),
// kept for the debug log's camera line.
REX_EXTERN(__imp__sub_82447D48);
REX_HOOK_RAW(sub_82447D48) {
  __imp__sub_82447D48(ctx, base);
  g_cam_mode = ctx.r3.u32;
}

// [claude-9b] 2026-10-27 0x824505E8(view): the frame's camera (CG_CalcViewValues calls it; the movement's
// view smoothing adds to it after, movement/hooks_sp.cpp). In a vehicle it runs with the view angles
// between the last two updates (cod3_server_smooth_view), so the camera's angles, its axis and its place
// around a turret all come out smooth; the game's own value is put back after.
REX_EXTERN(__imp__sub_824505E8);
namespace {
float g_trace_cam[3] = {};  // [claude-9b] cod3_debug_view_trace: the camera as 0x824505E8 left it
void TraceCamera(uint8_t* base) {
  if (REXCVAR_GET(cod3_debug_view_trace) <= 0) return;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client > 3) return;
  ReadVec(base, kCgBase + client * kCgSize + kCgViewOrigin, g_trace_cam);
}

// [acct2-cod3-motion] 2026-11-07 the camera's place as the game's view setup left it (cod3_server_smooth_linked_keep).
void NoteGameCamera(uint8_t* base) {
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  g_game_cam_have = local <= 3;
  if (g_game_cam_have) ReadVec(base, kCgBase + local * kCgSize + kCgViewOrigin, g_game_cam);
}

// [acct2-ocr] 2026-11-07 cod3_server_smooth_cams (see the setting): the camera's place and angles at the last two
// updates, and the frame's camera drawn between them.
struct UpdateCamera {
  bool valid = false;
  uint32_t mode = 0, local = 0;
  uint64_t update = 0;
  float prev[6] = {}, cur[6] = {};  // origin, then angles
};
UpdateCamera g_upcam;

bool SameCamera(const float a[6], const float b[6]) { return Same(a, b) && Same(a + 3, b + 3); }

void SmoothUpdateCamera(uint8_t* base) {
  const uint32_t mode = g_cam_mode;
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  if (g_clock.hz <= 0 || !REXCVAR_GET(cod3_server_smooth) || local > 3 || mode >= 23 ||
      ((uint32_t(REXCVAR_GET(cod3_server_smooth_cams)) >> mode) & 1u) == 0) {
    g_upcam.valid = false;
    return;
  }
  const uint32_t cg = kCgBase + local * kCgSize;
  float now[6];
  ReadVec(base, cg + kCgViewOrigin, now);
  ReadVec(base, cg + kCgViewAngles, now + 3);
  for (float f : now)
    if (!std::isfinite(f)) {
      g_upcam.valid = false;
      return;
    }
  if (!g_upcam.valid || g_upcam.mode != mode || g_upcam.local != local) {  // a new camera: nothing before it
    for (int i = 0; i < 6; ++i) g_upcam.prev[i] = g_upcam.cur[i] = now[i];
    g_upcam.valid = true;
    g_upcam.mode = mode;
    g_upcam.local = local;
    g_upcam.update = g_clock.updates;
    return;
  }
  if (g_upcam.update != g_clock.updates) {  // the first frame since an update: that update's camera
    const bool next = g_upcam.update + 1 == g_clock.updates;
    for (int i = 0; i < 6; ++i) {
      g_upcam.prev[i] = next ? g_upcam.cur[i] : now[i];
      g_upcam.cur[i] = now[i];
    }
    g_upcam.update = g_clock.updates;
  } else if (!SameCamera(now, g_upcam.cur)) {  // moved since the update: not the update's alone, left
    for (int i = 0; i < 6; ++i) g_upcam.prev[i] = g_upcam.cur[i] = now[i];
    return;
  }
  if (SameCamera(g_upcam.prev, g_upcam.cur)) return;
  if (Dist2(g_upcam.prev, g_upcam.cur) >= kMaxSmoothedMove * kMaxSmoothedMove) return;  // a cut
  for (int i = 3; i < 6; ++i)
    if (std::fabs(Turn(g_upcam.prev[i], g_upcam.cur[i])) >= kMaxSmoothedTurn) return;
  const float back = Back();
  if (back <= 0.0f) return;
  float out[6];
  for (int i = 0; i < 3; ++i) out[i] = g_upcam.cur[i] + (g_upcam.prev[i] - g_upcam.cur[i]) * back;
  for (int i = 3; i < 6; ++i) out[i] = g_upcam.cur[i] - Turn(g_upcam.prev[i], g_upcam.cur[i]) * back;
  // The axis the game made turned by the same turn as the angles (so whatever it holds beyond them is kept).
  BodyMove m;
  MakeBodyMove(g_upcam.cur, g_upcam.cur + 3, out, out + 3, m);
  float axis[3][3], out_axis[3][3];
  for (int a = 0; a < 3; ++a) {
    ReadVec(base, cg + kCgViewAxis + 12 * a, axis[a]);
    TurnBy(m, axis[a], out_axis[a]);
    for (float f : out_axis[a])
      if (!std::isfinite(f)) return;
  }
  for (float f : out)
    if (!std::isfinite(f)) return;
  WriteVec(base, cg + kCgViewOrigin, out);
  WriteVec(base, cg + kCgViewAngles, out + 3);
  for (int a = 0; a < 3; ++a) WriteVec(base, cg + kCgViewAxis + 12 * a, out_axis[a]);
  ++g_upcam_blended;
  g_upcam_drawn = true;  // [acct2-ocr] cod3_server_smooth_linked leaves this frame's camera
}

// [acct2-ocr] 2026-11-07 cod3_server_smooth_linked (see the setting): the camera's place in the first frame after
// each of the last two updates.
struct LinkedCamera {
  bool valid = false;
  uint32_t mode = 0, local = 0;
  uint64_t update = 0;
  float prev[3] = {}, cur[3] = {};
  float aprev[3] = {}, acur[3] = {};  // [acct2-cod3-motion] its angles (cg +4544) the same way
};
LinkedCamera g_linked;

// [acct2-cod3-motion] cod3_server_smooth_linked_turn: the linked camera's angles at the last two updates (kept in
// every frame SmoothLinkedCamera sees, from the angles as they are then), and drawn between them by Back() when this
// frame's are still the update's. Returns true when it drew them.
bool LinkedTurn(uint8_t* base, uint32_t cg, bool first_since_update, bool next) {
  float a[3];
  ReadVec(base, cg + kCgViewAngles, a);
  for (float f : a)
    if (!std::isfinite(f)) return false;
  if (first_since_update) {
    for (int i = 0; i < 3; ++i) {
      g_linked.aprev[i] = next ? g_linked.acur[i] : a[i];
      g_linked.acur[i] = a[i];
    }
  } else if (!Same(a, g_linked.acur)) {  // turned since the update (the player's look, every frame): left
    for (int i = 0; i < 3; ++i) g_linked.aprev[i] = g_linked.acur[i] = a[i];
    return false;
  }
  if (!REXCVAR_GET(cod3_server_smooth_linked_turn) || Same(g_linked.aprev, g_linked.acur)) return false;
  for (int i = 0; i < 3; ++i)
    if (std::fabs(Turn(g_linked.aprev[i], g_linked.acur[i])) >= kMaxSmoothedTurn) return false;  // a cut
  const float back = Back();
  if (back <= 0.0f) return false;
  float out[3];
  for (int i = 0; i < 3; ++i) out[i] = g_linked.acur[i] - Turn(g_linked.aprev[i], g_linked.acur[i]) * back;
  // The axis the game made turned by the same turn (as SmoothUpdateCamera does).
  BodyMove m;
  const float zero[3] = {0.0f, 0.0f, 0.0f};
  MakeBodyMove(zero, g_linked.acur, zero, out, m);
  float axis[3][3], out_axis[3][3];
  for (int k = 0; k < 3; ++k) {
    ReadVec(base, cg + kCgViewAxis + 12 * k, axis[k]);
    TurnBy(m, axis[k], out_axis[k]);
    for (float f : out_axis[k])
      if (!std::isfinite(f)) return false;
  }
  for (float f : out)
    if (!std::isfinite(f)) return false;
  WriteVec(base, cg + kCgViewAngles, out);
  for (int k = 0; k < 3; ++k) WriteVec(base, cg + kCgViewAxis + 12 * k, out_axis[k]);
  if (REXCVAR_GET(cod3_debug_server_step)) ++g_linked_turned;
  return true;
}
}  // namespace

namespace cod3::server_step {
// [acct2-ocr] 2026-11-07 (server_step.h)
void SmoothLinkedCamera(uint8_t* base, bool moved) {
  const bool upcam = g_upcam_drawn;
  g_upcam_trace = upcam;  // [acct2-cod3-motion] cod3_debug_view_trace
  const bool have_game_cam = g_game_cam_have;  // [acct2-cod3-motion] this frame's (NoteGameCamera)
  g_game_cam_have = false;
  g_upcam_drawn = false;
  const uint32_t mode = g_cam_mode;
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  const uint32_t client = PlayerClient(base);
  if (g_clock.hz <= 0 || !REXCVAR_GET(cod3_server_smooth) || local > 3 || mode >= 23 || upcam ||
      ((uint32_t(REXCVAR_GET(cod3_server_smooth_linked)) >> mode) & 1u) == 0 ||
      (client && RidesVehicle(base, client))) {
    g_linked.valid = false;
    g_linked_trace = -1;  // [acct2-cod3-motion]
    return;
  }
  // [acct2-cod3-motion] cod3_server_smooth_linked_keep: a frame SmoothView drew still keeps the history, from the
  // game's own camera place this frame.
  const bool keep_only = moved && REXCVAR_GET(cod3_server_smooth_linked_keep) && have_game_cam;
  if (moved) {  // SmoothView placed it (the movement steps or the update offset): left
    if (REXCVAR_GET(cod3_debug_server_step)) ++g_linked_moved;
    g_linked_trace = 2;  // [acct2-cod3-motion]
    if (!keep_only) {  // nothing kept (as before)
      g_linked.valid = false;
      return;
    }
  } else {
    g_linked_trace = 0;  // [acct2-cod3-motion]
  }
  const uint32_t at = kCgBase + local * kCgSize + kCgViewOrigin;
  float now[3];
  ReadVec(base, at, now);
  if (keep_only)
    for (int i = 0; i < 3; ++i) now[i] = g_game_cam[i];  // [acct2-cod3-motion] the game's place, not SmoothView's
  for (float f : now)
    if (!std::isfinite(f)) {
      g_linked.valid = false;
      return;
    }
  const uint32_t cg = kCgBase + local * kCgSize;  // [acct2-cod3-motion]
  if (!g_linked.valid || g_linked.mode != mode || g_linked.local != local) {  // a new camera: nothing before it
    for (int i = 0; i < 3; ++i) g_linked.prev[i] = g_linked.cur[i] = now[i];
    ReadVec(base, cg + kCgViewAngles, g_linked.acur);  // [acct2-cod3-motion]
    for (int i = 0; i < 3; ++i) g_linked.aprev[i] = g_linked.acur[i];
    g_linked.valid = true;
    g_linked.mode = mode;
    g_linked.local = local;
    g_linked.update = g_clock.updates;
    if (!keep_only) g_linked_trace = 3;  // [acct2-cod3-motion]
    return;
  }
  const bool first_since_update = g_linked.update != g_clock.updates;
  const bool next_update = g_linked.update + 1 == g_clock.updates;
  if (first_since_update) {  // the first frame since an update: that update's place
    for (int i = 0; i < 3; ++i) {
      g_linked.prev[i] = next_update ? g_linked.cur[i] : now[i];
      g_linked.cur[i] = now[i];
    }
    g_linked.update = g_clock.updates;
  }
  // [acct2-cod3-motion] cod3_server_smooth_linked_turn: its angles, in every frame (SmoothView moves its place alone)
  const bool turned = LinkedTurn(base, cg, first_since_update, next_update);
  if (turned && !keep_only) g_linked_trace = 4;  // 4: its angles alone
  if (keep_only) return;  // [acct2-cod3-motion] drawn by SmoothView this frame
  if (Same(g_linked.prev, g_linked.cur)) return;
  if (Dist2(g_linked.prev, g_linked.cur) >= kMaxSmoothedMove * kMaxSmoothedMove) return;  // a cut
  const float back = Back();
  if (back <= 0.0f) return;
  float out[3];
  // The part of the update's move not drawn yet taken off; what moved since the update (bob, lean) is kept.
  for (int i = 0; i < 3; ++i) out[i] = now[i] + (g_linked.prev[i] - g_linked.cur[i]) * back;
  for (float f : out)
    if (!std::isfinite(f)) return;
  WriteVec(base, at, out);
  if (REXCVAR_GET(cod3_debug_server_step)) ++g_linked_blended;
  g_linked_trace = turned ? 5 : 1;  // [acct2-cod3-motion] 5: its angles too
  for (int i = 0; i < 3; ++i) g_linked_trace_off[i] = out[i] - now[i];
}
}  // namespace cod3::server_step

REX_HOOK_RAW(sub_824505E8) {
  g_upcam_drawn = false;  // [acct2-ocr] set again when cod3_server_smooth_cams draws this frame's camera
  uint32_t at = 0;
  float now[3], draw[3];
  // [claude-9b-think] 2026-10-27 UNTESTED on a ridden vehicle with the view turned every frame (free look), the view
  // turned back by the update's turn not drawn yet (cod3_server_smooth_ride; the tank's turret keeps the first).
  if (!ViewAnglesToDraw(base, at, now, draw) && !RideViewAngles(base, at, now, draw)) {
    __imp__sub_824505E8(ctx, base);
    NoteGameCamera(base);  // [acct2-cod3-motion] cod3_server_smooth_linked_keep
    RideTurnCamera(base);  // [claude-9b-think] a vehicle camera turned by the vehicle's pose
    SmoothUpdateCamera(base);  // [acct2-ocr] a camera set in the updates alone (cod3_server_smooth_cams)
    TraceCamera(base);
    return;
  }
  WriteVec(base, at, draw);
  __imp__sub_824505E8(ctx, base);
  NoteGameCamera(base);  // [acct2-cod3-motion] cod3_server_smooth_linked_keep
  RideTurnCamera(base);  // [claude-9b-think] before the view angles go back: it compares the camera with them
  float after[3];
  ReadVec(base, at, after);
  if (Same(after, draw)) WriteVec(base, at, now);  // unless the camera code set them itself
  ++g_counts.views;
  TraceCamera(base);
}

namespace {

// [claude-9b] 2026-10-27 cod3_debug_pose (see the setting). The renderer's draw of a scene node (0x8242E450: model at
// node +0x5C, entity at +0x60) draws an animated model only while bit 0 of model +0xC4 is set; its bones are at
// [model +0x70] + 0x30, 0x40 bytes each, model +0xAB of them.
constexpr uint32_t kNodeModel = 0x5C;
constexpr uint32_t kNodeEntity = 0x60;
constexpr uint32_t kModelBones = 0x70;
constexpr uint32_t kModelBoneCount = 0xAB;
constexpr uint32_t kModelStamp = 0xAC;       // the update counter when its animation last ran
constexpr uint32_t kModelLod = 0xB8;         // +0xB8, +0xBC, +0xC0: the levels of detail the draw picks from
constexpr uint32_t kModelArmed = 0xC4;       // bit 0: not drawn since the last update
constexpr uint32_t kAnimUpdateCounter = 0x82A2ABC0;
constexpr uint32_t kEntSceneFlags = 0x278;   // 0x40000000: a scripted scene's actor
constexpr uint32_t kEntSceneFlags2 = 0x27C;  // bit 1: kept out of the not-drawn level of detail
constexpr int32_t kMaxEntityNumber = 8192;
constexpr float kJumpUnits = 8.0f;           // a drawn place that moved more than this in one frame

struct PoseTrack {
  int32_t number = -1;     // its entity's number (-1: not known yet)
  bool number_from_update = false;  // the number came from the update's list (the draw's entity may be another struct)
  uint64_t frame = 0;      // the drawn frame it was last counted in
  uint32_t hash[2] = {};   // the last two drawn poses ([0] the newest)
  float origin[2][3] = {};
  int seen = 0;            // drawn frames so far (up to 2 matter)
  uint32_t update_hash = 0;  // the pose at the last update's end (0: none)
  int frames = 0, aba = 0, between = 0, after_update = 0, jumps = 0;
  uint32_t flags = 0, flags2 = 0;
  uint32_t ent = 0;        // the entity, when its number came from the update's list
};
std::mutex g_pose_mutex;
// [claude-9b] 2026-10-27 Kept per model (its bones), as cod3mp's pose_debug_mp.cpp does: runs c9b-test-pose3-p1/-p3
// counted nothing while keyed by the draw's entity number (r4 of 0x82427D18 isn't always an entity with its number
// at +500), so the number is only shown when known.
std::unordered_map<uint32_t, PoseTrack> g_pose;
uint64_t g_pose_calls_node = 0, g_pose_calls_node_armed = 0, g_pose_calls_chars = 0;  // since the last log line
uint64_t g_pose_calls_no_bones = 0;  // models without readable bones (+0x70)
uint64_t g_pose_frame = 1;
int g_pose_lines = 0;
std::chrono::steady_clock::time_point g_pose_since;

uint32_t PoseHash(uint8_t* base, uint32_t model) {
  const uint32_t bones = g::ReadU32(base, model + kModelBones);
  if (!ValidGuest(bones)) return 0;
  const uint32_t n = std::min<uint32_t>(g::ReadU8(base, model + kModelBoneCount), 200);
  const uint8_t* p = base + bones + 0x30;
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n * 0x40; ++i) h = (h ^ p[i]) * 16777619u;
  return h ? h : 1;
}

// [claude-9b] 2026-10-27 One drawn model (the scene node draw below, and the characters' draw 0x82427D18: run
// c9b-test-pose1 counted nothing through the node draw alone).
void PoseCount(uint8_t* base, uint32_t model, uint32_t ent, int32_t want) {
  if (!ValidGuest(model)) return;
  const uint32_t h = PoseHash(base, model);
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  if (!h) {
    ++g_pose_calls_no_bones;
    return;
  }
  PoseTrack& t = g_pose[model];
  if (t.frame == g_pose_frame) return;  // counted in this frame already
  if (!t.number_from_update && ValidGuest(ent)) {
    const int32_t number = g::ReadS32(base, ent + kEntNumber);
    if (number >= 0 && number < kMaxEntityNumber) t.number = number;
  }
  const int32_t number = t.number;
  // The entity's place and flags only through an entity known to be one (its number from the update's list).
  const uint32_t known_ent = t.number_from_update ? t.ent : (number >= 0 ? ent : 0);
  float origin[3] = {};
  if (ValidGuest(known_ent)) ReadVec(base, known_ent + kEntDrawnOrigin, origin);
  t.frame = g_pose_frame;
  ++t.frames;
  if (ValidGuest(known_ent)) {
    t.flags = g::ReadU32(base, known_ent + kEntSceneFlags);
    t.flags2 = g::ReadU32(base, known_ent + kEntSceneFlags2);
  }
  if (t.seen >= 2 && h == t.hash[1] && h != t.hash[0]) ++t.aba;
  if (t.seen >= 1 && g_kind == 0 && h != t.hash[0]) ++t.between;
  if (t.update_hash && h != t.update_hash) ++t.after_update;
  if (t.seen >= 1 && ValidGuest(known_ent)) {
    const float dx = origin[0] - t.origin[0][0], dy = origin[1] - t.origin[0][1], dz = origin[2] - t.origin[0][2];
    if (dx * dx + dy * dy + dz * dz > kJumpUnits * kJumpUnits) ++t.jumps;
  }
  if (want > 0 && number == want - 1 && ValidGuest(known_ent) && g_pose_lines < 300) {
    ++g_pose_lines;
    float server[3];
    ReadVec(base, known_ent + kEntServerOrigin, server);
    REXLOG_INFO("cod3: pose {} frame {} kind {} updates {} pose {:08x} update pose {:08x} (last {:08x}, before {:08x}) "
                "lod {} {} {} stamp {} counter {} drawn {:.2f} {:.2f} {:.2f} server {:.2f} {:.2f} {:.2f} flags {:08x} "
                "{:08x} bones {}",
                number, g_pose_frame, g_kind, g_draw_updates, h, t.update_hash, t.hash[0], t.hash[1],
                g::ReadS32(base, model + kModelLod), g::ReadS32(base, model + kModelLod + 4),
                g::ReadS32(base, model + kModelLod + 8), g::ReadU32(base, model + kModelStamp),
                g::ReadU32(base, kAnimUpdateCounter), origin[0], origin[1], origin[2], server[0], server[1], server[2],
                t.flags, t.flags2, g::ReadU8(base, model + kModelBoneCount));
  }
  t.hash[1] = t.hash[0];
  t.hash[0] = h;
  for (int i = 0; i < 3; ++i) {
    t.origin[1][i] = t.origin[0][i];
    t.origin[0][i] = origin[i];
  }
  t.seen = std::min(t.seen + 1, 2);
}

void PoseAtDraw(uint8_t* base, uint32_t node, int32_t want) {
  if (!ValidGuest(node)) return;
  const uint32_t model = g::ReadU32(base, node + kNodeModel);
  {
    std::lock_guard<std::mutex> lock(g_pose_mutex);
    ++g_pose_calls_node;
    if (ValidGuest(model) && (g::ReadU32(base, model + kModelArmed) & 1)) ++g_pose_calls_node_armed;
  }
  if (!ValidGuest(model) || !(g::ReadU32(base, model + kModelArmed) & 1)) return;  // the game doesn't draw it now
  PoseCount(base, model, g::ReadU32(base, node + kNodeEntity), want);
}

// Each update's end (0x8253F438 from G_RunFrame): the pose every animated model was left in.
void PoseAfterUpdate(uint8_t* base) {
  const uint32_t handles = g::ReadU32(base, kAnimatedList);
  if (!ValidGuest(handles)) return;
  const int32_t count = std::min<int32_t>(g::ReadS32(base, kAnimatedList + 8), 4096);
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t h = g::ReadU32(base, handles + 4 * i);
    const uint32_t index = h & 0x3FFF;
    if (index >= 0x1000) continue;
    const uint32_t slot = kEntityHandles + index * 8;
    if (g::ReadU32(base, slot + 4) != (h >> 14)) continue;
    const uint32_t ent = g::ReadU32(base, slot);
    if (!ValidGuest(ent)) continue;
    const uint32_t model = g::ReadU32(base, ent + 508);
    if (!ValidGuest(model)) continue;
    const int32_t number = g::ReadS32(base, ent + kEntNumber);
    if (number < 0 || number >= kMaxEntityNumber) continue;
    PoseTrack& t = g_pose[model];
    t.update_hash = PoseHash(base, model);
    t.number = number;
    t.number_from_update = true;
    t.ent = ent;
  }
}

// Once a drawn frame (the end of GameUpdates): the frame count, and the log once a second.
void PoseFrameEnd(uint8_t* base) {
  if (!REXCVAR_GET(cod3_debug_pose)) return;
  std::lock_guard<std::mutex> lock(g_pose_mutex);
  ++g_pose_frame;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_pose_since < std::chrono::seconds(1)) return;
  g_pose_since = now;
  int models = 0, frames = 0, aba = 0, between = 0, after = 0, jumps = 0;
  std::vector<std::pair<int, uint32_t>> worst;  // (A-B-A count, model)
  for (auto& [model, t] : g_pose) {
    if (!t.frames) continue;
    ++models;
    frames += t.frames;
    aba += t.aba;
    between += t.between;
    after += t.after_update;
    jumps += t.jumps;
    if (t.aba || t.jumps || t.between) worst.emplace_back(t.aba * 4 + t.jumps * 4 + t.between, model);
  }
  float player[3] = {};
  const uint32_t pl = PlayerEntity(base);
  if (ValidGuest(pl)) ReadVec(base, pl + kEntServerOrigin, player);
  REXLOG_INFO("cod3: pose: {} animated models drawn {} times; went back to the pose before last (A-B-A) {}; changed in "
              "frames without an update {}; drawn unlike the last update's pose {}; jumped over {} units {} (calls: "
              "characters' draw {}, node draw {}, {} of them armed; no bones {})",
              models, frames, aba, between, after, int(kJumpUnits), jumps, g_pose_calls_chars, g_pose_calls_node,
              g_pose_calls_node_armed, g_pose_calls_no_bones);
  g_pose_calls_chars = g_pose_calls_node = g_pose_calls_node_armed = g_pose_calls_no_bones = 0;
  std::sort(worst.begin(), worst.end(), std::greater<>());
  for (size_t i = 0; i < worst.size() && i < 5; ++i) {
    const PoseTrack& t = g_pose[worst[i].second];
    const float dx = t.origin[0][0] - player[0], dy = t.origin[0][1] - player[1], dz = t.origin[0][2] - player[2];
    REXLOG_INFO("cod3: pose:   entity {} (model {:08x}) at {:.0f} {:.0f} {:.0f} ({:.0f} units away): drawn {}, A-B-A {}, "
                "changed without an update {}, unlike the update {}, jumped {}; flags {:08x} {:08x}{}",
                t.number, worst[i].second, t.origin[0][0], t.origin[0][1], t.origin[0][2], std::sqrt(dx * dx + dy * dy + dz * dz),
                t.frames, t.aba, t.between, t.after_update, t.jumps, t.flags, t.flags2,
                (t.flags & 0x40000000) ? " (a scripted scene's actor)" : "");
  }
  for (auto& [number, t] : g_pose) t.frames = t.aba = t.between = t.after_update = t.jumps = 0;
}

}  // namespace

// [rexglue-native npc] 2026-11-09 0x82421AA8(entity): G_RunFrame's tail (0x82563FC0) picks each animated entity's
// [rexglue-native npc] animation detail; with cod3_debug_server_step the choices are counted by the update's place in
// [rexglue-native npc] its frame (the coarsest: bit 0 of the model's +196 still set and entity +636 bit 1 clear, as the
// [rexglue-native npc] function tests it). The game's own call is unchanged.
REX_EXTERN(__imp__sub_82421AA8);
REX_HOOK_RAW(sub_82421AA8) {
  if (DebugOn() || FrameLinesOn()) {
    const uint32_t ent = ctx.r3.u32;
    const uint32_t model = ValidGuest(ent) ? g::ReadU32(base, ent + 508) : 0;
    if (ValidGuest(model)) {
      const int later = g_update_later ? 1 : 0;
      ++g_lod_models[later];
      if ((g::ReadU32(base, model + 196) & 1) && !((g::ReadU32(base, ent + 636) >> 1) & 1)) {
        ++g_lod_coarse[later];
        if (later) ++g_frame_coarse_later;
      }
    }
  }
  __imp__sub_82421AA8(ctx, base);
}

// [claude-9b] 2026-10-27 0x8253F438: G_RunFrame's last call (see cod3_server_step_draw).
REX_HOOK_RAW(sub_8253F438) {
  g_game_armed = true;
  __imp__sub_8253F438(ctx, base);
  if (REXCVAR_GET(cod3_debug_pose)) PoseAfterUpdate(base);  // [claude-9b] cod3_debug_pose
  // [claude-9b-think] 2026-10-27 UNTESTED cod3_blend_ride_bones: the ridden vehicle's bones as this update left them.
  {
    const uint32_t client = PlayerClient(base);
    cod3::ride_bones::AfterUpdate(ctx, base, client ? RiddenVehicle(base, client) : 0);
  }
}

// [claude-9b] 2026-10-27 cod3_debug_pose: the renderer's draw of a scene node, before it runs.
REX_EXTERN(__imp__sub_8242E450);
REX_HOOK_RAW(sub_8242E450) {
  const int32_t want = REXCVAR_GET(cod3_debug_pose);
  if (want != 0) PoseAtDraw(base, ctx.r3.u32, want);
  cod3::ride_bones::AtDraw(base, ctx.r3.u32);  // [claude-9b-think] cod3_blend_ride_bones: drawn inside the blend?
  __imp__sub_8242E450(ctx, base);
}

// [claude-9b] 2026-10-27 cod3_debug_pose: 0x82427D18(model, entity, ...), the characters' draw (it copies the bones
// [model +0x70] for the frame as the node draw's skinned path does).
REX_EXTERN(__imp__sub_82427D18);
REX_HOOK_RAW(sub_82427D18) {
  const int32_t want = REXCVAR_GET(cod3_debug_pose);
  if (want != 0) {
    {
      std::lock_guard<std::mutex> lock(g_pose_mutex);
      ++g_pose_calls_chars;
    }
    PoseCount(base, ctx.r3.u32, ctx.r4.u32, want);
  }
  __imp__sub_82427D18(ctx, base);
}

// CG_CalcEntityLerpPositions(entity): where the client game draws an entity this frame.
REX_EXTERN(__imp__CG_CalcEntityLerpPositions);
REX_HOOK_RAW(CG_CalcEntityLerpPositions) {
  const uint32_t ent = ctx.r3.u32;
  __imp__CG_CalcEntityLerpPositions(ctx, base);
  const bool debug = DebugOn() && ValidGuest(ent);
  if (debug) {
    ++g_draw[g_kind].lerps;
    if (g::ReadS32(base, ent + kEntPosType) == kTrInterpolate) ++g_draw[g_kind].lerp_interp;
  }
  if (g_clock.hz <= 0 || !REXCVAR_GET(cod3_server_smooth) || !ValidGuest(ent)) return;
  // [claude-9b-think] 2026-10-27 UNTESTED ride6: the ridden vehicle first, whatever its trajectory type; the trace
  // keeps the camera the scene was copied with (RE_RenderScene copies the refdef before it places the entities).
  const bool ridden = RideOn() && ent == g_ride.ent && ent != PlayerEntity(base);
  if (ridden) {
    const uint32_t client = g::ReadU32(base, kActiveLocalClient);
    if (client <= 3) {
      ReadVec(base, kCgBase + client * kCgSize + kCgViewOrigin, g_ride_trace_scam);
      g_ride_trace_scam_have = true;
    }
    g_ride_trace_vpath = 0;
    if (RideDrawVehicle(base, ent, g::ReadS32(base, ent + kEntPosType))) {
      g_ride_trace_vpath = 2;
      return;
    }
  }
  // [claude-9b] 2026-10-27 TR_STATIONARY too (cod3_server_smooth_stationary): drawn at its base, which the
  // game moves in its updates for parts carried by something that moves (a tank's turret and hatches,
  // things linked to a vehicle or a mover). Only TR_INTERPOLATE things (the vehicle itself) were drawn
  // between updates, one update behind, so the parts on them separated from them by up to one update's
  // motion (owner 10-26: "tank treads and tires ghosting, crate tops / turrets ... glitch";
  // claude-9b-think FINDINGS-2026-10-27 section 1). A base that doesn't change isn't touched.
  const int32_t pos_type = g::ReadS32(base, ent + kEntPosType);
  const bool stationary = pos_type == kTrStationary && REXCVAR_GET(cod3_server_smooth_stationary);
  if (pos_type != kTrInterpolate && !stationary) return;
  if (ent == PlayerEntity(base)) return;
  float now[3];
  ReadVec(base, ent + (stationary ? kEntTrBase : kEntServerOrigin), now);
  History& h = HistoryOf(base, ent);  // [acct2-cod3-motion] the flat table
  const int32_t number = g::ReadS32(base, ent + kEntNumber);
  if (h.number != number) {  // [claude-9b] another entity in this slot now
    h = History{};
    h.number = number;
  }
  const float back = Back();
  SmoothDrawnAngles(base, ent, h, back);  // [claude-9b] 2026-10-27
  if (!Advance(h, now)) return;
  if (debug) ++g_draw[g_kind].lerp_moved;
  if (back <= 0.0f) return;
  float drawn[3];
  ReadVec(base, ent + kEntDrawnOrigin, drawn);
  if (!Same(drawn, now)) return;  // not the plain copy the game makes for these: leave it
  Saved saved{ent, {drawn[0], drawn[1], drawn[2]}, {}};
  for (int i = 0; i < 3; ++i) drawn[i] = h.cur[i] + (h.prev[i] - h.cur[i]) * back;
  for (int i = 0; i < 3; ++i) saved.drawn[i] = drawn[i];
  g_saved.push_back(saved);
  WriteVec(base, ent + kEntDrawnOrigin, drawn);
  g::WriteU32(base, ent + kEntFlags, g::ReadU32(base, ent + kEntFlags) | kMovedFlag);
  ++g_counts.smoothed;
  if (ridden) g_ride_trace_vpath = 1;  // [claude-9b-think] ride6
}

// [claude-9b] 2026-10-27 Counting hooks for cod3_debug_server_step (they only count, and only with it
// on): what the client game draws in a frame without a game update.

// [claude-9b] cod3_debug_view_trace: one line a drawn frame while the player moves (after the frame's
// camera is final): the game clock, the frame's updates, the usercmd time, the client game's time, the
// player's playerState origin and command time, its entity's origin, the camera after 0x824505E8 and
// the camera drawn.
namespace {
int32_t g_trace_cmd_time = 0;
int32_t g_trace_sv = 0, g_trace_lead = 0;  // svs.time and Lead() as the frame's usercmd got them
void TraceFrame(uint8_t* base, int32_t cg_time) {
  static int logged = 0, last_setting = 0;
  const int setting = REXCVAR_GET(cod3_debug_view_trace);
  if (setting != last_setting) {
    logged = 0;
    last_setting = setting;
  }
  if (setting <= 0 || logged >= setting) return;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  const uint32_t ps = PlayerClient(base);
  if (client > 3 || !ps) return;
  float o[3], v[3], e[3], cam[3];
  ReadVec(base, ps + 0, o);
  ReadVec(base, ps + 16, v);
  // [claude-9b-think] on a vehicle the playerState's velocity may stay 0: the vehicle's move counts too.
  // [claude-9b-think] every frame while riding (any cod3_server_smooth_ride, so 0 and 1 compare).
  const uint32_t veh = RiddenVehicle(base, ps);
  // [acct2-cod3-motion] also every frame in a camera mode the update cameras smooth (cod3_server_smooth_linked / _cams:
  // a scripted ride where the player stands still on what carries him)
  const bool update_cam = g_cam_mode < 23 && (((uint32_t(REXCVAR_GET(cod3_server_smooth_linked)) |
                                                uint32_t(REXCVAR_GET(cod3_server_smooth_cams))) >> g_cam_mode) & 1u);
  if (v[0] * v[0] + v[1] * v[1] < 1.0f && !veh && !update_cam) return;  // not moving
  ReadVec(base, PlayerEntity(base) + kEntServerOrigin, e);
  ReadVec(base, kCgBase + client * kCgSize + kCgViewOrigin, cam);
  ++logged;
  REXLOG_INFO("cod3: view trace {} clk {} n {} cmd {} cg {} ct {} ps {:.2f} {:.2f} {:.2f} ent {:.2f} {:.2f} {:.2f} "
              "cam0 {:.2f} {:.2f} {:.2f} cam {:.2f} {:.2f} {:.2f} sv {} lt {} lead {} tick {}",
              logged, g_clock.time, g_kind ? g_draw_updates : 0, g_trace_cmd_time, cg_time,
              g::ReadS32(base, ps + 32), o[0], o[1], o[2], e[0], e[1], e[2], g_trace_cam[0], g_trace_cam[1],
              g_trace_cam[2], cam[0], cam[1], cam[2], g_trace_sv, g::ReadS32(base, kLevelTime), g_trace_lead,
              g_clock.tick);
  // [acct2-cod3-motion] the camera's smoothing this frame: mode, Back(), cod3_server_smooth_linked's state and offset,
  // cod3_server_smooth_cams drew it, the frame's milliseconds, the part of a millisecond the game's clock hasn't
  // counted (sub) and the real time this frame's clock was read, in microseconds (rt; for view_steps.py).
  REXLOG_INFO("cod3: view trace {} cam mode {} back {:.3f} linked {} off {:.2f} {:.2f} {:.2f} upcam {} ms {} sub {:.3f} "
              "rt {}", logged, g_cam_mode, Back(), g_linked_trace, g_linked_trace_off[0], g_linked_trace_off[1],
              g_linked_trace_off[2], g_upcam_trace ? 1 : 0, g_es_msec, g_subms.acc,
              static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                  g_subms.last.time_since_epoch()).count()));
  for (int i = 0; i < 3; ++i) g_linked_trace_off[i] = 0.0f;
  // [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_ride: one more line a frame on a vehicle. mode: the
  // camera's (0x82447D48); ride: -1 riding, nothing moved, 0 not used, 1 moved by off; drawn: the vehicle drawn from
  // the ride's pose (its yaw dyaw); dP: the vehicle's move in the last update; seat: the playerState origin's;
  // vturn / dturn: the view's and delta_angles' turn in it; y72 / y84 / y352: the yaw (second float) of the vectors at
  // +72 (+76), +84 (+88) and +352 (+356): the client's blend ends and the server's angles; aok: the drawn angles set;
  // p352 / r352 the server's pitch and roll; set: cod3_server_smooth_ride; fl: the seat's free look (entity +916).
// [claude-9b-think] ride6: scam: the camera the scene was drawn with (as the vehicle was placed); vd: the vehicle's
// drawn origin; loc: the scene's camera in the vehicle's drawn frame (forward, left, up), and lstep its change since
// the last frame (0 when the inside stays still on the screen); vpath: how the vehicle was placed (0 the game's own,
// 1 the other smoothing, 2 the ride's pose, -1 not this frame); b308: how far 0x82448308 moved the camera after
// SmoothView; v286: its view's +286 flags.
  if (veh) {
    float a72[3], a84[3], a352[3];
    ReadVec(base, veh + kEntAnglesPrev, a72);
    ReadVec(base, veh + kEntAnglesNow, a84);
    ReadVec(base, veh + kEntServerAngles, a352);
    // [claude-9b-think] ride6: the drawn origin and angles are still this frame's here (PutBackDrawn runs before the
    // next frame's usercmd).
    float vd[3], vang[3], vaxis[3][3], loc[3] = {0.0f, 0.0f, 0.0f};
    ReadVec(base, veh + kEntDrawnOrigin, vd);
    ReadVec(base, veh + kEntDrawnAngles, vang);
    AnglesToAxis(vang, vaxis);
    float lstep = -1.0f;
    if (g_ride_trace_scam_have) {
      float rel[3];
      for (int i = 0; i < 3; ++i) rel[i] = g_ride_trace_scam[i] - vd[i];
      for (int a = 0; a < 3; ++a) loc[a] = vaxis[a][0] * rel[0] + vaxis[a][1] * rel[1] + vaxis[a][2] * rel[2];
      if (g_ride_trace_rel_have) lstep = std::sqrt(Dist2(loc, g_ride_trace_rel_last));
      for (int i = 0; i < 3; ++i) g_ride_trace_rel_last[i] = loc[i];
      g_ride_trace_rel_have = true;
    }
    REXLOG_INFO("cod3: view trace {} ride mode {} ride {} back {:.3f} valid {} drawn {} dyaw {:.3f} off {:.2f} {:.2f} "
                "{:.2f} dP {:.2f} {:.2f} {:.2f} seat {:.2f} {:.2f} {:.2f} vturn {:.3f} {:.3f} {:.3f} dturn {:.3f} "
                "{:.3f} {:.3f} yaw prev {:.3f} cur {:.3f} y72 {:.3f} y84 {:.3f} view {:.3f} {:.3f} y352 {:.3f} "
                "turned {} pview {:.3f} {:.3f} aok {} p352 {:.3f} r352 {:.3f} set {} fl {:08X} dwhy {} ptype {} "
                "scam {:.2f} {:.2f} {:.2f} vd {:.2f} {:.2f} {:.2f} loc {:.2f} {:.2f} {:.2f} lstep {:.3f} vpath {} "
                "b308 {:.3f} v286 {:04X}",
                logged, g_cam_mode, g_ride_trace_state, Back(), g_ride.valid ? 1 : 0, g_ride_trace_drawn ? 1 : 0,
                g_ride_trace_yaw, g_ride_trace_off[0], g_ride_trace_off[1], g_ride_trace_off[2],
                g_ride.origin_cur[0] - g_ride.origin_prev[0], g_ride.origin_cur[1] - g_ride.origin_prev[1],
                g_ride.origin_cur[2] - g_ride.origin_prev[2], g_ride.seat_jump[0], g_ride.seat_jump[1],
                g_ride.seat_jump[2], g_ride.view_turn[0], g_ride.view_turn[1], g_ride.view_turn[2],
                g_ride.delta_turn[0], g_ride.delta_turn[1], g_ride.delta_turn[2], g_ride.angles_prev[1],
                g_ride.angles_cur[1], a72[1], a84[1], g::ReadF32(base, kCgBase + client * kCgSize + kCgViewAngles),
                g::ReadF32(base, kCgBase + client * kCgSize + kCgViewAngles + 4),
                a352[1], g_ride_trace_turned ? 1 : 0,  // [claude-9b-think]
                g::ReadF32(base, ps + kClientViewAngles), g::ReadF32(base, ps + kClientViewAngles + 4),
                g_ride_trace_angles_ok ? 1 : 0, a352[0], a352[2], REXCVAR_GET(cod3_server_smooth_ride),
                g::ReadU32(base, PlayerEntity(base) + kEntSeatFreeLook), g_ride_trace_draw_why,
                g::ReadS32(base, veh + kEntPosType),  // [claude-9b-think] why the vehicle wasn't drawn by the ride
                g_ride_trace_scam[0], g_ride_trace_scam[1], g_ride_trace_scam[2], vd[0], vd[1], vd[2], loc[0], loc[1],
                loc[2], lstep, g_ride_trace_vpath, g_ride_trace_b308, g_ride_trace_v286);
    // [claude-9b-think] 2026-10-27 UNTESTED ride9: the same camera in the frame the renderer draws the vehicle in (its
    // scene node), which loc above (the drawn +112 / +128) is not for a vehicle: etype the entity's type (14 a vehicle),
    // body how its node was placed (g_ride_trace_body), nd the node's origin, nloc the camera in the node's frame and
    // nlstep its change since the last frame (the inside's move on the screen; ~0 when it stays still).
    float nd[3] = {0.0f, 0.0f, 0.0f}, nloc[3] = {0.0f, 0.0f, 0.0f};
    float nlstep = -1.0f;
    const uint32_t node = g::ReadU32(base, veh + kEntSceneNode);
    if (ValidGuest(node) && g_ride_trace_scam_have) {
      float naxis[3][3], rel[3];
      ReadVec(base, node + kNodeOrigin, nd);
      for (int a = 0; a < 3; ++a) ReadVec(base, node + kNodeAxis + 12 * a, naxis[a]);
      for (int i = 0; i < 3; ++i) rel[i] = g_ride_trace_scam[i] - nd[i];
      for (int a = 0; a < 3; ++a) nloc[a] = naxis[a][0] * rel[0] + naxis[a][1] * rel[1] + naxis[a][2] * rel[2];
      if (g_ride_trace_nrel_have) nlstep = std::sqrt(Dist2(nloc, g_ride_trace_nrel_last));
      for (int i = 0; i < 3; ++i) g_ride_trace_nrel_last[i] = nloc[i];
      g_ride_trace_nrel_have = true;
    }
    REXLOG_INFO("cod3: view trace {} body etype {} body {} set {} nd {:.2f} {:.2f} {:.2f} nloc {:.2f} {:.2f} {:.2f} "
                "nlstep {:.3f}",
                logged, g::ReadU8(base, veh), g_ride_trace_body, REXCVAR_GET(cod3_server_smooth_body), nd[0], nd[1],
                nd[2], nloc[0], nloc[1], nloc[2], nlstep);
  }
  for (int i = 0; i < 3; ++i) g_ride_trace_off[i] = 0.0f;
}
}  // namespace

namespace {
void EntityStepFrame(uint8_t* base);   // [acct2-cod3-motion] cod3_debug_entity_steps (below)
void EntityStepCamera(uint8_t* base);  // [acct2-cod3-motion]
void CensusFrame(uint8_t* base);       // [acct2-cod3-motion] cod3_debug_drawn_census (below)
void CensusFrameDone(uint8_t* base);   // [acct2-cod3-motion]
}  // namespace

// CG_DrawActiveFrame(serverTime, ...): the client game's frame.
REX_EXTERN(__imp__CG_DrawActiveFrame);
REX_HOOK_RAW(CG_DrawActiveFrame) {
  EntityStepFrame(base);  // [acct2-cod3-motion] a new drawn frame for cod3_debug_entity_steps
  CensusFrame(base);      // [acct2-cod3-motion] and for cod3_debug_drawn_census
  // [claude-9b] the client game's clock between updates (see g_time_advancing).
  const int32_t in = ctx.r3.s32;
  int32_t out = in;
  const int hz = g_clock.hz;
  if (hz > 0 && REXCVAR_GET(cod3_server_client_time)) {
    out = in + Lead();
    if (in >= g_cg_last_in && out < g_cg_last_out) out = g_cg_last_out;  // never backwards
    ctx.r3.s32 = out;
  }
  g_cg_last_in = in;
  g_cg_last_out = out;
  if (DebugOn()) {
    DrawCounts& d = g_draw[g_kind];
    ++d.cg_frames;
    const int32_t t = out;
    if (t != g_last_cg_time) ++d.cg_time_moved;
    g_last_cg_time = t;
  }
  g_ride_trace_drawn = false;  // [claude-9b-think] cod3_debug_view_trace: set again by this frame's draw
  g_ride_trace_draw_why = 5;
  g_ride_trace_scam_have = false;  // [claude-9b-think] ride6
  g_ride_trace_vpath = -1;
  g_ride_trace_b308 = 0.0f;
  g_ride_trace_v286 = 0;
  g_ride_trace_body = -1;  // [claude-9b-think] ride9
  // [claude-9b-think] 2026-10-27 UNTESTED cod3_blend_ride_bones: the ridden vehicle's turret and gun between its last
  // two updates by Back(), as its hull and the camera are, before the camera reads tag_turret; PutBackDrawn undoes it.
  {
    const uint32_t client = g_clock.hz > 0 && REXCVAR_GET(cod3_server_smooth) ? PlayerClient(base) : 0;
    cod3::ride_bones::Blend(base, client ? RiddenVehicle(base, client) : 0, Back());
  }
  __imp__CG_DrawActiveFrame(ctx, base);
  EntityStepCamera(base);  // [acct2-cod3-motion] cod3_debug_entity_steps: the frame's camera
  cod3::movement::EndDrawnStepValues(base);  // [claude-9b] the game's aim and lean back after the client game's frame
  TraceFrame(base, out);
  CensusFrameDone(base);  // [acct2-cod3-motion] cod3_debug_drawn_census: the camera, the carried people (after the view
                          // trace, so a drawn trace line follows its frame's view trace line)
  PoseWorldLines(base);   // [rexglue-native npc] cod3_debug_pose_bones_near: the frame's scene nodes as drawn
  NearEntitiesLines(base);  // [rexglue-native-bugs] 2026-11-11 nl1 cod3_debug_near_entities
}

// [claude-9b-think] 2026-10-27 UNTESTED ride6, cod3_debug_view_trace only: 0x82448308(view), the last camera step of
// CG_DrawActiveFrame before the scene (after CG_CalcViewValues and SmoothView, after the gun is placed): it calls the
// view's bone follow 0x824411D8 (view +286 bit 0: the camera's origin and angles turned by a model's bone matrix,
// model = gclient +132, bone = view +288, or the one kept at view +304), 0x82448268 (a camera blend 0x82447A78 while
// view +276 < +280, then the axis from the angles) and 0x82432878 (the camera copied into view +48..+88); modes 16 / 18
// take the camera from elsewhere. The trace gets how far it moved cg +4480.
REX_EXTERN(__imp__sub_82448308);
REX_HOOK_RAW(sub_82448308) {
  const uint32_t view = ctx.r3.u32;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (REXCVAR_GET(cod3_debug_view_trace) <= 0 || client > 3) {
    __imp__sub_82448308(ctx, base);
    return;
  }
  const uint32_t cam = kCgBase + client * kCgSize + kCgViewOrigin;
  float before[3], after[3];
  ReadVec(base, cam, before);
  g_ride_trace_v286 = ValidGuest(view) ? g::ReadU16(base, view + 286) : 0;
  __imp__sub_82448308(ctx, base);
  ReadVec(base, cam, after);
  g_ride_trace_b308 = std::sqrt(Dist2(before, after));
}

// [claude-9b] 2026-10-27 The player's usercmds between updates (owner 10-27 03:03: "Player movement has
// regressed massively again. Likely because of the 60hz changes", 03:04: "the player is bouncing").
// CL_Frame (0x82511B28) makes the frame's usercmd (CL_CreateNewCommands -> CL_CreateCmd ->
// CL_FinishMove) with the client's server time (0x82A57900 + 1392, 7280 bytes a local client), runs it
// (SV_ClientThink) and then copies the server's time into it (svs.time 0x829C3A30, which SV_Frame
// advances) for the next frame. With updates of 1/hz s that time stood still between updates, so each
// usercmd but the first after an update had no time and was dropped: runs c9b-walk2-hz60 / -hz0
// (saint_lo, ~240 fps, cod3_debug_movement) moved the player with 60 usercmds a second instead of
// ~210, and the camera moved and turned only in the frame after each update (the view turned in steps
// of the update rate, and the movement steps' camera smoothing had no time to go by). Now each usercmd
// gets the server's time plus how far the game's clock is into the next update, as the client game's
// clock (cod3_server_client_time), never going back.
namespace {
constexpr uint32_t kSvTime = 0x829C3A30;                 // svs.time
constexpr uint32_t kClServerTime = 0x82A57900 + 1392;    // the client's server time: the usercmd's
constexpr uint32_t kClSize = 7280;                       // a local client's
int32_t g_cmd_last_sv = 0, g_cmd_last_out = 0;
}  // namespace
REX_EXTERN(__imp__CL_CreateNewCommands);
REX_HOOK_RAW(CL_CreateNewCommands) {
  PutBackDrawn(base);  // before the usercmd runs (game code)
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (g_clock.hz > 0 && REXCVAR_GET(cod3_server_client_time) && client <= 3) {
    const uint32_t at = kClServerTime + client * kClSize;
    const int32_t sv = g::ReadS32(base, kSvTime);
    int32_t t = sv + Lead();
    if (sv >= g_cmd_last_sv && t < g_cmd_last_out) t = g_cmd_last_out;  // never backwards
    g::WriteU32(base, at, static_cast<uint32_t>(t));
    g_cmd_last_sv = sv;
    g_cmd_last_out = t;
  }
  if (client <= 3) g_trace_cmd_time = g::ReadS32(base, kClServerTime + client * kClSize);  // [claude-9b] the trace
  g_trace_sv = g::ReadS32(base, kSvTime);
  g_trace_lead = Lead();
  __imp__CL_CreateNewCommands(ctx, base);
}

// [claude-9b-think] 2026-11-02 UNTESTED The player's movement steps against the game's updates (claude-9b's lead from
// the tester's ob25 Mace runs: the player read ~15% faster uncapped). Both are 1/60 s with the same 16, 17, 17 ms
// pattern, but on different origins: the steps end at fixed times of the usercmd clock (movement/hooks_sp.cpp
// RunHzSteps, rules.h MoveStepEndAfter: k * 1000 / 60), the updates where the game's clock has run whole updates since
// it started (svs.time at that start plus TickEnd(k)). So they are out of phase by svs.time's start modulo the
// pattern (any 0-16 ms, fixed until the game's clock starts again). Each frame runs its updates first and then its
// usercmd (CL_Frame after 0x823FFC10's / SV_Frame's updates), so an update sees the steps up to the last frame's usercmd
// time; above 60 fps that time is anywhere in the last frame before the update, and a step that ends within one frame
// time of the update's end is seen in that update or the next as the frame times fall: 0 or 2 steps in an update
// instead of 1 (about a quarter of the possible phases at ~240 fps, half at ~120 fps; at 60 fps the frames and the
// updates mostly coincide). With the steps' ends on the updates' ends, the step that ends with update k is run by the
// first usercmd after it, and update k + 1 sees exactly that one more step, at any frame rate above the update rate
// (below it, two updates in one frame see the same usercmd, as before).
// Not changed: the steps (60 a second, 16, 17, 17 ms), the usercmds and their input, the view angles (every usercmd),
// the camera between steps (it counts from the step's own end); the steps only start up to one step earlier or later,
// once. The CoD4 style keeps its own 8 ms steps (they can't end on 1/60 s updates); multiplayer has no fixed updates
// here (cod3mp is its own program, not touched).
REXCVAR_DEFINE_BOOL(cod3_move_hz_sync, true, "cod3/Movement",
                    "The player's movement steps (cod3_move_hz) end where the game's updates (cod3_server_hz) end, so "
                    "each update sees one more step at any frame rate (the AI and everything else the update reads); "
                    "the same steps, moved by less than one step. Off: the steps end at fixed times of the usercmd "
                    "clock, and above 60 fps an update can see 0 or 2 steps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::server_step {

int MoveStepPhase(uint8_t* base, int move_hz) {
  const int hz = g_clock.hz;
  if (!REXCVAR_GET(cod3_move_hz_sync) || hz <= 0 || move_hz <= 0 || move_hz % hz != 0) return 0;
  int a = 1000, b = hz;  // gcd(1000, hz)
  while (b != 0) {
    const int r = a % b;
    a = b;
    b = r;
  }
  const int64_t period = 1000 / a;  // the update ends' pattern repeats every period ms (50 at 60: 16, 17, 17)
  // The last update ended at svs.time, at TickEnd(tick) of the game's clock; the usercmds' times are svs.time plus the
  // game clock's lead (CL_CreateNewCommands above). So update k ends at origin + TickEnd(k) on the usercmd clock.
  const int64_t origin = int64_t(cod3::guest::ReadS32(base, kSvTime)) - TickEnd(g_clock.tick, hz);
  return static_cast<int>(((origin % period) + period) % period);
}

}  // namespace cod3::server_step

// CG_AddPacketEntities(): the entities into the scene (RE_RenderScene calls it). Its list read as it
// reads it, before it runs.
REX_EXTERN(__imp__CG_AddPacketEntities);
REX_HOOK_RAW(CG_AddPacketEntities) {
  if (DebugOn()) {
    DrawCounts& d = g_draw[g_kind];
    ++d.lists;
    const uint32_t snap = g::ReadU32(base, kLevelSnapTime);
    if (snap != g_last_snap_time) ++d.snap_time_moved;
    g_last_snap_time = snap;
    const uint32_t count = std::min<uint32_t>(g::ReadU32(base, kEntListCount), 0x4000);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = g::ReadU32(base, kEntList + 4 * i);
      if (!ValidGuest(e)) continue;
      ++d.list_items;
      const uint32_t linked = g::ReadU32(base, e + 548);
      if (!ValidGuest(linked)) continue;
      ++d.linked;
      if (g::ReadU32(base, linked + 256) == snap) ++d.stamped;
    }
  }
  __imp__CG_AddPacketEntities(ctx, base);
}

// [acct2-cod3-motion] 2026-11-07 (owner 20:31, St-Bert's intro at cod3_max_fps 250: "still doesn't look smooth. The
// npc's jolt around and lots of scripts telelporting"; 20:32 "Check all vehicle based intros"). What the camera line of
// cod3_debug_server_step does for the camera (acct2-ocr's tools/cam_steps.py reads it), for every entity the client
// game draws: its scene node (entity +548) as the renderer gets it, after the game's draw and the smoothing above
// (both inside CG_AddCEntity's call): the origin (node +68) and the forward axis (node +28). Per entity, from one drawn
// frame to the next: how far it moved and turned, in frames with a game update and in frames without one (a thing
// drawn in steps of the update rate moves only in the first kind), how often it moved back against the last frame's
// move (A-B-A: a jolt), moves far above its usual one (spikes) and moves of more than kEsJump units (a teleport or a
// cut). Once a second (game time) one line per entity type and, with N > 1, the N worst entities. Counting only:
// nothing drawn or run changes.
REXCVAR_DEFINE_INT32(cod3_debug_entity_steps, 0, "cod3",
                     "Log once a second, by entity type, how the drawn entities moved between frames with and without "
                     "a game update (moving only with updates = stepping), jolts back and forth and jumps; N > 1: also "
                     "the N worst entities. 0: off")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
struct EntitySteps {
  int32_t number = -1;
  uint8_t type = 0;
  uint64_t frame = 0;  // the drawn frame it was last seen in (0: never)
  float origin[3] = {}, fwd[3] = {}, move[3] = {};
  bool have_move = false;
  float usual = 0.0f;  // its usual move a frame (units, a slow average)
  uint32_t samples = 0;
  // This second: [0] frames without an update, [1] with one or more.
  uint32_t n[2] = {}, ms[2] = {};
  double mv[2] = {}, tr[2] = {};
  uint32_t reversals = 0, spikes = 0, jumps = 0;
  uint32_t still = 0;  // frames it didn't move at all
  float max_jump = 0.0f;
  int jump_kind = -1;
};
std::unordered_map<uint32_t, EntitySteps> g_esteps;  // by entity address
uint64_t g_es_frame = 0;                             // drawn frames (CG_DrawActiveFrame)
uint64_t g_es_frames[2] = {};                        // this second's, by kind
int64_t g_es_since = 0;
constexpr float kEsJump = 32.0f;     // more in one frame: a jump (teleport, cut)
constexpr float kEsMoving = 5.0f;    // units a second: below it an entity counts as still
constexpr float kEsReverse = 0.05f;  // the smallest move a reversal is counted for (units)

float Len(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

constexpr uint8_t kEsCameraType = 16;  // the camera, kept as one more "entity" (key 0)

// One drawn place this frame: `key` the entity's address (0 for the camera).
void EntityStepPoint(uint32_t key, int32_t number, uint8_t type, const float o[3], const float f[3]) {
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(o[i]) || !std::isfinite(f[i])) return;
  EntitySteps& s = g_esteps[key];
  if (s.number != number || s.type != type) {  // another entity in this slot now
    s = EntitySteps{};
    s.number = number;
    s.type = type;
  }
  if (s.frame != 0 && s.frame + 1 == g_es_frame) {
    const float m[3] = {o[0] - s.origin[0], o[1] - s.origin[1], o[2] - s.origin[2]};
    const float dist = Len(m);
    const float fl = Len(f), sl = Len(s.fwd);
    float turn = 0.0f;
    if (fl > 0.0f && sl > 0.0f) {
      const float c = (f[0] * s.fwd[0] + f[1] * s.fwd[1] + f[2] * s.fwd[2]) / (fl * sl);
      turn = std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f;
    }
    // A jump: far more than its usual move (a fast thing moves more than kEsJump a frame at low frame rates).
    if (dist > kEsJump && dist > (s.samples >= 30 ? 3.0f * s.usual + 16.0f : 128.0f)) {
      ++s.jumps;
      if (dist > s.max_jump) {
        s.max_jump = dist;
        s.jump_kind = g_kind;
      }
      s.have_move = false;
    } else {
      const int k = g_kind ? 1 : 0;
      ++s.n[k];
      s.ms[k] += static_cast<uint32_t>(std::max(g_es_msec, 0));
      s.mv[k] += dist;
      s.tr[k] += turn;
      if (dist < 0.001f) ++s.still;
      if (s.samples > 30 && dist > 2.0f + 6.0f * s.usual) ++s.spikes;
      if (s.have_move) {
        const float last = Len(s.move);
        const float dot = m[0] * s.move[0] + m[1] * s.move[1] + m[2] * s.move[2];
        if (dist > kEsReverse && last > kEsReverse && dot < -0.5f * dist * last) ++s.reversals;
      }
      s.usual = s.samples ? 0.95f * s.usual + 0.05f * dist : dist;
      ++s.samples;
      for (int i = 0; i < 3; ++i) s.move[i] = m[i];
      s.have_move = true;
    }
  } else {
    s.have_move = false;
  }
  for (int i = 0; i < 3; ++i) {
    s.origin[i] = o[i];
    s.fwd[i] = f[i];
  }
  s.frame = g_es_frame;
}

void EntityStepSample(uint8_t* base, uint32_t ent) {
  const uint32_t node = g::ReadU32(base, ent + kEntSceneNode);
  if (!ValidGuest(node)) return;
  float o[3], f[3];
  ReadVec(base, node + kNodeOrigin, o);
  ReadVec(base, node + kNodeAxis, f);
  EntityStepPoint(ent, g::ReadS32(base, ent + kEntNumber), g::ReadU8(base, ent) & 15, o, f);
}

// After the client game's frame: the camera it was drawn from (origin cg +4480, forward cg +4496).
void EntityStepCamera(uint8_t* base) {
  if (REXCVAR_GET(cod3_debug_entity_steps) <= 0) return;
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  if (local > 3) return;
  const uint32_t cg = kCgBase + local * kCgSize;
  float o[3], f[3];
  ReadVec(base, cg + kCgViewOrigin, o);
  ReadVec(base, cg + kCgViewAxis, f);
  EntityStepPoint(0, -2, kEsCameraType, o, f);
}

// Units (or degrees) a second in this second's frames of one kind.
float EsSpeed(const EntitySteps& s, int k, bool turn) {
  if (!s.ms[k]) return 0.0f;
  return float((turn ? s.tr[k] : s.mv[k]) * 1000.0 / double(s.ms[k]));
}

// Moving only in frames with an update: drawn in steps of the update rate.
bool EsStepped(const EntitySteps& s) {
  const float with = EsSpeed(s, 1, false), without = EsSpeed(s, 0, false);
  return s.n[0] >= 5 && s.n[1] >= 5 && with > kEsMoving && without < with / 3.0f;
}

void EntityStepLog(uint8_t* base) {
  const int setting = REXCVAR_GET(cod3_debug_entity_steps);
  struct TypeSum {
    uint32_t drawn = 0, moving = 0, stepped = 0, uneven = 0, reversing = 0;
    uint32_t reversals = 0, spikes = 0, jumps = 0, still = 0, frames = 0;
    float max_jump = 0.0f;
    double with = 0.0, without = 0.0;
  };
  TypeSum t[17];  // the entity types, then the camera
  struct Worst {
    float score;
    uint32_t ent;
  };
  std::vector<Worst> worst;
  for (auto it = g_esteps.begin(); it != g_esteps.end();) {
    EntitySteps& s = it->second;
    if (s.frame + 2000 < g_es_frame) {  // not drawn for a while: forgotten
      it = g_esteps.erase(it);
      continue;
    }
    if (s.n[0] + s.n[1] + s.jumps > 0) {
      TypeSum& ts = t[s.type <= kEsCameraType ? s.type : 0];
      ++ts.drawn;
      const float with = EsSpeed(s, 1, false), without = EsSpeed(s, 0, false);
      const bool moving = with > kEsMoving || without > kEsMoving;
      if (moving) {
        ++ts.moving;
        ts.with += with;
        ts.without += without;
        ts.still += s.still;  // a moving thing standing still in some frames: it moves in jerks
        ts.frames += s.n[0] + s.n[1];
        if (EsStepped(s))
          ++ts.stepped;
        else if (s.n[0] >= 5 && s.n[1] >= 5 && (without < with * 0.7f || without > with * 1.4f))
          ++ts.uneven;
      }
      if (s.reversals >= 5) ++ts.reversing;
      ts.reversals += s.reversals;
      ts.spikes += s.spikes;
      ts.jumps += s.jumps;
      ts.max_jump = std::max(ts.max_jump, s.max_jump);
      const float score = std::max(0.0f, with - without) + 2.0f * float(s.reversals) + 5.0f * float(s.spikes) +
                          20.0f * float(s.jumps);
      if (setting > 1 && score > 1.0f) worst.push_back(Worst{score, it->first});
    }
    ++it;
  }
  REXLOG_INFO("cod3: entity steps: {} frames with an update, {} without; camera mode {}", g_es_frames[1],
              g_es_frames[0], g_cam_mode);
  for (int k = 0; k <= kEsCameraType; ++k) {
    const TypeSum& ts = t[k];
    if (!ts.drawn) continue;
    REXLOG_INFO("cod3: entity steps: type {} drawn {} moving {} stepped {} uneven {} reversing {}; speed with an update "
                "{:.1f} without {:.1f} u/s; reversals {} spikes {} jumps {} (max {:.0f}); still in {} of {} frames",
                k, ts.drawn, ts.moving, ts.stepped, ts.uneven, ts.reversing,
                ts.moving ? ts.with / ts.moving : 0.0, ts.moving ? ts.without / ts.moving : 0.0, ts.reversals,
                ts.spikes, ts.jumps, ts.max_jump, ts.still, ts.frames);
  }
  std::sort(worst.begin(), worst.end(), [](const Worst& a, const Worst& b) { return a.score > b.score; });
  for (size_t i = 0; i < worst.size() && i < size_t(setting); ++i) {
    const uint32_t ent = worst[i].ent;
    const EntitySteps& s = g_esteps[ent];
    const bool real = ValidGuest(ent);  // 0: the camera
    REXLOG_INFO("cod3: entity steps: worst {} ent {} type {} pt {} at {} fl {:08X} with {} {:.1f} u/s {:.1f} deg/s "
                "without {} {:.1f} u/s {:.1f} deg/s stepped {} rev {} spk {} jmp {} max {:.0f} kind {} "
                "o {:.0f} {:.0f} {:.0f} still {}",
                i + 1, s.number, s.type, real ? g::ReadS32(base, ent + kEntPosType) : -1,
                real ? g::ReadS32(base, ent + 0x3C) : -1, real ? g::ReadU32(base, ent + kEntFlags) : 0u, s.n[1], EsSpeed(s, 1, false), EsSpeed(s, 1, true), s.n[0],
                EsSpeed(s, 0, false), EsSpeed(s, 0, true), EsStepped(s) ? 1 : 0, s.reversals, s.spikes, s.jumps,
                s.max_jump, s.jump_kind, s.origin[0], s.origin[1], s.origin[2], s.still);
  }
  for (auto& kv : g_esteps) {
    EntitySteps& s = kv.second;
    s.n[0] = s.n[1] = s.ms[0] = s.ms[1] = 0;
    s.mv[0] = s.mv[1] = s.tr[0] = s.tr[1] = 0.0;
    s.reversals = s.spikes = s.jumps = s.still = 0;
    s.max_jump = 0.0f;
    s.jump_kind = -1;
  }
  g_es_frames[0] = g_es_frames[1] = 0;
}

// Once a drawn frame, before its entities are drawn.
void EntityStepFrame(uint8_t* base) {
  if (REXCVAR_GET(cod3_debug_entity_steps) <= 0) {
    if (!g_esteps.empty()) g_esteps.clear();
    return;
  }
  ++g_es_frame;
  ++g_es_frames[g_kind ? 1 : 0];
  if (g_clock.time - g_es_since >= 1000 || g_clock.time < g_es_since) {
    EntityStepLog(base);
    g_es_since = g_clock.time;
  }
}
}  // namespace

// [acct2-cod3-motion] 2026-11-07 (owner 22:03: "When going over the bridge the npcs get raised in the air"; 22:04: "There
// are likely other issues similar to that"). A census of where each entity is drawn against where the game has it.
// After CG_AddCEntity's draw (and the smoothing inside it), the entity's scene node origin (the drawn place) against its
// server origin (+336) at the game's last two updates: drawn between updates, a thing stays on the line between those
// two places; off it by more than kCensusOff units it floats, sinks, slides or pops where the game doesn't have it
// (ours, where the game draws its node at +336). People carried by a vehicle (the rides-a-vehicle flag, or a vehicle
// within kCensusCarry units that moved with them in the update) also get their place in the vehicle's own frame, drawn
// (against the vehicle's drawn node) and in the game (against its server origin and angles): a drawn offset that differs
// from the game's is ours; a game offset that changes is the game's (an animation, or the game moving them on the
// vehicle). And the game's own jumps (more than kMaxSmoothedMove in one update: a teleport or a cut). Once a second
// (game time) one line, and with N > 1 the N worst entities (tools/drawn_census.py reads them). Counting only.
REXCVAR_DEFINE_INT32(cod3_debug_drawn_census, 0, "cod3",
                     "Log once a second where entities are drawn against where the game has them: off the game's path "
                     "(floating, sinking, sliding, popping), people drawn off the vehicle that carries them, and the "
                     "game's own jumps; N > 1: also the N worst entities. 0: off")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] One line a drawn frame for one entity (its number + 1): its drawn and game places, the game's last
// two, and its offsets on its carrier, drawn and in the game.
REXCVAR_DEFINE_INT32(cod3_debug_drawn_trace, 0, "cod3",
                     "With cod3_debug_drawn_census, log in every drawn frame where entity N-1 is drawn and where the "
                     "game has it (0: off)")
    .range(0, 4097)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] A second entity for the same trace (a rider's vehicle, say): its number + 1.
REXCVAR_DEFINE_INT32(cod3_debug_drawn_trace_also, 0, "cod3",
                     "With cod3_debug_drawn_trace, also trace entity N-1 (0: none)")
    .range(0, 4097)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-cod3-motion] 2026-11-07 (coordinator 22:55: find at run time what switches the St-Bert rider from sitting to
// standing on his seat at the bridge's crest, without decompiling the level) The traced entity's memory, once a game
// update: its first kDiffBytes bytes, the same at each pointer in them (the model at +0x1FC, the actor, its animation
// objects), and at each pointer in its model. A 32-bit word that changes after staying the same for at least N ms of
// game time is logged ("cod3: drawn diff"): the state the game changed (an animation, a flag, a link), not the
// counters and places that change all the time.
REXCVAR_DEFINE_INT32(cod3_debug_drawn_trace_diff, 0, "cod3",
                     "With cod3_debug_drawn_trace, also watch that entity's memory and the structures it points to "
                     "once a game update, and log each 32-bit word that changes after staying the same for at least "
                     "N ms (0: off)")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
constexpr float kCensusOff = 2.0f;      // units off the game's path (or its carrier) that count
constexpr float kCensusCarry = 256.0f;  // how near a vehicle that moved with a person in the update carries them

struct Census {
  int32_t number = -1;
  uint8_t type = 0;
  uint64_t update = 0;
  bool valid = false;
  float prev[3] = {}, cur[3] = {};  // the server origin at the last two updates
  uint64_t frame = 0;               // the drawn frame it was last seen in
  float drawn[3] = {}, game[3] = {}, angles[3] = {}, axis[3][3] = {};  // that frame's
  float off[3] = {};                // that frame's drawn place less the nearest point of the game's path
  float back = 0.0f;                // that frame's Back()
  uint32_t flags = 0, carrier = 0;  // carrier: the vehicle it rides (the flag); found at the frame's end otherwise
  int carried_by = 0;               // 1 the flag, 2 a vehicle that moved with it
  int32_t pos_type = -1;            // the trajectory type its game place was read by
  // this second
  uint32_t n = 0, offs = 0, jumps = 0, carried = 0, rel_offs = 0, pt_changes = 0;
  float max_off = 0.0f, max_up = 0.0f, max_down = 0.0f, max_jump = 0.0f, max_rel = 0.0f, rel_z = 0.0f;
  float local_min = 1e9f, local_max = -1e9f;  // its game height in its carrier's frame
  float at[3] = {}, at_game_z = 0.0f;         // its worst frame: where it was drawn, the game's height
  int32_t carrier_number = -1;
  // [acct2-cod3-motion] 2026-11-07 a carried person's pose (the bones' heights in the model's space; the St-Bert riders
  // stood up on their seats at the bridge): the last drawn frame's lowest bone, and this second's rises (the lowest bone
  // up more than kPoseRise in one drawn frame: the whole body lifted the moment an animation starts) and frames with
  // the feet up at its origin (lowest bone above kFeetUp: standing on the point it rides at, where a seated rider's feet
  // hang some 38 units below it)
  uint64_t pose_frame = 0;
  float pose_min = 0.0f;
  uint32_t pose_rises = 0, feet_up = 0;
  float max_rise = 0.0f;
};
constexpr float kPoseRise = 20.0f;
constexpr float kFeetUp = -12.0f;
std::unordered_map<uint32_t, Census> g_census;  // by entity address
uint64_t g_census_frame = 0;
uint32_t g_census_frames = 0;  // this second's
int64_t g_census_since = 0;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool IsVehicleType(uint8_t type) { return type == 14 || type == 15; }

// After CG_AddCEntity put the entity into the scene: where it went and where the game has it. The game's place is
// what its draw fills the node from: the server origin (+336; people and vehicles always, TR_INTERPOLATE things by the
// plain copy at +112), for a TR_STATIONARY thing its trajectory's base (+32), and for a thing the client game moves by
// its own trajectory (falling, thrown, flying: any other type) the place the client game worked out (+112, which no
// smoothing here touches), a point rather than a path.
void CensusSample(uint8_t* base, uint32_t ent) {
  const uint32_t node = g::ReadU32(base, ent + kEntSceneNode);
  if (!ValidGuest(node)) return;
  const uint8_t type = g::ReadU8(base, ent) & 15;
  const int32_t pos_type = g::ReadS32(base, ent + kEntPosType);
  const bool by_server = type == 1 || IsVehicleType(type);
  const bool at_base = pos_type == kTrStationary && !by_server;
  const bool own_path = pos_type != kTrStationary && pos_type != kTrInterpolate && !by_server;
  float d[3], now[3], ang[3];
  ReadVec(base, node + kNodeOrigin, d);
  ReadVec(base, ent + (at_base ? kEntTrBase : own_path ? kEntDrawnOrigin : kEntServerOrigin), now);
  ReadVec(base, ent + kEntServerAngles, ang);
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(d[i]) || !std::isfinite(now[i]) || !std::isfinite(ang[i])) return;
  const int32_t number = g::ReadS32(base, ent + kEntNumber);
  Census& c = g_census[ent];
  if (c.number != number || c.type != type) {  // another entity in this slot now
    c = Census{};
    c.number = number;
    c.type = type;
  }
  if (c.valid && c.pos_type != pos_type) {  // drawn by another trajectory type now: its place starts over
    ++c.pt_changes;
    c.valid = false;
  }
  if (own_path) c.valid = false;  // a point each frame: where the client game put it
  c.pos_type = pos_type;
  if (!c.valid || c.update != g_clock.updates) {  // the first draw since an update: that update's place
    const bool next = c.valid && c.update + 1 == g_clock.updates;
    const float moved2 = next ? Dist2(c.cur, now) : 0.0f;
    const bool jump = moved2 >= kMaxSmoothedMove * kMaxSmoothedMove;
    if (jump) {
      ++c.jumps;
      c.max_jump = std::max(c.max_jump, std::sqrt(moved2));
    }
    for (int i = 0; i < 3; ++i) {
      c.prev[i] = next && !jump ? c.cur[i] : now[i];
      c.cur[i] = now[i];
    }
    c.update = g_clock.updates;
    c.valid = true;
  } else if (!Same(now, c.cur)) {  // moved by the game since the update: it is drawn where it is now
    for (int i = 0; i < 3; ++i) c.prev[i] = c.cur[i] = now[i];
  }
  // The drawn place against the nearest point of the game's path between its last two places.
  float ab[3], ad[3];
  for (int i = 0; i < 3; ++i) {
    ab[i] = c.cur[i] - c.prev[i];
    ad[i] = d[i] - c.prev[i];
  }
  const float l2 = Dot(ab, ab);
  const float t = l2 > 0.0f ? std::clamp(Dot(ad, ab) / l2, 0.0f, 1.0f) : 0.0f;
  for (int i = 0; i < 3; ++i) c.off[i] = d[i] - (c.prev[i] + ab[i] * t);
  const float off = Len(c.off);
  ++c.n;
  if (off > kCensusOff) ++c.offs;
  if (off > c.max_off) {
    c.max_off = off;
    for (int i = 0; i < 3; ++i) c.at[i] = d[i];
    c.at_game_z = now[2];
  }
  c.max_up = std::max(c.max_up, c.off[2]);
  c.max_down = std::min(c.max_down, c.off[2]);
  for (int i = 0; i < 3; ++i) {
    c.drawn[i] = d[i];
    c.game[i] = now[i];
    c.angles[i] = ang[i];
    for (int a = 0; a < 3; ++a) c.axis[a][i] = g::ReadF32(base, node + kNodeAxis + 12 * a + 4 * i);
  }
  c.flags = g::ReadU32(base, ent + kEntFlags);
  c.carrier = 0;
  c.carried_by = 0;
  if (c.flags & kEntOnVehicleFlag) {
    const uint32_t veh = EntityOfHandle(base, g::ReadU32(base, ent + kEntVehicleRidden));
    if (veh && veh != ent) {
      c.carrier = veh;
      c.carried_by = 1;
    }
  }
  c.back = Back();
  c.frame = g_census_frame;
}

// The carrier of `c` this frame (0: none): the flagged vehicle, or the nearest vehicle drawn this frame that moved with
// it in the same update.
uint32_t CensusCarrier(const Census& c, uint32_t self) {
  if (c.carried_by == 1) {
    const auto it = g_census.find(c.carrier);
    return it != g_census.end() && it->second.frame == c.frame ? c.carrier : 0;
  }
  const float move[3] = {c.cur[0] - c.prev[0], c.cur[1] - c.prev[1], c.cur[2] - c.prev[2]};
  if (Len(move) < 0.5f) return 0;
  uint32_t best = 0;
  float best_d2 = kCensusCarry * kCensusCarry;
  for (const auto& kv : g_census) {
    const Census& v = kv.second;
    if (kv.first == self || !IsVehicleType(v.type) || v.frame != c.frame || v.update != c.update) continue;
    const float vm[3] = {v.cur[0] - v.prev[0], v.cur[1] - v.prev[1], v.cur[2] - v.prev[2]};
    const float dm[3] = {move[0] - vm[0], move[1] - vm[1], move[2] - vm[2]};
    if (Len(dm) > 0.5f) continue;
    const float d2 = Dist2(c.game, v.game);
    if (d2 < best_d2) {
      best_d2 = d2;
      best = kv.first;
    }
  }
  return best;
}

void CensusTrace(const Census& c, const Census* v, const float gl[3], const float dl[3]) {
  REXLOG_INFO("cod3: drawn trace {} ent {} type {} back {:.3f} drawn {:.2f} {:.2f} {:.2f} game {:.2f} {:.2f} {:.2f} "
              "prev {:.2f} {:.2f} {:.2f} off {:.2f} {:.2f} {:.2f} carrier {} ({}) game local {:.2f} {:.2f} {:.2f} "
              "drawn local {:.2f} {:.2f} {:.2f} carrier angles {:.1f} {:.1f} {:.1f}",
              c.frame, c.number, c.type, c.back, c.drawn[0], c.drawn[1], c.drawn[2], c.game[0], c.game[1],
              c.game[2], c.prev[0], c.prev[1], c.prev[2], c.off[0], c.off[1], c.off[2], v ? v->number : -1,
              v ? c.carried_by : 0, gl[0], gl[1], gl[2], dl[0], dl[1], dl[2], v ? v->angles[0] : 0.0f,
              v ? v->angles[1] : 0.0f, v ? v->angles[2] : 0.0f);
}

// [acct2-cod3-motion] 2026-11-07 (owner 22:24 via the coordinator: the riders in his own truck, both renderers) The
// frame's camera (cg +4480 origin, +4544 angles, +4496 axis), after CG_DrawActiveFrame, for the view trace below.
float g_cens_cam[3] = {}, g_cens_cam_ang[3] = {}, g_cens_cam_ax[3][3] = {};

// With cod3_debug_drawn_trace: the traced person against the camera and the carrier, as drawn: the camera's place in the
// carrier's frame (it rides the same truck: constant if the two are drawn alike), the person's place in the camera's
// frame (forward, left, up) and how high it looks (degrees above the view's level), and the person's up axis in the
// carrier's frame (does its pose tilt with the truck), with the game's pitch and roll of both (+352).
// Its pose: the bones' origins in the model's own space (model = entity +0x1FC, bones [model +0x70] +0x30, 0x40 bytes
// each with the origin at +0x30, count at model +0xAB; ride_bones.cpp): the lowest, highest and mean height, and the
// first bone's place (the root). A sitting soldier's head is lower than a standing one's; a pose that rises lifts the
// body off the seat. Returns how many bones were read.
uint32_t BonesZ(uint8_t* base, uint32_t ent, float& bmin, float& bmax, float& bmean, float root[3]) {
  bmin = bmax = bmean = 0.0f;
  root[0] = root[1] = root[2] = 0.0f;
  uint32_t bones = 0;
  const uint32_t model = g::ReadU32(base, ent + 0x1FC);
  if (!ValidGuest(model)) return 0;
  const uint32_t buf = g::ReadU32(base, model + 0x70);
  const uint32_t n = g::ReadU8(base, model + 0xAB);
  if (!ValidGuest(buf) || n == 0) return 0;
  double sum = 0.0;
  for (uint32_t b = 0; b < n; ++b) {
    const float z = g::ReadF32(base, buf + 0x30 + 0x40 * b + 0x30 + 8);
    if (!std::isfinite(z)) continue;
    if (bones == 0 || z < bmin) bmin = z;
    if (bones == 0 || z > bmax) bmax = z;
    sum += z;
    ++bones;
  }
  if (bones) bmean = float(sum / bones);
  ReadVec(base, buf + 0x30 + 0x30, root);
  return bones;
}

void CensusViewTrace(uint8_t* base, uint32_t ent, const Census& c, const Census* v) {
  float bmin, bmax, bmean, root[3];
  const uint32_t bones = BonesZ(base, ent, bmin, bmax, bmean, root);
  float rc[3], cl[3] = {}, ru[3] = {};
  for (int i = 0; i < 3; ++i) rc[i] = c.drawn[i] - g_cens_cam[i];
  float in_cam[3];
  for (int a = 0; a < 3; ++a) in_cam[a] = Dot(rc, g_cens_cam_ax[a]);
  const float elev = std::atan2(in_cam[2], std::sqrt(in_cam[0] * in_cam[0] + in_cam[1] * in_cam[1])) * 57.29578f;
  if (v) {
    float cv[3];
    for (int i = 0; i < 3; ++i) cv[i] = g_cens_cam[i] - v->drawn[i];
    for (int a = 0; a < 3; ++a) {
      cl[a] = Dot(cv, v->axis[a]);
      ru[a] = Dot(c.axis[2], v->axis[a]);
    }
  }
  REXLOG_INFO("cod3: drawn view trace {} ent {} cam {:.2f} {:.2f} {:.2f} cam angles {:.2f} {:.2f} {:.2f} cam on carrier "
              "{:.2f} {:.2f} {:.2f} in cam {:.2f} {:.2f} {:.2f} elev {:.2f} up on carrier {:.3f} {:.3f} {:.3f} game "
              "pitch/roll {:.2f} {:.2f} carrier {:.2f} {:.2f} carrier drawn {:.2f} {:.2f} {:.2f} mode {} bones {} z "
              "{:.2f} {:.2f} {:.2f} root {:.2f} {:.2f} {:.2f}",
              c.frame, c.number, g_cens_cam[0], g_cens_cam[1], g_cens_cam[2], g_cens_cam_ang[0], g_cens_cam_ang[1],
              g_cens_cam_ang[2], cl[0], cl[1], cl[2], in_cam[0], in_cam[1], in_cam[2], elev, ru[0], ru[1], ru[2],
              c.angles[0], c.angles[2], v ? v->angles[0] : 0.0f, v ? v->angles[2] : 0.0f, v ? v->drawn[0] : 0.0f,
              v ? v->drawn[1] : 0.0f, v ? v->drawn[2] : 0.0f, g_cam_mode, bones, bmin, bmax, bmean, root[0], root[1],
              root[2]);
}

// cod3_debug_drawn_trace_diff (above): the traced entity's memory, once a game update.
constexpr uint32_t kDiffBytes = 0x300;  // read at the entity and at each pointer
constexpr uint32_t kDiffActorAt = 0x214;     // the entity's actor (actor +0 = the entity; 0x823EA0C0 and the states)
constexpr uint32_t kDiffActorBytes = 0xB00;  // all of it: the state at +0x29C, its name at +0xAD4, moveaway at +0xA7C
constexpr size_t kDiffMaxRegions = 64;  // structures followed at most
constexpr int kDiffMaxLines = 96;       // lines a game update at most

struct DiffRegion {
  uint32_t addr = 0;
  std::vector<uint32_t> words;
  std::vector<int64_t> since;  // the game time each word last changed (or was first read)
};
struct DiffWatch {
  uint32_t ent = 0;
  int32_t number = -1;
  uint64_t update = 0;
  std::unordered_map<uint32_t, DiffRegion> regions;  // by where: ~0 the entity, a pointer's offset in it, 0x10000 | an
                                                     // offset in its model
};
DiffWatch g_diff;

// Whether the guest can read [a, a + bytes): every page in it committed and readable (a pointer read from the game's
// structures may be stale or not a pointer at all).
bool GuestReadable(uint32_t a, uint32_t bytes) {
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory || a < 0x10000 || uint64_t(a) + bytes > 0x100000000ull) return false;
  for (uint64_t page = a & ~0xFFFull; page < uint64_t(a) + bytes; page += 0x1000) {
    auto* heap = memory->LookupHeap(uint32_t(page));
    uint32_t protect = 0;
    if (!heap || !heap->QueryProtect(uint32_t(page), &protect) || !(protect & rex::memory::kMemoryProtectRead))
      return false;
  }
  return true;
}

// A word that may point at a structure: the guest's heaps or the image's data, not its constants and code.
bool DiffPointer(uint32_t p) { return ValidGuest(p) && p >= 0x40000000u && !(p >= 0x82000000u && p < 0x825C0000u); }

// The actors' animation states (the animscripts): 0x8253BE28 looks up each state's main and exit function by name
// ("anim_main_vehicle_in", "anim_exit_vehicle_in", ...; BroFunc 0x82498D50) and keeps them in pairs from 0x82A510B8 and
// 0x82A51470 (names from dev/waw-decomp/cod3/names.tsv). A changed word that holds one of them is named in the diff:
// the rider's animscript changing (from vehicle_in, say) is what would stand him up.
constexpr const char* kAnimStates1[] = {"combat", "concealment_crouch", "concealment_prone", "concealment_stand",
                                        "cover_crouch", "cover_left", "cover_prone", "cover_right", "cover_stand",
                                        "cover_wide_left", "cover_wide_right", "death", "debug",
                                        "grenade_return_throw", "init", "interaction", "moveaway", "pain",
                                        "reacquire", "run", "scripted", "stop", "grenade_cower", "grenade_recover",
                                        "vehicle_enter", "vehicle_in", "vehicle_exit", "walk"};
constexpr const char* kAnimStates2[] = {"wounded", "wounded_attack", "wounded_crawl", "wounded_walk", "ambush"};
constexpr uint32_t kAnimStates1At = 0x82A510B8, kAnimStates2At = 0x82A51470;

// The actor keeps the address of the state's slot (actor +0x29C the state it is in, +0x2A0 the next one: 0x823EA0C0,
// which seats a rider on its vehicle's tag, sets vehicle_in there), so a word is named by the slot it points at, or
// ("fn:") by the function it holds.
std::string AnimStateOf(uint8_t* base, uint32_t v) {
  if (v < 0x10000) return {};
  auto find = [&](const char* const* names, size_t count, uint32_t at) -> std::string {
    if (v >= at && v < at + 8 * uint32_t(count) && (v & 3) == 0)
      return std::string((v - at) % 8 ? "exit_" : "main_") + names[(v - at) / 8];
    for (size_t i = 0; i < count; ++i) {
      if (g::ReadU32(base, at + 8 * uint32_t(i)) == v) return std::string("fn:main_") + names[i];
      if (g::ReadU32(base, at + 8 * uint32_t(i) + 4) == v) return std::string("fn:exit_") + names[i];
    }
    return {};
  };
  std::string s = find(kAnimStates1, std::size(kAnimStates1), kAnimStates1At);
  return s.empty() ? find(kAnimStates2, std::size(kAnimStates2), kAnimStates2At) : s;
}

// The game's constant text a word points at (its read-only data: state names such as "animscripted"), or "".
std::string DiffText(uint8_t* base, uint32_t v) {
  if (v < 0x82000000u || v >= 0x82100000u) return {};
  std::string t;
  for (uint32_t i = 0; i < 48; ++i) {
    const uint8_t ch = g::ReadU8(base, v + i);
    if (ch == 0) break;
    if (ch < 0x20 || ch >= 0x7F || ch == '"') return {};
    t.push_back(char(ch));
  }
  return t.size() >= 3 ? t : std::string();
}

// One region (`bytes` long): logs its words that changed after staying the same for `stable_ms`.
void DiffRegionSample(uint8_t* base, DiffRegion& r, uint32_t addr, uint32_t bytes, const std::string& where,
                      int32_t number, int64_t now, int stable_ms, int& lines) {
  const size_t n = bytes / 4;
  if (r.addr != addr || r.words.size() != n) {  // new, or the pointer to it changed: read afresh
    r.addr = addr;
    r.words.assign(n, 0);
    r.since.assign(n, now);
    for (size_t i = 0; i < n; ++i) r.words[i] = g::ReadU32(base, addr + 4 * uint32_t(i));
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    const uint32_t w = g::ReadU32(base, addr + 4 * uint32_t(i));
    if (w == r.words[i]) continue;
    const int64_t held = now - r.since[i];
    if (held >= stable_ms && lines < kDiffMaxLines) {
      float before, after;
      std::memcpy(&before, &r.words[i], 4);
      std::memcpy(&after, &w, 4);
      const std::string was = AnimStateOf(base, r.words[i]), is = AnimStateOf(base, w);
      std::string state = was.empty() && is.empty() ? std::string()
                                                   : fmt::format(" state {} -> {}", was.empty() ? "-" : was,
                                                                 is.empty() ? "-" : is);
      const std::string text_was = DiffText(base, r.words[i]), text_is = DiffText(base, w);
      if (!text_was.empty() || !text_is.empty()) state += fmt::format(" text \"{}\" -> \"{}\"", text_was, text_is);
      REXLOG_INFO("cod3: drawn diff {} ent {} {}+0x{:X} {:08X} -> {:08X} ({:g} -> {:g}) after {} ms{}", g_census_frame,
                  number, where, 4 * i, r.words[i], w, before, after, held, state);
      ++lines;
    }
    r.words[i] = w;
    r.since[i] = now;
  }
}

void DiffSample(uint8_t* base, uint32_t ent, const Census& c) {
  const int stable_ms = REXCVAR_GET(cod3_debug_drawn_trace_diff);
  if (stable_ms <= 0) {
    if (!g_diff.regions.empty()) g_diff = DiffWatch{};
    return;
  }
  if (g_diff.ent != ent || g_diff.number != c.number) {
    g_diff = DiffWatch{};
    g_diff.ent = ent;
    g_diff.number = c.number;
  }
  if (g_diff.update == g_clock.updates) return;  // once a game update
  g_diff.update = g_clock.updates;
  if (!GuestReadable(ent, kDiffBytes)) return;
  const int64_t now = g_clock.time;
  int lines = 0;
  DiffRegionSample(base, g_diff.regions[~0u], ent, kDiffBytes, "", c.number, now, stable_ms, lines);
  // The structures it points to, and those its model points to.
  std::vector<std::pair<uint32_t, uint32_t>> follow;  // (where, address)
  for (uint32_t off = 0; off < kDiffBytes; off += 4) {
    const uint32_t p = g::ReadU32(base, ent + off);
    if (DiffPointer(p) && p != ent) follow.emplace_back(off, p);
  }
  const uint32_t model = g::ReadU32(base, ent + 0x1FC);
  if (DiffPointer(model) && GuestReadable(model, kDiffBytes)) {
    for (uint32_t off = 0; off < kDiffBytes; off += 4) {
      const uint32_t p = g::ReadU32(base, model + off);
      if (DiffPointer(p) && p != ent && p != model) follow.emplace_back(0x10000u | off, p);
    }
  }
  size_t used = 0;
  for (const auto& [where, p] : follow) {
    if (used >= kDiffMaxRegions) break;
    const uint32_t bytes = where == kDiffActorAt ? kDiffActorBytes : kDiffBytes;
    if (!GuestReadable(p, bytes)) continue;
    ++used;
    const std::string label =
        (where & 0x10000u) ? fmt::format("[[+0x1FC]+0x{:X}]", where & 0xFFFFu) : fmt::format("[+0x{:X}]", where);
    DiffRegionSample(base, g_diff.regions[where], p, bytes, label, c.number, now, stable_ms, lines);
  }
}

// The end of a drawn frame (after CG_DrawActiveFrame): the carried people's offsets on their carriers.
void CensusFrameEnd(uint8_t* base) {
  const int trace = REXCVAR_GET(cod3_debug_drawn_trace), also = REXCVAR_GET(cod3_debug_drawn_trace_also);
  auto traced = [&](const Census& c) {
    return (trace > 0 && c.number == trace - 1) || (also > 0 && c.number == also - 1);
  };
  for (auto& kv : g_census) {
    Census& c = kv.second;
    if (c.frame != g_census_frame) continue;
    if (IsVehicleType(c.type)) {
      if (traced(c)) {
        const float zero[3] = {};
        CensusTrace(c, nullptr, zero, zero);
        CensusViewTrace(base, kv.first, c, nullptr);
      }
      continue;
    }
    const uint32_t carrier = CensusCarrier(c, kv.first);
    float gl[3] = {}, dl[3] = {};
    const Census* v = nullptr;
    if (carrier) {
      if (c.carried_by == 0) c.carried_by = 2;
      v = &g_census.find(carrier)->second;  // CensusCarrier found it there
      float ga[3][3];
      AnglesToAxis(v->angles, ga);
      float gr[3], dr[3];
      for (int i = 0; i < 3; ++i) {
        gr[i] = c.game[i] - v->game[i];
        dr[i] = c.drawn[i] - v->drawn[i];
      }
      for (int a = 0; a < 3; ++a) {
        gl[a] = Dot(gr, ga[a]);
        dl[a] = Dot(dr, v->axis[a]);
      }
      const float diff[3] = {dl[0] - gl[0], dl[1] - gl[1], dl[2] - gl[2]};
      const float rel = Len(diff);
      ++c.carried;
      if (rel > kCensusOff) ++c.rel_offs;
      c.max_rel = std::max(c.max_rel, rel);
      if (std::fabs(diff[2]) > std::fabs(c.rel_z)) c.rel_z = diff[2];
      c.local_min = std::min(c.local_min, gl[2]);
      c.local_max = std::max(c.local_max, gl[2]);
      c.carrier_number = v->number;
      if (c.type == 11) {  // an actor riding: its pose (Census::pose_rises, feet_up)
        float bmin, bmax, bmean, root[3];
        if (BonesZ(base, kv.first, bmin, bmax, bmean, root)) {
          if (c.pose_frame + 1 == g_census_frame && bmin - c.pose_min > kPoseRise) {
            ++c.pose_rises;
            c.max_rise = std::max(c.max_rise, bmin - c.pose_min);
          }
          if (bmin > kFeetUp) ++c.feet_up;
          c.pose_min = bmin;
          c.pose_frame = g_census_frame;
        }
      }
    }
    if (traced(c)) {
      CensusTrace(c, v, gl, dl);
      CensusViewTrace(base, kv.first, c, v);
    }
    if (trace > 0 && c.number == trace - 1) DiffSample(base, kv.first, c);
  }
}

void CensusLog(uint8_t* base) {
  const int setting = REXCVAR_GET(cod3_debug_drawn_census);
  uint32_t drawn = 0, off_ents = 0, off_frames = 0, frames = 0, carried = 0, rel_ents = 0, rel_frames = 0, jumps = 0;
  float max_off = 0.0f, up = 0.0f, down = 0.0f, max_rel = 0.0f, rel_z = 0.0f, max_jump = 0.0f;
  struct Worst {
    float score;
    uint32_t ent;
  };
  std::vector<Worst> worst;
  for (auto it = g_census.begin(); it != g_census.end();) {
    Census& c = it->second;
    if (c.frame + 2000 < g_census_frame) {  // not drawn for a while: forgotten
      it = g_census.erase(it);
      continue;
    }
    if (c.n > 0) {
      ++drawn;
      frames += c.n;
      if (c.offs) {
        ++off_ents;
        off_frames += c.offs;
      }
      max_off = std::max(max_off, c.max_off);
      up = std::max(up, c.max_up);
      down = std::min(down, c.max_down);
      if (c.carried) {
        ++carried;
        if (c.rel_offs) {
          ++rel_ents;
          rel_frames += c.rel_offs;
        }
        max_rel = std::max(max_rel, c.max_rel);
        if (std::fabs(c.rel_z) > std::fabs(rel_z)) rel_z = c.rel_z;
      }
      jumps += c.jumps;
      max_jump = std::max(max_jump, c.max_jump);
      const float range = c.carried ? c.local_max - c.local_min : 0.0f;
      const float score = (c.offs ? c.max_off + 10.0f * float(c.offs) / float(c.n) : 0.0f) +
                          (c.rel_offs ? c.max_rel + 10.0f * float(c.rel_offs) / float(std::max(c.carried, 1u)) : 0.0f) +
                          5.0f * float(c.jumps) + (range > 4.0f ? range : 0.0f) + float(c.pt_changes) +
                          10.0f * float(c.pose_rises) + (c.feet_up ? 5.0f : 0.0f);
      if (setting > 1 && score > 0.0f) worst.push_back(Worst{score, it->first});
    }
    ++it;
  }
  REXLOG_INFO("cod3: drawn census: {} frames; {} entities drawn in {} entity frames; off the game's path by more than "
              "{:.0f} units: {} entities in {} frames (max {:.1f}; height +{:.1f} / {:.1f}); carried {}, off their "
              "carrier {} in {} frames (max {:.1f}, height {:.1f}); game jumps {} (max {:.0f}); camera mode {}",
              g_census_frames, drawn, frames, kCensusOff, off_ents, off_frames, max_off, up, down, carried, rel_ents,
              rel_frames, max_rel, rel_z, jumps, max_jump, g_cam_mode);
  std::sort(worst.begin(), worst.end(), [](const Worst& a, const Worst& b) { return a.score > b.score; });
  for (size_t i = 0; i < worst.size() && i < size_t(setting); ++i) {
    const uint32_t ent = worst[i].ent;
    const Census& c = g_census[ent];
    REXLOG_INFO("cod3: drawn census: worst {} ent {} type {} fl {:08X} pt {} frames {} off {} max {:.1f} up {:.1f} "
                "down {:.1f} carrier {} by {} carried {} off {} max {:.1f} height {:.1f} game height {:.1f}..{:.1f} "
                "jumps {} (max {:.0f}) at {:.0f} {:.0f} {:.0f} game z {:.1f} pt changes {} pose rises {} (max {:.1f}) "
                "feet up {}",
                i + 1, c.number, c.type, c.flags, g::ReadS32(base, ent + kEntPosType), c.n, c.offs, c.max_off,
                c.max_up, c.max_down, c.carrier_number, c.carried_by, c.carried, c.rel_offs, c.max_rel, c.rel_z,
                c.carried ? c.local_min : 0.0f, c.carried ? c.local_max : 0.0f, c.jumps, c.max_jump, c.at[0], c.at[1],
                c.at[2], c.at_game_z, c.pt_changes, c.pose_rises, c.max_rise, c.feet_up);
  }
  for (auto& kv : g_census) {
    Census& c = kv.second;
    c.n = c.offs = c.jumps = c.carried = c.rel_offs = c.pt_changes = c.pose_rises = c.feet_up = 0;
    c.max_off = c.max_up = c.max_down = c.max_jump = c.max_rel = c.rel_z = c.max_rise = 0.0f;
    c.local_min = 1e9f;
    c.local_max = -1e9f;
    c.carrier_number = -1;
  }
  g_census_frames = 0;
}

// Once a drawn frame, before its entities are drawn: the last frame's carried people, then this frame.
void CensusFrame(uint8_t* base) {
  if (REXCVAR_GET(cod3_debug_drawn_census) <= 0) {
    if (!g_census.empty()) g_census.clear();
    return;
  }
  ++g_census_frame;
  ++g_census_frames;
  if (g_clock.time - g_census_since >= 1000 || g_clock.time < g_census_since) {
    CensusLog(base);
    g_census_since = g_clock.time;
  }
}

// After CG_DrawActiveFrame: the frame's camera, then the frame's carried people.
void CensusFrameDone(uint8_t* base) {
  if (REXCVAR_GET(cod3_debug_drawn_census) <= 0 || !g_census_frame) return;
  const uint32_t local = g::ReadU32(base, kActiveLocalClient);
  if (local <= 3) {
    const uint32_t cg = kCgBase + local * kCgSize;
    ReadVec(base, cg + kCgViewOrigin, g_cens_cam);
    ReadVec(base, cg + kCgViewAngles, g_cens_cam_ang);
    for (int a = 0; a < 3; ++a) ReadVec(base, cg + kCgViewAxis + 12 * a, g_cens_cam_ax[a]);
  }
  CensusFrameEnd(base);
}
}  // namespace

// CG_AddCEntity(centity): one entity into the scene, by its type (+0).
REX_EXTERN(__imp__CG_AddCEntity);
REX_HOOK_RAW(CG_AddCEntity) {
  const uint32_t ent = ctx.r3.u32;  // [acct2-cod3-motion] cod3_debug_entity_steps
  if (DebugOn() && ValidGuest(ctx.r3.u32)) {
    DrawCounts& d = g_draw[g_kind];
    ++d.cents;
    ++d.cents_by_type[g::ReadU8(base, ctx.r3.u32) & 15];
  }
  const uint64_t refents = g_es_refents;
  __imp__CG_AddCEntity(ctx, base);
  // [acct2-cod3-motion] an entity the draw put into the scene this frame: where it went.
  if (g_es_refents != refents && ValidGuest(ent) && REXCVAR_GET(cod3_debug_entity_steps) > 0)
    EntityStepSample(base, ent);
  if (g_es_refents != refents && ValidGuest(ent) && REXCVAR_GET(cod3_debug_drawn_census) > 0)
    CensusSample(base, ent);  // [acct2-cod3-motion] cod3_debug_drawn_census
}

// [acct2-cod3-motion] 2026-11-07 (coordinator 23:50: count the calls to the actors' animscripted entry per actor, with
// the frame) 0x823ECBB0 is the think of the actors' scripted-animation state: it names the state "animscripted" (actor
// +0xAD4, the think's debug name, as Actor_Exposed_NonCombat_Think 0x823EB010 writes "noncombat_stop"), and it runs
// once a game update while the state lasts. It ends the state when the scripted animation's handle (actor +0x298,
// into the 256-entry table at 0x82A90178: pointer, generation) no longer holds or 0x823C7848(actor) says so (decomp
// conditions, dev/waw-decomp/cod3/decomp/823E0000.c). Nothing in the engine calls it directly (calls.tsv). The
// St-Bert bridge riders leave the state 13.5 s after their truck starts, on the crest, and stand up on their seats
// (NOTES.md). This logs each actor's first call of a spell in the state, every call after which its state changed,
// and every 60th call, with the scripted animation's object (the handle's entry and its first words: its time and
// length, if they are there). Off by default; it logs only, and the game's function runs unchanged. Campaign only:
// server_step.cpp is not part of cod3mp, which has no actors.
REXCVAR_DEFINE_BOOL(cod3_debug_animscripted_log, false, "cod3",
                    "Log the actors' scripted-animation think (0x823ECBB0): each actor's first call of a spell, every "
                    "call that changed its state, and every 60th, with the frame, game time, caller, arguments and the "
                    "scripted animation's object")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
constexpr uint32_t kLevelInterface = 0x82A2A2A0;  // the engine's functions for the level (G_InitLevelInterface)
constexpr uint32_t kLevelInterfaceSlots = 1024;
constexpr uint32_t kAnimscriptedEntry = 0x823ECBB0;
constexpr uint32_t kActorAnimState = 0x29C;   // the animscript's slot (AnimStateOf names it)
constexpr uint32_t kActorThinkName = 0xAD4;   // the think state's name ("animscripted", "noncombat_stop", ...)
constexpr uint32_t kArgDumpWords = 8;         // words logged at each argument that points somewhere readable
constexpr uint32_t kActorScriptedAnim = 0x298;     // the scripted animation's handle: index in the low 8 bits
constexpr uint32_t kScriptedAnimTable = 0x82A90178;  // 256 entries of (pointer, generation)
constexpr uint32_t kScriptedAnimWords = 24;          // words logged at the scripted animation's object
constexpr uint32_t kAsEvery = 60;                    // and every this many calls of a spell

struct AsActor {
  uint32_t calls = 0;        // all calls
  uint32_t spell_calls = 0;  // calls in this spell (calls in consecutive game updates)
  uint64_t last_update = 0;
};
std::mutex g_as_mutex;
std::unordered_map<int32_t, AsActor> g_as_calls;  // by entity number
bool g_as_slots_logged = false;

// The scripted animation the actor's handle names: "h HANDLE entry PTR gen G (held|stale) [words] f [floats]".
std::string ScriptedAnimText(uint8_t* base, uint32_t actor) {
  if (!actor) return "-";
  const uint32_t h = g::ReadU32(base, actor + kActorScriptedAnim);
  const uint32_t entry = kScriptedAnimTable + 8 * (h & 0xFF);
  const uint32_t p = g::ReadU32(base, entry), gen = g::ReadU32(base, entry + 4);
  std::string out = fmt::format("h {:08X} entry {:08X} gen {:X} {}", h, p, gen, (h >> 8) == gen ? "held" : "stale");
  if (p >= 0x10000 && GuestReadable(p, 4 * kScriptedAnimWords)) {
    std::string words = " [", floats = " f [";
    for (uint32_t w = 0; w < kScriptedAnimWords; ++w) {
      const uint32_t v = g::ReadU32(base, p + 4 * w);
      float f;
      std::memcpy(&f, &v, 4);
      words += fmt::format("{}{:08X}", w ? " " : "", v);
      floats += fmt::format("{}{:g}", w ? " " : "", f);
    }
    out += words + "]" + floats + "]";
  }
  return out;
}

// The entity an argument names: an entity (its actor at +0x214 points back at it) or an actor (its +0 the entity).
uint32_t ActorEntityOf(uint8_t* base, uint32_t v, uint32_t& actor) {
  if (!DiffPointer(v) || !GuestReadable(v, kDiffActorAt + 4)) return 0;
  const uint32_t a = g::ReadU32(base, v + kDiffActorAt);
  if (DiffPointer(a) && GuestReadable(a, kDiffActorBytes) && g::ReadU32(base, a) == v) {
    actor = a;
    return v;
  }
  const uint32_t e = g::ReadU32(base, v);
  if (DiffPointer(e) && GuestReadable(e, kDiffActorAt + 4) && g::ReadU32(base, e + kDiffActorAt) == v &&
      GuestReadable(v, kDiffActorBytes)) {
    actor = v;
    return e;
  }
  return 0;
}

// Text an argument points at (an animation's name, say), wherever the guest can read it, or "".
std::string ArgText(uint8_t* base, uint32_t v) {
  if (v < 0x10000 || !GuestReadable(v, 1)) return {};
  std::string t;
  for (uint32_t i = 0; i < 40; ++i) {
    if (((v + i) & 0xFFF) == 0 && !GuestReadable(v + i, 1)) return {};
    const uint8_t ch = g::ReadU8(base, v + i);
    if (ch == 0) break;
    if (ch < 0x20 || ch >= 0x7F || ch == '"') return {};
    t.push_back(char(ch));
  }
  return t.size() >= 3 ? t : std::string();
}

// The actor's animscript and think state, as "scripted/animscripted".
std::string ActorStates(uint8_t* base, uint32_t actor) {
  if (!actor) return "-";
  std::string s = AnimStateOf(base, g::ReadU32(base, actor + kActorAnimState));
  const std::string think = DiffText(base, g::ReadU32(base, actor + kActorThinkName));
  return (s.empty() ? std::string("-") : s) + "/" + (think.empty() ? std::string("-") : think);
}

// Each argument: its value, and its text or its first words when it points somewhere readable.
std::string ArgsText(uint8_t* base, const uint32_t* args, size_t count, uint32_t ent, uint32_t actor) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t v = args[i];
    out += fmt::format(" r{} {:08X}", 3 + i, v);
    if (v == 0 || v == ent || v == actor) continue;
    const std::string text = ArgText(base, v);
    if (!text.empty()) {
      out += fmt::format(" \"{}\"", text);
    } else if (v >= 0x10000 && GuestReadable(v, 4 * kArgDumpWords)) {
      out += " [";
      for (uint32_t w = 0; w < kArgDumpWords; ++w)
        out += fmt::format("{}{:08X}", w ? " " : "", g::ReadU32(base, v + 4 * w));
      out += "]";
    }
  }
  return out;
}
}  // namespace

REX_EXTERN(__imp__sub_823ECBB0);
REX_HOOK_RAW(sub_823ECBB0) {
  if (!REXCVAR_GET(cod3_debug_animscripted_log)) {
    __imp__sub_823ECBB0(ctx, base);
    return;
  }
  const uint32_t args[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const double fargs[3] = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64};
  const uint32_t caller = uint32_t(ctx.lr);
  uint32_t actor = 0, ent = 0;
  int named_by = -1;
  for (int i = 0; i < 3 && !ent; ++i) {
    ent = ActorEntityOf(base, args[i], actor);
    if (ent) named_by = 3 + i;
  }
  const int32_t number = ent ? g::ReadS32(base, ent + kEntNumber) : -1;
  const std::string before = ActorStates(base, actor);
  uint32_t count = 0, spell = 0;
  bool log_slots = false;
  {
    std::lock_guard<std::mutex> lock(g_as_mutex);
    AsActor& a = g_as_calls[number];
    a.spell_calls = (a.calls && a.last_update + 1 >= g_clock.updates) ? a.spell_calls + 1 : 1;
    a.last_update = g_clock.updates;
    count = ++a.calls;
    spell = a.spell_calls;
    log_slots = !g_as_slots_logged;
    g_as_slots_logged = true;
  }
  if (log_slots) {  // once: whether the level's interface holds it
    std::string slots;
    for (uint32_t i = 0; i < kLevelInterfaceSlots; ++i)
      if (g::ReadU32(base, kLevelInterface + 4 * i) == kAnimscriptedEntry) slots += fmt::format(" {}", i);
    REXLOG_INFO("cod3: animscripted entry {:08X} in the level interface {:08X} at slot(s):{}", kAnimscriptedEntry,
                kLevelInterface, slots.empty() ? " none" : slots);
  }
  const bool first = spell == 1;
  const std::string anim_before = first ? ScriptedAnimText(base, actor) : std::string();
  const std::string arg_text = first ? ArgsText(base, args, std::size(args), ent, actor) : std::string();
  __imp__sub_823ECBB0(ctx, base);
  const std::string after = ActorStates(base, actor);
  if (!first && after == before && spell % kAsEvery != 0) return;
  REXLOG_INFO("cod3: animscripted call {} (spell {}) ent {} (r{}) frame {} update {} time {} caller {:08X}{} f1-f3 "
              "{:g} {:g} {:g} state {} -> {} returned {:08X} anim {}{}",
              count, spell, number, named_by, g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime), caller,
              arg_text, fargs[0], fargs[1], fargs[2], before, after, ctx.r3.u32,
              first ? "before " + anim_before + " after " : "", ScriptedAnimText(base, actor));
}

// [acct2-cod3-motion] 2026-11-07 (coordinator 23:58: find what sets bit 0x8 at the task's +0x10) The only code that
// sets it (decomp, 823C0000.c / 823D0000.c / 82520000.c) is three stop paths, each of which first calls the actor's
// current animscript's exit function (the slot at actor +0x29C, +4), then marks the scripted task finished (+0x10 |=
// 0x4C) and clears the handle at actor +0x298:
//   0x823C7760(actor), called from 0x824A0634 and 0x824B2414 (level-facing code: 0x824B22C0 is called by 0x824B2D50
//   and 0x824B2D80, next to the level interface), and then 0x82527808(entity) when entity +0x200 is set;
//   0x823DE438(actor), called from 0x823E2034 and 0x823E52E4 (0x823E5208, which ~12 actor states call), which also
//   notifies the entity (0x824927C0 with the string id at 0x82A50F00).
// With cod3_debug_animscripted_log each call is logged with the actor's entity, the game time, its caller and the
// return addresses up the guest stack (to find the level code that stops the riders' seat animation at 13.5 s).
namespace {
std::string GuestBackChain(uint8_t* base, uint32_t sp, int frames) {
  std::string out;
  for (int i = 0; i < frames; ++i) {
    if (sp < 0x10000 || (sp & 3) || !GuestReadable(sp, 4)) break;
    const uint32_t next = g::ReadU32(base, sp);
    if (next <= sp || !GuestReadable(next - 8, 8)) break;
    out += fmt::format(" {:08X}", g::ReadU32(base, next - 8));  // the LR the caller saved below its back chain
    sp = next;
  }
  return out.empty() ? " -" : out;
}

void LogTaskStop(PPCContext& ctx, uint8_t* base, const char* which, bool arg_is_entity) {
  uint32_t actor = 0;
  uint32_t ent = arg_is_entity ? ctx.r3.u32 : 0;
  if (!arg_is_entity) ent = ActorEntityOf(base, ctx.r3.u32, actor);
  const int32_t number = ent && GuestReadable(ent, kEntNumber + 4) ? g::ReadS32(base, ent + kEntNumber) : -1;
  REXLOG_INFO("cod3: animscripted stop {} ent {} frame {} update {} time {} caller {:08X} stack{} state {} anim {}",
              which, number, g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime), uint32_t(ctx.lr),
              GuestBackChain(base, ctx.r1.u32, 8), ActorStates(base, actor), ScriptedAnimText(base, actor));
}
}  // namespace

REX_EXTERN(__imp__sub_823C7760);
REX_HOOK_RAW(sub_823C7760) {
  if (REXCVAR_GET(cod3_debug_animscripted_log)) LogTaskStop(ctx, base, "823C7760", false);
  __imp__sub_823C7760(ctx, base);
}

REX_EXTERN(__imp__sub_823DE438);
REX_HOOK_RAW(sub_823DE438) {
  if (REXCVAR_GET(cod3_debug_animscripted_log)) LogTaskStop(ctx, base, "823DE438", false);
  __imp__sub_823DE438(ctx, base);
}

REX_EXTERN(__imp__sub_82527808);
REX_HOOK_RAW(sub_82527808) {
  if (REXCVAR_GET(cod3_debug_animscripted_log)) LogTaskStop(ctx, base, "82527808", true);
  __imp__sub_82527808(ctx, base);
}

// [acct2-cod3-motion] 2026-11-08 (coordinator 00:11: log the writer of the actor's state stack when `scripted` ends)
// The actor's think states are a stack: actor +0xD4 the top's index, the states at actor +0xB8 + 4 * index (0xF the
// scripted animation's; decomp writers of +0xD4: 0x823CC2A8 pushes (actor, state), 0x823C8D40 pops, 0x823C8E18 and
// 0x823C8EF8 set it, 0x823CC210 clears it). With cod3_debug_animscripted_log each of them is logged, before and after,
// when the traced entity (cod3_debug_drawn_trace) is the actor or the scripted state is on top before or after: the
// stack, the argument, the caller and 8 return addresses up the guest stack. The St-Bert riders leave `scripted` for
// combat ~13.5 s into their ride with their seat animation still playing; this names who pushes the combat state.
namespace {
constexpr uint32_t kActorStateTop = 0xD4;     // the state stack's top index
constexpr uint32_t kActorStateStack = 0xB8;   // its entries, 4 bytes each
constexpr uint32_t kStateScripted = 0xF;
constexpr int32_t kStateStackMax = 16;

struct StateStack {
  int32_t top = -1;
  uint32_t states[kStateStackMax] = {};
};

StateStack ReadStateStack(uint8_t* base, uint32_t actor) {
  StateStack s;
  if (!actor || !GuestReadable(actor, kActorStateStack + 4 * kStateStackMax)) return s;
  const int32_t top = g::ReadS32(base, actor + kActorStateTop);
  if (top < 0 || top >= kStateStackMax) return s;
  s.top = top;
  for (int32_t i = 0; i <= top; ++i) s.states[i] = g::ReadU32(base, actor + kActorStateStack + 4 * uint32_t(i));
  return s;
}

std::string StateStackText(const StateStack& s) {
  if (s.top < 0) return "-";
  std::string out;
  for (int32_t i = 0; i <= s.top; ++i) out += fmt::format("{}{:X}", i ? "," : "", s.states[i]);
  return out;
}

bool StateTopIs(const StateStack& s, uint32_t state) { return s.top >= 0 && s.states[s.top] == state; }

// Calls the original and logs it when it concerns the traced entity or the scripted state.
template <typename Original>
void LogStateOp(PPCContext& ctx, uint8_t* base, const char* which, Original original) {
  if (!REXCVAR_GET(cod3_debug_animscripted_log)) {
    original(ctx, base);
    return;
  }
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, lr = uint32_t(ctx.lr), sp = ctx.r1.u32;
  uint32_t actor = 0;
  const uint32_t ent = ActorEntityOf(base, r3, actor);
  const StateStack before = ReadStateStack(base, actor);
  const std::string stack = actor ? GuestBackChain(base, sp, 8) : std::string();
  const std::string think_before = actor ? ActorStates(base, actor) : std::string();
  original(ctx, base);
  if (!actor) return;
  const StateStack after = ReadStateStack(base, actor);
  const int32_t number = g::ReadS32(base, ent + kEntNumber);
  const int32_t trace = REXCVAR_GET(cod3_debug_drawn_trace);
  const bool traced = trace > 0 && number == trace - 1;
  if (!traced && !StateTopIs(before, kStateScripted) && !StateTopIs(after, kStateScripted)) return;
  REXLOG_INFO("cod3: actor state {} ent {} frame {} update {} time {} r4 {:08X} r5 {:08X} stack [{}] -> [{}] think {} -> {} "
              "caller {:08X} guest{}",
              which, number, g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime), r4, r5,
              StateStackText(before), StateStackText(after), think_before, ActorStates(base, actor), lr, stack);
}
}  // namespace

REX_EXTERN(__imp__sub_823CC2A8);
REX_HOOK_RAW(sub_823CC2A8) { LogStateOp(ctx, base, "push 823CC2A8", __imp__sub_823CC2A8); }

REX_EXTERN(__imp__sub_823C8D40);
REX_HOOK_RAW(sub_823C8D40) { LogStateOp(ctx, base, "pop 823C8D40", __imp__sub_823C8D40); }

REX_EXTERN(__imp__sub_823C8E18);
REX_HOOK_RAW(sub_823C8E18) { LogStateOp(ctx, base, "set 823C8E18", __imp__sub_823C8E18); }

REX_EXTERN(__imp__sub_823C8EF8);
REX_HOOK_RAW(sub_823C8EF8) { LogStateOp(ctx, base, "set 823C8EF8", __imp__sub_823C8EF8); }

REX_EXTERN(__imp__sub_823CC210);
REX_HOOK_RAW(sub_823CC210) { LogStateOp(ctx, base, "clear 823CC210", __imp__sub_823CC210); }

// [acct2-cod3-motion] 2026-11-08 (the bridge riders; NOTES "Start here" 00:25) The scripted state's think 0x823ECBB0
// calls 0x8255F508(entity), which runs the entity's scripted animation once a game update (its CoD2 / WaW relatives are
// G_Animscripted-like), and pops the state when the entity's record at +0x39C is gone, even while the seat task still
// holds. The record (0x70 bytes) is made by 0x825547E8, which the level's animscripted start 0x824B22C0 calls: +0x40
// the animation (its index in the low half), +0x48 set on its first update, +0x50 a blend-out time. With +0x50 > 0,
// 0x8255F508 ends the animation once its time (the float at 0x82A59DC0 + 52 * slot; slot = the ushort at the entity's
// anim tree (+0x228) + 24 + 2 * index) passes (length - blend) / length: it notifies the entity and clears +0x40. On
// the next update it frees the record and the think pops the state. With cod3_debug_animscripted_log, for actors
// (type 11), this logs:
// - 0x8255F508 for the traced entity (cod3_debug_drawn_trace) at every call, and for the others when the call changed
//   the record or its animation, and every 60th call;
// - 0x825547E8's arguments and the record it made;
// - the traced entity's notifies (0x824927C0: the id, the caller; the scripted end's id is named).
// It logs only; the game's functions run unchanged. Campaign only (no actors in cod3mp).
namespace {
constexpr uint32_t kEntScripted = 0x39C;          // the scripted-animation record
constexpr uint32_t kEntAnimTree = 0x228;          // the entity's anim tree (0x823D0950 makes an actor's)
constexpr uint32_t kScriptedRecordBytes = 0x70;
constexpr uint32_t kAnimSlotTable = 0x82A59DB0;   // 52-byte entries; the time 0x8255F508 compares is at +16
constexpr uint32_t kAnimSlotBytes = 52;
constexpr uint32_t kScriptedEndId = 0x82AB9ADC;   // the end notify's id, once 0x8255F508 has made it
constexpr uint32_t kScriptedEndText = 0x82065B98;  // and its text

std::unordered_map<int32_t, uint32_t> g_scripted_calls;  // by entity number; guarded by g_as_mutex

bool TracedNumber(int32_t number) {
  const int32_t trace = REXCVAR_GET(cod3_debug_drawn_trace);
  return trace > 0 && number == trace - 1;
}

bool ActorEntity(uint8_t* base, uint32_t ent) {
  return ValidGuest(ent) && GuestReadable(ent, kEntScripted + 4) && g::ReadU8(base, ent) == 11;
}

uint32_t ScriptedAnim(uint8_t* base, uint32_t rec) {
  return rec && GuestReadable(rec, kScriptedRecordBytes) ? g::ReadU32(base, rec + 0x40) : 0;
}

// "rec R anim A +44 X started S +4C X blend B +54 X slot N time T [the slot's 13 words as floats]".
std::string ScriptedRecordText(uint8_t* base, uint32_t ent) {
  const uint32_t rec = g::ReadU32(base, ent + kEntScripted);
  if (!rec) return "rec 0";
  if (!GuestReadable(rec, kScriptedRecordBytes)) return fmt::format("rec {:08X} unreadable", rec);
  const uint32_t anim = g::ReadU32(base, rec + 0x40);
  std::string out = fmt::format("rec {:08X} anim {:08X} +44 {:08X} started {} +4C {:08X} blend {:g} +54 {:08X}", rec,
                                anim, g::ReadU32(base, rec + 0x44), g::ReadU32(base, rec + 0x48),
                                g::ReadU32(base, rec + 0x4C), g::ReadF32(base, rec + 0x50),
                                g::ReadU32(base, rec + 0x54));
  const uint32_t tree = g::ReadU32(base, ent + kEntAnimTree);
  const uint32_t index = anim & 0xFFFF;
  if (!ValidGuest(tree) || !GuestReadable(tree + 24 + 2 * index, 2)) return out + " tree -";
  const uint32_t slot = g::ReadU16(base, tree + 24 + 2 * index);
  out += fmt::format(" slot {}", slot);
  const uint32_t entry = kAnimSlotTable + kAnimSlotBytes * slot;
  if (slot && GuestReadable(entry, kAnimSlotBytes)) {
    out += fmt::format(" time {:g} [", g::ReadF32(base, entry + 16));
    for (uint32_t w = 0; w < kAnimSlotBytes / 4; ++w)
      out += fmt::format("{}{:g}", w ? " " : "", g::ReadF32(base, entry + 4 * w));
    out += "]";
  }
  return out;
}

std::string GuestVec3Text(uint8_t* base, uint32_t p) {
  if (!ValidGuest(p) || !GuestReadable(p, 12)) return "-";
  return fmt::format("({:g} {:g} {:g})", g::ReadF32(base, p), g::ReadF32(base, p + 4), g::ReadF32(base, p + 8));
}
}  // namespace

REX_EXTERN(__imp__sub_8255F508);
REX_HOOK_RAW(sub_8255F508) {
  const uint32_t ent = ctx.r3.u32;
  if (!REXCVAR_GET(cod3_debug_animscripted_log) || !ActorEntity(base, ent)) {
    __imp__sub_8255F508(ctx, base);
    return;
  }
  const int32_t number = g::ReadS32(base, ent + kEntNumber);
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t rec = g::ReadU32(base, ent + kEntScripted);
  const uint32_t anim = ScriptedAnim(base, rec);
  const std::string before = ScriptedRecordText(base, ent);
  __imp__sub_8255F508(ctx, base);
  const uint32_t rec_after = g::ReadU32(base, ent + kEntScripted);
  const uint32_t anim_after = ScriptedAnim(base, rec_after);
  uint32_t count = 0;
  {
    std::lock_guard<std::mutex> lock(g_as_mutex);
    count = ++g_scripted_calls[number];
  }
  const bool changed = rec != rec_after || anim != anim_after;
  if (!changed && !TracedNumber(number) && count % kAsEvery != 0) return;
  REXLOG_INFO("cod3: scripted think ent {} call {} frame {} update {} time {} caller {:08X}{} before {} after {}", number,
              count, g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime), caller,
              changed ? " CHANGED" : "", before, ScriptedRecordText(base, ent));
}

REX_EXTERN(__imp__sub_825547E8);
REX_HOOK_RAW(sub_825547E8) {
  const uint32_t ent = ctx.r3.u32;
  if (!REXCVAR_GET(cod3_debug_animscripted_log) || !ActorEntity(base, ent)) {
    __imp__sub_825547E8(ctx, base);
    return;
  }
  const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, r8 = ctx.r8.u32, r9 = ctx.r9.u32;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
  const uint32_t caller = uint32_t(ctx.lr);
  const std::string stack = GuestBackChain(base, ctx.r1.u32, 6);
  const std::string origin = GuestVec3Text(base, r4), angles = GuestVec3Text(base, r5);
  const std::string before = ScriptedRecordText(base, ent);
  __imp__sub_825547E8(ctx, base);
  REXLOG_INFO("cod3: scripted start ent {} frame {} update {} time {} caller {:08X} stack{} origin {} angles {} r6 {:08X} "
              "r7 {:08X} r8 {:08X} r9 {:08X} f1 {:g} f2 {:g} before {} after {}",
              g::ReadS32(base, ent + kEntNumber), g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime),
              caller, stack, origin, angles, r6, r7, r8, r9, f1, f2, before, ScriptedRecordText(base, ent));
}

REX_EXTERN(__imp__sub_824927C0);
REX_HOOK_RAW(sub_824927C0) {
  if (REXCVAR_GET(cod3_debug_animscripted_log) && REXCVAR_GET(cod3_debug_drawn_trace) > 0 &&
      ActorEntity(base, ctx.r3.u32)) {
    const int32_t number = g::ReadS32(base, ctx.r3.u32 + kEntNumber);
    if (TracedNumber(number)) {
      const uint32_t id = ctx.r4.u32;
      const bool end = id != 0 && id == g::ReadU32(base, kScriptedEndId);
      REXLOG_INFO("cod3: scripted notify ent {} id {:08X}{} frame {} update {} time {} caller {:08X}", number, id,
                  end ? " (the scripted end \"" + ArgText(base, kScriptedEndText) + "\")" : std::string(),
                  g_census_frame, g_clock.updates, g::ReadS32(base, kLevelTime), uint32_t(ctx.lr));
    }
  }
  __imp__sub_824927C0(ctx, base);
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_body: 0x82442338(entity), CG_AddCEntity's draw of types 14
// and 15 (vehicles): it fills the entity's scene node from the server's origin (+336) and AnglesToAxis(+352) and puts it
// in the scene (0x8241B748); the vehicle's model is drawn by that matrix (0x8242E450 -> 0x8242CB68). The node is moved
// after it to the pose the vehicle is drawn at between updates.
REX_EXTERN(__imp__sub_82442338);
REX_HOOK_RAW(sub_82442338) {
  const uint32_t ent = ctx.r3.u32;
  __imp__sub_82442338(ctx, base);
  if (ent == 0 || ent != g_ride.ent) {
    PlaceVehicleBody(base, ent);
    return;
  }
  // The ridden vehicle (cod3_debug_view_trace): how its node was placed, and the scene's camera then (with the ride off
  // too, so 0 and 1 compare).
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client <= 3) {
    ReadVec(base, kCgBase + client * kCgSize + kCgViewOrigin, g_ride_trace_scam);
    g_ride_trace_scam_have = true;
  }
  g_ride_trace_body = PlaceVehicleBody(base, ent);
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_server_smooth_body: 0x8244B610(entity), CG_AddCEntity's draw of type 1
// (players and soldiers): the node from the entity's +336 and its +352, or the +352 of the vehicle it rides.
REX_EXTERN(__imp__sub_8244B610);
REX_HOOK_RAW(sub_8244B610) {
  const uint32_t ent = ctx.r3.u32;
  __imp__sub_8244B610(ctx, base);
  PlaceRiderBody(base, ent);
}

// RE_AddRefEntityToScene(refEntity): a model into the renderer's scene.
REX_EXTERN(__imp__RE_AddRefEntityToScene);
REX_HOOK_RAW(RE_AddRefEntityToScene) {
  ++g_es_refents;  // [acct2-cod3-motion] cod3_debug_entity_steps: CG_AddCEntity's draw put one in
  if (DebugOn()) ++g_draw[g_kind].refents;
  __imp__RE_AddRefEntityToScene(ctx, base);
}

// [claude-9b] 2026-10-27 SV_SendClientMessages: with several updates in one frame, only the last one
// sends the snapshots (see g_skip_send).
// [claude-9b] 2026-10-27 04:20 What a skipped send still does: clear "a game frame ran and its snapshot
// isn't sent yet" (0x829C3A34). SV_Frame runs the game's frame (SV_RunGameFrame 0x82514198, vmMain 10)
// only while that is clear and sets it after, and SV_SendClientMessages clears it at its end; left
// set, the second update of a frame ran without the game's frame, so with two updates in every frame
// (below 60 fps) level.time, the AI and the player moved at half speed (claude-9b-test run
// c9b-test-mv-walk-fps30, run c9b-vt4b-30: 30 fps, level.time +16/17 ms a 33 ms frame, the walk 97
// units a second against 190), and any frame with two updates (a hitch) lost one update's time.
constexpr uint32_t kSvGameFramePending = 0x829C3A34;
REX_EXTERN(__imp__SV_SendClientMessages);
REX_HOOK_RAW(SV_SendClientMessages) {
  if (g_skip_send) {
    ++g_sends_skipped;
    g::WriteU32(base, kSvGameFramePending, 0);
    return;
  }
  __imp__SV_SendClientMessages(ctx, base);
}

// [claude-9b] 2026-10-27 NET_SendLoopPacket(sock, length, data, adr): the local client's and the
// server's packets. Each socket has a queue (loopbacks 0x829FCFA8, 12312 bytes each: 4 messages of
// 3076 bytes, then two counters at +12304 and +12308 whose difference is the number waiting), and the
// game writes the next message at that number with no check: a fifth message before the other side
// reads goes over the counters and into the next queue, and the following ones anywhere. Run
// c9b-shz11-snap broke the world tree's free node list inside Netchan_Transmit from
// SV_SendClientSnapshot (a link of 0x3C: a 60-byte packet's length, stored at message +3072) and then
// crashed in SV_LinkEntity. The reader takes the newest message first and the client drops the
// older ones (Netchan_Process), so with 4 waiting the new message takes the newest one's place.
namespace {
constexpr uint32_t kLoopbacks = 0x829FCFA8;
constexpr uint32_t kLoopbackSize = 12312, kLoopbackMessages = 4;
bool g_loop_full_logged = false;
}  // namespace
REX_EXTERN(__imp__NET_SendLoopPacket);
REX_HOOK_RAW(NET_SendLoopPacket) {
  // The queue as the game picks it: socket 4 by the address's local client (the 16 bits at +2 of the
  // address word passed in r8's high half), the others the server's queue after the four clients'.
  const uint32_t sock = ctx.r3.u32;
  const uint32_t index = sock == 4 ? static_cast<uint32_t>(ctx.r8.u64 >> 32) & 0xFFFF : kLoopbackMessages;
  if (index <= kLoopbackMessages) {
    const uint32_t q = kLoopbacks + index * kLoopbackSize;
    const uint32_t get = g::ReadU32(base, q + 12304), send = g::ReadU32(base, q + 12308);
    const int32_t waiting = static_cast<int32_t>(send - get);
    if (waiting < 0 || waiting >= static_cast<int32_t>(kLoopbackMessages)) {
      ++g_loop_full;
      if (!g_loop_full_logged) {
        g_loop_full_logged = true;
        REXLOG_WARN("cod3: a loopback queue (socket {}, queue {}) had {} messages waiting: the new one "
                    "takes the newest one's place (the game would write past the queue)",
                    sock, index, waiting);
      }
      // 4 waiting: the new message goes in as the 4th; anything else is broken: the queue starts again.
      g::WriteU32(base, q + 12304, waiting == static_cast<int32_t>(kLoopbackMessages)
                                       ? send - (kLoopbackMessages - 1)
                                       : send);
    }
  }
  __imp__NET_SendLoopPacket(ctx, base);
}
