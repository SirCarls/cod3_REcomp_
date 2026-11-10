// cod4 / cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation. [acct2-cod4]
// The same file builds both programs: COD4_MP is defined for the multiplayer one (mp/CMakeLists.txt).

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_init.h"
#else
#include "generated/default/cod4_init.h"
#endif

#include "cod4_app.h"

#ifdef COD4_MP
REX_DEFINE_APP(cod4mp, Cod4App::Create)
#else
REX_DEFINE_APP(cod4, Cod4App::Create)
#endif
