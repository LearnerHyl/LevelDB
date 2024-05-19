// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>

#include "leveldb/iterator.h"

namespace leveldb {

struct BlockContents;
class Comparator;

/**
 * Block是一个数据块，用于存储数据。Block的物理结构如下:
 * Data | Type | CRC
 * 这里主要关注Data部分,Data部分的逻辑结构如下(以DataBlock为例):
 * -------------------------
 * kv Entry 1(sharedLen, nonSharedLen, valueLen, keyDelta, value)
 * kv Entry 2
 * ....
 * kv Entry n
 * ----------
 * Restart Point 1
 * Restart Point 2
 * ....
 * Restart Point m
 * ---------------
 * NumRestarts
 * ----------
 * 其中，kv Entry是一个键值对，Restart
 * Point是一个重启点，NumRestarts是重启点的数量。
 * 重启点实现了Block的快速查找，每个重启点存储了一个键值对的偏移量。
 * 重启点是实现前缀压缩存储的关键，每一个重启点对应的键值对的key是一个完整的key，
 */

class Block {
 public:
  // Initialize the block with the specified contents.
  // 用指定的BlockContents初始化Block
  explicit Block(const BlockContents& contents);

  // Block禁止拷贝和赋值
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  // Block析构时，如果Block拥有data_的内存，则需要在析构时释放data_的内存
  ~Block();

  // 返回Block中的Data部分的大小
  size_t size() const { return size_; }
  // Block基于data_和size_创建一个Iterator，用于遍历Block中的所有键值对
  Iterator* NewIterator(const Comparator* comparator);

 private:
  class Iter;

  // 获取当前Block中的重启点数量
  uint32_t NumRestarts() const;

  // data_指向Block的数据部分，即除去type和CRC的部分
  const char* data_;
  // size_是data_部分的大小，Data包含了KV数据和重启点相关的数据
  size_t size_;
  // restart_offset_是restart数组在data_中的起始位置的偏移量
  uint32_t restart_offset_;
  // owned_代表了BlockContent中的heap_allocated字段，即代表数据是用户自己从堆上分配的,
  // 还是属于Arena统一管理的(false表示属于Arena管理，true表示用户自己从堆上分配，需要自己释放)
  bool owned_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
