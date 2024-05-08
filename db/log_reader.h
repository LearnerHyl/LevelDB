// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_READER_H_
#define STORAGE_LEVELDB_DB_LOG_READER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class SequentialFile;

namespace log {

class Reader {
 public:
  // Interface for reporting errors.
  // 用于报告错误的接口
  class Reporter {
   public:
    virtual ~Reporter();

    // Some corruption was detected.  "bytes" is the approximate number
    // of bytes dropped due to the corruption.
    // 检测到一些损坏。 “bytes”是由于损坏而丢弃的字节数的近似值。
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Create a reader that will return log records from "*file".
  // "*file" must remain live while this Reader is in use
  // 创建一个Reader对象，该对象将从“*file”中返回日志记录。
  // 在使用此Reader时，“*file”必须保持活动状态
  //
  // If "reporter" is non-null, it is notified whenever some data is
  // dropped due to a detected corruption.  "*reporter" must remain
  // live while this Reader is in use.
  // 如果"reporter"不是null，则在由于检测到损坏而丢弃一些数据时通知它。
  // 在使用此Reader时，“*reporter”必须保持活动状态。
  //
  // If "checksum" is true, verify checksums if available.
  // 如果“checksum”为true，则在可用时验证校验和。
  //
  // The Reader will start reading at the first record located at physical
  // position >= initial_offset within the file.
  // Reader将从文件中的物理位置>= initial_offset的第一条记录处开始读取。
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader();

  // Read the next record into *record.  Returns true if read
  // successfully, false if we hit end of the input.  May use
  // "*scratch" as temporary storage.  The contents filled in *record
  // will only be valid until the next mutating operation on this
  // reader or the next mutation to *scratch.
  // 将下一条记录读入*record。 如果成功读取，则返回true，如果遇到输入结束，则返回false。
  // 可能会使用“*scratch”作为临时存储。 
  // 填充在*record中的内容只在对此reader的下一个修改操作或对*scratch的下一个修改操作发生之前有效。
  bool ReadRecord(Slice* record, std::string* scratch);

  // Returns the physical offset of the last record returned by ReadRecord.
  // 返回ReadRecord函数返回的最后一条记录的物理偏移量。
  //
  // Undefined before the first call to ReadRecord.
  // 在第一次调用ReadRecord之前是处于未定义状态的。
  uint64_t LastRecordOffset();

 private:
  // Extend record types with the following special values
  // 用下述的特殊值扩展日志header中的type类型
  enum {
    // 有如下几种情况会报告KEof(ReadPhysicalRecord中有详细说明):
    // 1. 从file_中读取操作失败，大多数情况下是因为文件已经读取到末尾。
    // 2. 读取到的块大小<kHeaderSize，且文件末尾eof_为true：若buffer_不为空，
    // 意味着我们在文件末尾有一个截断的header，这可能是由于写入程序在写入header的中途崩溃引起的。
    // 不要将其视为错误，只需报告EOF。
    // 3. 解析record发现实际长度小于kHeaderSize+content_size:若到达文件末尾，这可能是
    // 写入到一半程序崩溃了，不要将其视为错误，只需报告EOF。

    kEof = kMaxRecordType + 1,
    // Returned whenever we find an invalid physical record.
    // Currently there are three situations in which this happens:
    // * The record has an invalid CRC (ReadPhysicalRecord reports a drop)
    // * The record is a 0-length record (No drop is reported)
    // * The record is below constructor's initial_offset (No drop is reported)

    // 有如下几种情况会报告kBadRecord(ReadPhysicalRecord中有详细说明):
    // 1. 解析record发现实际长度小于kHeaderSize+content_size:若未到达文件末尾，我们认为
    // 这是一条损坏的记录，报告kBadRecord。
    // 2. record的header中的crc与实际计算的crc不一致：我们认为这是一条损坏的记录，报告kBadRecord。
    kBadRecord = kMaxRecordType + 2
  };

  // Skips all blocks that are completely before "initial_offset_".
  // 跳过所有完全在“initial_offset_”之前的块。
  //
  // Returns true on success. Handles reporting.
  // 成功时返回true。 处理报告。
  bool SkipToInitialBlock();

  // Return type, or one of the preceding special values
  // 返回类型，或前面的特殊值之一
  unsigned int ReadPhysicalRecord(Slice* result);

  // Reports dropped bytes to the reporter.
  // 向reporter报告丢弃的字节。
  // buffer_ must be updated to remove the dropped bytes prior to invocation.
  // 在调用之前，必须更新buffer_以删除已丢弃的字节。
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  bool const checksum_; // 是否需要校验和
  char* const backing_store_;
  Slice buffer_; // 每次从文件中读取的数据都会存储在buffer_中，读取的单位是kBlockSize个字节
  // 最后一次Read()调用返回的结果是否是EOF，通过判断buffer_的大小是否小于kBlockSize来判断
  bool eof_;  // Last Read() indicated EOF by returning < kBlockSize

  // Offset of the last record returned by ReadRecord.
  // ReadRecord返回的最后一条记录的偏移量。
  uint64_t last_record_offset_;
  // Offset of the first location past the end of buffer_.
  // 缓冲区末尾之后的第一个位置的偏移量。即访问的字节地址必须小于end_of_buffer_offset_
  uint64_t end_of_buffer_offset_;

  // Offset at which to start looking for the first record to return
  // 从initial_offset_开始查找要返回的第一条记录的偏移量
  uint64_t const initial_offset_;

  // True if we are resynchronizing after a seek (initial_offset_ > 0). In
  // particular, a run of kMiddleType and kLastType records can be silently
  // skipped in this mode
  // 如果我们在一个seek之后重新同步（initial_offset_ > 0），则为true。
  // 特别是，在这种模式下，可以静默地跳过kMiddleType和kLastType记录的运行
  // 
  // 本质上resyncing_是用来记录是否需要跳过一些block的，这由后面的初始化函数:resyncing_ = (initial_offset_ > 0)不难看出。
  // 1. 初始值为resyncing = initial_offset_ > 0
  //    当第一次读record的时候，就跳把这些block跳过。
  //    然后设置recyncing为false
  // 2. 第二次读的时候，直接无视resyncing_.
  bool resyncing_;
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_READER_H_
