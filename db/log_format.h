// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Log format information shared by reader and writer.
// See ../doc/log_format.md for more detail.

#ifndef STORAGE_LEVELDB_DB_LOG_FORMAT_H_
#define STORAGE_LEVELDB_DB_LOG_FORMAT_H_

namespace leveldb {
namespace log {

enum RecordType {
  // Zero is reserved for preallocated files
  // 0 是为预分配的文件保留的
  kZeroType = 0,

  // 表示整个记录都在一个 block 中
  kFullType = 1,

  // 下述字段说明当前record跨多个block存在
  // For fragments
  kFirstType = 2, // 表示该记录的第一个分片
  // 表示该记录的中间部分分片，注意一个record可能有多个类型为kMiddleType的分片，也可能没有类型为kMiddleType的分片
  kMiddleType = 3,
  // 表示该记录的最后一个分片
  kLastType = 4
};
static const int kMaxRecordType = kLastType;

// 一个Block的默认大小为32KB
// NOTE:注意这里与block_builder.h中的kBlockSize的定义不同，这里是32KB，而block_builder.h中是4KB。
// 这里的kBlockSize是用来定义log文件中的block的大小的，而block_builder.h中的Block大小
static const int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
static const int kHeaderSize = 4 + 2 + 1;

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_FORMAT_H_
