// Copyright 2026 The Mozkey Authors

#include "protocol/renderer_callback_provenance.h"

#include <cstdint>
#include <limits>

#include "protocol/commands.pb.h"
#include "testing/gunit.h"

namespace mozc::commands {
namespace {

TEST(RendererCallbackProvenanceTest,
     TokenTransportRequiresExactNonzeroTokenAndAbiFit) {
  constexpr uint64_t kToken = 41;
  EXPECT_TRUE(IsRendererCallbackTokenTransportable(
      kToken, kToken, std::numeric_limits<uint32_t>::max()));
  EXPECT_FALSE(IsRendererCallbackTokenTransportable(
      /*received_token=*/0, kToken, std::numeric_limits<uint64_t>::max()));
  EXPECT_FALSE(IsRendererCallbackTokenTransportable(
      /*received_token=*/42, kToken, std::numeric_limits<uint64_t>::max()));

  constexpr uint64_t kWideToken = uint64_t{1} << 32;
  EXPECT_FALSE(IsRendererCallbackTokenTransportable(
      kWideToken, kWideToken, std::numeric_limits<uint32_t>::max()));
  EXPECT_TRUE(IsRendererCallbackTokenTransportable(
      kWideToken, kWideToken, std::numeric_limits<uint64_t>::max()));
}

TEST(RendererCallbackProvenanceTest,
     ProvenanceRequiresTokenDomainAndNonsecureInput) {
  constexpr RendererCallbackProvenance kExpected = {
      .token = 41,
      .focus_epoch = 7,
      .focus_revision = 3,
      .output_generation = 9,
  };
  EXPECT_TRUE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/41, /*current_focus_epoch=*/7,
      /*current_focus_revision=*/3, /*secure_input=*/false,
      /*current_output_generation=*/9));
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/42, /*current_focus_epoch=*/7,
      /*current_focus_revision=*/3, /*secure_input=*/false,
      /*current_output_generation=*/9));
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/41, /*current_focus_epoch=*/8,
      /*current_focus_revision=*/3, /*secure_input=*/false,
      /*current_output_generation=*/9));
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/41, /*current_focus_epoch=*/7,
      /*current_focus_revision=*/4, /*secure_input=*/false,
      /*current_output_generation=*/9));
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/41, /*current_focus_epoch=*/7,
      /*current_focus_revision=*/3, /*secure_input=*/true,
      /*current_output_generation=*/9));
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kExpected, /*received_token=*/41, /*current_focus_epoch=*/7,
      /*current_focus_revision=*/3, /*secure_input=*/false,
      /*current_output_generation=*/10));

  constexpr RendererCallbackProvenance kUninitializedDomain = {
      .token = 41,
      .focus_epoch = 0,
      .focus_revision = 3,
      .output_generation = 9,
  };
  EXPECT_FALSE(IsRendererCallbackProvenanceCurrent(
      kUninitializedDomain, /*received_token=*/41,
      /*current_focus_epoch=*/0, /*current_focus_revision=*/3,
      /*secure_input=*/false, /*current_output_generation=*/9));
}

TEST(RendererCallbackProvenanceTest, SeparatesSelectAndHighlightDispatch) {
  EXPECT_EQ(GetRendererCallbackKind(SessionCommand::SELECT_CANDIDATE),
            RendererCallbackKind::kSelect);
  EXPECT_EQ(GetRendererCallbackKind(SessionCommand::HIGHLIGHT_CANDIDATE),
            RendererCallbackKind::kHighlight);
  EXPECT_EQ(GetRendererCallbackKind(SessionCommand::SUBMIT_CANDIDATE),
            RendererCallbackKind::kSubmit);
  EXPECT_EQ(GetRendererCallbackKind(SessionCommand::SUBMIT),
            RendererCallbackKind::kUnsupported);

  constexpr uint32_t kSelectMessage = 100;
  constexpr uint32_t kHighlightMessage = 101;
  constexpr uint32_t kSubmitMessage = 102;
  EXPECT_EQ(GetRendererCallbackKindForMessage(kSelectMessage, kSelectMessage,
                                              kHighlightMessage,
                                              kSubmitMessage),
            RendererCallbackKind::kSelect);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kHighlightMessage,
                                              kSelectMessage,
                                              kHighlightMessage,
                                              kSubmitMessage),
            RendererCallbackKind::kHighlight);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kSubmitMessage, kSelectMessage,
                                              kHighlightMessage,
                                              kSubmitMessage),
            RendererCallbackKind::kSubmit);
  EXPECT_EQ(GetRendererCallbackKindForMessage(/*message=*/103, kSelectMessage,
                                              kHighlightMessage,
                                              kSubmitMessage),
            RendererCallbackKind::kUnsupported);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kSelectMessage, kSelectMessage,
                                              kSelectMessage, kSubmitMessage),
            RendererCallbackKind::kUnsupported);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kSelectMessage, kSelectMessage,
                                              kHighlightMessage,
                                              kSelectMessage),
            RendererCallbackKind::kUnsupported);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kHighlightMessage,
                                              kSelectMessage,
                                              kHighlightMessage,
                                              kHighlightMessage),
            RendererCallbackKind::kUnsupported);
  EXPECT_EQ(GetRendererCallbackKindForMessage(/*message=*/0, kSelectMessage,
                                              kHighlightMessage,
                                              kSubmitMessage),
            RendererCallbackKind::kUnsupported);
  EXPECT_EQ(GetRendererCallbackKindForMessage(kSelectMessage, kSelectMessage,
                                              kHighlightMessage,
                                              /*submit_message=*/0),
            RendererCallbackKind::kUnsupported);
}

TEST(RendererCallbackProvenanceTest, SubmitCandidateDispatchesLikeSelect) {
  Output output;
  CandidateWindow* candidate_window = output.mutable_candidate_window();
  candidate_window->set_focused_index(0);
  candidate_window->set_size(1);
  candidate_window->set_position(0);
  CandidateWindow::Candidate* candidate = candidate_window->add_candidate();
  candidate->set_index(0);
  candidate->set_id(7);
  candidate->set_value("value");

  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::SUBMIT_CANDIDATE, output, /*candidate_id=*/7),
            RendererCallbackCandidateDisposition::kDispatch);
  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::SUBMIT_CANDIDATE, output, /*candidate_id=*/8),
            RendererCallbackCandidateDisposition::kReject);
}

TEST(RendererCallbackProvenanceTest,
     CandidateMembershipSupportsNestedAndNegativeIds) {
  Output output;
  CandidateWindow* root = output.mutable_candidate_window();
  root->add_candidate()->set_id(10);
  CandidateWindow* nested = root->mutable_sub_candidate_window();
  nested->add_candidate()->set_id(-7);

  EXPECT_TRUE(HasRendererCallbackCandidate(output, 10));
  EXPECT_TRUE(HasRendererCallbackCandidate(output, -7));
  EXPECT_FALSE(HasRendererCallbackCandidate(output, 11));
}

TEST(RendererCallbackProvenanceTest, CandidateTraversalIsDepthBounded) {
  Output accepted;
  CandidateWindow* window = accepted.mutable_candidate_window();
  for (int depth = 0; depth < kMaxRendererCallbackCandidateDepth; ++depth) {
    window = window->mutable_sub_candidate_window();
  }
  window->add_candidate()->set_id(8);
  EXPECT_TRUE(HasRendererCallbackCandidate(accepted, 8));

  Output rejected;
  window = rejected.mutable_candidate_window();
  for (int depth = 0; depth <= kMaxRendererCallbackCandidateDepth; ++depth) {
    window = window->mutable_sub_candidate_window();
  }
  window->add_candidate()->set_id(9);
  EXPECT_FALSE(HasRendererCallbackCandidate(rejected, 9));
}

TEST(RendererCallbackProvenanceTest,
     CandidateDispositionRejectsStaleIdsAndSkipsFocusedHighlight) {
  Output output;
  CandidateWindow* candidate_window = output.mutable_candidate_window();
  candidate_window->set_focused_index(2);
  CandidateWindow::Candidate* candidate = candidate_window->add_candidate();
  candidate->set_id(-7);
  candidate->set_index(2);

  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::HIGHLIGHT_CANDIDATE, output, -7),
            RendererCallbackCandidateDisposition::kAlreadyFocused);
  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::SELECT_CANDIDATE, output, -7),
            RendererCallbackCandidateDisposition::kDispatch);
  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::SELECT_CANDIDATE, output, 99),
            RendererCallbackCandidateDisposition::kReject);
  EXPECT_EQ(GetRendererCallbackCandidateDisposition(
                SessionCommand::SUBMIT, output, -7),
            RendererCallbackCandidateDisposition::kReject);
}

}  // namespace
}  // namespace mozc::commands
