// Copyright 2010-2017, Google Inc.
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

#include "converter/immutable_converter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/port.h"
#include "base/string_piece.h"
#include "base/system_util.h"
#include "base/util.h"
#include "config/config_handler.h"
#include "converter/connector.h"
#include "converter/lattice.h"
#include "converter/segmenter.h"
#include "converter/segments.h"
#include "data_manager/data_manager_interface.h"
#include "data_manager/testing/mock_data_manager.h"
#include "dictionary/dictionary_impl.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_group.h"
#include "dictionary/suffix_dictionary.h"
#include "dictionary/suppression_dictionary.h"
#include "dictionary/system/system_dictionary.h"
#include "dictionary/system/value_dictionary.h"
#include "dictionary/user_dictionary_stub.h"
#include "prediction/suggestion_filter.h"
#include "protocol/commands.pb.h"
#include "request/conversion_request.h"
#include "testing/base/public/googletest.h"
#include "testing/base/public/gunit.h"

using mozc::dictionary::DictionaryImpl;
using mozc::dictionary::DictionaryInterface;
using mozc::dictionary::POSMatcher;
using mozc::dictionary::PosGroup;
using mozc::dictionary::SuffixDictionary;
using mozc::dictionary::SuppressionDictionary;
using mozc::dictionary::SystemDictionary;
using mozc::dictionary::UserDictionaryStub;
using mozc::dictionary::ValueDictionary;

namespace mozc {
namespace {

void SetCandidate(const string &key, const string &value, Segment *segment) {
  segment->set_key(key);
  Segment::Candidate *candidate = segment->add_candidate();
  candidate->Init();
  candidate->key = key;
  candidate->value = value;
  candidate->content_key = key;
  candidate->content_value = value;
}

class MockDataAndImmutableConverter {
 public:
  // Initializes data and immutable converter with given dictionaries. If NULL
  // is passed, the default mock dictionary is used. This class owns the first
  // argument dictionary but doesn't the second because the same dictionary may
  // be passed to the arguments.
  explicit MockDataAndImmutableConverter(
      const DictionaryInterface *dictionary = NULL,
      const DictionaryInterface *suffix_dictionary = NULL) {
    data_manager_.reset(new testing::MockDataManager);

    pos_matcher_.Set(data_manager_->GetPOSMatcherData());

    suppression_dictionary_.reset(new SuppressionDictionary);
    CHECK(suppression_dictionary_.get());

    if (dictionary) {
      dictionary_.reset(dictionary);
    } else {
      const char *dictionary_data = NULL;
      int dictionary_size = 0;
      data_manager_->GetSystemDictionaryData(&dictionary_data,
                                             &dictionary_size);
      SystemDictionary *sysdic =
          SystemDictionary::Builder(dictionary_data, dictionary_size).Build();
      dictionary_.reset(new DictionaryImpl(
          sysdic,  // DictionaryImpl takes the ownership
          new ValueDictionary(pos_matcher_, &sysdic->value_trie()),
          &user_dictionary_stub_,
          suppression_dictionary_.get(),
          &pos_matcher_));
    }
    CHECK(dictionary_.get());

    if (!suffix_dictionary) {
      StringPiece suffix_key_array_data, suffix_value_array_data;
      const uint32 *token_array;
      data_manager_->GetSuffixDictionaryData(&suffix_key_array_data,
                                             &suffix_value_array_data,
                                             &token_array);
      suffix_dictionary_.reset(new SuffixDictionary(suffix_key_array_data,
                                                    suffix_value_array_data,
                                                    token_array));
      suffix_dictionary = suffix_dictionary_.get();
    }
    CHECK(suffix_dictionary);

    connector_.reset(Connector::CreateFromDataManager(*data_manager_));
    CHECK(connector_.get());

    segmenter_.reset(Segmenter::CreateFromDataManager(*data_manager_));
    CHECK(segmenter_.get());

    pos_group_.reset(new PosGroup(data_manager_->GetPosGroupData()));
    CHECK(pos_group_.get());

    {
      const char *data = NULL;
      size_t size = 0;
      data_manager_->GetSuggestionFilterData(&data, &size);
      suggestion_filter_.reset(new SuggestionFilter(data, size));
    }

    immutable_converter_.reset(new ImmutableConverterImpl(
        dictionary_.get(),
        suffix_dictionary,
        suppression_dictionary_.get(),
        connector_.get(),
        segmenter_.get(),
        &pos_matcher_,
        pos_group_.get(),
        suggestion_filter_.get()));
    CHECK(immutable_converter_.get());
  }

  ImmutableConverterImpl *GetConverter() {
    return immutable_converter_.get();
  }

 private:
  std::unique_ptr<const DataManagerInterface> data_manager_;
  std::unique_ptr<const SuppressionDictionary> suppression_dictionary_;
  std::unique_ptr<const Connector> connector_;
  std::unique_ptr<const Segmenter> segmenter_;
  std::unique_ptr<const DictionaryInterface> suffix_dictionary_;
  std::unique_ptr<const DictionaryInterface> dictionary_;
  std::unique_ptr<const PosGroup> pos_group_;
  std::unique_ptr<const SuggestionFilter> suggestion_filter_;
  std::unique_ptr<ImmutableConverterImpl> immutable_converter_;
  UserDictionaryStub user_dictionary_stub_;
  dictionary::POSMatcher pos_matcher_;
};

}  // namespace

TEST(ImmutableConverterTest, KeepKeyForPrediction) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  segments.set_max_prediction_candidates_size(10);
  Segment *segment = segments.add_segment();
  // "よろしくおねがいしま"
  const string kRequestKey =
      "\xe3\x82\x88\xe3\x82\x8d\xe3\x81\x97\xe3\x81\x8f\xe3\x81\x8a"
      "\xe3\x81\xad\xe3\x81\x8c\xe3\x81\x84\xe3\x81\x97\xe3\x81\xbe";
  segment->set_key(kRequestKey);
  EXPECT_TRUE(data_and_converter->GetConverter()->Convert(&segments));
  EXPECT_EQ(1, segments.segments_size());
  EXPECT_GT(segments.segment(0).candidates_size(), 0);
  EXPECT_EQ(kRequestKey, segments.segment(0).key());
}

TEST(ImmutableConverterTest, DummyCandidatesCost) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segment segment;
  // "てすと"
  SetCandidate("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8", "test", &segment);
  data_and_converter->GetConverter()->InsertDummyCandidates(&segment, 10);
  EXPECT_GE(segment.candidates_size(), 3);
  EXPECT_LT(segment.candidate(0).wcost, segment.candidate(1).wcost);
  EXPECT_LT(segment.candidate(0).wcost, segment.candidate(2).wcost);
}

TEST(ImmutableConverterTest, DummyCandidatesInnerSegmentBoundary) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segment segment;
  // "てすと"
  SetCandidate("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8", "test", &segment);
  Segment::Candidate *c = segment.mutable_candidate(0);
  c->PushBackInnerSegmentBoundary(3, 2, 3, 2);
  c->PushBackInnerSegmentBoundary(6, 2, 6, 2);
  EXPECT_TRUE(c->IsValid());

  data_and_converter->GetConverter()->InsertDummyCandidates(&segment, 10);
  ASSERT_GE(segment.candidates_size(), 3);
  for (size_t i = 1; i < 3; ++i) {
    EXPECT_TRUE(segment.candidate(i).inner_segment_boundary.empty());
    EXPECT_TRUE(segment.candidate(i).IsValid());
  }
}

namespace {
class KeyCheckDictionary : public DictionaryInterface {
 public:
  explicit KeyCheckDictionary(const string &query)
      : target_query_(query), received_target_query_(false) {}
  virtual ~KeyCheckDictionary() {}

  virtual bool HasKey(StringPiece key) const { return false; }
  virtual bool HasValue(StringPiece value) const { return false; }

  virtual void LookupPredictive(
      StringPiece key,
      const ConversionRequest &convreq,
      Callback *callback) const {
    if (key == target_query_) {
      received_target_query_ = true;
    }
  }

  virtual void LookupPrefix(
      StringPiece key,
      const ConversionRequest &convreq,
      Callback *callback) const {
    // No check
  }

  virtual void LookupExact(StringPiece key,
                           const ConversionRequest &convreq,
                           Callback *callback) const {
    // No check
  }

  virtual void LookupReverse(StringPiece str,
                             const ConversionRequest &convreq,
                             Callback *callback) const {
    // No check
  }

  bool received_target_query() const {
    return received_target_query_;
  }

 private:
  const string target_query_;
  mutable bool received_target_query_;
};
}  // namespace

TEST(ImmutableConverterTest, PredictiveNodesOnlyForConversionKey) {
  Segments segments;
  {
    Segment *segment = segments.add_segment();
    // "いいんじゃな"
    segment->set_key("\xe3\x81\x84\xe3\x81\x84\xe3\x82\x93\xe3\x81\x98"
                     "\xe3\x82\x83\xe3\x81\xaa");
    segment->set_segment_type(Segment::HISTORY);
    Segment::Candidate *candidate = segment->add_candidate();
    candidate->Init();
    // "いいんじゃな"
    candidate->key =
        "\xe3\x81\x84\xe3\x81\x84\xe3\x82\x93\xe3\x81\x98"
        "\xe3\x82\x83\xe3\x81\xaa";
    // "いいんじゃな"
    candidate->value =
        "\xe3\x81\x84\xe3\x81\x84\xe3\x82\x93\xe3\x81\x98"
        "\xe3\x82\x83\xe3\x81\xaa";

    segment = segments.add_segment();
    // "いか"
    segment->set_key("\xe3\x81\x84\xe3\x81\x8b");

    EXPECT_EQ(1, segments.history_segments_size());
    EXPECT_EQ(1, segments.conversion_segments_size());
  }

  Lattice lattice;
  // "いいんじゃないか"
  lattice.SetKey("\xe3\x81\x84\xe3\x81\x84\xe3\x82\x93\xe3\x81\x98"
                 "\xe3\x82\x83\xe3\x81\xaa\xe3\x81\x84\xe3\x81\x8b");

  // "ないか"
  KeyCheckDictionary *dictionary =
      new KeyCheckDictionary("\xe3\x81\xaa\xe3\x81\x84\xe3\x81\x8b");
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter(dictionary, dictionary));
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();
  const ConversionRequest request;
  converter->MakeLatticeNodesForPredictiveNodes(segments, request, &lattice);
  EXPECT_FALSE(dictionary->received_target_query());
}

TEST(ImmutableConverterTest, AddPredictiveNodes) {
  Segments segments;
  {
    Segment *segment = segments.add_segment();
    // "よろしくおねがいしま"
    segment->set_key("\xe3\x82\x88\xe3\x82\x8d\xe3\x81\x97\xe3\x81\x8f"
                     "\xe3\x81\x8a\xe3\x81\xad\xe3\x81\x8c\xe3\x81\x84"
                     "\xe3\x81\x97\xe3\x81\xbe");

    EXPECT_EQ(1, segments.conversion_segments_size());
  }

  Lattice lattice;
  // "よろしくおねがいしま"
  lattice.SetKey("\xe3\x82\x88\xe3\x82\x8d\xe3\x81\x97\xe3\x81\x8f"
                 "\xe3\x81\x8a\xe3\x81\xad\xe3\x81\x8c\xe3\x81\x84"
                 "\xe3\x81\x97\xe3\x81\xbe");

  // "しま"
  KeyCheckDictionary *dictionary =
      new KeyCheckDictionary("\xe3\x81\x97\xe3\x81\xbe");
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter(dictionary, dictionary));
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();
  const ConversionRequest request;
  converter->MakeLatticeNodesForPredictiveNodes(segments, request, &lattice);
  EXPECT_TRUE(dictionary->received_target_query());
}

TEST(ImmutableConverterTest, InnerSegmenBoundaryForPrediction) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  segments.set_max_prediction_candidates_size(1);
  Segment *segment = segments.add_segment();
  const string kRequestKey =
      // "わたしのなまえはなかのです"
      "\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae\xe3\x81\xaa\xe3"
      "\x81\xbe\xe3\x81\x88\xe3\x81\xaf\xe3\x81\xaa\xe3\x81\x8b\xe3\x81"
      "\xae\xe3\x81\xa7\xe3\x81\x99";
  segment->set_key(kRequestKey);
  EXPECT_TRUE(data_and_converter->GetConverter()->Convert(&segments));
  ASSERT_EQ(1, segments.segments_size());
  ASSERT_EQ(1, segments.segment(0).candidates_size());

  // Result will be, "私の|名前は|中ノです" with mock dictionary.
  const Segment::Candidate &cand = segments.segment(0).candidate(0);
  std::vector<StringPiece> keys, values, content_keys, content_values;
  for (Segment::Candidate::InnerSegmentIterator iter(&cand);
       !iter.Done(); iter.Next()) {
    keys.push_back(iter.GetKey());
    values.push_back(iter.GetValue());
    content_keys.push_back(iter.GetContentKey());
    content_values.push_back(iter.GetContentValue());
  }
  ASSERT_EQ(3, keys.size());
  // "わたしの" | "なまえは" | "なかのです"
  EXPECT_EQ("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae", keys[0]);
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88\xe3\x81\xaf", keys[1]);
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xae\xe3\x81\xa7\xe3\x81\x99",
            keys[2]);

  // "私の" | "名前は" | "中ノです"
  ASSERT_EQ(3, values.size());
  EXPECT_EQ("\xe7\xa7\x81\xe3\x81\xae", values[0]);
  EXPECT_EQ("\xe5\x90\x8d\xe5\x89\x8d\xe3\x81\xaf", values[1]);
  EXPECT_EQ("\xe4\xb8\xad\xe3\x83\x8e\xe3\x81\xa7\xe3\x81\x99", values[2]);

  ASSERT_EQ(3, content_keys.size());
  // "わたし" | "なまえ" | "なかの"
  EXPECT_EQ("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97", content_keys[0]);
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88", content_keys[1]);
  EXPECT_EQ("\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xae", content_keys[2]);

  // "私" | "名前" | "中ノ"
  ASSERT_EQ(3, content_values.size());
  EXPECT_EQ("\xe7\xa7\x81", content_values[0]);
  EXPECT_EQ("\xe5\x90\x8d\xe5\x89\x8d", content_values[1]);
  EXPECT_EQ("\xe4\xb8\xad\xe3\x83\x8e", content_values[2]);
}

TEST(ImmutableConverterTest, NoInnerSegmenBoundaryForConversion) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segments segments;
  segments.set_request_type(Segments::CONVERSION);
  Segment *segment = segments.add_segment();
  const string kRequestKey =
      // "わたしのなまえはなかのです"
      "\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae\xe3\x81\xaa\xe3"
      "\x81\xbe\xe3\x81\x88\xe3\x81\xaf\xe3\x81\xaa\xe3\x81\x8b\xe3\x81"
      "\xae\xe3\x81\xa7\xe3\x81\x99";
  segment->set_key(kRequestKey);
  EXPECT_TRUE(data_and_converter->GetConverter()->Convert(&segments));
  EXPECT_LE(1, segments.segments_size());
  EXPECT_LT(0, segments.segment(0).candidates_size());
  for (size_t i = 0; i < segments.segment(0).candidates_size(); ++i) {
    const Segment::Candidate &cand = segments.segment(0).candidate(i);
    EXPECT_TRUE(cand.inner_segment_boundary.empty());
  }
}

TEST(ImmutableConverterTest, NotConnectedTest) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  ImmutableConverterImpl *converter = data_and_converter->GetConverter();

  Segments segments;
  segments.set_request_type(Segments::CONVERSION);

  Segment *segment = segments.add_segment();
  segment->set_segment_type(Segment::FIXED_BOUNDARY);
  // "しょうめい"
  segment->set_key(
      "\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86\xe3\x82\x81\xe3\x81\x84");

  segment = segments.add_segment();
  segment->set_segment_type(Segment::FREE);
  // "できる"
  segment->set_key("\xe3\x81\xa7\xe3\x81\x8d\xe3\x82\x8b");

  Lattice lattice;
  lattice.SetKey(
      // "しょうめいできる"
      "\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86\xe3\x82\x81\xe3\x81\x84"
      "\xe3\x81\xa7\xe3\x81\x8d\xe3\x82\x8b");
  const ConversionRequest request;
  converter->MakeLattice(request, &segments, &lattice);

  std::vector<uint16> group;
  converter->MakeGroup(segments, &group);
  converter->Viterbi(segments, &lattice);

  // Intentionally segmented position - 1
  // "しょうめ"
  const size_t pos = strlen(
      "\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86\xe3\x82\x81");
  bool tested = false;
  for (Node *rnode = lattice.begin_nodes(pos);
       rnode != NULL; rnode = rnode->bnext) {
    if (Util::CharsLen(rnode->key) <= 1) {
      continue;
    }
    // If len(rnode->value) > 1, that node should cross over the boundary
    EXPECT_TRUE(rnode->prev == NULL);
    tested = true;
  }
  EXPECT_TRUE(tested);
}

TEST(ImmutableConverterTest, HistoryKeyLengthIsVeryLong) {
  // "あ..." (100 times)
  const string kA100 =
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
      "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82";

  // Set up history segments.
  Segments segments;
  for (int i = 0; i < 4; ++i) {
    Segment *segment = segments.add_segment();
    segment->set_key(kA100);
    segment->set_segment_type(Segment::HISTORY);
    Segment::Candidate *candidate = segment->add_candidate();
    candidate->Init();
    candidate->key = kA100;
    candidate->value = kA100;
  }

  // Set up a conversion segment.
  segments.set_request_type(Segments::CONVERSION);
  Segment *segment = segments.add_segment();
  const string kRequestKey = "\xE3\x81\x82";  // "あ"
  segment->set_key(kRequestKey);

  // Verify that history segments are cleared due to its length limit and at
  // least one candidate is generated.
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  EXPECT_TRUE(data_and_converter->GetConverter()->Convert(&segments));
  EXPECT_EQ(0, segments.history_segments_size());
  ASSERT_EQ(1, segments.conversion_segments_size());
  EXPECT_GT(segments.segment(0).candidates_size(), 0);
  EXPECT_EQ(kRequestKey, segments.segment(0).key());
}

namespace {
bool AutoPartialSuggestionTestHelper(const ConversionRequest &request) {
  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  segments.set_max_prediction_candidates_size(10);
  Segment *segment = segments.add_segment();
  const string kRequestKey =
      // "わたしのなまえはなかのです"
      "\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae\xe3\x81\xaa\xe3"
      "\x81\xbe\xe3\x81\x88\xe3\x81\xaf\xe3\x81\xaa\xe3\x81\x8b\xe3\x81"
      "\xae\xe3\x81\xa7\xe3\x81\x99";
  segment->set_key(kRequestKey);
  EXPECT_TRUE(data_and_converter->GetConverter()->ConvertForRequest(
      request, &segments));
  EXPECT_EQ(1, segments.conversion_segments_size());
  EXPECT_LT(0, segments.segment(0).candidates_size());
  bool includes_only_first = false;
  const string &segment_key = segments.segment(0).key();
  for (size_t i = 0; i < segments.segment(0).candidates_size(); ++i) {
    const Segment::Candidate &cand = segments.segment(0).candidate(i);
    if (cand.key.size() < segment_key.size() &&
        Util::StartsWith(segment_key, cand.key)) {
      includes_only_first = true;
      break;
    }
  }
  return includes_only_first;
}
}  // namespace

TEST(ImmutableConverterTest, EnableAutoPartialSuggestion) {
  const commands::Request request;
  ConversionRequest conversion_request;
  conversion_request.set_request(&request);
  conversion_request.set_create_partial_candidates(true);

  EXPECT_TRUE(AutoPartialSuggestionTestHelper(conversion_request));
}

TEST(ImmutableConverterTest, DisableAutoPartialSuggestion) {
  const commands::Request request;
  ConversionRequest conversion_request;
  conversion_request.set_request(&request);
  conversion_request.set_create_partial_candidates(false);

  EXPECT_FALSE(AutoPartialSuggestionTestHelper(conversion_request));
}

TEST(ImmutableConverterTest, AutoPartialSuggestionDefault) {
  const commands::Request request;
  ConversionRequest conversion_request;
  conversion_request.set_request(&request);

  EXPECT_FALSE(AutoPartialSuggestionTestHelper(conversion_request));
}

TEST(ImmutableConverterTest, AutoPartialSuggestionForSingleSegment) {
  const commands::Request request;
  ConversionRequest conversion_request;
  conversion_request.set_request(&request);
  conversion_request.set_create_partial_candidates(true);

  std::unique_ptr<MockDataAndImmutableConverter> data_and_converter(
      new MockDataAndImmutableConverter);
  const string kRequestKeys[] = {
      // "たかまち"
      "\xE3\x81\x9F\xE3\x81\x8B\xE3\x81\xBE\xE3\x81\xA1",
      // "なのは"
      "\xE3\x81\xAA\xE3\x81\xAE\xE3\x81\xAF",
      // "まほうしょうじょ"
      "\xE3\x81\xBE\xE3\x81\xBB\xE3\x81\x86\xE3\x81\x97"
      "\xE3\x82\x87\xE3\x81\x86\xE3\x81\x98\xE3\x82\x87",
  };
  for (size_t testcase = 0; testcase < arraysize(kRequestKeys); ++testcase) {
    Segments segments;
    segments.set_request_type(Segments::PREDICTION);
    segments.set_max_prediction_candidates_size(10);
    Segment *segment = segments.add_segment();
    segment->set_key(kRequestKeys[testcase]);
    EXPECT_TRUE(data_and_converter->GetConverter()->
                    ConvertForRequest(conversion_request, &segments));
    EXPECT_EQ(1, segments.conversion_segments_size());
    EXPECT_LT(0, segments.segment(0).candidates_size());
    const string &segment_key = segments.segment(0).key();
    for (size_t i = 0; i < segments.segment(0).candidates_size(); ++i) {
      const Segment::Candidate &cand = segments.segment(0).candidate(i);
      if (cand.attributes & Segment::Candidate::PARTIALLY_KEY_CONSUMED) {
        EXPECT_LT(cand.key.size(), segment_key.size()) << cand.DebugString();
      } else {
        EXPECT_GE(cand.key.size(), segment_key.size()) << cand.DebugString();
      }
    }
  }
}

}  // namespace mozc
