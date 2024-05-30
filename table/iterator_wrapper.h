// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_
#define STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_

#include "leveldb/iterator.h"
#include "leveldb/slice.h"

namespace leveldb {

// A internal wrapper class with an interface similar to Iterator that
// caches the valid() and key() results for an underlying iterator.
// This can help avoid virtual function calls and also gives better
// cache locality.
// 一个内部包装类，其接口类似于Iterator，但是缓存了底层迭代器的valid()和key()的结果。
// 这可以帮助避免虚函数调用，并且还提供了更好的缓存局部性。
class IteratorWrapper {
 public:
  IteratorWrapper() : iter_(nullptr), valid_(false) {}
  explicit IteratorWrapper(Iterator* iter) : iter_(nullptr) { Set(iter); }
  ~IteratorWrapper() { delete iter_; }
  Iterator* iter() const { return iter_; }

  // Takes ownership of "iter" and will delete it when destroyed, or
  // when Set() is invoked again.
  // 获取"iter"的所有权，当销毁时、或者再次调用Set()时将删除它。
  void Set(Iterator* iter) {
    // 删除原来的迭代器，以接管新的迭代器
    delete iter_;
    iter_ = iter;
    if (iter_ == nullptr) {
      valid_ = false;
    } else { // 更新valid_和key_的值
      Update();
    }
  }

  // Iterator interface methods
  bool Valid() const { return valid_; }
  Slice key() const {
    assert(Valid());
    return key_;
  }
  Slice value() const {
    assert(Valid());
    return iter_->value();
  }
  // Methods below require iter() != nullptr
  Status status() const {
    assert(iter_);
    return iter_->status();
  }
  
  // Next()改变了迭代器的位置，需要调用Update()方法来更新valid_和key_的值。
  void Next() {
    assert(iter_);
    iter_->Next();
    Update();
  }
  void Prev() {
    assert(iter_);
    iter_->Prev();
    Update();
  }
  void Seek(const Slice& k) {
    assert(iter_);
    iter_->Seek(k);
    Update();
  }
  void SeekToFirst() {
    assert(iter_);
    iter_->SeekToFirst();
    Update();
  }
  void SeekToLast() {
    assert(iter_);
    iter_->SeekToLast();
    Update();
  }

 private:
  // 当IteratorWrapper接管了新的迭代器后，需要调用Update()方法来更新valid_和key_的值。
  void Update() {
    valid_ = iter_->Valid();
    if (valid_) {
      key_ = iter_->key();
    }
  }

  // 接管的迭代器
  Iterator* iter_;
  // iter_是否有效
  bool valid_;
  // iter_当前的key
  Slice key_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_
