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
  // NOTE:这里会特殊处理Level 0层，一旦碰到符合要求的文件，且该文件本身范围包含了[begin, end]，
  // 那么会将[begin, end]的范围扩大到该文件的范围。我猜这是为了寻找更多的文件，以便进行Compaction。
  // 在Level0的场景下，最终[begin,end]的范围是所有与[begin,end]重叠的文件的范围的并集。
  // 
  // 在Level 0中为什么需要尽可能的合理扩大[begin, end]的范围呢？假设有这样的一种情况：
  // Level 0中有f1,f2,f3,f4这四个文件，我们首先写了d键，在f1中的序列号为10，然后删除了d键，删除
  // 操作在f2中的序列号为100，假设compaction操作时只选取了f1，则下次查找d键时会先从level0的f1中找到
  // 这个结果，但是实际上这个结果是无效的，因为f2中已经删除了d键。所以在level0中需要尽可能的合理扩大
  // [begin, end]的范围，从而使得compaction操作能够尽可能的多的找到无效的key，从而触发compaction操作。
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
  // size_compaction策略下计算出的最需要压缩的层级
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
  // 分配并返回一个新的文件号。用于分配新的SSTable文件的文件号。
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
  // 为新的Compaction选择具体的level，以及level层和level+1层中要参与compaction的文件等等。
  // 将上述所有信息以Compaction对象的形式返回。如果没有Compaction需要执行，则返回nullptr。
  // 否则返回一个指向堆分配的对象的指针，该对象描述了Compaction。调用者应负责删除结果。
  Compaction* PickCompaction();

  // Return a compaction object for compacting the range [begin,end] in
  // the specified level.  Returns nullptr if there is nothing in that
  // level that overlaps the specified range.  Caller should delete
  // the result.
  // 返回一个Compaction对象，用于在指定级别中压缩范围[begin,end]。
  // 如果该级别中没有与指定范围重叠的内容，则返回nullptr。调用者应负责删除结果。
  // 该接口用于Manual Compaction，即手动触发Compaction操作。
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

  /**
   * 辅助函数。每当开启一次Compaction操作时，首先通过PickCompaction()函数从current_version中
   * 判断是否有需要Compaction的层级，若有，获取到需要compaction的level，从Compact_pointer_中
   * 获取第一个要压缩的文件，之后根据该文件负责的key范围，在level中找到与该文件重叠的文件，这些文件
   * 就是在level层需要参与Compaction的文件。这些文件会被存储在c->inputs_[0]中。
   * SetupOtherInputs()函数就是用来设置c->inputs_[1]，即level+1层需要参与Compaction的文件。
   * 根据已知的c->inputs_[0]，找到level+1层中与c->inputs_[0]重叠的文件，这些文件就是level+1层需要
   * 参与Compaction的文件。
   * NOTE:之后还有一次根据上面已经确定的c->inputs_[0]和c->inputs_[1]，尝试在不扩展c->inputs_[1]的文件
   * 集合的前提下，尽可能的扩展c->inputs_[0]的文件集合，这样可以减少Compaction的次数。
   */
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
  // descriptor_log_是descriptor_file_的封装，用于将VersionEdit序列化为Log Record。
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
  // 每个层级的键，下一个在该层级进行Compaction的键应该从哪里开始，必须大于compact_pointer_[level]记录的键。
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
  // 返回正在压缩的层级。来自“level”和“level+1”的输入将被合并以生成一组“level+1”文件。
  int level() const { return level_; }

  // Return the object that holds the edits to the descriptor done
  // by this compaction.
  // 返回由此次compaction操作完成后而产生的增量信息对象，即VersionEdit对象。
  VersionEdit* edit() { return &edit_; }

  // "which" must be either 0 or 1
  // 返回第"which"个输入文件的数量，因为compaction操作会涉及level_和level_+1两个层级，
  // 所以which只能是0或1。inputs_[0]表示level_层参与的文件，inputs_[1]表示level_+1层参与的文件。
  int num_input_files(int which) const { return inputs_[which].size(); }

  // Return the ith input file at "level()+which" ("which" must be 0 or 1).
  // 返回“level()+which”中的第i个输入文件（“which”必须是0或1）。0代表level_层的输入文件集合，
  // 1代表level_+1层的输入文件集合。
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  // Maximum size of files to build during this compaction.
  // 本次compaction操作的最大输出文件大小
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // Is this a trivial compaction that can be implemented by just
  // moving a single input file to the next level (no merging or splitting)
  // 这是一个简单的Compaction吗？可以通过将单个输入文件移动到下一个级别来实现（无需合并或拆分）
  bool IsTrivialMove() const;

  // Add all inputs to this compaction as delete operations to *edit.
  // 参与本次compaction操作的所有输入文件，都会compaction操作完成后，作为
  // 将要被删除的文件，添加到VersionEdit对象中。
  void AddInputDeletions(VersionEdit* edit);

  // Returns true if the information we have available guarantees that
  // the compaction is producing data in "level+1" for which no data exists
  // in levels greater than "level+1".
  // 如果我们当前可用的信息保证compaction操作正在生成“level+1”中的数据，而在大于“level+1”的级别中不存在
  // 任何与user_key重叠的数据，则返回true。
  // 本质上是为了判断level+1是否是该user_key的最高层级。
  bool IsBaseLevelForKey(const Slice& user_key);

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  // 如果我们应该在处理“internal_key”之前停止构建当前输出，则返回true。
  // 即internal_key与grandparents_中的文件重叠的字节数超过了阈值，需要
  // 构建新的输出文件。
  bool ShouldStopBefore(const Slice& internal_key);

  // Release the input version for the compaction, once the compaction
  // is successful.
  // 一旦compaction操作成功，就释放对compaction的输入版本的引用。
  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options* options, int level);

  // 本次compaction的层级，即level_和level_+1
  int level_;
  // 本次compaction的最大输出文件大小
  uint64_t max_output_file_size_;
  // 本次compaction操作的基础版本
  Version* input_version_;
  // 本次compaction操作完成后相对于基础版本产生的增量信息，
  // 基础版本应用该增量信息后，就得到了新的版本。
  VersionEdit edit_;

  // Each compaction reads inputs from "level_" and "level_+1"
  // 每个compaction操作都会从“level_”和“level_+1”读取输入，因此inputs_
  // 是一个二维数组，inputs_[0]表示level_层参与的文件，inputs_[1]表示level_+1层参与的文件
  std::vector<FileMetaData*> inputs_[2];

  // State used to check for number of overlapping grandparent files
  // (parent == level_ + 1, grandparent == level_ + 2)
  // 该变量用于检查与某次compaction操作重叠的祖父文件数量(parent == level_ + 1，grandparent == level_ + 2)
  // 在每次开启compaction之前，在SetupOtherInputs()函数中会初始化该变量。用于ShouldStopBefore()函数。
  std::vector<FileMetaData*> grandparents_; 
  // grandparent_index_是grandparents_中的索引，用于指向当前处理的grandparents_中的文件。
  // 用于ShouldStopBefore()函数。
  size_t grandparent_index_;
  // 表示是否已经处理过某些输出键，若为true，则代表已经有一些键被处理过。
  // 用于跟踪在压缩过程中是否已经遇到任何输出键。用于ShouldStopBefore()函数，
  // 记录已经处理过的输出键，以便在压缩过程中进行合理的处理。
  bool seen_key_;
  // 记录当前输出文件与祖父层文件之间重叠的字节数。用于计算重叠区域的数据量，以便在压缩过程中进行合理的处理。 
  int64_t overlapped_bytes_;

  // State for implementing IsBaseLevelForKey
  // 用于实现IsBaseLevelForKey的状态

  // level_ptrs_ holds indices into input_version_->levels_: our state
  // is that we are positioned at one of the file ranges for each
  // higher level than the ones involved in this compaction (i.e. for
  // all L >= level_ + 2).
  // level_ptrs_ 数组的每个元素保存 input_version_->levels_ 中的索引。
  // 这些索引指向在压缩过程中每一层（从 level_ + 2 开始及更高层）的文件范围。
  // 
  // 在进行压缩操作时，LevelDB 会涉及多个层次的文件。level_ptrs_ 数组用于跟踪每一层（从 level_ + 2 开始及更高层）当前正在处理的文件位置。
  // 通过这个数组，IsBaseLevelForKey 方法能够检查给定的键是否在更高层的文件范围内，从而决定是否需要在这些层次中进一步查找。
  size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H_
