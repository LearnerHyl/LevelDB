// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"

#include "leveldb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

// 是TwoLevelIterator的第二层迭代器的构造函数，输入是从第一层index block的迭代器中获取的blockhandle，
// 之后将该Blockhandle作为参数传递给block_function，从而获取到该blockhandle对应的data block的迭代器。
typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function,
                   void* arg, const ReadOptions& options);

  ~TwoLevelIterator() override;

  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  bool Valid() const override { return data_iter_.Valid(); }
  Slice key() const override {
    assert(Valid());
    return data_iter_.key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_.value();
  }
  Status status() const override {
    // It'd be nice if status() returned a const Status& instead of a Status
    // 若index_iter_和data_iter_都没问题，则返回status_，否则返回index_iter_和data_iter_中的错误状态。
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
      return data_iter_.status();
    } else {
      return status_;
    }
  }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  // 向Next方向遍历，直到找到第一个有效的data block
  void SkipEmptyDataBlocksForward();
  // 向Prev方向遍历，直到找到第一个有效的data block
  void SkipEmptyDataBlocksBackward();
  // 将当前的data_iter_设置为给定的data_iter
  // requires: data_iter_ 在设置前必须为空
  void SetDataIterator(Iterator* data_iter);
  // 根据当前index_iter_的值来初始化data_iter_
  void InitDataBlock();

  // 用于创建TwoLevelIterator的第二层迭代器的函数指针,接收来自第一层迭代器查找到的value(即blockhandle)
  // 作为参数，返回一个迭代器，该迭代器可以遍历该blockhandle对应的data block。
  BlockFunction block_function_;
  void* arg_;
  const ReadOptions options_;
  Status status_;
  // TwoLevelIterator中的第一层迭代器，用于获取目标key对应的data block的blockhandle。
  IteratorWrapper index_iter_;
  // TwoLevelIterator中的第二层迭代器，用于遍历目标key对应的data block。
  IteratorWrapper data_iter_;  // May be nullptr
  // If data_iter_ is non-null, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the data_iter_.
  // 如果data_iter_不为空，则"data_block_handle_"保存了传递给block_function_的"index_value"，
  // 可以用来创建data_iter_。该值就是通过index_iter_获取的blockhandle。
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter,
                                   BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() = default;

void TwoLevelIterator::Seek(const Slice& target) {
  // index_iter_更新后，需要调用InitDataBlock()来及时更新data_iter_
  index_iter_.Seek(target);
  InitDataBlock();
  // 若data_iter_不为空，则将data_iter_指向target对应的kv对
  if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
  // 向前遍历，将data_iter_指向第一个不为空的data block，
  // 因为当前查找到的data block可能为空，需要跳过这些空的data block。
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
  // index_iter_更新后，需要调用InitDataBlock()来及时更新data_iter_
  index_iter_.SeekToFirst();
  InitDataBlock();
  // 若data_iter_不为空，则将data_iter_指向target对应的kv对
  if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  // 前进方向遍历，将data_iter_指向第一个不为空的data block，
  // 因为当前查找到的data block可能为空，需要跳过这些空的data block。
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  // index_iter_更新后，需要调用InitDataBlock()来及时更新data_iter_
  index_iter_.SeekToLast();
  InitDataBlock();
  // 若data_iter_不为空，则将data_iter_指向target对应的kv对
  if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  // 后退方向遍历，将data_iter_指向第一个不为空的data block，
  // 因为当前查找到的data block可能为空，需要跳过这些空的data block。
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_.Next();
  // 向前遍历，将data_iter_指向第一个不为空的data block，
  // 因为当前查找到的data block可能为空，需要跳过这些空的data block。
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_.Prev();
  // 后退方向遍历，将data_iter_指向第一个不为空的data block，
  // 因为当前查找到的data block可能为空，需要跳过这些空的data block。
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  // 若data_iter_无效或为nullptr，则继续前进遍历，直到找到第一个有效的data block
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) { // 若index_iter_无效，则将data_iter_置空
      SetDataIterator(nullptr);
      return;
    }
    // 每次Index_iter_改变时，都需要同步更新对应的data_iter_
    // 因此index_iter_的指针改变与InitDataBlock()的调用是绑定的
    index_iter_.Next();
    InitDataBlock();
    // 若data_iter_不为空，则将data_iter_指向当前block的第一个kv对
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  // 若data_iter_无效或为nullptr，则继续后退遍历，直到找到第一个有效的data block
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    // 每次Index_iter_改变时，都需要同步更新对应的data_iter_
    // 因此index_iter_的指针改变与InitDataBlock()的调用是绑定的
    index_iter_.Prev();
    InitDataBlock();
    // 若data_iter_不为空，则将data_iter_指向当前block的最后一个kv对
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
  if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
  data_iter_.Set(data_iter);
}

void TwoLevelIterator::InitDataBlock() {
  // 若index_iter_无效，则将data_iter_置空
  if (!index_iter_.Valid()) {
    SetDataIterator(nullptr);
  } else {
    // 获取当前index_iter_指向的blockhandle
    Slice handle = index_iter_.value();
    if (data_iter_.iter() != nullptr &&
        handle.compare(data_block_handle_) == 0) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
      // data_iter_已经使用这个迭代器构造了，所以不需要改变任何东西
    } else { // 若data_iter_还没有构造，或者handle和data_block_handle_不相等，则重新构造data_iter_
      // 调用block_function_，获取当前handle对应的data block的迭代器
      Iterator* iter = (*block_function_)(arg_, options_, handle);
      // 将handle保存到data_block_handle_中，并将data_iter_设置为iter
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(iter);
    }
  }
}

}  // namespace

// 两级迭代器适用于两种场景：
// 1. 在SSTable中, index block iter->data block iter,最终找到指定的key-value
// 返回一个新的两级迭代器。index_iter是一个index block的迭代器，index block中的每一个kv对中的value
// 都存储了一个data block在sstable文件中的偏移量。之后block_function将这个value转换为一个data block的迭代器。
// 2. 在多层Level中，找到指定的file。
// 返回一个新的两级迭代器。index_iter是给定level的迭代器，首先从index iter中获取目标file的文件编号和文件大小，
// 之后从TableCache中获取该文件的迭代器。具体见LevelFileNumIterator和GetFileIterator。
Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace leveldb
