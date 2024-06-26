// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include "leveldb/env.h"
#include "leveldb/options.h"
#include "port/port.h"
#include "table/block.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  // 将offset_和size_用varint64编码方式存储到dst中
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  // 注意编码的时候是先存储offset_，再存储size_
  // 因此解码的时候也是先解码offset_，再解码size_
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    return Status::Corruption("bad block handle");
  }
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  // 依次将metaindex_handle_和index_handle_的编码存储到dst中
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  // 无论metaindex_handle_和index_handle_的编码长度总和是否达到2*BlockHandle::kMaxEncodedLength，
  // 都要填充到2*BlockHandle::kMaxEncodedLength
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  // 将kTableMagicNumber用固定长度编码方式存储到dst中
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;  // Disable unused variable warning.
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < kEncodedLength) {
    return Status::Corruption("not an sstable (footer too short)");
  }
  // 首先解码出MagicNumber字段
  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }

  // 按照Footer编码数据时的顺序，依次解码metaindex_handle_和index_handle_
  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    // 我们跳过“input”中的任何剩余数据（目前只是填充）
    // end意味着Footer的结束位置(meta_index_handle_ | index_handle_ | padding | magic_number)
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result) {
  // 初始化结果BlockContents对象
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

  // 读取块内容以及类型/校验和尾部。
  // 详细见table_builder.cc中构建此结构的代码。
  size_t n = static_cast<size_t>(handle.size());
  // 分配一个新的缓冲区用于存储块内容和尾部信息
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;
  // 从文件中读取指定大小的数据到缓冲区
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  // 如果读取的数据大小不符合预期，则返回截断块读取的错误状态
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated block read");
  }

  // 检查类型和块内容的crc校验和
  const char* data = contents.data();  // 指向读取数据的位置
  if (options.verify_checksums) {
    // 计算并验证crc校验和
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }

  // 根据块类型处理数据
  switch (data[n]) {
    case kNoCompression:  // 无压缩
      if (data != buf) {
        // 文件实现给了我们指向其他数据的指针。直接使用它，假设它在文件打开期间有效。
        delete[] buf;
        result->data = Slice(data, n);
        result->heap_allocated = false;
        result->cachable = false;  // 不进行双重缓存
      } else {
        result->data = Slice(buf, n);
        result->heap_allocated = true;
        result->cachable = true;
      }
      break;

    case kSnappyCompression: {  // Snappy压缩
      size_t ulength = 0;
      // 获取Snappy压缩数据的解压后长度
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        delete[] buf;
        return Status::Corruption("corrupted snappy compressed block length");
      }
      // 分配缓冲区用于存储解压后的数据
      char* ubuf = new char[ulength];
      // 解压数据
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        delete[] buf;
        delete[] ubuf;
        return Status::Corruption("corrupted snappy compressed block contents");
      }
      delete[] buf;
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }

    case kZstdCompression: {  // Zstd压缩
      size_t ulength = 0;
      // 获取Zstd压缩数据的解压后长度
      if (!port::Zstd_GetUncompressedLength(data, n, &ulength)) {
        delete[] buf;
        return Status::Corruption("corrupted zstd compressed block length");
      }
      // 分配缓冲区用于存储解压后的数据
      char* ubuf = new char[ulength];
      // 解压数据
      if (!port::Zstd_Uncompress(data, n, ubuf)) {
        delete[] buf;
        delete[] ubuf;
        return Status::Corruption("corrupted zstd compressed block contents");
      }
      delete[] buf;
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }

    default:  // 未知压缩类型
      delete[] buf;
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

}  // namespace leveldb
