// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DB_IMPL_H_
#define STORAGE_LEVELDB_DB_DB_IMPL_H_

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/snapshot.h"
#include <atomic>
#include <deque>
#include <set>
#include <string>

#include "leveldb/db.h"
#include "leveldb/env.h"

#include "port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  ~DBImpl() override;

  // Implementations of the DB interface
  Status Put(const WriteOptions&, const Slice& key,
             const Slice& value) override;
  Status Delete(const WriteOptions&, const Slice& key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key,
             std::string* value) override;
  Iterator* NewIterator(const ReadOptions&) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;
  bool GetProperty(const Slice& property, std::string* value) override;
  void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) override;
  void CompactRange(const Slice* begin, const Slice* end) override;

  // Extra methods (for testing) that are not in the public DB interface
  // 用于测试的额外方法，不在公共DB接口中

  // Compact any files in the named level that overlap [*begin,*end]
  // 将指定level中与[*begin,*end]重叠的文件压缩。用于测试CompactRange方法。
  void TEST_CompactRange(int level, const Slice* begin, const Slice* end);

  // Force current memtable contents to be compacted.
  // 强制当前的memtable内容被压缩。用于测试CompactMemTable方法。
  Status TEST_CompactMemTable();

  // Return an internal iterator over the current state of the database.
  // The keys of this iterator are internal keys (see format.h).
  // The returned iterator should be deleted when no longer needed.
  // 返回一个迭代器，用于遍历数据库的当前状态。迭代器的key是InternalKey对象(见format.h)。
  // 当不再需要时，应删除返回的迭代器。
  Iterator* TEST_NewInternalIterator();

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  // 返回level>=1的任何文件在下一个level上的最大重叠数据(以字节为单位)。
  int64_t TEST_MaxNextLevelOverlappingBytes();

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.
  // 在指定的内部键处记录读取的字节的样本。查看该key是否存在无效的SSTable读取。
  // 若存在则更新无效文件的FileMetaData的allowed_seeks字段。是seek compaction的一部分。
  // 每隔config::kReadBytesPeriod字节左右记录一次样本。
  void RecordReadSample(Slice key);

 private:
  friend class DB;
  struct CompactionState;
  struct Writer;

  // Information for a manual compaction
  // 用于记录手动compaction的信息，会规定level，begin，end等信息。
  struct ManualCompaction {
    int level;
    bool done;
    const InternalKey* begin;  // null means beginning of key range
    const InternalKey* end;    // null means end of key range
    InternalKey tmp_storage;   // Used to keep track of compaction progress
  };

  // Per level compaction stats.  stats_[level] stores the stats for
  // compactions that produced data for the specified "level".
  // 每个level的compaction统计信息。stats_[level]存储了为指定“level”生成数据的compaction的统计信息。
  struct CompactionStats {
    CompactionStats() : micros(0), bytes_read(0), bytes_written(0) {}

    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }

    // 用于记录compaction操作的时间，读取的字节数，写入的字节数。
    int64_t micros;
    // 记录本次compaction操作所有输入文件的总字节数。
    int64_t bytes_read;
    // 用于记录compaction操作生成文件的大小。
    int64_t bytes_written;
  };

  Iterator* NewInternalIterator(const ReadOptions&,
                                SequenceNumber* latest_snapshot,
                                uint32_t* seed);

  Status NewDB();

  // Recover the descriptor from persistent storage.  May do a significant
  // amount of work to recover recently logged updates.  Any changes to
  // be made to the descriptor are added to *edit.
  // 从持久存储中恢复描述符。可能需要做大量工作来恢复最近记录的更新。
  // 要对描述符进行的任何更改都将添加到*edit中。
  Status Recover(VersionEdit* edit, bool* save_manifest)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void MaybeIgnoreError(Status* s) const;

  // Delete any unneeded files and stale in-memory entries.
  // 删除任何不需要的文件和过时的内存条目。在完成compaction操作后调用。
  void RemoveObsoleteFiles() EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Compact the in-memory write buffer to disk.  Switches to a new
  // log-file/memtable and writes a new descriptor iff successful.
  // Errors are recorded in bg_error_.
  // 将内存中的写缓冲区压缩到磁盘。如果成功，切换到新的log文件/memtable并写入新的描述符。
  // 错误将记录在bg_error_中。是minor compaction操作的上层接口之一。
  void CompactMemTable() EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // 崩溃恢复，用于读取上次未形成sstable文件的log文件，将其中的操作重新应用到memtable中。
  Status RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest,
                        VersionEdit* edit, SequenceNumber* max_sequence)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // 将当前的memtable转换为磁盘上的sstable文件，并将其加入到VersionSet中。
  // 是真正负责执行minor compaction操作的函数。
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // 确保memtable中有足够的空间来写入新的数据。
  Status MakeRoomForWrite(bool force /* compact even if there is room? */)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  WriteBatch* BuildBatchGroup(Writer** last_writer)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void RecordBackgroundError(const Status& s);
  // 尝试安排一个在后台运行的compaction任务
  // EXCLUSIVE_LOCKS_REQUIRED(mutex_),这是一种锁的注解，表示这个函数在调用时必顶持有mutex_锁。
  // 可以是调用者持有锁，也可以是函数内部持有锁。这里表现为调用该函数时，调用者必须持有mutex_锁。
  void MaybeScheduleCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  static void BGWork(void* db);
  void BackgroundCall();
  // 负责调度所有类型的Compaction操作，决定是否进行compaction操作。
  // 优先级：minor compaction > manual compaction > major compaction
  // 在major compaction中，size compaction > seek compaction
  void BackgroundCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void CleanupCompaction(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // 当Compaction对象构造完毕后，调用该函数进行实际的compaction操作。
  // 该函数是真正执行compaction操作的入口函数，主要实现了Major Compaction的逻辑。
  Status DoCompactionWork(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // 为当前compaction操作打开一个新的输出的SSTable文件编号，并创建一个TableBuilder对象。
  Status OpenCompactionOutputFile(CompactionState* compact);
  // 将当前已经从input中读取到TableBuilder中的这些kv对构造成一个sstable文件。input中可能还有剩余的kv对。
  // 当系统在compaction过程中发现当前已经遍历的kv对和其grandparents中的kv对重叠总文件字节超过了阈值时，
  // 会调用该函数将当前的kv对构造成一个sstable文件。以防止重叠的kv对过多导致的读放大问题。
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  // 将CompactState对象中的待删除的输入文件和新生成的文件同步到VersionEdit对象中。
  // 最后调用VersionSet对象的LogAndApply方法将VersionEdit对象中的变更应用到当前的Version对象中。
  Status InstallCompactionResults(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }

  // Constant after construction
  // 每个DB在初始化时都会初始化下面的几个成员变量，这些成员变量在DB的整个生命周期中都是不变的。
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const InternalFilterPolicy internal_filter_policy_;
  const Options options_;  // options_.comparator == &internal_comparator_
  const bool owns_info_log_;
  const bool owns_cache_;
  const std::string dbname_;

  // table_cache_ provides its own synchronization
  // 用于缓存sstable文件的TableCache对象。该对象本身提供了自己的同步机制。
  TableCache* const table_cache_;

  // Lock over the persistent DB state.  Non-null iff successfully acquired.
  // 用于保护数据库的状态，当成功获取时，db_lock_不为空。
  FileLock* db_lock_;

  // State below is protected by mutex_
  port::Mutex mutex_;
  // 用于标记是否正在关闭数据库。
  std::atomic<bool> shutting_down_;
  port::CondVar background_work_finished_signal_ GUARDED_BY(mutex_);

  // mem_是当前正在被写入的MemTable对象
  MemTable* mem_;
  // imm_是正在被压缩的MemTable对象
  MemTable* imm_ GUARDED_BY(mutex_);  // Memtable being compacted
  // 用于标记imm_是否为空，以便后台线程检测到非空的imm_
  std::atomic<bool> has_imm_;  // So bg thread can detect non-null imm_

  // 用于记录当前的log文件和log文件的Writer对象。
  // 表示数据库当前正在写入的Log文件的对象。
  WritableFile* logfile_;
  // 表示当前正在写入的Log文件的编号。
  uint64_t logfile_number_ GUARDED_BY(mutex_);
  // 是一个用于写入Log文件的Writer对象，需要用一个WritableFile对象作为参数。
  log::Writer* log_;
  uint32_t seed_ GUARDED_BY(mutex_);  // For sampling.

  // Queue of writers.
  // 存储了待写入的Writer对象，按照先进先出的顺序进行写入。
  std::deque<Writer*> writers_ GUARDED_BY(mutex_);
  WriteBatch* tmp_batch_ GUARDED_BY(mutex_);

  // 快照列表，用于存储LevelDB数据库中的不同版本的状态。
  SnapshotList snapshots_ GUARDED_BY(mutex_);

  // Set of table files to protect from deletion because they are
  // part of ongoing compactions.
  // 用于存储正在进行的compaction操作中涉及到的sstable文件的编号。以防止这些文件被删除。
  std::set<uint64_t> pending_outputs_ GUARDED_BY(mutex_);

  // Has a background compaction been scheduled or is running?
  // 用于标记是否已经安排或者有一个后台线程正在进行compaction操作。
  bool background_compaction_scheduled_ GUARDED_BY(mutex_);

  // 当前正在进行的manual compaction操作。
  ManualCompaction* manual_compaction_ GUARDED_BY(mutex_);

  // 当前数据库的version set对象。
  VersionSet* const versions_ GUARDED_BY(mutex_);

  // Have we encountered a background error in paranoid mode?
  // 用于标记是否在paranoid模式下遇到了后台错误。
  Status bg_error_ GUARDED_BY(mutex_);

  // 用于记录每个level的compaction操作的统计信息。
  CompactionStats stats_[config::kNumLevels] GUARDED_BY(mutex_);
};

// Sanitize db options.  The caller should delete result.info_log if
// it is not equal to src.info_log.
Options SanitizeOptions(const std::string& db,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DB_IMPL_H_
