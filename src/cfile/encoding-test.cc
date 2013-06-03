// Copyright (c) 2012, Cloudera, inc.

#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/utility/binary.hpp>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <stdlib.h>

#include "cfile/block_encodings.h"
#include "cfile/cfile.h"
#include "cfile/gvint_block.h"
#include "cfile/string_plain_block.h"
#include "cfile/string_prefix_block.h"
#include "common/columnblock.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/stringprintf.h"
#include "util/hexdump.h"
#include "util/memory/arena.h"
#include "util/test_macros.h"
#include "util/stopwatch.h"

namespace kudu { namespace cfile {

extern void DumpSSETable();

class TestEncoding : public ::testing::Test {
public:
  TestEncoding() :
    ::testing::Test(),
    arena_(1024, 1024*1024)
  {
  }

protected:
  virtual void SetUp() {
    arena_.Reset();
  }

  template<DataType type>
  void CopyOne(BlockDecoder *decoder,
               typename TypeTraits<type>::cpp_type *ret) {
    ColumnBlock cb(GetTypeInfo(type), ret, 1, &arena_);
    size_t n = 1;
    ASSERT_STATUS_OK(decoder->CopyNextValues(&n, &cb));
    ASSERT_EQ(1, n);
  }

  // Insert a given number of strings into the provided
  // StringPrefixBlockBuilder.
  template<class BuilderType>
  static Slice CreateStringBlock(BuilderType *sbb,
                                 int num_items,
                                 const char *fmt_str) {
    boost::ptr_vector<string> to_insert;
    std::vector<Slice> slices;

    for (uint i = 0; i < num_items; i++) {
      string *val = new string(StringPrintf(fmt_str, i));
      to_insert.push_back(val);
      slices.push_back(Slice(*val));
    }


    int rem = slices.size();
    Slice *ptr = &slices[0];
    while (rem > 0) {
      int added = sbb->Add(reinterpret_cast<const uint8_t *>(ptr),
                           rem);
      CHECK(added > 0);
      rem -= added;
      ptr += added;
    }

    CHECK_EQ(slices.size(), sbb->Count());
    return sbb->Finish(12345L);
  }

  template<class BuilderType, class DecoderType>
  void TestStringSeekByValueSmallBlock() {
    WriterOptions opts;
    BuilderType sbb(&opts);
    // Insert "hello 0" through "hello 9"
    const uint kCount = 10;
    Slice s = CreateStringBlock(&sbb, kCount, "hello %d");
    DecoderType sbd(s);
    ASSERT_STATUS_OK(sbd.ParseHeader());

    // Seeking to just after a key should return the
    // next key ('hello 4x' falls between 'hello 4' and 'hello 5')
    Slice q = "hello 4x";
    bool exact;
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    ASSERT_FALSE(exact);

    Slice ret;
    ASSERT_EQ(12345 + 5u, sbd.ordinal_pos());
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 5"), ret.ToString());

    sbd.SeekToPositionInBlock(0);

    // Seeking to an exact key should return that key
    q = "hello 4";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    ASSERT_EQ(12345 + 4u, sbd.ordinal_pos());
    ASSERT_TRUE(exact);
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 4"), ret.ToString());

    // Seeking to before the first key should return first key
    q = "hello";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    ASSERT_EQ(12345, sbd.ordinal_pos());
    ASSERT_FALSE(exact);
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 0"), ret.ToString());

    // Seeking after the last key should return not found
    q = "zzzz";
    ASSERT_TRUE(sbd.SeekAtOrAfterValue(&q, &exact).IsNotFound());

    // Seeking to the last key should succeed
    q = "hello 9";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    ASSERT_EQ(12345 + 9u, sbd.ordinal_pos());
    ASSERT_TRUE(exact);
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 9"), ret.ToString());
  }

  template<class BuilderType, class DecoderType>
  void TestStringSeekByValueLargeBlock() {
    Arena arena(1024, 1024*1024); // TODO: move to fixture?
    WriterOptions opts;
    StringPrefixBlockBuilder sbb(&opts);
    const uint kCount = 1000;
    // Insert 'hello 000' through 'hello 999'
    Slice s = CreateStringBlock(&sbb, kCount, "hello %03d");
    StringPrefixBlockDecoder sbd(s);
    ASSERT_STATUS_OK(sbd.ParseHeader());

    // Seeking to just after a key should return the
    // next key ('hello 444x' falls between 'hello 444' and 'hello 445')
    Slice q = "hello 444x";
    bool exact;
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    ASSERT_FALSE(exact);

    Slice ret;
    ASSERT_EQ(12345 + 445u, sbd.ordinal_pos());
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 445"), ret.ToString());

    sbd.SeekToPositionInBlock(0);

    // Seeking to an exact key should return that key
    q = "hello 004";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    EXPECT_TRUE(exact);
    EXPECT_EQ(12345 + 4u, sbd.ordinal_pos());
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 004"), ret.ToString());

    // Seeking to before the first key should return first key
    q = "hello";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    EXPECT_FALSE(exact);
    EXPECT_EQ(12345, sbd.ordinal_pos());
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 000"), ret.ToString());

    // Seeking after the last key should return not found
    q = "zzzz";
    ASSERT_TRUE(sbd.SeekAtOrAfterValue(&q, &exact).IsNotFound());

    // Seeking to the last key should succeed
    q = "hello 999";
    ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
    EXPECT_TRUE(exact);
    EXPECT_EQ(12345 + 999u, sbd.ordinal_pos());
    CopyOne<STRING>(&sbd, &ret);
    ASSERT_EQ(string("hello 999"), ret.ToString());

    // Randomized seek
    char target[20];
    char before_target[20];
    for (int i = 0; i < 1000; i++) {
      int ord = random() % kCount;
      int len = snprintf(target, sizeof(target), "hello %03d", ord);
      q = Slice(target, len);

      ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
      EXPECT_TRUE(exact);
      EXPECT_EQ(12345u + ord, sbd.ordinal_pos());
      CopyOne<STRING>(&sbd, &ret);
      ASSERT_EQ(string(target), ret.ToString());

      // Seek before this key
      len = snprintf(before_target, sizeof(target), "hello %03d.before", ord-1);
      q = Slice(before_target, len);
      ASSERT_STATUS_OK(sbd.SeekAtOrAfterValue(&q, &exact));
      EXPECT_FALSE(exact);
      EXPECT_EQ(12345u + ord, sbd.ordinal_pos());
      CopyOne<STRING>(&sbd, &ret);
      ASSERT_EQ(string(target), ret.ToString());
    }
  }

  template<class BuilderType, class DecoderType>
  void TestStringBlockRoundTrip() {
    WriterOptions opts;
    BuilderType sbb(&opts);
    const uint kCount = 10;
    Slice s = CreateStringBlock(&sbb, kCount, "hello %d");

    LOG(INFO) << "Block: " << HexDump(s);

    // the slice should take at least a few bytes per entry
    ASSERT_GT(s.size(), kCount * 2u);

    DecoderType sbd(s);
    ASSERT_STATUS_OK(sbd.ParseHeader());
    ASSERT_EQ(kCount, sbd.Count());
    ASSERT_EQ(12345u, sbd.ordinal_pos());
    ASSERT_TRUE(sbd.HasNext());

    // Iterate one by one through data, verifying that it matches
    // what we put in.
    for (uint i = 0; i < kCount; i++) {
      ASSERT_EQ(12345u + i, sbd.ordinal_pos());
      ASSERT_TRUE(sbd.HasNext()) << "Failed on iter " << i;
      Slice s;
      CopyOne<STRING>(&sbd, &s);
      string expected = StringPrintf("hello %d", i);
      ASSERT_EQ(expected, s.ToString()) << "failed at iter " << i;
    }
    ASSERT_FALSE(sbd.HasNext());

    // Now iterate backwards using positional seeking
    for (int i = kCount - 1; i >= 0; i--) {
      sbd.SeekToPositionInBlock(i);
      ASSERT_EQ(12345u + i, sbd.ordinal_pos());
    }

    // Try to request a bunch of data in one go
    ScopedColumnBlock<STRING> cb(kCount + 10);
    sbd.SeekToPositionInBlock(0);
    size_t n = kCount + 10;
    ASSERT_STATUS_OK(sbd.CopyNextValues(&n, &cb));
    ASSERT_EQ(kCount, n);
    ASSERT_FALSE(sbd.HasNext());

    for (uint i = 0; i < kCount; i++) {
      string expected = StringPrintf("hello %d", i);
      ASSERT_EQ(expected, cb[i].ToString());
    }
  }

  void DoSeekTest(int num_ints, int num_queries, bool verify) {
    // Don't start the ints at 0, in order to check that we properly
    // deal with the frame-of-reference.
    const uint32_t kBase = 6;

    uint32_t data[num_ints];
    for (uint32_t i = 0; i < num_ints; i++) {
      data[i] = kBase + i * 2;
    }

    gscoped_ptr<WriterOptions> opts(new WriterOptions());
    GVIntBlockBuilder ibb(opts.get());
    CHECK_EQ(num_ints, ibb.Add(reinterpret_cast<uint8_t *>(&data[0]),
                               num_ints));

    Slice s = ibb.Finish(0);
    GVIntBlockDecoder ibd(s);
    ibd.ParseHeader();

    // Benchmark seeking
    LOG_TIMING(INFO, "Seeking in gvint block") {
      for (int i = 0; i < num_queries; i++) {
        bool exact;
        uint32_t target = random() % (num_ints * 2 + kBase);
        Status s = ibd.SeekAtOrAfterValue(&target, &exact);
        if (verify) {
          SCOPED_TRACE(target);
          if (s.IsNotFound()) {
            ASSERT_EQ(kBase + num_ints * 2 - 1, target);
            continue;
          }
          ASSERT_STATUS_OK_FAST(s);

          uint32_t got;
          CopyOne<UINT32>(&ibd, &got);

          if (target < kBase) {
            ASSERT_EQ(kBase, got);
            ASSERT_FALSE(exact);
          } else if (target % 2 == 0) {
            // Was inserted
            ASSERT_EQ(target, got);
            ASSERT_TRUE(exact);
          } else {
            ASSERT_EQ(target + 1, got);
            ASSERT_FALSE(exact);
          }
        }
      }
    }
  }

  template <class BlockBuilderType, class BlockDecoderType>
  void TestEmptyBlockEncodeDecode() {
    gscoped_ptr<WriterOptions> opts(new WriterOptions());
    BlockBuilderType bb(opts.get());
    Slice s = bb.Finish(0);
    ASSERT_GT(s.size(), 0);
    LOG(INFO) << "Encoded size for 0 items: " << s.size();

    BlockDecoderType bd(s);
    ASSERT_STATUS_OK(bd.ParseHeader());
    ASSERT_EQ(0, bd.Count());
    ASSERT_FALSE(bd.HasNext());
  }

  Arena arena_;
};

TEST_F(TestEncoding, TestIntBlockEncoder) {
  gscoped_ptr<WriterOptions> opts(new WriterOptions());
  GVIntBlockBuilder ibb(opts.get());

  int *ints = new int[10000];
  for (int i = 0; i < 10000; i++) {
    ints[i] = random();
  }
  ibb.Add(reinterpret_cast<const uint8_t *>(ints), 10000);
  delete[] ints;

  Slice s = ibb.Finish(12345);
  LOG(INFO) << "Encoded size for 10k ints: " << s.size();

  // Test empty case -- should be 5 bytes for just the
  // header word (all zeros)
  ibb.Reset();
  s = ibb.Finish(0);
  ASSERT_EQ(5UL, s.size());
}

TEST_F(TestEncoding, TestIntBlockRoundTrip) {
  gscoped_ptr<WriterOptions> opts(new WriterOptions());
  const uint32_t kOrdinalPosBase = 12345;

  srand(123);

  std::vector<uint32_t> to_insert;
  for (int i = 0; i < 10003; i++) {
    to_insert.push_back(random());
  }

  GVIntBlockBuilder ibb(opts.get());
  ibb.Add(reinterpret_cast<const uint8_t *>(&to_insert[0]),
          to_insert.size());
  Slice s = ibb.Finish(kOrdinalPosBase);

  GVIntBlockDecoder ibd(s);
  ibd.ParseHeader();

  ASSERT_EQ(kOrdinalPosBase, ibd.ordinal_pos());

  std::vector<uint32_t> decoded;
  decoded.resize(to_insert.size());

  ColumnBlock dst_block(GetTypeInfo(UINT32),
                        &decoded[0],
                        to_insert.size(),
                        &arena_);
  int dec_count = 0;
  while (ibd.HasNext()) {
    ASSERT_EQ((uint32_t)(kOrdinalPosBase + dec_count),
              ibd.ordinal_pos());

    size_t to_decode = (random() % 30) + 1;
    size_t n = to_decode;
    ASSERT_STATUS_OK_FAST(ibd.CopyNextValues(&n, &dst_block));
    ASSERT_GE(to_decode, n);
    dst_block.Advance(n);
    dec_count += n;
  }

  ASSERT_EQ(0, dst_block.nrows())
    << "Should have no space left in the buffer after "
    << "decoding all rows";

  for (uint i = 0; i < to_insert.size(); i++) {
    if (to_insert[i] != decoded[i]) {
      FAIL() << "Fail at index " << i <<
        " inserted=" << to_insert[i] << " got=" << decoded[i];
    }
  }

  // Test Seek within block by ordinal
  for (int i = 0; i < 100; i++) {
    int seek_off = random() % decoded.size();
    ibd.SeekToPositionInBlock(seek_off);

    EXPECT_EQ((uint32_t)(kOrdinalPosBase + seek_off),
              ibd.ordinal_pos());
    uint32_t ret;
    CopyOne<UINT32>(&ibd, &ret);
    EXPECT_EQ(decoded[seek_off], ret);
  }
}

TEST_F(TestEncoding, TestIntEmptyBlockEncodeDecode) {
  TestEmptyBlockEncodeDecode<GVIntBlockBuilder, GVIntBlockDecoder>();
}

// Test seeking to a value in a small block.
// Regression test for a bug seen in development where this would
// infinite loop when there are no 'restarts' in a given block.
TEST_F(TestEncoding, TestStringPrefixBlockBuilderSeekByValueSmallBlock) {
  TestStringSeekByValueSmallBlock<StringPrefixBlockBuilder, StringPrefixBlockDecoder>();
}

TEST_F(TestEncoding, TestStringPlainBlockBuilderSeekByValueSmallBlock) {
  TestStringSeekByValueSmallBlock<StringPlainBlockBuilder, StringPlainBlockDecoder>();
}

// Test seeking to a value in a large block which contains
// many 'restarts'
TEST_F(TestEncoding, TestStringPrefixBlockBuilderSeekByValueLargeBlock) {
  TestStringSeekByValueLargeBlock<StringPrefixBlockBuilder, StringPrefixBlockDecoder>();
}

TEST_F(TestEncoding, TestStringPlainBlockBuilderSeekByValueLargeBlock) {
  TestStringSeekByValueLargeBlock<StringPlainBlockBuilder, StringPlainBlockDecoder>();
}

// Test round-trip encode/decode of a string block.
TEST_F(TestEncoding, TestStringPrefixBlockBuilderRoundTrip) {
  TestStringBlockRoundTrip<StringPrefixBlockBuilder, StringPrefixBlockDecoder>();
}

TEST_F(TestEncoding, TestStringPlainBlockBuilderRoundTrip) {
  TestStringBlockRoundTrip<StringPlainBlockBuilder, StringPlainBlockDecoder>();
}

// Test empty block encode/decode
TEST_F(TestEncoding, TestStringPlainEmptyBlockEncodeDecode) {
  TestEmptyBlockEncodeDecode<StringPlainBlockBuilder, StringPlainBlockDecoder>();
}

TEST_F(TestEncoding, TestStringPrefixEmptyBlockEncodeDecode) {
  TestEmptyBlockEncodeDecode<StringPrefixBlockBuilder, StringPrefixBlockDecoder>();
}

#ifdef NDEBUG
TEST_F(TestEncoding, GVIntSeekBenchmark) {
  DoSeekTest(32768, 1000000, false);
}
#endif

TEST_F(TestEncoding, GVIntSeekTest) {
  DoSeekTest(64, 1000, true);
}


TEST_F(TestEncoding, GVIntSeekTestTinyBlock) {
  for (int block_size = 1; block_size < 16; block_size++) {
    DoSeekTest(block_size, 1000, true);
  }
}

} // namespace cfile
} // namespace kudu
