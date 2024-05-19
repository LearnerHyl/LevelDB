// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_FORMAT_H_
#define STORAGE_LEVELDB_TABLE_FORMAT_H_

#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/table_builder.h"

namespace leveldb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// BlockHandle is a pointer to the extent of a file that stores a data
// block or a meta block.
// BlockHandle是一个指向存储数据块或元数据块的文件范围的指针。
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle
  // BlockHandle的最大编码长度
  enum { kMaxEncodedLength = 10 + 10 };

  BlockHandle();

  // The offset of the block in the file.
  // 在文件中，该block的起始位置的偏移量
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  // 存储的block的大小
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  // 将BlockHandle的offset_字段和size_字段用变长编码方式存储到dst中
  // varint64编码最长为10字节，因此kMaxEncodedLength为10 + 10 = 20
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_;  // block在文件中的起始位置的偏移量
  uint64_t size_;    // block的大小，同时也是可以访问的字节数
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
// Footer封装了存储在每个表文件尾部的固定信息。
// 每个SSTable文件的尾部都有一个Footer，主要包含了metaindex block和index
// block的BlockHandle。
class Footer {
 public:
  // Encoded length of a Footer.  Note that the serialization of a
  // Footer will always occupy exactly this many bytes.  It consists
  // of two block handles and a magic number.
  // Footer的编码长度。请注意，Footer的序列化始终占用完全这么多字节。它由两个块句柄和一个魔数组成。
  enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

  Footer() = default;

  // The block handle for the metaindex block of the table
  // SSTable文件的metaindex block的BlockHandle，用于定位metaindex
  // block在该SSTable文件中的位置
  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // The block handle for the index block of the table
  // SSTable文件的index block的BlockHandle，用于定位index
  // block在该SSTable文件中的位置
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  // 将Footer中的metaindex_handle_和index_handle_、MagicNumber用变长编码方式存储到dst中
  // 注意：不管metaindex_handle_和index_handle_的编码长度总和是否达到2*BlockHandle::kMaxEncodedLength，
  // 这两个字段在被编码进dst中后，都会按照2*BlockHandle::kMaxEncodedLength的长度填充到dst中
  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;  // metaindex block的BlockHandle
  BlockHandle index_handle_;      // index block的BlockHandle
};

// kTableMagicNumber was picked by running
//    echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
// kTableMagicNumber是通过运行echo http://code.google.com/p/leveldb/ | sha1sum
// 并取前64位得到的。
// SSTable的魔数是一个64位的常量，用于标识SSTable文件。
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
// 每个Block的尾部包含一个1字节的type和一个32位的crc，一共5个字节
// type用于标识block使用的压缩算法，crc用于校验block的内容是否正确
static const size_t kBlockTrailerSize = 5;

// 读取Block后会将读取的内容存储到BlockContents中，同理初始化Block时也需要BlockContents
struct BlockContents { // 注意BlockContents中的Data不包含Block的type和crc
  // Block中的数据
  Slice data;           // Actual contents of data
  // 这份数据是否可以被缓存
  bool cachable;        // True iff data can be cached
  // 这份数据是否是堆分配的，如果是的话，调用者需要delete[] data.data()，即需要自行释放data.data()指向的内存
  bool heap_allocated;  // True iff caller should delete[] data.data()
};

// Read the block identified by "handle" from "file".  On failure
// return non-OK.  On success fill *result and return OK.
// 从文件"file"中读取由"handle"标识的block。如果失败，则返回非OK。成功时填充*result并返回OK。
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

// Implementation details follow.  Clients should ignore,

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FORMAT_H_
