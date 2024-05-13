// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SKIPLIST_H_
#define STORAGE_LEVELDB_DB_SKIPLIST_H_

// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the SkipList will not be destroyed
// while the read is in progress.  Apart from that, reads progress
// without any internal locking or synchronization.
// 写操作需要外部同步，大多数情况下是一个互斥锁。读操作需要保证在读取过程中SkipList不会被销毁。除此之外，读操作不需要任何内部锁或同步。
//
// Invariants:
// LevelDB的SkipList实现有两个不变量：
//
// (1) Allocated nodes are never deleted until the SkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
// 分配的节点在SkipList被销毁之前永远不会被删除。这个不变量由代码轻松保证，因为我们从不删除任何SkipList节点。
// 因为LevelDB把Delete函数的逻辑用Add操作替代，所以SkipList的节点永远不会被删除。
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the SkipList.
// Only Insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
// 一个节点的内容在节点被链接到SkipList之后是不可变的，除了next/prev指针。只有Insert()修改了列表，
// 它会小心地初始化一个节点，并使用release-stores来发布一个或多个列表中的节点。
//
// ... prev vs. next pointer ordering ...

#include <atomic>
#include <cassert>
#include <cstdlib>

#include "util/arena.h"
#include "util/random.h"

namespace leveldb {
/**
 * SkipList对象代表一个独立的跳表，它包含了SkipList的所有操作。
 * 本质上SkipList对象本身并不存储数据，它只是一个管理SkipList的对象，即它包含了SkipList的头节点head_和SkipList的最大高度max_height_。
 * 头节点本身不存储数据，只是一个哨兵节点，它的key是0，next指针指向SkipList的第一个节点。
*/
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;  // 代表SkipList中的一个节点

 public:
  // Create a new SkipList object that will use "cmp" for comparing keys,
  // and will allocate memory using "*arena".  Objects allocated in the arena
  // must remain allocated for the lifetime of the skiplist object.
  // 创建一个新的SkipList对象，它将使用“cmp”来比较键，并将使用“*arena”分配内存。
  // 在arena中分配的对象必须保持分配状态，直到skiplist对象的生命周期结束。
  explicit SkipList(Comparator cmp, Arena* arena);

  // 不允许拷贝和赋值构造函数
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Insert key into the list.
  // REQUIRES: nothing that compares equal to key is currently in the list.
  // 插入key到列表中。
  // 要求: 当前列表中没有与key相等的内容。
  void Insert(const Key& key);

  // Returns true iff an entry that compares equal to key is in the list.
  // 如果列表中有一个与key相等的条目，则返回true。
  bool Contains(const Key& key) const;

  // Iteration over the contents of a skip list
  // 遍历SkipList的内容
  class Iterator {
   public:
    // Initialize an iterator over the specified list.
    // The returned iterator is not valid.
    // 初始化一个指向指定列表的迭代器。返回的迭代器是无效的。
    explicit Iterator(const SkipList* list);

    // Returns true iff the iterator is positioned at a valid node.
    // 如果迭代器位于有效节点，则返回true。
    bool Valid() const;

    // Returns the key at the current position.
    // REQUIRES: Valid()
    // 返回当前位置的键。
    // 要求: 当前节点是有效的。
    const Key& key() const;

    // Advances to the next position.
    // REQUIRES: Valid()
    // 在当前节点有效的情况下，前进到下一个位置。
    // 本质上是通过调用Node::Next(0)方法找到当前节点在第0层的下一个节点
    void Next();

    // Advances to the previous position.
    // REQUIRES: Valid()
    // 在当前节点有效的情况下，后退到上一个位置。
    // 本质上是通过SkipList::FindLessThan()方法找到小于当前节点的最后一个节点
    void Prev();

    // Advance to the first entry with a key >= target
    // 将迭代器移动到第一个键大于等于target的位置。
    // 本质上是通过SkipList::FindGreaterOrEqual()方法找到大于等于target的第一个节点
    void Seek(const Key& target);

    // Position at the first entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    // 将迭代器移动到列表中的第一个条目。
    // 当且仅当列表不为空时，迭代器的最终状态是有效的。
    // 本质上是通过head_的next指针找到第一个节点
    void SeekToFirst();

    // Position at the last entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    // 将迭代器移动到列表中的最后一个条目。
    // 当且仅当列表不为空时，迭代器的最终状态是有效的。
    // 本质上是通过SkipList::FindLast()方法找到最后一个节点
    void SeekToLast();

   private:
    const SkipList* list_;  // Iterator所属的SkipList
    Node* node_;            // Iterator当前指向的节点
    // Intentionally copyable
  };

 private:
  // SkipList的最大高度 12，这是一个权衡了时间开销和空间开销的经验值
  enum { kMaxHeight = 12 };

  inline int GetMaxHeight() const { // 获取跳表中当前所有节点的最大高度(不包括头节点)
    return max_height_.load(std::memory_order_relaxed);
  }

  // 创建一个新的节点，key是节点的key，height是该节点的高度
  // 一个Node需要申请的内存为:sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1)
  // Node类本身占用的内存为sizeof(Node)，由于Node声明时next_已经有1个指针的空间，
  // 所以后续只需要再申请height - 1个指针的空间即可
  Node* NewNode(const Key& key, int height);
  // 随机生成一个高度，高度的范围是[1, kMaxHeight]
  // 用于确定新插入的节点的高度
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

  // Return true if key is greater than the data stored in "n"
  // 如果key大于“n”中存储的数据，则返回true，意味着key在n之后
  bool KeyIsAfterNode(const Key& key, Node* n) const;

  // Return the earliest node that comes at or after key.
  // Return nullptr if there is no such node.
  // 返回key之后的最早的节点，如果没有这样的节点，则返回nullptr。
  // 即寻找与key相等或者大于key的最小节点
  //
  // If prev is non-null, fills prev[level] with pointer to previous
  // node at "level" for every level in [0..max_height_-1].
  // prev是一个指向指针的指针，如果prev不为空，则将prev[level]填充为“level”中的每个级别的前一个节点的指针，范围是[0..max_height_-1]。
  // 也就是说，如果prev不为空，那么prev[level]就是当前节点在level层的前一个节点
  // 该函数每次都会从最大高度开始查找，然后逐层向下查找，不断地缩小查找范围
  // 函数每次都会查到最底层后再返回，因为只有这样才能保证返回的节点是大于等于key的最小节点，底层的节点是最完整的
  //
  // 类似于n分查找，时间复杂度为O(logn)。n=1/p，p是采样概率，即每次增加高度的概率
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  // Return the latest node with a key < key.
  // Return head_ if there is no such node.
  // 返回最后一个键小于key的节点，如果没有这样的节点，则返回head_。
  Node* FindLessThan(const Key& key) const;

  // Return the last node in the list.
  // Return head_ if list is empty.
  // 返回列表中的最后一个节点，如果列表为空，则返回head_。
  Node* FindLast() const;

  // Immutable after construction
  // 在构造之后不可变
  Comparator const compare_;
  // 每个跳表都有一个Arena对象，用于分配节点内存
  Arena* const arena_;  // Arena used for allocations of nodes

  // 代表当前跳表的头节点，不存储数据，只是一个哨兵节点，head_中存储了kMaxHeight个指针，这是预分配的最大高度
  Node* const head_;

  // Modified only by Insert().  Read racily by readers, but stale
  // values are ok.
  // 只能被Insert()修改，被读者读取时可能是脏的，但是过期的值是可以的
  std::atomic<int> max_height_;  // Height of the entire list

  // Read/written only by Insert().
  // 只能被Insert()读写
  Random rnd_;
};

// Implementation details follow
// 用模板类实现的SkipList的节点
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  // 该节点的key
  // C++: const成员变量只能在构造函数初始化列表中初始化，不能在构造函数内部初始化
  // Key const key和const Key key在这里是等价的，都是表示key是一个常量
  Key const key;

  // Accessors/mutators for links.  Wrapped in methods so we can
  // add the appropriate barriers as necessary.
  // 链接的访问器/修改器。封装在方法中，以便我们可以根据需要添加适当的屏障。
  // 即访问和修改当前节点的next指针
  Node* Next(int n) {
    assert(n >= 0);
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    // 使用`acquire_load`内存序列化语义，以便我们观察到返回的Node的完全初始化版本。
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    // 使用`release_store`内存序列化语义，以便通过这个指针读取的任何人都能观察到插入节点的完全初始化版本。
    next_[n].store(x, std::memory_order_release);
  }

  // No-barrier variants that can be safely used in a few locations.
  // 可以在少数位置安全使用的无屏障变体。
  // memory_order_relaxed：不对内存访问排序，也不对其它线程的内存访问排序，
  // 只保证当前线程的内存访问操作是原子的(不可分割的)
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  // Array of length equal to the node height.  next_[0] is lowest level link.
  // 长度等于节点高度的数组。next_[0]是最低级别的链接。
  // Flexible Array Member (FAM)：C99标准中的一个特性，允许结构体的最后一个成员是一个未知大小的数组。
  // 这里使用了FAM，next_是一个柔性数组成员，它的长度是动态的，取决于节点的高度。
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
      // 这里分配的内存包含两部分:
      // 1. Node对象本身，大小为sizeof(Node)
      // 2. Node对象的next_数组，大小为sizeof(std::atomic<Node*>) * (height - 1)，
      // 因为Node对象中的next_数组本身已经包含了一个指针空间，所以这里只需要分配height - 1个指针空间即可
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
      // 与new Node(key)不同，这里是在arena_上分配内存，而不是在堆上分配内存
      // 是所谓的placement new，即在指定的内存地址上构造对象
  return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
  list_ = list;
  node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
  // node_不为空则说明迭代器当前指向的节点是有效的
  return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
  assert(Valid());
  return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
  assert(Valid());
  // Node类的方法，获取当前节点在第0层的下一个节点
  node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  // 不使用显式的“prev”链接，我们只搜索在key之前的最后一个节点。
  assert(Valid());
  // 通过SkipList::FindLessThan()方法找到小于当前节点的最后一个节点
  // 即找到了当前节点的前一个节点
  node_ = list_->FindLessThan(node_->key);
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
  // 通过SkipList::FindGreaterOrEqual()方法找到大于等于target的第一个节点
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
  // head_本身不存储数据，只是一个哨兵节点，head_的next指针指向第一个节点
  node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // Increase height with probability 1 in kBranching
  // 每次增加高度的概率是1/kBranching
  static const unsigned int kBranching = 4;
  int height = 1;
  // 本质上是rnd_.OneIn(kBranching)方法，即每次调用都有1/kBranching的概率返回true
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  assert(height > 0);
  assert(height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
  // null n is considered infinite
  // 用于判断key是否在n之后，如果n为空，则返回false
  // compare_(n->key, key) < 0表示n->key < key，即key在n之后
  // == 0表示n->key == key，即key与n相等
  // > 0表示n->key > key，即key在n之前
  return (n != nullptr) && (compare_(n->key, key) < 0);
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  Node* x = head_;
  // 获取当前有效节点的最大高度，总是从最大高度开始查找
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) { // 若key应该排在next之后，则继续在当前层向后查找
      // Keep searching in this list
      x = next;
    } else { // key不排在next之后，说明key等于next或者key在next之前
      // 若prev不为空，将prev[level]设置为key在level层的前一个节点
      if (prev != nullptr) prev[level] = x;
      if (level == 0) { // 第0层已经是最底层，直接返回next
        return next;
      } else { // 进一步的缩小查找范围，向下查找意味着查找区间内更多的节点
        // Switch to next list
        level--;
      }
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  // 每次都从最大高度开始查找，然后逐层向下查找，不断地缩小查找范围
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    // next为空意味着x是最后一个节点
    // 或者next->key >= key，意味着next是大于等于key的最小节点
    // 因此x就是小于key的最后一个节点
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) { // 若已经是最底层，直接返回x
        return x;
      } else { // 进一步的缩小查找范围，向下查找意味着查找区间内更多的节点
        // Switch to next list
        level--;
      }
    } else { // 继续在当前层向后查找
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast()
    const {
  Node* x = head_;
  // 从最大高度开始查找，然后逐层向下查找
  int level = GetMaxHeight() - 1;
  // 从最高层向下查找，每当遍历完一层时，level减1，直到level为0且next为空时代表遍历到了最后一个节点
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) { // 初始化head_的kMaxHeight个next指针为nullptr
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  // TODO(opt): We can use a barrier-free variant of FindGreaterOrEqual()
  // here since Insert() is externally synchronized.
  // 我们可以在这里使用一个无屏障的FindGreaterOrEqual()变体，因为Insert()是外部同步的。
  // prev指针数组用来存储每一层上小于key的最后一个节点
  Node* prev[kMaxHeight];
  // 通过FindGreaterOrEqual()方法找到当前跳表中大于等于key的第一个节点
  Node* x = FindGreaterOrEqual(key, prev);

  // Our data structure does not allow duplicate insertion
  // 我们的数据结构不允许重复插入，即每个key都是唯一的
  assert(x == nullptr || !Equal(key, x->key));

  // 随机生成一个高度，高度的范围是[1, kMaxHeight]
  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    // 初始化新增高度的prev指针为head_
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // It is ok to mutate max_height_ without any synchronization
    // with concurrent readers.  A concurrent reader that observes
    // the new value of max_height_ will see either the old value of
    // new level pointers from head_ (nullptr), or a new value set in
    // the loop below.  In the former case the reader will
    // immediately drop to the next level since nullptr sorts after all
    // keys.  In the latter case the reader will use the new node.
    // 在并发读取操作中，可以不需要对`max_height_`的读写进行同步。
    // 如果一个并发读取操作观察到了`max_height_`的新值，那么他可能会看到两种情况：
    // 1. 他可能会看到`head_`中新层的指针的旧值（nullptr）。这意味着读取操作会立即降到下一层，因为nullptr会排在所有的key之后。
    // 这意味着在观察期间，该插入操作还未执行，因此他会继续使用旧的指针数组。所以这种情况下，读取操作不会看到正在插入的节点。
    // 2. 他可能会看到`head_`中新层的指针的新值。这意味着在观察期间，该插入操作已经执行，新的指针数组已经被设置。
    // 在这种情况下，读取操作会看到新的节点。
    // TODO: 读到max_height_的新值的同时却读不到新的节点已经违反了一致性，为什么还要这么设计？
    // 除非LevelDB认为max_height_的更新与实际的节点插入不是一个原子操作。
    // 或者无论读写操作，都会提前获取一把大锁，这样就不会有这个问题了。
    max_height_.store(height, std::memory_order_relaxed);
  }

  // 创建一个新的节点，key是节点的key，height是该节点的高度
  x = NewNode(key, height);
  // 更新节点每一层的next指针
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "x" in prev[i].
    // NoBarrier_SetNext()足够了，因为当我们在prev[i]中发布一个指向“x”的指针时，我们将添加一个屏障。
    // 因为这里我们只要需要确保prev[i]设置next的操作在prev[i]设置x的操作之后即可
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  // 通过FindGreaterOrEqual()方法找到当前跳表中大于等于key的第一个节点
  Node* x = FindGreaterOrEqual(key, nullptr);
  // 若x不为空且x的key与key相等，则说明key在跳表中
  if (x != nullptr && Equal(key, x->key)) {
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SKIPLIST_H_
