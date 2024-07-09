// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"

#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
// 每2KB数据生成一个新的filter
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // 默认每2KB数据生成一个新的filter
  // 计算当前data block中的数据当前需要的filter的index
  uint64_t filter_index = (block_offset / kFilterBase);
  // 若当前data
  // block中的数据需要的filter数量大于当前已经生成的filter数量，则生成新的filter
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
  // 若在调用Finish()之前，已经添加了key，则在result_中生成新的filter data
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  // array_offset记录了filter_offsets_数组在result_中的起始位置
  // array_offset同时也是所有filter data的总字节长度
  // 这一点从filter block的布局中可以得出
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }
  // 完成上面的操作后，result_中的内容如下：
  // |filter data 1|filter data 2|...|filter data n|filter data 1 offset|
  // filter data 2 offset|...|filter data n offset|array_offset|kFilterBaseLg|

  // 如上所述，这里是分别将array_offset和kFilterBaseLg写入到result_中
  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  // 此时result_本身就是一个filter block，以slice对象的形式返回
  // block，返回一个Slice对象，指向result_的起始位置
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  // 在本次调用GenerateFilter()之前，已经添加的key的数量
  const size_t num_keys = start_.size();
  // 若自上次调用GenerateFilter()以来没有添加任何key，则直接返回
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    // 若没有添加任何key，则直接在filter_offsets_记录新的filter data的起始位置
    // 若没有添加任何key，则直接生成一个空的filter data
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  // start_的第i个元素表示第i个key在keys_中的起始位置，start_的最后一个元素表示keys_的长度
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  // 将keys_中的所有key提取出来，保存到tmp_keys_中,
  // 不包括上一步添加的表示keys_长度的元素
  for (size_t i = 0; i < num_keys; i++) {
    // 获取第i个key的起始位置
    const char* base = keys_.data() + start_[i];
    // 第i个key的长度为第i+1个key的起始位置减去第i个key的起始位置
    size_t length = start_[i + 1] - start_[i];
    // 将第i个key保存到tmp_keys_中
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  // 为当前的keys_集合生成filter data，并将filter data追加到result_中。
  // 首先将filter_offsets数组的起始位置偏移量放到filter_offsets_中，
  // 不难理解filter_offsets_'s offset的值等于所有filter data的总大小，这由filter block的布局可以得出
  filter_offsets_.push_back(result_.size());
  // 调用FilterPolicy的CreateFilter()方法为当前的keys_集合生成filter data，
  // 并将filter data保存到result_中。
  // 注意每个filter data的尾部都保存了该过滤器使用了多少个hash函数，是size_t类型的
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  // 清空tmp_keys_和keys_、start_，为下一次GenerateFilter()做准备
  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  // 若contents的长度小于5，则直接返回
  // 一个filter block至少包含一个字节的base_lg_和4个字节的filter offset's start
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  // 获取base_lg_
  base_lg_ = contents[n - 1];
  // 获取最后4个字节的内容，即filter offset's start，同时也是filter
  // data的结束位置 注意：这里的filter offset's start是相对于filter
  // block的起始位置的偏移量
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  // 若filter
  // data的结束位置大于n-5，意味着有问题，因为最后5个字节的内容应该是filter
  // offset's start和base_lg_
  if (last_word > n - 5) return;
  // 获取filter data的起始位置，同时也是filter block的起始位置
  data_ = contents.data();
  // 获取filter offset's start的起始位置
  offset_ = data_ + last_word;
  // 获取filter data的数量，一个offset占4个字节
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  // 根据block_offset计算出当前key所在的filter的index
  // 位运算要比除法运算快
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // start是第index个filter data的起始位置
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    // limit是第index+1个filter data的起始位置，同时也是第index个filter
    // data的结束位置
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    // 若start <= limit <= offset_，则说明第index个filter data存在
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      // 获取第index个filter data的内容，将其转化为Slice对象
      Slice filter = Slice(data_ + start, limit - start);
      // 调用FilterPolicy的KeyMayMatch()方法判断key是否在filter中，
      // 判断的过程中使用的hash函数和处理流程与CreateFilter()方法一致
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {  // 从这里来看，GernalFilterPolicy的CreateFilter()方法生成的filter
                                  // data可能为空
      // Empty filters do not match any keys
      // 若出现第index个filter data的起始位置等于结束位置，则说明第index个filter
      // data为空
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
