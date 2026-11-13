# Gameplay patches

Every change the port makes to how Call of Duty 3 plays, in the campaign
program (`cod3`) and the multiplayer program (`cod3mp`): what it changes,
its settings and their defaults, which program has it, its files, the chat
that wrote it, and how far it has been tested. Section 1 lists what is broken
or still open.

Written 2026-11-07 by acct2-ocr, updated the same day at 13:00 (the AI fix; split
screen match rules) and 13:15 (ride bones and the frame parameter measured;
dead zone rows in F2) and 13:40 (bugs 14 and 15) and 14:30 (bug 15: the
vehicle scan and the Crossroads fix) and 15:30 (bug 16: the cinematic bars) and 16:05 (item 16: the other full-screen effects) and 23:15 (the
controller's settings per player, the CONTROLLER pages, the field of view per player: 7.9, 7.10, 8.1, item 17)
and 23:40 (the port's settings as rows of the game's own Options pages: 7.11, 8.1, item 18) and 23:58 (the Use
hold per player: 7.4, 7.11; the mouse's zoom from the keyboard player's view: 8.1)
(owner 12:06: "Please document each gameplay
related patch we have added"; 12:07: "If you encounter something that's
broken Please say so"). Read from the sources, both source lists,
`docs/MOVEMENT.md`, `dev/agents/BOARD.md` and the chats' notes. No game was
run for the first version; the test results are the chats' own, with their
run names. The 13:00 update adds runs acct2-ocr-host1, acct2-ocr-sl-c and acct2-ocr-hz60-a;
the 13:15 update adds acct2-ocr-ride1 and acct2-ocr-ride2; item 14 adds
acct2-ocr-edge1 and acct2-ocr-edge2, and item 15 adds acct2-ocr-jit1 and acct2-ocr-jit2. Item 16 adds acct2-ocr-lb1
to acct2-ocr-lb7 and acct2-ocr-lbfix1 and acct2-ocr-lbfix2, and its second part adds
acct2-ocr-edge-host-off, acct2-ocr-edge-host2-off, -on, -scaled and -scaled-off, and
acct2-ocr-edge-mp-split.

Not here: text and voice languages, fonts, the renderer, textures, sound and
the look of the menus. They don't change how the game plays.

**How to read it**

- **SP** is the campaign program, **MP** the multiplayer program. "Both"
  means the file is in both source lists (`recomp/CMakeLists.txt`,
  `recomp/mp/CMakeLists.txt`, as of 2026-11-07).
- Settings are the port's own: `cod3.toml`, F2, and the ~ console by name.
  **protected** marks a server_sided_protected dvar
  (`game/protected_dvars.h`): in a match the host's value applies, and in the
  campaign it is a normal setting.
- **Chat** lists the `[tags]` in the files. "port" means untagged code from
  before the chats tagged their work.
- **State** is one of these:
  - **Tested**: a run or the owner checked it; what was seen is given.
  - **Built**: in the build, but no test record was found.
  - **UNTESTED**: the file itself says so.
  - **Off**: off by default.
  - **Not built**: the file is in the folder but in neither source list.
- The port caps the frame rate at 60 by default (`cod3_max_fps` 60,
  F2 > Display). Most of the frame-rate fixes below matter once a player
  raises the cap or removes it.

---

## 1. Broken or open

These don't work the way their settings say, or work only in part. The
biggest come first.

1. **Multiplayer, system link: the host's match rules reach the other
   consoles** (fixed 11-07 by acct2-ocr; it was "the match rules stay at
   their defaults").
   - The host is the Demonware session's host now: `mp::IsHost()` in
     `game/mp/session.cpp` is true when the bdSession's host connection
     (+60) is 0. Outside a match, and in split screen, the PC is its own
     host as before.
   - The host sends `protected_dvars::ServerInfoPairs()` to the other
     consoles as a port-only peer message (type 0x70, which the game has no
     handler for): as soon as another console is in the match, again 4 s
     later, at once when a rule changes, and every 10 s. A guest applies it
     with `ApplyServerInfo` ("the server sets aim_slowdown = 40") and drops it
     with `LeaveMatch` when its match ends, so its own values come back. The
     host ignores these messages (its own come back to it too); a guest takes
     at most 6 of each kind in 10 s, and only printable text of 255 bytes at
     most.
   - A rule the host changes is also announced to everyone ("Aim assist:
     aim_slowdown 60 (set by the host)."). Cheats work the same way: only the
     host can turn `sv_cheats` on, and the others are told.
   - The host sets the rules in F2 > Game > Server or in the ~ console
     (`aim_slowdown 60`, `set levitation_fix 0` ...). The console handling
     was campaign only (`ui/game_console.cpp`); it is in cod3mp too now. A
     guest's own change is refused while it plays ("set by the server in
     this match ... your own value comes back when you leave").
   - Tested in muted two-console system link runs on mp_merv (host A,
     joiner B, both PASSED, 0 errors and 0 crashes):
     - acct2-ocr-sl-r1 (build 20261107-194408): B got A's rules 0.1 s after
       A saw it join (`aim_slowdown` 40, `levitation_fix` 0, against B's own
       100 and 1). When A's match ended, B logged "left the match; your own
       values apply again".
     - acct2-ocr-sl-r2 (build 20261107-195114): B's `aim_slowdown 20` was
       refused; A's `aim_slowdown 60` reached B 0.2 s later with the
       announcement, and B logged "the server sets aim_slowdown = 60".
   - Not done: the messages are not signed, so a third console in the
     same session could pretend to be the host. What it could send is
     limited to the rules' own ranges (`aim_slowdown` 0-100 and so on).
   - `disconnect` from cod3_autoexec did nothing in cod3mp in run r2 (no
     log line); the leave path was checked with the host's match ending
     instead (r1).
2. **Multiplayer: F2 > Game has no Movement tab.** cod3mp has no CoD4
   movement yet (item 4), so there is nothing for that tab to set. The
   Server tab is there now (see below).
3. **Multiplayer: the host's game runs once per drawn frame.**
   - `cod3mp_server_hz` is 0 ("0 until the tester's runs pass", claude-9b,
     10-27).
   - In a match this program hosts (split screen, local, or hosting system
     link), grenades, physics, vehicles, movers and script waits step with
     the frame rate. The campaign did the same before `cod3_server_hz`.
   - The fixed-step code is already there (`game/mp/server_step_mp.cpp`;
     `cod3mp_server_hz 60` turns it on).
   - First test, run acct2-ocr-hz60-a (2026-11-07, split screen, mp_merv,
     uncapped, `cod3mp_server_hz=60`):
     - In the match every one-second window had 60-61 game updates at
       126-145 fps ("hosting yes").
     - In the menus it stayed once per frame ("hosting no"), as
       designed.
     - 0 errors, 0 crashes in 82 s.
   - Not checked yet: whether moving things look smooth above 60 fps with
     it on. The campaign draws them between updates; multiplayer's local
     players see the snapshots. That is for the owner to look at before
     the default changes to 60.
4. **Multiplayer: no Call of Duty 4 movement style.**
   - `game/movement/hooks_mp.cpp` is stage 1 (claude-9b-think, 2026-11-02,
     UNTESTED).
   - `cod3mp_movement_rule` and `cod3mp_movement` / `_2`..`_4` are only read
     and logged: "every match moves as Call of Duty 3".
5. **Campaign: the AI's shooting above 60 fps. Fixed** (owner 2026-11-07
   12:28: "The AI shooting IS fixed"). The number is kept so the references
   below still match; see 4.1.
6. **No in-game KEY BINDINGS pages.**
   - `game/keybinds/menu_binds.{h,cpp}` (acct2-main, 11-05) is in neither
     source list. acct2-main's 2.34.0 note says "menu_binds (not built)".
   - Keys and controller buttons can only be bound in F2 and `cod3.toml`.
   - The per-player stick dead zones (`cod3_deadzone_p1`..`_p4`) now have
     rows in F2 > Controls > Controller, under the shared `joy_threshold`
     slider (acct2-ocr, 13:15): one row in the campaign, four in
     multiplayer. Built (build 20261107-130659); nobody has looked at the
     rows yet. Since 23:14 the pause menu's CONTROLLER page has a STICK DEAD
     ZONE row for the menu's player too (7.10; built, not run).
7. **Riding a tank: the turret and gun move in 60 Hz steps.** Above 60 fps
   the hull and the camera are smooth but the turret and gun are not. The
   fix, `cod3_blend_ride_bones` (`game/ride_bones.cpp`, claude-9b-think), is
   off by default.
   - Measured 2026-11-07 in Black Burn (uncapped, a scripted stick turning
     the turret, muted, one seat):
     - It finds the ridden tank: vehicle 16948, 44 bones, `tag_turret` 5,
       `tag_barrel` 6, `tag_gunner_turret` 14, `tag_gunner_barrel` 18.
     - Run acct2-ocr-ride1, mode 2 (checks only, draws nothing different):
       every update while the turret turned made a pair (60 a second, 19
       bones, at most 0.57 degrees an update). 16,741 frames would have been
       blended. No guard tripped in 132 seconds, and one A-B-A skip was
       logged.
     - Run acct2-ocr-ride2, mode 1 (on): 15,549 frames blended at about 140-380
       fps. Every blend was drawn inside the blend and restored after it. No
       bone was changed by other code in between, and none was drawn on
       another thread.
     - Both runs: 0 errors, 0 stalls, 0 crashes. Run 2 logged one 730 ms
       frame and one 316 ms frame. Other chats' runs were on the PC then
       (run 2 got seat 3).
   - What is left: the owner's eyes on the turret above 60 fps with
     `cod3_blend_ride_bones 1`. The default stays 0 (claude-9b-think's).
8. **Materials animated by the frame number run with the frame rate.**
   `cod3_fix_frame_param` is off "until measured" (claude-9b, 10-27).
   - Measured in run acct2-ocr-ride1 (`cod3_debug_anim_clock`, 149 one-second
     lines): in the front end, the loading screen and Black Burn, no material
     asked for the frame number (0 a frame). The world-time parameter was
     asked for 2-3 times a frame.
   - So in those places the fix has nothing to change and nothing runs
     fast. Other levels are not measured yet; one run each would tell.
9. **Levitation fix, campaign part: built but never exercised.**
   - The rule is in `game/movement/hooks_sp.cpp`: the game's unstick
     (0x82483C20) runs with the characters left out. No run has met it yet.
   - The owner's Eder Dam hand test (multiplayer, his terminal) is still to
     do. The command is in `dev/agents/acct2-ocr/NOTES.md` (11-07 11:55).
10. **Use without holding it: waiting for the owner.** The scripted run of
    `cod3_use_hold_ms` (acct2-decomp, run acct2-decomp-hold1, 11-05 11:00)
    was clean, but a script can't press Use at a door or a charge. No in-game
    check has been recorded since.
11. **No test run ever sees a real controller.** Games started from REA or
    devrun run in a sandbox without `/dev/input` (found 11-05). Pad binds,
    dead zones, L3 sprint, rumble and button pictures were tested with
    scripted pads, or by the owner only.
12. **Movement's known limits** (`docs/MOVEMENT.md` section 10):
    - CoD4's sprint recharge (4 s) is an estimate.
    - ADS, crouch and prone speeds and fall damage stay CoD3's.
    - A real bounce (a PC jump trick) has never been seen.
    - The main menu's Options has no MOVEMENT page.
    - Controller layout C has no sprint button (its L3 is stance).
    - There is no sprint meter and no sprint camera bob.
    - Sprinting into a wall still uses stamina.
13. **Files left behind, not built.** Nothing is broken by these; they can be
    removed:
    - `game/movement/mantle.{h,cpp}`: CoD4 mantling, taken out on 2026-10-20
      (setup tool 2.15.4).
    - `game/stick_actions.{h,cpp}`: replaced by `qte_actions` on 10-16.
    - `game/keybinds/pad_state_mp.cpp`: its header says it is in
      `mp/CMakeLists.txt`, but it isn't. `button_prompts.cpp`'s pad state hook
      does that job in cod3mp.
14. **Picture: a dark, see-through band around the edge of the view.** The
    owner reported it on 2026-11-07 at 13:10: "Theres a slightly black see
    through bar around the border of the camera. Happens in splitscreen and
    SP."
    - This is the picture, not a gameplay patch. It is listed here because
      the owner asked for it to be added.
    - First look (acct2-ocr, 13:20). Runs acct2-ocr-edge1 (1280x720) and
      acct2-ocr-edge2 (1920x1013) used default settings in Black Burn and
      took 8 console `screenshot`s. A screenshot is the game's own picture,
      before the presenter.
      - In those pictures the outer 32 pixels on every side are as bright as
        the pixels just inside them. No band with a clear inner edge showed.
      - The dark areas that were found are the tank's own scene, and they
        look the same at both sizes.
      - The tool prints numbers only:
        `dev/agents/acct2-ocr/tools/edge_profile.py`.
    - So either the band is drawn after the game's picture (by the
      presenter, a present effect or an overlay), or it is a soft darkening
      that this scene hides.
    - Next: find out where it is drawn. That needs the owner's description.
      Then it goes to the chat that owns that part.
    - 15:20 (acct2-ocr, while on item 16): the game's own 2x MSAA mode
      (`cod3_render_method` `scaled`) was checked as well. The scene's outer
      columns and rows look the same as in native mode, so that mode is not
      the cause. Still open.
15. **Campaign: the camera or the people jitter in some vehicles.** The
    owner reported it on 2026-11-07 at 13:18: "The camera or NPCs can be
    jittering depending on the vehicle. It's related to our fps changes.
    saint lo intro car ride has it. really bad."
    - Found in run acct2-ocr-jit1 (Saint-Lô from the start, uncapped, about
      280 fps):
      - The whole intro ride uses camera mode 15 of the game's view setup,
        and the player rides no vehicle there. So none of the vehicle view
        smoothing applied.
      - The camera turned and moved only in the 60 frames a second that had
        a game update. In the 200-230 frames between it stood still, while
        the jeep and its riders were drawn between updates.
    - Fixed for that camera (acct2-ocr, 13:35) with `cod3_server_smooth_cams`
      in `game/server_step.cpp`. The default covers mode 15 only.
      - If the game left the camera exactly as the last update set it, the
        camera is drawn between its places at the last two updates, like
        the moving things.
      - A camera that changes between updates, or a cut, is left alone.
    - Tested in run acct2-ocr-jit2 (build 20261107-132434):
      - Through the ride the camera turned and moved by the same amount in
        frames with and without an update (for example 0.36 and 0.34
        degrees a frame).
      - 9,929 frames were drawn between updates in 36 s, with 0 errors and
        0 crashes.
    - The vehicle scan (acct2-ocr, 14:00-14:30, owner 13:55: "Keep going
      with bug checks"):
      - Method: with `cod3_debug_server_step` on, the log's camera line gives
        the camera's mode and how far it moved and turned in frames with and
        without an update. `dev/agents/acct2-ocr/tools/cam_steps.py` flags
        each second in which the camera moved or turned only in the update
        frames. It prints numbers only.
      - Runs (one seat, muted, uncapped, from the level start unless said):
        - Night Drop's opening plane ride is mode 15 as well, so the fix
          above covers it: 0 stepped seconds in 36 (acct2-ocr-veh-nightd).
        - The hostage jeep ride, mode 8 (the vehicle ride smoothing): 0
          stepped seconds in 64 (acct2-ocr-veh-hostage).
        - Black Burn's tank, mode 4, with scripted driving and turret input:
          0 stepped seconds in 94 (acct2-ocr-veh-blkbrn).
        - Crossroads: mode 15 at the start, 0 stepped seconds in 16. About
          45 s in, mode 12 (the player carried by something the level moves,
          not riding a vehicle) stepped for 6 s. The camera moved 1.5-1.9
          units a frame in update frames and 0.02-0.06 in the frames between,
          about 100 units a second in 60 Hz steps (acct2-ocr-veh-crssrds).
          Fixed below.
      - Fix for mode 12 (acct2-ocr, 14:20): new setting
        `cod3_server_smooth_linked`, a bit mask of camera modes like
        `cod3_server_smooth_cams` (default: mode 12). After the game's view
        and the movement smoothing (SmoothView), if nothing moved the camera
        and the player rides no vehicle, the camera's place is drawn between
        its places at the last two updates. Its angles are left alone,
        because the player turns them every frame.
        - Files: `game/server_step.{h,cpp}`. In `game/movement/hooks_sp.cpp`,
          SmoothView now says whether it moved the camera, and its hook calls
          the new function after it.
        - Build 20261107-142307.
        - Tested in run acct2-ocr-link1-crssrds: mode 12 had 0 stepped
          seconds in 26. While it moved, the camera went 0.5-0.7 units a
          frame with an update and 0.5-0.55 without. Update frames are
          probably slightly longer, so they move a little more. SmoothView moved the
          camera in 0-5 frames a second of those, so it was not doing it
          before either.
        - Saint-Lô again (acct2-ocr-link2-stlo): mode 15 had 0 stepped
          seconds in 34. 0 errors in both runs.
      - People in vehicles:
        - In the campaign, soldiers and the people riding in vehicles are
          not drawn by the game's player draw (entity type 1). That draw is
          used once a frame, for the player.
        - They are drawn by the general model draw (type 7, 116-540 a
          frame). That draw takes the drawn origin, which the smoothing
          already places between updates.
        - So Saint-Lô's riders were drawn with the jeep between updates.
          The jitter against them was the camera stepping, which is fixed.
        - `cod3_server_smooth_body` 2 only changes the type 1 draw, so it
          does nothing in the campaign. Run acct2-ocr-jit3 gave the same
          counts with 1 and 2.
    - Still open:
      - Mode 21 (the game picks it when the playerState's type is 4 or
        more): for 2 s after the hostage ride ends, the camera moved 0.1-0.5
        units a frame, only in update frames. It is small and short, so it
        was left alone.
        - Adding bit 21 to `cod3_server_smooth_linked` (2101248 for modes 12
          and 21) should cover it. That is not tested.
      - Rides deeper into levels have not been reached. The runs have no
        real controller, and the scripted input only drives and turns.
16. **Picture: the cinematic bars and other full-screen effects missed the
    last column and row.** Fixed, except one case in scaled mode (see the end of this item). The owner reported it on 2026-11-07 at 15:09: "the cinematic
    letterboxing Needs a pass. It's missing a pixel on the bottom right and
    top right."
    - This is the picture, not a gameplay patch. It is listed here like item
      14.
    - The cause (acct2-ocr, 15:10-15:25):
      - The game draws the bars as 2D quads in its 640x480 screen space,
        60 units tall. `game/ultrawide.cpp` finds them and moves their edges
        onto the edges of that space.
      - The game uses its own 2x MSAA when `cod3_render_method` is `scaled`,
        or when the render size is below the window's. With it, a bar that
        ended exactly on the right or bottom edge left the picture's last
        column and last row uncovered. The scene showed through there.
    - Runs (Saint-Lô's intro, one seat, muted):
      - Native at 1280x720, 1920x1013, 1600x900, and 1600x900 with a 150%
        render had no gap (acct2-ocr-lb1, acct2-ocr-lb3-1913,
        acct2-ocr-lb4-1600, acct2-ocr-lb5-p150).
      - Scaled, a 720p render in a 1600x900 window (acct2-ocr-lb6-scaled,
        and the owner's two screenshots of it) and in 1280x720
        (acct2-ocr-lb7-scaled-dbg): the top bar's last column had a
        brightness of 26-60 against 0 in the bar. The last row had 31-39.
      - acct2-ocr-lb2-1080 (a 1920x1080 window) stopped 9 s in at the main
        menu. Its log has no error and devrun wrote no summary, so both were
        stopped from outside, most likely by the test tools. It was run
        again at 1920x1013.
    - Fix (acct2-ocr, 15:25): the bars now end 2 units past the edges,
      not on them, and the render target clips the rest. The change is in
      `game/ultrawide.cpp`, which both programs build. Build 20261107-152502.
    - Tested:
      - acct2-ocr-lbfix1-scaled (the lb6 settings): in the game's 5
        screenshots, and in 3 grabs of the window as shown on screen, both
        bars are black out to the last column and the last row.
      - acct2-ocr-lbfix2-native (1920x1013, native): the same. Nothing
        changed there.
      - 0 errors in both. cod3mp was not run; it uses the same code.
    - Tools (numbers only, no pixels printed):
      `dev/agents/acct2-ocr/tools/lb_edges.py` and `lb_grab.sh`.
    - Other effects (acct2-ocr, 15:35-16:05; owner 15:32: "Ok there might be
      other effects that have this. Please check"):
      - The scan: the edges of all 8,885 screenshots in the run folders were
        measured, comparing the last column and row with the ones next to
        them. The tools are `seam_scan.py`, `seam_report.py` and
        `seam_rows.py` in `dev/agents/acct2-ocr/tools/` (numbers only).
        - Older builds showed the bars' gap, from before the bars were
          snapped.
        - Hostage with today's build showed the gap at moments when an effect
          covered the screen.
        - acct2-bro's own renderer tests showed edge differences too. Those
          are that chat's experiments, not the game's default picture.
      - Hostage reproduced (run acct2-ocr-edge-host2-off: the earlier runs'
        walk forward, with the fix off, native 1280x720):
        - About 40-43 s after the level loads the game draws a dark red
          overlay (x -10.5 to 639.5) and a red flash (x -0.5 to 639.5).
          Later it draws a fade to black (x -0.5 to 639.5).
        - In the screenshots at those moments the last column was 23 and 15
          brighter than the one next to it, in 78% and 53% of the rows.
        - Like the bars, these quads end half a unit short of the edge. With
          the game's 2x MSAA (scaled mode) the fade also left the last row
          uncovered: 17 and 19 brighter (acct2-ocr-edge-host2-scaled-off).
      - Fix: `cod3_edge_fill` (default 1), in `game/ultrawide.cpp`, both
        programs (section 8.6).
        - A 2D quad that spans the screen's width or height (or a split-screen
          view's width) and ends on an outer edge is drawn 2 units past that
          edge.
        - Corners only move outwards, and edges between split-screen views
          stay as they are.
        - With the ultrawide HUD mapping, only quads drawn during play have
          their sides moved, because a menu's or the movie's sides are inside
          the picture there.
        - The quad is put back after the list copies it.
        - `cod3_debug_edge_fill` logs each kind of quad it sees.
        - Build 20261107-155147. The build before it, at 15:47, differed only
          in the debug log.
      - Tested:
        - acct2-ocr-edge-host2-on (the same run with the fix on): 0 seams in
          13 screenshots, including the two moments above. The log shows the
          red overlay and the fade "moved past the edges".
        - acct2-ocr-edge-host2-scaled (scaled, 720p in 1600x900): the fade
          showed no gap.
        - acct2-ocr-edge-mp-split (cod3mp, two players in split screen):
          0 seams in 8 screenshots. Each half's quads were moved at the outer
          edges only.
        - 0 errors and 0 crashes in every run.
        - Not run: the movie (the test runs skip films), and an ultrawide
          window.
      - Still open, scaled mode only: at bright flashes the last column comes
        out darker than the one next to it. It was 19 darker when the
        picture's mean was 116 (fix off), and 6 darker in the run with the
        fix on.
        - Native runs at the same moments have none.
        - No 2D quad was logged at those moments, so this is not one this
          setting can move. It comes from the scaled mode's own drawing (the
          renderer).

### Fixed since this page was first written

- **Saint-Lô's intro ride: the camera's 60 Hz judder** (item 15,
  acct2-ocr, 13:35): `cod3_server_smooth_cams`. Measured in runs
  acct2-ocr-jit1 (before) and acct2-ocr-jit2 (after). Night Drop's opening
  plane ride uses the same camera mode and is covered too.
- **Crossroads: the camera of a player carried by the level** (item 15,
  acct2-ocr, 14:20): `cod3_server_smooth_linked`. Measured in runs
  acct2-ocr-veh-crssrds (before) and acct2-ocr-link1-crssrds (after).
- **Full-screen fades and overlays: the last column and row** (item 16,
  acct2-ocr, 15:50): `cod3_edge_fill`. Measured in runs
  acct2-ocr-edge-host2-off (before) and acct2-ocr-edge-host2-on (after).
- **The cinematic bars' last column and row** (item 16, acct2-ocr, 15:25):
  with the game's 2x MSAA (`cod3_render_method` `scaled`), the bars now
  reach the picture's edges. Measured in runs acct2-ocr-lb6-scaled (before)
  and acct2-ocr-lbfix1-scaled (after).
- **AI shooting above 60 fps** (item 5): fixed, the owner confirmed it on
  2026-11-07 at 12:28.
- **Split screen: the match rules are this PC's own** (acct2-ocr,
  2026-11-07, owner 12:28 "start going thru these open or broken things").
  - What changed:
    - `game/mp/session.cpp` now tells a match that no other console plays
      apart from one that others play (`IsLocalOnlyMatch()`, declared in
      `game/multiplayer.h`).
    - The test: no remote player in MPPeer's slot table, and no host
      connection in the bdSession. If anything can't be read, the match
      counts as having other consoles.
    - `protected_dvars.cpp` counts such a match as hosted here. So in split
      screen the PC's own `levitation_fix`, `sticky_aim` and `aim_slowdown`
      apply to every local player.
  - F2 > Game > Server is now in cod3mp: `ui/server_tab.cpp` is in
    `recomp/mp/CMakeLists.txt`, and `ui/hud_overlay.cpp` has a `COD3_MP`
    block for it. It shows outside a match and in split screen, and is
    hidden in system link. It shows the levitation fix and the Sticky Aim
    rows, and leaves the movement rule out.
  - Unchanged: `mp::IsHost()` and the cheat policy (cheats stay off in every
    match), and system link (item 1). [acct2-ocr] Since 11-07 the system link
    host is the host (item 1), so the Server tab shows on the host's PC too.
  - Tested in run acct2-ocr-host1 (split screen, mp_merv,
    `--saved levitation_fix=0`, build 20261107-124131, 82 s):
    - The log said "this PC alone plays the match: its own match rules
      apply (player manager yes, remote players 0, host connection
      00000000, peer index 0)" and "the first unstick on this console
      (levitation_fix 0)". Before, it said "levitation_fix 1, the server's".
    - The player push ran unskipped (2402 calls, 0 skipped), and with the
      fix off the old 12.11 lifts came back.
    - 0 errors, 0 crashes.
  - System link, run acct2-ocr-sl-c (host A, joiner B, both set to
    `levitation_fix 0`):
    - The host was alone for its first 25 s, so its own 0 applied. When B
      joined, it logged "other consoles play the match ... (remote players
      1, host connection 00000000, peer index 0)" and went back to the
      default 1.
    - The joiner logged "(remote players 0, host connection 00000001, peer
      index 1)" and stayed at the default 1 ("the server's") the whole
      time. Note that the joiner's slot table showed no remote player, so
      the host-connection check is what keeps a joiner locked.
    - Both test syslink checks PASSED; 0 errors and 0 crashes on both
      consoles (232 s and 231 s).
    - So the bdSession's +60 tells the host (0) from a joiner (nonzero) in
      system link. That is the first half of item 1; the channel to the
      guests is the other half.
17. **The controller's settings per player, the CONTROLLER pages and the field
    of view per player: built, not run** (acct2-ocr, 2026-11-07 22:16 to 23:14;
    7.9, 7.10, 8.1). Builds 20261107-224745, -225842 and -231407, both
    programs. Nothing was run because the owner was playing. To check, and
    what is known:
    - The campaign's page buttons are now five on one line (GAMEPLAY, VIDEO,
      MOVEMENT, AIM, CONTROLLER), 0.36 of the usual distance apart. Longer
      languages may overlap; it needs a look.
    - A controller setting set in F2 or the console (not -1) follows the game's
      Options page at once. If you then answer "No" to the page's "save
      changes?", that change stays. With the settings at -1 (the default) the
      page works as it always did.
    - The new menu texts (CONTROLLER, SENSITIVITY LIMIT, STICK DEAD ZONE,
      VIBRATE WITH KEYBOARD, their notes, OPTIONS / CONTROLLER) are English in
      every language until their translations are added to the text packs
      (acct2-ocr's own work, next).
    - cod3mp's Pause > OPTIONS page called CONTROLS is now CONTROLLER, with ten
      rows; with ten rows the page has no note line.
    - The custom crosshair is still one for every split-screen player
      (acct2-decomp's crosshair work).
    - The game's stick speed (its own `m_yaw` / `m_pitch`, 0.7) can't be set by
      name in the console: there those names are World at War's mouse settings.
      The sensitivity rows (up to 100 now) are the way to turn faster.
18. **The port's settings as rows of the game's own Options pages: built, not
    run** (acct2-ocr, 2026-11-07 23:24 to 23:40; 7.11). Nothing was run
    because the owner was playing. To check, and what is known:
    - The campaign's GAMEPLAY and CONTROLLER pages have ten rows each now, as
      AIM has; their note line under the rows needs a look.
    - The new labels are English in every language until the text packs get
      their "@port" rows (acct2-ocr, next); values and notes are English.
    - TEXT LANGUAGE changes the game's text from the next text it asks for:
      the page's own labels change when the page opens again. Russian,
      Chinese, Japanese and Korean, and a change between them and the Latin
      ones, apply after a restart (as in F2).
    - USE HOLD TIME is each player's own (owner 23:40: "it's per person";
      7.4). Which local player a Use belongs to is taken from the local
      players' entity table; a split-screen run with `cod3_debug_use_hold`
      should log "local player 2" for the second player's Use. In system
      link each console runs its own players' Use, so each keeps their own
      (from the code; not seen in a two-console run yet).
    - The mouse rows are the keyboard and mouse player's (the local client
      on controller 0): the other players' menus show them and don't change
      them.

---

## 2. Frame rate

### 2.1 The campaign's game in fixed updates

- **What:** the campaign used to run its whole game frame once per drawn
  frame, with that frame's time: the AI, grenades and rockets, doors and
  movers, vehicles, physics objects and scripts. So above 60 fps things moved
  and collided differently. Now the game runs in fixed updates, and moving
  things are drawn between them.
- **Settings:**
  - `cod3_server_hz` 60 (range 0-240; 0 = the game's own, once a frame).
  - Drawing between updates, all on by default: `cod3_server_smooth`,
    `cod3_server_smooth_stationary`, `cod3_server_smooth_view`,
    `cod3_server_smooth_turret`, `cod3_server_smooth_ride_vehicle`,
    `cod3_server_client_time`, `cod3_server_step_draw`,
    `cod3_server_step_sync`, `cod3_server_step_systems`, `cod3_move_hz_sync`.
    Two of them take numbers: `cod3_server_smooth_ride` 1 and
    `cod3_server_smooth_body` 1. Two are bit masks of camera modes (acct2-ocr,
    2026-11-07, item 15): `cod3_server_smooth_cams` (mode 15, a camera set
    in the updates alone) and `cod3_server_smooth_linked` (mode 12, a player
    carried by something the level moves).
  - Logs: `cod3_debug_server_step`, `cod3_debug_view_trace`,
    `cod3_debug_pose`.
- **Where:** SP. `game/server_step.{h,cpp}`.
- **Chat:** acct2-main (2026-10-26), claude-9b, claude-9b-think; acct2-ocr
  (2026-11-07, the two camera mode settings).
- **State:** Tested. It has been on in every campaign run since late October,
  and other chats' checks compare it with 0 (acct2-decomp's runs, 11-05).
- **Note:** by design, a frame longer than 6 updates (a level load or a long
  hitch, about 100 ms at 60 Hz) runs as one update of all its time, as the
  game itself does.

### 2.2 The multiplayer host in fixed updates

- **Settings:** `cod3mp_server_hz` 0 (off), `cod3mp_server_step_systems` on,
  log `cod3mp_debug_server_step`.
- **Where:** MP. `game/mp/server_step_mp.cpp`.
- **Chat:** claude-9b (10-27).
- **State:** Off; see 1.3. Only a host changes; guests draw the snapshots.

### 2.3 Physics in fixed steps

- **What:** window glass, boxes, doors, debris and ragdolls were stepped with
  the frame's time. Above 60 fps the small steps let glass and boxes hit hard
  enough to kill the player. Now physics runs 60 steps a second.
- **Settings:** `cod3_physics_hz` 60 (0 = the game's own),
  `cod3_physics_ragdoll_pose` on, `cod3_physics_vehicle_wheels` on, log
  `cod3_physics_vehicle_log`.
- **Where:** SP ("campaign only for now"). In multiplayer, physics runs with
  the host's frames (1.3). `game/physics_step.cpp`.
- **Chat:** claude-9b, acct2-main.
- **State:** Tested, on in every campaign run. acct2-decomp's 11-05 Forest
  checks also ran with it at 0.

### 2.4 Animation clocks

- **What:** HUD, menu and loading-screen animations ran 16.7 times too fast.
  The game's fallback clock counted the emulated 1000 Hz vblanks as 60 Hz
  ones.
- **Settings:** `cod3_fix_ui_clock` on, `cod3_fix_frame_param` off (see
  1.8), log `cod3_debug_anim_clock`.
- **Where:** both. `game/anim_clock.{h,cpp}`.
- **Chat:** claude-9b (10-27).
- **State:** Built. Measured in Black Burn on 2026-11-07 (1.8): world time
  ran at real time, and no material used the frame number.

### 2.5 The frame limit

- **What:** `com_maxfps` is paced on the host's clock, so 144 really gives
  144. The game's own busy loop gave 166 and kept a core spinning. The
  guest's vblank runs at 1000 Hz so it can't act as a hidden second limit.
  The port's cap is `cod3_max_fps` (60, F2 > Display).
- **Where:** both. `game/frame_rate.{h,cpp}`; `cod3_max_fps` is in
  `ui/hud_overlay.cpp`.
- **Chat:** port.
- **Note (11-05, Saint-Lô):** `com_maxfps 30` sent by a devrun script didn't
  change the frame rate, but `--set cod3_max_fps=N` does.

### 2.6 Level scripts on host fibers

- **What:** level script threads (their waits and waittills) run on host
  fibers, so a script carries on where it stopped. The game's scripts depend
  on this. Script waits count game updates, so they run 60 times a second at
  any frame rate (claude-9b-think, 11-02).
- **Where:** both. `game/script_threads.cpp`, `game/script_fiber.cpp`,
  `game/level_hooks.cpp` (with the level configs' hooks).
- **Chat:** port, claude-9b-think.
- **State:** in use since the first playable builds.

---

## 3. Movement

### 3.1 Sprint, the sprint button and the Call of Duty 4 style (campaign)

- **What:**
  - Sprint in the campaign (the console campaign has none), held or toggled.
  - A Call of Duty 4 movement style in three parts:
    - CoD4 sprint: 4 s, then it recharges.
    - CoD4 strafe and back speeds, plus CoD4 PC's movement at 125 fps (its
      air strafing and jump height).
    - PC bounces off steep slopes.
  - `docs/MOVEMENT.md` explains every value and where it comes from.
- **Player settings:** `cod3_sprint` on, `cod3_sprint_toggle` off (hold),
  `cod3_cod4_movement` off (Call of Duty 3), the parts `cod3_cod4_sprint`,
  `cod3_cod4_speeds` and `cod3_cod4_jumps` on, log `cod3_debug_movement`.
- **Match settings (protected):**
  - `alternate_movement`: 2 in the campaign (each player's own style), 0 in
    multiplayer.
  - `alternate_movement_sprint`, `_speeds`, `_jumps`: 1.
- **Where:** SP, plus the shared rules in `game/movement/rules.cpp` (both).
  Also `game/movement/{settings,sprint_input,hooks_sp,menu_movement}`, the
  `program*.h` tables, and `game/menu_fov.cpp` (Pause > Options >
  MOVEMENT OPTIONS). F2 > Game > Movement shows the same rows.
- **Chat:** the movement chat (`dev/agents/movement/`), acct2-main,
  claude-9b, claude-9b-think.
- **State:** Tested.
  - 2026-10-16, Saint-Lô (cloud runs):
    - Walk 190.
    - Sprint 304 (CoD3) and 285 (CoD4).
    - CoD4 sprint 4 s, recharge 4 s.
    - CoD4 strafe 152 and back 133.
    - Jump 39 (CoD3) and 41.27 (CoD4 at 125 fps).
    - A pretend match with the server's rules.
  - 2026-10-18, Forest: slopes checked on the owner's PC.
  - Multiplayer: see 1.1, 1.2 and 1.4.

### 3.2 Movement steps at a fixed rate (campaign)

- **What:** how the player meets walls, stairs, slopes and the ground is the
  same at any frame rate. The view, the bob and aiming are drawn smoothly
  between the steps.
- **Settings:** `cod3_move_hz` 60 (range 0-240). These are on by default:
  `cod3_move_stairs_view`, `cod3_move_bob_smooth`, `cod3_move_aim_smooth`,
  `cod3_move_keep_box`.
- **Where:** SP. `game/movement/hooks_sp.cpp`, `step_view.h`, `settings.cpp`.
- **Chat:** claude-9b, claude-9b-think.
- **State:** Tested: claude-9b confirmed the bob and aim smoothing by A/B on
  10-27 at 10:40.
- **Note:** claude-9b's "held in place" fix (the unstick at 0x82483C20) runs
  once per usercmd that ran a step. Below 60 fps that is about half as
  often, but it frees the player at once.

### 3.3 Touches once per movement step (campaign)

- **What:** after the move, the game calls two things:
  - the area touches: the player's box against triggers and entities
    (0x823D0FE8);
  - the touch functions of what the move ran into (0x825459B8).

  Both ran once per usercmd, which means once per drawn frame. Now they run
  once per movement step (none after a usercmd with no step, at most 6). So
  edge detection and rubbing against the AI no longer depend on the frame
  rate.
- **Settings:** `cod3_move_touch_per_step` on, log `cod3_debug_move_touch`,
  test only `cod3_debug_move_touch_steer`.
- **Where:** SP. `game/movement/hooks_sp.cpp`.
- **Chat:** acct2-ocr (11-05).
- **State:** Tested.
  - Saint-Lô runs at `cod3_max_fps` 30, 60 and 0.
  - On: 60 touches a second at 30, 60 and about 257 fps.
  - Off: 30, 60 and 191 a second.
  - 0 errors.
  - The positions didn't differ there, because the guide isn't pushed by the
    player at that spot.

### 3.4 Touches 60 times a game second (multiplayer)

- **What:** the same two calls, now 60 times a game second for each player,
  counted from each usercmd's time. It applies on the host only.
- **Settings:** the same names, `cod3_move_touch_per_step` (on) and
  `cod3_debug_move_touch`.
- **Where:** MP. `game/movement/touch_pace_mp.cpp`.
- **Chat:** acct2-ocr (11-05).
- **State:** Tested, per game second:

  | Setting | Frame cap | Usercmds | Touch runs |
  |---|---|---|---|
  | on | 30 | 30 | 60 |
  | on | uncapped | 182 | 60 |
  | off | 30 | 30 | 30 |
  | off | uncapped | 155 | 155 |

  0 errors. Not covered by the runs: a different entity added from a kept
  touch list.

### 3.5 The levitation fix

- **What:** this is the Eder Dam "levitation" glitch.
  - A standing player walks into a prone player against a wall.
  - The game's unstick pushes the standing player out of the prone one and
    into the wall, then frees him from the wall upward, 12.11 units at a
    time.
  - Both players rise: from 9 to 396 units in about 8 s in the owner's 11-05
    run.

  The fix uses CoD4's rule: the unstick frees a player from walls only, never
  from another character.
- **Settings:**
  - `levitation_fix` 1 (protected; 0 = the game's own). Its row is on F2 >
    Game > Server, in both programs since 2026-11-07. In cod3mp the tab shows
    outside a match and in split screen.
  - Log `cod3_debug_stuck_fix` (MP).
  - Test probe (MP, off): `cod3_debug_levitate`, `cod3_test_levitate`,
    `cod3_test_levitate_steer`.
- **Where:** both.
  - MP: `game/movement/stuck_fix_mp.cpp` and `stuck_fix.h`; the probe is
    `levitate_probe_mp.cpp`.
  - SP: a block in `hooks_sp.cpp`.
  - Both: `protected_dvars`, `ui/server_tab.cpp`.
- **Chat:** acct2-ocr (11-05, 11-07).
- **State:**
  - Multiplayer: Tested, 0 errors in all runs.
    - Split screen, 11-05 (acct2-ocr-fix-off2 / -on3): with the fix off the
      player push moved the player 600 times per 10 s; with it on, every one
      was skipped.
    - System link, 11-05 (acct2-ocr-sl-a).
    - 11-07 (acct2-ocr-lev7-off, acct2-ocr-sl-b-A/B).
  - Multiplayer: the off switch works in split screen since 2026-11-07 (run
    acct2-ocr-host1, above). In system link the fix stays on (1.1).
  - Campaign: Built, not exercised (1.9).

### 3.6 Multiplayer movement rules, stage 1

- **Settings:** `cod3mp_movement_rule` "3", `cod3mp_movement` and `_2`..`_4`
  "3", log `cod3mp_debug_movement`, test only `cod3mp_movement_stage2` off.
- **Where:** MP. `game/movement/hooks_mp.cpp`.
- **Chat:** claude-9b-think (11-02), acct2-main.
- **State:** UNTESTED. It only logs; see 1.4.

### 3.7 Mantling

Taken out on 2026-10-20. The files remain but aren't built (1.13).

---

## 4. AI (campaign)

### 4.1 The AI's shots at the player above 60 fps

- **Settings:** `cod3_fix_ai_player_history` off. When on, the AI reads the
  player's movement over the same time span as at 60 fps.
- **Where:** SP. `game/ai_aim.cpp`.
- **Chat:** claude-9b-think (11-02).
- **State:** UNTESTED. The owner says the AI shooting is fixed (2026-11-07);
  this setting stays off by default.

### 4.2 AI logs

- **Settings, all off:**
  - `cod3_debug_ai_shots`, `cod3_debug_ai_spawns`, `cod3_debug_ai_accuracy`
    (`game/ai_shots.cpp`);
  - `cod3_debug_ai_census`, `cod3_debug_ai_pace` (`game/ai_census.cpp`; the
    pace log is acct2-ocr's).
- These change nothing.
- **Findings:**
  - Script threads and their waits run once per game update.
  - Actors move at the same speed at any frame rate.
- **Chat:** claude-9b-think, acct2-ocr.

---

## 5. Aim

### 5.1 Raw mouse aim

- **What:** the mouse turns the view directly, World at War style, instead of
  pretending to be the right stick.
- **Settings:** `cod3_raw_mouse` on, `cod3_sensitivity` 5.0, `cod3_m_yaw`
  0.022, `cod3_m_pitch` 0.022, `cod3_invert_mouse` off,
  `cod3_zoom_sensitivity` 1.0, `cod3_mouse_game_sensitivity` off,
  `cod3_mouse_resample` on.
- **Where:** both. `game/mouse_look.{h,cpp}`.
- **Chat:** port, claude-9b, claude-9b-think.
- **State:** in use; `cod3_mouse_resample` has been on since setup tool
  2.25.0 (11-02).

### 5.2 Sticky Aim (the aim assist) at a fixed 60 Hz, and its match rules

- **What:** the same strength at any frame rate, and never stronger than the
  game's own (owner 10-20: "aim assist cannot scale up or down based on
  FPS").
- **Settings:**
  - `cod3_sticky_aim_smooth` on (campaign), log `cod3_debug_sticky_aim`.
  - `sticky_aim` 1 (protected; 0 = off for everyone, 1 = each player's own).
  - `aim_slowdown` 100 (protected; 0-100 %).
- **Where:** both (the smoothing only in SP). `game/sticky_aim.{h,cpp}`.
- **Chat:** claude-9b, claude-9b-think.
- **State:** Tested: the smoothing by A/B on 10-27. In multiplayer the rules
  are the PC's own in split screen since 2026-11-07, and stay at their
  defaults in system link (1.1).

### 5.3 The aim assist switch

- **Settings:** `cod3_aim_assist` -1. -1 follows Options > Gameplay > Sticky
  Aim, 0 is off, 1 is on. It is applied again whenever the profile loads.
- **Where:** SP; in multiplayer it is set per controller profile.
  `game/aim_assist.{h,cpp}`.
- **Chat:** port.

### 5.4 Sensitivity per weapon group, from the hip and aiming

- **Settings:**
  - `cod3_weapon_sensitivity` "" for player 1; `_p2`..`_p4` for split
    screen.
  - The value is a list of `group=hip/aiming` percentages from 0 to 200. A
    group left out stays at 100/100.
  - Log: `cod3_debug_weapon_sensitivity`.
- **Where:** both; the AIM page in the campaign's Options is
  `game/menu_aim.cpp`, SP. `game/aim_sensitivity.{h,cpp}`.
- **Chat:** port, claude-9b (owner 10-20).

---

## 6. Cheats

### 6.1 One cheat switch; g_speed and g_gravity protected

- **What:**
  - `cheats` / `sv_cheats` is the only way to turn cheats on or off, from
    every source: the console, autoexec, the config file and F2.
  - In a match only the host may use it. Nobody can yet, not even in split
    screen (`mp::IsHost()` is unchanged; 1.1).
  - `g_speed` and `g_gravity` are cheat-protected. The level scripts' forced
    writes are not affected.
- **Where:** both. `game/cheat_guard.{h,cpp}`.
- **Chat:** port; the g_speed and g_gravity part is acct2-ocr's (11-05).
- **State:** Tested (acct2-ocr-cheatdv-sp): refused with cheats off, and
  reset by `sv_cheats 0`.

### 6.2 The CHEATS USED mark

- **What:**
  - The mark appears once a cheat has worked: god, notarget, noclip, ufo,
    give, setviewpos, jumptonode, or a change to a cheat-protected dvar.
  - "Cheats enabled" or "Cheats disabled" shows the live state.
  - Each cheat used is listed, green while on and red once off.
- **Settings:** log `cod3_debug_cheats`.
- **Where:** both. `game/cheat_watch.{h,cpp}`, `ui/hud_overlay.cpp`.
- **Chat:** claude-9b, acct2-ocr (11-05).
- **State:** Tested (acct2-ocr sp4, sp5 and mp3, 11-05).
- **Multiplayer:** `session.cpp` turns the game's own `sv_cheats 1` off again
  at each map load and during a match.

---

## 7. Controls

### 7.1 Keyboard and mouse keys call the game's own actions

- **What:**
  - Fire, aim, use, reload, grenades, melee, binoculars, sprint, lean, jump,
    stance and weapons each get a key of their own. Before, the keyboard
    pretended to press controller buttons, so R reloaded and F used, but both
    pressed X.
  - One key can hold up to three actions (acct2-main).
  - In F2, Backspace or Delete in a key's box removes the bind (acct2-ocr,
    11-05).
- **Settings:** `cod3_binds_kbm_p1` "" (the defaults), `cod3_binds_actions`
  on, `cod3_aim_toggle_kbm_p1` off, `cod3_binds_scores_pad` on (campaign),
  log `cod3_debug_binds`, test only `cod3_test_vehicle_hold`.
- **Where:** both. `game/keybinds/{binds,model,keys}`.
- **Chat:** the keybinds chat, claude-9b, claude-9b-think, acct2-main,
  acct2-decomp, mp-menus, acct2-ocr.

### 7.2 Controller binds and dead zone per player

- **Settings:**
  - `cod3_binds_pad_p1`..`_p4` "": only the buttons that differ from the
    layout. START stays the pause button.
  - `cod3_deadzone_p1`..`_p4` -1: 0-100 of the stick's 128. -1 means
    `joy_threshold`, the game's 25, shared by all players.
  - F2 > Controls > Controller has a `joy_threshold` slider (acct2-ocr,
    11-05).
- **Where:** both. `game/keybinds/{pad_binds,pad_model}`,
  `ui/hud_overlay.cpp`.
- **Chat:** acct2-main (11-05), acct2-ocr.
- **State:** Tested with scripted pads, 11-05 09:33: the dead zones gave each
  player the expected values, 0 errors. F2 > Controls > Controller has their
  rows since 2026-11-07 13:15 (acct2-ocr; built, not yet looked at). The
  game's own menus have none (1.6), and there is no test with a real
  controller (1.11).

### 7.3 Multiplayer grenade keys follow its layouts

- **What:** multiplayer's `mp_xenon_*.cfg` can give the frag grenade and the
  special grenade the other shoulder. The keyboard's two grenade keys now
  follow it.
- **Where:** MP. `game/mp/controls_mp.cpp`, `controls_rules.h`.
- **Chat:** claude-9b (owner 10-19).

### 7.4 How long Use is held

- **Settings:** `cod3_use_hold_ms` -1, the game's own `g_useholdtime` (250
  with every layout). 0 means a plain press, and the prompts say so.
  cod3mp split screen (acct2-ocr, 23:58; owner 23:40 "it's per person"):
  `cod3_use_hold_ms_p2`..`_p4` -1 (= player 1's), -2 (the game's own) or
  milliseconds. The server's Use check (cod3mp 0x8256B478, r3 the entity)
  takes the hold of the local player whose entity it is (the local players'
  entities `ReadU32(ReadU32(0x82BCC7FC) + 4 * (local client + 1))`); an
  entity of none of this console's players gets the game's own. The hints
  follow the player whose text it is. `cod3_debug_use_hold` logs whose hold
  each check took. The old note "the host's setting counts for everyone in
  its match" was wrong: each console runs its own players' ClientThink.
- **Where:** both. `game/use_hold.{h,cpp}`, with its prompts in
  `button_prompts.cpp`; F2 > Prompts and each player's Pause > OPTIONS >
  GAMEPLAY (7.11).
- **Chat:** acct2-decomp (11-05), acct2-ocr (per player, 11-07).
- **State:** see 1.10.

### 7.5 Battle actions (QTEs) with any input and at any frame rate

- **What:**
  - With the mouse, swirls, stick toggles and rowing use a spinner. These
    used to soft-lock keyboard players (owner 10-13).
  - Above 60 fps, swirls progress as fast as on the console.
  - Timed events (a crane's drop, levers, valves) fire at any frame rate.
- **Settings:** `cod3_qte_style` "spinner", `cod3_qte_spinner_with_pad` on,
  `cod3_qte_spinner_scale` 1.0, `cod3_qte_fps_fix` on,
  `cod3_turret_hints_any_input` on, `cod3_fix_interact_events` on, test
  only `cod3_qte_demo` "off".
- **Where:** SP; multiplayer has no battle actions. `game/qte_actions.{h,cpp}`,
  `game/qte_math.h`, `ui/qte_spinner`.
- **Chat:** claude-setup, acct2-main (11-05 19:06: the Forest mortar's swirl
  turret), claude-9b-think.

### 7.6 The stick arrow with the keyboard

- **Settings:** `cod3_stick_arrow_keys` on. With the keyboard and mouse, the
  game's own stick arrow (`cod3_qte_style` original) shows a MOUSE key.
- **Where:** SP. `game/stick_arrow_keys.cpp`.
- **Chat:** claude-9b-think, claude-9b (10-27).

### 7.7 Prompts show your own keys and buttons

- **Settings:** `cod3_key_prompts` on, `cod3_glyph_style` "auto",
  `cod3_key_caps` on, `cod3_key_cap_scale` 1.0. Translated stick hints get
  keyboard-and-mouse words too (`config/prompt_text_kbm.tsv`).
- **Where:** both. `game/button_prompts.{h,cpp}`.
- **Chat:** port, claude-9b, claude-setup, acct2-decomp, mp-menus,
  acct2-ocr.

### 7.8 No rumble while the keyboard is in use

- **Settings:** `cod3_rumble_with_keys` off.
- **Where:** both. `game/rumble.{h,cpp}`.
- **Chat:** port.

### 7.9 One sensitivity cap, and the controller's settings per player

- **What:**
  - The game's own pages stopped Horizontal Sensitivity at 37 and Vertical
    at 50: the campaign's Options > Controls in its left / right
    (`sub_824DC1C0`), cod3mp's Pause > OPTIONS (Init `0x82500868`) and its
    main menu's Options > Controls (Init `0x823F15B0`) in their gauges' top
    steps. Both rows now go to one cap (owner 22:08: "same cap for both").
  - The stick turns by the row x 0.04 (0 counts as 0.1) x the game's own
    `m_yaw` / `m_pitch` (0.7) in CL_MouseMove (campaign `0x8250FF48`, cod3mp
    `0x8252B578`). `cl_yawspeed` / `cl_pitchspeed` only turn with the keys.
  - Horizontal and Vertical Sensitivity, Invert Aim and Vibration are
    settings for each player. -1 leaves the row to the game's page; set, it
    is written into that player's own control settings each frame it differs,
    and a change on the game's page becomes the setting's. In cod3mp, local
    player N's are written into the controller profile that player's client
    uses (`*(0x82C7A270 + 180 + N * 3208)`, in use while `+184` isn't 0), not
    profile N.
- **Settings:** `cod3_pad_sensitivity_max` 50 (37-100);
  `cod3_pad_horizontal_sensitivity`, `cod3_pad_vertical_sensitivity`,
  `cod3_pad_invert`, `cod3_pad_vibration` -1 (cod3mp also `_p2`..`_p4`).
  F2 > Controls > Controller, with a player picker in cod3mp.
- **Where:** both. `game/pad_settings.{h,cpp}` (synced each frame from
  `game/engine.cpp`), `game/menu_fov.cpp` (SP), `game/mp/options_mp.cpp` (MP),
  `ui/hud_overlay.cpp`.
- **Chat:** acct2-ocr (owner 22:00-22:48).
- **State:** built, not run (item 17). World at War's `aim_turnrate_yaw` /
  `_pitch` were first mapped to `cl_yawspeed` / `cl_pitchspeed` (22:16); that
  was wrong (those are the keys') and is gone.

### 7.10 A CONTROLLER page in the game's own menus

- **What:** (owner 22:22: "a controller tab in every settings menu")
  - Campaign: Pause > Options has a fifth page button, CONTROLLER: SENSITIVITY
    LIMIT, STICK DEAD ZONE (player 1's own; stepping onto the shared
    `joy_threshold` value hands it back to that), VIBRATE WITH KEYBOARD.
  - cod3mp: Pause > OPTIONS' CONTROLS page is CONTROLLER now, with the game's
    own rows and SENSITIVITY LIMIT and STICK DEAD ZONE for the player whose
    menu it is (the page's local client, `+80`).
  - A change saves at once. cod3mp never set `protected_dvars`' save
    handler, so such saves waited for the exit; it does now
    (`cod3mp_app.cpp`).
- **Settings:** `cod3_pad_sensitivity_max`, `cod3_deadzone_p1`..`_p4`,
  `cod3_rumble_with_keys` (7.2, 7.8, 7.9).
- **Where:** both. `game/menu_controller.{h,cpp}`, `game/menu_fov.cpp` (SP),
  `game/options_pages.{h,cpp}` and `game/mp/options_mp.cpp` (MP),
  `cod3mp_app.cpp`.
- **Chat:** acct2-ocr.
- **State:** built, not run (item 17).

### 7.11 The port's settings in the game's own Options pages

- **What:** (owner 23:17: "Option menus need more options. We can edit a lot")
  the port's settings as rows of the pause menu's Options pages, the same
  settings F2 and the ~ console have. A change applies and saves at once.
  - Campaign, Pause > Options:
    - GAMEPLAY: BATTLE ACTIONS, SKIP CUTSCENES, HOLD TO SKIP, CUTSCENE BARS,
      USE HOLD TIME, LEFT-HANDED WEAPON, VEHICLE FOV SCALE, TEXT LANGUAGE,
      VOICES, MASTER VOLUME.
    - CONTROLLER: SENSITIVITY LIMIT, STICK DEAD ZONE, VIBRATE WITH KEYBOARD,
      KEYS IN PROMPTS, KEYS AS KEY CAPS, BUTTON PICTURES, RAW MOUSE, INVERT
      MOUSE, MOUSE ADS SCALE, EVEN MOUSE TURNING.
    - The rows with numbers are sliders with a number box (HOLD TO SKIP,
      VEHICLE FOV SCALE, MASTER VOLUME, MOUSE ADS SCALE), like AIM's.
  - cod3mp, Pause > OPTIONS (six categories now):
    - GAMEPLAY: FIELD OF VIEW, FOV SCALE, VEHICLE FOV SCALE, LEFT-HANDED
      WEAPON, USE HOLD TIME, SPLIT-SCREEN HUD.
    - CONTROLLER: as 7.10.
    - AIM: STICKY AIM, MOUSE SENSITIVITY, MOUSE ADS SCALE, INVERT MOUSE, RAW
      MOUSE, EVEN MOUSE TURNING.
    - VIDEO: as before.
    - AUDIO (new): MASTER VOLUME, VOICES.
    - INTERFACE (new): TEXT LANGUAGE, KEYS IN PROMPTS, KEYS AS KEY CAPS, BUTTON
      PICTURES, BUTTON COLORS, COLORS IN NAMES, SHOW VERSION.
  - Split screen (owner 22:24, 22:48, 23:40): VEHICLE FOV SCALE, LEFT-HANDED
    WEAPON and USE HOLD TIME are the menu's player's own. New per player:
    `cod3_vehicle_fov_scale_p2`..`_p4` (-1 = player 1's), used on that
    player's view only, and `cod3_use_hold_ms_p2`..`_p4` (7.4). The mouse rows
    are the keyboard and mouse player's (the local client on controller 0).
    The rest is the machine's (one sound, one text language; "auto" button
    pictures are each player's own controller's).
- **Settings:** `cod3_skip_movies`, `cod3_skip_hold_ms` (9.1);
  `cod3_letterbox`; `cod3_use_hold_ms` (7.4); `cod3_viewmodel_flip` and
  `_2`..`_4` (8.3); `cod3_vehicle_fov_scale` and `_p2`..`_p4` (8.1);
  `cod3_text_language`; `cod3_voice_language`; `cod3_volume`;
  `cod3_key_prompts`, `cod3_key_caps`, `cod3_glyph_style`,
  `cod3_glyph_color_mode` (7.7); `cod3_raw_mouse`, `cod3_invert_mouse`,
  `cod3_sensitivity`, `cod3_zoom_sensitivity`, `cod3_mouse_resample` (5.1);
  `cod3_color_codes_in_text`; `cod3_show_version`; `cod3_hud_safe_area`
  (MP).
- **Where:** both. `game/menu_port_rows.{h,cpp}` (the rows),
  `game/menu_fov.cpp` (SP pages), `game/options_pages.{h,cpp}` and
  `game/mp/options_mp.cpp` (MP pages), `game/text_language.{h,cpp}` (the
  language choice for the row), `game/fov.{h,cpp}` (the vehicle FOV per
  player).
- **Chat:** acct2-ocr.
- **State:** built, not run (item 18).

---

## 8. View

### 8.1 Field of view

- **What:**
  - `cg_fov` (the game's 65, range 1-160) and `cg_fovScale` (World at War's)
    are kept apart from the game's own FOV changes (scripted zooms,
    interactions) and saved.
  - There are FIELD OF VIEW and FOV SCALE rows in the Options menus.
  - cod3mp split screen (acct2-ocr, 2026-11-07 23:00): players 2 to 4 can
    have their own field of view and FOV scale, on their own view only.
    CG_GetViewFov runs once per local client's view (the client in
    `*0x82BCBE9C`); for such a player cgame's copy of `cg_fov` holds theirs
    during that call only, unless a script or an interaction has the copy.
    Aiming down the sights narrows each view from its own player's FOV, as
    the game always did per player.
  - The mouse's slow-down while aiming down the sights now takes its zoom from
    the keyboard and mouse player's view only (cod3mp: the local client on
    controller 0; 23:58, before that player 1's), against that player's own
    field of view; in split screen it came from whichever view was drawn last.
  - The vehicle FOV scale is per player too (acct2-ocr, 23:30): players 2 to 4
    can have their own, on their own view (7.11).
- **Settings:** `cod3_vehicle_fov_scale` 1.0; `cod3_fov_p2`..`_p4` and
  `cod3_fov_scale_p2`..`_p4` -1 (= player 1's), MP, in F2 > Game > View and
  each player's own Pause > OPTIONS > GAMEPLAY; `cod3_vehicle_fov_scale_p2`..`_p4`
  -1 (= player 1's), MP, in the player's own Pause > OPTIONS > GAMEPLAY and
  the console.
- **Where:** both. `game/fov.{h,cpp}`; `game/menu_fov.cpp` (SP);
  `game/mp/options_mp.cpp` (MP).
- **Chat:** port, claude-9b, mp-menus, acct2-ocr.

### 8.2 Weapon position

- **Settings:** `cod3_viewmodel_offset_x`, `_y`, `_z` 0 and
  `cod3_viewmodel_presetpos` 0 (Counter-Strike 2's commands).
- **Where:** both. `game/viewmodel.{h,cpp}`.
- **Chat:** claude-9b.

### 8.3 The weapon in the left hand

- **Settings:** `cod3_viewmodel_flip` and `_2`..`_4` off. The key "Flip
  weapon hand" switches it.
- **Where:** both. `game/viewmodel_flip.{h,cpp}`.
- **Chat:** claude-9b (10-27).

### 8.4 A crosshair for each split-screen player

- **Where:** MP. `game/mp/hud_mp.cpp`.
- **Chat:** claude-9b.

### 8.5 The ridden tank's turret between updates

See 1.7. `cod3_blend_ride_bones` 0, SP, `game/ride_bones.{h,cpp}`,
claude-9b-think. Tested by its log in Black Burn on 2026-11-07 (runs
acct2-ocr-ride1 and -ride2); how it looks is still to be checked.

### 8.6 Full-screen 2D reaches the picture's edges

This is how the picture looks, not how the game plays. It is listed because
the owner asked for item 16 to be checked.

- What it does: the game's fades, flashes, overlays and the cinematic bars
  end half a unit short of the right and bottom edges, so the picture's last
  column and row showed what was under them.
  - The bars' own snap (`cod3_letterbox`, claude-9b-think, with acct2-ocr's
    fix) takes them 2 units past the edges.
  - `cod3_edge_fill` does the same for any other quad that spans the
    screen's width or height. That also covers a split-screen view's width.
- Settings:
  - `cod3_edge_fill` (cod3/Display): 1 by default; 0 is off. 2 does this
    only for 2D drawn into the game's off-screen pictures, and 3 does both.
    No such off-screen quad was seen in the test runs.
  - `cod3_debug_edge_fill` logs the quads it sees.
- Programs: both. File: `game/ultrawide.cpp`. Chat: acct2-ocr.
- State: **Tested** (item 16): Hostage native and scaled, and cod3mp split
  screen. The movie and ultrawide windows were not run.

---

## 9. Cutscenes

### 9.1 Skipping cutscenes

- **Settings:** `cod3_skip_movies` off, `cod3_skip_prompt` on,
  `cod3_skip_prompt_position` "corner", `cod3_skip_hold_ms` 1000,
  `cod3_skip_movies_after_ms` 250.
- **Where:** SP; multiplayer plays no films. `game/cutscene_skip.{h,cpp}`.
- **Chat:** port, claude-9b (the movie-skip hang fix, 10-28),
  claude-9b-think, acct2-decomp.

---

## 10. Sources

- The sources and both source lists as of 2026-11-07.
- `docs/MOVEMENT.md`, `docs/KEYBINDS.md`, `dev/agents/BOARD.md`,
  `dev/agents/acct2-ocr/NOTES.md`.
- `dev/agents/acct2-ocr/tools/patch_inventory.py` prints each file's top
  comment, its settings with their defaults, and its hook count. Usage:
  `python3 -I patch_inventory.py SRC_DIR HEAD_CHARS FILE...`.
