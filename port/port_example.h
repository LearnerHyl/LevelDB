// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// This file contains the specification, but not the implementations,
// of the types/operations/etc. that should be defined by a platform
// specific port_<platform>.h file.  Use this file as a reference for
// how to port this package to a new platform.

#ifndef STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_
#define STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_

#include "port/thread_annotations.h"

namespace leveldb {
namespace port {

// TODO(jorlow): Many of these belong more in the environment class rather than
//               here. We should try moving them and see if it affects perf.

// ------------------ Threading -------------------

// A Mutex represents an exclusive lock.
class LOCKABLE Mutex {
 public:
  Mutex();
  ~Mutex();

  // Lock the mutex.  Waits until other lockers have exited.
  // Will deadlock if the mutex is already locked by this thread.
  void Lock() EXCLUSIVE_LOCK_FUNCTION();

  // Unlock the mutex.
  // REQUIRES: This mutex was locked by this thread.
  void Unlock() UNLOCK_FUNCTION();

  // Optionally crash if this thread does not hold this mutex.
  // The implementation must be fast, especially if NDEBUG is
  // defined.  The implementation is allowed to skip all checks.
  void AssertHeld() ASSERT_EXCLUSIVE_LOCK();
};

class CondVar {
 public:
  explicit CondVar(Mutex* mu);
  ~CondVar();

  // Atomically release *mu and block on this condition variable until
  // either a call to SignalAll(), or a call to Signal() that picks
  // this thread to wakeup.
  // REQUIRES: this thread holds *mu
  void Wait();

  // If there are some threads waiting, wake up at least one of them.
  void Signal();

  // Wake up all waiting threads.
  void SignalAll();
};

// ------------------ Compression -------------------

// Store the snappy compression of "input[0,input_length-1]" in *output.
// Returns false if snappy is not supported by this port.
// 存储input[0,input_length-1]的snappy压缩结果到*output中，如果snappy不被支持则返回false。
bool Snappy_Compress(const char* input, size_t input_length,
                     std::string* output);

// If input[0,input_length-1] looks like a valid snappy compressed
// buffer, store the size of the uncompressed data in *result and
// return true.  Else return false.
// 如果input[0,input_length-1]看起来像一个有效的snappy压缩缓冲区，则将解压缩数据的大小存储在*result中并返回true，否则返回false。
bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                  size_t* result);

// Attempt to snappy uncompress input[0,input_length-1] into *output.
// Returns true if successful, false if the input is invalid snappy
// compressed data.
//
// REQUIRES: at least the first "n" bytes of output[] must be writable
// where "n" is the result of a successful call to
// Snappy_GetUncompressedLength.
// 尝试将input[0,input_length-1]解压缩到*output中，如果成功则返回true，如果输入是无效的snappy压缩数据则返回false。
// 要求output[]的前“n”个字节必须是可写的，其中“n”是对Snappy_GetUncompressedLength的成功调用的结果。
bool Snappy_Uncompress(const char* input_data, size_t input_length,
                       char* output);

// Store the zstd compression of "input[0,input_length-1]" in *output.
// Returns false if zstd is not supported by this port.
bool Zstd_Compress(int level, const char* input, size_t input_length,
                   std::string* output);

// If input[0,input_length-1] looks like a valid zstd compressed
// buffer, store the size of the uncompressed data in *result and
// return true.  Else return false.
bool Zstd_GetUncompressedLength(const char* input, size_t length,
                                size_t* result);

// Attempt to zstd uncompress input[0,input_length-1] into *output.
// Returns true if successful, false if the input is invalid zstd
// compressed data.
//
// REQUIRES: at least the first "n" bytes of output[] must be writable
// where "n" is the result of a successful call to
// Zstd_GetUncompressedLength.
bool Zstd_Uncompress(const char* input_data, size_t input_length, char* output);

// ------------------ Miscellaneous -------------------

// If heap profiling is not supported, returns false.
// Else repeatedly calls (*func)(arg, data, n) and then returns true.
// The concatenation of all "data[0,n-1]" fragments is the heap profile.
bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg);

// Extend the CRC to include the first n bytes of buf.
//
// Returns zero if the CRC cannot be extended using acceleration, else returns
// the newly extended CRC value (which may also be zero).
uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size);

}  // namespace port
}  // namespace leveldb

/* A deleted class: AtomicPointer
AtomicPtr提供了两种读写方式：有屏障和无屏障。这在多核CPU中是非常重要的，因为在多核CPU中，不同核之间的数据是不共享的，所以需要通过屏障来保证数据的一致性。
1. 若采用有屏障的方式，比如说我们调用了Acquire_Load()-读操作，那么在调用Acquire_Load()之前的所有读写操作都会在调用Acquire_Load()之前执行，
    而在调用Acquire_Load()之后的所有读写操作都会在调用Acquire_Load()之后执行。
2. 若采用有屏障的方式，比如说我们调用了Release_Store()-写操作，那么在调用Release_Store()之前的所有读写操作都会在调用Release_Store()之前执行，
    而在调用Release_Store()之后的所有读写操作都会在调用Release_Store()之后执行。
class AtomicPointer {
 private:
  void* rep_; // 用于存储数据地址
public:
  AtomicPointer() : rep_(NULL) { }
  explicit AtomicPointer(void* v); 
  void* Acquire_Load() const; // 有屏障的读操作
  void Release_Store(void* v); // 有屏障的写操作
  void* NoBarrier_Load() const; // 无屏障的读操作
  void NoBarrier_Store(void* v); // 无屏障的写操作
};

void* AtomicPointer::Acquire_Load() const {
  void* result = rep_;
  MemoryBarrier(); // 内存屏障
  return result;
}

void AtomicPointer::Release_Store(void* v) {
  MemoryBarrier(); // 内存屏障
  rep_ = v;
}

void* AtomicPointer::NoBarrier_Load() const {
  return rep_;
}

void AtomicPointer::NoBarrier_Store(void* v) {
  rep_ = v;
}

*/

#endif  // STORAGE_LEVELDB_PORT_PORT_EXAMPLE_H_
