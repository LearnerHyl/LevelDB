// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace leveldb {

class Arena {
 public:
  Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  // 返回一个指向新分配的内存块的首地址的指针，大小为bytes字节
  char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc.
  // 用malloc提供的正常对齐保证分配内存。这种方法保证了内存首地址满足对齐要求。
  // 目前的主流服务器架构中大多为64位，所以align默认为8(64/8)，即分配的内存首地址必须是8的倍数。
  char* AllocateAligned(size_t bytes);

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  // 返回由arena分配的数据的总内存使用量的估计值
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  char* alloc_ptr_; // 总是指向当前最新block中的第一个可用的字节
  size_t alloc_bytes_remaining_; // 当前block中剩余的可用字节数，与alloc_ptr_配合使用

  // Array of new[] allocated memory blocks
  std::vector<char*> blocks_; // 保存所有分配的内存块的起始地址，每个block默认大小为4KB(kBlockSize)

  // Total memory usage of the arena.
  // Arena已经申请的内存总量大小
  //
  // TODO(costan): This member is accessed via atomics, but the others are
  //               accessed without any locking. Is this OK?
  std::atomic<size_t> memory_usage_;
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  // 如果我们允许0字节的分配，那么返回的语义会有点混乱，所以我们在这里禁止它们(我们不需要它们来进行内部使用)。
  assert(bytes > 0);
  // 如果bytes小于等于当前block中剩余的可用字节数，那么直接使用当前block中的空间，更新alloc_ptr_和alloc_bytes_remaining_即可
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  // 如果bytes大于当前block中剩余的可用字节数，那么调用AllocateFallback()方法分配新的block
  return AllocateFallback(bytes);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
