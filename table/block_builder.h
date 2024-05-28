// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

namespace leveldb {

struct Options;

// BlockBuilder对象用于生成一个Block的Data部分。一个Block的物理组成为：
// Data | Compression type | CRC。
class BlockBuilder { 
 public:
  //  构造时需要传入Options，表示一些配置信息
  explicit BlockBuilder(const Options* options);

  // BlockBuilder不可拷贝，不可赋值
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Reset the contents as if the BlockBuilder was just constructed.
  // 重置BlockBuilder，清空buffer_、restarts_、last_key_等成员变量，
  // 就像刚刚开始构造BlockBuilder一样
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  // 添加一个kv对，key和value都是Slice类型
  // 要求：在上一次调用Reset()之后，没有调用Finish()方法
  // 要求：key比之前添加的任何key都要大
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // 完成Block的构建，返回一个Slice，该Slice引用存储kv的buffer_。
  // 返回的Slice在BlockBuilder的生命周期内都是有效的，或者在调用Reset()之前都是有效的。
  // 返回的Slice的格式已经符合data block中data部分的格式：kv+restarts[restart_num]+restart_num
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  // 返回当前BlockBuilder构建的block的大小(未压缩的大小)的估计值, 注意这里的压缩指的是整个block采用的压缩算法，
  // 而不是kv对中的key采用的前缀压缩算法，不要混淆
  // 未压缩的kv对大小+所有重启点的总大小+重启点数目值
  size_t CurrentSizeEstimate() const;

  // Return true iff no entries have been added since the last Reset()
  bool empty() const { return buffer_.empty(); }

 private:
  //  构造时传入的Options，表示一些配置信息
  const Options* options_;
  // 所有的kv对都会被存储在buffer_中，格式与data block中的kv对保存格式一致
  std::string buffer_;
  // 保存所有重启点的偏移量，即restarts_[i]表示第i个重启点在buffer_中的偏移量,
  // 此时存储的kv对不再进行前缀压缩，而是直接存储整个key
  std::vector<uint32_t> restarts_;
  // 在最近的重启点后，添加的kv对的数量，默认是每隔16个kv对添加一个重启点
  int counter_;
  // 表示是否已经调用了Finish()方法，该方法依据当前的数据生成一个block
  bool finished_;
  // 上一个添加的key，当添加新的key时，会与last_key_进行比较，以便计算前缀压缩的长度
  std::string last_key_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
