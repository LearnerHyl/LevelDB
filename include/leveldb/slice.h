// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Slice is a simple structure containing a pointer into some external
// storage and a size.  The user of a Slice must ensure that the slice
// is not used after the corresponding external storage has been
// deallocated.
//
// Multiple threads can invoke const methods on a Slice without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Slice must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_SLICE_H_
#define STORAGE_LEVELDB_INCLUDE_SLICE_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

#include "leveldb/export.h"

namespace leveldb {

// Slice与c++的string类似，那么为什么不直接使用string呢？Slice有什么优势？
// 1. Slice是一个简单的结构，只包含一个指向外部存储的指针和一个大小。Slice的用户必须确保在相应的外部存储被释放后，Slice不再被使用。
// 2. 多个线程可以在没有外部同步的情况下调用Slice的const方法，但是如果任何一个线程可能调用非const方法，那么所有访问同一个Slice的线程都必须使用外部同步。
// 3. Slice的实现是线程安全的，因为它只包含一个指针和一个大小，没有状态。
// 4. Slice不以'\0'结尾，因此可以包含任意的二进制数据。
class LEVELDB_EXPORT Slice {
 public:
  // Create an empty slice.
  // 创建一个空的slice
  Slice() : data_(""), size_(0) {}

  // Create a slice that refers to d[0,n-1].
  // 创建一个slice，指向d[0,n-1]
  Slice(const char* d, size_t n) : data_(d), size_(n) {}

  // Create a slice that refers to the contents of "s"
  // 创建一个slice，指向s的内容，即将string转换为slice
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

  // Create a slice that refers to s[0,strlen(s)-1]
  // 创建一个slice，指向s[0,strlen(s)-1]
  Slice(const char* s) : data_(s), size_(strlen(s)) {}

  // Intentionally copyable.
  // overloading the copy constructor and copy assignment operator
  Slice(const Slice&) = default;
  Slice& operator=(const Slice&) = default;

  // Return a pointer to the beginning of the referenced data
  // 返回引用数据的指针
  const char* data() const { return data_; }

  // Return the length (in bytes) of the referenced data
  // 返回引用数据的长度
  size_t size() const { return size_; }

  // Return true iff the length of the referenced data is zero
  // 如果引用数据的长度为0，则返回true
  bool empty() const { return size_ == 0; }

  // Return the ith byte in the referenced data.
  // 返回第i个字节
  // REQUIRES: n < size()
  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }

  // Change this slice to refer to an empty array
  // 把data_设置为空字符串，size_设置为0
  void clear() {
    data_ = "";
    size_ = 0;
  }

  // Drop the first "n" bytes from this slice.
  // 删除slice的前n个字节
  void remove_prefix(size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }

  // Return a string that contains the copy of the referenced data.
  // 返回一个包含引用数据副本的字符串
  std::string ToString() const { return std::string(data_, size_); }

  // Three-way comparison.  Returns value:
  //   <  0 iff "*this" <  "b",
  //   == 0 iff "*this" == "b",
  //   >  0 iff "*this" >  "b"
  int compare(const Slice& b) const;

  // Return true iff "x" is a prefix of "*this"
  // 如果x是*this的前缀，则返回true
  bool starts_with(const Slice& x) const {
    return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) {
  return ((x.size() == y.size()) &&
          (memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }

inline int Slice::compare(const Slice& b) const {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size_)
      r = -1;
    else if (size_ > b.size_)
      r = +1;
  }
  return r;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_SLICE_H_
