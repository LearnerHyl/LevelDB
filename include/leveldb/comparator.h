// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
#define STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

// A Comparator object provides a total order across slices that are
// used as keys in an sstable or a database.  A Comparator implementation
// must be thread-safe since leveldb may invoke its methods concurrently
// from multiple threads.
// 一个Comparator对象提供了一个全局的顺序，用于sstable或者数据库中的key。
// Comparator的实现必须是线程安全的，因为leveldb可能会从多个线程并发地调用它的方法。
class LEVELDB_EXPORT Comparator {
 public:
  virtual ~Comparator();

  // Three-way comparison.  Returns value:
  //   < 0 iff "a" < "b",
  //   == 0 iff "a" == "b",
  //   > 0 iff "a" > "b"
  virtual int Compare(const Slice& a, const Slice& b) const = 0;

  // The name of the comparator.  Used to check for comparator
  // mismatches (i.e., a DB created with one comparator is
  // accessed using a different comparator.
  // Comparator的名字。用于检查comparator是否匹配（即使用一个comparator创建的DB，使用不同的comparator访问）。
  // 这意味着创建DB的comparator和访问DB的comparator必须是相同的。
  //
  // The client of this package should switch to a new name whenever
  // the comparator implementation changes in a way that will cause
  // the relative ordering of any two keys to change.
  // 无论何时，如果comparator的实现发生了改变，且这种改变会导致任何的两个key的相对顺序发生改变，
  // 那么这个包的客户端应该切换到一个新的名字。
  //
  // Names starting with "leveldb." are reserved and should not be used
  // by any clients of this package.
  // 以"leveldb."开头的名字是保留的，不应该被这个包的任何客户端使用。
  virtual const char* Name() const = 0;

  // Advanced functions: these are used to reduce the space requirements
  // for internal data structures like index blocks.
  // 高级函数：这些函数用于减少内部数据结构（如索引块）的空间需求。
  // 只看定义可能还有些模糊，LevelDB实现了两个默认的Comparator，分别是BytewiseComparator和InternalKeyComparator。
  // 可以看看这些默认的Comparator是如何实现这些高级函数的。

  // If *start < limit, changes *start to a short string in [start,limit).
  // Simple comparator implementations may return with *start unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 如果*start < limit，将*start更改为[start,limit)中的一个短字符串。
  // 简单的comparator实现可能会返回*start不变，即，一个什么都不做的这个方法的实现是正确的。
  // 即start经过压缩后，在字符串比较中应该始终满足：old_start <= new_start < limit
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const = 0;

  // Changes *key to a short string >= *key.
  // Simple comparator implementations may return with *key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  // 将*key更改为一个>=*key的短字符串。
  // 简单的comparator实现可能会返回*key不变，即，一个什么都不做的这个方法的实现是正确的。
  // 即key经过压缩后，在字符串比较中应该始终满足：old_key <= new_key
  virtual void FindShortSuccessor(std::string* key) const = 0;
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering.  The result remains the property of this module and
// must not be deleted.
LEVELDB_EXPORT const Comparator* BytewiseComparator();

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_COMPARATOR_H_
