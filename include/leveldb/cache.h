// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
// Cache是一个将key映射到value的接口。它有内部同步机制，可以安全地被多个线程并发访问。
// 它可以自动地驱逐条目以为新条目腾出空间。Value对缓存容量有指定的费用。
// 例如，一个缓存，其中的值是可变长度的字符串，可以使用字符串的长度作为字符串的费用。
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)
// 提供了一个内置的缓存实现，它使用最近最少使用(LRU)的驱逐策略。如果客户端需要更复杂的实现（如抵抗扫描、自定义驱逐策略、可变大小的缓存等），
// 客户端可以使用自己的实现。

#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

namespace leveldb {

class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
// 创建一个固定大小容量的新缓存。这个Cache的实现使用最近最少使用(LRU)的驱逐策略。
LEVELDB_EXPORT Cache* NewLRUCache(size_t capacity);

// Cache是一个接口类，里面定义了一系列的纯虚函数接口，当具体实现类继承Cache类时，需要实现这些接口。
class LEVELDB_EXPORT Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  // 通过调用传递给构造函数的“deleter”函数销毁所有现有条目。
  virtual ~Cache();

  // Opaque handle to an entry stored in the cache.
  // Cache中存储的条目的不透明句柄。
  struct Handle {};

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  // 将key->value的映射插入到缓存中，其占用的缓存空间为charge。
  //
  // Returns a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  // 返回与映射对应的句柄。当不再需要返回的映射时，调用者必须调用this->Release(handle)。
  //
  // When the inserted entry is no longer needed, the key and
  // value will be passed to "deleter".
  // 当不再需要插入的条目时，key和value将被传递给“deleter”。
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // If the cache has no mapping for "key", returns nullptr.
  // 如果缓存中没有“key”的映射，则返回nullptr。
  //
  // Else return a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  // 否则返回与映射对应的句柄。当不再需要返回的映射时，调用者必须调用this->Release(handle)。
  virtual Handle* Lookup(const Slice& key) = 0;

  // Release a mapping returned by a previous Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 释放先前Lookup()返回的映射。本质上是调用一次Unref()。
  // 要求：句柄handle还没有被释放。
  // 要求：句柄handle必须是由*this上的一个方法返回的。
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 返回由成功的Lookup()返回的句柄中封装的值。
  // 要求：句柄handle还没有被释放。
  // 要求：句柄handle必须是由*this上的一个方法返回的。
  virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it.  Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.
  // 如果缓存包含key的条目，则擦除它。请注意，直到所有现存的句柄都被释放为止，在此之前，底层条目将被保留。
  virtual void Erase(const Slice& key) = 0;

  // Return a new numeric id.  May be used by multiple clients who are
  // sharing the same cache to partition the key space.  Typically the
  // client will allocate a new id at startup and prepend the id to
  // its cache keys.
  // 返回一个新的数字id。可以被多个共享相同缓存的客户端使用，以分割key空间。
  // 通常，客户端将在启动时分配一个新的id，并将id前置到其缓存key中。
  virtual uint64_t NewId() = 0;

  // Remove all cache entries that are not actively in use.  Memory-constrained
  // applications may wish to call this method to reduce memory usage.
  // Default implementation of Prune() does nothing.  Subclasses are strongly
  // encouraged to override the default implementation.  A future release of
  // leveldb may change Prune() to a pure abstract method.
  // 移除所有未被活动使用的缓存条目。内存受限的应用程序可能希望调用此方法以减少内存使用。
  // Prune()的默认实现什么也不做。强烈建议子类重写默认实现。未来的leveldb版本可能会将Prune()更改为纯抽象方法。
  virtual void Prune() {}

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  // 返回存储在缓存中的所有元素的组合费用的估计值。
  virtual size_t TotalCharge() const = 0;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
