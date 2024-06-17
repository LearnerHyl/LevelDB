// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"

#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
// 为每个等待的写入者保留的信息
struct DBImpl::Writer {
  explicit Writer(port::Mutex* mu)
      : batch(nullptr), sync(false), done(false), cv(mu) {}

  Status status;      // 用于存储写入操作的状态信息
  WriteBatch* batch;  // 指向待写入的批处理数据（WriteBatch）的指针
  bool sync;          // 是否同步写入(即是否需要等待写入完成)
  bool done;          // 是否写入完成
  port::CondVar cv;  // 条件变量,用于等待写入完成
};

// 保存Compaction操作结果的统计信息
struct DBImpl::CompactionState {
  // Files produced by compaction
  // Compaction操作产生的输出文件
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };

  // 获取当前Compaction操作正在处理的输出文件
  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        smallest_snapshot(0),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {}

  // Compaction对象，用于描述本次compaction操作的相关信息，
  // 如目标level，level和level+1层参与compaction的文件等
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  // 小于smallest_snapshot的序列号不重要，因为我们永远不会服务于小于smallest_snapshot的快照。
  // 因此，如果我们看到一个序列号S <= smallest_snapshot，我们可以删除所有具有序列号<S的相同user_key的条目。
  // 用于记录当前Compaction操作的最小序列号。快照本质上是一个序列号，表示在这个序列号之前的所有操作都已经完成。
  SequenceNumber smallest_snapshot;

  // 保存本次Compaction操作产生的所有输出文件
  std::vector<Output> outputs;

  // State kept for output being generated
  // 用于生成输出文件的状态，构建出SSTable对象后，需要将其写入到磁盘文件中
  WritableFile* outfile;
  // 将compaction操作的输出文件构建为SSTable文件的TableBuilder对象
  TableBuilder* builder;

  // 本次Compaction操作产生的总字节数
  uint64_t total_bytes;
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
  ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  if (result.info_log == nullptr) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

static int TableCacheSize(const Options& sanitized_options) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_,
                               &internal_comparator_)) {}

DBImpl::~DBImpl() {
  // Wait for background work to finish.
  mutex_.Lock();
  shutting_down_.store(true, std::memory_order_release);
  while (background_compaction_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->RemoveFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::RemoveObsoleteFiles() {
  mutex_.AssertHeld();

  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // Make a set of all of the live files
  // 创建一个存储所有存活文件编号的集合
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  // 将数据库目录下的所有文件名存储到filenames中
  env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
  uint64_t number; // 文件序列号
  FileType type; // 文件类型
  std::vector<std::string> files_to_delete; // 保存可以删除的文件名
  for (std::string& filename : filenames) {
    if (ParseFileName(filename, &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          // 对于记录kv操作的WAL文件，只保留最新的WAL文件和PrevLogNumber对应的WAL文件，
          // 删除文件序列号小于log_number_并且不等于prev_log_number_的WAL文件
          keep = ((number >= versions_->LogNumber()) ||
                  (number == versions_->PrevLogNumber()));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          // 保留当前最新的MANIFEST文件，以及任何更新的MANIFEST文件（以防存在允许其他版本的竞争）
          // 当前正在使用的MANIFEST文件就是我们需要的最旧的MANIFEST文件
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case kTableFile:
          // SSTable文件，只要不在live集合中的SSTable文件都可以删除。
          // 即没有参与当前的Compaction操作且不在活跃的版本中的SSTable文件都可以删除。
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          // 任何当前正在写入的临时文件都必须记录在pending_outputs_中，这些文件会被插入到“live”中。
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        // 更新files_to_delete，将可以删除的文件名加入到files_to_delete中，
        files_to_delete.push_back(std::move(filename));
        if (type == kTableFile) { // 将可以删除的SSTable文件从table_cache_中删除
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
            static_cast<unsigned long long>(number));
      }
    }
  }

  // While deleting all files unblock other threads. All files being deleted
  // have unique names which will not collide with newly created files and
  // are therefore safe to delete while allowing other threads to proceed.
  mutex_.Unlock();
  for (const std::string& filename : files_to_delete) {
    env_->RemoveFile(dbname_ + "/" + filename);
  }
  mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  // 忽略CreateDir的错误，因为只有在创建descriptor时才会提交DB的创建，而且这个目录可能已经存在于之前的创建尝试失败中。
  // 创建数据库目录，若目录已存在则忽略错误
  env_->CreateDir(dbname_);
  assert(db_lock_ == nullptr);
  // 互斥锁，用于保护数据库文件
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  if (!env_->FileExists(CurrentFileName(
          dbname_))) {  // 若当前目录下不存在CURRENT文件，则说明数据库不存在
    if (options_.create_if_missing) {
      Log(options_.info_log, "Creating DB %s since it was missing.",
          dbname_.c_str());
      s = NewDB();  // 新建数据库
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(dbname_,
                                     "exists (error_if_exists is true)");
    }
  }

  // 读取当前最新版本的MANIFEST文件，进行元数据恢复
  s = versions_->Recover(save_manifest);
  if (!s.ok()) {
    return s;
  }
  SequenceNumber max_sequence(0);

  // Recover from all newer log files than the ones named in the
  // descriptor (new log files may have been added by the previous
  // incarnation without registering them in the descriptor).
  // 只从比descriptor中命名的log文件更新的log文件中恢复（新的log文件可能已经被之前的版本添加，但没有在descriptor中注册）。
  // 从上一次的崩溃后最新的MANIFEST文件中读取的log文件编号，我们只会从比这个编号大的log文件中恢复。
  // 因为比这个编号小的log文件已经成功的以SSTable文件的形式写入到磁盘中了，意味着编号小的log文件已经被成功的应用到了数据库中。
  //
  // Note that PrevLogNumber() is no longer used, but we pay
  // attention to it in case we are recovering a database
  // produced by an older version of leveldb.
  // 注意，PrevLogNumber()不再使用，但是我们在恢复由旧版本的leveldb生成的数据库时会注意到它。
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log =
      versions_
          ->PrevLogNumber();  // 用于记录上一次的崩溃后最新的MANIFEST文件中读取的log文件编号
  std::vector<std::string> filenames;
  // 获取数据库目录下的所有文件名
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  // 遍历数据库目录下的所有文件名，只留下版本号大于等于最新MANIFEST文件中记录的log文件编号的log文件
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }
  // 遍历完一遍后，expected中剩下的文件编号是在MANIFEST文件中记录的，但是在数据库目录下没有找到的文件编号
  // 说明这些文件可能丢失了或者损坏了，需要报错
  if (!expected.empty()) {
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%d missing files; e.g.",
                  static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }

  // Recover in the order in which the logs were generated
  // 按照生成log的顺序进行恢复
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      return s;
    }

    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    // 之前的版本可能在分配这个log
    // number之后没有写入任何MANIFEST记录。所以我们手动更新VersionSet中的文件编号分配计数器。
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // null if options_.paranoid_checks==false
    void Corruption(size_t bytes, const Status& s) override {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""), fname,
          static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  // 日志文件的命名规则为dbname_log_number.log
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  // 尝试打开日志文件
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  // 我们故意让log::Reader进行校验，即使paranoid_checks==false;这样即使出现损坏，也会跳过整个提交，而不是传播错误信息（如过大的序列号）。
  log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long)log_number);

  // Read all the records and add to a memtable
  // 读取所有记录并添加到memtable中
  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;  // 记录当前Log文件对应的MemTable触发的Compaction次数
  // 每个Log文件对应一个MemTable
  MemTable* mem = nullptr;
  // 从Log文件中依次读取log record
  while (reader.ReadRecord(&record, &scratch) && status.ok()) {
    // 若读取的log record小于12字节，则认为是损坏的，跳过
    // TODO:为什么是12字节？
    // 猜测：kheaderSize是7字节，假设content部分只有一个Delete操作，那么最少也要5字节，所以12字节是一个比较合理的值
    // 因此，如果读取的log record小于12字节，就认为是损坏的，跳过
    if (record.size() < 12) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      continue;
    }
    // 从log record中解析出对应的WriteBatch对象
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == nullptr) {  // 按照DB启动时设定的comparator创建一个新的MemTable
      mem = new MemTable(internal_comparator_);
      mem->Ref();  // 引用计数+1
    }
    // 将WriteBatch中的操作插入到MemTable中，遍历并应用WriteBatch中的所有操作
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    // 应用完这批操作后，更新当前MemTable的最大操作序列号
    // 计算当前WriteBatch中的最后一个操作的序列号
    const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
                                    WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    // 如果MemTable的内存使用量超过了设定的阈值，会触发MemTable的Compaction操作，将MemTable转化为SSTable
    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      // 执行minor compaction操作，将MemTable转化为SSTable
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.ok()) {  // 若Compaction操作失败，则直接返回
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        // 立即反映错误，以便像文件系统已满这样的条件导致DB::Open()失败。
        break;
      }
    }
  }

  delete file;

  // See if we should keep reusing the last log file.
  // 检查是否应该继续重用最后一个log文件。
  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == nullptr);
    assert(log_ == nullptr);
    assert(mem_ == nullptr);
    uint64_t lfile_size;
    // 若名为fname的文件存在且大小不为0，且在该文件后面可以追加写入，则重用该文件
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      // 将重用的文件的初始大小设置为lfile_size，即在该文件后面可以追加写入
      log_ = new log::Writer(logfile_, lfile_size);
      // 更新日志文件序号
      logfile_number_ = log_number;
      // 若当前日志文件的memtable有数据，则将其转移到当前的DBImpl对象中
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {  // 若当前日志文件的memtable为空，则创建一个新的memtable
        // mem can be nullptr if lognum exists but was empty.
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }

  // 若mem不为空，说明并没有重用这次操作使用的log文件
  if (mem != nullptr) {
    // mem did not get reused; compact it.
    // 做一次Compaction操作，将其持久化为磁盘上的SSTable文件
    if (status.ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }

  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  // 每个MemTable对应一个SSTable文件，生成一个新的SSTable文件序号
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  // 生成当前memtable的迭代器，本质上是调用底层的SkipList的迭代器，
  // 遍历当前immutable memtable中的所有kv对
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long)meta.number);

  Status s;
  {
    // I/O操作开始后，释放锁，允许其他线程继续操作
    mutex_.Unlock();
    // 根据，传入的memtable迭代器，生成一个新的SSTable文件
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long)meta.number, (unsigned long long)meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  // 如果file_size为零，则文件已被删除，不应将其添加到manifest中。
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      // 根据由minor compaction操作新生成的SSTable文件的最小和最大user_key，选择该SSTable
      // 文件应该被放置在哪个level中
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    // 在VersionEdit对象的对应level中记录该SSTable文件的元数据信息
    edit->AddFile(level, meta.number, meta.file_size, meta.smallest,
                  meta.largest);
  }

  // 记录本次Compaction操作的统计信息，并加入到相应的level中
  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);

  // Save the contents of the memtable as a new Table
  // 对于minor compaction，同样是用current_version+versionEdit的方法来记录compaction操作
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  // 开始执行minor compaction，将imm_中的数据写入到SSTable文件中
  Status s = WriteLevel0Table(imm_, &edit, base);
  base->Unref();

  if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,
                               const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
         bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      background_work_finished_signal_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = nullptr;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // nullptr batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != nullptr && bg_error_.ok()) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.SignalAll();
  }
}
// 尝试安排一个在后台运行的compaction任务
// EXCLUSIVE_LOCKS_REQUIRED(mutex_),这是一种锁的注解，表示这个函数在调用时必顶持有mutex_锁。
// 可以是调用者持有锁，也可以是函数内部持有锁。这里表现为调用该函数时，调用者必须持有mutex_锁。
void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (background_compaction_scheduled_) {  // 已经安排了后台压缩任务，不需要再安排
    // Already scheduled
  } else if (
      shutting_down_.load(
          std::
              memory_order_acquire)) {  // 数据库正在被删除，不需要再进行后台压缩
    // DB is being deleted; no more background compactions
  } else if (!bg_error_.ok()) {  // 后台压缩任务出现错误，不需要再进行后台压缩
    // Already got an error; no more changes
  } else if (
      imm_ == nullptr && manual_compaction_ == nullptr &&
      !versions_
           ->NeedsCompaction()) {  // 没有需要压缩的文件，不需要再进行后台压缩
    // No work to be done
  } else {  // 上述条件都不满足，则说明存在需要进行压缩的文件，需要安排后台压缩任务
    background_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  MutexLock l(&mutex_);
  assert(background_compaction_scheduled_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    // No more background work when shutting down.
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    BackgroundCompaction();
  }

  background_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  MaybeScheduleCompaction();
  background_work_finished_signal_.SignalAll();
}
void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();

  if (imm_ != nullptr) {
    // 若存在immutable memtable，则进行minor compaction操作
    // 将该Immutable MemTable转化为SSTable文件
    CompactMemTable();
    return;
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != nullptr);
  InternalKey manual_end;
  // 若是手动触发的Compaction操作
  if (is_manual) {
    // 根据规定的compaction信息，设置一个Compaction对象，用于后续的Compaction操作
    ManualCompaction* m = manual_compaction_;
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == nullptr);
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    // 若不是手动触发的Compaction操作，则根据当前的版本信息，让系统自动选择compaction的level和范围。
    c = versions_->PickCompaction();
  }
  // 到此，c中已经保存了需要进行Compaction操作的信息

  Status status;
  if (c == nullptr) {
    // Nothing to do
  } else if (!is_manual && c->IsTrivialMove()) { // 非手动触发的Compaction操作，且是Trivial Move，具体见version_set.cc
    // Move file to next level
    // 这意味着本次压缩操作只是将一个文件从一个level移动到下一个level，不需要进行文件合并/拆分等操作
    // 本次compaction的Level-0层只允许有一个参与compaction的文件
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    // 将文件f从level-0层移动到level-1层，直接操作即可，不需要进行文件合并/拆分等操作
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
                       f->largest);
    // 将产生的version edit应用到当前版本中，产生新的版本
    status = versions_->LogAndApply(c->edit(), &mutex_);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number), c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(), versions_->LevelSummary(&tmp));
  } else { // 非Trivial Move，需要进行文件合并/拆分等操作;
    // 新建CompactionState对象，用于保存Compaction操作的状态信息
    CompactionState* compact = new CompactionState(c);
    // 执行Compaction操作
    status = DoCompactionWork(compact);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    // 释放CompactionState对象内存
    CleanupCompaction(compact);
    // 释放对本次compaction操作中的版本信息的引用
    c->ReleaseInputs();
    // 删除任何不再需要的文件和过时的内存条目
    RemoveObsoleteFiles();
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = nullptr;
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  // 若CompactionState对象中的builder对象不为空，则放弃该builder对象
  if (compact->builder != nullptr) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == nullptr);
  }
  // 删除compact对象中的outfile对象，是SSTable文件的输出文件的内存形式
  delete compact->outfile;
  // 清空pending_outputs_中的compact对象中的输出文件编号，因为这些文件已经写入磁盘了
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  // 删除compact对象
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    // 访问元数据时需要加锁
    mutex_.Lock();
    // 生成一个新的SSTable文件序号
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    // 更新CompactionState对象中的outputs信息
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

  // Make the output file
  // 将新生成的SSTable文件打开，compact->outfile指向该文件
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    // 若上述操作都成功了，为该SSTable文件生成一个builder对象，用于在outfile对象上按照
    // SSTable文件的格式构造SSTable文件
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  assert(compact != nullptr);
  assert(compact->outfile != nullptr);
  assert(compact->builder != nullptr);

  // 当前正在处理的文件序列号
  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  // 获取当前builder对象中已经写入的kv对数量
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    // 将当前已经遍历完毕的kv对写入到SSTable文件中
    s = compact->builder->Finish();
  } else {
    // 如果在遍历过程中出现错误，则放弃当前builder对象
    compact->builder->Abandon();
  }
  // 上面Finish()函数生成的SSTable文件的大小
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  // Finish and check for file errors
  // 将SSTable文件刷入磁盘
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  // 若写入成功，则打印日志
  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    // 若当前SSTable文件写入成功，则将其加入到table_cache_中，方便后面访问
    Iterator* iter =
        table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long)output_number, compact->compaction->level(),
          (unsigned long long)current_entries,
          (unsigned long long)current_bytes);
    }
  }
  return s;
}

// 将CompactState对象中的待删除的输入文件和新生成的文件同步到VersionEdit对象中。
  // 最后调用VersionSet对象的LogAndApply方法将VersionEdit对象中的变更应用到当前的Version对象中。
Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1), compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  // 本次参与compaction的输入文件在compaction操作完成后要被删除
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  // 将compaction操作生成的SSTable文件加入到VersionEdit对象中，这些文件在新版本中都会被放到level+1层
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
                                         out.smallest, out.largest);
  }
  // 到此version edit中已经记录了compaction操作的结果，接下来将其应用到当前版本中
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

// 当compaction对象构造完毕后，调用DoCompactionWork()函数执行compaction操作，
// 该函数是真正执行compaction操作的入口函数，主要实现了Major Compaction的逻辑。
Status DBImpl::DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  // 用于记录minor compaction操作的耗时
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  Log(options_.info_log, "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  // 很明显，compaction操作被执行前，首先目标level必须有文件，builder对象和outfile对象必须为空
  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  // 不能有用于apply version edit的builder对象，因为操作还未开始
  assert(compact->builder == nullptr);
  // 不能有compaction输出文件
  assert(compact->outfile == nullptr);
  // 为当前compaction操作设置最小快照，即最小的操作序列号，其实每个快照对象
  // 本质上只是封装了一个操作序列号-SequenceNumber
  // NOTE:这里的逻辑对理解下面的过滤kv操作的原理很重要
  if (snapshots_.empty()) {
    // 若当前数据库中没有快照记录，则最小快照为当前版本中的最大操作序列号
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    // 若当前数据库中有快照，则获取最旧的快照的操作序列号
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  // 返回一个MergeIterator对象，用于有序遍历level和level+1层文件中的所有kv对
  Iterator* input = versions_->MakeInputIterator(compact->compaction);

  // Release mutex while we're actually doing the compaction work
  // 释放锁，以便在执行compaction操作时，其他线程可以继续操作
  mutex_.Unlock();
  // 开始执行compaction操作
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  // 遍历level和level+1层文件中的所有kv对
  // 若input->Valid()为true；且shutting_down_为false，则继续遍历
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    // Prioritize immutable compaction work
    // 无论当前处理的是哪一层的文件，都优先处理immutable memtable的compaction操作，即先执行minor compaction操作
    // 优先处理immutable memtable的compaction操作，即先执行minor compaction操作
    if (has_imm_.load(std::memory_order_relaxed)) {
      const uint64_t imm_start = env_->NowMicros();
      // 在执行minor compaction操作期间，需要持有mutex_锁，以保证compaction操作的正确性
      mutex_.Lock();
      if (imm_ != nullptr) {
        // 执行minor compaction操作，将imm_中的数据写入到SSTable文件
        CompactMemTable();
        // Wake up MakeRoomForWrite() if necessary.
        // 唤醒MakeRoomForWrite()函数中由于Level0层文件数量过多而阻塞的线程
        background_work_finished_signal_.SignalAll();
      }
      mutex_.Unlock();
      // 记录minor compaction操作的耗时
      imm_micros += (env_->NowMicros() - imm_start);
    }

    // 处理本次compaction操作的kv对
    Slice key = input->key();
    // 若发现到当前key后，当前compaction操作已经遍历过的kv对与grandparent level中的存在kv重叠
    // 文件的总字节数超过了设定的阈值，则停止当前compaction操作，马上生成输出文件，以减少重叠文件的数量
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    // 处理当前kv对，将其添加到builder对象中
    bool drop = false; // 用于判断一个kv对是否可以删除，若为true，则该kv对不需要写入到输出文件中
    if (!ParseInternalKey(key, &ikey)) { // 解析当前kv对的InternalKey，若解析失败，则直接跳过
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      // 如果这是遍历的第一个kv对，或者当前kv对的user key与上一个kv对的user key不同，
      // 说明该kv对中的user key是第一次出现
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        // First occurrence of this user key
        // 更新current_user_key内容
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        // 更新最后一次见到的该user key的操作序列号，第一次遇到该user_key时，将其设置为kMaxSequenceNumber
        last_sequence_for_key = kMaxSequenceNumber;
      }

      // 若不是第一次见关于该user key的kv对，则比较最近见到的kv对(不包括当前kv对)的操作序列号)与
      // 本次compaction操作的最小快照，若小于等于最小快照，则当前kv对不需要写入到输出文件中
      // 考虑对key1的操作，smallest=100,第一次操作是操作为100的insert，根据代码逻辑，操作是一定会成功的。
      // 第二次操作为101，无论类型是什么，因为第一次操作为100, 根据下述逻辑，第二次操作会被drop掉。
      // 若还有第三次操作103，那么第三次操作会成功，因为第二次操作的seq大于smallest。
      // 这不会影响到最终查询结果，因为我们永远不会服务一个小于smallest的快照，因此100的insert是无效的，所以我们看不到这个数据。
      // 若第二次是delete，虽然他会被drop掉，但是结果是一样的。若第二次是insert，那么会暂时有不一致性，但是这个不一致性是中间状态。
      // 第三次一定会成功，所以从第三次开始，数据就会即时进入builder中。当次数大于3时，数据没问题，但是次数为2时，数据会被drop掉。
      // 问题在于，若只有两次操作，第一次是insert，第二次还是insert，那么第二次会被drop掉，这是不是就意味着数据丢失了呢？
      // FIXME:当某个user_key只有两次操作，且第一次操作等于smallest，第二次操作为insert时，第二次操作会被drop掉，这样会导致数据丢失。
      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        // 之前遍历到了一个版本号小于当前版本号的kv对，丢弃，之后会有一个更新的kv对覆盖它
        drop = true;  // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // 若当前kv对是一个删除操作，且其操作序列号小于等于compact->smallest_snapshot，
        // 且该kv对的user key的最早版本在level+1层中,即更高层不会再有关于该键的操作出现，
        // 那么该kv对不需要写入到输出文件中。
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        // 对于该user_key而言:
        // (1) 除了当前level，在更高的level中没有该user_key的数据
        // (2) 在更低的level中有更大的操作序列号
        // (3) 根据规则(A)可知，在这里被compaction的level中，有更小操作序列号的数据将在接下来的几次迭代中被删除。
        // 因此，这个删除标记是过时的，可以被删除。
        drop = true;
      }

      // 更新我们见到的当前key的最大操作序列号
      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    // 若该kv对需要被添加到输出文件中，则将其添加到builder对象中
    if (!drop) {
      // Open output file if necessary
      // 若builder对象为空，则打开一个新的SSTable文件,并将builder指向该文件
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      // 若当前key是该compaction操作处理的第一个key，则将其设置为当前输出文件的最小key
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      // 将kv对添加到builder对象中，并不断地更新当前输出文件的最大key
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());

      // Close output file if it is big enough
      // 若当前输出文件的大小超过了设定的阈值，则将其写入到磁盘中
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        // 阶段性地将当前输出的SSTable文件写入到磁盘中
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }
    // 继续遍历下一个kv对
    input->Next();
  }

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("Deleting DB during compaction");
  }
  // 若遍历完毕，但是当前输出文件中还有kv对，则将其以文件的形式写入到磁盘中
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  // 处理完毕，删除input对象
  delete input;
  input = nullptr;

  // 若compaction操作成功，则记录compaction操作的统计信息
  CompactionStats stats;
  // 记录compaction操作的耗时
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  // 记录本次compaction操作输入文件的总的字节数
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  // 记录本次compaction操作产生的所有输出文件的总的字节数
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  // 更新compaction操作的统计信息，本次compaction操作会被记录到level+1层
  stats_[compact->compaction->level() + 1].Add(stats);

  // 将待删除的文件和新生成的文件同步到VersionEdit对象中，并通过调用
  // VersionSet对象的LogAndApply方法将VersionEdit对象中的变更应用到当前的Version对象中
  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  // 打印compaction操作的统计信息，完成本次compaction操作
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

namespace {

struct IterState {
  port::Mutex* const mu;
  Version* const version GUARDED_BY(mu);
  MemTable* const mem GUARDED_BY(mu);
  MemTable* const imm GUARDED_BY(mu);

  IterState(port::Mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
      : mu(mutex), version(version), mem(mem), imm(imm) {}
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != nullptr) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key,
                   std::string* value) {
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  // 尝试获取Get操作对应的SequenceNumber
  if (options.snapshot !=
      nullptr) {  // 若options中指定了sequence number,则直接使用
    snapshot =
        static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  } else {  // 若没有指定,则使用当前的SequenceNumber
    snapshot = versions_->LastSequence();
  }

  // 获取对应的memtable和不可变的memtable,以及当前的Version
  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  // 为memtable、不可变的memtable和Version增加引用
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    // 先在memtable中查找,然后在不可变的memtable中查找(如果有的话)。
    LookupKey lkey(key, snapshot);  // 根据key和SequenceNumber创建LookupKey对象
    if (mem->Get(lkey, value, &s)) {  // 先从memtable中查找
      // Done
    } else if (imm != nullptr &&
               imm->Get(
                   lkey, value,
                   &s)) {  // 若memtable中没有找到,则从不可变的memtable中查找
      // Done
    } else {  // 若不可变的memtable中也没有找到,则从sstable文件中查找
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  // 如果在sstable文件中找到了,则更新统计信息，并根据情况进行压缩
  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  // 查找完毕后，释放对memtable、不可变的memtable和Version的引用
  // Note：当引用计数为0时，说明当前没有用户在使用该对象，可以触发GC操作
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(this, user_comparator(), iter,
                       (options.snapshot != nullptr
                            ? static_cast<const SnapshotImpl*>(options.snapshot)
                                  ->sequence_number()
                            : latest_snapshot),
                       seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock l(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  // 首先实例化一个Writer对象，用于记录当前写操作的相关信息
  Writer w(&mutex_);
  w.batch = updates;
  w.sync = options.sync;
  w.done = false;

  // 将当前写操作加入到writers_队列中
  MutexLock l(
      &mutex_);  // 注意这是一个ScopedLock对象，当离开作用域时，会自动释放锁
  writers_.push_back(&w);
  // 这里的同步方式很像CS162中讲述的Mesa调度模型，即使被唤醒了，也需要重新检查条件
  // 这确保了在当前写操作完成之前，不会有其他写操作被执行
  while (!w.done && &w != writers_.front()) {
    // 如果当前写操作不是队列中的第一个操作，则等待
    // 条件变量的wait()一般是睡眠当前线程，等待被唤醒
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  // 首先使用MakeRoomForWrite()函数来确保有足够的空间来执行写操作
  Status status = MakeRoomForWrite(updates == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer =
      &w;  // last_writer指向当前写操作，并不是writers_队列中的最后一个写操作
  if (status.ok() && updates != nullptr) {  // nullptr batch is for compactions
    WriteBatch* write_batch = BuildBatchGroup(&last_writer);
    // 将当前写操作的SequenceNumber设置为last_sequence+1，即起始值为last_sequence+1
    WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
    // 依据批量写操作的大小来更新last_sequence，当本次写操作成功后，这就是新的last_sequence值
    last_sequence += WriteBatchInternal::Count(write_batch);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    // 添加到日志文件中，并将其应用到memtable中。我们可以在这个阶段释放锁，因为&w当前负责记录日志，
    // 并保护记录操作免受并发的loggers和并发写入memtable的影响。
    // 本质在于，没有signal()唤醒其他写操作，并且上述的queue机制确保了只有头部的写操作才会被执行，单线程机制
    {
      mutex_.Unlock();
      // 将写操作记录到日志文件中，并将日志文件刷盘
      // 在此期间允许新的写操作被放入writers_队列中，但是不会被执行
      status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
      bool sync_error = false;
      // 根据WriteOptions中的sync参数来决定是否需要将日志文件实时的刷入到磁盘中(严格来说是磁盘的disk
      // cache中)
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      // 先将操作记录到日志文件中，然后再将操作应用到memtable中
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(write_batch, mem_);
      }
      mutex_.Lock();
      if (sync_error) {  // 若日志刷盘失败，即使本次写操作有部分成功，也需要将DB置为不可用状态
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        // 日志文件的状态是不确定的：我们刚刚添加的日志记录可能会在重新打开DB时显示，也可能不会显示。
        // 因此，我们强制DB进入一个所有未来写操作都失败的模式。
        RecordBackgroundError(status);
      }
    }
    // tmp_batch_是一个临时的WriteBatch对象，用于在写操作完成后清空
    if (write_batch == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }
  // 由于写入操作完成，因此需要将当前写操作从writers_队列中移除
  // 由于合并写入操作一次可能会处理多个writers_队列中的写操作，因此需要循环处理
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    // TODO:只有头部的写操作才会被执行，但是为什么这里会考虑有多个写操作同时完成的情况？
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    // 若当前pop的writer_刚好是本次写操作，则退出循环
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  // 若当前writers_队列不为空，则当前队列头部的写操作可以被执行，唤醒它
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  // 若不是强制写入，则允许延迟写入
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {  // 若后台线程出现错误，则不允许写入
      // Yield previous error
      s = bg_error_;
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >=
            config::kL0_SlowdownWritesTrigger) {  // 判断是否符合延迟写入的条件
      // 若允许延迟写入，且当前L0层的文件数量超过了阈值，说明需要backgroud线程进行compact操作，
      // 需要等到background线程完成compact操作后再进行写入，因此进行延迟

      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      // 我们接近达到L0文件数量的硬限制。与其在达到硬限制时延迟单个写操作几秒钟，
      // 不如在达到硬限制时延迟每个单独的写操作1ms，以减少延迟方差。
      // 此外，这种延迟将一些CPU交给压缩线程，以防它与写线程共享同一个核心。
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      // 对于同一个写操作，只允许延迟一次
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (
        !force &&
        (mem_->ApproximateMemoryUsage() <=
         options_.write_buffer_size)) {  // 当前Memtable是否有足够的空间进行写入
      // 若是非强制写入，并且当前memtable的内存使用量小于write_buffer_size，说明仍有足够的空间进行写入
      // There is room in current memtable
      break;
    } else if (imm_ != nullptr) {  // 是否有正在进行的compact操作的imm_，minor compaction操作
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      // 当前的mem_已经被写满，但是不可变的mem_仍在被压缩，因此需要等待压缩完毕后，才能交换imm_和mem_的内存
      Log(options_.info_log, "Current memtable full; waiting...\n");
      background_work_finished_signal_.Wait();
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      // L0层文件数量过多，等待，这里会被完成的minor compaction操作唤醒1
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      background_work_finished_signal_.Wait();
    } else {  // 到此，说明当前的memtable已经写满，且没有正在进行的compact操作，且L0层的文件数量没有超过阈值，因此需要进行compact操作
      // Attempt to switch to a new memtable and trigger compaction of old
      assert(versions_->PrevLogNumber() == 0);
      // 创建一个新的memtable，回收当前memtable使用的相关log对象的资源，最后将当前memtable设置为不可变的memtable，更新相关的对象

      // 这里表现为创建一个新的log文件，一个log文件对应一个memtable
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = nullptr;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        // 避免在一个紧凑的循环中耗尽文件编号空间。若创建失败，则保留该文件编号，以便下次使用
        versions_->ReuseFileNumber(new_log_number);
        break;
      }

      // 删除旧的log Writer对象，意味着不再旧的log文件中写入数据
      delete log_;

      s = logfile_->Close();
      if (!s.ok()) {
        // We may have lost some data written to the previous log file.
        // Switch to the new log file anyway, but record as a background
        // error so we do not attempt any more writes.
        // 可能会丢失写入到上一个日志文件的一些数据。无论如何切换到新的日志文件，但是记录为后台错误，
        // 这样我们就不会尝试进行更多的写操作。
        //
        // We could perhaps attempt to save the memtable corresponding
        // to log file and suppress the error if that works, but that
        // would add more complexity in a critical code path.
        // 我们可能会尝试保存与日志文件对应的memtable，并在这种情况下抑制错误，但是这将在关键代码路径中增加更多的复杂性。
        RecordBackgroundError(s);
      }
      delete logfile_;

      // 旧的memtable的资源回收没有问题，因此可以将其设置为不可变的memtable
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      imm_ = mem_;  // 将旧的memtable设置为不可变的memtable，启动minor compaction操作
      has_imm_.store(true, std::memory_order_release);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      // 新的memtable已经被创建，空间足够进行新的写入操作，因此不需要再强制进行另一个compact操作
      force = false;  // Do not force another compaction if have room
      // 尝试在后台开启一个compact操作
      MaybeScheduleCompaction();
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      std::snprintf(buf, sizeof(buf), "%d",
                    versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "                               Compactions\n"
                  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                  "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
                      level, files, versions_->NumLevelBytes(level) / 1048576.0,
                      stats_[level].micros / 1e6,
                      stats_[level].bytes_read / 1048576.0,
                      stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  // TODO(opt): better implementation
  MutexLock l(&mutex_);
  Version* v = versions_->current();
  v->Ref();

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  v->Unref();
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
// 如果子类想要调用，可以调用DB的默认实现
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  *dbptr = nullptr;

  // 创建一个DBImpl对象
  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.Lock();
  VersionEdit edit;
  // Recover handles create_if_missing, error_if_exists
  // Recover处理create_if_missing和error_if_exists
  bool save_manifest = false;
  // 尝试恢复之前存在的数据库数据
  Status s = impl->Recover(&edit, &save_manifest);
  // 若恢复操作成功，且MemTable为空，则创建新的Log和MemTable
  if (s.ok() && impl->mem_ == nullptr) {
    // Create new log and a corresponding memtable.
    // 创建新的Log和MemTable
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    // 创建新的Log文件
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {  // 若创建成功，则创建新的Log::Writer和MemTable
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
    }
  }
  // 若恢复操作成功，且save_manifest为true，则将VersionEdit对象写入MANIFEST文件
  // 注意数据库每应用一个VersionEdit对象，都会写入MANIFEST文件，这代表了数据库相关的元数据信息
  if (s.ok() && save_manifest) {
    edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }
  // 若前面的操作都成功了，调用RemoveObsoleteFiles():清理一些无用/过时的文件
  // 调用MaybeScheduleCompaction():检查是否需要进行Log Compaction操作
  if (s.ok()) {
    impl->RemoveObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }
  impl->mutex_.Unlock();
  if (s.ok()) {
    assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Snapshot::~Snapshot() = default;

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    // Ignore error in case directory does not exist
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->RemoveFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->RemoveFile(lockname);
    env->RemoveDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

}  // namespace leveldb
