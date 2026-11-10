// cod2 - from launch to the main menu without test settings (front_end.cpp).
// [acct2-cod2]

#pragma once

namespace cod2::front_end {

// The SDK's keyboard pad on, Return/Escape as START, unless the player set
// them. From Cod2App::OnPreSetup, before the input drivers start; logs the
// keyboard and start-up film settings once.
void ApplyInputDefaults();

}  // namespace cod2::front_end
