// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/iterator.h"

namespace leveldb {

class Block;
class BlockHandle;
class Footer;
struct Options;
class RandomAccessFile;
struct ReadOptions;
class TableCache;

// A Table is a sorted map from strings to strings.  Tables are
// immutable and persistent.  A Table may be safely accessed from
// multiple threads without external synchronization.
// Table是一个有序的字符串到字符串的映射。Table是不可变的和持久化的。
// Table可以安全地被多个线程并发访问，而不需要外部同步机制。
// Table是SSTable(Sorted String Table)的主要实现载体，而其内部封装的Rep对象存储了访问一个SSTable所需的所有信息。
// 因此可以认为Table是对Rep对象的进一步封装，提供了一些访问Rep对象的接口。
// 
// 每个db实例都有一个TableCache实例，TableCache中封装了一个ShardedLRUCache实例，用于缓存Table实例。
// TableCache中的kv对的key是SSTable的编号，value统一为Cache::Handle，里面的value是TableAndFile实例的指针。
// BlockCache中的kv对的key是cached_id+block_offset，value统一为Cache::Handle，里面的value是Block实例的指针。
// - block_offset是从BlockHandle中获取的，表示Block在SSTable中的偏移量。
// 
class LEVELDB_EXPORT Table {
 public:
  // Attempt to open the table that is stored in bytes [0..file_size)
  // of "file", and read the metadata entries necessary to allow
  // retrieving data from the table.
  // 尝试打开存储在file对象中的[0..file_size)字节范围内的table，并读取必要的元数据条目
  // 以允许从table中检索数据。
  //
  // If successful, returns ok and sets "*table" to the newly opened
  // table.  The client should delete "*table" when no longer needed.
  // If there was an error while initializing the table, sets "*table"
  // to nullptr and returns a non-ok status.  Does not take ownership of
  // "*source", but the client must ensure that "source" remains live
  // for the duration of the returned table's lifetime.
  // 如果成功，返回ok并将“*table”设置为新打开的table。当不再需要“*table”时，客户端应该删除“*table”。
  // 如果在初始化table时出现错误，则将“*table”设置为nullptr并返回非ok状态。
  // 不拥有“*source”的所有权，但客户端必须确保“source”在返回的table的生命周期内保持活动状态。
  //
  // *file must remain live while this Table is in use.
  // *file在Table使用期间必须保持活动状态。
  static Status Open(const Options& options, RandomAccessFile* file,
                     uint64_t file_size, Table** table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  ~Table();

  // Returns a new iterator over the table contents.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  // 返回一个新的迭代器，用于遍历table的内容。
  // NewIterator()的结果最初是无效的（调用者必须在使用迭代器之前调用其中的一个Seek方法）。
  Iterator* NewIterator(const ReadOptions&) const;

  // Given a key, return an approximate byte offset in the file where
  // the data for that key begins (or would begin if the key were
  // present in the file).  The returned value is in terms of file
  // bytes, and so includes effects like compression of the underlying data.
  // E.g., the approximate offset of the last key in the table will
  // be close to the file length.
  // 给定一个key，通过查找index block，返回key对应的data block在SSTable中的起始位置的近似偏移量。
  // 若key不存在，则默认返回meta_index block的起始位置。
  uint64_t ApproximateOffsetOf(const Slice& key) const;

 private:
  friend class TableCache;
  struct Rep;

  // 用于将IndexBlock中的kv的value(压缩后的BlockHandle,是特定data block在SSTable中的位置和大小)进行解压，
  // 之后根据BlockHandle的信息从SSTable中读取对应的data block。
  // 之后根据data block的内容构建一个Iterator对象，用于遍历data block中的所有kv。
  static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

  // rep_是Table的内部实现，封装了访问一个SSTable需要的所有元数据信息
  explicit Table(Rep* rep) : rep_(rep) {}

  // Calls (*handle_result)(arg, ...) with the entry found after a call
  // to Seek(key).  May not make such a call if filter policy says
  // that key is not present.
  // 在调用Seek(key)后，使用找到的entry调用(*handle_result)(arg, ...)。
  // 通俗来说，当我们找到key之后，可能会对key进行一些处理，(*handle_result)(arg, ...)就是对key进行处理的函数。
  // 如果filter policy表示key不存在，则可能不会进行这样的调用。
  Status InternalGet(const ReadOptions&, const Slice& key, void* arg,
                     void (*handle_result)(void* arg, const Slice& k,
                                           const Slice& v));

  // 从SSTable的footer中读取元数据信息，即读取metaindex_block和index_block的BlockHandle
  void ReadMeta(const Footer& footer);
  // 在ReadMeta中调用，根据meta index block中存放的filter block的BlockHandle读取filter block
  void ReadFilter(const Slice& filter_handle_value);

  // 封装了访问一个SSTable需要的所有元数据信息
  Rep* const rep_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_H_
