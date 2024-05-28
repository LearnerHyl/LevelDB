// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// BlockBuilder generates blocks where keys are prefix-compressed:
// BlockBuilder生成的块，其中键是前缀压缩的：
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
// 当我们存储一个键时，我们会丢弃与前一个字符串共享的前缀。这有助于显著减少空间需求。
// 此外，每隔K个键，我们不应用前缀压缩，而是存储整个键。我们称之为“重启点”。
// 块的尾部存储了所有重启点的偏移量，并且可以在查找特定键时使用二分搜索。
// 值按原样存储(不压缩)，紧随相应的键之后。
//
// An entry for a particular key-value pair has the form:
// 特定键值对的条目的格式如下：
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
// 块的尾部的格式如下：
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/options.h"

#include "util/coding.h"

namespace leveldb {

BlockBuilder::BlockBuilder(const Options* options)
    : options_(options), restarts_(), counter_(0), finished_(false) {
  assert(options->block_restart_interval >= 1);
  // 第一个重启点在buffer_中的偏移量为0
  restarts_.push_back(0);
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return (
      buffer_.size() +                       // Raw data buffer
      restarts_.size() * sizeof(uint32_t) +  // Restart array
      sizeof(uint32_t));  // Restart array length，uint32_t类型的重启点数目值
}

Slice BlockBuilder::Finish() {
  // Append restart array
  // 将restarts_中的所有重启点的偏移量用定长32位整数存储到buffer_中
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  // 将重启点数目值存储到buffer_中
  PutFixed32(&buffer_, restarts_.size());
  finished_ = true;
  // 此时buffer_中存储了所有的kv对和重启点信息，且符合data block中data部分的格式
  return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  // 上一个添加的key
  Slice last_key_piece(last_key_);
  // 确保没有调用Finish()方法
  assert(!finished_);
  // 确保最近的重启点拥有的kv对数量小于block_restart_interval
  assert(counter_ <= options_->block_restart_interval);
  // 确保key比之前添加的任何key都要大
  assert(buffer_.empty()  // No values yet?
         || options_->comparator->Compare(key, last_key_piece) > 0);
  //  shared用来保存本次key与上一次key的共享前缀长度
  size_t shared = 0;
  // counter_用来记录最近的重启点拥有的kv对数量,当counter_ < block_restart_interval时，进行前缀压缩
  if (counter_ < options_->block_restart_interval) {
    // See how much sharing to do with previous string
    // 计算key与上一次key的共享前缀长度
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression
    // 当counter_ >= block_restart_interval时, 存储整个key，不进行前缀压缩
    // 记录新的重启点，并reset counter_
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  // 参考block中kv对的存储格式，下面很容易理解
  // non_shared用来保存key中不与上一次key共享的部分的长度
  const size_t non_shared = key.size() - shared;

  // kv对的存储格式：shared_bytes | unshared_bytes | value_length | key_delta | value

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}

}  // namespace leveldb
