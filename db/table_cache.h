// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Thread-safe (provides internal synchronization)

#ifndef STORAGE_LEVELDB_DB_TABLE_CACHE_H_
#define STORAGE_LEVELDB_DB_TABLE_CACHE_H_

#include <cstdint>
#include <string>

#include "db/dbformat.h"
#include "leveldb/cache.h"
#include "leveldb/table.h"
#include "port/port.h"

namespace leveldb {

class Env;

/**
 * 每个sstable文件对应一个Table实例，TableCache类用于管理这些Table实例。
 * SSTable文件被加载到内存后，会被缓存到TableCache中。每一个db实例都会持有
 * 一个TableCache实例，TableCache实例进一步的封装了Cache实例。
 * 
 * TableCache:kv对的key是对应文件的file_number，value是TableAndFile实例的指针。
 * BlockCache在DB启动的时候作为一个可选项被初始化。
*/
class TableCache {
 public:

  TableCache(const std::string& dbname, const Options& options, int entries);

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  ~TableCache();

  // Return an iterator for the specified file number (the corresponding
  // file length must be exactly "file_size" bytes).  If "tableptr" is
  // non-null, also sets "*tableptr" to point to the Table object
  // underlying the returned iterator, or to nullptr if no Table object
  // underlies the returned iterator.  The returned "*tableptr" object is owned
  // by the cache and should not be deleted, and is valid for as long as the
  // returned iterator is live.
  // 返回一个指向指定文件编号的迭代器(相应的文件长度必须正好为“file_size”字节)。
  // 如果“tableptr”不为空，则还将“*tableptr”设置为指向返回的迭代器的底层的Table对象，
  // 如果返回的迭代器下没有Table对象，则将*tableptr设置为nullptr。返回的“*tableptr”对象由缓存拥有，
  // 并且禁止被删除，并且在返回的迭代器的活跃期间有效。
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number,
                        uint64_t file_size, Table** tableptr = nullptr);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value).
  // 如果在指定文件中查找内部键“k”找到一个条目，则调用(*handle_result)(arg, found_key, found_value)。
  // 即如果找到了指定的key的Entry，则调用handle_result函数处理该Entry。
  Status Get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, const Slice& k, void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // Evict any entry for the specified file number
  // 该函数用于清除指定文件编号的所有cache entries。
  void Evict(uint64_t file_number);

 private:
  /**
   * FindTable用于根据file_number查找Table实例，如果找到了对应的Table实例，
   * 则返回对应的Cache::Handle实例，该Cache::Handle实例指向的是TableAndFile实例。
   * @param file_number SSTable文件的编号。
   * @param file_size 要读取SSTable文件中的[0, file_size)范围的数据。
   * @param handle 返回的Cache::Handle实例，实际上是指向TableAndFile实例的指针。
  */
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

  // 用来操作TableAndFile实例中的RandomAccessFile实例
  Env* const env_;
  // 每个db实例对应一个TableCache实例，dbname_是db实例的名字。
  const std::string dbname_;
  // 在打开该TableCache时的指定的选项
  // TODO:注意，BlockCache就在options_中被初始化，即在DB打开的时候会初始化BlockCache。
  const Options& options_;
  // 实际上是ShardedLRUCache实例，是TableCache的底层实现。
  Cache* cache_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_TABLE_CACHE_H_
