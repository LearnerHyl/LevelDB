// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_
#define STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_

#include "db/dbformat.h"
#include "leveldb/write_batch.h"

namespace leveldb {

class MemTable;

// WriteBatchInternal provides static methods for manipulating a
// WriteBatch that we don't want in the public WriteBatch interface.
// WriteBatchInternal提供了用于操作WriteBatch的静态方法，我们不希望在公共WriteBatch接口中使用这些方法。
class WriteBatchInternal {
 public:
  // Return the number of entries in the batch.
  // 返回batch中的entry数量
  static int Count(const WriteBatch* batch);

  // Set the count for the number of entries in the batch.
  // 设置batch中的entry数量
  static void SetCount(WriteBatch* batch, int n);

  // Return the sequence number for the start of this batch.
  // 返回batch的起始序列号
  static SequenceNumber Sequence(const WriteBatch* batch);

  // Store the specified number as the sequence number for the start of
  // this batch.
  // 将指定的数字存储为此batch的起始序列号
  static void SetSequence(WriteBatch* batch, SequenceNumber seq);

  static Slice Contents(const WriteBatch* batch) { return Slice(batch->rep_); }

  static size_t ByteSize(const WriteBatch* batch) { return batch->rep_.size(); }

  static void SetContents(WriteBatch* batch, const Slice& contents);

  // BackGround：调用DBImpl::write()方法写入一批writers数据，每个writer包含一个WriteBatch对象，
  // 每当一个WriteBatch对象中的操作被写入到log后，就会调用WriteBatchInternal::InsertInto()方法将
  // 这个WriteBatch对象中的操作插入到MemTable中。同时这个过程会使这批操作对用户可见,即插入到MemTable中。
  static Status InsertInto(const WriteBatch* batch, MemTable* memtable);

  static void Append(WriteBatch* dst, const WriteBatch* src);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_WRITE_BATCH_INTERNAL_H_
