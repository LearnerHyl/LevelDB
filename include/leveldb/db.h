// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_DB_H_
#define STORAGE_LEVELDB_INCLUDE_DB_H_

#include <cstdint>
#include <cstdio>

#include "leveldb/export.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"

namespace leveldb {

// Update CMakeLists.txt if you change these
static const int kMajorVersion = 1;
static const int kMinorVersion = 23;

struct Options;
struct ReadOptions;
struct WriteOptions;
class WriteBatch;

// Abstract handle to particular state of a DB.
// A Snapshot is an immutable object and can therefore be safely
// accessed from multiple threads without any external synchronization.
// 一个DB的特定状态的抽象句柄。
// Snapshot是一个不可变的对象，因此可以安全地被多个线程并发访问，而不需要外部同步机制。
class LEVELDB_EXPORT Snapshot {
 protected:
  virtual ~Snapshot();
};

// A range of keys
// key的范围，包含start，不包含limit。左闭右开。
struct LEVELDB_EXPORT Range {
  Range() = default;
  Range(const Slice& s, const Slice& l) : start(s), limit(l) {}

  Slice start;  // Included in the range
  Slice limit;  // Not included in the range
};

// A DB is a persistent ordered map from keys to values.
// A DB is safe for concurrent access from multiple threads without
// any external synchronization.
// 一个DB对象是一个持久化的有序map，它将key映射到value。
// DB对象可以安全地被多个线程并发访问，而不需要外部同步机制。
// 注：其具体实现是DBImpl类。在db_impl.h中。
class LEVELDB_EXPORT DB {
 public:
  // Open the database with the specified "name".
  // Stores a pointer to a heap-allocated database in *dbptr and returns
  // OK on success.
  // Stores nullptr in *dbptr and returns a non-OK status on error.
  // Caller should delete *dbptr when it is no longer needed.
  // 用指定的"name"打开数据库。
  // 将一个指向堆分配的数据库的指针存储在*dbptr中，并在成功时返回OK。
  // 在错误时，将nullptr存储在*dbptr中，并返回一个非OK的状态。
  // 当不再需要*dbptr时，调用者应该删除*dbptr。
  static Status Open(const Options& options, const std::string& name,
                     DB** dbptr);

  DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual ~DB();

  // Set the database entry for "key" to "value".  Returns OK on success,
  // and a non-OK status on error.
  // Note: consider setting options.sync = true.
  // 为"key"设置数据库条目为"value"。成功时返回OK，错误时返回非OK状态。
  // 注意：考虑设置options.sync = true。
  virtual Status Put(const WriteOptions& options, const Slice& key,
                     const Slice& value) = 0;

  // Remove the database entry (if any) for "key".  Returns OK on
  // success, and a non-OK status on error.  It is not an error if "key"
  // did not exist in the database.
  // Note: consider setting options.sync = true.
  // 删除"key"的数据库条目（如果有的话）。成功时返回OK，错误时返回非OK状态。
  // 如果"key"在数据库中不存在，这不是一个错误。
  // 注意：考虑设置options.sync = true。
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

  // Apply the specified updates to the database.
  // Returns OK on success, non-OK on failure.
  // Note: consider setting options.sync = true.
  // 将指定的更新应用到数据库。
  // 成功时返回OK，失败时返回非OK状态。
  // 注意：考虑设置options.sync = true。
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // If the database contains an entry for "key" store the
  // corresponding value in *value and return OK.
  //
  // If there is no entry for "key" leave *value unchanged and return
  // a status for which Status::IsNotFound() returns true.
  //
  // May return some other Status on an error.
  // 如果数据库包含"key"的条目，则将相应的value存储在*value中，并返回OK。
  // 如果没有"key"的条目，则保持*value不变，并返回一个Status，其中Status::IsNotFound()返回true。
  // 可能会在错误时返回其他Status。
  virtual Status Get(const ReadOptions& options, const Slice& key,
                     std::string* value) = 0;

  // Return a heap-allocated iterator over the contents of the database.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  // 返回一个堆分配的迭代器，用于遍历数据库的内容。
  // NewIterator()的结果最初是无效的（调用者必须在使用之前调用迭代器的Seek方法之一）。
  // 
  // Caller should delete the iterator when it is no longer needed.
  // The returned iterator should be deleted before this db is deleted.
  // 当不再需要迭代器时，调用者应该删除它。
  // 返回的迭代器应该在删除这个db之前被删除。
  virtual Iterator* NewIterator(const ReadOptions& options) = 0;

  // Return a handle to the current DB state.  Iterators created with
  // this handle will all observe a stable snapshot of the current DB
  // state.  The caller must call ReleaseSnapshot(result) when the
  // snapshot is no longer needed.
  // 返回当前DB状态的句柄。使用这个句柄创建的迭代器都将观察到当前DB状态的稳定快照。
  // 当不再需要快照时，调用者必须调用ReleaseSnapshot(result)。
  virtual const Snapshot* GetSnapshot() = 0;

  // Release a previously acquired snapshot.  The caller must not
  // use "snapshot" after this call.
  // 释放先前获取的快照。调用者在此调用后不能使用"snapshot"。
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // DB implementations can export properties about their state
  // via this method.  If "property" is a valid property understood by this
  // DB implementation, fills "*value" with its current value and returns
  // true.  Otherwise returns false.
  // DB实现可以通过这种方法导出关于它们状态的属性。
  // 若"property"可以被DB实现理解为一个有效的属性，则用它的当前值填充"*value"并返回true。
  // 否则返回false。
  //
  //
  // Valid property names include:
  // 有效的属性名包括：
  //
  //  "leveldb.num-files-at-level<N>" - return the number of files at level <N>,
  //     where <N> is an ASCII representation of a level number (e.g. "0").
  //  "leveldb.num-files-at-level<N>" - 返回level <N>上的文件数量，
  //     其中<N>是一个level编号的ASCII表示（例如"0"）。
  // 
  //  "leveldb.stats" - returns a multi-line string that describes statistics
  //     about the internal operation of the DB.
  //  "leveldb.stats" - 返回一个多行字符串，描述了DB内部操作的统计信息。
  // 
  //  "leveldb.sstables" - returns a multi-line string that describes all
  //     of the sstables that make up the db contents.
  //  "leveldb.sstables" - 返回一个多行字符串，描述了构成db内容的所有sstables。
  // 
  //  "leveldb.approximate-memory-usage" - returns the approximate number of
  //     bytes of memory in use by the DB.
  //  "leveldb.approximate-memory-usage" - 返回DB使用的内存的近似字节数。
  virtual bool GetProperty(const Slice& property, std::string* value) = 0;

  // For each i in [0,n-1], store in "sizes[i]", the approximate
  // file system space used by keys in "[range[i].start .. range[i].limit)".
  // 对于[0,n-1]中的每个i，将"[range[i].start .. range[i].limit)"中的key使用的近似文件系统空间大小存储在"sizes[i]"中。
  //
  // Note that the returned sizes measure file system space usage, so
  // if the user data compresses by a factor of ten, the returned
  // sizes will be one-tenth the size of the corresponding user data size.
  // 注意，返回的大小是衡量文件系统空间使用情况的，因此如果用户数据压缩了十倍，返回的大小将是相应用户数据大小的十分之一。
  //
  // The results may not include the sizes of recently written data.
  // 结果可能不包括最近写入数据的大小。
  virtual void GetApproximateSizes(const Range* range, int n,
                                   uint64_t* sizes) = 0;

  // Compact the underlying storage for the key range [*begin,*end].
  // In particular, deleted and overwritten versions are discarded,
  // and the data is rearranged to reduce the cost of operations
  // needed to access the data.  This operation should typically only
  // be invoked by users who understand the underlying implementation.
  // 压缩负责存储key范围为[*begin,*end]的底层存储空间。
  // 特别地，删除和覆盖的版本被丢弃，并且数据被重新排列以减少访问数据所需的操作成本。
  // 这个操作通常只应该被了解底层实现的用户调用。
  //
  // begin==nullptr is treated as a key before all keys in the database.
  // end==nullptr is treated as a key after all keys in the database.
  // Therefore the following call will compact the entire database:
  //    db->CompactRange(nullptr, nullptr);
  // begin==nullptr被视为数据库中所有key之前的key。
  // end==nullptr被视为数据库中所有key之后的key。
  // 因此，以下调用将压缩整个数据库：
  //    db->CompactRange(nullptr, nullptr);
  virtual void CompactRange(const Slice* begin, const Slice* end) = 0;
};

// Destroy the contents of the specified database.
// Be very careful using this method.
// 销毁指定数据库的内容。一定要非常小心使用这个方法。
//
// Note: For backwards compatibility, if DestroyDB is unable to list the
// database files, Status::OK() will still be returned masking this failure.
// 注意：为了向后兼容，如果DestroyDB无法列出数据库文件，将仍然返回Status::OK()，掩盖这个失败。
LEVELDB_EXPORT Status DestroyDB(const std::string& name,
                                const Options& options);

// If a DB cannot be opened, you may attempt to call this method to
// resurrect as much of the contents of the database as possible.
// Some data may be lost, so be careful when calling this function
// on a database that contains important information.
// 如果无法打开DB，可以尝试调用这个方法，以尽可能恢复数据库的内容。
// 一些数据可能会丢失，因此在对包含重要信息的数据库调用此函数时要小心。
LEVELDB_EXPORT Status RepairDB(const std::string& dbname,
                               const Options& options);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_DB_H_
