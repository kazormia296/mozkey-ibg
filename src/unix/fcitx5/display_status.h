// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#ifndef MOZC_UNIX_FCITX5_DISPLAY_STATUS_H_
#define MOZC_UNIX_FCITX5_DISPLAY_STATUS_H_

#include <string>
#include <string_view>

#include "protocol/candidate_window.pb.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"

namespace fcitx {

// Fcitx-free helpers that turn the display related flags of a Mozc Output
// into the text shown in the Fcitx aux line.  Windows and macOS draw the same
// information with a renderer overlay and coloured preedit; Fcitx can only
// show text.

// What produced the visible preedit.  This mirrors
// mozc::renderer::ConversionDisplayStatus and its resolver in
// renderer/conversion_status_indicator.h, a target that is not visible to
// //unix.  Keep the two in sync.
enum class ConversionDisplayStatus {
  kHidden,
  kReading,
  kMozc,
  kZenzPending,
  kZenz,
};

ConversionDisplayStatus ResolveConversionDisplayStatus(
    const mozc::commands::Output& output);

// A suggestion list nobody navigates yet.  Clicking one of its entries commits
// it (SUBMIT_CANDIDATE); the conversion status stays visible next to it.
bool IsPassiveSuggestionWindow(
    const mozc::commands::CandidateWindow& candidate_window);

// Label for the conversion status: "Mozc", "Zenz…" (correction running) or
// "Zenz" (correction applied), or their one-letter forms.  Empty while the
// user is only composing, while a candidate list is being navigated, and for
// the colour-only style, which text cannot express.
std::string ConversionStatusLabel(
    const mozc::commands::Output& output,
    mozc::config::Config::ConversionStatusIndicatorLabel label_style);

// The status label is an annotation of something the input panel displays
// anyway: the reading, the candidate footer, or a preedit drawn in the panel
// because the application cannot show one.  It never opens a popup by itself,
// so turning the reading off still means "no popup while typing".
bool StatusLabelHasHost(bool has_reading, bool has_footer,
                        bool panel_shows_preedit);

// The typed reading to show next to the preedit: during live conversion, and
// before it starts when the server asks for it
// (Output.pre_live_conversion_reading).  Empty otherwise.
std::string BuildDisplayReading(const mozc::commands::Output& output);

// Composes the aux line as "<reading> <status> [<footer>]", leaving out the
// empty parts.
std::string BuildAuxText(std::string_view reading,
                         std::string_view status_label,
                         std::string_view footer);

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_DISPLAY_STATUS_H_
