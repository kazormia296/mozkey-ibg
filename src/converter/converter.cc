// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "converter/converter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "base/strings/assign.h"
#include "base/util.h"
#include "base/vlog.h"
#include "composer/composer.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/converter_util.h"
#include "converter/history_reconstructor.h"
#include "converter/immutable_converter_interface.h"
#include "converter/inner_segment.h"
#include "converter/reverse_converter.h"
#include "converter/segments.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_matcher.h"
#include "engine/contextual_candidate_reranker.h"
#include "engine/modules.h"
#include "prediction/predictor_interface.h"
#include "prediction/result.h"
#include "protocol/commands.pb.h"
#include "request/conversion_request.h"
#include "request/request_util.h"
#include "rewriter/rewriter_interface.h"
#include "transliteration/transliteration.h"

namespace mozc {
namespace converter {
namespace {

constexpr size_t kErrorIndex = static_cast<size_t>(-1);

size_t GetSegmentIndex(const Segments* segments, size_t segment_index) {
  const size_t history_segments_size = segments->history_segments_size();
  const size_t result = history_segments_size + segment_index;
  if (result >= segments->segments_size()) {
    return kErrorIndex;
  }
  return result;
}


// Consolidates a multi-segment candidate (span > 1) at `raw_segment_index` into
// a single segment by updating the segment key to the candidate's full reading,
// erasing covered trailing segments, and decrementing `converted_segment_count`
// accordingly.
void MergeMultiSegmentCandidate(Segments* segments, size_t raw_segment_index,
                                bool force_update_key = false) {
  Segment* seg = segments->mutable_segment(raw_segment_index);
  if (seg->candidates_size() == 0) {
    return;
  }
  Candidate* top_cand = seg->mutable_candidate(0);
  const size_t span = top_cand->effective_converted_segment_count();
  if ((force_update_key || span > 1) && !top_cand->key.empty()) {
    seg->set_key(top_cand->key);
  }
  if (span > 1 && raw_segment_index + 1 < segments->segments_size()) {
    const size_t remaining_segments =
        segments->segments_size() - 1 - raw_segment_index;
    DCHECK_LE(span - 1, remaining_segments);
    const size_t erase_count = std::min(span - 1, remaining_segments);
    segments->erase_segments(raw_segment_index + 1, erase_count);
    top_cand->converted_segment_count -= erase_count;
  }
}

bool ShouldInitSegmentsForPrediction(absl::string_view key,
                                     const Segments& segments) {
  // (1) If the segment size is 0, invoke SetKey because the segments is not
  //   correctly prepared.
  //   If the key of the segments differs from the input key,
  //   invoke SetKey because current segments should be completely reset.
  // (2) Otherwise keep current key and candidates.
  //
  // This SetKey omitting is for mobile predictor.
  // On normal inputting, we are showing suggestion results. When users
  // push expansion button, we will add prediction results just after the
  // suggestion results. For this, we don't reset segments for prediction.
  // However, we don't have to do so for suggestion. Here, we are deciding
  // whether the input key is changed or not by using segment key. This is not
  // perfect because for roman input, conversion key is not updated by
  // incomplete input, for example, conversion key is "あ" for the input "a",
  // and will still be "あ" for the input "ak". For avoiding mis-reset of
  // the results, we will reset always for suggestion request type.
  return segments.conversion_segments_size() == 0 ||
         segments.conversion_segment(0).key() != key;
}

bool IsValidSegments(const ConversionRequest& request,
                     const Segments& segments) {
  const bool is_mobile = request.request().zero_query_suggestion() &&
                         request.request().mixed_conversion();

  // All segments should have candidate
  for (const Segment& segment : segments) {
    if (segment.candidates_size() != 0) {
      continue;
    }
    // On mobile, we don't distinguish candidates and meta candidates
    // So it's ok if we have meta candidates even if we don't have candidates
    // TODO(team): we may remove mobile check if other platforms accept
    // meta candidate only segment
    if (is_mobile && segment.meta_candidates_size() != 0) {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

Converter::Converter(
    std::unique_ptr<engine::Modules> modules,
    const ImmutableConverterFactory& immutable_converter_factory,
    const PredictorFactory& predictor_factory,
    const RewriterFactory& rewriter_factory)
    : modules_(std::move(modules)),
      immutable_converter_(immutable_converter_factory(*modules_)),
      pos_matcher_(modules_->GetPosMatcher()),
      user_dictionary_(modules_->GetUserDictionary()),
      history_reconstructor_(modules_->GetPosMatcher()),
      reverse_converter_(*immutable_converter_),
      general_noun_id_(pos_matcher_.GetGeneralNounId()) {
  DCHECK(immutable_converter_);
  predictor_ = predictor_factory(*modules_, *this, *immutable_converter_);
  rewriter_ = rewriter_factory(*modules_);
  DCHECK(predictor_);
  DCHECK(rewriter_);
}

bool Converter::StartConversion(const ConversionRequest& request,
                                Segments* segments) const {
  DCHECK_EQ(request.request_type(), ConversionRequest::CONVERSION);

  absl::string_view key = request.key();
  if (key.empty()) {
    return false;
  }

  segments->InitForConvert(key);
  ApplyConversion(segments, request);
  return IsValidSegments(request, *segments);
}

bool Converter::StartReverseConversion(Segments* segments,
                                       const absl::string_view key) const {
  segments->Clear();
  if (key.empty()) {
    return false;
  }
  segments->InitForConvert(key);

  return reverse_converter_.ReverseConvert(key, segments);
}

// static
void Converter::MaybeSetConsumedKeySizeToCandidate(size_t consumed_key_size,
                                                   Candidate* candidate) {
  if (candidate->attributes & Attribute::PARTIALLY_KEY_CONSUMED) {
    // If PARTIALLY_KEY_CONSUMED is set already,
    // the candidate has set appropriate attribute and size by predictor.
    return;
  }
  candidate->attributes |= Attribute::PARTIALLY_KEY_CONSUMED;
  candidate->consumed_key_size = consumed_key_size;
}

// static
void Converter::MaybeSetConsumedKeySizeToSegment(size_t consumed_key_size,
                                                 Segment* segment) {
  for (size_t i = 0; i < segment->candidates_size(); ++i) {
    MaybeSetConsumedKeySizeToCandidate(consumed_key_size,
                                       segment->mutable_candidate(i));
  }
  for (size_t i = 0; i < segment->meta_candidates_size(); ++i) {
    MaybeSetConsumedKeySizeToCandidate(consumed_key_size,
                                       segment->mutable_meta_candidate(i));
  }
}

namespace {
bool ValidateConversionRequestForPrediction(const ConversionRequest& request) {
  switch (request.request_type()) {
    case ConversionRequest::CONVERSION:
      // Conversion request is not for prediction.
      return false;
    case ConversionRequest::PREDICTION:
    case ConversionRequest::SUGGESTION:
      // Typical use case.
      return true;
    case ConversionRequest::PARTIAL_PREDICTION:
    case ConversionRequest::PARTIAL_SUGGESTION: {
      // Partial prediction/suggestion request is applicable only if the
      // cursor is in the middle of the composer.
      const size_t cursor = request.composer().GetCursor();
      return cursor != 0 || cursor != request.composer().GetLength();
    }
    default:
      ABSL_UNREACHABLE();
  }
}
}  // namespace

bool Converter::StartPrediction(const ConversionRequest& request,
                                Segments* segments) const {
  DCHECK(ValidateConversionRequestForPrediction(request));

  absl::string_view key = request.key();
  if (ShouldInitSegmentsForPrediction(key, *segments)) {
    segments->InitForConvert(key);
  }
  DCHECK_EQ(segments->conversion_segments_size(), 1);
  DCHECK_EQ(segments->conversion_segment(0).key(), key);

  if (!PredictForRequestWithSegments(request, segments)) {
    // Prediction can fail for keys like "12". Even in such cases, rewriters
    // (e.g., number and variant rewriters) can populate some candidates.
    // Therefore, this is not an error.
    MOZC_VLOG(1) << "PredictForRequest failed for key: "
                 << segments->segment(0).key();
  }
  ApplyPostProcessing(request, segments);
  return IsValidSegments(request, *segments);
}

bool Converter::StartPredictionWithPreviousSuggestion(
    const ConversionRequest& request, const Segment& previous_segment,
    Segments* segments) const {
  bool result = StartPrediction(request, segments);
  segments->PrependCandidates(previous_segment);
  if (!result) {
    return false;
  }

  ApplyPostProcessing(request, segments);
  return IsValidSegments(request, *segments);
}

void Converter::PrependCandidates(const ConversionRequest& request,
                                  const Segment& segment,
                                  Segments* segments) const {
  segments->PrependCandidates(segment);
  ApplyPostProcessing(request, segments);
}

void Converter::ApplyPostProcessing(const ConversionRequest& request,
                                    Segments* segments) const {
  RewriteAndSuppressCandidates(request, segments);
  TrimCandidates(request, segments);
  if (request.request_type() == ConversionRequest::PARTIAL_SUGGESTION ||
      request.request_type() == ConversionRequest::PARTIAL_PREDICTION) {
    // Here 1st segment's key is the query string of
    // the partial prediction/suggestion.
    // e.g. If the composition is "わた|しは", the key is "わた".
    // If partial prediction/suggestion candidate is submitted,
    // all the characters which are located from the head to the cursor
    // should be submitted (in above case "わた" should be submitted).
    // To do this, PARTIALLY_KEY_CONSUMED and consumed_key_size should be set.
    // Note that this process should be done in a predictor because
    // we have to do this on the candidates created by rewriters.
    MaybeSetConsumedKeySizeToSegment(Util::CharsLen(request.key()),
                                     segments->mutable_conversion_segment(0));
  }
}

void Converter::FinishConversion(const ConversionRequest& request,
                                 Segments* segments) const {
  // Consolidates multi-segment candidates (span > 1) into single segments
  // before committing to history and learning. During conversion, individual
  // segments are preserved for navigation and reversion, but upon commit,
  // covered trailing segments are merged and erased.
  const size_t hist_size = segments->history_segments_size();
  const bool force_update_key =
      request.request_type() != ConversionRequest::CONVERSION;
  for (size_t i = 0; i < segments->conversion_segments_size(); ++i) {
    MergeMultiSegmentCandidate(segments, hist_size + i, force_update_key);
  }

  for (Segment& segment : *segments) {
    // revert SUBMITTED segments to FIXED_VALUE
    // SUBMITTED segments are created by "submit first segment" operation
    // (ctrl+N for ATOK keymap).
    // To learn the conversion result, we should change the segment types
    // to FIXED_VALUE.
    if (segment.segment_type() == Segment::SUBMITTED) {
      segment.set_segment_type(Segment::FIXED_VALUE);
    }
    if (segment.candidates_size() > 0) {
      CompletePosIds(segment.mutable_candidate(0));
    }
  }

  PopulateReadingOfCommittedCandidateIfMissing(segments);

  // Sets unique revert id.
  // Clients store the last commit operations with this id.
  absl::BitGen bitgen;
  const uint64_t revert_id = absl::Uniform<uint64_t>(
      absl::IntervalClosed, bitgen, 1, std::numeric_limits<uint64_t>::max());
  segments->set_revert_id(revert_id);

  const prediction::Result history_result = HistorySegmentsToResult(*segments);
  const std::vector<prediction::Result> committed_results =
      ConversionSegmentsToResults(*segments);
  const ConversionRequest finish_req = ConversionRequestBuilder()
                                           .SetConversionRequestView(request)
                                           .SetHistoryResultView(history_result)
                                           .Build();

  // A suggestion is represented as one outer Segment whose key can still be
  // the typed prefix, while its selected candidate holds the complete reading
  // and (sometimes) multiple inner segments. Preserve that ordinary commit
  // for the predictor, history context, character-form rules, and undo.
  // Build a separately verified view *only* for user segment history.
  std::optional<Segments> suggestion_learning_segments;
  if (request.options().enable_verified_suggestion_segment_learning &&
      request.request_type() == ConversionRequest::CONVERSION &&
      !request.request().mixed_conversion() && !request.incognito_mode() &&
      !request.context().is_private_input() &&
      request.composer().GetInputFieldType() != commands::Context::PASSWORD &&
      request.options().enable_user_history_for_conversion &&
      request.config().history_learning_level() ==
          config::Config::DEFAULT_HISTORY &&
      !segments->resized() && segments->conversion_segments_size() == 1 &&
      segments->conversion_segment(0).candidates_size() > 0) {
    // Undo cleanup must precede speculative candidate ranking and learning.
    rewriter_->PrepareForSuggestionLearning(finish_req);
    const Segment& source = segments->conversion_segment(0);
    const Candidate& selected = source.candidate(0);
    constexpr uint32_t kUnsafeSuggestionAttributes =
        Attribute::NO_HISTORY_LEARNING | Attribute::NO_SUGGEST_LEARNING |
        Attribute::PARTIALLY_KEY_CONSUMED |
        Attribute::AUTO_PARTIAL_SUGGESTION |
        Attribute::TYPING_CORRECTION | Attribute::SPELLING_CORRECTION |
        Attribute::KEY_EXPANDED_IN_DICTIONARY |
        Attribute::COMMAND_CANDIDATE | Attribute::POST_CORRECTION |
        Attribute::NUMBER;
    constexpr size_t kMaxSegments = 16;
    constexpr size_t kMaxKeyChars = 64;
    constexpr size_t kMaxValueChars = 64;
    constexpr size_t kMaxValueBytes = 512;
    const auto valid_text = [](absl::string_view text) {
      return !text.empty() && Util::IsValidUtf8(text) &&
             text.find_first_of("\t\r\n") == absl::string_view::npos;
    };
    if (valid_text(source.key()) && valid_text(selected.key) &&
        valid_text(selected.value) &&
        source.key() == request.key() &&
        absl::StartsWith(selected.key, source.key()) &&
        selected.effective_converted_segment_count() == 1 &&
        (selected.attributes & kUnsafeSuggestionAttributes) == 0 &&
        Util::CharsLen(selected.key) <= kMaxKeyChars &&
        Util::CharsLen(selected.value) <= kMaxValueChars &&
        selected.value.size() <= kMaxValueBytes) {
      // Preserve the original commit for every component except legacy
      // segment-history learning. A learning view retains native candidate
      // metadata and the original revert_id; it is not another commit.
      auto build_learning_view =
          [&](const Segments& native,
              const std::vector<std::pair<std::string, std::string>>& pairs,
              const std::vector<size_t>& selected_indices)
          -> std::optional<Segments> {
        if (pairs.empty() || pairs.size() != selected_indices.size() ||
            native.conversion_segments_size() != pairs.size()) {
          return std::nullopt;
        }
        Segments learning = *segments;
        learning.clear_conversion_segments();
        learning.set_resized(false);  // This is not a boundary edit.
        for (size_t i = 0; i < pairs.size(); ++i) {
          const Segment& native_seg = native.conversion_segment(i);
          const auto& [key, value] = pairs[i];
          const size_t matched = selected_indices[i];
          if (native_seg.key() != key ||
              matched >= native_seg.candidates_size() ||
              native_seg.candidate(matched).key != key ||
              native_seg.candidate(matched).value != value ||
              native_seg.candidate(matched)
                      .effective_converted_segment_count() != 1 ||
              (native_seg.candidate(matched).attributes &
               Attribute::NO_HISTORY_LEARNING) != 0) {
            return std::nullopt;
          }
          Segment* dest = learning.add_segment();
          *dest = native_seg;
          dest->set_segment_type(Segment::FIXED_VALUE);
          dest->mutable_candidate(0)->attributes |= Attribute::BEST_CANDIDATE;
          dest->move_candidate(matched, 0);
          if (matched != 0) {
            dest->mutable_candidate(0)->attributes |= Attribute::RERANKED;
            dest->mutable_candidate(0)->attributes &=
                ~Attribute::BEST_CANDIDATE;
          }
        }
        return learning;
      };

      // Only a postprocessed *normal* conversion has a meaningful displayed
      // default rank. Dictionary-only paths can verify spelling and boundaries
      // but must never supply RERANKED/BEST_CANDIDATE provenance.
      std::optional<Segments> displayed_normal;

      // Verify candidate-supplied boundaries (or strict reverse-resolved
      // boundaries) against the immutable converter before learning.
      auto verify_learning_pairs =
          [&](const std::vector<std::pair<std::string, std::string>>& pairs)
          -> std::optional<Segments> {
        if (pairs.empty() || pairs.size() > kMaxSegments) {
          return std::nullopt;
        }
        std::vector<uint8_t> key_sizes;
        std::string joined_key;
        std::string joined_value;
        key_sizes.reserve(pairs.size());
        for (const auto& [key, value] : pairs) {
          const size_t key_chars = Util::CharsLen(key);
          if (!valid_text(key) || !valid_text(value) || key_chars == 0 ||
              key_chars > std::numeric_limits<uint8_t>::max()) {
            return std::nullopt;
          }
          key_sizes.push_back(static_cast<uint8_t>(key_chars));
          joined_key.append(key);
          joined_value.append(value);
        }
        if (joined_key != selected.key || joined_value != selected.value) {
          return std::nullopt;
        }

        // Fixed-boundary conversion is read-only and keeps native candidate
        // metadata (including POS and content keys) instead of inventing it.
        Segments native;
        native.InitForConvert(selected.key);
        ConversionRequest::Options native_options = request.options();
        native_options.request_type = ConversionRequest::CONVERSION;
        native_options.enable_user_history_for_conversion = false;
        native_options.enable_verified_suggestion_segment_learning = false;
        native_options.max_conversion_candidates_size = std::max<int>(
            64, native_options.max_conversion_candidates_size);
        if (!native.Resize(0, key_sizes) ||
            !immutable_converter_->Convert(native_options, &native) ||
            native.conversion_segments_size() != pairs.size()) {
          return std::nullopt;
        }

        std::vector<size_t> selected_indices;
        selected_indices.reserve(pairs.size());
        for (size_t i = 0; i < pairs.size(); ++i) {
          const Segment& native_seg = native.conversion_segment(i);
          const auto& [key, value] = pairs[i];
          if (native_seg.key() != key) {
            return std::nullopt;
          }
          size_t matched = native_seg.candidates_size();
          for (size_t j = 0; j < native_seg.candidates_size(); ++j) {
            const Candidate& cand = native_seg.candidate(j);
            if (cand.key == key && cand.value == value &&
                cand.effective_converted_segment_count() == 1 &&
                (cand.attributes & Attribute::NO_HISTORY_LEARNING) == 0) {
              matched = j;
              break;
            }
          }
          if (matched == native_seg.candidates_size()) {
            return std::nullopt;
          }
          selected_indices.push_back(matched);
        }
        // Validate that the very same pairs exist in the real, postprocessed
        // normal-conversion candidate list, and take their indices from THERE.
        // The natural segmentation may differ from the forced dictionary
        // boundaries; in that case the native ranks cannot be generalized to
        // an ordinary full-reading conversion, so skip extra segment learning.
        if (!displayed_normal.has_value() ||
            displayed_normal->conversion_segments_size() != pairs.size()) {
          return std::nullopt;
        }
        std::vector<size_t> displayed_indices;
        displayed_indices.reserve(pairs.size());
        for (size_t i = 0; i < pairs.size(); ++i) {
          const Segment& visible = displayed_normal->conversion_segment(i);
          const auto& [key, value] = pairs[i];
          if (visible.key() != key) {
            return std::nullopt;
          }
          size_t matched = visible.candidates_size();
          for (size_t j = 0; j < visible.candidates_size(); ++j) {
            const Candidate& candidate = visible.candidate(j);
            if (candidate.key == key && candidate.value == value &&
                candidate.effective_converted_segment_count() == 1 &&
                (candidate.attributes & Attribute::NO_HISTORY_LEARNING) == 0) {
              if (matched != visible.candidates_size()) {
                return std::nullopt;  // Ambiguous identical surfaces.
              }
              matched = j;
            }
          }
          if (matched == visible.candidates_size()) {
            return std::nullopt;
          }
          displayed_indices.push_back(matched);
        }
        return build_learning_view(*displayed_normal, pairs,
                                   displayed_indices);
      };

      // A suggestion may lack inner boundaries even when its complete
      // surface corresponds to candidates visible in ordinary conversion.
      // Match the *postprocessed* normal-conversion segments, not only the
      // immutable converter output. The probe never commits anything.
      auto match_normal_conversion = [&](const Segments& normal)
          -> std::optional<Segments> {
        if (normal.conversion_segments_size() <= 1 ||
            normal.conversion_segments_size() > kMaxSegments) {
          return std::nullopt;
        }
        std::string joined_key;
        for (const Segment& seg : normal.conversion_segments()) {
          if (!valid_text(seg.key()) || seg.candidates_size() == 0) {
            return std::nullopt;
          }
          joined_key.append(seg.key());
        }
        if (joined_key != selected.key) {
          return std::nullopt;
        }

        // A capped, exhaustive walk of *visible* candidate surfaces.  The
        // default conversion size is 200, so an arbitrary 64-candidate
        // threshold would reject common readings even when the second
        // candidate is the user's explicit choice.
        constexpr size_t kMaxCandidateChecks = 32768;
        constexpr size_t kMaxCandidatesPerSegment = 512;
        size_t candidate_checks = 0;
        size_t distinct_paths = 0;
        bool exhausted = false;
        std::vector<size_t> active_indices;
        std::vector<size_t> matched_indices;
        std::function<void(size_t, size_t)> find_path =
            [&](size_t segment_index, size_t byte_offset) {
              if (exhausted || distinct_paths > 1) {
                return;
              }
              if (segment_index == normal.conversion_segments_size()) {
                if (byte_offset == selected.value.size()) {
                  ++distinct_paths;
                  if (distinct_paths == 1) {
                    matched_indices = active_indices;
                  }
                }
                return;
              }
              const Segment& seg = normal.conversion_segment(segment_index);
              const size_t limit = std::min(seg.candidates_size(),
                                            kMaxCandidatesPerSegment);
              if (seg.candidates_size() > limit) {
                exhausted = true;  // Cannot prove uniqueness by truncation.
                return;
              }
              for (size_t j = 0; j < limit; ++j) {
                if (++candidate_checks > kMaxCandidateChecks) {
                  exhausted = true;
                  return;
                }
                const Candidate& cand = seg.candidate(j);
                if (cand.key != seg.key() || cand.value.empty() ||
                    cand.effective_converted_segment_count() != 1 ||
                    (cand.attributes & Attribute::NO_HISTORY_LEARNING) != 0 ||
                    cand.value.size() > selected.value.size() - byte_offset ||
                    selected.value.compare(byte_offset, cand.value.size(),
                                           cand.value) != 0) {
                  continue;
                }
                bool duplicate_surface = false;
                for (size_t previous = 0; previous < j; ++previous) {
                  const Candidate& earlier = seg.candidate(previous);
                  if (earlier.key == cand.key && earlier.value == cand.value &&
                      earlier.effective_converted_segment_count() == 1 &&
                      (earlier.attributes & Attribute::NO_HISTORY_LEARNING) ==
                          0) {
                    duplicate_surface = true;
                    break;
                  }
                }
                if (duplicate_surface) {
                  continue;
                }
                active_indices.push_back(j);
                find_path(segment_index + 1,
                          byte_offset + cand.value.size());
                active_indices.pop_back();
                if (exhausted || distinct_paths > 1) {
                  return;
                }
              }
            };
        find_path(0, 0);
        if (exhausted || distinct_paths != 1) {
          return std::nullopt;
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.reserve(matched_indices.size());
        for (size_t i = 0; i < matched_indices.size(); ++i) {
          const Segment& seg = normal.conversion_segment(i);
          pairs.emplace_back(seg.key(),
                             seg.candidate(matched_indices[i]).value);
        }
        // A non-top choice in the actual conversion is the same signal as
        // a direct CommitSegmentValue(..., nonzero_index).
        return build_learning_view(normal, pairs, matched_indices);
      };

      // Complete normal conversion, including history prediction and rewriter
      // ordering, on an isolated Segments copy.  The composer may still hold
      // the typed prefix, so SetKey explicitly supplies the full reading.
      // Suppress pending-revert consumption while examining candidates.
      {
        ConversionRequest::Options normal_options = request.options();
        normal_options.request_type = ConversionRequest::CONVERSION;
        normal_options.enable_verified_suggestion_segment_learning = false;
        normal_options.used_in_predictor_realtime_conversion = false;
        normal_options.skip_pending_revert_for_suggestion_learning = true;
        normal_options.use_explicit_key_for_user_history_conversion = true;
        const ConversionRequest normal_request =
            ConversionRequestBuilder()
                .SetConversionRequestView(request)
                .SetOptions(normal_options)
                .SetKey(selected.key)
                .Build();
        Segments normal = *segments;
        normal.clear_conversion_segments();
        normal.set_revert_id(0);
        normal.set_resized(false);
        if (StartConversion(normal_request, &normal)) {
          // EngineConverter::Convert applies this final contextual ranking
          // after Converter::StartConversion. Learning must compare against
          // the same displayed default as ordinary conversion.
          engine::ContextualCandidateReranker contextual_reranker;
          contextual_reranker.Rerank(&normal);
          suggestion_learning_segments = match_normal_conversion(normal);
          if (!suggestion_learning_segments.has_value()) {
            displayed_normal = std::move(normal);
          }
        }
      }

      // Retain strict source-boundary verification when the visible native
      // conversion cannot uniquely explain the selected suggestion.
      std::vector<std::pair<std::string, std::string>> original_pairs;
      for (const auto& inner : selected.inner_segments()) {
        if (original_pairs.size() >= kMaxSegments) {
          original_pairs.clear();
          break;
        }
        original_pairs.emplace_back(inner.GetKey(), inner.GetValue());
      }
      if (!suggestion_learning_segments.has_value() &&
          original_pairs.size() > 1) {
        suggestion_learning_segments = verify_learning_pairs(original_pairs);
      }

      // Do not treat immutable dictionary order as the order of candidates
      // shown in ordinary conversion. If the visible candidate path cannot be
      // proven, the original whole-suggestion history is still committed.

      if (!suggestion_learning_segments.has_value()) {
        // Zenz already uses this native reverse-reading resolver for text
        // without segment boundaries. It requires an exact key/value round
        // trip and a unique forward-verified segmentation. Failure is safe:
        // retain the existing whole-suggestion learning behavior.
        std::vector<ExternalConversionSegment> resolved;
        if (ResolveExternalConversionSegments(request, selected.key,
                                              selected.value, &resolved) &&
            resolved.size() > 1 && resolved.size() <= kMaxSegments) {
          std::vector<std::pair<std::string, std::string>> resolved_pairs;
          resolved_pairs.reserve(resolved.size());
          for (const ExternalConversionSegment& segment : resolved) {
            resolved_pairs.emplace_back(segment.key, segment.value);
          }
          suggestion_learning_segments = verify_learning_pairs(resolved_pairs);
        }
      }

      // Preserve Phase 1's verified full-reading single-segment learning when
      // there was no usable multi-segment explanation. No partial or guessed
      // segmentation is persisted.
      if (!suggestion_learning_segments.has_value() &&
          original_pairs.size() == 1) {
        suggestion_learning_segments = verify_learning_pairs(original_pairs);
      }
    }
  }

  if (suggestion_learning_segments.has_value()) {
    rewriter_->FinishWithSuggestionLearningSegments(
        finish_req, *segments, &*suggestion_learning_segments);
  } else {
    rewriter_->Finish(finish_req, *segments);
  }
  predictor_->Finish(finish_req, committed_results, segments->revert_id());

  // Remove the front segments except for some segments which will be
  // used as history segments.
  const int start_index = std::max<int>(
      0, segments->segments_size() - segments->max_history_segments_size());
  for (int i = 0; i < start_index; ++i) {
    segments->pop_front_segment();
  }

  // Remaining segments are used as history segments.
  for (Segment& segment : *segments) {
    segment.set_segment_type(Segment::HISTORY);
  }
}

bool Converter::LearnExternalConversionResult(
    const ConversionRequest& request,
    absl::string_view key,
    absl::string_view value) const {
  return LearnExternalConversionResultReversibly(
      request, key, value, /*revert_segments=*/nullptr);
}

bool Converter::LearnExternalConversionResultReversibly(
    const ConversionRequest& request,
    absl::string_view key,
    absl::string_view value,
    Segments* revert_segments) const {
  key = absl::StripAsciiWhitespace(key);
  value = absl::StripAsciiWhitespace(value);

  constexpr size_t kMinKeyChars = 2;
  constexpr size_t kMaxKeyChars = 128;
  constexpr size_t kMaxValueChars = 128;
  constexpr size_t kMaxValueBytes = 512;

  if (key.empty() || value.empty()) {
    return false;
  }

  if (!Util::IsValidUtf8(key) || !Util::IsValidUtf8(value)) {
    return false;
  }

  if (Util::CharsLen(key) < kMinKeyChars ||
      Util::CharsLen(key) > kMaxKeyChars ||
      Util::CharsLen(value) > kMaxValueChars ||
      value.size() > kMaxValueBytes) {
    return false;
  }

  // Do not persist multi-line or TSV-breaking generated text.
  if (key.find('\t') != absl::string_view::npos ||
      key.find('\r') != absl::string_view::npos ||
      key.find('\n') != absl::string_view::npos ||
      value.find('\t') != absl::string_view::npos ||
      value.find('\r') != absl::string_view::npos ||
      value.find('\n') != absl::string_view::npos) {
    return false;
  }

  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }

  if (request.incognito_mode()) {
    return false;
  }

  if (!request.options().enable_user_history_for_conversion) {
    return false;
  }

  if (request.config().history_learning_level() !=
      config::Config::DEFAULT_HISTORY) {
    return false;
  }

  // Learn the accepted external result as one complete committed conversion.
  // Segment-local or lexical-unit extraction is intentionally not performed
  // here; callers that need it must route it through a separate Mozc-history
  // path with explicit safety rules.
  Segments learning_segments;
  learning_segments.InitForCommit(key, value);

  if (learning_segments.conversion_segments_size() != 1) {
    return false;
  }

  Segment* segment = learning_segments.mutable_conversion_segment(0);
  if (segment->candidates_size() == 0) {
    return false;
  }

  segment->set_segment_type(Segment::FIXED_VALUE);

  Candidate* candidate = segment->mutable_candidate(0);
  candidate->key.assign(key.data(), key.size());
  candidate->content_key.assign(key.data(), key.size());
  candidate->value.assign(value.data(), value.size());
  candidate->content_value.assign(value.data(), value.size());

  // Mark this as a user-selected non-default result.  UserSegmentHistoryRewriter
  // uses RERANKED to force trigger-key insertion, which is important for
  // recalling an externally generated candidate later.
  candidate->attributes |= Attribute::RERANKED;

  FinishConversion(request, &learning_segments);
  if (revert_segments != nullptr) {
    *revert_segments = learning_segments;
  }
  return true;
}

bool Converter::LearnExternalConversionSegments(
    const ConversionRequest& request,
    absl::Span<const ExternalConversionSegment> segments) const {
  return LearnExternalConversionSegmentsReversibly(
      request, segments, /*revert_segments=*/nullptr);
}

bool Converter::LearnExternalConversionSegmentsReversibly(
    const ConversionRequest& request,
    absl::Span<const ExternalConversionSegment> segments,
    Segments* revert_segments) const {
  constexpr size_t kMaxSegments = 16;
  constexpr size_t kMaxSegmentKeyChars = 128;
  constexpr size_t kMaxSegmentValueChars = 128;
  constexpr size_t kMaxSegmentValueBytes = 512;
  // UserBoundaryHistoryRewriter persists each target segment length in a
  // 4-bit field. A boundary-resized external commit must not claim success
  // when its target boundary cannot be represented by that downstream store.
  constexpr size_t kMaxBoundaryHistorySegmentKeyChars = 15;
  constexpr size_t kMinTotalKeyChars = 2;
  constexpr size_t kMaxTotalKeyChars = 256;
  constexpr size_t kMaxTotalValueChars = 256;
  constexpr size_t kMaxTotalValueBytes = 1024;

  if (segments.empty() || segments.size() > kMaxSegments) {
    return false;
  }

  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }

  if (request.incognito_mode()) {
    return false;
  }

  if (!request.options().enable_user_history_for_conversion) {
    return false;
  }

  if (request.config().history_learning_level() !=
      config::Config::DEFAULT_HISTORY) {
    return false;
  }

  const bool boundary_resized = segments.front().boundary_resized;

  std::vector<ExternalConversionSegment> normalized_segments;
  normalized_segments.reserve(segments.size());
  std::vector<uint8_t> key_sizes;
  key_sizes.reserve(segments.size());

  std::string total_key;
  std::string total_value;
  for (const ExternalConversionSegment& segment : segments) {
    if (segment.boundary_resized != boundary_resized) {
      return false;
    }

    const std::string key =
        std::string(absl::StripAsciiWhitespace(segment.key));
    const std::string value =
        std::string(absl::StripAsciiWhitespace(segment.value));

    if (key.empty() || value.empty()) {
      return false;
    }
    if (!Util::IsValidUtf8(key) || !Util::IsValidUtf8(value)) {
      return false;
    }

    const size_t key_chars = Util::CharsLen(key);
    if (key_chars > kMaxSegmentKeyChars ||
        Util::CharsLen(value) > kMaxSegmentValueChars ||
        value.size() > kMaxSegmentValueBytes ||
        key_chars > std::numeric_limits<uint8_t>::max()) {
      return false;
    }
    if (boundary_resized &&
        key_chars > kMaxBoundaryHistorySegmentKeyChars) {
      return false;
    }

    // Do not persist multi-line or TSV-breaking generated text.
    if (key.find('\t') != std::string::npos ||
        key.find('\r') != std::string::npos ||
        key.find('\n') != std::string::npos ||
        value.find('\t') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
      return false;
    }

    absl::StrAppend(&total_key, key);
    absl::StrAppend(&total_value, value);
    key_sizes.push_back(static_cast<uint8_t>(key_chars));
    normalized_segments.push_back(
        {key, value, segment.is_reranked, boundary_resized});
  }

  if (Util::CharsLen(total_key) < kMinTotalKeyChars ||
      Util::CharsLen(total_key) > kMaxTotalKeyChars ||
      Util::CharsLen(total_value) > kMaxTotalValueChars ||
      total_value.size() > kMaxTotalValueBytes) {
    return false;
  }

  // Probe the accepted sequence once with the caller-supplied boundaries and
  // user history disabled.  This preserves intra-phrase context when copying
  // native candidate metadata and avoids N independent StartConversion calls.
  // For a caller-reported boundary change, every accepted surface must be
  // reproduced exactly by this immutable fixed-boundary conversion.
  std::vector<std::optional<Candidate>> fixed_boundary_candidates(
      normalized_segments.size());
  bool fixed_boundary_probe_valid = false;

  Segments fixed_segments;
  fixed_segments.InitForConvert(total_key);
  if (fixed_segments.Resize(0, key_sizes)) {
    ConversionRequest::Options fixed_options = request.options();
    fixed_options.request_type = ConversionRequest::CONVERSION;
    fixed_options.enable_user_history_for_conversion = false;
    fixed_options.max_conversion_candidates_size =
        std::max<int>(64, fixed_options.max_conversion_candidates_size);

    if (immutable_converter_->Convert(fixed_options, &fixed_segments) &&
        fixed_segments.conversion_segments_size() ==
            normalized_segments.size()) {
      fixed_boundary_probe_valid = true;
      for (size_t i = 0; i < normalized_segments.size(); ++i) {
        const Segment& fixed_segment = fixed_segments.conversion_segment(i);
        if (fixed_segment.key() != normalized_segments[i].key) {
          fixed_boundary_probe_valid = false;
          break;
        }

        for (size_t j = 0; j < fixed_segment.candidates_size(); ++j) {
          const Candidate& candidate = fixed_segment.candidate(j);
          if (candidate.value == normalized_segments[i].value) {
            fixed_boundary_candidates[i] = candidate;
            break;
          }
        }
      }
    }
  }

  if (boundary_resized) {
    if (!fixed_boundary_probe_valid) {
      return false;
    }
    for (const std::optional<Candidate>& candidate :
         fixed_boundary_candidates) {
      if (!candidate.has_value()) {
        return false;
      }
    }
  }

  // Learn the accepted external result as one committed sequence. Phrase
  // boundaries are either the unchanged live Mozc boundaries or a
  // converter-verified native resegmentation.
  Segments learning_segments;
  learning_segments.clear_conversion_segments();

  for (size_t segment_index = 0;
       segment_index < normalized_segments.size(); ++segment_index) {
    const ExternalConversionSegment& segment =
        normalized_segments[segment_index];

    Segment* learning_segment = learning_segments.add_segment();
    learning_segment->set_key(segment.key);
    learning_segment->set_segment_type(Segment::FIXED_VALUE);

    Candidate* candidate = learning_segment->add_candidate();
    if (fixed_boundary_probe_valid &&
        fixed_boundary_candidates[segment_index].has_value()) {
      *candidate = *fixed_boundary_candidates[segment_index];
    } else {
      candidate->key = segment.key;
      candidate->content_key = segment.key;
      candidate->value = segment.value;
      candidate->content_value = segment.value;
    }

    // The copied candidate may already contain ranking-related attributes.
    // External learning controls only those ranking signals explicitly.
    candidate->attributes &=
        ~(Attribute::RERANKED |
          Attribute::BEST_CANDIDATE |
          Attribute::USER_SEGMENT_HISTORY_REWRITER);

    // Treat only segments with explicit local evidence as user-selected
    // non-default commits. Other context segments remain part of the committed
    // sequence and are learned with ordinary Mozc commit semantics.
    if (segment.is_reranked) {
      candidate->attributes |= Attribute::RERANKED;
    }
  }

  // A caller-proven and converter-verified resegmentation reproduces the same
  // signal as a manual boundary edit.  A one-segment result is intentional:
  // UserBoundaryHistoryRewriter uses it to learn joining several old segments
  // into one.
  learning_segments.set_resized(boundary_resized);

  FinishConversion(request, &learning_segments);
  if (revert_segments != nullptr) {
    *revert_segments = learning_segments;
  }
  return true;
}

bool Converter::ResolveExternalConversionSegments(
    const ConversionRequest& request, absl::string_view key,
    absl::string_view value,
    std::vector<ExternalConversionSegment>* segments) const {
  if (segments == nullptr) {
    return false;
  }
  segments->clear();

  constexpr size_t kMaxSegments = 16;
  // Native resegmentation is best-effort user-history enrichment on the
  // latency-sensitive IME path. Long spans are more expensive to reverse and
  // are also less suitable for durable local generalization, so fail closed
  // and leave them to the existing full-sequence feedback/fallback paths.
  constexpr size_t kMaxKeyChars = 64;
  constexpr size_t kMaxValueChars = 64;
  constexpr size_t kMaxValueBytes = 1024;
  // Explore lower-ranked reverse readings only while the ambiguity remains
  // small. Once many exact prefix paths survive, the evidence is weak enough
  // that persistent local-boundary learning is not worth blocking the IME.
  constexpr size_t kMaxReverseReadingPaths = 16;

  if (key.empty() || value.empty() ||
      request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }
  if (!Util::IsValidUtf8(key) || !Util::IsValidUtf8(value) ||
      Util::CharsLen(key) > kMaxKeyChars ||
      Util::CharsLen(value) > kMaxValueChars ||
      value.size() > kMaxValueBytes) {
    return false;
  }

  // First ask Mozc's reverse converter for a native surface segmentation.
  // Reverse conversion can expose multiple reading candidates for one surface.
  // Candidate rank is useful evidence, but it must not choose a persistent
  // boundary when another exact reading-boundary explanation also round-trips.
  Segments reverse_segments;
  if (!StartReverseConversion(&reverse_segments, value)) {
    return false;
  }
  const size_t reverse_size = reverse_segments.conversion_segments_size();
  if (reverse_size == 0 || reverse_size > kMaxSegments) {
    return false;
  }

  std::vector<std::string> native_values;
  native_values.reserve(reverse_size);
  std::string reconstructed_value;
  for (const Segment& reverse_segment : reverse_segments.conversion_segments()) {
    if (reverse_segment.key().empty() ||
        reverse_segment.candidates_size() == 0 ||
        reverse_segment.candidate(0).value.empty()) {
      return false;
    }

    const std::string native_value(reverse_segment.key());
    // LearnExternalConversionSegments strips ASCII whitespace at segment
    // boundaries.  A reverse surface containing such whitespace cannot be
    // represented by the downstream learning contract without mutation.
    if (absl::StripAsciiWhitespace(native_value).size() !=
        native_value.size()) {
      return false;
    }
    reconstructed_value.append(native_value);
    native_values.push_back(native_value);
  }
  if (reconstructed_value != value) {
    return false;
  }

  ConversionRequest::Options probe_options = request.options();
  probe_options.request_type = ConversionRequest::CONVERSION;
  probe_options.enable_user_history_for_conversion = false;
  probe_options.max_conversion_candidates_size =
      std::max<int>(64, probe_options.max_conversion_candidates_size);

  // Validates one complete reading path by forcing its boundaries onto the
  // original key and asking the immutable converter to reproduce every reverse
  // surface exactly.  This probe has no rewriter / Finish() side effects.
  auto validate_reading_path =
      [&](const std::vector<std::string>& native_keys,
          std::vector<ExternalConversionSegment>* resolved) -> bool {
    if (resolved == nullptr || native_keys.size() != reverse_size) {
      return false;
    }
    resolved->clear();

    std::vector<uint8_t> native_key_sizes;
    native_key_sizes.reserve(reverse_size);
    std::string reconstructed_key;
    for (const std::string& native_key : native_keys) {
      if (native_key.empty() || !Util::IsValidUtf8(native_key) ||
          absl::StripAsciiWhitespace(native_key).size() != native_key.size()) {
        return false;
      }
      const size_t key_chars = Util::CharsLen(native_key);
      if (key_chars == 0 || key_chars > std::numeric_limits<uint8_t>::max()) {
        return false;
      }
      reconstructed_key.append(native_key);
      native_key_sizes.push_back(static_cast<uint8_t>(key_chars));
    }
    if (reconstructed_key != key) {
      return false;
    }

    Segments forward_segments;
    forward_segments.InitForConvert(key);
    if (!forward_segments.Resize(0, native_key_sizes)) {
      return false;
    }
    if (!immutable_converter_->Convert(probe_options, &forward_segments) ||
        forward_segments.conversion_segments_size() != reverse_size) {
      return false;
    }

    std::vector<ExternalConversionSegment> candidate_resolution;
    candidate_resolution.reserve(reverse_size);
    for (size_t i = 0; i < reverse_size; ++i) {
      const Segment& forward_segment = forward_segments.conversion_segment(i);
      if (forward_segment.key() != native_keys[i]) {
        return false;
      }

      size_t matched_index = forward_segment.candidates_size();
      for (size_t j = 0; j < forward_segment.candidates_size(); ++j) {
        if (forward_segment.candidate(j).value == native_values[i]) {
          matched_index = j;
          break;
        }
      }
      if (matched_index == forward_segment.candidates_size()) {
        return false;
      }

      candidate_resolution.push_back(
          {native_keys[i], native_values[i], matched_index != 0});
    }

    *resolved = std::move(candidate_resolution);
    return true;
  };

  // Enumerate every byte-exact reverse-reading path, including candidate(0).
  // The resolver is allowed to persist a boundary only when forward verification
  // leaves exactly one explanation.  This intentionally gives up the old
  // candidate(0) fast return in exchange for fail-closed boundary learning.

  struct ReverseReadingPath {
    size_t key_bytes = 0;
    std::vector<std::string> readings;
  };

  std::vector<ReverseReadingPath> paths(1);
  paths.front().readings.reserve(reverse_size);

  // Search only reading candidates that are byte-exact prefixes of the
  // original Zenz key.  Duplicate readings inside one reverse segment are
  // collapsed so duplicate dictionary candidates do not create fake
  // ambiguity.  Search explosion fails closed.
  for (size_t segment_index = 0; segment_index < reverse_size;
       ++segment_index) {
    const Segment& reverse_segment =
        reverse_segments.conversion_segment(segment_index);
    std::vector<ReverseReadingPath> next_paths;

    for (const ReverseReadingPath& path : paths) {
      std::vector<absl::string_view> seen_readings;
      seen_readings.reserve(reverse_segment.candidates_size());

      for (const auto& candidate_ptr : reverse_segment.candidates()) {
        const Candidate& candidate = *candidate_ptr;
        const absl::string_view reading = candidate.value;
        if (reading.empty() ||
            std::find(seen_readings.begin(), seen_readings.end(), reading) !=
                seen_readings.end()) {
          continue;
        }
        seen_readings.push_back(reading);

        if (absl::StripAsciiWhitespace(reading).size() != reading.size() ||
            path.key_bytes + reading.size() > key.size() ||
            key.substr(path.key_bytes, reading.size()) != reading) {
          continue;
        }

        const size_t next_key_bytes = path.key_bytes + reading.size();
        const bool is_last = segment_index + 1 == reverse_size;
        if ((is_last && next_key_bytes != key.size()) ||
            (!is_last && next_key_bytes == key.size())) {
          continue;
        }

        if (next_paths.size() >= kMaxReverseReadingPaths) {
          return false;
        }
        ReverseReadingPath next = path;
        next.key_bytes = next_key_bytes;
        next.readings.emplace_back(reading);
        next_paths.push_back(std::move(next));
      }
    }

    if (next_paths.empty()) {
      return false;
    }
    paths = std::move(next_paths);
  }

  // Do not guess between multiple reading-boundary explanations, regardless of
  // reverse-candidate rank.  Forward verification may eliminate false reverse
  // candidates; exactly one verified path is required.
  std::vector<ExternalConversionSegment> unique_resolution;
  bool found_resolution = false;
  for (const ReverseReadingPath& path : paths) {
    if (path.key_bytes != key.size()) {
      continue;
    }
    std::vector<ExternalConversionSegment> candidate_resolution;
    if (!validate_reading_path(path.readings, &candidate_resolution)) {
      continue;
    }
    if (found_resolution) {
      segments->clear();
      return false;
    }
    unique_resolution = std::move(candidate_resolution);
    found_resolution = true;
  }

  if (!found_resolution) {
    return false;
  }
  *segments = std::move(unique_resolution);
  return true;
}

bool Converter::EvaluateExternalConversionSegments(
    const ConversionRequest& request,
    absl::Span<const ExternalConversionSegment> segments,
    std::vector<ExternalConversionSegment>* evaluated_segments) const {
  if (evaluated_segments == nullptr) {
    return false;
  }
  evaluated_segments->clear();

  constexpr size_t kMaxSegments = 16;
  constexpr size_t kMaxTotalKeyChars = 256;
  constexpr size_t kMaxTotalValueChars = 256;
  constexpr size_t kMaxTotalValueBytes = 1024;

  if (segments.empty() || segments.size() > kMaxSegments ||
      request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }

  std::string total_key;
  std::string total_value;
  std::vector<uint8_t> key_sizes;
  key_sizes.reserve(segments.size());

  for (const ExternalConversionSegment& segment : segments) {
    if (segment.key.empty() || segment.value.empty() ||
        !Util::IsValidUtf8(segment.key) || !Util::IsValidUtf8(segment.value)) {
      return false;
    }

    // Keep the exact boundary contract. Trimming here would make the probe
    // evaluate a different segment sequence than the one observed by Session.
    if (absl::StripAsciiWhitespace(segment.key).size() != segment.key.size() ||
        absl::StripAsciiWhitespace(segment.value).size() !=
            segment.value.size()) {
      return false;
    }

    const size_t key_chars = Util::CharsLen(segment.key);
    if (key_chars == 0 || key_chars > std::numeric_limits<uint8_t>::max()) {
      return false;
    }

    total_key.append(segment.key);
    total_value.append(segment.value);
    key_sizes.push_back(static_cast<uint8_t>(key_chars));
  }

  if (Util::CharsLen(total_key) > kMaxTotalKeyChars ||
      Util::CharsLen(total_value) > kMaxTotalValueChars ||
      total_value.size() > kMaxTotalValueBytes) {
    return false;
  }

  Segments probe_segments;
  probe_segments.InitForConvert(total_key);
  if (!probe_segments.Resize(0, key_sizes)) {
    return false;
  }

  ConversionRequest::Options probe_options = request.options();
  probe_options.request_type = ConversionRequest::CONVERSION;
  probe_options.enable_user_history_for_conversion = false;
  probe_options.max_conversion_candidates_size =
      std::max<int>(64, probe_options.max_conversion_candidates_size);

  // Use the immutable converter so this probe cannot invoke a history rewriter
  // or a Finish() hook. The supplied Resize() boundaries are the only
  // segmentation under evaluation.
  if (!immutable_converter_->Convert(probe_options, &probe_segments) ||
      probe_segments.conversion_segments_size() != segments.size()) {
    return false;
  }

  std::vector<ExternalConversionSegment> evaluated;
  evaluated.reserve(segments.size());
  for (size_t i = 0; i < segments.size(); ++i) {
    const Segment& probe_segment = probe_segments.conversion_segment(i);
    const ExternalConversionSegment& expected = segments[i];

    if (probe_segment.key() != expected.key) {
      return false;
    }

    size_t matched_index = probe_segment.candidates_size();
    for (size_t j = 0; j < probe_segment.candidates_size(); ++j) {
      if (probe_segment.candidate(j).value == expected.value) {
        matched_index = j;
        break;
      }
    }
    if (matched_index == probe_segment.candidates_size()) {
      return false;
    }

    evaluated.push_back(
        {expected.key, expected.value, matched_index != 0, false});
  }

  *evaluated_segments = std::move(evaluated);
  return true;
}

void Converter::LookupUserDictionaryPrefixEntries(
    absl::string_view key,
    std::vector<UserDictionaryLookupResult>* results) const {
  if (results == nullptr) {
    return;
  }
  results->clear();
  if (key.empty()) {
    return;
  }

  using Callback = dictionary::DictionaryInterface::Callback;
  dictionary::InlineCallback callback;
  callback.OnToken([results](absl::string_view, absl::string_view,
                             const dictionary::Token& token) {
    if (token.key.empty() || token.value.empty()) {
      return Callback::TRAVERSE_CONTINUE;
    }

    UserDictionaryLookupResult result;
    result.key = std::string(token.key);
    result.value = std::string(token.value);
    results->push_back(std::move(result));
    return Callback::TRAVERSE_CONTINUE;
  });

  user_dictionary_.LookupPrefix(key, &callback);
}

void Converter::CancelConversion(Segments* segments) const {
  segments->clear_conversion_segments();
}

void Converter::ResetConversion(Segments* segments) const { segments->Clear(); }

void Converter::RevertConversion(Segments* segments) const {
  if (segments->revert_id() == 0) {
    return;
  }
  rewriter_->Revert(*segments);
  predictor_->Revert(segments->revert_id());
  segments->set_revert_id(0);
}

void Converter::DiscardConversionRevertState(Segments* segments) const {
  if (segments == nullptr || segments->revert_id() == 0) {
    return;
  }
  rewriter_->DiscardRevert(*segments);
  predictor_->DiscardRevert(segments->revert_id());
  segments->set_revert_id(0);
}

bool Converter::DeleteCandidateFromHistory(const Segments& segments,
                                           size_t segment_index,
                                           int candidate_index) const {
  DCHECK_LT(segment_index, segments.segments_size());
  const Segment& segment = segments.segment(segment_index);
  DCHECK(segment.is_valid_index(candidate_index));
  const Candidate& candidate = segment.candidate(candidate_index);
  bool result = false;
  result |=
      rewriter_->ClearHistoryEntry(segments, segment_index, candidate_index);
  result |= predictor_->ClearHistoryEntry(candidate.key, candidate.value);

  return result;
}

bool Converter::AddSuppressionEntry(absl::string_view key,
                                    absl::string_view value) const {
  return user_dictionary_.AddSuppressionEntry(key, value);
}

bool Converter::ReconstructHistory(
    Segments* segments, const absl::string_view preceding_text) const {
  segments->Clear();
  return history_reconstructor_.ReconstructHistory(preceding_text, segments);
}

bool Converter::CommitSegmentValueInternal(
    Segments* segments, size_t segment_index, int candidate_index,
    Segment::SegmentType segment_type) const {
  segment_index = GetSegmentIndex(segments, segment_index);
  if (segment_index == kErrorIndex) {
    return false;
  }

  Segment* segment = segments->mutable_segment(segment_index);
  const int values_size = static_cast<int>(segment->candidates_size());
  if (candidate_index < -transliteration::NUM_T13N_TYPES ||
      candidate_index >= values_size) {
    return false;
  }

  segment->set_segment_type(segment_type);
  segment->move_candidate(candidate_index, 0);

  if (candidate_index != 0) {
    segment->mutable_candidate(0)->attributes |= Attribute::RERANKED;
  }

  return true;
}

bool Converter::CommitSegmentValue(Segments* segments, size_t segment_index,
                                   int candidate_index) const {
  return CommitSegmentValueInternal(segments, segment_index, candidate_index,
                                    Segment::FIXED_VALUE);
}

bool Converter::CommitPartialSuggestionSegmentValue(
    Segments* segments, size_t segment_index, int candidate_index,
    const absl::string_view current_segment_key,
    const absl::string_view new_segment_key) const {
  DCHECK_GT(segments->conversion_segments_size(), 0);

  const size_t raw_segment_index = GetSegmentIndex(segments, segment_index);
  if (!CommitSegmentValueInternal(segments, segment_index, candidate_index,
                                  Segment::SUBMITTED)) {
    return false;
  }

  Segment* segment = segments->mutable_segment(raw_segment_index);
  DCHECK_LT(0, segment->candidates_size());
  segment->set_key(current_segment_key);

  Segment* new_segment = segments->insert_segment(raw_segment_index + 1);
  new_segment->set_key(new_segment_key);
  DCHECK_GT(segments->conversion_segments_size(), 0);

  return true;
}

bool Converter::FocusSegmentValue(Segments* segments, size_t segment_index,
                                  int candidate_index) const {
  segment_index = GetSegmentIndex(segments, segment_index);
  if (segment_index == kErrorIndex) {
    return false;
  }

  return rewriter_->Focus(segments, segment_index, candidate_index);
}

bool Converter::CommitSegments(Segments* segments,
                               absl::Span<const size_t> candidate_index) const {
  for (size_t i = 0; i < candidate_index.size(); ++i) {
    // 2nd argument must always be 0 because on each iteration
    // 1st segment is submitted.
    // Using 0 means submitting 1st segment iteratively.
    const size_t raw_segment_index = GetSegmentIndex(segments, 0);
    if (!CommitSegmentValueInternal(segments, 0, candidate_index[i],
                                    Segment::SUBMITTED)) {
      return false;
    }
    MergeMultiSegmentCandidate(segments, raw_segment_index);
  }
  return true;
}

bool Converter::ResizeSegment(Segments* segments,
                              const ConversionRequest& request,
                              size_t segment_index, int offset_length) const {
  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }

  // invalid request
  if (offset_length == 0) {
    return false;
  }

  if (segment_index >= segments->conversion_segments_size()) {
    return false;
  }

  const size_t key_len = segments->conversion_segment(segment_index).key_len();
  if (key_len == 0) {
    return false;
  }

  const int new_size = key_len + offset_length;
  if (new_size <= 0 || new_size > std::numeric_limits<uint8_t>::max()) {
    return false;
  }
  const std::array<uint8_t, 1> new_size_array = {
      static_cast<uint8_t>(new_size)};
  return ResizeSegments(segments, request, segment_index, new_size_array);
}

bool Converter::ResizeSegments(Segments* segments,
                               const ConversionRequest& request,
                               size_t start_segment_index,
                               absl::Span<const uint8_t> new_size_array) const {
  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }

  start_segment_index = GetSegmentIndex(segments, start_segment_index);
  if (start_segment_index == kErrorIndex) {
    return false;
  }

  if (!segments->Resize(start_segment_index, new_size_array)) {
    return false;
  }

  ApplyConversion(segments, request);
  return true;
}

void Converter::CommitContext(const ConversionRequest& request) const {
  predictor_->CommitContext(request);
}

void Converter::ApplyConversion(Segments* segments,
                                const ConversionRequest& request) const {
  if (!immutable_converter_->Convert(request.options(), segments)) {
    // Conversion can fail for keys like "12". Even in such cases, rewriters
    // (e.g., number and variant rewriters) can populate some candidates.
    // Therefore, this is not an error.
    MOZC_VLOG(1) << "Convert failed for key: " << segments->segment(0).key();
  }

  MaybeApplyPredictionToConversion(request, segments);
  ApplyPostProcessing(request, segments);
}

bool Converter::ResizeSegmentsByResult(const ConversionRequest& request,
                                       const prediction::Result& result,
                                       Segments* segments) const {
  std::vector<uint8_t> new_sizes;
  for (const auto& inner_seg : result.inner_segments()) {
    const size_t char_len = Util::CharsLen(inner_seg.GetKey());
    if (char_len == 0 || char_len > std::numeric_limits<uint8_t>::max()) {
      return false;
    }
    new_sizes.push_back(static_cast<uint8_t>(char_len));
  }

  bool need_resize = (segments->conversion_segments_size() < new_sizes.size());
  if (!need_resize) {
    for (size_t i = 0; i < new_sizes.size(); ++i) {
      if (segments->conversion_segment(i).key_len() != new_sizes[i]) {
        need_resize = true;
        break;
      }
    }
  }

  if (need_resize) {
    if (!ResizeSegments(segments, request, 0, new_sizes)) {
      LOG(WARNING) << "Failed to resize or convert segments for result match";
      return false;
    }
  }

  return true;
}

void Converter::ApplyResultToSegments(const prediction::Result& result,
                                      size_t target_pos,
                                      uint32_t additional_attributes,
                                      Segments* segments) const {
  if (result.inner_segments().size() != segments->conversion_segments_size()) {
    return;
  }

  size_t seg_idx = 0;
  size_t inner_idx = 0;
  const size_t inner_segment_count = result.inner_segments().size();
  for (const auto& inner_seg : result.inner_segments()) {
    if (seg_idx >= segments->conversion_segments_size()) break;
    const size_t current_seg_idx = seg_idx++;
    Segment* segment = segments->mutable_conversion_segment(current_seg_idx);
    const uint8_t ranking_constraints =
        (result.inner_segment_ranking_constraints.size() ==
         inner_segment_count)
            ? result.inner_segment_ranking_constraints[inner_idx]
            : result.ranking_constraints;
    ++inner_idx;

    // Find existing candidate with matching value.
    int existing_index = -1;
    for (int i = 0; i < segment->candidates_size(); ++i) {
      if (segment->candidate(i).value == inner_seg.GetValue()) {
        existing_index = i;
        break;
      }
    }

    const size_t pos = std::min(
        target_pos, static_cast<size_t>(segment->candidates_size() - 1));
    if (existing_index >= 0) {
      segment->move_candidate(existing_index, pos);
      Candidate* cand = segment->mutable_candidate(pos);
      cand->cost = std::min(cand->cost, result.cost);
      cand->wcost = std::min(cand->wcost, result.wcost);
      cand->attributes |= (result.attributes | additional_attributes);
      // The existing candidate and `result` are alternative derivations of
      // the same surface.  Keep NO_TOP only when both derivations require it.
      // This preserves the explicit non-EMOTICON user-dictionary escape hatch.
      // Mozkey IbG: PROJECT_DICTIONARY is provenance, not an ordering
      // constraint, so it is kept when either derivation carries it.
      const uint8_t project_provenance =
          (cand->ranking_constraints | ranking_constraints) &
          RankingConstraint::PROJECT_DICTIONARY;
      cand->ranking_constraints =
          (cand->ranking_constraints & ranking_constraints) |
          project_provenance;
      if (current_seg_idx < result.inner_segment_boundary.size()) {
        cand->inner_segment_boundary = {
            result.inner_segment_boundary[current_seg_idx]};
      }
    } else {
      Candidate* cand = (pos == 0) ? segment->push_front_candidate()
                                   : segment->insert_candidate(pos);
      strings::Assign(cand->key, inner_seg.GetKey());
      strings::Assign(cand->value, inner_seg.GetValue());
      strings::Assign(cand->content_key, inner_seg.GetContentKey());
      strings::Assign(cand->content_value, inner_seg.GetContentValue());
      cand->lid = result.lid;
      cand->rid = result.rid;
      cand->wcost = result.wcost;
      cand->cost = result.cost;
      cand->attributes = (result.attributes | additional_attributes);
      cand->ranking_constraints = ranking_constraints;
      cand->consumed_key_size = result.consumed_key_size;
      if (current_seg_idx < result.inner_segment_boundary.size()) {
        cand->inner_segment_boundary = {
            result.inner_segment_boundary[current_seg_idx]};
      }
    }
  }
}

std::vector<prediction::Result> Converter::PredictForConversion(
    const ConversionRequest& request, const Segments& segments,
    const prediction::Result& default_result) const {
  // Build ConversionRequest with history_result extracted from segments,
  // consistent with PredictForRequestWithSegments.
  const prediction::Result history_result = HistorySegmentsToResult(segments);
  const ConversionRequest conv_req = ConversionRequestBuilder()
                                         .SetConversionRequestView(request)
                                         .SetHistoryResultView(history_result)
                                         .Build();

  // 1. Run SupplementalModel (PostCorrect).
  // Stop applying post correction when handwriting mode, consistent with
  // DictionaryPredictor::MaybeApplyPostCorrection.
  // For single-segment conversion, passes up to 5 N-best candidates so that
  // supplemental model works consistently with SUGGESTION mode.
  // TODO(taku): Extend multi-segment conversion to also pass N-best candidates
  // instead of only the single top composite result.
  std::vector<prediction::Result> pc_results =
      ConversionSegmentsToResults(segments);
  if (!request_util::IsHandwriting(conv_req) &&
      modules_->GetSupplementalModel().IsAvailable() &&
      RewriterInterface::DisableLegacyRewriter(
          conv_req, RewriterInterface::kDisableCollocation)) {
    modules_->GetSupplementalModel().PostCorrect(conv_req, pc_results);
  }

  // 2. Run UserHistoryPredictor.
  std::vector<prediction::Result> user_history_results;
  constexpr int kLegacyHistoryMode =
      RewriterInterface::kDisableUserSegmentHistory |
      RewriterInterface::kDisableUserBoundaryHistory;
  if (predictor_ &&
      RewriterInterface::DisableLegacyRewriter(conv_req, kLegacyHistoryMode)) {
    user_history_results = predictor_->Convert(conv_req);
  }

  const bool is_default_multi_segment =
      (segments.conversion_segments_size() > 1);
  const bool enable_multi_segment = request.request()
                                        .decoder_experiment_params()
                                        .enable_multi_segment_candidate();

  // Fail-safe for legacy single-segment resizing (!enable_multi_segment):
  // Older history entries lack inner_segment boundary information, indicated
  // by Attribute::USER_HISTORY_EMPTY_INNER_SEGMENT_BOUNDARY.
  // When enable_multi_segment is false and top_result is a single-segment
  // prediction without boundary info, resizing a multi-segment default Viterbi
  // would corrupt the structure.
  // When enable_multi_segment is true, multi-segment alignment handles
  // single-segment candidates as spanning multi-segment entries without
  // resizing.
  if (!user_history_results.empty()) {
    const prediction::Result& history_top = user_history_results.front();
    const bool is_history_multi_segment =
        (history_top.inner_segment_boundary.size() > 1);
    const bool missing_boundary_error =
        !enable_multi_segment && !is_history_multi_segment &&
        is_default_multi_segment &&
        (history_top.attributes &
         Attribute::USER_HISTORY_EMPTY_INNER_SEGMENT_BOUNDARY);
    if (missing_boundary_error || history_top.key.empty() ||
        history_top.value.empty()) {
      user_history_results.clear();
    }
  }

  // 3. Merge results: user_history_results -> pc_results.
  return MergePredictionResults(std::move(user_history_results),
                                std::move(pc_results));
}

void Converter::PopulatePredictionResultsToSegments(
    const ConversionRequest& request, const prediction::Result& default_result,
    absl::Span<const prediction::Result> results, Segments* segments) const {
  if (results.empty()) {
    return;
  }

  const bool enable_multi_segment = request.request()
                                        .decoder_experiment_params()
                                        .enable_multi_segment_candidate();

  const prediction::Result& top_result = results.front();
  const bool is_default_multi_segment =
      (segments->conversion_segments_size() > 1);
  const bool top_is_same_as_default =
      (top_result.value == default_result.value &&
       top_result.inner_segment_boundary ==
           default_result.inner_segment_boundary);

  if (!enable_multi_segment) {
    // If there is no change in the top candidate and the conversion has
    // multiple segments, do nothing.
    if (top_is_same_as_default && is_default_multi_segment) {
      return;
    }

    // 1. Resize segments if needed to match top_result's inner segment
    // boundaries.
    if (!ResizeSegmentsByResult(request, top_result, segments)) {
      return;
    }

    // 2. Apply results across conversion segments.
    // For single-segment conversion, apply up to 5 results.
    // If the 1st candidate is identical to the default result, apply starting
    // from candidate position 1.
    const bool is_single_segment = (!is_default_multi_segment &&
                                    segments->conversion_segments_size() == 1);
    const size_t start_idx = top_is_same_as_default ? 1 : 0;

    if (is_single_segment) {
      const size_t max_results = std::min<size_t>(results.size(), 5);
      for (size_t i = start_idx; i < max_results; ++i) {
        const prediction::Result& res = results[i];
        if (res.key.empty() || res.value.empty()) continue;
        ApplyResultToSegments(res, /*target_pos=*/i,
                              Attribute::DEFAULT_ATTRIBUTE, segments);
      }
    } else if (!top_is_same_as_default) {
      // Multi-segment conversion: only apply top_result when it modifies
      // default.
      ApplyResultToSegments(top_result, /*target_pos=*/0,
                            Attribute::DEFAULT_ATTRIBUTE, segments);
    }
    return;
  }

  // When enable_multi_segment is true:
  // Do NOT resize segments. Apply up to 5 results with multi-segment alignment.
  const size_t start_idx = top_is_same_as_default ? 1 : 0;
  const size_t max_results = std::min<size_t>(results.size(), 5);
  for (size_t i = start_idx; i < max_results; ++i) {
    ApplyResultToSegmentsMultiSegment(results[i], /*target_pos=*/i, *segments);
  }
}

void Converter::MaybeApplyPredictionToConversion(
    const ConversionRequest& request, Segments* segments) const {
  // NOTE: Skip when `used_in_predictor_realtime_conversion` is true to prevent
  // PostCorrect and UserHistoryPredictor from being called twice per
  // keystroke (once inside RealtimeDecoder's nested StartConversion call and
  // once in DictionaryPredictor).
  // TODO(taku): This is a temporary workaround during the migration period;
  // remove once nested Converter::StartConversion in RealtimeDecoder is
  // deprecated and removed.
  if (segments == nullptr || segments->conversion_segments_size() == 0 ||
      segments->resized() ||
      request.options().used_in_predictor_realtime_conversion) {
    return;
  }

  // 1. Build a single Result from the top candidates of default conversion
  // segments.
  const std::optional<prediction::Result> default_result =
      ConversionSegmentsToResult(segments->conversion_segments());
  if (!default_result.has_value()) {
    return;
  }

  // 2. Generate and merge prediction results from UserHistoryPredictor and
  // SupplementalModel (PostCorrection).
  const std::vector<prediction::Result> results =
      PredictForConversion(request, *segments, *default_result);
  if (results.empty()) {
    return;
  }

  // 3. Populate prediction results into conversion segments (including segment
  // resizing if needed).
  PopulatePredictionResultsToSegments(request, *default_result, results,
                                      segments);
}

void Converter::CompletePosIds(Candidate* candidate) const {
  DCHECK(candidate);
  if (candidate->value.empty() || candidate->key.empty()) {
    return;
  }

  if (candidate->lid != 0 && candidate->rid != 0) {
    return;
  }

  // Use general noun,  unknown word ("サ変") tend to produce
  // "する" "して", which are not always acceptable for non-sahen words.
  candidate->lid = general_noun_id_;
  candidate->rid = general_noun_id_;
  constexpr size_t kExpandSizeStart = 5;
  constexpr size_t kExpandSizeDiff = 50;
  constexpr size_t kExpandSizeMax = 80;
  // In almost all cases, user chooses the top candidate.
  // In order to reduce the latency, first, expand 5 candidates.
  // If no valid candidates are found within 5 candidates, expand
  // candidates step-by-step.
  for (size_t size = kExpandSizeStart; size < kExpandSizeMax;
       size += kExpandSizeDiff) {
    Segments segments;
    segments.InitForConvert(candidate->key);
    // use PREDICTION mode, as the size of segments after
    // PREDICTION mode is always 1, thanks to real time conversion.
    // However, PREDICTION mode produces "predictions", meaning
    // that keys of result candidate are not always the same as
    // query key. It would be nice to have PREDICTION_REALTIME_CONVERSION_ONLY.
    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetOptions({
                .request_type = ConversionRequest::PREDICTION,
                .max_conversion_candidates_size = static_cast<int>(size),
            })
            .Build();
    // In order to complete PosIds, call ImmutableConverter again.
    if (!immutable_converter_->Convert(request.options(), &segments)) {
      LOG(ERROR) << "ImmutableConverter::Convert() failed";
      return;
    }
    for (size_t i = 0; i < segments.segment(0).candidates_size(); ++i) {
      const Candidate& ref_candidate = segments.segment(0).candidate(i);
      if (ref_candidate.value == candidate->value) {
        candidate->lid = ref_candidate.lid;
        candidate->rid = ref_candidate.rid;
        candidate->cost = ref_candidate.cost;
        candidate->wcost = ref_candidate.wcost;
        candidate->structure_cost = ref_candidate.structure_cost;
        MOZC_VLOG(1) << "Set LID: " << candidate->lid;
        MOZC_VLOG(1) << "Set RID: " << candidate->rid;
        return;
      }
    }
  }
  MOZC_DVLOG(2) << "Cannot set lid/rid. use default value. "
                << "key: " << candidate->key << ", "
                << "value: " << candidate->value << ", "
                << "lid: " << candidate->lid << ", "
                << "rid: " << candidate->rid;
}

void Converter::RewriteAndSuppressCandidates(const ConversionRequest& request,
                                             Segments* segments) const {
  // 1. Resize segments if needed.
  // Check if the segments need to be resized.
  if (std::optional<RewriterInterface::ResizeSegmentsRequest> resize_request =
          rewriter_->CheckResizeSegmentsRequest(request, *segments);
      resize_request.has_value()) {
    if (ResizeSegments(segments, request, resize_request->segment_index,
                       resize_request->segment_sizes)) {
      // If the segments are resized, ResizeSegments recursively executed
      // RewriteAndSuppressCandidates with resized segments. No need to execute
      // them again.
      // TODO(b/381537649): Stop using the recursive call of
      // RewriteAndSuppressCandidates.
      return;
    }
  }

  // 2. Rewrite candidates in each segment.
  // Suppression below is a final converter invariant and must not depend on
  // whether the rewriter chain reports that it modified the segments.
  rewriter_->Rewrite(request, segments);

  // 3. Suppress candidates in each segment.
  // Although the suppression dictionary is applied at node-level in dictionary
  // layer, there's possibility that bad words are generated from multiple nodes
  // and by rewriters. Hence, apply it again at the last stage of converter when
  // suppression entries exist.
  if (user_dictionary_.HasSuppressedEntries()) {
    for (Segment& segment : segments->conversion_segments()) {
      for (size_t j = 0; j < segment.candidates_size();) {
        const Candidate& cand = segment.candidate(j);
        if (user_dictionary_.IsSuppressedEntry(cand.key, cand.value)) {
          segment.erase_candidate(j);
        } else {
          ++j;
        }
      }
    }
  }

  // 4. Hard final-ranking invariants must run after every ordinary rewriter,
  // history reranking path, prediction-to-conversion merge, and suppression.
  EnforceCandidateRankingConstraints(request, segments);
}

void Converter::TrimCandidates(const ConversionRequest& request,
                               Segments* segments) const {
  const mozc::commands::Request& request_proto = request.request();
  if (!request_proto.has_candidates_size_limit()) {
    return;
  }

  const int limit = request_proto.candidates_size_limit();
  for (Segment& segment : segments->conversion_segments()) {
    const int candidates_size = segment.candidates_size();
    // A segment should have at least one candidate.
    const int candidates_limit =
        std::max<int>(1, limit - segment.meta_candidates_size());
    if (candidates_size < candidates_limit) {
      continue;
    }
    segment.erase_candidates(candidates_limit,
                             candidates_size - candidates_limit);
  }
}

bool Converter::Reload() {
  modules().GetUserDictionary().Reload();
  return rewriter().Reload() && predictor().Reload();
}

bool Converter::Sync() { return rewriter().Sync() && predictor().Sync(); }

bool Converter::Wait() {
  modules().GetUserDictionary().WaitForReloader();
  return predictor().Wait();
}

bool Converter::AddUserHistory(absl::string_view key, absl::string_view value) {
  value = absl::StripAsciiWhitespace(value);
  key = absl::StripAsciiWhitespace(key);

  static constexpr int kMaxValueSize = 128;
  if (value.empty() || value.size() >= kMaxValueSize) {
    return false;
  }

  // When key is empty, use reverse conversion.
  std::string reading;
  if (key.empty()) {
    auto result = GetReading(value, /* multi_segment */ true);
    if (!result.has_value()) return false;
    reading = std::move(result.value());
    key = reading;
  }

  return predictor().AddHistoryEntry(key, value);
}

std::optional<std::string> Converter::GetReading(absl::string_view text,
                                                 bool multi_segment) const {
  Segments segments;
  if (!StartReverseConversion(&segments, text)) {
    LOG(ERROR) << "Reverse conversion failed to get the reading of " << text;
    return std::nullopt;
  }

  if (segments.conversion_segments_size() == 0 ||
      (!multi_segment && segments.conversion_segments_size() > 1)) {
    LOG(ERROR) << "Reverse conversion returned an invalid result for " << text;
    return std::nullopt;
  }

  std::string reading;
  for (const Segment& segment : segments) {
    if (segment.candidates_size() == 0) return std::nullopt;
    absl::StrAppend(&reading, segment.candidate(0).value);
  }

  return reading;
}

void Converter::PopulateReadingOfCommittedCandidateIfMissing(
    Segments* segments) const {
  if (segments->conversion_segments_size() == 0) return;

  Segment* segment = segments->mutable_conversion_segment(0);
  if (segment->candidates_size() == 0) return;

  Candidate* cand = segment->mutable_candidate(0);
  if (!cand->key.empty() || cand->value.empty()) return;

  if (cand->content_value == cand->value) {
    if (std::optional<std::string> key = GetReading(cand->value);
        key.has_value()) {
      cand->key = *key;
      cand->content_key = *std::move(key);
    }
    return;
  }

  if (cand->content_value.empty()) {
    LOG(ERROR) << "Content value is empty: " << *cand;
    return;
  }

  const absl::string_view functional_value = cand->functional_value();
  if (Util::GetScriptType(functional_value) != Util::HIRAGANA) {
    LOG(ERROR) << "The functional value is not hiragana: " << *cand;
    return;
  }
  if (std::optional<std::string> content_key = GetReading(cand->content_value);
      content_key.has_value()) {
    cand->key = absl::StrCat(*content_key, functional_value);
    cand->content_key = *std::move(content_key);
  }
}

bool Converter::PredictForRequestWithSegments(const ConversionRequest& request,
                                              Segments* segments) const {
  DCHECK(segments);
  DCHECK(predictor_);

  const prediction::Result history_result = HistorySegmentsToResult(*segments);

  const ConversionRequest conv_req = ConversionRequestBuilder()
                                         .SetConversionRequestView(request)
                                         .SetHistoryResultView(history_result)
                                         .Build();

  std::vector<prediction::Result> results = predictor_->Predict(conv_req);

  Segment* segment = segments->mutable_conversion_segment(0);
  DCHECK(segment);

  for (prediction::Result& result : results) {
    PopulateCandidateFromResult(std::move(result), segment->add_candidate());
  }

  return !results.empty();
}

}  // namespace converter
}  // namespace mozc
