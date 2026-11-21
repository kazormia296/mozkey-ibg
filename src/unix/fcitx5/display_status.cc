// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/display_status.h"

#include <string>
#include <string_view>

#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "renderer/live_conversion_reading.h"

namespace fcitx {

ConversionDisplayStatus ResolveConversionDisplayStatus(
    const mozc::commands::Output& output) {
  if (!output.has_preedit() || output.preedit().segment_size() == 0) {
    return ConversionDisplayStatus::kHidden;
  }
  if (output.zenz_live_correction_pending()) {
    return ConversionDisplayStatus::kZenzPending;
  }
  if (output.zenz_live_correction_applied()) {
    return ConversionDisplayStatus::kZenz;
  }
  if (output.live_conversion()) {
    return ConversionDisplayStatus::kMozc;
  }
  // Before live conversion starts, or when the user is composing normally,
  // there is no materialized Mozc/Zenz conversion to attribute.
  return ConversionDisplayStatus::kReading;
}

bool IsPassiveSuggestionWindow(
    const mozc::commands::CandidateWindow& candidate_window) {
  return candidate_window.category() == mozc::commands::SUGGESTION &&
         candidate_window.candidate_size() > 0 &&
         !candidate_window.has_focused_index();
}

std::string ConversionStatusLabel(
    const mozc::commands::Output& output,
    mozc::config::Config::ConversionStatusIndicatorLabel label_style) {
  if (label_style == mozc::config::Config::CONVERSION_STATUS_COLOR_ONLY) {
    return {};
  }
  // Unfocused suggestions are companions to ongoing composition.  Hide the
  // status only while a conversion, prediction or suggestion list is being
  // navigated.
  if (output.has_candidate_window() &&
      !IsPassiveSuggestionWindow(output.candidate_window())) {
    return {};
  }

  const bool symbol =
      label_style == mozc::config::Config::CONVERSION_STATUS_SYMBOL;
  switch (ResolveConversionDisplayStatus(output)) {
    case ConversionDisplayStatus::kMozc:
      return symbol ? "M" : "Mozc";
    case ConversionDisplayStatus::kZenzPending:
      // U+2026 HORIZONTAL ELLIPSIS.
      return symbol ? "\xE2\x80\xA6" : "Zenz\xE2\x80\xA6";
    case ConversionDisplayStatus::kZenz:
      return symbol ? "Z" : "Zenz";
    case ConversionDisplayStatus::kReading:
    case ConversionDisplayStatus::kHidden:
      // No popup just to say that the user is typing.
      return {};
  }
  return {};
}

bool StatusLabelHasHost(bool has_reading, bool has_footer,
                        bool panel_shows_preedit) {
  return has_reading || has_footer || panel_shows_preedit;
}

std::string BuildDisplayReading(const mozc::commands::Output& output) {
  if (output.live_conversion()) {
    return mozc::renderer::BuildLiveConversionReading(output);
  }
  if (!output.pre_live_conversion_reading() || !output.has_preedit()) {
    return {};
  }
  // The shared helper answers only for live-conversion Outputs.  Ask it about
  // the same preedit so both phases use one definition of "reading".
  mozc::commands::Output probe;
  probe.set_live_conversion(true);
  *probe.mutable_preedit() = output.preedit();
  return mozc::renderer::BuildLiveConversionReading(probe);
}

std::string BuildAuxText(std::string_view reading,
                         std::string_view status_label,
                         std::string_view footer) {
  std::string aux(reading);
  if (!status_label.empty()) {
    if (!aux.empty()) {
      aux += " ";
    }
    aux += status_label;
  }
  if (!footer.empty()) {
    if (!aux.empty()) {
      aux += " ";
    }
    aux += "[";
    aux += footer;
    aux += "]";
  }
  return aux;
}

}  // namespace fcitx
