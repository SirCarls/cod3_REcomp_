// bo1 - Call of Duty: Black Ops (Xbox 360) static recompilation, the campaign and zombies program. [waw-decomp]
// The multiplayer program (bo1mp) will build from the same file with BO1_MP defined, as Call of Duty 4's cod4mp.

#ifdef BO1_MP
#include "generated/default_mp/bo1mp_init.h"
#else
#include "generated/default/bo1_init.h"
#endif

#include "bo1_app.h"

#ifdef BO1_MP
REX_DEFINE_APP(bo1mp, Bo1App::Create)
#else
REX_DEFINE_APP(bo1, Bo1App::Create)
#endif
