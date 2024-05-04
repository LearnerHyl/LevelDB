// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

static const int kBlockSize = 4096;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() { // 析构函数，一次性释放所有分配的内存块
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}

// 当Allocate发现当前block中剩余的空间不足以分配bytes字节时，调用AllocateFallback
// 由于new的方法本身就是内存对齐的，所以AllocateFallback总是返回对齐的内存
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    // 对象大小超过block大小的四分之一，单独分配以避免浪费太多空间
    // 这里我们直接分配一个bytes大小的内存块，从而避免剩余内存空间的浪费
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  // 当bytes小于等于block大小的四分之一时，直接分配一个kBlockSize大小的内存块
  // 之后再新的block中分配bytes大小的内存，并更新alloc_ptr_和alloc_bytes_remaining_
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) { // 一般align为8
  // 用于确定对齐方式。如果指针的大小大于 8 字节(64 位系统上通常是这样)，则对齐方式为指针的大小；
  // 否则，对齐方式为 8 字节。这个对齐方式用于确保返回的内存地址满足指针大小的对齐要求。
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  // 保证align是2的幂
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  // 计算当前内存块空闲的起始地址的 alloc_ptr_ 的偏移量，即当前指针相对于对齐边界的偏移量
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  // 计算需要填充的字节数，以确保下一个内存块对齐到指定的边界。如果当前偏移量为 0，则不需要填充；否则，填充的字节数为对齐方式减去当前偏移量。
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  // 计算需要分配的总字节数，包括请求的字节数和填充的字节数。
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop; // 这块分配的内存的首地址是alloc_ptr_ + slop
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returned aligned memory
    // AllocateFallback总是返回对齐的内存
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result); // 将新分配的block的首地址保存到blocks_中
  // 更新memory_usage_，增加block_bytes和一个指针的大小
  // 一个指针的大小是因为blocks_是一个保存char*的vector，每次增加一个新块时，除了块本身的大小外，还需要保存一个指向块的指针
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace leveldb
