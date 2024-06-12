// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include "db/dbformat.h"
#include <set>
#include <utility>
#include <vector>

namespace leveldb {

class VersionSet;

// 用于记录SSTable文件的元数据信息，包括文件编号、文件大小、文件中的最小键和最大键。
struct FileMetaData {
  FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

  // 文件的引用计数，当引用计数为0时，表示该文件可以被删除。
  int refs;
  // 允许的最大的无效查找次数，当无效查找次数达到该值时，表示该文件需要进行Compaction操作。
  // 无效查询代表在该文件中查找某个键时，发现该键不存在，因此是无效的。
  // allow_seeks_用于触发seek compaction，当一个文件的allow_seeks_达到一个阈值时，
  // 说明这个文件中的数据已经过时，需要进行compaction操作。

  int allowed_seeks;
  // 文件序列号，文件序列号是一个递增的序列号，用于唯一标识一个文件。
  uint64_t number;
  // 文件大小，单位为字节。
  uint64_t file_size;
  // 该SSTable文件中的最小键
  InternalKey smallest;
  // 该SSTable文件中的最大键
  InternalKey largest;
};

class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() = default;

  // 清空VersionEdit对象中的所有字段
  void Clear();

  // 设置比较器名称
  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  // 设置日志文件序列号
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  // 设置上一个日志文件序列号。该字段已经废弃，代码保留是为了向后兼容。
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  // 设置将要为下一个文件分配的序列号
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  // 设置当前版本的最大序列号，即最后这批写操作的最新序列号
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  // 当level层的Compaction操作结束后，本次compaction操作的最大键为key，
  // 下次compaction操作时，选择的键不得小于key。
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.push_back(std::make_pair(level, key));
  }

  // Add the specified file at the specified number.
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  // 添加一个具有特定文件编号的特定文件。
  // 要求：该版本尚未保存（请参阅VersionSet::SaveTo）
  // 要求："smallest"和"largest"是文件中的最小和最大键
  // 
  // 向level层添加一个文件，文件编号为file，文件大小为file_size，文件中的最小键为smallest，最大键为largest。
  // 用于向指定层级添加一个文件。
  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    // 将信息封装为FileMetaData结构体，然后添加到new_files_中。
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.push_back(std::make_pair(level, f));
  }

  // Delete the specified "file" from the specified "level".
  // 删除指定层级的具有特定文件编号的文件。
  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  // 将VersionEdit对象序列化为字符串，存储到dst中，注意存储之前已经对每个字段进行了变长编码压缩。
  // 每个字段都有一个自己的Tag，用于标识该字段的类型。这是为了有选择的编码部分字段。
  void EncodeTo(std::string* dst) const;
  // 将字符串src解析为VersionEdit对象，注意解析之后需要对每个字段进行变长解码。
  Status DecodeFrom(const Slice& src);

  // 将VersionEdit中的字段的值依次输出到字符串中返回，用于调试。
  std::string DebugString() const;

 private:
  friend class VersionSet;

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

  // 比较器名称，用于比较两个 InternalKey的大小
  std::string comparator_;
  // 日志文件编号，日志文件与MemTable一一对应，当一个MemTable生成为SSTable后，
  // 可以将该MemTable对应的日志文件删除。日志文件名称由6位日志文件序列号+".log"组成，例如"000001.log"。
  uint64_t log_number_;
  // 上一个日志文件编号。该字段已经废弃，代码保留是为了向后兼容。
  uint64_t prev_log_number_;
  // 为下一个文件分配的序列号。LevelDB中有日志文件、SSTable文件、MANIFEST文件。
  // SSTable文件的文件名由6位序列号+".sst"组成，例如"000001.sst"。
  // MANIFEST文件的文件名由Manifest-前缀+6位序列号组成，例如"Manifest-000001"。
  // 所有文件的序列号都是递增的，next_file_number_字段记录了下一个文件的序列号。
  uint64_t next_file_number_;
  // 当前版本的最大序列号。仅对于SSTable文件而言，该字段记录了该SSTable文件中的最大序列号。
  // 即当前SSTable文件中的最新的写操作的序列号。
  SequenceNumber last_sequence_;
  bool has_comparator_;
  bool has_log_number_;
  bool has_prev_log_number_;
  bool has_next_file_number_;
  bool has_last_sequence_;

  // 记录每个层级进行下一次Compaction操作时从哪个键开始。对每个层级L，会记录该层
  // 上次Compaction操作结束时的最大键，下次Compaction操作选取文件时，目标文件的
  // 最小键应该大于该键。即每一层的compaction操作都会在该层的键空间中循环执行。
  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  // 记录每个层级执行Compaction操作后，删除掉的文件，这里只需要记录删除文件的序列号即可。
  // DeletedFileSet是一个set容器，其中的元素是pair<int, uint64_t>类型，表示删除的文件的层级和文件编号。
  DeletedFileSet deleted_files_;
  // 记录每个层级执行Compaction操作后，新增的文件。新增文件的元数据信息以FileMetaData结构体的形式保存。
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_
