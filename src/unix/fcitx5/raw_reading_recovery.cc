// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/raw_reading_recovery.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "protocol/commands.pb.h"
#include "unix/fcitx5/grimodex_context.h"
#include "unix/fcitx5/mozc_client_interface.h"

namespace fcitx {

RawReadingRecovery::RawReadingRecovery(
    SessionInitializer session_initializer, KeyFinalizer key_finalizer)
    : session_initializer_(std::move(session_initializer)),
      key_finalizer_(std::move(key_finalizer)) {}

RawReadingRecovery::PrepareResult RawReadingRecovery::Prepare(
    MozcClientInterface* client, const mozc::commands::Context& context,
    bool secure_input, mozc::commands::Output* recovered_output) {
  if (client == nullptr || recovered_output == nullptr ||
      !session_initializer_) {
    return PrepareResult::kFailed;
  }
  if (recovery_in_progress_) {
    return PrepareResult::kFailed;
  }
  if (secure_input) {
    // A fallible session/initializer call must never leave a prior non-secure
    // journal available to a later request.
    ClearReading();
  }
  recovered_output->Clear();

  uint64_t generation = 0;
  bool stable_session = false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!client->EnsureSession()) {
      return PrepareResult::kFailed;
    }
    generation = client->session_generation();
    if (generation == 0) {
      return PrepareResult::kFailed;
    }
    if (observed_session_generation_ == generation) {
      stable_session = true;
      break;
    }

    if (!session_initializer_(client, generation, context)) {
      if (client->session_generation() != generation) {
        continue;
      }
      return PrepareResult::kFailed;
    }
    if (client->session_generation() != generation) {
      continue;
    }
    stable_session = true;
    break;
  }
  if (!stable_session) {
    return PrepareResult::kFailed;
  }

  if (observed_session_generation_ == 0) {
    observed_session_generation_ = generation;
    if (secure_input) {
      ClearReading();
    }
    return PrepareResult::kReady;
  }
  if (observed_session_generation_ == generation) {
    if (secure_input) {
      ClearReading();
    }
    return PrepareResult::kReady;
  }

  const uint64_t previous_generation = observed_session_generation_;
  observed_session_generation_ = generation;
  last_snapshot_relation_ = SnapshotRelation::kUnknown;
  if (secure_input) {
    ClearReading();
    return PrepareResult::kSessionChanged;
  }
  if (journal_.empty() || journal_suppressed_) {
    return PrepareResult::kSessionChanged;
  }

  const std::string previous_digest = pinned_snapshot_digest_;
  recovery_in_progress_ = true;
  mozc::commands::Output latest_output;
  for (const mozc::commands::KeyEvent& key : journal_) {
    latest_output.Clear();
    if (!SendKeyWithGrimodexContext(client, key, context, &latest_output) ||
        journal_invalidation_pending_ ||
        client->session_generation() != generation ||
        HasHostVisibleSideEffect(latest_output) ||
        !latest_output.has_consumed() || !latest_output.consumed() ||
        !HasActiveComposition(latest_output)) {
      if (client->session_generation() == generation) {
        BestEffortAbortPartialRecovery(client, context);
      }
      recovery_in_progress_ = false;
      // Restore the old baseline so the replacement boundary remains pending.
      // The next Prepare must report kSessionChanged even if replay failed in
      // the same generation, otherwise a stale SessionCommand could cross it.
      observed_session_generation_ = previous_generation;
      journal_invalidation_pending_ = false;
      SuppressReading();
      return PrepareResult::kFailed;
    }
  }
  recovery_in_progress_ = false;

  const std::string recovered_digest = SnapshotDigest(latest_output);
  if (!previous_digest.empty() && !recovered_digest.empty()) {
    last_snapshot_relation_ =
        previous_digest == recovered_digest ? SnapshotRelation::kSame
                                            : SnapshotRelation::kChanged;
  }
  if (!recovered_digest.empty()) {
    pinned_snapshot_digest_ = recovered_digest;
  }
  *recovered_output = latest_output;
  return PrepareResult::kSessionChanged;
}

mozc::commands::KeyEvent RawReadingRecovery::FinalizeKey(
    const mozc::commands::KeyEvent& key, PrepareResult prepare_result,
    const mozc::commands::Output& recovered_output) const {
  mozc::commands::KeyEvent finalized = key;
  if (key_finalizer_) {
    // Prepare leaves `recovered_output` empty unless it replayed the journal.
    const bool replayed = prepare_result == PrepareResult::kSessionChanged &&
                          recovered_output.has_consumed();
    key_finalizer_(replayed ? &recovered_output : nullptr, &finalized);
  }
  return finalized;
}

bool RawReadingRecovery::DispatchKey(
    MozcClientInterface* client, const mozc::commands::KeyEvent& key,
    const mozc::commands::Context& context, bool secure_input,
    mozc::commands::Output* output,
    mozc::commands::KeyEvent* dispatched_key) {
  if (client == nullptr || output == nullptr) {
    return false;
  }
  const auto fail_unconsumed_key = [this, client, &context, secure_input]() {
    if (recovery_in_progress_) {
      // Avoid invalidating an outer replay's journal iterators.  The outer
      // loop observes this flag after its current SendKey returns and performs
      // the abort and suppression itself.
      journal_invalidation_pending_ = true;
      return false;
    }
    if (observed_session_generation_ != 0 &&
        client->session_generation() == observed_session_generation_) {
      BestEffortAbortPartialRecovery(client, context);
    }
    if (secure_input) {
      ClearReading();
    } else {
      // The current key will be forwarded to the application.  Replaying an
      // older journal on a later key would reorder application-visible input.
      SuppressReading();
    }
    return false;
  };
  mozc::commands::Output recovered_output;
  const PrepareResult prepare_result =
      Prepare(client, context, secure_input, &recovered_output);
  if (prepare_result == PrepareResult::kFailed) {
    return fail_unconsumed_key();
  }
  // Finalized only now: Prepare may have adopted a replacement session, whose
  // initializer and replay decide the mode this key is typed in.
  mozc::commands::KeyEvent wire_key =
      FinalizeKey(key, prepare_result, recovered_output);
  if (SendKeyWithGrimodexContext(client, wire_key, context, output)) {
    if (dispatched_key != nullptr) {
      *dispatched_key = std::move(wire_key);
    }
    return true;
  }
  if (secure_input) {
    return fail_unconsumed_key();
  }

  // Client deliberately does not retry managed protobuf requests.  Only a
  // newly observed generation authorizes this one raw-key retry.
  recovered_output.Clear();
  if (Prepare(client, context, /*secure_input=*/false, &recovered_output) !=
      PrepareResult::kSessionChanged) {
    return fail_unconsumed_key();
  }
  // The first copy was stamped for the session that is gone.
  wire_key = FinalizeKey(key, PrepareResult::kSessionChanged, recovered_output);
  if (!SendKeyWithGrimodexContext(client, wire_key, context, output)) {
    return fail_unconsumed_key();
  }
  if (dispatched_key != nullptr) {
    *dispatched_key = std::move(wire_key);
  }
  return true;
}

void RawReadingRecovery::RecordSuccessfulKey(
    const mozc::commands::KeyEvent& key,
    const mozc::commands::Output& output, bool secure_input) {
  if (secure_input) {
    ClearReading();
    return;
  }
  if (!output.has_consumed() || !output.consumed()) {
    return;
  }
  if (HasHostVisibleSideEffect(output) || !HasActiveComposition(output)) {
    ClearReading();
    return;
  }
  if (!IsRecoverableRawKey(key)) {
    // Keeping only the suffix after a conversion/navigation key would rebuild
    // a different reading at a different cursor position.  Drop the whole
    // journal and remain suppressed until the next composition boundary.
    SuppressReading();
    return;
  }
  if (journal_suppressed_) {
    return;
  }

  const size_t key_bytes = key.ByteSizeLong();
  if (journal_.size() >= kMaxEventCount ||
      key_bytes > kMaxSerializedBytes - journal_bytes_) {
    SuppressReading();
    return;
  }

  journal_.push_back(key);
  journal_bytes_ += key_bytes;
  const std::string digest = SnapshotDigest(output);
  if (!digest.empty()) {
    pinned_snapshot_digest_ = digest;
  }
}

void RawReadingRecovery::ClearReading() {
  journal_.clear();
  journal_bytes_ = 0;
  journal_suppressed_ = false;
  pinned_snapshot_digest_.clear();
  last_snapshot_relation_ = SnapshotRelation::kUnknown;
}

void RawReadingRecovery::MarkCompositionNotReplayable() {
  if (recovery_in_progress_) {
    // Same rule as DispatchKey: never invalidate the journal an outer replay
    // is iterating.  The replay loop observes the flag and suppresses itself.
    journal_invalidation_pending_ = true;
    return;
  }
  SuppressReading();
}

void RawReadingRecovery::ResetSessionBoundary() {
  ClearReading();
  observed_session_generation_ = 0;
  recovery_in_progress_ = false;
  journal_invalidation_pending_ = false;
}

bool RawReadingRecovery::HasActiveComposition(
    const mozc::commands::Output& output) {
  return output.has_preedit() && output.preedit().segment_size() > 0;
}

bool RawReadingRecovery::HasHostVisibleSideEffect(
    const mozc::commands::Output& output) {
  return output.has_result() || output.has_deletion_range() ||
         (output.has_url() && !output.url().empty()) ||
         (output.has_launch_tool_mode() &&
          output.launch_tool_mode() != mozc::commands::Output::NO_TOOL);
}

bool RawReadingRecovery::IsRecoverableRawKey(
    const mozc::commands::KeyEvent& key) {
  const auto is_printable_code_point = [](uint32_t code) {
    return (code >= 0x21 && code <= 0x7E) ||
           (code >= 0xA0 && code <= 0x10FFFF &&
            !(code >= 0xD800 && code <= 0xDFFF));
  };

  if (key.probable_key_event_size() != 0 || key.has_activated() ||
      (key.has_modifiers() && key.modifiers() != 0) ||
      (key.has_input_style() &&
       key.input_style() != mozc::commands::KeyEvent::FOLLOW_MODE)) {
    return false;
  }
  // The composition mode a key was typed in is part of the reading and is
  // replayed with it.  A key that would turn the IME off or carries no real
  // composition mode is an action, not reading.
  if (key.has_mode() &&
      (key.mode() == mozc::commands::DIRECT ||
       key.mode() >= mozc::commands::NUM_OF_COMPOSITIONS)) {
    return false;
  }

  for (const auto modifier : key.modifier_keys()) {
    switch (modifier) {
      case mozc::commands::KeyEvent::SHIFT:
      case mozc::commands::KeyEvent::LEFT_SHIFT:
      case mozc::commands::KeyEvent::RIGHT_SHIFT:
      case mozc::commands::KeyEvent::CAPS:
        break;
      default:
        // CTRL/ALT and key-up/down modifier events are actions, not reading.
        return false;
    }
  }

  if (key.has_special_key() &&
      key.special_key() != mozc::commands::KeyEvent::NO_SPECIALKEY) {
    return (key.special_key() == mozc::commands::KeyEvent::BACKSPACE ||
            key.special_key() == mozc::commands::KeyEvent::DEL) &&
           !key.has_key_code() &&
           (!key.has_key_string() || key.key_string().empty()) &&
           key.modifier_keys_size() == 0;
  }

  if (key.has_key_string() && !key.key_string().empty()) {
    for (const unsigned char byte : key.key_string()) {
      if (byte < 0x20 || byte == 0x7F) {
        return false;
      }
    }
    return !key.has_key_code() || is_printable_code_point(key.key_code());
  }
  if (!key.has_key_code()) {
    return false;
  }

  // Fcitx maps space and control/navigation keys to SpecialKey.  Accept only
  // printable Unicode scalar values here so an unknown action cannot enter
  // the recovery journal through key_code.
  return is_printable_code_point(key.key_code());
}

std::string RawReadingRecovery::SnapshotDigest(
    const mozc::commands::Output& output) {
  if (!output.has_grimodex_session_status() ||
      !output.grimodex_session_status().has_pinned_payload_sha256()) {
    return {};
  }
  return output.grimodex_session_status().pinned_payload_sha256();
}

void RawReadingRecovery::BestEffortAbortPartialRecovery(
    MozcClientInterface* client, const mozc::commands::Context& context) {
  // This is a new-session cleanup command, not replay of an old command.  A
  // failed raw replay may already have rebuilt a prefix; never leave that
  // invisible partial preedit to affect the next user key.
  mozc::commands::SessionCommand revert;
  revert.set_type(mozc::commands::SessionCommand::REVERT);
  mozc::commands::Output ignored;
  SendCommandWithGrimodexContext(client, revert, context, &ignored);
}

void RawReadingRecovery::SuppressReading() {
  journal_.clear();
  journal_bytes_ = 0;
  journal_suppressed_ = true;
  journal_invalidation_pending_ = false;
  pinned_snapshot_digest_.clear();
  last_snapshot_relation_ = SnapshotRelation::kUnknown;
}

}  // namespace fcitx
