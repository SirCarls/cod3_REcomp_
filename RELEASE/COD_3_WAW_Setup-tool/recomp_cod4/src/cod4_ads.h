// cod4 / cod4mp - the turn while aiming down the sights: the game's own zoom scaling, monitor distance, and a
// percentage on top (inputkit::sensscale::AdsScale). [input-port] 2026-11-24
//
// CL_MouseMove multiplies the mouse turn by a float in clientActive_t (campaign +51584, multiplayer +152812) that the
// cgame sets every frame to tan(zoomed fov / 2) / tan(cg_fov / 2) (campaign 0x822E15A0 computes it; 0x822FFA10 stores
// it, times an aim-assist factor). With cod4_ads_model or cod4_ads_percent changed, that float holds the player's
// choice for the one read in CL_MouseMove and the game's own value again right after, so nothing else sees it.

#pragma once

#include <cstdint>

namespace cod4::ads {

// Lives around one CL_MouseMove call. `client_active` is clientActive_t (CL_GetMouseMovement's r3; 0 before the first
// read: nothing changes).
class Swap {
 public:
  Swap(uint8_t* base, uint32_t client_active);
  ~Swap();
  Swap(const Swap&) = delete;
  Swap& operator=(const Swap&) = delete;

 private:
  uint8_t* base_;
  uint32_t address_ = 0;  // the float's guest address, 0 when nothing was changed
  uint32_t saved_ = 0;    // the game's value, as stored (big-endian bits)
};

}  // namespace cod4::ads
