// Copyright 2026 The Mozkey Authors

#include "protocol/renderer_callback_provenance.h"

#include <cstdint>

namespace mozc::commands {
namespace {

bool HasCandidateInWindow(const CandidateWindow& candidate_window,
                          int32_t candidate_id, int depth) {
  if (depth > kMaxRendererCallbackCandidateDepth) {
    return false;
  }
  for (const CandidateWindow::Candidate& candidate :
       candidate_window.candidate()) {
    if (candidate.id() == candidate_id) {
      return true;
    }
  }
  return candidate_window.has_sub_candidate_window() &&
         HasCandidateInWindow(candidate_window.sub_candidate_window(),
                              candidate_id, depth + 1);
}

bool IsCandidateFocusedInWindow(const CandidateWindow& candidate_window,
                                int32_t candidate_id, int depth) {
  if (depth > kMaxRendererCallbackCandidateDepth) {
    return false;
  }
  if (candidate_window.has_focused_index()) {
    const uint32_t focused_index = candidate_window.focused_index();
    for (const CandidateWindow::Candidate& candidate :
         candidate_window.candidate()) {
      if (candidate.index() == focused_index &&
          candidate.id() == candidate_id) {
        return true;
      }
    }
  }
  return candidate_window.has_sub_candidate_window() &&
         IsCandidateFocusedInWindow(candidate_window.sub_candidate_window(),
                                    candidate_id, depth + 1);
}

}  // namespace

RendererCallbackKind GetRendererCallbackKind(
    SessionCommand::CommandType type) {
  switch (type) {
    case SessionCommand::SELECT_CANDIDATE:
      return RendererCallbackKind::kSelect;
    case SessionCommand::HIGHLIGHT_CANDIDATE:
      return RendererCallbackKind::kHighlight;
    case SessionCommand::SUBMIT_CANDIDATE:
      return RendererCallbackKind::kSubmit;
    default:
      return RendererCallbackKind::kUnsupported;
  }
}

RendererCallbackKind GetRendererCallbackKindForMessage(
    uint32_t message, uint32_t select_message, uint32_t highlight_message,
    uint32_t submit_message) {
  if (message == 0 || select_message == 0 || highlight_message == 0 ||
      submit_message == 0 || select_message == highlight_message ||
      select_message == submit_message ||
      highlight_message == submit_message) {
    return RendererCallbackKind::kUnsupported;
  }
  if (message == select_message) {
    return RendererCallbackKind::kSelect;
  }
  if (message == highlight_message) {
    return RendererCallbackKind::kHighlight;
  }
  if (message == submit_message) {
    return RendererCallbackKind::kSubmit;
  }
  return RendererCallbackKind::kUnsupported;
}

bool IsRendererCallbackTokenTransportable(uint64_t received_token,
                                          uint64_t expected_token,
                                          uint64_t transport_max) {
  return received_token != 0 && received_token == expected_token &&
         received_token <= transport_max;
}

bool IsRendererCallbackProvenanceCurrent(
    const RendererCallbackProvenance& expected, uint64_t received_token,
    uint64_t current_focus_epoch, int32_t current_focus_revision,
    bool secure_input, uint64_t current_output_generation) {
  return !secure_input && received_token != 0 &&
         received_token == expected.token &&
         expected.focus_epoch != 0 &&
         expected.focus_epoch == current_focus_epoch &&
         expected.focus_revision == current_focus_revision &&
         expected.output_generation != 0 &&
         expected.output_generation == current_output_generation;
}

bool HasRendererCallbackCandidate(const Output& output, int32_t candidate_id) {
  return output.has_candidate_window() &&
         HasCandidateInWindow(output.candidate_window(), candidate_id,
                              /*depth=*/0);
}

bool IsRendererCallbackCandidateFocused(const Output& output,
                                        int32_t candidate_id) {
  return output.has_candidate_window() &&
         IsCandidateFocusedInWindow(output.candidate_window(), candidate_id,
                                    /*depth=*/0);
}

RendererCallbackCandidateDisposition GetRendererCallbackCandidateDisposition(
    SessionCommand::CommandType type, const Output& output,
    int32_t candidate_id) {
  const RendererCallbackKind kind = GetRendererCallbackKind(type);
  if (kind == RendererCallbackKind::kUnsupported ||
      !HasRendererCallbackCandidate(output, candidate_id)) {
    return RendererCallbackCandidateDisposition::kReject;
  }
  if (kind == RendererCallbackKind::kHighlight &&
      IsRendererCallbackCandidateFocused(output, candidate_id)) {
    return RendererCallbackCandidateDisposition::kAlreadyFocused;
  }
  return RendererCallbackCandidateDisposition::kDispatch;
}

}  // namespace mozc::commands
