// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

namespace leveldb {

struct TableAndFile {
  // 该SSTable本身的文件句柄，用于随机读取文件内容。
  RandomAccessFile* file;
  // 该SSTable对应的Table实例，存放了从SSTable预读取的一些索引数据、
  // 还有一些元数据。
  Table* table;
};

/**
 * DeleteEntry函数用于删除缓存中的条目，释放TableAndFile实例。这里针对的
 * 是TableCache中的条目，每个条目对应一个SSTable文件。
 * @param key 是SSTable.file_number的编码结果，即SSTable的编号。
 * @param value 是TableAndFile实例的指针。
*/
static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

/**
 * UnrefEntry函数用于释放arg1的Cache实例中的arg2节点。
 * @param arg1 是Cache实例的指针。
 * @param arg2 是Cache::Handle实例的指针。
*/
static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname, const Options& options,
                       int entries)
    : env_(options.env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  // 将file_number封装为key，很明显TableCache中的kv对是以file_number为key的。
  Slice key(buf, sizeof(buf));
  // 以file_number为key，在cache_中获取对应表的句柄。即LRUHandle实例。
  *handle = cache_->Lookup(key);
  // 若在缓存中没有找到对应的句柄，则需要从文件中加载对应的SSTable文件。
  if (*handle == nullptr) {
    // 根据dbname和file_number构造SSTable文件的文件名。
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    // 尝试打开该表文件
    s = env_->NewRandomAccessFile(fname, &file);
    // 如果打开失败，尝试打开旧的表文件
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }

    // 如果文件打开成功，则进一步的打开Table实例。 
    if (s.ok()) {
      s = Table::Open(options_, file, file_size, &table);
    }

    // 如果表打开失败，则释放文件句柄。
    if (!s.ok()) {
      // 确保table指针为空，并且删除文件句柄。
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
      // 我们不缓存错误结果，以便如果错误是短暂的，或者有人修复了文件，我们会自动恢复。
    } else { // 如果表打开成功
      // 将文件句柄和Table实例封装为TableAndFile实例。
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      // 很明显，将file_number作为key，TableAndFile实例作为value插入到cache_中。
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  // 将tableptr指向的Table实例初始化为nullptr。
  // 注意tableptr存储的是Table实例的指针的指针。
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  // 首先根据file_number从cache_中查找对应的TableAndFile实例。
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  // 从TableAndFile实例中获取Table实例。
  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  // 为该Iterator注册清理函数，以便在Iterator被销毁时，释放对应的Table实例。
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  // 当操作成功时，将tableptr指向的Table实例指向table。
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}

/**
 * Get函数首先读取指定的SSTable文件，然后调用Table::InternalGet函数查找指定的key。
 * @param options 读选项。
 * @param file_number SSTable文件的编号。
 * @param file_size 要读取的SSTable文件的指定范围，[0, file_size)。
 * @param k 要查找的key。
*/
Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  // 保存对应的Cache::Handle实例，本质上是指向TableAndFile实例的指针。
  Cache::Handle* handle = nullptr;
  // 首先根据file_number从cache_中查找对应的TableAndFile实例。
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    // 这里可以通过reinterpret_cast逐字节拷贝，将Cache::Handle实例转换为TableAndFile实例。
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 从对应的实例中查找指定的key。若找到了对应的key，则调用handle_result函数处理。
    s = t->InternalGet(options, k, arg, handle_result);
    // 查找完成后，释放对应的Cache::Handle实例。
    cache_->Release(handle);
  }
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  // 将file_number编码为key，然后从cache_中删除对应的条目。
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
