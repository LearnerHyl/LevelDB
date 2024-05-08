// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_reader.h"

#include <cstdio>

#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }

// 一般情况下，Reader对象的initial_offset_在第一个block内(其实initial_offset_一般是0)
// TODO:该函数目前无论在LevelDB还是RocksDB中都没有被调用，因为在Reader的构造函数中，initial_offset_总是等于0
bool Reader::SkipToInitialBlock() {
  // offset_in_block是初始偏移量换算成块内偏移量
  const size_t offset_in_block = initial_offset_ % kBlockSize;
  // block_start_location是初始偏移量所在块的起始地址
  uint64_t block_start_location = initial_offset_ - offset_in_block;

  // Don't search a block if we'd be in the trailer
  // 如果偏移量在块的最后6个字节内，则跳过该块，将block_start_location指向下一个块
  if (offset_in_block > kBlockSize - 6) {
    block_start_location += kBlockSize;
  }

  // 这里可以认为我们初始化buffer_为一个空的buffer，因此将end_of_buffer_offset_设置为block_start_location
  // 而block_start_location是初始偏移量所在块的起始地址，一般为0
  end_of_buffer_offset_ = block_start_location;

  // Skip to start of first block that can contain the initial record
  // 跳转到包含初始记录的第一个块的起始位置
  if (block_start_location > 0) { // 如果初始偏移量不在第一个块的起始位置
    // 跳转到block_start_location位置
    Status skip_status = file_->Skip(block_start_location);
    if (!skip_status.ok()) {
      ReportDrop(block_start_location, skip_status);
      return false;
    }
  }

  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  // 如果上一次读取的记录的偏移量小于该Reader当前规定的初始偏移量
  // 则跳转到当前的初始块的位置
  // FIXME:SkipToInitialBlock其实在LevelDB中是没有用的，因为在Reader的构造函数中，initial_offset_总是等于0
  // 即我们总是从头开始读取日志文件，而rocksDB中也已经将这部分代码删除了
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) {
      return false;
    }
  }

  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;
  // Record offset of the logical record that we're reading
  // 0 is a dummy value to make compilers happy
  // 我们正在读取的逻辑记录的记录偏移量 0 是一个虚拟值，可以让编译器高兴
  // 记录一条记录的开始的物理偏移量，如果有多个分片，那么这个值是第一个分片的物理偏移量
  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    // 尝试从buffer_中读取一条记录，buffer_是file_抽象的一个预读取的缓冲区
    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    // ReadPhysicalRecord may have only had an empty trailer remaining in its
    // internal buffer. Calculate the offset of the next physical record now
    // that it has returned, properly accounting for its header size.
    // ReadPhysicalRecord可能只剩下一个空的trailer在其内部缓冲区中。
    // 现在，它已经返回，正确计算下一个物理记录的偏移量，以正确计算其头部大小。
    // 
    // physical_record_offset是当前记录的物理偏移量
    uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

    // resyncing_=initial_offset_ > 0, 意味着我们是否需要跳过一些block
    // 注意之前在ReadRecord开头我们已经判断了是否需要跳过一些block，这里是为了预防有之前陈旧的跨block的记录
    // 因为我们要寻找的是从initial_offset_开始的第一条记录，因此第一条记录的type只能是kFullType或者kFirstType
    if (resyncing_) {
      if (record_type == kMiddleType) { // 说明是陈旧的跨block的记录的中间分片，继续尝试读取下一个记录0
        continue;
      } else if (record_type == kLastType) { // 说明是陈旧的跨block的记录的最后一个分片
        resyncing_ = false; // 到此resyncing_设置为false，说明我们已经跳过了陈旧的跨block的记录
        continue;
      } else { // 遇见了新的记录，可以继续读取
        resyncing_ = false; // 到此resyncing_设置为false，说明我们已经跳过了陈旧的跨block的记录
      }
    }

    switch (record_type) {
      case kFullType: // 说明这条记录完全在一个block中
        if (in_fragmented_record) { // 是记录的某一个分片，很明显这种情况是不合法的
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          // 处理log::Writer早期版本中的错误，该错误可能在块的末尾发出一个空的kFirstType记录，
          // 然后在下一个块的开头发出一个kFullType或kFirstType记录。
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(1)");
          }
        }
        // 获取这条记录开始的物理偏移量
        prospective_record_offset = physical_record_offset;
        // 清空scratch以确保没有之前的残留
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;

      case kFirstType: // 说明这条记录跨多个block，且这是第一个分片
        if (in_fragmented_record) { // 名义上是记录的第一个分片，但是实际上是之前的记录的中间分片，这种情况是不合法的
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          // 处理log::Writer早期版本中的错误，该错误可能在块的末尾发出一个空的kFirstType记录，
          // 然后在下一个块的开头发出一个kFullType或kFirstType记录。
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(2)");
          }
        }
        // 获取这条记录开始的物理偏移量
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true; // 标记当前是一条跨block的记录
        break;

      case kMiddleType: // 说明这条记录跨多个block，且这是中间分片
        if (!in_fragmented_record) { // 中间记录却没有开始记录，这种情况是不合法的
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(1)");
        } else { // 正常情况下，将中间分片的内容追加到scratch中
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType: // 说明这条记录跨多个block，且这是最后一个分片
        if (!in_fragmented_record) { // 最后一个分片却没有开始记录，这种情况是不合法的
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(2)");
        } else { // 正常情况下，将最后一个分片的内容追加到scratch中
          scratch->append(fragment.data(), fragment.size());
          // 将整条记录存储在record中
          *record = Slice(*scratch);
          // 更新last_record_offset_的值为这条记录的第一个分片的开始的物理偏移量
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof: // 具体说明见log_reader.h中的注释
        if (in_fragmented_record) {
          // This can be caused by the writer dying immediately after
          // writing a physical record but before completing the next; don't
          // treat it as a corruption, just ignore the entire logical record.
          // 这可能是由于写入程序在写入物理记录后立即死亡，但在完成下一个物理记录之前死亡；
          // 不要将其视为损坏，只需忽略整个逻辑记录。
          scratch->clear();
        }
        return false;

      case kBadRecord: // 记录本身有问题，具体说明见log_reader.h中的注释
        if (in_fragmented_record) { // 记录有问题，且是跨block的记录
          // 这种类型的问题需要报告给用户
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default: { // 未知的记录类型，告知用户
        char buf[40];
        std::snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
        ReportCorruption(
            (fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
            buf);
        in_fragmented_record = false;
        scratch->clear();
        break;
      }
    }
  }
  return false;
}

uint64_t Reader::LastRecordOffset() { return last_record_offset_; }

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr &&
      end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

// 读取物理记录，将读取到的记录存储在result中，并实时更新buffer_的内容
unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) { // buffer_中的数据不足一个header的大小
      if (!eof_) { // 若当前没有到达log文件的末尾
        // Last read was a full read, so this is a trailer to skip
        // 上一次读取的是一个完整的块，所以剩余的空间是一个填充的trailer，直接跳过剩余的空间
        buffer_.clear();
        // 从文件中读取下一个块的数据，并存储到buffer_中
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        // 相应的更新end_of_buffer_offset_的值
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        } else if (buffer_.size() < kBlockSize) { // 如果从文件中读取的新块的大小小于kBlockSize，说明这是文件的最后一个块
          eof_ = true;
        }
        continue;
      } else { // 若当前已经到达log文件的末尾
        // Note that if buffer_ is non-empty, we have a truncated header at the
        // end of the file, which can be caused by the writer crashing in the
        // middle of writing the header. Instead of considering this an error,
        // just report EOF.
        // 注意，如果buffer_不为空，我们在文件末尾有一个截断的header，这可能是由于写入程序在写入header的中途崩溃引起的。
        // 不要将其视为错误，只需报告EOF。
        buffer_.clear();
        return kEof;
      }
    }

    // buffer_中的数据已经足够一个header的大小，我们尝试解析这条record

    // Parse the header
    const char* header = buffer_.data();
    // header[4]和header[5]是记录的长度，header[6]是记录的类型
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = header[6];
    const uint32_t length = a | (b << 8);
    if (kHeaderSize + length > buffer_.size()) { // 若content长度+header长度大于实际buffer_的大小
      // 说明这条记录在写入的时候有问题
      size_t drop_size = buffer_.size();
      buffer_.clear();
      // 没有到达文件的末尾，且content长度+header长度大于实际buffer_的大小，说明这条记录是有问题的
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      // If the end of the file has been reached without reading |length| bytes
      // of payload, assume the writer died in the middle of writing the record.
      // Don't report a corruption.
      // 如果在读取|length|字节的有效负载之前已经到达文件的末尾，则假定写入程序在写入记录的中途死亡。
      // 不要报告损坏。
      return kEof;
    }

    // 跳过预分配的文件区域
    if (type == kZeroType && length == 0) {
      // Skip zero length record without reporting any drops since
      // such records are produced by the mmap based writing code in
      // env_posix.cc that preallocates file regions.
      // 跳过零长度记录，而不报告任何丢失，因为这些记录是由env_posix.cc中基于mmap的写入代码生成的，该代码预分配文件区域。
      buffer_.clear();
      return kBadRecord;
    }

    // Check crc
    if (checksum_) {
      // 获得header中的crc32校验和
      uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      // 计算实际的crc32校验和
      uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) { // 若实际的crc32校验和与header中的crc32校验和不相等
        // Drop the rest of the buffer since "length" itself may have
        // been corrupted and if we trust it, we could find some
        // fragment of a real log record that just happens to look
        // like a valid log record.
        // 丢弃缓冲区的其余部分，因为“length”本身可能已损坏，如果我们信任它，
        // 我们可能会找到一些看起来像有效日志记录的真实日志记录的片段。
        size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    // 成功解析出一条record，移动buffer_的指针，将record存储在result中
    buffer_.remove_prefix(kHeaderSize + length);

    // Skip physical record that started before initial_offset_
    // 跳过在initial_offset_之前开始的物理记录
    if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length <
        initial_offset_) {
      result->clear();
      return kBadRecord;
    }

    // 将record中的content内容存储在result中
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace log
}  // namespace leveldb
