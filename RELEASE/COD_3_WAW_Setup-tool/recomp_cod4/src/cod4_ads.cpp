// cod4 / cod4mp - the turn while aiming down the sights (cod4_ads.h). [input-port] 2026-11-24

#include "cod4_ads.h"

#include <cstring>

#include <rex/cvar.h>

#include "inputkit/sens_scale.h"

REXCVAR_DEFINE_INT32(cod4_ads_model, 0, "cod4",
                     "The turn while aiming down the sights: 0 the game's own (the tangents of the zoomed and normal "
                     "fields of view), 1 monitor distance (cod4_ads_coefficient; modern Call of Duty's and "
                     "Battlefield's default)")
    .range(0, 1);
REXCVAR_DEFINE_DOUBLE(cod4_ads_coefficient, 1.33, "cod4",
                      "Monitor distance: where on the screen the zoomed turn matches the normal one, in half-heights "
                      "from the center (1.33: the edge of a 4:3 picture; 0: the center, the game's own)")
    .range(0.0, 5.0);
REXCVAR_DEFINE_DOUBLE(cod4_ads_percent, 100.0, "cod4",
                      "The turn while aiming down the sights, in percent of what cod4_ads_model gives (100: as is)")
    .range(0.0, 200.0);

namespace {

#ifdef COD4_MP
constexpr uint32_t kFovScale = 152812;      // clientActive_t: CL_MouseMove 0x822D3D48 reads it (lfsx, +0x254EC)
constexpr uint32_t kCgFovDvar = 0x823F54F8;  // dvar_t* cg_fov (the cgame's zoom ratio divides by its tangent)
#else
constexpr uint32_t kFovScale = 51584;        // clientActive_t: CL_MouseMove 0x822CC838 reads it (lfsx, +0xC980)
constexpr uint32_t kCgFovDvar = 0x82488A18;  // dvar_t* cg_fov (0x822E15A0)
#endif
constexpr uint32_t kDvarValue = 12;  // dvar_t: the current value (CL_MouseMove reads its dvars there)

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

uint32_t FloatToBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

}  // namespace

namespace cod4::ads {

Swap::Swap(uint8_t* base, uint32_t client_active) : base_(base) {
  inputkit::sensscale::Ads ads;
  ads.model = REXCVAR_GET(cod4_ads_model) == 1 ? inputkit::sensscale::ZoomModel::kMonitorDistance
                                               : inputkit::sensscale::ZoomModel::kGame;
  ads.coefficient = REXCVAR_GET(cod4_ads_coefficient);
  ads.percent = REXCVAR_GET(cod4_ads_percent);
  if (client_active == 0 || (ads.model == inputkit::sensscale::ZoomModel::kGame && ads.percent == 100.0)) return;
  const uint32_t dvar = LoadBe32(base, kCgFovDvar);
  if (dvar == 0) return;  // before the cgame registered it
  const float game = BitsToFloat(LoadBe32(base, client_active + kFovScale));
  const float hip = BitsToFloat(LoadBe32(base, dvar + kDvarValue));
  // cg_fov is horizontal at 4:3 in the IW engine (the picture is widened from there).
  const float mine = float(inputkit::sensscale::AdsScale(ads, game, hip, 4.0 / 3.0));
  if (mine == game) return;  // the hip: AdsScale never changes it
  address_ = client_active + kFovScale;
  std::memcpy(&saved_, base + address_, sizeof(saved_));
  const uint32_t be = __builtin_bswap32(FloatToBits(mine));
  std::memcpy(base + address_, &be, sizeof(be));
}

Swap::~Swap() {
  if (address_ != 0) std::memcpy(base_ + address_, &saved_, sizeof(saved_));
}

}  // namespace cod4::ads
