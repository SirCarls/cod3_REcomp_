// cod3 - [acct2-main] 2026-11-15 A text the game set from its string tables follows a change of the text language,
// in every menu and on the HUD, in both programs (owner 20:26: "swapping language within a menu bugs it out";
// 20:34: "just assume it's within all the in-game menus").
//
// Why it was needed: a menu page sets its words once, when the page is made (FEMenuEntry slot 48 -> its label's
// SetTextKey, slot 132). A text keeps the words, not the key. After cod3_text_language changes
// (game/text_language.h), every text the game asks for again comes in the new language, but the texts a page
// already holds stay in the old one: the page showed two languages until it was made again.
//
// How: the game's three ways to set a text from its string tables are hooked in text_rekey.cpp, for both kinds of
// text (FEText, and FEMultiLineText, which is one):
//   slot 128 (vfunc_32)  set from a key's hash      (the campaign's 0x8252A638 / cod3mp's 0x8254B250 lookup)
//   slot 132 (vfunc_33)  set from a key's name      (Localize: 0x8252CB98 / 0x8254D180)
//   slot 360 (vfunc_90)  a wrapped text from a key, its width and scale (the help boxes, the message boxes; cod3mp's
//                        is slot 96, 384, at 0x82501168)
// Each remembers how the text was set, the words it then showed and the text language's generation
// (text::Generation()). When such a text is about to be drawn (button_prompts.cpp's draw hooks call Refresh
// first) and the language changed since, it is set again the same way through its own vtable: the game's lookup,
// the port's Localize hooks (its own keys) and button_prompts take part as when the page was made.
//
// A text whose words changed since then some other way (a value, a count, a name set as words) is no longer
// the key's: it is left as it is and forgotten.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace cod3::textkeys {

// Before `text` (an FEText or FEMultiLineText) is drawn or read: set again from its key, hash or wrapped key when
// the text language changed since it was set that way. Game thread (wherever the game draws its texts). A text
// that wasn't set from the string tables, or whose words changed since, is left alone.
void Refresh(PPCContext& ctx, uint8_t* base, uint32_t text);

// A text made or deleted at this address: what it was set from goes.
void Forget(uint32_t text);

// Around a re-layout of the same words (button_prompts.cpp's RefreshPlain / Refresh at draw time: the button
// names of a text change, its key doesn't): the words it shows afterwards are taken as the key's.
class KeepKey {
 public:
  KeepKey(uint8_t* base, uint32_t text);
  ~KeepKey();
  KeepKey(const KeepKey&) = delete;
  KeepKey& operator=(const KeepKey&) = delete;

 private:
  uint8_t* base_;
  uint32_t text_;
  bool keyed_ = false;  // it showed its key's words before
};

}  // namespace cod3::textkeys
