// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_
#define STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_

#include "leveldb/iterator.h"

namespace leveldb {

struct ReadOptions;

// Return a new two level iterator.  A two-level iterator contains an
// index iterator whose values point to a sequence of blocks where
// each block is itself a sequence of key,value pairs.  The returned
// two-level iterator yields the concatenation of all key/value pairs
// in the sequence of blocks.  Takes ownership of "index_iter" and
// will delete it when no longer needed.
// 返回一个新的两级迭代器。两级迭代器包含一个index block的迭代器，index block每一个kv对中的value都
// 存储了一个data block在sstable文件中的偏移量。返回的two-level iterator可以为该SSTable中的所有data block
// 产生一个迭代器。取得"index_iter"的所有权，当不再需要时将删除它。
//
// Uses a supplied function to convert an index_iter value into
// an iterator over the contents of the corresponding block.
// 使用提供的函数将index_iter的value，该value是一个待解码的blockhandle，
// 使用block_function将其转换该blockhandle对应的data block的迭代器。
Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                const Slice& index_value),
    void* arg, const ReadOptions& options);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_
