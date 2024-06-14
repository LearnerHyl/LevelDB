// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DBFORMAT_H_
#define STORAGE_LEVELDB_DB_DBFORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
#include "leveldb/table_builder.h"

#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// Grouping of constants.  We may want to make some of these
// parameters set via options.
// 常量的分组。我们可能希望通过选项设置其中的一些参数。
namespace config {
// LSM-Tree的最大层数
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
// 当Level-0中的文件数达到这个值时，开始compaction。
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
// Level-0文件的数量的软限制。在这一点上，我们减慢写入速度。具体可见MakeRoomForWrite()函数。
// 我们选择将每个写操作都延迟1ms，而不是把单个写操作延迟很多s，这样可以减小延迟方差。
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
// Level-0文件的最大数量。在这一点上，我们停止写入。
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
// 如果新的压缩的memtable没有创建重叠，该memtable可以被推送到的最大层级。
// 我们尝试推送到第2层，以避免相对昂贵的0=>1层级的compaction，并避免一些昂贵的manifest文件操作。
// 我们不会一直推送到最大层级，因为如果相同的key空间被重复覆盖，这可能会产生大量的浪费磁盘空间。
// TODO:后面看compact模块时再回头认真学习这个参数，先留坑。
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
// 在迭代期间读取的数据样本之间的近似间隔（字节）。
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
// 值类型编码为内部键的最后一个组件。
// 不要更改这些枚举值：它们嵌入在磁盘上的数据结构中。
enum ValueType { kTypeDeletion = 0x0, kTypeValue = 0x1 };
// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
// kValueTypeForSeek定义了在构造用于寻找特定序列号的ParsedInternalKey对象时应传递的ValueType
// （因为我们按降序对序列号进行排序，并且值类型嵌入在内部键中的序列号的低8位中，
// 我们需要使用最高编号的ValueType，而不是最低编号的ValueType）。
// 
// 在Version_set.cc的SomeFileOverlapsWithRange()函数中，在搜索文件时，会使用kValueTypeForSeek。
static const ValueType kValueTypeForSeek = kTypeValue;

typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
// 0x1-ull：意味着是类型为unsigned long long的常量1
// sequenceNum：一共有64位，8个字节。最高8位是type，低56位是sequence number。
// 因此我们保留最高8位空间，这样type和sequence number可以被打包到64位中。
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence; // 7 bytes，表示版本号
  ValueType type;  // 1 byte，表示该版本是删除还是插入

  ParsedInternalKey() {}  // Intentionally left uninitialized (for speed)
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  std::string DebugString() const;
};

// Return the length of the encoding of "key".
// 返回“user_key”的编码后的长度。+8是因为编码后的数据中还包含了sequence和type。
// sequence是7个字节，type是1个字节。
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
// 将“key”的序列化后的数据附加到*result。
void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
// 尝试从“internal_key”解析一个内部键。成功时，将解析后的数据存储在“*result”中，并返回true。
//
// On error, returns false, leaves "*result" in an undefined state.
// 在错误时，返回false，将“*result”保留在未定义状态。
bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// Returns the user key portion of an internal key.
// 返回内部键的user key部分。
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
// 一个用于内部key的comparator，它使用一个指定的comparator来比较user
// key部分，并通过减小的sequence number来打破平局。
class InternalKeyComparator : public Comparator {
 private:
  // 一般是BytewiseComparatorImpl类型
  const Comparator* user_comparator_;

 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
  const char* Name() const override;
  int Compare(const Slice& a, const Slice& b) const override;
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override;
  void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Filter policy wrapper that converts from internal keys to user keys
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;

 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) {}
  const char* Name() const override;
  void CreateFilter(const Slice* keys, int n, std::string* dst) const override;
  bool KeyMayMatch(const Slice& key, const Slice& filter) const override;
};

// Modules in this directory should keep internal keys wrapped inside
// the following class instead of plain strings so that we do not
// incorrectly use string comparisons instead of an InternalKeyComparator.
// 此目录中的模块应该将internal key包装在以下类而不是普通字符串中，
// 这是为了防止我们错误地使用字符串比较，我们应该使用InternalKeyComparator。
// 注意InternalKey在比较时，不仅要比较user key，还要比较sequence number和value type。
class InternalKey {
 private:
  // rep_用于存储InternalKey序列化后的数据,格式如下：
  // user_key | sequence | type
  // 由AppendInternalKey()函数生成
  std::string rep_;

 public:
  InternalKey() {}  // Leave rep_ as empty to indicate it is invalid
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(const Slice& s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }

  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};

inline int InternalKeyComparator::Compare(const InternalKey& a,
                                          const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  // 从后往前解析，最后8个字节依次存储了sequence和type，sequence是7个字节，type是1个字节。
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return (c <= static_cast<uint8_t>(kTypeValue));
}

// A helper class useful for DBImpl::Get()
// 一个有用的辅助类，用于DBImpl::Get()
// LookUpKey: key本身数据长度(注意不包括tag)+key本身数据+tag(sequence number)
// 用于在MemTable中查找key。此外,InternalKey是由userkey和tag组成的，没有key_len。
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  // 为查找具有指定序列号的快照中的user_key初始化*this。
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  // 返回一个适合在MemTable中查找的key。返回的是key_len(注意不包括tag)+userkey+tag
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  // 返回一个内部键（适合传递给内部迭代器）。返回的是userkey+tag，没有key_len
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  // 只返回userkey
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32               <-- start_
  //    userkey  char[klength]          <-- kstart_
  //    tag      uint64
  //                                    <-- end_
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  // 这个数组是一个合适的MemTable key。
  // 以"userkey"开头的后缀可以用作InternalKey。
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];  // Avoid allocation for short keys
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DBFORMAT_H_
