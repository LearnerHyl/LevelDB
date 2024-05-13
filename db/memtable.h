// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include "db/dbformat.h"
#include "db/skiplist.h"
#include <string>

#include "leveldb/db.h"

#include "util/arena.h"

namespace leveldb {

class InternalKeyComparator;
class MemTableIterator;

class MemTable {
 public:
  // MemTables are reference counted.  The initial reference count
  // is zero and the caller must call Ref() at least once.
  // MemTable是引用计数的。初始引用计数为零，调用者必须至少调用一次Ref()。
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Increase reference count.
  // 增加引用计数。
  void Ref() { ++refs_; }

  // Drop reference count.  Delete if no more references exist.
  // 减少引用计数。如果没有更多的引用存在，则删除。
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  // Returns an estimate of the number of bytes of data in use by this
  // data structure. It is safe to call when MemTable is being modified.
  // 返回此数据结构中正在使用的数据字节数的估计值。在修改MemTable时调用是安全的。
  // 本质上返回的是此MemTable对象中arena_内存池已经申请的内存大小
  size_t ApproximateMemoryUsage();

  // Return an iterator that yields the contents of the memtable.
  // 返回一个迭代器，该迭代器产生MemTable的内容。
  //
  // The caller must ensure that the underlying MemTable remains live
  // while the returned iterator is live.  The keys returned by this
  // iterator are internal keys encoded by AppendInternalKey in the
  // db/format.{h,cc} module.
  // 调用者必须确保底层MemTable在返回的迭代器仍然活动时保持活动状态。
  // 此迭代器返回的键是由db/format.{h,cc}模块中的AppendInternalKey编码的内部键。
  Iterator* NewIterator();

  // Add an entry into memtable that maps key to value at the
  // specified sequence number and with the specified type.
  // Typically value will be empty if type==kTypeDeletion.
  // 将一个条目添加到MemTable中，该条目将键映射到具有指定序列号和指定类型的值。
  // 如果type==kTypeDeletion，则通常值将为空。
  /**
   * @param seq 序列号, 用于标识这个操作代表的版本，比如说上一个操作的序列号是10，那么这个操作的序列号就是11
   * @param type 操作类型，kTypeValue表示插入这个key-value对，kTypeDeletion表示删除这个key
  */
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // If memtable contains a value for key, store it in *value and return true.
  // If memtable contains a deletion for key, store a NotFound() error
  // in *status and return true.
  // Else, return false.
  // 若Memtable中包含指定key的值，则将其存储在*value中并返回true。
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  friend class MemTableIterator;
  friend class MemTableBackwardIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  typedef SkipList<const char*, KeyComparator> Table;

  ~MemTable();  // Private since only Unref() should be used to delete it

  KeyComparator comparator_;  // 用于比较两个key的大小
  int refs_;                  // 当前MemTable对象的引用计数
  Arena arena_;               // 用Arena内存池来管理内存
  Table table_;               // 定义了比较函数的跳表
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
