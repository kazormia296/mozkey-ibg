// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#ifndef MOZC_UNIX_FCITX5_RAW_READING_RECOVERY_H_
#define MOZC_UNIX_FCITX5_RAW_READING_RECOVERY_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "protocol/commands.pb.h"
#include "unix/fcitx5/mozc_client_interface.h"

namespace fcitx {

// This journal deliberately stores only raw KeyEvent messages.  SessionCommand
// messages, candidate IDs, callbacks, and effects are scoped to one Mozc
// session and must never be replayed into a replacement session.
class RawReadingRecovery final {
 public:
  static constexpr size_t kMaxEventCount = 256;
  static constexpr size_t kMaxSerializedBytes = 64 * 1024;

  enum class PrepareResult {
    kReady,
    kSessionChanged,
    kFailed,
  };

  enum class SnapshotRelation {
    kUnknown,
    kSame,
    kChanged,
  };

  using SessionInitializer = std::function<bool(
      MozcClientInterface*, uint64_t, const mozc::commands::Context&)>;

  // Stamps the parts of a user key that depend on the session it is sent to
  // (KeyEvent.mode).  DispatchKey calls it on a copy of the key immediately
  // before every send, that is after Prepare ran the session initializer and
  // any replay, so the stamp describes the session that really receives the
  // key.  `replayed_output` is the last Output of the replay this dispatch
  // performed into a replacement session, or nullptr when nothing was
  // replayed.
  using KeyFinalizer = std::function<void(
      const mozc::commands::Output* replayed_output,
      mozc::commands::KeyEvent* key)>;

  // `session_initializer` must initialize exactly the supplied generation.
  // Prepare invokes it before adopting a new generation or replaying keys.
  // `key_finalizer` may be empty: keys are then sent as they are given.
  explicit RawReadingRecovery(SessionInitializer session_initializer,
                              KeyFinalizer key_finalizer = nullptr);

  // Ensures that a session exists and, when its generation changed, rebuilds
  // the non-secure reading from raw keys.  A kSessionChanged result tells the
  // caller to discard any pending SessionCommand.  `recovered_output` is the
  // last pure-composition response produced by replay, if one was available.
  PrepareResult Prepare(MozcClientInterface* client,
                        const mozc::commands::Context& context,
                        bool secure_input,
                        mozc::commands::Output* recovered_output);

  // Dispatches one current raw key.  If the first call discovers a destroyed
  // session, the old reading is reconstructed and this key is retried at most
  // once.  Secure input is never retried.
  // `key` is the key without session-dependent fields; the key finalizer adds
  // them for each send.  On success `dispatched_key`, if given, receives the
  // key exactly as the server got it.  That is the form to pass to
  // RecordSuccessfulKey, so that a replay repeats what was really sent.
  bool DispatchKey(MozcClientInterface* client,
                   const mozc::commands::KeyEvent& key,
                   const mozc::commands::Context& context,
                   bool secure_input,
                   mozc::commands::Output* output,
                   mozc::commands::KeyEvent* dispatched_key = nullptr);

  // Records a successfully dispatched user key only while it leaves an active
  // composition.  Any commit or other host-visible side effect terminates the
  // journal.  `key` is journalled and replayed verbatim, including its
  // KeyEvent.mode: the server switches the composer to that mode and decides
  // direct commit / auto conversion by it, so a replay without it would
  // rebuild a different composition.
  void RecordSuccessfulKey(const mozc::commands::KeyEvent& key,
                           const mozc::commands::Output& output,
                           bool secure_input);

  // A local composition boundary (commit/reset) preserves knowledge of the
  // current server session but drops all reading data.
  void ClearReading();

  // The current composition was created by something other than raw keys
  // (reverse conversion of application text), so replaying journalled keys
  // could not rebuild it.  Drops the journal and keeps it suppressed until the
  // next composition boundary; later edit keys must not be journalled alone.
  void MarkCompositionNotReplayable();

  // A focus/domain/client ownership boundary drops both reading data and the
  // observed generation.  The next session is adopted as a fresh baseline.
  void ResetSessionBoundary();

  size_t journal_size_for_test() const { return journal_.size(); }
  size_t journal_bytes_for_test() const { return journal_bytes_; }
  bool journal_suppressed_for_test() const { return journal_suppressed_; }
  SnapshotRelation last_snapshot_relation_for_test() const {
    return last_snapshot_relation_;
  }

 private:
  static bool HasActiveComposition(const mozc::commands::Output& output);
  static bool HasHostVisibleSideEffect(const mozc::commands::Output& output);
  static bool IsRecoverableRawKey(const mozc::commands::KeyEvent& key);
  static std::string SnapshotDigest(const mozc::commands::Output& output);
  void BestEffortAbortPartialRecovery(
      MozcClientInterface* client, const mozc::commands::Context& context);
  void SuppressReading();
  // Returns the copy of `key` to send after `prepare_result`.
  mozc::commands::KeyEvent FinalizeKey(
      const mozc::commands::KeyEvent& key, PrepareResult prepare_result,
      const mozc::commands::Output& recovered_output) const;

  std::vector<mozc::commands::KeyEvent> journal_;
  size_t journal_bytes_ = 0;
  uint64_t observed_session_generation_ = 0;
  bool journal_suppressed_ = false;
  bool recovery_in_progress_ = false;
  bool journal_invalidation_pending_ = false;
  std::string pinned_snapshot_digest_;
  SnapshotRelation last_snapshot_relation_ = SnapshotRelation::kUnknown;
  SessionInitializer session_initializer_;
  KeyFinalizer key_finalizer_;
};

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_RAW_READING_RECOVERY_H_
