// cod3 - a slider with a number box after it (owner 2026-10-20: "sliders +
// textbox"): drag the slider, or type an exact value into the box. Both write
// the same value; a typed value is kept within the slider's range.
//
// Laid out like an ImGui slider: the slider and the box together take the
// item width (or `width` when given), and the label (the part before "##")
// follows on the right. An empty label ("##id") draws none.

#pragma once

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace cod3::ui {

namespace slider_box_detail {

// Width of the number box: room for "-000.00".
inline float BoxWidth() { return ImGui::CalcTextSize("-000.00").x + ImGui::GetStyle().FramePadding.x * 2.0f; }

// The visible part of a label ("Right##x" -> "Right"); nullptr when none.
inline const char* VisibleEnd(const char* label) {
  const char* hash = std::strstr(label, "##");
  return hash ? hash : label + std::strlen(label);
}

template <typename Draw>
bool Row(const char* label, float width, Draw&& draw) {
  ImGuiStyle& st = ImGui::GetStyle();
  const float total = width > 0.0f ? width : ImGui::CalcItemWidth();
  const float box = BoxWidth();
  const float slider = std::max(1.0f, total - box - st.ItemInnerSpacing.x);
  ImGui::PushID(label);
  const bool changed = draw(slider, box, st.ItemInnerSpacing.x);
  const char* end = VisibleEnd(label);
  if (end != label) {
    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label, end);
  }
  ImGui::PopID();
  return changed;
}

}  // namespace slider_box_detail

inline bool SliderIntBox(const char* label, int* v, int lo, int hi, const char* format = "%d", float width = 0.0f) {
  return slider_box_detail::Row(label, width, [&](float slider, float box, float gap) {
    bool changed = false;
    ImGui::SetNextItemWidth(slider);
    changed |= ImGui::SliderInt("##slider", v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(box);
    int typed = *v;
    if (ImGui::InputInt("##box", &typed, 0, 0)) {
      *v = std::clamp(typed, lo, hi);
      changed = true;
    }
    return changed;
  });
}

inline bool SliderFloatBox(const char* label, float* v, float lo, float hi, const char* format = "%.1f",
                           float width = 0.0f) {
  return slider_box_detail::Row(label, width, [&](float slider, float box, float gap) {
    bool changed = false;
    ImGui::SetNextItemWidth(slider);
    changed |= ImGui::SliderFloat("##slider", v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(box);
    float typed = *v;
    if (ImGui::InputFloat("##box", &typed, 0.0f, 0.0f, format)) {
      *v = std::clamp(typed, lo, hi);
      changed = true;
    }
    return changed;
  });
}

}  // namespace cod3::ui
