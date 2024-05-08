// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <cstdint>

#include "leveldb/env.h"

#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

Status Writer::AddRecord(const Slice& slice) {
  // ptr指向需要写入的数据
  const char* ptr = slice.data();
  // left表示需要写入的数据的长度
  size_t left = slice.size();

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  // 若有必要，对记录进行分片并发出。请注意，如果slice为空，我们仍然希望迭代一次以发出一个零长度的记录。
  Status s;
  // begin为true表明该条记录是第一次写入，即使这条记录会跨越多个block，也只有当写入第一个block时begin为true，
  // 后续的block写入时begin为false。通过begin字段可以判断当前recordType的类型是否为kFirstType。
  bool begin = true;
  do {
    // kBlockSize是一个block的大小(默认为32KB)，block_offset_表示当前block中已经写入的数据的大小(字节数)，
    // 故leftover表示当前block中剩余的可用空间大小(字节数)
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);
    // kHeaderSize为一个record的头部大小(7字节)，若当前block中剩余的可用空间小于一个record的头部大小且大于0，
    // 则需要用\x00填充剩余的所有空间，然后切换到一个新的block
    if (leftover < kHeaderSize) {
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize being 7)
        // 填充剩余的所有空间
        static_assert(kHeaderSize == 7, "");
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
      }
      // 若leftover等于0，说明正好写满一个block，不需要填充，直接开始写入下一个新的block
      block_offset_ = 0;
    }

    // Invariant: we never leave < kHeaderSize bytes in a block.
    // 经过上述处理，当前使用的block中剩余的可用空间至少有kHeaderSize个字节
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

    // 计算当前block中除去record头部后的剩余可用空间大小
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    // 计算可以被写入当前Block分片的数据大小，若left小于等于avail,说明当前block正好可以容纳下这条记录(数据+头部)
    // 若left大于avail,说明当前block无法容纳下这条记录，需要分片写入，取avail字节数据写入当前block
    const size_t fragment_length = (left < avail) ? left : avail;

    RecordType type;
    const bool end = (left == fragment_length); // 若left等于fragment_length，说明这是最后一次写入，该record代表最后一个分片
    if (begin && end) { // 第一次写入该记录，且该记录可以被一次性写入当前block，因此该记录的类型为kFullType
      type = kFullType;
    } else if (begin) { // 第一次写入该记录，但该记录需要分片写入，因此该记录的类型为kFirstType
      type = kFirstType;
    } else if (end) { // 不是第一次写入该记录，且该记录是最后一次写入，因此该记录的类型为kLastType
      type = kLastType;
    } else { // 既不是第一次写入该记录，也不是最后一次写入，因此该记录的类型为kMiddleType
      type = kMiddleType;
    }

    // 将记录按照格式写入到缓冲区中，参考该函数写入的具体逻辑
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    // 更新ptr和left，准备写入下一条记录
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}

// 按照log record的格式将这条记录的数据写入并执行Flush(取决于写入逻辑)，更新block_offset_
Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  // len字段长度为2B，因此length必须小于等于0xffff
  assert(length <= 0xffff);  // Must fit in two bytes
  // block_offset_表示当前block中已经写入的数据的大小(字节数)，因此block_offset_ + kHeaderSize + length必须小于等于kBlockSize
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // Format the header
  // 将record的头部格式化为一个7字节的buf数组
  char buf[kHeaderSize];
  // 因为LevelDB默认使用的是小端存储，因此buf[4]存储length的低8位，buf[5]存储length的高8位
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  // buf[6]存储record的类型
  buf[6] = static_cast<char>(t);

  // Compute the crc of the record type and the payload.
  // 计算record type和payload的crc32c校验和
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // Adjust for storage
  // 将crc32c校验和存储到buf[0]~buf[3]中
  EncodeFixed32(buf, crc);

  // Write the header and the payload
  // 将buf和ptr中的length字节数据写入到磁盘文件中
  // 首先写入header，然后写入payload，注意这里的写入仅仅代表写入到内核缓冲区，而不是写入到磁盘文件
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace log
}  // namespace leveldb
