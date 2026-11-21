// Copyright 2026 The Mozkey Authors

#ifndef MOZC_PROTOCOL_RENDERER_CALLBACK_PROVENANCE_H_
#define MOZC_PROTOCOL_RENDERER_CALLBACK_PROVENANCE_H_

#include <cstdint>

#include "protocol/commands.pb.h"

namespace mozc::commands {

// The renderer callback channel is deliberately smaller than SessionCommand.
// Keeping its validation in a platform-neutral helper makes the Windows
// message transport and TSF consumer share a fail-closed policy.
enum class RendererCallbackKind {
  kUnsupported,
  kSelect,
  kHighlight,
  // SUBMIT_CANDIDATE: commits a candidate that was never focused (mouse commit
  // of a passive suggestion). Validated like kSelect.
  kSubmit,
};

enum class RendererCallbackCandidateDisposition {
  kReject,
  kDispatch,
  kAlreadyFocused,
};

struct RendererCallbackProvenance {
  uint64_t token = 0;
  uint64_t focus_epoch = 0;
  int32_t focus_revision = 0;
  uint64_t output_generation = 0;
};

inline constexpr int kMaxRendererCallbackCandidateDepth = 8;

RendererCallbackKind GetRendererCallbackKind(
    SessionCommand::CommandType type);

// Maps a native message identifier to its callback operation. Zero or
// colliding registered-message identifiers are rejected.
RendererCallbackKind GetRendererCallbackKindForMessage(
    uint32_t message, uint32_t select_message, uint32_t highlight_message,
    uint32_t submit_message);

// Checks both capability freshness and whether the token can be transported
// losslessly by the target ABI (for example, in a Windows WPARAM).
bool IsRendererCallbackTokenTransportable(uint64_t received_token,
                                          uint64_t expected_token,
                                          uint64_t transport_max);

// Checks the complete producer domain before a native callback can be
// dispatched. Secure input always rejects renderer callbacks.
bool IsRendererCallbackProvenanceCurrent(
    const RendererCallbackProvenance& expected, uint64_t received_token,
    uint64_t current_focus_epoch, int32_t current_focus_revision,
    bool secure_input, uint64_t current_output_generation);

bool HasRendererCallbackCandidate(const Output& output, int32_t candidate_id);

bool IsRendererCallbackCandidateFocused(const Output& output,
                                        int32_t candidate_id);

RendererCallbackCandidateDisposition GetRendererCallbackCandidateDisposition(
    SessionCommand::CommandType type, const Output& output,
    int32_t candidate_id);

}  // namespace mozc::commands

#endif  // MOZC_PROTOCOL_RENDERER_CALLBACK_PROVENANCE_H_
