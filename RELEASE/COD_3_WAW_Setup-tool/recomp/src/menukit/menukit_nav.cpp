// menukit - navigation and layout, the same for every game. [waw-decomp/menus1] 2026-11-23

#include "menukit.h"

#include <algorithm>
#include <cmath>

namespace menukit {
namespace {

bool ViewOf(Adapter& adapter, const Row& row, const Context& context, RowView* view) {
  RowProvider* provider = nullptr;
  std::string_view name;
  if (!FindRow(adapter, row.id, &provider, &name) || !provider->View(name, context, view)) return false;
  if (!row.note_key.empty()) view->note = adapter.Text(row.note_key);
  return true;
}

// The two looks, in the game's 640x480 menu space. The original look's numbers are the game's own Pause > Options
// (dev/agents/f2-menus/native-menus/LAYOUTS.md, measured from CoD3's draw calls); other games' adapters can still
// draw these items in their own letters.
struct Metrics {
  float title_x, title_y, title_h;
  float list_x0, list_x1;            // the category column (modern only)
  float label_x0, value_x0, value_x1;
  float rows_y, pitch, row_h;
  float note_y, rule_y;
  int rows_shown;
};

constexpr Metrics kModern = {48, 36, 30, 48, 196, 220, 430, 592, 100, 24, 22, 412, 400, 12};
constexpr Metrics kOriginal = {118, 89, 38, 0, 0, 118, 440, 528, 139, 20, 20, 389, 370, 11};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Navigator

void Navigator::Open(const Menu* menu, Look look, Adapter& adapter, const Context& context) {
  menu_ = menu && !menu->pages.empty() ? menu : nullptr;
  look_ = look;
  page_ = scroll_ = 0;
  row_ = -1;
  in_list_ = look == Look::kModern;
  if (menu_ && !in_list_) FirstRow(adapter, context);
}

void Navigator::Close() { menu_ = nullptr; }

std::vector<int> Navigator::VisibleRows(Adapter& adapter, const Context& context) const {
  std::vector<int> out;
  if (!menu_ || page_ >= static_cast<int>(menu_->pages.size())) return out;
  const Page& page = menu_->pages[page_];
  for (int i = 0; i < static_cast<int>(page.rows.size()); ++i)
    if (page.rows[i].when.empty() || adapter.Condition(page.rows[i].when, context)) out.push_back(i);
  return out;
}

bool Navigator::Focusable(int index, Adapter& adapter, const Context& context) const {
  RowView view;
  return ViewOf(adapter, menu_->pages[page_].rows[index], context, &view) && view.kind != Kind::kNote;
}

void Navigator::FirstRow(Adapter& adapter, const Context& context) {
  row_ = -1;
  scroll_ = 0;
  for (int index : VisibleRows(adapter, context)) {
    if (Focusable(index, adapter, context)) {
      row_ = index;
      return;
    }
  }
}

void Navigator::MoveRow(int direction, Adapter& adapter, const Context& context) {
  const std::vector<int> rows = VisibleRows(adapter, context);
  if (rows.empty()) return;
  const int count = static_cast<int>(rows.size());
  int at = static_cast<int>(std::find(rows.begin(), rows.end(), row_) - rows.begin());
  if (at >= count) at = direction > 0 ? -1 : 0;
  for (int tries = 0; tries < count; ++tries) {
    at = (at + direction + count) % count;  // wraps, as the games' own menus do
    if (Focusable(rows[at], adapter, context)) {
      row_ = rows[at];
      return;
    }
  }
}

void Navigator::MovePage(int direction) {
  const int count = static_cast<int>(menu_->pages.size());
  page_ = (page_ + direction + count) % count;
  row_ = -1;
  scroll_ = 0;
}

void Navigator::KeepInView(int focus_at, int rows_shown, int count) {
  if (rows_shown <= 0) return;
  if (focus_at < count) {
    if (focus_at < scroll_) scroll_ = focus_at;
    if (focus_at >= scroll_ + rows_shown) scroll_ = focus_at - rows_shown + 1;
  }
  scroll_ = std::clamp(scroll_, 0, std::max(0, count - rows_shown));
}

Outcome Navigator::Press(Input input, Adapter& adapter, const Context& context) {
  if (!menu_ || menu_->pages.empty()) return Outcome::kIgnored;
  if (input == Input::kPagePrev || input == Input::kPageNext) {
    MovePage(input == Input::kPageNext ? 1 : -1);
    FirstRow(adapter, context);
    adapter.Play(Sound::kMove, context);
    return Outcome::kHandled;
  }
  if (in_list_) {
    switch (input) {
      case Input::kUp:
      case Input::kDown:
        MovePage(input == Input::kDown ? 1 : -1);
        adapter.Play(Sound::kMove, context);
        return Outcome::kHandled;
      case Input::kAccept:
      case Input::kRight:
        FirstRow(adapter, context);
        if (row_ < 0) {
          adapter.Play(Sound::kRefuse, context);
          return Outcome::kHandled;
        }
        in_list_ = false;
        adapter.Play(Sound::kAccept, context);
        return Outcome::kHandled;
      case Input::kBack:
        return Outcome::kLeave;
      default:
        return Outcome::kIgnored;
    }
  }
  if (input == Input::kBack) {
    if (look_ == Look::kOriginal) return Outcome::kLeave;
    in_list_ = true;
    adapter.Play(Sound::kBack, context);
    return Outcome::kHandled;
  }
  if (input == Input::kUp || input == Input::kDown) {
    MoveRow(input == Input::kDown ? 1 : -1, adapter, context);
    adapter.Play(Sound::kMove, context);
    return Outcome::kHandled;
  }
  if (row_ < 0) return Outcome::kIgnored;
  const Row& row = menu_->pages[page_].rows[row_];
  RowProvider* provider = nullptr;
  std::string_view name;
  RowView view;
  if (!FindRow(adapter, row.id, &provider, &name) || !provider->View(name, context, &view)) return Outcome::kIgnored;
  bool changed = false;
  Sound sound = Sound::kStep;
  switch (input) {
    case Input::kLeft:
    case Input::kRight:
      changed = !view.locked && view.kind != Kind::kAction &&
                provider->Step(name, context, input == Input::kRight ? 1 : -1);
      break;
    case Input::kAccept:
      sound = Sound::kAccept;
      changed = !view.locked && (view.kind == Kind::kAction ? provider->Activate(name, context)
                                                            : provider->Step(name, context, 1));
      break;
    case Input::kDefault:
      changed = !view.locked && provider->Reset(name, context);
      break;
    default:
      return Outcome::kIgnored;
  }
  adapter.Play(changed ? sound : Sound::kRefuse, context);
  return Outcome::kHandled;
}

void Navigator::Hover(int index, bool category, Adapter& adapter, const Context& context) {
  if (!menu_ || index < 0) return;
  if (category) {
    if (look_ != Look::kModern || index >= static_cast<int>(menu_->pages.size()) || !in_list_ || index == page_)
      return;
    page_ = index;
    row_ = -1;
    scroll_ = 0;
  } else {
    if (index >= static_cast<int>(menu_->pages[page_].rows.size()) || (index == row_ && !in_list_)) return;
    if (!Focusable(index, adapter, context)) return;
    row_ = index;
    in_list_ = false;
  }
  adapter.Play(Sound::kMove, context);
}

Outcome Navigator::Click(int index, bool category, int side, float fraction, Adapter& adapter,
                         const Context& context) {
  if (!menu_ || index < 0) return Outcome::kIgnored;
  if (category) {
    if (index >= static_cast<int>(menu_->pages.size())) return Outcome::kIgnored;
    page_ = index;
    in_list_ = true;
    return Press(Input::kAccept, adapter, context);
  }
  if (index >= static_cast<int>(menu_->pages[page_].rows.size()) || !Focusable(index, adapter, context))
    return Outcome::kIgnored;
  row_ = index;
  in_list_ = false;
  RowProvider* provider = nullptr;
  std::string_view name;
  RowView view;
  if (!FindRow(adapter, menu_->pages[page_].rows[index].id, &provider, &name) ||
      !provider->View(name, context, &view))
    return Outcome::kIgnored;
  if (view.kind == Kind::kSlider && fraction >= 0.0f && !view.locked) {
    const Number& n = view.number;
    double value = n.min + std::clamp(static_cast<double>(fraction), 0.0, 1.0) * (n.max - n.min);
    if (n.step > 0) value = n.min + std::round((value - n.min) / n.step) * n.step;
    const bool changed = provider->SetNumber(name, context, std::clamp(value, n.min, n.max));
    adapter.Play(changed ? Sound::kStep : Sound::kRefuse, context);
    return Outcome::kHandled;
  }
  if (view.kind == Kind::kAction) return Press(Input::kAccept, adapter, context);
  if (side == 0) {
    adapter.Play(Sound::kMove, context);  // a click on the label only focuses the row
    return Outcome::kHandled;
  }
  return Press(side < 0 ? Input::kLeft : Input::kRight, adapter, context);
}

// ---------------------------------------------------------------------------------------------------------------
// Layout

std::vector<DrawItem> LayOut(Navigator& navigator, Adapter& adapter, const Context& context, const Box& area) {
  std::vector<DrawItem> items;
  const Menu* menu = navigator.menu();
  if (!menu || menu->pages.empty()) return items;
  const bool modern = navigator.look() == Look::kModern;
  const Metrics& m = modern ? kModern : kOriginal;
  const float sx = (area.x1 - area.x0) / 640.0f, sy = (area.y1 - area.y0) / 480.0f;
  auto add = [&](What what, float x0, float y0, float x1, float y1, std::string text, bool focused, int index,
                 bool category) -> DrawItem& {
    DrawItem item;
    item.what = what;
    item.box = {area.x0 + x0 * sx, area.y0 + y0 * sy, area.x0 + x1 * sx, area.y0 + y1 * sy};
    item.text = std::move(text);
    item.focused = focused;
    item.index = index;
    item.category = category;
    items.push_back(std::move(item));
    return items.back();
  };
  const Page& page = menu->pages[navigator.page()];
  const std::string title = adapter.Text(modern ? menu->title_key : page.title_key);
  add(What::kTitle, m.title_x, m.title_y, 592, m.title_y + m.title_h, title, false, -1, false);
  add(What::kRule, m.title_x, m.title_y + m.title_h + 6, 592, m.title_y + m.title_h + 7, "", false, -1, false);

  if (modern) {
    for (int i = 0; i < static_cast<int>(menu->pages.size()); ++i) {
      const float y = m.rows_y + static_cast<float>(i) * m.pitch;
      const bool chosen = i == navigator.page();
      if (chosen && navigator.in_list())
        add(What::kHighlight, m.list_x0 - 8, y, m.list_x1, y + m.row_h, "", true, i, true);
      add(What::kCategory, m.list_x0, y, m.list_x1, y + m.row_h, adapter.Text(menu->pages[i].label_key), chosen, i,
          true);
    }
  } else if (menu->pages.size() > 1) {
    const float y = m.rows_y - m.pitch - 4;
    add(What::kCategory, m.label_x0, y, m.value_x1, y + m.row_h, adapter.Text(page.label_key), true, navigator.page(),
        true);
  }

  const std::vector<int> rows = navigator.VisibleRows(adapter, context);
  // The window counts visible rows: find the focused row's place among them.
  const int count = static_cast<int>(rows.size());
  const int focus_at = static_cast<int>(std::find(rows.begin(), rows.end(), navigator.row()) - rows.begin());
  navigator.KeepInView(focus_at, m.rows_shown, count);
  const int first = navigator.scroll();
  std::string note;
  for (int k = 0; k < m.rows_shown && first + k < count; ++k) {
    const int index = rows[first + k];
    RowView view;
    if (!ViewOf(adapter, page.rows[index], context, &view)) continue;
    const float y = m.rows_y + static_cast<float>(k) * m.pitch;
    const bool focused = index == navigator.row() && !navigator.in_list();
    if (focused) {
      add(What::kHighlight, m.label_x0 - 8, y, m.value_x1 + 8, y + m.row_h, "", true, index, false);
      note = view.note;
    }
    add(What::kLabel, m.label_x0, y, m.value_x0 - 8, y + m.row_h, view.label, focused, index, false).locked =
        view.locked;
    if (view.kind == Kind::kChoice || view.kind == Kind::kSlider) {
      if (!modern) add(What::kArrows, m.value_x0, y, m.value_x1, y + m.row_h, "", focused, index, false);
      if (view.kind == Kind::kSlider) {
        const float g0 = m.value_x0 + (modern ? 0 : 15), g1 = modern ? m.value_x1 - 52 : m.value_x0 + 71;
        DrawItem& gauge = add(What::kGauge, g0, y + 9, g1, y + m.row_h - 9, "", focused, index, false);
        const double span = view.number.max - view.number.min;
        gauge.fill = span > 0 ? static_cast<float>(std::clamp((view.number_value - view.number.min) / span, 0.0, 1.0))
                              : 0.0f;
        add(What::kValue, g1 + 6, y, m.value_x1, y + m.row_h, view.value, focused, index, false);
      } else {
        add(What::kValue, m.value_x0, y, m.value_x1, y + m.row_h, view.value, focused, index, false).locked =
            view.locked;
      }
    }
  }
  if (first > 0) add(What::kScroll, m.label_x0, m.rows_y - 10, m.value_x1, m.rows_y - 2, "", false, -1, false);
  if (first + m.rows_shown < count) {
    const float y = m.rows_y + static_cast<float>(m.rows_shown) * m.pitch;
    add(What::kScroll, m.label_x0, y, m.value_x1, y + 8, "", false, -1, false).fill = 1.0f;
  }
  add(What::kRule, m.label_x0, m.rule_y, 592, m.rule_y + 1, "", false, -1, false);
  if (!note.empty()) add(What::kNote, m.label_x0, m.note_y, 592, m.note_y + 40, note, false, -1, false);
  return items;
}

const DrawItem* HitTest(const std::vector<DrawItem>& items, float x, float y) {
  for (const DrawItem& item : items) {
    if (item.what != What::kLabel && item.what != What::kValue && item.what != What::kGauge &&
        item.what != What::kCategory && item.what != What::kArrows)
      continue;
    if (x >= item.box.x0 && x < item.box.x1 && y >= item.box.y0 && y < item.box.y1) return &item;
  }
  return nullptr;
}

}  // namespace menukit
