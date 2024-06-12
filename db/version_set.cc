// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_set.h"

#include <algorithm>
#include <cstdio>

#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "leveldb/env.h"
#include "leveldb/table_builder.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// 获取当前数据库实例允许写入的最大的单个文件大小
static size_t TargetFileSize(const Options* options) {
  return options->max_file_size;
}

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
// 在我们停止在level->level+1压缩中构建单个文件之前，祖父级别(即level+2)允许的最大重叠字节数。
// 取值为10倍数据库实例允许写入的最大的单个文件大小
static int64_t MaxGrandParentOverlapBytes(const Options* options) {
  return 10 * TargetFileSize(options);
}

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
// 所有压缩文件中的最大字节数。如果扩展压缩的较低级别文件集会使总压缩覆盖的字节数超过这么多，我们将避免这样做。
// 取值为25倍数据库实例允许写入的最大的单个文件大小
static int64_t ExpandedCompactionByteSizeLimit(const Options* options) {
  return 25 * TargetFileSize(options);
}

// 每个Level允许的所有文件的总字节大小的和，不适用于Level 0，Level 0采用了固定文件数的策略
static double MaxBytesForLevel(const Options* options, int level) {
  // Note: the result for level zero is not really used since we set
  // the level-0 compaction threshold based on number of files.
  // 注意：level 0的结果实际上并没有真正使用，因为我们根据文件数量设置了level-0压缩阈值。

  // Result for both level-0 and level-1
  // Level 0和Level 1的结果。1048576 = 2^20B=1MB
  // Level 1的最大字节大小为10MB，再往后每个Level的最大字节大小是前一个Level的10倍
  double result = 10. * 1048576.0;
  while (level > 1) {
    result *= 10;
    level--;
  }
  return result;
}

// 获取每一层的单个文件允许的最大字节大小，默认所有层的单个文件允许的最大字节大小都相同，
// 取自options->max_file_size_，默认为2MB。
static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
  // We could vary per level to reduce number of files?
  return TargetFileSize(options);
}

// 获取files中所有文件的总字节大小
static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}

Version::~Version() {
  // 在释放该Version对象之前，需要确保该Version对象不再被引用，即refs_为0
  assert(refs_ == 0);

  // Remove from linked list
  // 将该Version对象从VersionSet的双向链表中移除
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  // 释放该Version对象中对所有层中的所有文件的引用，即将所有文件的引用计数减1
  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData* f = files_[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) { // 如果文件的引用计数为0，则释放该文件
        delete f;
      }
    }
  }
}

int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files, const Slice& key) {
  // 二分查找
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData* f = files[mid];
    // key大于当前mid文件的最大key，则说明key在mid文件之后的文件中
    if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      // key大于mid文件的最大key，因此mid文件及之前的文件都不感兴趣
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      // key小于mid文件的最大key，因此mid文件及之后的文件都不感兴趣
      right = mid;
    }
  }
  return right;
}

// 如果user_key大于f的最大key，则返回true，这说明user_key在f之后的文件中
static bool AfterFile(const Comparator* ucmp, const Slice* user_key,
                      const FileMetaData* f) {
  // null user_key occurs before all keys and is therefore never after *f
  // 若user_key为nullptr,则说明user_key在所有key之前，因此永远不会在*f之后
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

// 如果user_key小于f的最小key，则返回true，这说明user_key在f之前的文件中
static bool BeforeFile(const Comparator* ucmp, const Slice* user_key,
                       const FileMetaData* f) {
  // null user_key occurs after all keys and is therefore never before *f
  // 若user_key为nullptr,则说明user_key在所有key之后，因此永远不会在*f之前
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  // 如果files中存储的文件不是key有序且不相交的，则需要依次检查所有文件
  if (!disjoint_sorted_files) {
    // Need to check against all files
    // 需要检查所有文件
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      // 若smallest_user_key大于f的最大key，或largest_user_key小于f的最小key，
      // 则说明user_key在f之前或之后，不会与f相交
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // 如果files中存储的文件是key有序且不相交的，则可以使用二分查找
  // Binary search over file list
  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    // Find the earliest possible internal key for smallest_user_key
    // 找到smallest_user_key的最早可能的内部键
    InternalKey small_key(*smallest_user_key, kMaxSequenceNumber,
                          kValueTypeForSeek);
    // 找到smallest_user_key的最早可能的内部键在files中的位置
    index = FindFile(icmp, files, small_key.Encode());
  }

  // 若没找到，则说明smallest_user_key大于所有文件的最大key，不会与任何文件相交
  if (index >= files.size()) {
    // beginning of range is after all files, so no overlap.
    // 范围的开始在所有文件之后，因此没有重叠。
    return false;
  }

  // 已知files[index]的最大key大于smallest_user_key，只要largest_user_key不小于files[index]的最小key，
  // 则说明[smallest_user_key, largest_user_key]与files[index]相交。
  // [f->min, f-max]。已知f-max >= smallest_user_key，只要largest_user_key >= f->min，就说明两者有交集。
  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
// 一个内部迭代器。对于给定的version/level对，提供关于该层中文件的信息。
// 在LevelFileNumIterator中，对于给定的entry，key()是文件中出现的最大key，
// value()是一个16字节的值，包含文件编号和文件大小，都使用EncodeFixed64编码。
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>* flist)
      : icmp_(icmp), flist_(flist), index_(flist->size()) {  // Marks as invalid
  }
  bool Valid() const override { return index_ < flist_->size(); }
  void Seek(const Slice& target) override {
    index_ = FindFile(icmp_, *flist_, target);
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  void Next() override {
    assert(Valid());
    index_++;
  }
  void Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();  // Marks as invalid
    } else {
      index_--;
    }
  }
  Slice key() const override {
    assert(Valid());
    // key()代表文件中出现的最大key
    return (*flist_)[index_]->largest.Encode();
  }
  Slice value() const override {
    assert(Valid());
    // value()是一个16字节的值，包含文件编号和文件大小，都使用EncodeFixed64编码
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_ + 8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  Status status() const override { return Status::OK(); }

 private:
  // 用于比较两个InternalKey对象
  const InternalKeyComparator icmp_;
  // 该层中的所有文件
  const std::vector<FileMetaData*>* const flist_;
  // 该Iterator当前指向的文件在flist_中的索引
  uint32_t index_;

  // Backing store for value().  Holds the file number and size.
  // value()的后备存储。保存文件编号和大小。用于保存当前index_指向的文件的编号和大小
  mutable char value_buf_[16];
};

// 用于获取指定File的Iterator，file_value是一个16字节的值，包含文件编号和文件大小，都使用EncodeFixed64编码。
// file_value是使用指定层的LevelFileNumIterator从指定文件中获取的。
static Iterator* GetFileIterator(void* arg, const ReadOptions& options,
                                 const Slice& file_value) {
  TableCache* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16) {
    return NewErrorIterator(
        Status::Corruption("FileReader invoked with unexpected value"));
  } else {
    // 根据file_value中的文件编号和文件大小，从TableCache中获取该文件的Iterator
    return cache->NewIterator(options, DecodeFixed64(file_value.data()),
                              DecodeFixed64(file_value.data() + 8));
  }
}

// 构建TwoLevelIterator，最终目的是获取到指定level的指定文件的Iterator。
// 第一层Iterator是LevelFileNumIterator，用于获取指定level的所有文件。根据key值，找到指定文件的编号和大小。
// 第二层Iterator是GetFileIterator，用于从TableCache中获取指定文件的Iterator。
Iterator* Version::NewConcatenatingIterator(const ReadOptions& options,
                                            int level) const {
  return NewTwoLevelIterator(
      new LevelFileNumIterator(vset_->icmp_, &files_[level]), &GetFileIterator,
      vset_->table_cache_, options);
}

void Version::AddIterators(const ReadOptions& options,
                           std::vector<Iterator*>* iters) {
  // Merge all level zero files together since they may overlap
  // 合并所有level 0的文件，因为它们可能重叠
  // 为Level 0中的所有文件创建Iterator，这里直接使用TableCache中的NewIterator方法创建Iterator
  // 创建完后会直接将Level0层的文件加载到内存中
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters->push_back(vset_->table_cache_->NewIterator(
        options, files_[0][i]->number, files_[0][i]->file_size));
  }

  // For levels > 0, we can use a concatenating iterator that sequentially
  // walks through the non-overlapping files in the level, opening them
  // lazily.
  // 对于大于0的层级，使用TwoLevelIterator，它会顺序遍历该层中不重叠的文件，懒加载地打开它们。
  // 即我们预先为大于Level0的每一层创建一个TwoLevelIterator，当需要遍历该层时，才会打开该层的文件。
  // 要知道，大于Level0的每一层中的内部文件之间的key是有序且不相交的。
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

// Callback from TableCache::Get()
namespace {
enum SaverState {
  kNotFound,
  kFound,
  kDeleted,
  kCorrupt,
};
// Saver用于保存Get(点查询操作)的返回值
struct Saver {
  // 表示TableCache::Get()操作的状态
  SaverState state;
  // 用于比较两个InternalKey对象
  const Comparator* ucmp;
  // Interkey中的user_key部分
  Slice user_key;
  // 如果state为kFound，value用于保存user_key对应的value
  std::string* value;
};
}  // namespace
/**
 * 在 LevelDB 中，SaveValue 函数是一个静态函数，通常作为回调函数使用，用于在查找过程中处理找到的键值对。
 * 这个函数的主要任务是解析内部键（ikey）并根据用户键和类型信息更新 Saver 结构体的状态和值。
 * @param arg 通用指针，指向 Saver 结构体
 * @param ikey 一个InternalKey对象，包含了用户键和类型信息
 * @param v 一个Slice对象，包含了键对应的值
 */
static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) { // 尝试解析InternalKey对象
    s->state = kCorrupt;
  } else { // 解析成功
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) { // 二次确认user_key是否相等
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      if (s->state == kFound) { // 只有在InternalKey的类型为kTypeValue时，才意味着value有效
        s->value->assign(v.data(), v.size());
      }
    }
  }
}

// 判断文件a的序列号是否大于文件b的序列号
static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
  return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  // Search level-0 in order from newest to oldest.
  // 从最新到最旧的顺序搜索level-0中的文件。因为Level0中的SSTable文件可能存在Key重叠的情况，
  // 所以需要逐个检查Level0中的文件，以确保找到所有与user_key重叠的文件。
  // 将可能与user_key重叠的文件存储在tmp中
  std::vector<FileMetaData*> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    FileMetaData* f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(f);
    }
  }
  // 对tmp中的文件按照文件序列号从大到小排序，即按照最新到最旧的顺序
  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    // 依次对排序后的文件调用func函数
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!(*func)(arg, 0, tmp[i])) {
        return;
      }
    }
  }

  // Search other levels.
  // 从Level1开始，依次检查每一层的文件，如果发现与user_key重叠的文件，调用func函数处理
  for (int level = 1; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Binary search to find earliest index whose largest key >= internal_key.
    // 使用二分查找找到最小的SSTable文件的索引，该SSTable文件的最大key大于等于internal_key
    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      FileMetaData* f = files_[level][index];
      // 若user_key比目标文件的最小key还小，则说明user_key在目标文件之前，不会与目标文件重叠
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
        // All of "f" is past any data for user_key
      } else { // 说明user_key属于目标文件的key范围内
        if (!(*func)(arg, level, f)) { // 若调用func函数返回false，则终止该流程
          return;
        }
      }
    }
  }
}

Status Version::Get(const ReadOptions& options, const LookupKey& k,
                    std::string* value, GetStats* stats) {
  stats->seek_file = nullptr;
  stats->seek_file_level = -1;

  // 记录一次Get操作产生的结果信息
  struct State {
    // 用于保存Get操作的返回值
    Saver saver;
    // 如果本次Get不止seek了一个文件（仅会发生在level 0的情况），就将搜索的第一个文件保存在stats中。如果stat有数据返回，
    // 表明本次读取在搜索到包含key的sstable文件之前，还做了其它无谓的搜索。这个结果将用在UpdateStats()中。
    GetStats* stats;
    // 执行Get操作时的选项
    const ReadOptions* options;
    // 目标InternalKey的序列化形式，与参数中的LookupKey对象k对应
    Slice ikey;
    // 因为Level 0的SSTable文件可能存在Key重叠的情况，所以可能需要检索多个文件，
    // 在搜索了不止一个文件后，保存搜索到的上一个文件。
    FileMetaData* last_file_read;
    // last_file_read所在的层级
    int last_file_read_level;

    // 当前DB的VersionSet对象，以双向链表的形式管理所有的Version对象
    VersionSet* vset;
    Status s;
    // 本次Get操作是否找到了user_key对应的value
    bool found;

    // 用于处理每个可能符合条件的文件，并返回一个布尔值，表示是否继续搜索其他文件
    /**
     * @param arg 通用指针，指向State结构体
     * @param level 当前文件所在的层级
     * @param f 当前文件
     */
    static bool Match(void* arg, int level, FileMetaData* f) {
      State* state = reinterpret_cast<State*>(arg);

      // 在本次查找开始之前，如果state->stats->seek_file为空，且state->last_file_read不为空，
      // 说明这次读取已经进行了多次查找，将最后一次查找的文件设置为当前查找的文件。
      if (state->stats->seek_file == nullptr &&
          state->last_file_read != nullptr) {
        // We have had more than one seek for this read.  Charge the 1st file.
        // 对于这次读取，我们进行了多次查找。将最后一次查找的文件设置为当前查找的文件。
        state->stats->seek_file = state->last_file_read;
        state->stats->seek_file_level = state->last_file_read_level;
      }

      // 更新state->last_file_read和state->last_file_read_level为当前在处理的文件
      state->last_file_read = f;
      state->last_file_read_level = level;

      // 从TableCache中获取目标文件的Iterator，查找是否存在ikey对应的value，如果存在则保存到state->saver.value中
      state->s = state->vset->table_cache_->Get(*state->options, f->number,
                                                f->file_size, state->ikey,
                                                &state->saver, SaveValue);
      if (!state->s.ok()) {
        state->found = true;
        return false;
      }
      switch (state->saver.state) {
        case kNotFound: // 未找到，继续搜索其他文件
          return true;  // Keep searching in other files
        case kFound:  // 找到，返回true，表示终止搜索
          state->found = true;
          return false;
        case kDeleted: // internal_key对应的value已被删除，停止搜索
          return false;
        case kCorrupt: // 该kv对已损坏，停止搜索
          state->s =
              Status::Corruption("corrupted key for ", state->saver.user_key);
          state->found = true;
          return false;
      }

      // Not reached. Added to avoid false compilation warnings of
      // "control reaches end of non-void function".
      // 未到达。添加以避免“控制到达非void函数末尾”的错误编译警告。
      return false;
    }
  };

  // 初始化state结构体
  State state;
  state.found = false;
  state.stats = stats;
  state.last_file_read = nullptr;
  state.last_file_read_level = -1;

  state.options = &options;
  state.ikey = k.internal_key();
  state.vset = vset_;

  state.saver.state = kNotFound;
  state.saver.ucmp = vset_->icmp_.user_comparator();
  state.saver.user_key = k.user_key();
  state.saver.value = value;

  // 在当前Version中，从Level 0开始查找与user_key重叠的文件，若遇到可能会重叠的文件，调用State::Match函数处理，
  // 若Match函数返回false，则说明找到了user_key对应的value或者user_key对应的value已被删除、或者损坏，停止搜索。
  // 若Match函数返回true，则继续搜索其他文件。
  ForEachOverlapping(state.saver.user_key, state.ikey, &state, &State::Match);

  // 如果state.saver.state为kFound，则说明找到了user_key对应的value，返回OK。
  return state.found ? state.s : Status::NotFound(Slice());
}

bool Version::UpdateStats(const GetStats& stats) {
  // 1.在Get操作中，若seek多次，即意味着在搜索到包含key的sstable文件之前，还搜索了其他无效的SSTable文件。
  //   在Get操作的场景下，stats中存储相对于当前来说最后一次搜索的文件，这种情况只会发生在Level 0中。
  // 2.在RecordReadSample操作中，stats存储的是第一个可能包含internal_key的文件，即第一个匹配的文件。
  FileMetaData* f = stats.seek_file;
  if (f != nullptr) {
    f->allowed_seeks--;
    // 若该文件的无效搜索次数已经达到了0，且当前没有在进行的seek compaction，则将该文件加入到seek compaction队列中。
    if (f->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
      file_to_compact_ = f;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

bool Version::RecordReadSample(Slice internal_key) {
  // 将目标InternalKey解码，依次获取user_key、sequence、value_type
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }

  struct State {
    // 存储第一个匹配的文件所处的层级和FileMetaData对象
    GetStats stats;  // Holds first matching file
    // 为了目标InternalKey而查找过的SSTable文件的数量
    int matches;

    /**
     * 用于处理每个可能符合条件的文件，并返回一个布尔值，表示是否继续搜索其他文件
     * @param arg 通用指针，指向State结构体
     * @param level 当前文件所在的层级
     * @param f 当前文件的元数据对象
     */
    static bool Match(void* arg, int level, FileMetaData* f) {
      State* state = reinterpret_cast<State*>(arg);
      state->matches++;
      if (state->matches == 1) { // 若是第一个匹配的文件，更新stats
        // Remember first match.
        state->stats.seek_file = f;
        state->stats.seek_file_level = level;
      }
      // We can stop iterating once we have a second match.
      // 一旦找到了第二个匹配的文件，就可以停止迭代。
      return state->matches < 2;
    }
  };

  State state;
  state.matches = 0;
  // 从Level 0开始查找与user_key重叠的文件，若遇到可能会重叠的文件，调用State::Match函数处理，
  // 一旦找到了第二个匹配的文件，就可以停止迭代。
  ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

  // Must have at least two matches since we want to merge across
  // files. But what if we have a single file that contains many
  // overwrites and deletions?  Should we have another mechanism for
  // finding such files?
  // 必须至少有两个匹配项，因为我们要跨文件合并。但是如果我们有一个包含许多覆盖和删除的单个文件呢？
  // 我们是否应该有另一种机制来查找这样的文件？
  if (state.matches >= 2) {
    // 1MB cost is about 1 seek (see comment in Builder::Apply).
    // 1MB成本约为1次查找（请参阅Builder::Apply中的注释）。
    return UpdateStats(state.stats);
  }
  return false;
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
  // 实际上是将对应level的file列表中的文件与smallest_user_key和largest_user_key进行比较，
  // 判断是否有文件与[smallest_user_key, largest_user_key]有重叠。
  // Level0中的SSTable文件可能存在Key重叠的情况，所以需要逐个检查Level0中的文件，以确保找到所有与user_key重叠的文件。
  // 其余层级的SSTable文件不存在Key重叠的情况，所以可以使用二分查找的方式来查找与user_key重叠的文件。
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                        const Slice& largest_user_key) {
  int level = 0;
  // 若Level 0 中有重叠的key，则Level 0层就是memtable的输出层
  // 若在Level 0中没有重叠的key
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    // 如果在下一层没有重叠，并且在下下一层中重叠的字节数有限，则推送到下一层。
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<FileMetaData*> overlaps;
    while (level < config::kMaxMemCompactLevel) {
      // 若在level + 1中有重叠的key，则Level就是memtable的输出层
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      // level + 1层中没有重叠的key，检查level + 2层中的重叠字节数是否有限
      if (level + 2 < config::kNumLevels) {
        // Check that file does not overlap too many grandparent bytes.
        // 检查文件是否不重叠太多的祖父字节。
        GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
        const int64_t sum = TotalFileSize(overlaps);
        // 若重叠的字节数超过了阈值，则退出循环，此时返回值为level
        if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
          break;
        }
      }
      // 若level + 1层中没有重叠的key，且level + 2层中的重叠字节数有限，则继续向下一层查找
      level++;
    }
  }
  // 返回memtable的输出层
  return level;
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin,
                                   const InternalKey* end,
                                   std::vector<FileMetaData*>* inputs) {
  // 确认层级在有效范围内
  assert(level >= 0);
  assert(level < config::kNumLevels);

  inputs->clear();
  
  // 初始化用户键范围
  Slice user_begin, user_end;
  if (begin != nullptr) {
    user_begin = begin->user_key();  // 提取起始内部键的用户键
  }
  if (end != nullptr) {
    user_end = end->user_key();  // 提取结束内部键的用户键
  }

  // 获取用户键比较器
  const Comparator* user_cmp = vset_->icmp_.user_comparator();

  // 遍历指定层级的所有文件元数据
  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData* f = files_[level][i++];  // 获取当前文件元数据并递增索引
    const Slice file_start = f->smallest.user_key();  // 当前文件的最小用户键
    const Slice file_limit = f->largest.user_key();  // 当前文件的最大用户键

    // 检查当前文件是否完全在指定范围之前，如果是则跳过
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
      // 当前文件的最大键小于指定范围的最小键
    } 
    // 检查当前文件是否完全在指定范围之后，如果是则跳过
    else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
      // 当前文件的最小键大于指定范围的最大键
    } 
    else {  // 当前文件与指定范围有重叠
      inputs->push_back(f);  // 将当前文件加入到重叠文件列表中

      // 特别处理第0层，因为第0层文件之间可能重叠
      if (level == 0) {
        // 检查新添加的文件是否扩展了范围，如果是更新user_begin和user_end
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;  // 更新起始键范围
          inputs->clear();  // 清空result，重新开始搜索
          i = 0;  // 重置索引，重新开始搜索
        } else if (end != nullptr && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;  // 更新结束键范围
          inputs->clear();  // 清空result，重新开始搜索
          i = 0;  // 重置索引，重新开始搜索
        }
      }
    }
  }
}


std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    // 
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" ---\n");
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      AppendNumberTo(&r, files[i]->number);
      r.push_back(':');
      AppendNumberTo(&r, files[i]->file_size);
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append("]\n");
    }
  }
  return r;
}

// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
// Builder是一个辅助类，因此我们可以在不创建包含中间状态的完整副本的中间版本的情况下，
// 有效地将整个序列的versionEdits应用于特定状态。
// Builder就是将VersionEdit应用于当前Version的工具类。
class VersionSet::Builder {
 private:
  // Helper to sort by v->files_[file_number].smallest
  // 用于根据v->files_[file_number].smallest排序的辅助函数
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;

    // 若f1文件的最小key小于f2文件的最小key，则返回true
    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      int r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        // Break ties by file number
        // 如果两个文件的最小key相等，则根据文件编号从小到大排序
        return (f1->number < f2->number);
      }
    }
  }; 

  // 定义一个FileSet类型，是一个指向FileMetaData*的set集合，并使用BySmallestKey进行排序
  typedef std::set<FileMetaData*, BySmallestKey> FileSet;
  struct LevelState {
    // 存储被删除的文件编号
    std::set<uint64_t> deleted_files;
    // 存储新增的文件
    FileSet* added_files;
  };

  // 当前使用的VersionSet对象
  VersionSet* vset_;
  // 当前使用的Version对象，要基于该Version对象进行编辑
  Version* base_;
  // 用于存储每个层级的状态，LevelState中包含了新增的文件元信息和被删除的文件的ID
  LevelState levels_[config::kNumLevels];

 public:
  // Initialize a builder with the files from *base and other info from *vset
  // 使用*base中的文件和*vset中的其他信息初始化一个builder
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->Ref();
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    // 初始化levels_数组，为每个层级新增文件的集合分配内存
    for (int level = 0; level < config::kNumLevels; level++) {
      levels_[level].added_files = new FileSet(cmp);
    }
  }

  ~Builder() {
    for (int level = 0; level < config::kNumLevels; level++) {
      // 获取当前层级的新增文件集合，从而拿到其FileMetaData对象
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      // 获取当前层新增文件的FileMetaData对象。
      for (FileSet::const_iterator it = added->begin(); it != added->end();
           ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      // 当前流程已经不再使用这些文件，因此将其引用计数减1
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }
    // 当前流程已经不再使用该version，因此将其引用计数减1
    base_->Unref();
  }

  // Apply all of the edits in *edit to the current state.
  // 将VersionEdit中的所有编辑应用到当前状态。产生新的Version。
  // 该函数主要负责将VersionEdit中的压缩状态同步到VersionSet的compaction_pointer_中，
  // 将VersionEdit中的新增文件和删除文件同步到Builder的delta changes中，后续在SaveTo函数
  // 中会将base_和delta changes合并生成新的Version对象。
  void Apply(const VersionEdit* edit) {
    // Update compaction pointers
    // 更新每个层级的compact_pointer_，即下次compaction操作的起始key必须大于该值
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] =
          edit->compact_pointers_[i].second.Encode().ToString();
    }

    // Delete files
    // 同步本次compaction操作删除的文件
    for (const auto& deleted_file_set_kvp : edit->deleted_files_) {
      const int level = deleted_file_set_kvp.first; // 层级
      const uint64_t number = deleted_file_set_kvp.second; // 文件编号
      levels_[level].deleted_files.insert(number);
    }

    // Add new files
    // 同步本次compaction操作新增的文件
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first; // 层级
      // 为新增文件创建FileMetaData对象
      FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
      // 正在使用该文件，因此引用计数初始化为1
      f->refs = 1;

      // We arrange to automatically compact this file after
      // a certain number of seeks.  Let's assume:
      //   (1) One seek costs 10ms
      //   (2) Writing or reading 1MB costs 10ms (100MB/s)
      //   (3) A compaction of 1MB does 25MB of IO:
      //         1MB read from this level
      //         10-12MB read from next level (boundaries may be misaligned)
      //         10-12MB written to next level
      // This implies that 25 seeks cost the same as the compaction
      // of 1MB of data.  I.e., one seek costs approximately the
      // same as the compaction of 40KB of data.  We are a little
      // conservative and allow approximately one seek for every 16KB
      // of data before triggering a compaction.
      // 我们设计了一个自动触发compaction的机制，即每个文件允许的seek次数是一个固定值，超过这个值就会触发compaction。
      // 该值的计算基于以下假设：
      // 1. 一个seek操作耗时10ms。指的是从磁盘读取或写入1MB数据耗时10ms。
      // 2. 硬盘的读取速度是100MB/s，即向磁盘读取或写入1MB数据耗时10ms。
      // 3. 1MB数据的compaction操作会产生25MB的IO：
      //    1MB从当前层级读取
      //    10-12MB从下一层级读取(每一层最大大小为前一层的10倍，且需要考虑边界对齐的问题)
      //    执行归并排序后写入10-12MB数据到下一个层级
      // 这意味着25次seek的成本与1MB数据的compaction成本相同。也就是说，一个seek的成本大约等于40KB数据的compaction成本。
      // 我们稍微保守一点，允许每16KB数据触发一次compaction。即我们认为当对该文件的无效seek次数超过allowed_seeks时，
      // 意味着该文件中所有数据在当前都是无效的，需要触发compaction。
      f->allowed_seeks = static_cast<int>((f->file_size / 16384U));
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;
      
      // 若待删除文件集合中包含该文件，则将其从待删除文件集合中移。
      // 可能我们复用了该文件的序列号，因此需要将其从待删除文件集合中移除
      levels_[level].deleted_files.erase(f->number);
      // 将新增文件加入到对应层级的added_files集合中
      levels_[level].added_files->insert(f);
    }
  }

  // Save the current state in *v.
  // 将传入的base版本和Builder中的delta changes合并，生成新的Version对象保存到*v中。
  void SaveTo(Version* v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    // 遍历Builder中存储的每一层级对应的新增文件集合和被删除文件集合，将其应用到base_中，
    // 将产生的新的Version对象保存到*v中。
    for (int level = 0; level < config::kNumLevels; level++) {
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      // 合并新增文件集合和已存在文件集合，删除被删除的文件，将结果存储到*v中。
      // base版本中level层的文件集合
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
      std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
      // Builder中level层的新增文件集合
      const FileSet* added_files = levels_[level].added_files;
      // 因为要将最终的文件集合保存到v中，所以需要预先分配空间
      v->files_[level].reserve(base_files.size() + added_files->size());
      // 将base_版本、Builder中level层的新增文件集合，按照smallest key从小到大的顺序合并到v中
      for (const auto& added_file : *added_files) {
        // Add all smaller files listed in base_
        // upper_bound()返回第一个大于added_file的元素的迭代器
        // 该函数的作用是首先将base版本中level层的文件集合中小于added_file的文件尝试加入到v中。
        for (std::vector<FileMetaData*>::const_iterator bpos =
                 std::upper_bound(base_iter, base_end, added_file, cmp);
             base_iter != bpos; ++base_iter) {
          MaybeAddFile(v, level, *base_iter);
        }
        // 之后将added_file加入到v中
        MaybeAddFile(v, level, added_file);
      }

      // Add remaining base files
      // 将base版本中level层的文件集合中剩余的文件加入到v中，这些文件的
      // smallest key都大于Builder中level层的新增文件集合中最后一个文件的smallest key
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(v, level, *base_iter);
      }

#ifndef NDEBUG
      // Make sure there is no overlap in levels > 0
      // 确保level > 0的文件集合是有序且不重叠的
      if (level > 0) {
        for (uint32_t i = 1; i < v->files_[level].size(); i++) {
          const InternalKey& prev_end = v->files_[level][i - 1]->largest;
          const InternalKey& this_begin = v->files_[level][i]->smallest;
          if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
            std::fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                         prev_end.DebugString().c_str(),
                         this_begin.DebugString().c_str());
            std::abort();
          }
        }
      }
#endif
    }
  }

  // 根据版本v在层级level中的文件情况，决定是否将文件f加入到v中
  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    // 若该文件已被删除，则不加入到v中
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // File is deleted: do nothing
    } else {
      // 获取版本v中level层的文件集合
      std::vector<FileMetaData*>* files = &v->files_[level];
      // 当level > 0时，文件集合必须是有序且不重叠的
      if (level > 0 && !files->empty()) {
        // Must not overlap
        // 要求待加入的文件f的最小key大于当前level层的最大key，即当前层最后一个文件的largest key
        assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest,
                                    f->smallest) < 0);
      }
      // 将文件f加入到版本v中，并增加文件f的引用计数，意味着版本v正在使用该文件
      f->refs++;
      files->push_back(f);
    }
  }
};

VersionSet::VersionSet(const std::string& dbname, const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(nullptr),
      descriptor_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  // 释放对当前版本的引用
  current_->Unref();
  // versionset中的版本列表应该为空
  assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
  delete descriptor_log_;
  delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
  // Make "v" current
  // 在更新最新版本时，需要确保当前的版本current_的引用计数为0
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != nullptr) {
    current_->Unref();
  }
  current_ = v;
  v->Ref();

  // Append to linked list
  // 将v作为当前链表中的最新版本，插入到dummy_versions_之前
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

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
Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
  // 检查VerionEdit中的日志文件元数据信息是否合法
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    // 若VersionEdit中没有设置log_number_，则使用当前的log_number_
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  // 使用current_版本创建一个副本，Apply负责将edit中产生的变更信息同步到Vset和Builder的
  // delta changes中，最后SaveTo负责将base_和delta changes合并生成新的Version对象。
  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  // 新的version产生后，首先判断其是否需要进行size compaction，若需要则进行size compaction
  Finalize(v);

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  // 如果需要，通过创建一个包含当前版本快照的临时文件来初始化新的描述符日志文件。
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) { // 第一次打开数据库，需要创建MANIFEST文件
    // No reason to unlock *mu here since we only hit this path in the
    // first call to LogAndApply (when opening the database).
    // 这里没有理由解锁*mu，因为我们只会在第一次调用LogAndApply时（打开数据库时）进入这个路径。
    assert(descriptor_file_ == nullptr);
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      // 首次创建MANIFEST文件时，需要将当前版本信息通过Log::writer对象
      // 写入到MANIFEST文件中，即descriptor_file_对象。
      descriptor_log_ = new log::Writer(descriptor_file_);
      s = WriteSnapshot(descriptor_log_);
    }
  }

  // Unlock during expensive MANIFEST log write
  // 当向磁盘写入MANIFEST log record的时候，可以释放mu锁，提高并发度
  {
    mu->Unlock();

    // Write new record to MANIFEST log
    // 向MANIFEST log中写入新的record，其record格式与WAL log record格式相同
    if (s.ok()) {
      std::string record;
      // 将VersionEdit序列化为字符串，VersionEdit的内容就是Log record的Content
      // 字段，即VersionEdit中的所有变更信息都会被序列化到record中。
      edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
      if (s.ok()) { // 若写入到MANIFEST log成功，则将其刷盘
        s = descriptor_file_->Sync();
      }
      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
      }
    }

    // If we just created a new descriptor file, install it by writing a
    // new CURRENT file that points to it.
    // 如果刚刚创建了一个新的描述符文件-即MANIFEST文件，则通过写入一个新的CURRENT
    // 文件来安装它，该文件指向它。将新的MANIFEST文件的序列号更新到当前使用的CURRENT
    // 文件序列号。
    // 将MANIFEST文件的初始状态写入到log文件中
    if (s.ok() && !new_manifest_file.empty()) {
      s = SetCurrentFile(env_, dbname_, manifest_file_number_);
    }

    mu->Lock();
  }

  // Install the new version
  // 将新版本安装到VersionSet中
  if (s.ok()) {
    // 将v作为当前版本，插入到dummy_versions_之前
    AppendVersion(v);
    // 更新元数据信息
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    // 若写入失败，则释放v的引用计数，删除v
    delete v;
    // 并删除当前正在写入的MANIFEST文件，因为写入失败，所以不需要保存
    if (!new_manifest_file.empty()) {
      delete descriptor_log_;
      delete descriptor_file_;
      descriptor_log_ = nullptr;
      descriptor_file_ = nullptr;
      env_->RemoveFile(new_manifest_file);
    }
  }

  return s;
}

Status VersionSet::Recover(bool* save_manifest) {
  // 创建一个LogReporter对象，用于依次从Log文件中读取每一次版本变更时产生的
  // Log Record信息，从而逐步重建出最新的版本信息。
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    void Corruption(size_t bytes, const Status& s) override {
      if (this->status->ok()) *this->status = s;
    }
  };

  // Read "CURRENT" file, which contains a pointer to the current manifest file
  // 读取CURRENT文件，该文件包含指向当前MANIFEST文件的指针。current字符串中保存了
  // 序列化的CURRENT文件中的内容。
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  // 首先检查CURRENT文件是否读取成功，以及CURRENT文件是否以换行符结尾
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current[current.size() - 1] != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  current.resize(current.size() - 1);

  // 获取到current文件中的内容之后，构造出manifest文件的路径，
  // 这里的意思很明显是CURRENT文件中保存的是MANIFEST文件的名称，
  // 从而打开对应的manifest文件，读取其中的内容。
  std::string dscname = dbname_ + "/" + current;
  SequentialFile* file;
  s = env_->NewSequentialFile(dscname, &file);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return Status::Corruption("CURRENT points to a non-existent file",
                                s.ToString());
    }
    return s;
  }

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  Builder builder(this, current_);
  int read_records = 0;

  {
    LogReporter reporter;
    reporter.status = &s;
    // 通过LogReader对象逐条读取Log文件中的记录，逐步解析并应用每条记录，
    // 从而重建出最新的版本信息。
    log::Reader reader(file, &reporter, true /*checksum*/,
                       0 /*initial_offset*/);
    Slice record;
    std::string scratch;
    // 从Log文件中读取每一条记录，然后解析出VersionEdit对象，最后将VersionEdit
    // 应用到builder中，从而逐步重建出最新的版本信息。
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      ++read_records;
      // 从当前record解析出VersionEdit对象
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      // 解析成功后，要确保VersionEdit中的Comparator和当前VersionSet中的Comparator一致
      if (s.ok()) {
        if (edit.has_comparator_ &&
            edit.comparator_ != icmp_.user_comparator()->Name()) {
          s = Status::InvalidArgument(
              edit.comparator_ + " does not match existing comparator ",
              icmp_.user_comparator()->Name());
        }
      }

      // 将当前VersionEdit中的变更信息同步到VersionSet和Builder的delta changes中
      if (s.ok()) {
        builder.Apply(&edit);
      }

      // 将edit相关的元数据信息同步到VersionSet中
      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }

      if (edit.has_prev_log_number_) {
        prev_log_number = edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
  }
  // 关闭Log文件
  delete file;
  file = nullptr;

  // 检查这些元数据信息是否都存在，若不存在则返回Corruption错误
  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    } else if (!have_log_number) {
      s = Status::Corruption("no meta-lognumber entry in descriptor");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence-number entry in descriptor");
    }

    if (!have_prev_log_number) {
      prev_log_number = 0;
    }

    MarkFileNumberUsed(prev_log_number);
    MarkFileNumberUsed(log_number);
  }

  // 将恢复并重建出来的最终Version对象保存到VersionSet中
  if (s.ok()) {
    // 将current_版本与builder中的delta changes合并生成新的Version对象，
    // 将最终结果保存到v中。
    Version* v = new Version(this);
    builder.SaveTo(v);
    // Install recovered version
    // 新版本产生后，检查其是否需要进行Size Compaction，若需要则进行Size Compaction
    Finalize(v);
    // 将新版本安装到VersionSet中
    AppendVersion(v);
    // 更新VersionSet中的元数据信息
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;

    // See if we can reuse the existing MANIFEST file.
    // 检查是否可以重用现有的MANIFEST文件
    if (ReuseManifest(dscname, current)) {
      // No need to save new manifest
      // 若重用现存的MANIFEST文件，则不需要保存新的MANIFEST文件，
      // 可以继续接着使用现存的MANIFEST文件。
    } else { // 否则需要保存现在的MANIFEST文件
      *save_manifest = true;
    }
  } else { // 前面解析应用Log Record时出现错误
    std::string error = s.ToString();
    Log(options_->info_log, "Error recovering version set with %d records: %s",
        read_records, error.c_str());
  }

  return s;
}

bool VersionSet::ReuseManifest(const std::string& dscname,
                               const std::string& dscbase) {
  // 打开DB实例的时候就预设了是否选择重用MANIFEST文件和LOG文件
  if (!options_->reuse_logs) {
    return false;
  }
  FileType manifest_type;
  uint64_t manifest_number;
  uint64_t manifest_size;
  // dscbase中保存了CURRENT文件中的内容，即MANIFEST文件的名称
  // 从名称中解析出MANIFEST文件的序列号和类型
  // NOTE: ParseFileName函数是将文件名解析为文件类型和文件序列号，从该
  // 函数中可以知道MANIFEST文件的命名规则是MANIFEST-[0-9]+
  if (!ParseFileName(dscbase, &manifest_number, &manifest_type) ||
      manifest_type != kDescriptorFile ||
      !env_->GetFileSize(dscname, &manifest_size).ok() ||
      // Make new compacted MANIFEST if old one is too big
      manifest_size >= TargetFileSize(options_)) {
    // 若MANIFEST文件的类型不是kDescriptorFile，或者MANIFEST文件的大小超过了
    // 允许的单个文件的最大大小，则不重用MANIFEST文件。
    return false;
  }

  assert(descriptor_file_ == nullptr);
  assert(descriptor_log_ == nullptr);
  // 使用之前使用过的MANIFEST文件名创建一个可追加的文件对象，
  // 若文件之前已经有内容，则会接着之前的内容继续写入。
  Status r = env_->NewAppendableFile(dscname, &descriptor_file_);
  if (!r.ok()) {
    Log(options_->info_log, "Reuse MANIFEST: %s\n", r.ToString().c_str());
    assert(descriptor_file_ == nullptr);
    return false;
  }

  Log(options_->info_log, "Reusing MANIFEST %s\n", dscname.c_str());
  // 为当前MANIFEST文件创建一个log::Writer对象，用于实时记录MANIFEST文件的变更信息，
  // 即每次操作产生的VersionEdit都会被记录到该descriptor_file_文件中。
  // 若descriptor_file_已经有内容了，则会接着之前的内容继续写入。
  descriptor_log_ = new log::Writer(descriptor_file_, manifest_size);
  manifest_file_number_ = manifest_number;
  return true;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

void VersionSet::Finalize(Version* v) {
  // Precomputed best level for next compaction
  // 为当前Version找到最需要进行Size Compaction的level
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < config::kNumLevels - 1; level++) {
    double score;
    if (level == 0) {
      // We treat level-0 specially by bounding the number of files
      // instead of number of bytes for two reasons:
      // 我们特别对待level-0，通过限制文件数量而不是字节数来处理两个原因：
      //
      // (1) With larger write-buffer sizes, it is nice not to do too
      // many level-0 compactions.
      // (1) 使用更大的写缓冲区大小，最好不要做太多的level-0压缩。
      //
      // (2) The files in level-0 are merged on every read and
      // therefore we wish to avoid too many files when the individual
      // file size is small (perhaps because of a small write-buffer
      // setting, or very high compression ratios, or lots of
      // overwrites/deletions).
      // (2) level-0中的文件在每次读取时都会合并，因此当单个文件大小较小时(可能是因为写缓冲区设置较小，
      // 或者压缩比例很高，或者有很多覆盖/删除)我们希望避免太多文件。

      // Level-0的分数通过Level0的文件个数除以kL0_CompactionTrigger来计算，
      // 默认的kL0_CompactionTrigger是4，所以Level-0的分数是Level0的文件个数除以4。
      score = v->files_[level].size() /
              static_cast<double>(config::kL0_CompactionTrigger);
    } else {
      // Compute the ratio of current size to size limit.
      // Level1-6的分数通过Level的文件大小除以该Level允许的最大字节数来计算。
      const uint64_t level_bytes = TotalFileSize(v->files_[level]);
      score =
          static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level);
    }

    // 分数最高的level会被选为最需要进行Size Compaction的level
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }

  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
  // TODO: Break up into multiple records to reduce memory usage on recovery?

  // Save metadata
  // 将当前VersionSet中的最新版本保存到VersionEdit中，之后序列化VersionEdit为字符串，
  // 最后将字符串作为Content字段写入到kDescriptorFile类型的日志文件中。
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  // Save compaction pointers
  // 保存当前版本中每一层级的compact pointer，即下次compaction操作的起始key
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  // Save files
  // 将当前版本中现存的文件信息保存到VersionEdit中
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  // 将VersionEdit序列化为字符串，VersionEdit的内容就是Log record的Content字段，
  std::string record;
  edit.EncodeTo(&record);
  // 将record写入到kDescriptorFile类型的日志文件中
  return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
  // Update code if kNumLevels changes
  static_assert(config::kNumLevels == 7, "");
  std::snprintf(
      scratch->buffer, sizeof(scratch->buffer), "files[ %d %d %d %d %d %d %d ]",
      int(current_->files_[0].size()), int(current_->files_[1].size()),
      int(current_->files_[2].size()), int(current_->files_[3].size()),
      int(current_->files_[4].size()), int(current_->files_[5].size()),
      int(current_->files_[6].size()));
  return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    // 获取level层的当前存在的文件集合
    const std::vector<FileMetaData*>& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        // 文件的最大key小于ikey，说明整个文件都在ikey之前，所以直接加上文件大小
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        // Entire file is after "ikey", so ignore
        // 文件的最小key大于ikey，说明整个文件都在ikey之后，所以忽略
        if (level > 0) { // 若level > 0，则后续的文件都不会包含ikey的数据，level>0时SSTable文件不相交
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for
          // "ikey".
          // level > 0时，除了level 0，其他level的文件都是按照meta->smallest排序的，
          // 所以在这个level中没有更多的文件会包含"ikey"的数据。level>0时，SSTable文件不相交。
          // 说明已经找到了ikey所在的SSTable文件，后续的文件都不会包含ikey的数据，所以直接返回result。
          break;
        }
      } else { // 说明当前file符合: files[i]->smallest <= ikey <= files[i]->largest
        // "ikey" falls in the range for this table.  Add the
        // approximate offset of "ikey" within the table.
        // ikey在这个文件的范围内，计算ikey在这个文件中的大致偏移量。
        Table* tableptr;
        Iterator* iter = table_cache_->NewIterator(
            ReadOptions(), files[i]->number, files[i]->file_size, &tableptr);
        if (tableptr != nullptr) {
          result += tableptr->ApproximateOffsetOf(ikey.Encode());
        }
        delete iter;
      }
    }
  }
  return result;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  // 不做说明，直接看代码
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

int64_t VersionSet::NumLevelBytes(int level) const {
  // 不做说明，直接看代码
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  // 计算level层和level+1层之间的最大重叠字节数
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    // 计算level层和level+1层之间的最大重叠字节数
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      const FileMetaData* f = current_->files_[level][i];
      // 在level+1层中查找和level层文件f重叠的文件，将重叠的文件加入到overlaps中
      current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest,
                                     &overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      // 计算level层和level+1层之间的最大重叠字节数
      if (sum > result) {
        result = sum;
      }
    }
  }
  // 返回level层和level+1层之间的最大重叠字节数
  return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
// 获取inputs中所有文件的最小key和最大key，存储到smallest和largest中
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs,
                          InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  // 依次遍历inputs中的所有文件，最后就能得到所有文件的最小key和最大key
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
// 获取inputs1和inputs2中所有文件的最小key和最大key，存储到smallest和largest中
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2,
                           InternalKey* smallest, InternalKey* largest) {
  // 将inputs1和inputs2中的所有文件合并到all中
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  // 获取all中所有文件的最小key和最大key，存储到smallest和largest中
  GetRange(all, smallest, largest);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  // 遍历compaction对象时的数据不需要填充到cache中
  options.fill_cache = false;

  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  // Level-0的文件必须合并在一起。对于其他级别，我们将为每个级别创建一个concatenating迭代器。
  // TODO: 如果没有重叠，对于level-0也可以使用concatenating迭代器
  const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
  Iterator** list = new Iterator*[space];
  int num = 0;
  for (int which = 0; which < 2; which++) {
    if (!c->inputs_[which].empty()) {
      if (c->level() + which == 0) {
        const std::vector<FileMetaData*>& files = c->inputs_[which];
        for (size_t i = 0; i < files.size(); i++) {
          list[num++] = table_cache_->NewIterator(options, files[i]->number,
                                                  files[i]->file_size);
        }
      } else {
        // Create concatenating iterator for the files from this level
        list[num++] = NewTwoLevelIterator(
            new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
            &GetFileIterator, table_cache_, options);
      }
    }
  }
  assert(num <= space);
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}

Compaction* VersionSet::PickCompaction() {
  Compaction* c;
  int level;

  // We prefer compactions triggered by too much data in a level over
  // the compactions triggered by seeks.
  const bool size_compaction = (current_->compaction_score_ >= 1);
  const bool seek_compaction = (current_->file_to_compact_ != nullptr);
  if (size_compaction) {
    level = current_->compaction_level_;
    assert(level >= 0);
    assert(level + 1 < config::kNumLevels);
    c = new Compaction(options_, level);

    // Pick the first file that comes after compact_pointer_[level]
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      FileMetaData* f = current_->files_[level][i];
      if (compact_pointer_[level].empty() ||
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(f);
        break;
      }
    }
    if (c->inputs_[0].empty()) {
      // Wrap-around to the beginning of the key space
      c->inputs_[0].push_back(current_->files_[level][0]);
    }
  } else if (seek_compaction) {
    level = current_->file_to_compact_level_;
    c = new Compaction(options_, level);
    c->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return nullptr;
  }

  c->input_version_ = current_;
  c->input_version_->Ref();

  // Files in level 0 may overlap each other, so pick up all overlapping ones
  if (level == 0) {
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    // Note that the next call will discard the file we placed in
    // c->inputs_[0] earlier and replace it with an overlapping set
    // which will include the picked file.
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }

  SetupOtherInputs(c);

  return c;
}

// Finds the largest key in a vector of files. Returns true if files is not
// empty.
bool FindLargestKey(const InternalKeyComparator& icmp,
                    const std::vector<FileMetaData*>& files,
                    InternalKey* largest_key) {
  if (files.empty()) {
    return false;
  }
  *largest_key = files[0]->largest;
  for (size_t i = 1; i < files.size(); ++i) {
    FileMetaData* f = files[i];
    if (icmp.Compare(f->largest, *largest_key) > 0) {
      *largest_key = f->largest;
    }
  }
  return true;
}

// Finds minimum file b2=(l2, u2) in level file for which l2 > u1 and
// user_key(l2) = user_key(u1)
FileMetaData* FindSmallestBoundaryFile(
    const InternalKeyComparator& icmp,
    const std::vector<FileMetaData*>& level_files,
    const InternalKey& largest_key) {
  const Comparator* user_cmp = icmp.user_comparator();
  FileMetaData* smallest_boundary_file = nullptr;
  for (size_t i = 0; i < level_files.size(); ++i) {
    FileMetaData* f = level_files[i];
    if (icmp.Compare(f->smallest, largest_key) > 0 &&
        user_cmp->Compare(f->smallest.user_key(), largest_key.user_key()) ==
            0) {
      if (smallest_boundary_file == nullptr ||
          icmp.Compare(f->smallest, smallest_boundary_file->smallest) < 0) {
        smallest_boundary_file = f;
      }
    }
  }
  return smallest_boundary_file;
}

// Extracts the largest file b1 from |compaction_files| and then searches for a
// b2 in |level_files| for which user_key(u1) = user_key(l2). If it finds such a
// file b2 (known as a boundary file) it adds it to |compaction_files| and then
// searches again using this new upper bound.
//
// If there are two blocks, b1=(l1, u1) and b2=(l2, u2) and
// user_key(u1) = user_key(l2), and if we compact b1 but not b2 then a
// subsequent get operation will yield an incorrect result because it will
// return the record from b2 in level i rather than from b1 because it searches
// level by level for records matching the supplied user key.
//
// parameters:
//   in     level_files:      List of files to search for boundary files.
//   in/out compaction_files: List of files to extend by adding boundary files.
void AddBoundaryInputs(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>& level_files,
                       std::vector<FileMetaData*>* compaction_files) {
  InternalKey largest_key;

  // Quick return if compaction_files is empty.
  if (!FindLargestKey(icmp, *compaction_files, &largest_key)) {
    return;
  }

  bool continue_searching = true;
  while (continue_searching) {
    FileMetaData* smallest_boundary_file =
        FindSmallestBoundaryFile(icmp, level_files, largest_key);

    // If a boundary file was found advance largest_key, otherwise we're done.
    if (smallest_boundary_file != NULL) {
      compaction_files->push_back(smallest_boundary_file);
      largest_key = smallest_boundary_file->largest;
    } else {
      continue_searching = false;
    }
  }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest, largest;

  AddBoundaryInputs(icmp_, current_->files_[level], &c->inputs_[0]);
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level + 1, &smallest, &largest,
                                 &c->inputs_[1]);
  AddBoundaryInputs(icmp_, current_->files_[level + 1], &c->inputs_[1]);

  // Get entire range covered by compaction
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    AddBoundaryInputs(icmp_, current_->files_[level], &expanded0);
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const int64_t expanded0_size = TotalFileSize(expanded0);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size <
            ExpandedCompactionByteSizeLimit(options_)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      current_->GetOverlappingInputs(level + 1, &new_start, &new_limit,
                                     &expanded1);
      AddBoundaryInputs(icmp_, current_->files_[level + 1], &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) {
        Log(options_->info_log,
            "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
            level, int(c->inputs_[0].size()), int(c->inputs_[1].size()),
            long(inputs0_size), long(inputs1_size), int(expanded0.size()),
            int(expanded1.size()), long(expanded0_size), long(inputs1_size));
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  if (level + 2 < config::kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }

  // Update the place where we will do the next compaction for this level.
  // We update this immediately instead of waiting for the VersionEdit
  // to be applied so that if the compaction fails, we will try a different
  // key range next time.
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin,
                                     const InternalKey* end) {
  std::vector<FileMetaData*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) {
    return nullptr;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // But we cannot do this for level-0 since level-0 files can overlap
  // and we must not pick one file and drop another older file if the
  // two files overlap.
  if (level > 0) {
    const uint64_t limit = MaxFileSizeForLevel(options_, level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }

  Compaction* c = new Compaction(options_, level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}

Compaction::Compaction(const Options* options, int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(options, level)),
      input_version_(nullptr),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
  for (int i = 0; i < config::kNumLevels; i++) {
    level_ptrs_[i] = 0;
  }
}

Compaction::~Compaction() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
  }
}

bool Compaction::IsTrivialMove() const {
  const VersionSet* vset = input_version_->vset_;
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  return (num_input_files(0) == 1 && num_input_files(1) == 0 &&
          TotalFileSize(grandparents_) <=
              MaxGrandParentOverlapBytes(vset->options_));
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->RemoveFile(level_ + which, inputs_[which][i]->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Maybe use binary search to find right entry instead of linear search?
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    while (level_ptrs_[lvl] < files.size()) {
      FileMetaData* f = files[level_ptrs_[lvl]];
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // Key falls in this file's range, so definitely not base level
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  const VersionSet* vset = input_version_->vset_;
  // Scan to find earliest grandparent file that contains key.
  const InternalKeyComparator* icmp = &vset->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
         icmp->Compare(internal_key,
                       grandparents_[grandparent_index_]->largest.Encode()) >
             0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;

  if (overlapped_bytes_ > MaxGrandParentOverlapBytes(vset->options_)) {
    // Too much overlap for current output; start new output
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}

void Compaction::ReleaseInputs() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
    input_version_ = nullptr;
  }
}

}  // namespace leveldb
