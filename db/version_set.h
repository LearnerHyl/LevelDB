// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The representation of a DBImpl consists of a set of Versions.  The
// newest version is called "current".  Older versions may be kept
// around to provide a consistent view to live iterators.
// DBImpl的表示由一组版本组成。最新的版本称为“current”。可能会保留旧版本，以为活动迭代器提供一致的视图。
//
// Each Version keeps track of a set of Table files per level.  The
// entire set of versions is maintained in a VersionSet.
// 每个版本跟踪每个级别的一组表文件。整个版本集维护在VersionSet中。
//
// Version,VersionSet are thread-compatible, but require external
// synchronization on all accesses.
// Version和VersionSet是线程兼容的，但需要对所有访问进行外部同步。

#ifndef STORAGE_LEVELDB_DB_VERSION_SET_H_
#define STORAGE_LEVELDB_DB_VERSION_SET_H_

#include "db/dbformat.h"
#include "db/version_edit.h"
#include <map>
#include <set>
#include <vector>

#include "port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

namespace log {
class Writer;
}

class Compaction;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
// REQUIRES: "files" contains a sorted list of non-overlapping files.
// 返回最小的索引i，使得files[i]->largest >=
// key。如果没有这样的文件，则返回files.size()。
// 要求：“files”包含一个排序的不重叠文件列表。由于files是有序的，所以可以使用二分查找。
// 一般用于在某一层级的文件列表中查找出符合要求的文件。
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files, const Slice& key);

// Returns true iff some file in "files" overlaps the user key range
// [*smallest,*largest].
// smallest==nullptr represents a key smaller than all keys in the DB.
// largest==nullptr represents a key largest than all keys in the DB.
// REQUIRES: If disjoint_sorted_files, files[] contains disjoint ranges
//           in sorted order.
// 如果“files”中的某个文件与用户键范围[*smallest,*largest]重叠，则返回true。
// smallest==nullptr表示小于DB中所有键的键。
// largest==nullptr表示大于DB中所有键的键。
// 要求：如果disjoint_sorted_files为true，则files[]包含按排序顺序排列的不相交范围。
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key);

class Version {
 public:
  // 用来保存查找过程中的中间状态
  struct GetStats {
    // 当前查找到的SSTable文件的元数据信息
    FileMetaData* seek_file;
    // 当前查找到的SSTable文件所在的层级
    int seek_file_level;
  };

  // Append to *iters a sequence of iterators that will
  // yield the contents of this Version when merged together.
  // REQUIRES: This version has been saved (see VersionSet::SaveTo)
  // 将一系列迭代器附加到*iters，当这些迭代器合并在一起时，将产生该Version的内容。
  // 该函数最终在DB::NewIterators()接口中被调用，调用层次为：
  // DBImpl::NewIterator()->DBImpl::NewInternalIterator()->Version::AddIterators()。
  // 函数功能是为该Version中的所有sstable文件都创建一个Two Level Iterator，以遍历sstable文件的内容。
  // 对于level=0级别的sstable文件，直接通过TableCache::NewIterator()接口创建，这会直接载入sstable文件到内存cache中。
  // 对于level>0级别的sstable文件，通过函数NewTwoLevelIterator()创建一个TwoLevelIterator，这就使用了lazy
  // open的机制。 要求：该version已经被保存（请参阅VersionSet::SaveTo）
  void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

  // Lookup the value for key.  If found, store it in *val and
  // return OK.  Else return a non-OK status.  Fills *stats.
  // REQUIRES: lock is not held
  // 查找目标的kv对。如果找到，将key对应的value存储在*val中并返回OK。否则返回非OK状态。填充*stats。
  // 在多版本场景下为LevelDB提供点查询能力，在DBImpl::Get()接口中被调用。在搜索文件的
  // 过程中，会更新具有无效读取次数的文件的allowed_seeks_字段，为seek compaction策略提供支持。
  // 要求：未持有锁
  Status Get(const ReadOptions&, const LookupKey& key, std::string* val,
             GetStats* stats);

  // Adds "stats" into the current state.  Returns true if a new
  // compaction may need to be triggered, false otherwise.
  // REQUIRES: lock is held
  // 将“stats”添加到当前状态中。如果需要触发新的Compaction，则返回true，否则返回false。
  // 更新目标SSTable文件的allowed_seeks_字段，当allowed_seeks_字段减少到0时，触发seek compaction策略。
  // 作为seek compaction策略的一部分，会更新allowed_seeks_字段。
  // 要求：持有锁
  bool UpdateStats(const GetStats& stats);

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.  Returns true if a new compaction may need to be triggered.
  // REQUIRES: lock is held
  // 在指定的内部键处记录读取的字节的样本。大约每config::kReadBytesPeriod字节采集一次样本。
  // 如果需要触发新的Compaction，则返回true。是seek compaction策略的一部分。
  // 通俗来说，我们通过一个迭代器进行遍历，当读取的所有key的字节数达到config::kReadBytesPeriod时，
  // 会调用RecordReadSample()函数进行采样，该方法会判断当前key是否在两个或两个以上的SSTable文件中进行了读取，
  // 若是，则会进一步调用UpdateStats()函数，更新第一次读取的SSTable文件的allowed_seeks_字段，说明这是一次
  // 无效的读取。当allowed_seeks_字段减少到0时，触发seek compaction策略。
  // 要求：持有锁
  bool RecordReadSample(Slice key);

  // Reference count management (so Versions do not disappear out from
  // under live iterators)
  // 引用计数管理（因此版本不会在活动迭代器下消失）
  void Ref();
  void Unref();

  // 在指定的level中查找与[begin, end]重叠的文件，将level层中与[begin, end]重叠的文件存储在inputs中。
  // 若begin为nullptr，则表示在所有key之前。若end为nullptr，则表示在所有key之后。
  // TODO:这里会特殊处理Level 0层，一旦碰到符合要求的文件，且该文件本身范围包含了[begin, end]，
  // 那么会将[begin, end]的范围扩大到该文件的范围。我猜这是为了寻找更多的文件，以便进行Compaction。
  void GetOverlappingInputs(
      int level,
      const InternalKey* begin,  // nullptr means before all keys
      const InternalKey* end,    // nullptr means after all keys
      std::vector<FileMetaData*>* inputs);

  // Returns true iff some file in the specified level overlaps
  // some part of [*smallest_user_key,*largest_user_key].
  // smallest_user_key==nullptr represents a key smaller than all the DB's keys.
  // largest_user_key==nullptr represents a key largest than all the DB's keys.
  // 如果指定层级中的某个文件与[*smallest_user_key,*largest_user_key]的某部分重叠，则返回true。
  // smallest_user_key==nullptr表示小于DB中所有键的键。
  // largest_user_key==nullptr表示大于DB中所有键的键。
  bool OverlapInLevel(int level, const Slice* smallest_user_key,
                      const Slice* largest_user_key);

  // Return the level at which we should place a new memtable compaction
  // result that covers the range [smallest_user_key,largest_user_key].
  // 返回应该放置新的memtable压缩结果的层级，该压缩结果覆盖范围[smallest_user_key,largest_user_key]。
  // 通过检查给定的[smallest_user_key,largest_user_key]范围，来决定新的memtable压缩结果应该放置在哪个层级。
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);

  // 返回指定层级的文件数量
  int NumFiles(int level) const { return files_[level].size(); }

  // Return a human readable string that describes this version's contents.
  // 返回一个人类可读的字符串，描述该版本的内容。
  // 返回每一层中的SSTable文件的信息，包括文件号、文件大小、文件范围等。
  // --level n--
  // 文件序列号:文件大小[最小键 .. 最大键]
  std::string DebugString() const;

 private:
  friend class Compaction;
  friend class VersionSet;

  class LevelFileNumIterator;

  explicit Version(VersionSet* vset)
      : vset_(vset),
        next_(this),
        prev_(this),
        refs_(0),
        file_to_compact_(nullptr),
        file_to_compact_level_(-1),
        compaction_score_(-1),
        compaction_level_(-1) {}

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // 将该Version从VersionSet中删除，并释放其占用的资源
  ~Version();

  // 返回一个TwoLevelIterator，第一层迭代器是LevelFileNumIterator，第二层迭代器是TableCache::NewIterator()
  // 第一层迭代器负责在指定level中找到符合要求的文件，第二层迭代器负责在指定文件中找到符合要求的数据。
  Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

  // Call func(arg, level, f) for every file that overlaps user_key in
  // order from newest to oldest.  If an invocation of func returns
  // false, makes no more calls.
  // 遍历所有层级的文件，并在此过程中尝试寻找与user_key重叠的文件。
  // 若找到了重叠的文件，则调用func(arg, level, f)。若在中间某一层级的文件调用func返回false，
  // 则不再继续调用, 直接return，不再继续执行。
  //
  // 要求：internal_key的user_key部分 == user_key。
  void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                          bool (*func)(void*, int, FileMetaData*));

  // 该version所属的VersionSet
  VersionSet* vset_;
  // VersionSet为双向循环链表，next_指向下一个Version
  Version* next_;
  // VersionSet为双向循环链表，prev_指向上一个Version
  Version* prev_;
  // 该version的引用计数
  int refs_;

  // List of files per level
  // 每个层级的文件列表，files_[i]表示第i层的文件列表
  std::vector<FileMetaData*> files_[config::kNumLevels];

  // NOTE: LevelDB有两种触发Compaction操作的策略。
  // 1. size_compaction: 当某一层的文件总大小超过阈值时，触发Compaction操作。
  // 2. seek_compaction:
  // 当某一层的某个文件的无效读取次数超过阈值时，触发Compaction操作。
  // FileMetaData::allowed_seeks_字段记录了允许的最大无效读取次数。

  // Next file to compact based on seek stats.

  // 用于seek_compaction策略。
  // 若有SSTable的无效查找次数超过其规定的allowed_seeks_，则将file_to_compact_指向该SSTable的FileMetaData。
  FileMetaData* file_to_compact_;
  // 将file_to_compact_置为达到无效查找次数的SSTable所在的层级。
  int file_to_compact_level_;

  // 用于size_compaction策略。
  // 分数<1表示不严格需要压缩。而>=1表示需要进行Compaction操作。
  // 这些字段由Finalize()初始化。请看VersionSet::Finalize()源码理解其初始化过程。

  // 下一个应该压缩的层级的压缩分数，用于size_compaction
  // 每当compaction_score_ >= 1时，意味着必须进行Compaction操作。
  // Level0和别的Level计算compaction_score_的方式不同。具体请见VersionSet::Finalize()源码。
  double compaction_score_;
  // 下一个应该压缩的层级
  int compaction_level_;
};

class VersionSet {
 public:
  VersionSet(const std::string& dbname, const Options* options,
             TableCache* table_cache, const InternalKeyComparator*);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // 析构时，当前VersionSet中应该没有任何有效的Version
  ~VersionSet();

  // Apply *edit to the current version to form a new descriptor that
  // is both saved to persistent state and installed as the new
  // current version.  Will release *mu while actually writing to the file.
  // REQUIRES: *mu is held on entry.
  // REQUIRES: no other thread concurrently calls LogAndApply()
  // 应用*edit到当前版本，形成一个新的描述符，该描述符既保存到持久状态，又安装为新的当前版本。
  // 在实际写入文件时，将释放*mu。
  // 每次进行完一次Compaction操作后，会产生一个VersionEdit，然后调用VersionSet::LogAndApply()接口，
  // 将VersionEdit应用到当前的Version中，从而产生一个新的Version。
  // 要求：*mu在进入时被持有。没有其他线程并发调用LogAndApply()。
  Status LogAndApply(VersionEdit* edit, port::Mutex* mu)
      EXCLUSIVE_LOCKS_REQUIRED(mu);

  // Recover the last saved descriptor from persistent storage.
  // 从持久存储中，通过读取MANIFEST的log文件，将每一次的改动回放到
  // 当前的Version中，从而重建出崩溃前的状态。
  Status Recover(bool* save_manifest);

  // Return the current version.
  // 返回当前正在使用的Version
  Version* current() const { return current_; }

  // Return the current manifest file number
  // 返回当前MANIFEST文件正在使用的文件序列号
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  // Allocate and return a new file number
  // 分配并返回一个新的文件号，用于新创建的MANIFEST文件
  uint64_t NewFileNumber() { return next_file_number_++; }

  // Arrange to reuse "file_number" unless a newer file number has
  // already been allocated.
  // 除非已经分配了更新的文件号，否则安排重用“file_number”。这里是重用现在
  // 的MANIFEST文件的文件号，就可以避免一次刷盘操作。
  // REQUIRES: "file_number" was returned by a call to NewFileNumber().
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  // Return the number of Table files at the specified level.
  // 返回当前版本中指定层级的SSTable文件数量
  int NumLevelFiles(int level) const;

  // Return the combined file size of all files at the specified level.
  // 返回当前版本中指定层级的所有SSTable文件的总大小
  int64_t NumLevelBytes(int level) const;

  // Return the last sequence number.
  // 返回当前最新版本的写操作的最新序列号
  uint64_t LastSequence() const { return last_sequence_; }

  // Set the last sequence number to s.
  // 将当前写操作的最新序列号设置为s
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  // Mark the specified file number as used.
  // 将指定的文件号标记为已使用，用于标记MANIFEST文件中正在使用的日志文件的序列号。
  void MarkFileNumberUsed(uint64_t number);

  // Return the current log file number.
  // 返回当前MANIFEST文件正在使用的日志文件的序列号。
  uint64_t LogNumber() const { return log_number_; }

  // Return the log file number for the log file that is currently
  // being compacted, or zero if there is no such log file.
  // 返回当前正在压缩的日志文件的日志文件号，如果没有这样的日志文件，则返回零。
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  // Pick level and inputs for a new compaction.
  // Returns nullptr if there is no compaction to be done.
  // Otherwise returns a pointer to a heap-allocated object that
  // describes the compaction.  Caller should delete the result.
  // 为新的Compaction选择级别和输入。如果没有要执行的Compaction，则返回nullptr。
  // 否则返回指向描述Compaction的堆分配对象的指针。调用者应负责删除结果。
  Compaction* PickCompaction();

  // Return a compaction object for compacting the range [begin,end] in
  // the specified level.  Returns nullptr if there is nothing in that
  // level that overlaps the specified range.  Caller should delete
  // the result.
  // 返回一个Compaction对象，用于在指定级别中压缩范围[begin,end]。
  // 如果该级别中没有与指定范围重叠的内容，则返回nullptr。调用者应负责删除结果。
  Compaction* CompactRange(int level, const InternalKey* begin,
                           const InternalKey* end);

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  // 返回下一个层级中任何文件的最大重叠数据（以字节为单位），该文件位于级别>=1。
  int64_t MaxNextLevelOverlappingBytes();

  // Create an iterator that reads over the compaction inputs for "*c".
  // 返回一个迭代器，该迭代器用于遍历输入的compaction对象*c。
  Iterator* MakeInputIterator(Compaction* c);

  // Returns true iff some level needs a compaction.
  // 如果某个层级需要Compaction，则返回true。无论是size_compaction还是seek_compaction。
  bool NeedsCompaction() const {
    Version* v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
  }

  // Add all files listed in any live version to *live.
  // May also mutate some internal state.
  // 将列在任何活动版本中的所有文件添加到*live。将VersionSet中当前活跃的所有版本中的所有
  // 正在使用的文件的文件序列号保存到live结构体中。
  void AddLiveFiles(std::set<uint64_t>* live);

  // Return the approximate offset in the database of the data for
  // "key" as of version "v".
  // 返回数据库中“key”的数据的近似偏移量，作为版本“v”的一部分。
  uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

  // Return a human-readable short (single-line) summary of the number
  // of files per level.  Uses *scratch as backing store.
  struct LevelSummaryStorage {
    char buffer[100];
  };
  // 返回每个层级的文件数量的人类可读的简短（单行）摘要。使用*scratch作为后备存储。
  const char* LevelSummary(LevelSummaryStorage* scratch) const;

 private:
  class Builder;

  friend class Compaction;
  friend class Version;

  // 是否重用现有的MANIFEST文件，重用现有的Log文件或者MANIFEST文件，可以避免额外的创建和刷盘操作。
  // 这在启动DB的时候，由options->reuse_logs来控制。
  /**
   * @param dscname 使用dscbase构造的MANIFEST文件的名字
   * @param dscbase 代表指向的MANIFEST文件的序列号/指针
   */
  bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

  // 每当LevelDB调用VersionSet::LogAndApply()更新Version后，都会让新Version
  // 调用VersionSet::Finalize()方法，来计算每层SSTable是否需要Size Compaction，
  // 并选出最需要进行Size Compaction的层级作为下次Compaction的目标层级。
  void Finalize(Version* v);

  // Stores the minimal range that covers all entries in inputs in
  // *smallest, *largest.
  // REQUIRES: inputs is not empty
  // 获取inputs中所有文件的最小key和最大key，存储到smallest和largest中
  void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                InternalKey* largest);

  // Stores the minimal range that covers all entries in inputs1 and inputs2
  // in *smallest, *largest.
  // REQUIRES: inputs is not empty
  // 获取inputs1和inputs2中所有文件的最小key和最大key，存储到smallest和largest中
  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2,
                 InternalKey* smallest, InternalKey* largest);

  void SetupOtherInputs(Compaction* c);

  // Save current contents to *log
  // 将CURRENT文件中的初始内容序列化为VersionEdit，然后以Log Record
  // 的形式写入到log文件中。
  Status WriteSnapshot(log::Writer* log);

  // 将版本v作为新的current_版本，并将其追加到VersionSet的版本链表中
  void AppendVersion(Version* v);

  // env_提供了文件系统的接口
  Env* const env_;
  // dbname_是数据库的名字
  const std::string dbname_;
  // options_是数据库的配置选项
  const Options* const options_;
  // 代表该DB实例中缓存SSTable的TableCache
  TableCache* const table_cache_;
  // icmp_是用于比较InternalKey的比较器
  const InternalKeyComparator icmp_;
  // next_file_number_是要分配的下一个文件号
  uint64_t next_file_number_;
  // 当前的VersionSet正在使用的MANIFEST文件的序列号
  uint64_t manifest_file_number_;
  // 当前操作的最新序列号，是写类型的序列号，如Put/Delete
  uint64_t last_sequence_;
  // log_number_是当前日志文件的序列号
  uint64_t log_number_;
  // 0或者是正在压缩的memtable的后备存储
  // 0 or backing store for memtable being compacted
  uint64_t prev_log_number_;

  // Opened lazily
  // descriptor_file_用于记录每次版本变更时的VersionEdit信息，
  // 是元数据信息的WAL日志文件，文件类型为kDescriptorFile。
  // 同时是下面的descriptor_log_写入的实际文件。
  WritableFile* descriptor_file_;
  // 每一次Compaction操作都会产生一个新的VersionEdit，每次版本变更的时候
  // 都会将VersionEdit序列化为Log Record，写入到当前Log文件中。
  // 这种Log文件与WAL Log使用的文件格式是一样的，只是Content内容字段不同。
  // 在LevelDB中，这种Log文件被称为kDescriptorFile文件。
  log::Writer* descriptor_log_;

  // Head of circular doubly-linked list of versions.
  // VersionSet中以双向循环链表的形式维护了一组Version
  // dummy_versions_是一个哨兵节点，初始化时next_和prev_都指向自己，本身不存储数据
  Version dummy_versions_;
  // current_指向当前的Version，也就是最新的Version
  // == dummy_versions_.prev_
  Version* current_;

  // Per-level key at which the next compaction at that level should start.
  // Either an empty string, or a valid InternalKey.
  // 每个层级的键，下一个在该层级进行Compaction的键应该从哪里开始。
  // 可以是一个空字符串，也可以是一个有效的InternalKey，该InternalKey是被序列化后的。
  std::string compact_pointer_[config::kNumLevels];
};

// A Compaction encapsulates information about a compaction.
// Compaction对象封装了有关Compaction的信息。
class Compaction {
 public:
  ~Compaction();

  // Return the level that is being compacted.  Inputs from "level"
  // and "level+1" will be merged to produce a set of "level+1" files.
  int level() const { return level_; }

  // Return the object that holds the edits to the descriptor done
  // by this compaction.
  VersionEdit* edit() { return &edit_; }

  // "which" must be either 0 or 1
  int num_input_files(int which) const { return inputs_[which].size(); }

  // Return the ith input file at "level()+which" ("which" must be 0 or 1).
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  // Maximum size of files to build during this compaction.
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // Is this a trivial compaction that can be implemented by just
  // moving a single input file to the next level (no merging or splitting)
  // 这是一个简单的Compaction吗？可以通过将单个输入文件移动到下一个级别来实现（无需合并或拆分）
  bool IsTrivialMove() const;

  // Add all inputs to this compaction as delete operations to *edit.
  void AddInputDeletions(VersionEdit* edit);

  // Returns true if the information we have available guarantees that
  // the compaction is producing data in "level+1" for which no data exists
  // in levels greater than "level+1".
  bool IsBaseLevelForKey(const Slice& user_key);

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  bool ShouldStopBefore(const Slice& internal_key);

  // Release the input version for the compaction, once the compaction
  // is successful.
  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options* options, int level);

  int level_;
  uint64_t max_output_file_size_;
  Version* input_version_;
  VersionEdit edit_;

  // Each compaction reads inputs from "level_" and "level_+1"
  std::vector<FileMetaData*> inputs_[2];  // The two sets of inputs

  // State used to check for number of overlapping grandparent files
  // (parent == level_ + 1, grandparent == level_ + 2)
  std::vector<FileMetaData*> grandparents_;
  size_t grandparent_index_;  // Index in grandparent_starts_
  bool seen_key_;             // Some output key has been seen
  int64_t overlapped_bytes_;  // Bytes of overlap between current output
                              // and grandparent files

  // State for implementing IsBaseLevelForKey

  // level_ptrs_ holds indices into input_version_->levels_: our state
  // is that we are positioned at one of the file ranges for each
  // higher level than the ones involved in this compaction (i.e. for
  // all L >= level_ + 2).
  size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H_
