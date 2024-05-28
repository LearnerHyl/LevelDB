// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/comparator.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>

#include "leveldb/slice.h"
#include "util/logging.h"
#include "util/no_destructor.h"

namespace leveldb {

Comparator::~Comparator() = default;

namespace {
class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() = default;

  const char* Name() const override { return "leveldb.BytewiseComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }

  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    // Find length of common prefix
    size_t min_length = std::min(start->size(), limit.size());
    // diff_index代表start字符串和limit切片的第一个不相同的字符的索引
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }
    // 这意味着start字符串和limit切片的[0, diff_index)部分是相同的
    // 当diff_index == min_length时，这意味着某一个字符串是另一个字符串的前缀
    // 一般情况下diff_index意味着第一个不相同的字符的索引

    if (diff_index >= min_length) {// 若一个字符串是另一个字符串的前缀，则不需要缩短
      // Do not shorten if one string is a prefix of the other
    } else {
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      // 这里的diff_byte是start字符串和limit切片的第一个不相同的字符
      // 若diff_byte < static_cast<uint8_t>(0xff)，则可以将start字符串的diff_index处的字符加1
      // 若diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])，则可以将start字符串的diff_index处的字符加1
      // 这样可以保证start字符串和limit切片的[0, diff_index)部分是相同的，且start字符串的diff_index处的字符比limit切片的diff_index处的字符小
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        (*start)[diff_index]++;
        // 之后只保留start字符串前diff_index + 1个字符，即区间为[0, diff_index]
        // 最后那个字符就是自增后的字符
        start->resize(diff_index + 1);
        // *start的压缩结果必须小于limit
        assert(Compare(*start, limit) < 0);
      }
    }
    // 举例说明：*start="abcd","limit"="abzf",
    // 经过遍历，diff_index=2，diff_byte='c'，diff_byte+1='d'，limit[diff_index]='z'
    // 满足条件，*start[diff_index]++="d"，start->resize(3)，即*start="abd"
  }

  void FindShortSuccessor(std::string* key) const override {
    // Find first character that can be incremented
    // 找到第一个可以自增的字符，即不是0xff的字符，找到后将其自增，然后将后面的字符全部删除
    // e.g. input="abcd", output="b"; input="f", output="g";
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        // 之后只保留key字符串前i + 1个字符，即区间为[0, i]，最后那个字符就是自增后的字符
        key->resize(i + 1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
  }
};
}  // namespace

const Comparator* BytewiseComparator() {
  static NoDestructor<BytewiseComparatorImpl> singleton;
  return singleton.get();
}

}  // namespace leveldb
