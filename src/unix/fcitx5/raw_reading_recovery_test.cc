// Copyright 2026 The Mozkey Authors
// Licensed under the Apache License, Version 2.0.

#include "unix/fcitx5/raw_reading_recovery.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "testing/gunit.h"
#include "unix/fcitx5/mozc_client_interface.h"

namespace fcitx {
namespace {

mozc::commands::KeyEvent Key(char code) {
  mozc::commands::KeyEvent key;
  key.set_key_code(code);
  return key;
}

mozc::commands::Output ActiveOutput(std::string_view digest = {}) {
  mozc::commands::Output output;
  output.set_consumed(true);
  output.mutable_preedit()->set_cursor(1);
  auto* segment = output.mutable_preedit()->add_segment();
  segment->set_annotation(mozc::commands::Preedit::Segment::UNDERLINE);
  segment->set_value("a");
  segment->set_value_length(1);
  if (!digest.empty()) {
    output.mutable_grimodex_session_status()->set_pinned_payload_sha256(
        digest);
  }
  return output;
}

mozc::commands::Context ValidContext(bool secure_input = false) {
  mozc::commands::Context context;
  context.set_revision(1);
  context.mutable_grimodex()->set_program("raw-recovery-test");
  context.mutable_grimodex()->set_frontend("test");
  context.mutable_grimodex()->set_focus_epoch(1);
  context.mutable_grimodex()->set_secure_input(secure_input);
  if (secure_input) {
    context.set_input_field_type(mozc::commands::Context::PASSWORD);
  }
  return context;
}

RawReadingRecovery MakeRecovery() {
  return RawReadingRecovery([](MozcClientInterface*, uint64_t,
                               const mozc::commands::Context&) {
    return true;
  });
}

RawReadingRecovery MakeModeInitializingRecovery() {
  return RawReadingRecovery(
      [](MozcClientInterface* client, uint64_t generation,
         const mozc::commands::Context& context) {
        if (client == nullptr || generation == 0 ||
            client->session_generation() != generation) {
          return false;
        }
        mozc::commands::SessionCommand command;
        command.set_type(
            mozc::commands::SessionCommand::SWITCH_COMPOSITION_MODE);
        command.set_composition_mode(mozc::commands::HIRAGANA);
        mozc::commands::Output output;
        return client->SendCommandWithContext(command, context, &output) &&
               client->session_generation() == generation &&
               output.has_mode();
      });
}

struct ClientEvent {
  enum class Kind {
    kKey,
    kCommand,
  };

  Kind kind;
  uint64_t generation;
  int value;
  bool succeeded;
};

class RecoveryTestClient final : public MozcClientInterface {
 public:
  bool EnsureConnection() override { return ensure_succeeds; }
  bool EnsureSession() override { return ensure_succeeds; }
  uint64_t session_generation() const override { return generation; }

  bool SendKeyWithContext(const mozc::commands::KeyEvent& key,
                          const mozc::commands::Context&,
                          mozc::commands::Output* output) override {
    sent_keys.push_back(key);
    events.push_back(ClientEvent{
        .kind = ClientEvent::Kind::kKey,
        .generation = generation,
        .value = key.has_key_code() ? static_cast<int>(key.key_code()) : -1,
        .succeeded = false,
    });
    if (on_next_key) {
      std::function<void()> callback = std::move(on_next_key);
      callback();
    }
    if (recreate_on_next_key) {
      recreate_on_next_key = false;
      ++generation;
      return false;
    }
    if (fail_next_key) {
      fail_next_key = false;
      return false;
    }
    if (recreate_on_key_call != 0 &&
        sent_keys.size() == recreate_on_key_call) {
      ++generation;
      return false;
    }
    if (fail_on_key_call != 0 && sent_keys.size() == fail_on_key_call) {
      return false;
    }
    if (!queued_outputs.empty()) {
      *output = queued_outputs.front();
      queued_outputs.pop_front();
    } else {
      *output = ActiveOutput(default_digest);
    }
    events.back().succeeded = true;
    return true;
  }

  bool SendCommandWithContext(const mozc::commands::SessionCommand& command,
                              const mozc::commands::Context&,
                              mozc::commands::Output* output) override {
    ++sent_commands;
    sent_command_types.push_back(command.type());
    events.push_back(ClientEvent{
        .kind = ClientEvent::Kind::kCommand,
        .generation = generation,
        .value = static_cast<int>(command.type()),
        .succeeded = false,
    });
    if (recreate_on_next_command) {
      recreate_on_next_command = false;
      ++generation;
      return false;
    }
    if (recreate_commands_remaining > 0) {
      --recreate_commands_remaining;
      ++generation;
      return false;
    }
    if (fail_next_command) {
      fail_next_command = false;
      return false;
    }
    if (output != nullptr) {
      output->set_mode(mozc::commands::HIRAGANA);
    }
    events.back().succeeded = true;
    return true;
  }
  bool IsDirectModeCommand(const mozc::commands::KeyEvent&) const override {
    return false;
  }
  bool GetConfig(mozc::config::Config*) override { return true; }
  void set_client_capability(const mozc::commands::Capability&) override {}
  bool SyncData() override { return true; }
  bool LaunchTool(const std::string&, std::string_view) override { return true; }
  bool LaunchToolWithProtoBuf(const mozc::commands::Output&) override {
    return true;
  }

  bool ensure_succeeds = true;
  bool fail_next_key = false;
  bool fail_next_command = false;
  size_t fail_on_key_call = 0;
  size_t recreate_on_key_call = 0;
  bool recreate_on_next_key = false;
  bool recreate_on_next_command = false;
  int recreate_commands_remaining = 0;
  uint64_t generation = 1;
  int sent_commands = 0;
  std::vector<mozc::commands::SessionCommand::CommandType> sent_command_types;
  std::string default_digest;
  std::deque<mozc::commands::Output> queued_outputs;
  std::vector<mozc::commands::KeyEvent> sent_keys;
  std::vector<ClientEvent> events;
  std::function<void()> on_next_key;
};

TEST(RawReadingRecoveryTest, RecreatesCompositionFromRawKeysAfterSessionKill) {
  RecoveryTestClient client;
  client.default_digest = std::string(32, 'a');
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();

  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(client.default_digest),
                               false);
  recovery.RecordSuccessfulKey(Key('b'), ActiveOutput(client.default_digest),
                               false);

  client.generation = 2;  // Simulates server/session recreation.
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  ASSERT_EQ(client.sent_keys.size(), 2);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'a');
  EXPECT_EQ(client.sent_keys[1].key_code(), 'b');
  EXPECT_EQ(recovery.journal_size_for_test(), 2);
  EXPECT_EQ(recovery.last_snapshot_relation_for_test(),
            RawReadingRecovery::SnapshotRelation::kSame);
}

TEST(RawReadingRecoveryTest,
     CurrentRawKeyIsRetriedExactlyOnceAfterGenerationAdvances) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.recreate_on_next_key = true;
  EXPECT_TRUE(recovery.DispatchKey(&client, Key('b'), context, false, &output));
  ASSERT_EQ(client.sent_keys.size(), 3);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'b');  // Failed discovery call.
  EXPECT_EQ(client.sent_keys[1].key_code(), 'a');  // Reading reconstruction.
  EXPECT_EQ(client.sent_keys[2].key_code(), 'b');  // The only retry.
  EXPECT_EQ(client.generation, 2);

  ASSERT_EQ(client.events.size(), 4);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[0].generation, 1);
  EXPECT_EQ(client.events[0].value, 'b');
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].generation, 2);
  EXPECT_EQ(client.events[1].value,
            mozc::commands::SessionCommand::SWITCH_COMPOSITION_MODE);
  EXPECT_TRUE(client.events[1].succeeded);
  EXPECT_EQ(client.events[2].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[2].generation, 2);
  EXPECT_EQ(client.events[2].value, 'a');
  EXPECT_TRUE(client.events[2].succeeded);
  EXPECT_EQ(client.events[3].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[3].generation, 2);
  EXPECT_EQ(client.events[3].value, 'b');
  EXPECT_TRUE(client.events[3].succeeded);
}

TEST(RawReadingRecoveryTest,
     InitializerFailureNeverReplaysOrRetriesCurrentKey) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.recreate_on_next_key = true;
  client.fail_next_command = true;
  EXPECT_FALSE(
      recovery.DispatchKey(&client, Key('b'), context, false, &output));

  ASSERT_EQ(client.events.size(), 2);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[0].generation, 1);
  EXPECT_EQ(client.events[0].value, 'b');
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].generation, 2);
  EXPECT_FALSE(client.events[1].succeeded);
  ASSERT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'b');
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest,
     SameGenerationKeyFailureRevertsServerComposition) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.fail_next_key = true;
  EXPECT_FALSE(
      recovery.DispatchKey(&client, Key('b'), context, false, &output));

  ASSERT_EQ(client.events.size(), 2);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[0].generation, 1);
  EXPECT_EQ(client.events[0].value, 'b');
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].generation, 1);
  EXPECT_EQ(client.events[1].value,
            mozc::commands::SessionCommand::REVERT);
  EXPECT_TRUE(client.events[1].succeeded);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest,
     FailedCurrentKeyAfterReplayRevertsReplacementComposition) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.recreate_on_next_key = true;
  client.fail_on_key_call = 3;
  EXPECT_FALSE(
      recovery.DispatchKey(&client, Key('b'), context, false, &output));

  ASSERT_EQ(client.events.size(), 5);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kKey);
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].value,
            mozc::commands::SessionCommand::SWITCH_COMPOSITION_MODE);
  EXPECT_EQ(client.events[1].generation, 2);
  EXPECT_EQ(client.events[2].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[2].value, 'a');
  EXPECT_TRUE(client.events[2].succeeded);
  EXPECT_EQ(client.events[3].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[3].value, 'b');
  EXPECT_FALSE(client.events[3].succeeded);
  EXPECT_EQ(client.events[4].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[4].value, mozc::commands::SessionCommand::REVERT);
  EXPECT_EQ(client.events[4].generation, 2);
  EXPECT_TRUE(client.events[4].succeeded);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest,
     GenerationChangeDuringInitializationRetriesBeforeReplay) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.generation = 2;
  client.recreate_on_next_command = true;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kSessionChanged);

  ASSERT_EQ(client.events.size(), 3);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 2);
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].generation, 3);
  EXPECT_TRUE(client.events[1].succeeded);
  EXPECT_EQ(client.events[2].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[2].generation, 3);
  EXPECT_EQ(client.events[2].value, 'a');
  EXPECT_TRUE(client.events[2].succeeded);
}

TEST(RawReadingRecoveryTest,
     RepeatedGenerationChangeDuringInitializationFailsBeforeReplay) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.generation = 2;
  client.recreate_commands_remaining = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kFailed);

  ASSERT_EQ(client.events.size(), 2);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 2);
  EXPECT_FALSE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[1].generation, 3);
  EXPECT_FALSE(client.events[1].succeeded);
  EXPECT_TRUE(client.sent_keys.empty());
  EXPECT_EQ(recovery.journal_size_for_test(), 1);
}

TEST(RawReadingRecoveryTest,
     ReplayGenerationChangeRequiresInitializationBeforeNextCommand) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.generation = 2;
  client.recreate_on_key_call = 1;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kFailed);
  ASSERT_EQ(client.events.size(), 2);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 2);
  EXPECT_TRUE(client.events[0].succeeded);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[1].generation, 2);
  EXPECT_FALSE(client.events[1].succeeded);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  client.events.clear();
  client.sent_keys.clear();
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  ASSERT_EQ(client.events.size(), 1);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 3);
  EXPECT_TRUE(client.events[0].succeeded);
  EXPECT_TRUE(client.sent_keys.empty());
}

TEST(RawReadingRecoveryTest,
     ReentrantDispatchDuringReplayDefersJournalInvalidation) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  client.events.clear();
  client.sent_keys.clear();

  client.generation = 2;
  client.on_next_key = [&]() {
    mozc::commands::Output nested_output;
    EXPECT_FALSE(recovery.DispatchKey(&client, Key('x'), context, false,
                                      &nested_output));
  };
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kFailed);

  ASSERT_EQ(client.events.size(), 3);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 2);
  EXPECT_EQ(client.events[1].kind, ClientEvent::Kind::kKey);
  EXPECT_EQ(client.events[1].generation, 2);
  EXPECT_EQ(client.events[1].value, 'a');
  EXPECT_EQ(client.events[2].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[2].value, mozc::commands::SessionCommand::REVERT);
  EXPECT_EQ(client.events[2].generation, 2);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  client.events.clear();
  EXPECT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  ASSERT_EQ(client.events.size(), 1);
  EXPECT_EQ(client.events[0].kind, ClientEvent::Kind::kCommand);
  EXPECT_EQ(client.events[0].generation, 2);
}

TEST(RawReadingRecoveryTest,
     SecurePrepareClearsJournalBeforeInitializerFailure) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeModeInitializingRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  ASSERT_EQ(recovery.journal_size_for_test(), 1);

  client.generation = 2;
  client.fail_next_command = true;
  EXPECT_EQ(recovery.Prepare(&client, ValidContext(true), true, &output),
            RawReadingRecovery::PrepareResult::kFailed);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_FALSE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest, SecureCurrentKeyIsNotRetriedAfterRecreation) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext(true);
  ASSERT_EQ(recovery.Prepare(&client, context, true, &output),
            RawReadingRecovery::PrepareResult::kReady);

  client.recreate_on_next_key = true;
  EXPECT_FALSE(recovery.DispatchKey(&client, Key('p'), context, true, &output));
  ASSERT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'p');
}

TEST(RawReadingRecoveryTest,
     SessionChangedSignalDropsStaleCandidateAndSendsOnlyRawKeys) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();

  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('x'), ActiveOutput(), false);
  client.generation = 2;

  // The adapter sees kSessionChanged and must return without dispatching the
  // pending SELECT_CANDIDATE command.  The recovery object itself has no API
  // capable of sending SessionCommand messages.
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  EXPECT_EQ(client.sent_commands, 0);
  ASSERT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'x');
}

TEST(RawReadingRecoveryTest, SecureInputNeverJournalsOrRecoversRawKeys) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context nonsecure_context = ValidContext();
  const mozc::commands::Context secure_context = ValidContext(true);

  ASSERT_EQ(recovery.Prepare(&client, nonsecure_context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('s'), ActiveOutput(), false);
  ASSERT_EQ(recovery.journal_size_for_test(), 1);

  client.generation = 2;
  EXPECT_EQ(recovery.Prepare(&client, secure_context, true, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  EXPECT_TRUE(client.sent_keys.empty());
  EXPECT_EQ(recovery.journal_size_for_test(), 0);

  recovery.RecordSuccessfulKey(Key('p'), ActiveOutput(), true);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
}

TEST(RawReadingRecoveryTest, EventCapFailsClosedInsteadOfKeepingTail) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);

  for (size_t i = 0; i < RawReadingRecovery::kMaxEventCount; ++i) {
    recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  }
  EXPECT_EQ(recovery.journal_size_for_test(),
            RawReadingRecovery::kMaxEventCount);
  recovery.RecordSuccessfulKey(Key('b'), ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  client.generation = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  EXPECT_TRUE(client.sent_keys.empty());
}

TEST(RawReadingRecoveryTest,
     ConversionAndCandidateNavigationKeysSuppressWholeJournal) {
  for (const auto special_key : {
           mozc::commands::KeyEvent::SPACE,
           mozc::commands::KeyEvent::TAB,
           mozc::commands::KeyEvent::LEFT,
           mozc::commands::KeyEvent::DOWN,
           mozc::commands::KeyEvent::HENKAN,
       }) {
    RecoveryTestClient client;
    RawReadingRecovery recovery = MakeRecovery();
    mozc::commands::Output recovered;
    const mozc::commands::Context context = ValidContext();
    ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
              RawReadingRecovery::PrepareResult::kReady);
    recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

    mozc::commands::KeyEvent action;
    action.set_special_key(special_key);
    recovery.RecordSuccessfulKey(action, ActiveOutput(), false);
    EXPECT_EQ(recovery.journal_size_for_test(), 0);
    EXPECT_TRUE(recovery.journal_suppressed_for_test());

    // A later character cannot become a misleading suffix-only journal.
    recovery.RecordSuccessfulKey(Key('b'), ActiveOutput(), false);
    EXPECT_EQ(recovery.journal_size_for_test(), 0);
  }
}

TEST(RawReadingRecoveryTest, ModifiedShortcutSuppressesWholeJournal) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

  mozc::commands::KeyEvent shortcut = Key('x');
  shortcut.add_modifier_keys(mozc::commands::KeyEvent::CTRL);
  recovery.RecordSuccessfulKey(shortcut, ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest, BackspaceMayRemainInRawReadingJournal) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

  mozc::commands::KeyEvent backspace;
  backspace.set_special_key(mozc::commands::KeyEvent::BACKSPACE);
  recovery.RecordSuccessfulKey(backspace, ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 2);
  EXPECT_FALSE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest, ByteCapFailsClosed) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);

  mozc::commands::KeyEvent huge;
  huge.set_key_string(
      std::string(RawReadingRecovery::kMaxSerializedBytes + 1, 'x'));
  recovery.RecordSuccessfulKey(huge, ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest, FocusBoundaryClearsJournalAndAdoptsFreshClient) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

  recovery.ResetSessionBoundary();  // Focus-out/domain change.
  client.generation = 1;            // A new client can restart its counter.
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  EXPECT_TRUE(client.sent_keys.empty());
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
}

TEST(RawReadingRecoveryTest, DomainBoundaryDropsReadingWithoutReplay) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('d'), ActiveOutput(), false);

  recovery.ResetSessionBoundary();  // Capability/domain change.
  client.generation = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  EXPECT_TRUE(client.sent_keys.empty());
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
}

TEST(RawReadingRecoveryTest, FailedRecoveryIsBoundedAndDropsJournal) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

  client.generation = 2;
  client.fail_next_key = true;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kFailed);
  EXPECT_EQ(client.sent_keys.size(), 1);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  EXPECT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  EXPECT_EQ(client.sent_keys.size(), 1);
}

TEST(RawReadingRecoveryTest, PartialReplayIsRevertedBeforeFailingClosed) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  recovery.RecordSuccessfulKey(Key('b'), ActiveOutput(), false);

  client.generation = 2;
  client.fail_on_key_call = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kFailed);
  ASSERT_EQ(client.sent_keys.size(), 2);
  EXPECT_EQ(client.sent_commands, 1);
  ASSERT_EQ(client.sent_command_types.size(), 1);
  EXPECT_EQ(client.sent_command_types[0],
            mozc::commands::SessionCommand::REVERT);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

TEST(RawReadingRecoveryTest, ChangedSnapshotStillReplaysRawReading) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  const std::string old_digest(32, 'o');
  const std::string new_digest(32, 'n');
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(old_digest), false);

  client.generation = 2;
  client.default_digest = new_digest;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  ASSERT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(recovery.last_snapshot_relation_for_test(),
            RawReadingRecovery::SnapshotRelation::kChanged);
}

TEST(RawReadingRecoveryTest, CommitClearsJournal) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);

  mozc::commands::Output committed;
  committed.set_consumed(true);
  committed.mutable_result()->set_value("committed");
  recovery.RecordSuccessfulKey(Key('\n'), committed, false);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
}

TEST(RawReadingRecoveryTest, ReconversionSuppressesJournalUntilCommit) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);

  // The composition now on screen came from CONVERT_REVERSE, not from keys.
  recovery.MarkCompositionNotReplayable();
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  // Edits of that composition must not start a journal of their own: a lone
  // Backspace or a trailing character would rebuild something else.
  mozc::commands::KeyEvent backspace;
  backspace.set_special_key(mozc::commands::KeyEvent::BACKSPACE);
  recovery.RecordSuccessfulKey(backspace, ActiveOutput(), false);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_EQ(recovery.journal_bytes_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());

  // A replacement session gets nothing replayed into it.
  client.generation = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  EXPECT_TRUE(client.sent_keys.empty());
  EXPECT_FALSE(recovered.has_consumed());

  // The commit is the composition boundary that re-enables journalling.
  mozc::commands::Output committed;
  committed.set_consumed(true);
  committed.mutable_result()->set_value("committed");
  recovery.RecordSuccessfulKey(Key('\n'), committed, false);
  EXPECT_FALSE(recovery.journal_suppressed_for_test());
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  EXPECT_EQ(recovery.journal_size_for_test(), 1);
}

TEST(RawReadingRecoveryTest, ReconversionDropsReadingTypedBefore) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
  ASSERT_EQ(recovery.journal_size_for_test(), 1);

  recovery.MarkCompositionNotReplayable();
  EXPECT_EQ(recovery.journal_size_for_test(), 0);
  EXPECT_EQ(recovery.journal_bytes_for_test(), 0);
  EXPECT_TRUE(recovery.journal_suppressed_for_test());
}

mozc::commands::KeyEvent KeyInMode(char code,
                                   mozc::commands::CompositionMode mode) {
  mozc::commands::KeyEvent key = Key(code);
  key.set_mode(mode);
  return key;
}

mozc::commands::Output ActiveOutputInMode(
    mozc::commands::CompositionMode mode) {
  mozc::commands::Output output = ActiveOutput();
  output.set_mode(mode);
  return output;
}

TEST(RawReadingRecoveryTest, ReplayedKeysCarryTheirRecordedMode) {
  RecoveryTestClient client;
  RawReadingRecovery recovery = MakeRecovery();
  mozc::commands::Output recovered;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kReady);

  // "e." typed in half-width ASCII mode.  Replayed without the mode the
  // server would compose it in the initial mode and direct-commit at the
  // punctuation.
  const mozc::commands::KeyEvent letter =
      KeyInMode('e', mozc::commands::HALF_ASCII);
  const mozc::commands::KeyEvent period =
      KeyInMode('.', mozc::commands::HALF_ASCII);
  recovery.RecordSuccessfulKey(letter, ActiveOutput(), false);
  recovery.RecordSuccessfulKey(period, ActiveOutput(), false);
  ASSERT_EQ(recovery.journal_size_for_test(), 2);
  EXPECT_FALSE(recovery.journal_suppressed_for_test());
  EXPECT_EQ(recovery.journal_bytes_for_test(),
            letter.ByteSizeLong() + period.ByteSizeLong());

  client.generation = 2;
  EXPECT_EQ(recovery.Prepare(&client, context, false, &recovered),
            RawReadingRecovery::PrepareResult::kSessionChanged);
  ASSERT_EQ(client.sent_keys.size(), 2);
  ASSERT_TRUE(client.sent_keys[0].has_mode());
  EXPECT_EQ(client.sent_keys[0].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(client.sent_keys[0].SerializeAsString(),
            letter.SerializeAsString());
  ASSERT_TRUE(client.sent_keys[1].has_mode());
  EXPECT_EQ(client.sent_keys[1].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(client.sent_keys[1].SerializeAsString(),
            period.SerializeAsString());
}

TEST(RawReadingRecoveryTest, EveryCompositionModeIsPartOfTheReading) {
  for (const mozc::commands::CompositionMode mode :
       {mozc::commands::HIRAGANA, mozc::commands::FULL_KATAKANA,
        mozc::commands::HALF_ASCII, mozc::commands::FULL_ASCII,
        mozc::commands::HALF_KATAKANA}) {
    RecoveryTestClient client;
    RawReadingRecovery recovery = MakeRecovery();
    mozc::commands::Output recovered;
    ASSERT_EQ(recovery.Prepare(&client, ValidContext(), false, &recovered),
              RawReadingRecovery::PrepareResult::kReady);
    recovery.RecordSuccessfulKey(KeyInMode('a', mode), ActiveOutput(), false);
    EXPECT_EQ(recovery.journal_size_for_test(), 1) << mode;
    EXPECT_FALSE(recovery.journal_suppressed_for_test()) << mode;
  }
}

TEST(RawReadingRecoveryTest, DirectModeOrActivationKeyIsNotReading) {
  const mozc::commands::Context context = ValidContext();
  {
    RecoveryTestClient client;
    RawReadingRecovery recovery = MakeRecovery();
    mozc::commands::Output recovered;
    ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
              RawReadingRecovery::PrepareResult::kReady);
    recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
    // A key that carries DIRECT would turn the IME off when replayed.
    recovery.RecordSuccessfulKey(KeyInMode('b', mozc::commands::DIRECT),
                                 ActiveOutput(), false);
    EXPECT_EQ(recovery.journal_size_for_test(), 0);
    EXPECT_TRUE(recovery.journal_suppressed_for_test());
  }
  {
    RecoveryTestClient client;
    RawReadingRecovery recovery = MakeRecovery();
    mozc::commands::Output recovered;
    ASSERT_EQ(recovery.Prepare(&client, context, false, &recovered),
              RawReadingRecovery::PrepareResult::kReady);
    recovery.RecordSuccessfulKey(Key('a'), ActiveOutput(), false);
    mozc::commands::KeyEvent activating =
        KeyInMode('b', mozc::commands::HIRAGANA);
    activating.set_activated(true);
    recovery.RecordSuccessfulKey(activating, ActiveOutput(), false);
    EXPECT_EQ(recovery.journal_size_for_test(), 0);
    EXPECT_TRUE(recovery.journal_suppressed_for_test());
  }
}

// Stands in for MozcState: the adapter mirrors the session's composition mode,
// its session initializer applies the initial mode, and its key finalizer
// stamps the mode of the session that receives the key.
struct ModeHost {
  static constexpr mozc::commands::CompositionMode kInitialMode =
      mozc::commands::HIRAGANA;

  RawReadingRecovery MakeRecovery() {
    return RawReadingRecovery(
        [this](MozcClientInterface* client, uint64_t generation,
               const mozc::commands::Context&) {
          if (client == nullptr ||
              client->session_generation() != generation) {
            return false;
          }
          ++initializer_calls;
          mode = kInitialMode;
          return true;
        },
        [this](const mozc::commands::Output* replayed_output,
               mozc::commands::KeyEvent* key) {
          finalized_after_initializer_calls.push_back(initializer_calls);
          finalized_with_replay.push_back(replayed_output != nullptr);
          key->set_mode(replayed_output != nullptr &&
                                replayed_output->has_mode()
                            ? replayed_output->mode()
                            : mode);
        });
  }

  mozc::commands::CompositionMode mode = kInitialMode;
  int initializer_calls = 0;
  std::vector<int> finalized_after_initializer_calls;
  std::vector<bool> finalized_with_replay;
};

TEST(RawReadingRecoveryTest, UserKeyIsFinalizedForTheSessionThatReceivesIt) {
  RecoveryTestClient client;
  ModeHost host;
  RawReadingRecovery recovery = host.MakeRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  ASSERT_EQ(host.initializer_calls, 1);

  // The user switched to katakana; the first session follows.
  host.mode = mozc::commands::FULL_KATAKANA;
  mozc::commands::KeyEvent dispatched;
  ASSERT_TRUE(recovery.DispatchKey(&client, Key('a'), context, false, &output,
                                   &dispatched));
  ASSERT_EQ(client.sent_keys.size(), 1);
  EXPECT_EQ(client.sent_keys[0].mode(), mozc::commands::FULL_KATAKANA);
  EXPECT_EQ(dispatched.SerializeAsString(),
            client.sent_keys[0].SerializeAsString());
  client.sent_keys.clear();

  // The server dies.  The send that discovers it still carries the old mode,
  // but the retry goes to a session the initializer just put into the initial
  // mode, and must not override it with the mode of the session that is gone.
  client.recreate_on_next_key = true;
  ASSERT_TRUE(recovery.DispatchKey(&client, Key('b'), context, false, &output,
                                   &dispatched));
  ASSERT_EQ(client.sent_keys.size(), 2);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'b');
  EXPECT_EQ(client.sent_keys[0].mode(), mozc::commands::FULL_KATAKANA);
  EXPECT_EQ(client.sent_keys[1].key_code(), 'b');
  EXPECT_EQ(client.sent_keys[1].mode(), ModeHost::kInitialMode);
  EXPECT_EQ(host.initializer_calls, 2);
  // What the caller journals is the key the surviving session received.
  EXPECT_EQ(dispatched.SerializeAsString(),
            client.sent_keys[1].SerializeAsString());

  // First key, discovering key, retry: the retry was stamped after the
  // initializer of the replacement session ran, and nothing was replayed.
  EXPECT_EQ(host.finalized_after_initializer_calls,
            (std::vector<int>{1, 1, 2}));
  EXPECT_EQ(host.finalized_with_replay,
            (std::vector<bool>{false, false, false}));
}

TEST(RawReadingRecoveryTest, UserKeyAfterReplayContinuesInTheReplayedMode) {
  RecoveryTestClient client;
  ModeHost host;
  RawReadingRecovery recovery = host.MakeRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);

  // A composition typed in half-width ASCII mode.
  host.mode = mozc::commands::HALF_ASCII;
  mozc::commands::KeyEvent dispatched;
  ASSERT_TRUE(recovery.DispatchKey(&client, Key('e'), context, false, &output,
                                   &dispatched));
  recovery.RecordSuccessfulKey(dispatched, output, false);
  ASSERT_EQ(recovery.journal_size_for_test(), 1);
  client.sent_keys.clear();
  host.finalized_with_replay.clear();

  // The server dies.  The initializer resets the adapter to the initial
  // mode, the replay rebuilds the ASCII composition, and the key that
  // triggered all this continues that composition instead of switching it to
  // the initial mode half-way.
  client.recreate_on_next_key = true;
  client.queued_outputs.push_back(
      ActiveOutputInMode(mozc::commands::HALF_ASCII));  // Reply to the replay.
  ASSERT_TRUE(recovery.DispatchKey(&client, Key('x'), context, false, &output,
                                   &dispatched));
  EXPECT_EQ(host.mode, ModeHost::kInitialMode);
  ASSERT_EQ(client.sent_keys.size(), 3);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'x');  // Failed discovery call.
  EXPECT_EQ(client.sent_keys[1].key_code(), 'e');  // Replay, as journalled.
  EXPECT_EQ(client.sent_keys[1].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(client.sent_keys[2].key_code(), 'x');  // Retry.
  EXPECT_EQ(client.sent_keys[2].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(dispatched.SerializeAsString(),
            client.sent_keys[2].SerializeAsString());
  EXPECT_EQ(host.finalized_with_replay, (std::vector<bool>{false, true}));
}

TEST(RawReadingRecoveryTest, ReplayFoundBeforeTheFirstSendDecidesTheMode) {
  RecoveryTestClient client;
  ModeHost host;
  RawReadingRecovery recovery = host.MakeRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);
  recovery.RecordSuccessfulKey(KeyInMode('e', mozc::commands::HALF_ASCII),
                               ActiveOutput(), false);

  // The replacement session already exists when the next key arrives.
  host.mode = mozc::commands::HALF_ASCII;
  client.generation = 2;
  client.queued_outputs.push_back(
      ActiveOutputInMode(mozc::commands::HALF_ASCII));  // Reply to the replay.
  ASSERT_TRUE(recovery.DispatchKey(&client, Key('x'), context, false, &output));
  EXPECT_EQ(host.mode, ModeHost::kInitialMode);
  ASSERT_EQ(client.sent_keys.size(), 2);
  EXPECT_EQ(client.sent_keys[0].key_code(), 'e');
  EXPECT_EQ(client.sent_keys[0].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(client.sent_keys[1].key_code(), 'x');
  EXPECT_EQ(client.sent_keys[1].mode(), mozc::commands::HALF_ASCII);
  EXPECT_EQ(host.finalized_with_replay, (std::vector<bool>{true}));
}

TEST(RawReadingRecoveryTest, FailedDispatchReportsNoDispatchedKey) {
  RecoveryTestClient client;
  ModeHost host;
  RawReadingRecovery recovery = host.MakeRecovery();
  mozc::commands::Output output;
  const mozc::commands::Context context = ValidContext();
  ASSERT_EQ(recovery.Prepare(&client, context, false, &output),
            RawReadingRecovery::PrepareResult::kReady);

  mozc::commands::KeyEvent dispatched;
  client.fail_next_key = true;
  EXPECT_FALSE(recovery.DispatchKey(&client, Key('a'), context, false, &output,
                                    &dispatched));
  EXPECT_EQ(dispatched.ByteSizeLong(), 0);
}

}  // namespace
}  // namespace fcitx
