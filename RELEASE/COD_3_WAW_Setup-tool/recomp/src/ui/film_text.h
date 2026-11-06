// cod3 - the films' burned-in English text, in the chosen language. [acct2-ocr]
//
// Eleven mission films carry English title cards burned into the picture
// (date and time, place, region: typed on letter by letter near the bottom),
// and the legal screens are English text films. Ours are drawn over them while
// the film plays, a dark plate over each English line and our line on it:
//   - which film: Game_PlayCinematic (0x824EED20, r3 the film's name, e.g.
//     "crssrds"; it opens movies/<name>.wmv and its sound track) is hooked here,
//     campaign only (the multiplayer program plays no mission films). Nothing
//     else hooks its entry; voice_language's mid-function hook inside it runs
//     as before;
//   - when: the time since that call returned (the film plays in real time on
//     its sound clock). The game's own elapsed value (addr::movie::kElapsedMs)
//     is reset to 0 when a level finishes loading under its briefing
//     (0x824EEBDC, dev/agents/cutscenes/NOTES.md row 9), so it isn't used.
//     cod3_film_text_delay_ms shifts ours if a film turns out late or early;
//   - while: the film object (addr::movie::kHandle) is the one that call made;
//     it goes to 0 when the film ends or is skipped;
//   - what and where: assets/text/film_cards.txt (film, first and last second,
//     the English line's box in the film's 1280x720 picture, key; made on the PC
//     by tools/film_timing.py) and the pack's "@films" rows (game/text_language.h,
//     FilmText). The picture fills the game's 640x480 screen, i.e. the HUD area
//     (ui/hud_overlay.cpp), mapped as the other overlay pieces.
// Shown when the chosen text language has the row and cod3_text_film_cards is
// on (F2 > Sound > Text > Forced), whatever the other text kinds are set to.
// Russian, Chinese, Japanese and Korean use the text layer's font
// (ui/text_overlay.h); the Latin languages ImGui's own.
//
// Unverified until a run: the timing (real time from the call) and the
// picture's place on a non-16:9 window. With the PC's own film player
// (claude-9b-think's native-movies draft, not applied) the picture would be
// letterboxed to 16:9; FilmArea() is the one place to change then.
#pragma once

#include <cstdint>
#include <string>

namespace cod3::film_text {

// Game_PlayCinematic returned: `name` as the game passed it, `opened` its result. Game thread.
void OnFilmStart(uint8_t* base, const std::string& name, bool opened);

// HudOverlay::OnDraw: the current film's lines over the HUD area (pixels).
void Draw(float x, float y, float width, float height);

}  // namespace cod3::film_text
