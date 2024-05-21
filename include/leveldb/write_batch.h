// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
// WriteBatch 持有一组更新，这组更新要原子地应用到一个DB上。
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
// updates是按照它们被添加到WriteBatch中的顺序应用的。
// 例如，下面的batch被写入后，"key"的值将是"v3"：
//
//  batch.Put("key", "v1");
//  batch.Delete("key");
//  batch.Put("key", "v2");
//  batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.
// 多个线程可以在不需要外部同步的情况下调用WriteBatch的const方法，
// 但是如果任何一个线程可能调用一个非const方法，所有访问相同WriteBatch的线程都必须使用外部同步。

#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include <string>

#include "leveldb/export.h"
#include "leveldb/status.h"

namespace leveldb {

class Slice;
/**
 * WriteBatch类:相当于一个缓冲区，用于存储一组更新操作，然后将这组更新操作原子地应用到数据库中。
 * 每个WriteBatch对应一条日志记录，日志记录中存储了一组更新操作。
 * 1. WriteBatch类用于将一组更新原子地应用到数据库中。
 * 2. 更新是按照它们被添加到WriteBatch中的顺序应用的。
 * 3. 这组操作可以是Put操作，也可以是Delete操作。
 */
class LEVELDB_EXPORT WriteBatch {
 public:
  class LEVELDB_EXPORT Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  // Intentionally copyable.
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  // Store the mapping "key->value" in the database.
  // 将映射"key->value"存储到这个WriteBatch中。
  void Put(const Slice& key, const Slice& value);

  // If the database contains a mapping for "key", erase it.  Else do nothing.
  // 如果数据库中包含"key"的映射，则删除它。否则什么也不做。
  // 实际上也是将这个操作存储到WriteBatch中。
  void Delete(const Slice& key);

  // Clear all updates buffered in this batch.
  // 清除此batch中缓冲的所有更新。
  void Clear();

  // The size of the database changes caused by this batch.
  // 此batch引起的数据库变化的大小。
  //
  // This number is tied to implementation details, and may change across
  // releases. It is intended for LevelDB usage metrics.
  // 这个数字与实现细节有关，可能会在不同版本之间发生变化。它用于LevelDB的使用度量。
  // 实际上是rep_的大小。
  size_t ApproximateSize() const;

  // Copies the operations in "source" to this batch.
  // 将"source"中的操作复制到此batch中。
  //
  // This runs in O(source size) time. However, the constant factor is better
  // than calling Iterate() over the source batch with a Handler that replicates
  // the operations into this batch.
  // 这需要O(source size)的时间。然而，常数因子比使用一个Handler在source
  // batch上调用Iterate()来复制操作到此batch中要好。
  void Append(const WriteBatch& source);

  // Support for iterating over the contents of a batch.
  // 支持遍历batch的内容。遍历的过程中，将对应的操作应用到Handler中。
  // BackGround:由WriteBatchInternal::InsertInto()方法调用，将这个WriteBatch对象中的操作插入到MemTable中。
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  // rep_是WriteBatch的实际数据，是一个字符串，存储了一组更新操作。
  std::string rep_;  // See comment in write_batch.cc for the format of rep_
  //
  // WriteBatch::rep_ :=
  //    sequence: fixed64         // 序列号，代表这个WriteBatch的序列号
  //    count: fixed32           // 表示这组更新操作的数量
  //    data: record[count]     // 一组更新操作，每个record都表示一个更新操作
  // record的内部格式根据操作类型不同而不同：
  // 1.若是KTypeValue类型，则是一个Put操作，后面的两个varstring分别为key和value
  // 2.若是KTypeDeletion类型，则是一个Delete操作，后面的varstring为key
  // record :=
  //    kTypeValue varstring varstring         |
  //    kTypeDeletion varstring
  // varstring :=
  //    len: varint32
  //    data: uint8[len]
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
