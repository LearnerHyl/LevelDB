// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_WRITER_H_
#define STORAGE_LEVELDB_DB_LOG_WRITER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WritableFile;

namespace log {

class Writer {
 public:
  // Create a writer that will append data to "*dest".
  // "*dest" must be initially empty.
  // "*dest" must remain live while this Writer is in use.
  // 创建一个Writer对象，该对象将数据追加到“*dest”中。
  // “*dest”必须最初为空。
  // 在使用此Writer时，“*dest”必须保持活动状态。
  explicit Writer(WritableFile* dest);

  // Create a writer that will append data to "*dest".
  // "*dest" must have initial length "dest_length".
  // "*dest" must remain live while this Writer is in use.
  // 创建一个Writer对象，该对象将数据追加到“*dest”中。
  // “*dest”必须具有初始长度“dest_length”。
  // 在使用此Writer时，“*dest”必须保持活动状态。
  Writer(WritableFile* dest, uint64_t dest_length);

  // 不允许拷贝构造和赋值
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  // 将slice中的record写入到log文件中
  // 根据slice的大小，可能会将slice分片写入到log文件中
  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_; // 一个具有一定buffer的可以顺序写入的文件抽象类
  int block_offset_;  // Current offset in block

  // crc32c values for all supported record types.  These are
  // pre-computed to reduce the overhead of computing the crc of the
  // record type stored in the header.
  // 所有支持的记录类型的crc32c值。 这些是预先计算的，以减少计算存储在header中的记录类型的crc的开销。
  uint32_t type_crc_[kMaxRecordType + 1];
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_WRITER_H_
