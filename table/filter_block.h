// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A filter block is stored near the end of a Table file.  It contains
// filters (e.g., bloom filters) for all data blocks in the table combined
// into a single filter block.
// filter
// block存储在Table文件的末尾附近。它包含了表中所有数据块的filters(例如，布隆过滤器)组合成一个单独的filter
// block。
// LevelDB默认2KB数据生成一个filter，每个filter包含多个key，每个key对应一个bit，每个key对应的bit位在filter中的位置由hash函数计算得到。

#ifndef STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "leveldb/slice.h"

#include "util/hash.h"

namespace leveldb {

class FilterPolicy;

// A FilterBlockBuilder is used to construct all of the filters for a
// particular Table.  It generates a single string which is stored as
// a special block in the Table.
// FilterBlockBuilder用于构建特定Table的所有filters。它生成一个字符串，该字符串存储为Table中的特殊块。
//
// The sequence of calls to FilterBlockBuilder must match the regexp:
//      (StartBlock AddKey*)* Finish
// FilterBlockBuilder的调用序列必须匹配正则表达式：
//      (StartBlock AddKey*)* Finish
class FilterBlockBuilder {
 public:
  // 用指定的FilterPolicy构造FilterBlockBuilder
  explicit FilterBlockBuilder(const FilterPolicy*);

  // 禁止拷贝构造函数和赋值运算符
  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  /**
   * 根据SSTable中data block的偏移量批量生成filter,一旦有新的data
   * block产生，需要调用StartBlock()方法， 在写入数据的同时生成对应的filter。
   *
   * 在调用若干次AddKey()方法后，可以调用一次StartBlock()方法，查看是否需要生成新的filter。
   * @param block_offset SSTable中data block的偏移量
   */
  void StartBlock(uint64_t block_offset);

  /**
   * 添加key到当前filter中，key会被序列化后添加到keys_中，并更新start_数组
   * @param key 待添加的key
   */
  void AddKey(const Slice& key);

  /**
   * Finish()方法负责将当前的数据组装起来并生成Filter Block。
   * SSTable中我们看到的Filter Block就是通过Finish()方法生成的。
  */
  Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  // keys_保存自上次调用GenerateFilter()以来添加的所有key。
  // 本质上是将key序列化后追加到keys_中
  std::string keys_;
  // start_保存每个key在keys_中的起始位置
  std::vector<size_t> start_;
  std::string result_;  // Filter data computed so far
  // 当调用GenerateFilter(),且keys_中有新添加的key时，会将keys_中的key转化为Slice对象保存在tmp_keys_中，
  // 用于调用policy_->CreateFilter()方法生成filter
  std::vector<Slice> tmp_keys_;
  // filter_offsets_保存每个filter在result_中的起始位置
  std::vector<uint32_t> filter_offsets_;
};

/**
 * FilterBlockReader用于读取FilterBlockBuilder产生的Filter Block。
 * 这里可以类比Block和BlockBuilder的关系。
 * 
 * 一般使用BloomFilterPolicy作为FilterPolicy，Filter Block中的Filter数据是通过BloomFilterPolicy生成的。
 * BloomFilterPolicy可能存在一定的误判(False Positive),当查询结果显示某个元素不存在时，该元素一定不存在；
 * 但是当查询结果显示某个元素存在时，该元素可能不存在（False Positive）。误判的概率取决于BloomFilter的
 * 大小和哈希函数的数量。
*/
class FilterBlockReader {
 public:
  // REQUIRES: "contents" and *policy must stay live while *this is live.
  // 要求："contents"和*policy在*this存活期间必须保持活动。
  /**
   * 构造函数
   * @param policy FilterPolicy对象,一般是BloomFilterPolicy对象
   * @param contents Filter Block的Data部分
  */
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);
  bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  // 该FilterBlockReader对象读取的Filter Block采取的FilterPolicy，一般是BloomFilterPolicy对象
  const FilterPolicy* policy_;
  // 指向该filter block的data部分的起始位置
  const char* data_;   
  // 指向该filter block的offset数组的起始位置，即filter offset's start
  // 同时也是所有filter data的总字节长度
  const char* offset_; 
  // 该filter block中filter data的数量
  size_t num_;          
  // 编码值，默认是2048，即2KB数据生成一个filter
  size_t base_lg_;      // Encoding parameter (see kFilterBaseLg in .cc file)
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
