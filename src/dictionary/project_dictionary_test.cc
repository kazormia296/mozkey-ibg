// Copyright 2026 Grimodex Contributors
//
// Licensed under the same terms as Mozc.

#include "dictionary/project_dictionary.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/dictionary_token.h"
#include "testing/gunit.h"

namespace mozc::dictionary {
namespace {

ProjectDictionaryEntry Entry(absl::string_view key, absl::string_view value,
                             uint8_t priority, absl::string_view entry_id) {
  return ProjectDictionaryEntry{
      .key = std::string(key),
      .value = std::string(value),
      .cost = 4000 - 200 * priority,
      .lid = 10,
      .rid = 10,
      .priority = priority,
      .entry_id = std::string(entry_id),
  };
}

std::shared_ptr<const ProjectDictionarySnapshot> Snapshot(
    uint64_t generation, absl::string_view fingerprint,
    std::vector<ProjectDictionaryEntry> entries) {
  auto snapshot = ProjectDictionarySnapshot::Create(
      generation, "project-a", fingerprint, std::move(entries));
  EXPECT_TRUE(snapshot.ok()) << snapshot.status();
  return snapshot.ok() ? *std::move(snapshot) : nullptr;
}

class CollectTokens final : public DictionaryInterface::Callback {
 public:
  ResultType OnToken(absl::string_view, absl::string_view,
                     Token token) override {
    tokens.push_back(std::move(token));
    return TRAVERSE_CONTINUE;
  }

  std::vector<Token> tokens;
};

class CullFirstKey final : public DictionaryInterface::Callback {
 public:
  ResultType OnKey(absl::string_view key) override {
    keys.emplace_back(key);
    return keys.size() == 1 ? TRAVERSE_CULL : TRAVERSE_CONTINUE;
  }

  std::vector<std::string> keys;
};

TEST(ProjectDictionarySnapshotTest, ExactLookupIsStableAndDeduplicated) {
  const auto snapshot = Snapshot(
      7, "sha256:a",
      {Entry("こーでっくす", "Codex", 1, "later"),
       Entry("こーでっくす", "Codex", 3, "authoritative"),
       Entry("こーでっくす", "コード", 2, "second")});
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->generation(), 7);
  EXPECT_EQ(snapshot->size(), 2);
  EXPECT_TRUE(snapshot->HasKey("こーでっくす"));
  EXPECT_TRUE(snapshot->HasValue("Codex"));

  CollectTokens callback;
  snapshot->LookupExact("こーでっくす", &callback);
  ASSERT_EQ(callback.tokens.size(), 2);
  EXPECT_EQ(callback.tokens[0].value, "Codex");
  EXPECT_EQ(callback.tokens[0].cost, 3400);
  EXPECT_NE(callback.tokens[0].attributes & Token::PROJECT_DICTIONARY, 0);
  EXPECT_EQ(callback.tokens[1].value, "コード");
}

TEST(ProjectDictionarySnapshotTest, PrefixAndPredictiveLookup) {
  const auto snapshot = Snapshot(
      1, "sha256:a",
      {Entry("ぐり", "Grim", 1, "1"),
       Entry("ぐりもでっくす", "Grimodex", 3, "2"),
       Entry("べつ", "別", 1, "3")});
  ASSERT_NE(snapshot, nullptr);

  CollectTokens predictive;
  snapshot->LookupPredictive("ぐり", &predictive);
  ASSERT_EQ(predictive.tokens.size(), 2);
  EXPECT_EQ(predictive.tokens[0].value, "Grim");
  EXPECT_EQ(predictive.tokens[1].value, "Grimodex");

  CollectTokens prefix;
  snapshot->LookupPrefix("ぐりもでっくすの", &prefix);
  ASSERT_EQ(prefix.tokens.size(), 2);
  EXPECT_EQ(prefix.tokens[0].value, "Grim");
  EXPECT_EQ(prefix.tokens[1].value, "Grimodex");
}

TEST(ProjectDictionarySnapshotTest, CullSkipsTheCurrentPrefixSubtree) {
  const auto snapshot = Snapshot(
      1, "sha256:a",
      {Entry("a", "A", 1, "1"), Entry("ab", "AB", 1, "2"),
       Entry("abc", "ABC", 1, "3"), Entry("b", "B", 1, "4")});
  ASSERT_NE(snapshot, nullptr);

  CullFirstKey predictive;
  snapshot->LookupPredictive("", &predictive);
  EXPECT_EQ(predictive.keys, (std::vector<std::string>{"a", "b"}));

  CullFirstKey prefix;
  snapshot->LookupPrefix("abc", &prefix);
  EXPECT_EQ(prefix.keys, (std::vector<std::string>{"a"}));
}

TEST(ProjectDictionarySnapshotTest, RejectsInvalidEntries) {
  auto invalid_priority = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a", {Entry("key", "value", 4, "id")});
  EXPECT_FALSE(invalid_priority.ok());

  auto empty_key = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a", {Entry("", "value", 1, "id")});
  EXPECT_FALSE(empty_key.ok());

  auto invalid_utf8 = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a",
      {Entry("key", std::string("\xC2 "), 1, "id")});
  EXPECT_FALSE(invalid_utf8.ok());

  auto long_entry_id = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a",
      {Entry("key", "value", 1, std::string(129, 'x'))});
  EXPECT_FALSE(long_entry_id.ok());

  auto long_key = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a",
      {Entry(std::string(257, 'a'), "value", 1, "id")});
  EXPECT_FALSE(long_key.ok());

  auto long_value = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a",
      {Entry("key", std::string(257, 'a'), 1, "id")});
  EXPECT_FALSE(long_value.ok());

  auto invalid_source = ProjectDictionarySnapshot::Create(
      1, std::string("\xC2 "), "sha256:a",
      {Entry("key", "value", 1, "id")});
  EXPECT_FALSE(invalid_source.ok());

  auto long_fingerprint = ProjectDictionarySnapshot::Create(
      1, "project-a", std::string(129, 'f'),
      {Entry("key", "value", 1, "id")});
  EXPECT_FALSE(long_fingerprint.ok());

  auto too_many_entries = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a",
      std::vector<ProjectDictionaryEntry>(20'001));
  EXPECT_FALSE(too_many_entries.ok());

  auto invalid_digest = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a", {Entry("key", "value", 1, "id")},
      ProjectDictionaryMetadata{.payload_sha256 = "ABC"});
  EXPECT_FALSE(invalid_digest.ok());

  auto invalid_condition = ProjectDictionarySnapshot::Create(
      1, "project-a", "sha256:a", {Entry("key", "value", 1, "id")},
      ProjectDictionaryMetadata{.topic = std::string("\xC2 ")});
  EXPECT_FALSE(invalid_condition.ok());
}

TEST(ProjectDictionarySnapshotTest, KeepsCompositionMetadataImmutable) {
  const std::string digest(64, 'a');
  auto snapshot = ProjectDictionarySnapshot::Create(
      7, "project-a", "sha256:7", {Entry("key", "value", 1, "id")},
      ProjectDictionaryMetadata{
          .topic = "compiler",
          .style = "concise",
          .preference = "prefer project terminology",
          .payload_sha256 = digest,
      });
  ASSERT_TRUE(snapshot.ok()) << snapshot.status();
  EXPECT_EQ((*snapshot)->metadata().topic, "compiler");
  EXPECT_EQ((*snapshot)->metadata().style, "concise");
  EXPECT_EQ((*snapshot)->metadata().preference,
            "prefer project terminology");
  EXPECT_EQ((*snapshot)->metadata().payload_sha256, digest);
}

TEST(ProjectDictionaryRegistryTest, PinsCompositionAcrossAtomicPublish) {
  ProjectDictionaryRegistry registry;
  const auto generation1 =
      Snapshot(1, "sha256:1", {Entry("a", "A", 1, "1")});
  const auto generation2 =
      Snapshot(2, "sha256:2", {Entry("a", "B", 1, "2")});

  EXPECT_EQ(registry.Publish(generation1),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  EXPECT_EQ(registry.PinForComposition(), generation1);
  EXPECT_EQ(registry.Publish(generation2),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  EXPECT_EQ(registry.latest(), generation2);
  EXPECT_EQ(registry.pinned(), generation1);

  registry.EndComposition();
  EXPECT_EQ(registry.PinForComposition(), generation2);
  EXPECT_EQ(registry.Publish(generation1),
            ProjectDictionaryRegistry::PublishResult::kRejectedStale);
}

TEST(ProjectDictionaryRegistryTest, SecureInputPurgesAndRequiresFreshPublish) {
  ProjectDictionaryRegistry registry;
  const auto generation1 =
      Snapshot(1, "sha256:1", {Entry("a", "A", 1, "1")});
  ASSERT_EQ(registry.Publish(generation1),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  ASSERT_NE(registry.PinForComposition(), nullptr);

  registry.SetSecureInput(true);
  EXPECT_TRUE(registry.secure_input());
  EXPECT_EQ(registry.latest(), nullptr);
  EXPECT_EQ(registry.pinned(), nullptr);
  EXPECT_EQ(registry.Publish(generation1),
            ProjectDictionaryRegistry::PublishResult::kRejectedSecure);

  registry.SetSecureInput(false);
  EXPECT_FALSE(registry.secure_input());
  EXPECT_EQ(registry.PinForComposition(), nullptr);
  EXPECT_EQ(registry.Publish(generation1),
            ProjectDictionaryRegistry::PublishResult::kApplied);
}

TEST(ProjectDictionaryRegistryTest, SameGenerationMustMatchFingerprint) {
  ProjectDictionaryRegistry registry;
  const auto original =
      Snapshot(4, "sha256:1", {Entry("a", "A", 1, "1")});
  const auto identical =
      Snapshot(4, "sha256:1", {Entry("a", "A", 1, "1")});
  const auto conflict =
      Snapshot(4, "sha256:other", {Entry("a", "B", 1, "2")});

  EXPECT_EQ(registry.Publish(original),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  EXPECT_EQ(registry.Publish(identical),
            ProjectDictionaryRegistry::PublishResult::kUnchanged);
  EXPECT_EQ(registry.Publish(conflict),
            ProjectDictionaryRegistry::PublishResult::kRejectedConflict);
  EXPECT_EQ(registry.latest(), original);
}

TEST(ProjectDictionaryRegistryTest, SecurePurgeIsIsolatedPerSessionRegistry) {
  ProjectDictionaryRegistry session_a;
  ProjectDictionaryRegistry session_b;
  const auto generation =
      Snapshot(1, "sha256:1", {Entry("a", "A", 1, "1")});
  ASSERT_EQ(session_a.Publish(generation),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  ASSERT_EQ(session_b.Publish(generation),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  ASSERT_NE(session_a.PinForComposition(), nullptr);
  ASSERT_NE(session_b.PinForComposition(), nullptr);

  session_a.SetSecureInput(true);
  EXPECT_EQ(session_a.latest(), nullptr);
  EXPECT_EQ(session_a.pinned(), nullptr);
  EXPECT_EQ(session_b.latest(), generation);
  EXPECT_EQ(session_b.pinned(), generation);
}

TEST(ProjectDictionaryRegistryTest, CopyHasIndependentLifecycleState) {
  ProjectDictionaryRegistry original;
  const auto generation =
      Snapshot(3, "sha256:3", {Entry("a", "A", 1, "1")});
  ASSERT_EQ(original.Publish(generation),
            ProjectDictionaryRegistry::PublishResult::kApplied);
  ASSERT_EQ(original.PinForComposition(), generation);

  ProjectDictionaryRegistry copied(original);
  EXPECT_EQ(copied.status().latest_generation, 3);
  EXPECT_EQ(copied.status().pinned_generation, 3);

  copied.SetSecureInput(true);
  EXPECT_TRUE(copied.status().secure_input);
  EXPECT_EQ(copied.status().latest_generation, std::nullopt);
  EXPECT_EQ(copied.status().pinned_generation, std::nullopt);
  EXPECT_FALSE(original.status().secure_input);
  EXPECT_EQ(original.status().latest_generation, 3);
  EXPECT_EQ(original.status().pinned_generation, 3);

  ProjectDictionaryRegistry assigned;
  assigned = original;
  assigned.EndComposition();
  EXPECT_EQ(assigned.status().pinned_generation, std::nullopt);
  EXPECT_EQ(original.status().pinned_generation, 3);
}

}  // namespace
}  // namespace mozc::dictionary
