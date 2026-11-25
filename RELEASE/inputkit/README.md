# inputkit

The input code the ports share: key names, the key-bind model, the controller-bind model, mouse motion
resampling and turn math, per-weapon-group sensitivity, the stick's radial dead zone, and other shooters'
sensitivity scales (`sens_scale.h`: type in your number from Valorant, modern Call of Duty, Fortnite and nine more,
and turn the same distance per inch here; monitor-distance zoom matching too), and which device the player is
using (`activity.h`: the keyboard and mouse or the first controller, for its vibration and prompts). It started as
Call of Duty 3's `game/keybinds`, `mouse_look` and `aim_sensitivity`; those files now call into it, and the
other ports (World at War, Call of Duty 2, Call of Duty 4, Big Red One, Finest Hour) use it too.

It has no game data, no addresses and no engine code. Each game keeps its own action table, its own
hooks (where the keys and the turn reach the game) and its own settings names.

## Using it

C++ ports (ReXGlue): `#include "inputkit/binds.h"` and friends, namespace `inputkit`.
C ports (xboxrecomp): `#include "inputkit/inputkit.h"`, the `ik_*` functions.

CMake: each game finds the folder through `INPUTKIT_DIR` first (the setup tool passes it, since its copy of a port
in the work folder can't see the project), then by where it sits in the project:

```cmake
include("${INPUTKIT_DIR}/cmake/Inputkit.cmake")
inputkit_add_library()
target_link_libraries(my_game PRIVATE inputkit::inputkit)   # C++
inputkit_link_c(my_game)                                    # C: links as C++; MinGW gets the C++ runtime built in
```

## Who uses what

| Game | Files | What it takes from here |
|---|---|---|
| Call of Duty 3 | `recomp/src/game/keybinds/`, `mouse_look.cpp`, `aim_sensitivity.cpp` | everything: the key and pad binds, mouse resampling and turn math, per-weapon sensitivity |
| World at War | `recomp_waw/src/waw_mouse.cpp`, `waw_rumble.cpp` | mouse capture (`rex/pointer_capture.h`), resampling and turn math, controller 1's vibration held back while on the keyboard and mouse (`rex/rumble_hold.h`) |
| Call of Duty 4 | `recomp_cod4/src/cod4_mouse.cpp`, `cod4_rumble.cpp` | the same as World at War |
| Call of Duty 2 | `recomp_cod2/src/cod2_pc_input.cpp`, `game/rumble.cpp` | the same as World at War, and controller 1's vibration held back while on the keyboard and mouse (`rex/rumble_hold.h`, on `activity.h`) |
| Big Red One | `bro/src/input/bro_binds.c`, `bro_pad.c` | the key names and bind rules (C interface), the sticks' dead zone |
| Finest Hour | `fh/src/fh_pad.c` | the key names and bind rules (C interface), the sticks' dead zone |

Big Red One's and Finest Hour's keys were checked against their old code key by key:
`dev/agents/input-port/tests/bro_compare/` and `fh_compare/` (what differs on purpose is listed in each script).

## Tests

```sh
cmake -S inputkit -B build-inputkit -DINPUTKIT_BUILD_TESTS=ON
cmake --build build-inputkit -j
ctest --test-dir build-inputkit --output-on-failure
```

`inputkit_test` covers the C++ side, `inputkit_test_c` the C interface, `inputkit_test_frame_rate` the turn at
20 to 1000 fps, `inputkit_test_sens_scale` the other games' scales against the calculators' worked examples,
`inputkit_test_activity` which device is in use, with the same hands at 20 to 1000 reads a second. Call of Duty 3's own
`dev/agents/keybinds/tests/test_binds.cpp` must still pass after any change here, since its note texts come
from this code word for word.

## Rules

- Keep it game-neutral: anything that knows an address, a cvar or a menu belongs in the game.
- Change a note's wording here only together with the tests that check it.
- No allocation on the game thread's per-frame paths (`MotionQueue::Take`, `TurnFromCounts`, `weaponsens::Table::Multiplier`).
