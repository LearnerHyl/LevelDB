// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
// Cache条目有一个“in_cache”布尔值，指示缓存是否对条目有引用。这个值在不调用条目的“deleter”函数的情况下变为false的唯一方式是通过Erase()、
// 通过Insert()插入具有重复key的元素，或者在缓存销毁时。
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
//   Elements are moved between these lists by the Ref() and Unref() methods,
//   when they detect an element in the cache acquiring or losing its only
//   external reference.
// cache维护了两个链表，其中包含缓存中的条目。所有缓存中的条目都在且只能在其中一个链表中，绝对不会同时出现在两个链表中。
// 仍然被客户端引用但从缓存中删除的条目既不在一个链表中也不在另一个链表中。这些链表是：
// - in-use:
// 包含当前被客户端引用的条目，没有特定的顺序。(这个链表用于不变量检查。如果我们删除了这个检查，
// 那么本来应该在这个链表中的元素可能会被留在孤立的单例链表中)。该链表中的LRUHandle的refs>=2，即被客户端引用。
// - LRU:
// 包含当前未被客户端引用的条目，按照LRU顺序排列。元素在这些链表之间移动是通过Ref()和Unref()方法完成的，
// 当它们检测到缓存中的元素获取或失去唯一外部引用时。获取唯一外部引用的元素会从LRU链表中移动到in-use链表中，
// 失去唯一外部引用的元素会从in-use链表中移动到LRU链表中。该队列中的LRUHandle的refs恒为1，即只有缓存引用，没有客户端引用。

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
// 一个Entry是一个可变长度的堆分配结构。Entry被保存在一个按访问时间排序的循环双向链表中。
// 一个LRUHandle对象可以认为是一个LRUNode，因为其内部封装了太多功能，所以这里称之为LRUHandle。
struct LRUHandle {
  // 该LRUHandle对象中保存的value值的指针，存储的是value对象的地址，而不是对象本身。
  // 在TableCache中，value存储的是TableAndFile对象的地址。在BlockCache中，value存储的是Block对象的地址。
  // 在BlockCache中，value存储的是Block对象的地址。
  void* value;
  // 该节点的销毁函数
  void (*deleter)(const Slice&, void* value);
  // 若产生Hash冲突，则用一个单向链表串联起冲突的节点，用于指向同一个bucket中的下一个节点
  // 用于在HandleTable中查找节点
  LRUHandle* next_hash;
  // 该节点在LRU链表中的前一个节点，用于在LRU链表中查找
  LRUHandle* next;
  // 该节点在LRU链表中的后一个节点，用于在LRU链表中查找
  LRUHandle* prev;
  // 当前节点占用的容量，在LevelDB中，每个节点容量为1
  size_t charge;
  // 该节点的key的长度
  size_t key_length;
  // 该节点是否在缓存中，若不在，可以调用deleter函数销毁该节点
  bool in_cache;
  // 该节点的引用计数
  uint32_t refs;
  // 该节点的hash值，该值缓存在此处可以加快查找速度，避免每次查找都要重新计算hash值。
  // 在ShardedLRUCache中使用，用于确定该LRUHandle节点应该在哪个LRUCache中。
  uint32_t hash;
  // 该节点的占位符，键的实际长度保存在key_length中，可以认为是key的首地址
  char key_data[1];

  Slice key() const {
    // next is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    // next只有在LRUHandle是空列表的列表头时才等于this。列表头从不具有有意义的键。
    // List head的作用是哨兵，不存储数据，只是为了方便操作
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
// HandleTable是LevelDB自行实现的一个简单哈希表。因为它消除了一大堆的移植问题，
// 并且在我们测试的一些编译器/运行时组合中，它也比一些内置的哈希表实现更快。
// 例如，与g++ 4.4.3的内置哈希表相比，readrandom的速度提高了约5%。
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  // 在哈希表中查找key值为key，hash值为hash的LRUHandle元素
  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  // 将h插入到哈希表中，若之前存在相同的key值的LRUHandle元素，则替换之，
  // 返回之前存在的LRUHandle元素；若之前不存在相同的key值的LRUHandle元素，则返回nullptr
  LRUHandle* Insert(LRUHandle* h) {
    // 先在hashtable中确定之前是否已经存在相同的key值的LRUHandle元素
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    // 将新节点的next_hash指向旧节点的next_hash
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    // 将hashtable中的旧节点替换为新节点
    *ptr = h;
    if (old == nullptr) {  // 若之前不存在相同的key值的LRUHandle元素
      // 增加元素个数
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        // 由于每个缓存条目都相当大，我们的目标是一个小的平均链表长度(<= 1)。
        Resize();
      }
    }
    // 返回之前存在的LRUHandle元素。若之前不存在相同的key值的LRUHandle元素，则返回nullptr
    return old;
  }

  // 从哈希表中移除key值为key，hash值为hash的LRUHandle元素，
  // 注意，该函数只是从哈希表中移除该元素，但不会释放该元素的内存
  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    // ptr是指向目标LRUHandle的指针的指针
    // 可以理解为ptr中存储的是指向目标LRUHandle的指针的地址
    LRUHandle** ptr = FindPointer(key, hash);
    // result存储指向目标LRUHandle的指针
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      // 将ptr中的地址改为指向目标LRUHandle的next_hash的指针，因为
      // 目标LRUHandle已经被移除，要让该bucket中的链表连续
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  // 哈希表由一个buckets数组组成，其中每个bucket是一个缓存被hash到该bucket中的LRUHandle元素的链表。
  // length_是buckets数组的长度,代表buckets数组中的bucket的个数。
  uint32_t length_;
  // elems_是哈希表中的元素个数
  uint32_t elems_;
  // 指针数组，数组的每个元素指向一个bucket的头节点
  LRUHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // 如果某个bucket中存在相同的key和hash值的LRUHandle元素，则返回指向该元素的指针。
  // 如果该bucket没有目标key元素，则返回指向该bucket链表的尾节点的指针。目的是
  // 可以直接对该bucket中的元素进行操作。
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    // 这里的&运算相当于取模运算，hash值与length_取模，得到的值就是该hash值对应的bucket的索引
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    // 遍历该bucket中的链表，直到找到目标元素或者找到链表的尾节点
    // 当指针不为空且哈希值或键不匹配时，继续遍历链表
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    // 返回指向目标元素或者链表尾节点的指针
    return ptr;
  }

  // 调整哈希表的大小
  void Resize() {
    // 初始的新长度为4
    uint32_t new_length = 4;
    // 当新长度小于元素个数时，新长度翻倍，直到新长度大于等于元素个数
    while (new_length < elems_) {
      new_length *= 2;
    }
    // 分配一个新的哈希表数组，有new_length个buckets
    LRUHandle** new_list = new LRUHandle*[new_length];
    // 初始化新的哈希表数组中所有的bucket为空
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    // 计数器，用于验证调整大小后元素数量是否一致
    uint32_t count = 0;
    // 遍历旧哈希表的每个桶
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      // 遍历每个桶中的节点
      while (h != nullptr) {
        // 保存下一个节点的指针
        LRUHandle* next = h->next_hash;
        // 获取当前节点的hash值
        uint32_t hash = h->hash;
        // 计算当前节点在新哈希表中的索引，即hash值与new_length取模
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        // 下面采用的是头插法，将当前节点插入到新哈希表中对应bucket的头节点
        // 将当前节点的next_hash指向新哈希表中的对应bucket的头节点
        h->next_hash = *ptr;
        // 将当前节点作为新哈希表中对应bucket的头节点
        *ptr = h;
        // 继续遍历下一个节点
        h = next;
        count++;
      }
    }
    // 确保新表中的元素数量与调整前的元素数量一致
    assert(elems_ == count);
    // 释放旧哈希表的内存
    delete[] list_;
    // 更新哈希表的长度
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
// ShardLRUCache中包含了若干个LRUCache，每个LRUCache称之为一个shard。
// LevelDB实现的LRUCache中，dummy->head->prev代表当前最新的节点，dummy->head->next代表当前最旧的节点。
// 如果需要淘汰节点，则从dummy->head->next开始淘汰。
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  // 与构造函数分开，以便调用者可以轻松地创建LRUCache数组
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  // 类似Cache方法，但有一个额外的“hash”参数。为了统一性，将LRUHandle转换为Cache::Handle
  // hash值用于确定key值应在的bucket中
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  // hash值用于确定key值应在的bucket中,之后遍历该bucket中的链表，直到找到与key值相同的节点或到达链表尾部
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  // 当调用者不再需要使用handle时，调用该函数释放handle的使用权，
  // 本质上是将handle的引用计数减1，当引用计数为0时，释放handle
  void Release(Cache::Handle* handle);
  // 当调用者不再需要使用key对应的value时，调用该函数将key对应的value从缓存中删除
  void Erase(const Slice& key, uint32_t hash);
  // 移除所有未被客户端引用的节点，即在lru_双向链表中的节点
  void Prune();
  // 返回缓存中所有元素的总容量
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  // 将节点e从双向链表中移除
  void LRU_Remove(LRUHandle* e);
  // 将节点e追加到双向链表list的头部
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  // 增加对节点e的引用计数
  void Ref(LRUHandle* e);
  // 减少对节点e的引用计数
  void Unref(LRUHandle* e);
  // 从缓存中删除节点e
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Initialized before use.
  // 在使用LRUCache之前初始化容量值
  size_t capacity_;

  // mutex_ protects the following state.
  // mutex_保护以下状态在多线程的场景下的一致性
  mutable port::Mutex mutex_;
  size_t usage_ GUARDED_BY(mutex_);

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  // LRU双向链表的哨兵节点。这是LRUCache维护的第一个双向链表，用于存储当前未被客户端引用的节点。
  // lru.prev指向最新的节点，lru.next指向最旧的节点。
  // 此链表中所有条目都满足refs==1和in_cache==true。表示所有条目只被缓存引用一次，
  // 而没有被客户端引用，即没有被客户端调用Lookup()方法。
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  // in_use双向链表的哨兵节点。这是LRUCache维护的第二个双向链表，用于存储当前被客户端引用的节点。
  // 保存所有仍然被客户端引用的节点，由于这些条目被客户端引用的同时也被缓存引用，所以refs>=2和in_cache==true。
  LRUHandle in_use_ GUARDED_BY(mutex_);

  // 所有条目(包括in_use_双向链表和lru_双向链表)的哈希表，用于实现O(1)时间复杂度的查找、删除、插入
  HandleTable table_ GUARDED_BY(mutex_);
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  // 初始化lru_和in_use_双向链表，这两个链表都是循环链表
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
  // 在调用析构函数时，不允许in_use_链表中还有节点，即不允许还有节点被客户端引用
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  // 释放所有lru_链表中的节点
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    // 保存下一个待释放节点的指针
    LRUHandle* next = e->next;
    // 该节点当前必须还在cache中,即在lru_双向链表中
    assert(e->in_cache);
    // 释放该节点
    e->in_cache = false;
    // 很明显在lru_双向链表中的节点的引用计数必须为1
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

void LRUCache::Ref(LRUHandle* e) {
  // 如果节点的引用计数为1且在缓存中，则将节点从lru_链表移动到in_use_链表
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  // 如果节点的引用计数为0，则释放该节点
  // 说明该节点在lru_双向链表中，且我们已经想要释放该节点或者调用了析构函数
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache &&
             e->refs == 1) {  // 说明没有客户端引用该节点，只有缓存引用该节点
    // No longer in use; move to lru_ list.
    // 从in_use_链表中移动到lru_链表
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  // 将e插入到list的前面，从而使e成为最新的条目
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {  // 找到有效的节点，增加引用计数
    Ref(e);
  }
  // 将LRUHandle转换为Cache::Handle，reinterpret_cast是强制类型转换，本质上是
  // 把待转化对象中的内容逐个bit拷贝到目标对象中
  // 如果是BlockCache，则value存储的是Block对象的地址
  // 如果是TableCache，则value存储的是TableAndFile对象的地址
  return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 将key和value封装成一个LRUHandle对象

  // 为LRUHandle对象分配内存空间，这里的-1代表key_data的首地址，因为LRUHandle中存储的实际字段就是key的首地址，
  // key.size()代表key_data的长度。因此这里-1再加上key.size()代表key实际所需的内存空间
  LRUHandle* e =
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  // 初始化LRUHandle对象的各个字段
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  // Insert操作在返回时，相当于在使用这个handle，所以引用计数初始化为1
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) { // 若当前的LRUCache的容量大于0，则缓存
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    // refs >= 2，因此将该节点插入到in_use_链表中
    LRU_Append(&in_use_, e);
    usage_ += charge;
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // 若当前的LRUCache的容量等于0，则不缓存。这意味着关闭缓存功能
    // next is read by key() in an assert, so it must be initialized
    // next被key()函数在assert中读取，因此必须初始化
    e->next = nullptr;
  }
  // 若当前使用的容量超过了预设的容量，且lru_双向链表中还有节点，则从lru_双向链表中移除节点，直到使用容量小于等于预设容量
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
// 若目标节点e不为空，则完成从LRUCache中移除e的操作，并在随后释放e的内存；之前e已经从哈希表中移除。返回e是否不为空。
// 本函数的调用者有两种情况：
// 1. 若Erase()调用本函数，意味着已经将e从hashtable中移除，接下来就是将e从LRUCache中移除，最后释放e
// 2. 若Insert()调用本函数，意味着已经将e插入到hashtable中，table_.insert()会可能会返回一个旧的LRUHandle节点，
// 这时就需要将旧的节点从LRUCache中移除，最后释放旧的节点。
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }
  return e != nullptr;
}

void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  // table_.Remove()函数返回的是被从HashTable中移除的节点
  // 下一步就是将该节点从LRUCache中移除，最后释放该节点
  // 一般来说这个节点在lru_双向链表中，因为只有这样执行Unref操作后，节点才会被释放(此时reference count为0)
  FinishErase(table_.Remove(key, hash));
}

void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    // lru_双向链表中的节点的引用计数必须为1
    assert(e->refs == 1);
    // 首先将节点从hashtable中移除，之后调用FinishErase()函数将节点从LRUCache中移除，最后释放该节点
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}


/**
 * 引入ShardedLRUCache的目的在于减小锁的粒度，提高并发性能。策略是将一个LRUCache分成多个shard-
 * 利用key的hash值的前kNumShardBits位来确定key应该在哪个shard中(我们称之为分片路由)，从而减小锁的粒度。
 * 因此，ShardedLRUCache中包含了kNumShards个LRUCache，每个LRUCache称之为一个shard。
 * 
 * ShardedLRUCache本质上是一个封装了多个LRUCache的Cache实现，负责将操作路由到对应的LRUCache中。
*/

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

class ShardedLRUCache : public Cache {
 private:
  LRUCache shard_[kNumShards]; // 存储多个LRUCache实例，每个LRUCache称之为一个shard
  port::Mutex id_mutex_; // 用于保护 last_id_ 的访问，确保生成新的 ID 是线程安全的。
  uint64_t last_id_; // 记录上一次生成的 ID

  // s是输入的目标key，返回key的hash值
  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  // 根据HashSlice()函数计算出的hash值作为输入，计算出该hash值应该在哪个shard中。
  // 通过hash值的前kNumShardBits位来确定key应该在哪个shard中。
  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    // 直接使用 capacity / kNumShards 会导致在整数除法中舍去小数部分，从而可能导致总容量被低估。
    // 例如，如果 capacity 为 10 而 kNumShards 为 3，直接除法会得到每个分片的容量为 3，总容量为 3 * 3 = 9，少了 1。
    // 通过加上 kNumShards - 1，确保在舍入时能正确处理余数部分，最大限度地均衡分配容量。例如，对于上面的情况，计算 (10 + 2) / 3 = 12 / 3 = 4，
    // 这样每个分片的容量为 4，总容量为 4 * 3 = 12，稍微超出，但保证了没有分片容量不足的问题。
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    // 对key的hash值进行分片路由，将key插入到对应的shard中
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    // 对key的hash值进行分片路由，确定该key所属的shard，然后在该shard中查找key
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    // 由该LRUHandle的hash值确定该LRUHandle应该在哪个shard中，然后在该shard中释放该LRUHandle
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    // 对key的hash值进行分片路由，确定该key所属的shard，然后在该shard中查找key
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    // 保证生成新的 ID 是线程安全的
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  void Prune() override {
    // 清除所有shard中未被客户端引用的节点，即所有shard中的lru_双向链表中的节点
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    // 计算所有shard中所有节点的容量的总和
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
