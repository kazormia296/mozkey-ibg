// Copyright 2026 The Mozkey Authors

#include "grimodex/project_dictionary_bridge.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "grimodex/protocol_v1.h"
#include "testing/gunit.h"

namespace mozc::grimodex {
namespace {

constexpr ProjectDictionaryPosIds kPosIds = {
    .person = {.lid = 101, .rid = 102},
    .place = {.lid = 201, .rid = 202},
    .noun = {.lid = 301, .rid = 302},
};

std::shared_ptr<const ProtocolV1Snapshot> Snapshot(
    std::vector<DictionaryEntryDto> entries,
    absl::string_view state_digest = std::string(64, 'a'),
    absl::string_view project_digest = std::string(64, 'b'),
    ProjectConditionsDto conditions = {}) {
  return std::make_shared<const ProtocolV1Snapshot>(ProtocolV1Snapshot{
      .project_id = "project-a",
      .project_name = "Project A",
      .entries = std::move(entries),
      .conditions = std::move(conditions),
      .state_updated_at = "2026-07-17T01:02:03.000Z",
      .project_generated_at = "2026-07-17T01:02:02.000Z",
      .state_sha256 = std::string(state_digest),
      .project_sha256 = std::string(project_digest),
  });
}

std::shared_ptr<const PublishedProtocolV1Snapshot> Publication(
    uint64_t sequence, std::shared_ptr<const ProtocolV1Snapshot> snapshot,
    LoadDiagnostic diagnostic = LoadDiagnostic::kLoaded) {
  return std::make_shared<const PublishedProtocolV1Snapshot>(
      PublishedProtocolV1Snapshot{
          .sequence = sequence,
          .snapshot = std::move(snapshot),
          .diagnostic = diagnostic,
      });
}

DictionaryEntryDto Entry(absl::string_view yomi, absl::string_view surface,
                         DictionaryCategory category, int priority,
                         absl::string_view entry_id) {
  return DictionaryEntryDto{
      .yomi = std::string(yomi),
      .surface = std::string(surface),
      .category = category,
      .priority = priority,
      .entry_id = std::string(entry_id),
  };
}

class CollectTokens final : public dictionary::DictionaryInterface::Callback {
 public:
  ResultType OnToken(absl::string_view, absl::string_view,
                     dictionary::Token token) override {
    tokens.push_back(std::move(token));
    return TRAVERSE_CONTINUE;
  }

  std::vector<dictionary::Token> tokens;
};

dictionary::Token OnlyToken(
    const dictionary::ProjectDictionarySnapshot& snapshot,
    absl::string_view key) {
  CollectTokens callback;
  snapshot.LookupExact(key, &callback);
  EXPECT_EQ(callback.tokens.size(), 1);
  return callback.tokens.empty() ? dictionary::Token() : callback.tokens[0];
}

TEST(ProjectDictionaryBridgeTest, MapsEveryCategoryAndPriority) {
  const auto published = Publication(
      42, Snapshot({
              Entry("ひと", "人", DictionaryCategory::kPerson, 1, "person"),
              Entry("ばしょ", "場所", DictionaryCategory::kPlace, 2, "place"),
              Entry("もの", "物", DictionaryCategory::kNoun, 3, "noun"),
          }, std::string(64, 'a'), std::string(64, 'b'),
          ProjectConditionsDto{
              .topic = "space opera",
              .style = "formal",
              .preference = "names first",
          }));

  const ProjectDictionaryBridgeResult result =
      BuildProjectDictionarySnapshot(published, kPosIds);

  ASSERT_TRUE(result.ready()) << result.status;
  ASSERT_NE(result.snapshot, nullptr);
  ASSERT_NE(result.snapshot->dictionary, nullptr);
  EXPECT_FALSE(result.should_clear());
  EXPECT_EQ(result.snapshot->generation, 42);
  EXPECT_EQ(result.snapshot->dictionary->generation(), 42);
  EXPECT_EQ(result.snapshot->dictionary->source_id(), "project-a");
  EXPECT_EQ(result.snapshot->dictionary->size(), 3);
  EXPECT_EQ(result.snapshot->dictionary->fingerprint().size(), 71);
  EXPECT_EQ(result.snapshot->dictionary->fingerprint().substr(0, 7),
            "sha256:");
  EXPECT_EQ(result.snapshot->conditions.topic, "space opera");
  EXPECT_EQ(result.snapshot->conditions.style, "formal");
  EXPECT_EQ(result.snapshot->conditions.preference, "names first");
  EXPECT_EQ(result.snapshot->dictionary->metadata().topic, "space opera");
  EXPECT_EQ(result.snapshot->dictionary->metadata().style, "formal");
  EXPECT_EQ(result.snapshot->dictionary->metadata().preference,
            "names first");
  EXPECT_EQ(result.snapshot->dictionary->metadata().payload_sha256,
            std::string(64, 'b'));
  EXPECT_EQ(result.snapshot->project_id, "project-a");
  EXPECT_EQ(result.snapshot->project_name, "Project A");
  EXPECT_EQ(result.snapshot->state_updated_at,
            "2026-07-17T01:02:03.000Z");
  EXPECT_EQ(result.snapshot->project_generated_at,
            "2026-07-17T01:02:02.000Z");
  EXPECT_EQ(result.snapshot->state_sha256, std::string(64, 'a'));
  EXPECT_EQ(result.snapshot->project_sha256, std::string(64, 'b'));

  const dictionary::Token person =
      OnlyToken(*result.snapshot->dictionary, "ひと");
  EXPECT_EQ(person.lid, 101);
  EXPECT_EQ(person.rid, 102);
  EXPECT_EQ(person.cost, 3800);
  EXPECT_NE(person.attributes & dictionary::Token::PROJECT_DICTIONARY, 0);

  const dictionary::Token place =
      OnlyToken(*result.snapshot->dictionary, "ばしょ");
  EXPECT_EQ(place.lid, 201);
  EXPECT_EQ(place.rid, 202);
  EXPECT_EQ(place.cost, 3600);

  const dictionary::Token noun =
      OnlyToken(*result.snapshot->dictionary, "もの");
  EXPECT_EQ(noun.lid, 301);
  EXPECT_EQ(noun.rid, 302);
  EXPECT_EQ(noun.cost, 3400);
}

TEST(ProjectDictionaryBridgeTest, RehashesBothSourceDigests) {
  const std::vector<DictionaryEntryDto> entries = {
      Entry("もの", "物", DictionaryCategory::kNoun, 1, "noun")};
  const auto original = BuildProjectDictionarySnapshot(
      Publication(1, Snapshot(entries, std::string(64, 'a'),
                              std::string(64, 'b'))),
      kPosIds);
  const auto changed_state = BuildProjectDictionarySnapshot(
      Publication(1, Snapshot(entries, std::string(64, 'c'),
                              std::string(64, 'b'))),
      kPosIds);
  const auto changed_project = BuildProjectDictionarySnapshot(
      Publication(1, Snapshot(entries, std::string(64, 'a'),
                              std::string(64, 'd'))),
      kPosIds);

  ASSERT_TRUE(original.ready());
  ASSERT_TRUE(changed_state.ready());
  ASSERT_TRUE(changed_project.ready());
  EXPECT_NE(original.snapshot->dictionary->fingerprint(),
            changed_state.snapshot->dictionary->fingerprint());
  EXPECT_NE(original.snapshot->dictionary->fingerprint(),
            changed_project.snapshot->dictionary->fingerprint());
  EXPECT_LE(original.snapshot->dictionary->fingerprint().size(), 128);
}

TEST(ProjectDictionaryBridgeTest,
     ConditionsAndProvenanceRemainPinnedToTheirGeneration) {
  const auto generation1 = BuildProjectDictionarySnapshot(
      Publication(
          7,
          Snapshot({Entry("もの", "物", DictionaryCategory::kNoun, 1,
                          "noun")},
                   std::string(64, 'a'), std::string(64, 'b'),
                   ProjectConditionsDto{.topic = "old topic"})),
      kPosIds);
  const auto generation2 = BuildProjectDictionarySnapshot(
      Publication(
          8,
          Snapshot({Entry("もの", "物", DictionaryCategory::kNoun, 1,
                          "noun")},
                   std::string(64, 'c'), std::string(64, 'd'),
                   ProjectConditionsDto{.topic = "new topic"})),
      kPosIds);

  ASSERT_TRUE(generation1.ready());
  ASSERT_TRUE(generation2.ready());
  EXPECT_EQ(generation1.snapshot->generation, 7);
  EXPECT_EQ(generation1.snapshot->dictionary->generation(), 7);
  EXPECT_EQ(generation1.snapshot->conditions.topic, "old topic");
  EXPECT_EQ(generation1.snapshot->state_sha256, std::string(64, 'a'));
  EXPECT_EQ(generation1.snapshot->project_sha256, std::string(64, 'b'));
  EXPECT_EQ(generation2.snapshot->generation, 8);
  EXPECT_EQ(generation2.snapshot->dictionary->generation(), 8);
  EXPECT_EQ(generation2.snapshot->conditions.topic, "new topic");
  EXPECT_EQ(generation2.snapshot->state_sha256, std::string(64, 'c'));
  EXPECT_EQ(generation2.snapshot->project_sha256, std::string(64, 'd'));
}

TEST(ProjectDictionaryBridgeTest, NullAndInactiveAreExplicitClearResults) {
  const auto null_result = BuildProjectDictionarySnapshot(nullptr, kPosIds);
  EXPECT_EQ(null_result.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kNullPublication);
  EXPECT_TRUE(null_result.should_clear());
  EXPECT_EQ(null_result.snapshot, nullptr);

  const auto inactive = BuildProjectDictionarySnapshot(
      Publication(8, nullptr, LoadDiagnostic::kInactive), kPosIds);
  EXPECT_EQ(inactive.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kInactive);
  EXPECT_TRUE(inactive.status.ok());
  EXPECT_TRUE(inactive.should_clear());
  EXPECT_EQ(inactive.snapshot, nullptr);
}

TEST(ProjectDictionaryBridgeTest,
     BridgesOneRetryableGraceThenFailsClosedWithoutSnapshot) {
  const auto old_snapshot = Snapshot(
      {Entry("ふるい", "古い", DictionaryCategory::kNoun, 1, "old")});

  for (const LoadDiagnostic diagnostic : {
           LoadDiagnostic::kMissingSnapshot,
           LoadDiagnostic::kStateChangedDuringRead,
       }) {
    const auto retained_during_grace =
        BuildProjectDictionarySnapshot(Publication(9, old_snapshot, diagnostic),
                                       kPosIds);
    ASSERT_TRUE(retained_during_grace.ready())
        << static_cast<int>(diagnostic) << ": "
        << retained_during_grace.status;
    ASSERT_NE(retained_during_grace.snapshot, nullptr);
    EXPECT_EQ(retained_during_grace.snapshot->generation, 9);
    EXPECT_TRUE(
        retained_during_grace.snapshot->dictionary->HasKey("ふるい"));

    const auto second_failure = BuildProjectDictionarySnapshot(
        Publication(10, nullptr, diagnostic), kPosIds);
    EXPECT_EQ(second_failure.diagnostic,
              ProjectDictionaryBridgeDiagnostic::kInvalidPublication);
    EXPECT_TRUE(second_failure.should_clear());
    EXPECT_EQ(second_failure.snapshot, nullptr);
  }

  const auto loaded_without_snapshot = BuildProjectDictionarySnapshot(
      Publication(9, nullptr, LoadDiagnostic::kLoaded), kPosIds);
  EXPECT_EQ(loaded_without_snapshot.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kInvalidPublication);
  EXPECT_EQ(loaded_without_snapshot.snapshot, nullptr);
}

TEST(ProjectDictionaryBridgeTest, RejectsInvalidPosIdsAndDigests) {
  const auto published = Publication(
      1, Snapshot(
             {Entry("もの", "物", DictionaryCategory::kNoun, 1, "noun")}));
  ProjectDictionaryPosIds invalid_pos_ids = kPosIds;
  invalid_pos_ids.place.lid = 0;

  const auto invalid_pos =
      BuildProjectDictionarySnapshot(published, invalid_pos_ids);
  EXPECT_EQ(invalid_pos.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kInvalidPosIds);
  EXPECT_EQ(invalid_pos.status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(invalid_pos.snapshot, nullptr);

  const auto invalid_digest = BuildProjectDictionarySnapshot(
      Publication(1, Snapshot(
                         {Entry("もの", "物", DictionaryCategory::kNoun, 1,
                                "noun")},
                         "not-a-digest", std::string(64, 'b'))),
      kPosIds);
  EXPECT_EQ(invalid_digest.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kInvalidPublication);
  EXPECT_EQ(invalid_digest.snapshot, nullptr);
}

TEST(ProjectDictionaryBridgeTest, PropagatesNativeValidationFailure) {
  // A hand-built DTO can bypass ProtocolV1Loader.  The native factory remains
  // the final validation boundary and its exact status is returned.
  const auto published = Publication(
      3, Snapshot(
             {Entry("", "empty reading", DictionaryCategory::kNoun, 2,
                    "invalid-native-entry")}));

  const auto result = BuildProjectDictionarySnapshot(published, kPosIds);

  EXPECT_EQ(result.diagnostic,
            ProjectDictionaryBridgeDiagnostic::kNativeValidationFailed);
  EXPECT_EQ(result.status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(result.snapshot, nullptr);
  EXPECT_TRUE(result.should_clear());
}

}  // namespace
}  // namespace mozc::grimodex
