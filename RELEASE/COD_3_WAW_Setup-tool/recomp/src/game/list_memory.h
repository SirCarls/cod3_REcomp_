// cod3 - [claude-9b-think] 2026-11-02 the render lists' frame memory (list_memory.cpp; campaign only). UNTESTED.
#pragma once

#include <cstdint>

namespace cod3::list_memory {

// Before 0x82130808 opens the next frame's list: the frame just sent is measured (main thread).
void BeforeBeginNextFrame(uint8_t* base);
// Before the render back end 0x8214F770 draws the current node (render job): its callbacks and links checked.
void CheckNode(uint8_t* base);

}  // namespace cod3::list_memory
