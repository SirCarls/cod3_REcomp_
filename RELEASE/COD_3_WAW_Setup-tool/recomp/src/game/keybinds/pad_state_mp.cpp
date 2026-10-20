// cod3mp - the pad state the game reads, for the keyboard's actions inside an
// interaction (game/keybinds/binds.h). Compiled into cod3mp only
// (mp/CMakeLists.txt): the campaign's game/button_prompts.cpp has this hook
// and calls binds::ApplyToPad itself.
//
// sub_82344F98 is the campaign's name: game/mp/mp_aliases.h makes it
// multiplayer's 0x82314360 (the same three-instruction wrapper of
// XamInputGetState: r3 user, r4 X_INPUT_STATE*; mp/config/campaign_map.tsv).

#include <cstdint>

#include <rex/hook.h>

#include "game/guest.h"
#include "game/keybinds/binds.h"

REX_EXTERN(__imp__sub_82344F98);
REX_HOOK_RAW(sub_82344F98) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82344F98(ctx, base);
  // A pad the game reads, read without error: the controller buttons the
  // keys press inside an interaction go in.
  if (ctx.r3.u32 == 0 && state >= 0x10000 && state < 0xC0000000u) cod3::binds::ApplyToPad(base, user, state);
}
