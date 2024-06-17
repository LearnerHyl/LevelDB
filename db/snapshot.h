// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SNAPSHOT_H_
#define STORAGE_LEVELDB_DB_SNAPSHOT_H_

#include "db/dbformat.h"
#include "leveldb/db.h"

namespace leveldb {

class SnapshotList;

// Snapshots are kept in a doubly-linked list in the DB.
// Each SnapshotImpl corresponds to a particular sequence number.
// 快照在DB中以双向链表的形式保存。
// 每个SnapshotImpl对应一个特定的序列号，即一个kv操作的序列号/时间戳。
// 可以认为每个SnapshotImpl对象都封装了一个SequenceNumber对象。
class SnapshotImpl : public Snapshot {
 public:
  SnapshotImpl(SequenceNumber sequence_number)
      : sequence_number_(sequence_number) {}

  SequenceNumber sequence_number() const { return sequence_number_; }

 private:
  friend class SnapshotList;

  // SnapshotImpl is kept in a doubly-linked circular list. The SnapshotList
  // implementation operates on the next/previous fields directly.
  // SnapshotImpl保存在一个双向循环链表中。SnapshotList实现直接操作next/previous字段。
  SnapshotImpl* prev_;
  SnapshotImpl* next_;

  // 这是快照的本质，就是kv操作的序列号/时间戳
  const SequenceNumber sequence_number_;

#if !defined(NDEBUG)
  SnapshotList* list_ = nullptr;
#endif  // !defined(NDEBUG)
};

// SnapshotList是双向链表，里面的节点类型是SnapshotImpl，
// 头部是最旧的快照，尾部是最新的快照。
class SnapshotList {
 public:
  SnapshotList() : head_(0) {
    head_.prev_ = &head_;
    head_.next_ = &head_;
  }

  bool empty() const { return head_.next_ == &head_; }
  // 返回最旧的快照，即头部的下一个节点
  SnapshotImpl* oldest() const {
    assert(!empty());
    return head_.next_;
  }
  // 返回最新的快照，即尾部的上一个节点，代表最新的kv操作时间戳
  SnapshotImpl* newest() const {
    assert(!empty());
    return head_.prev_;
  }

  // Creates a SnapshotImpl and appends it to the end of the list.
  // 创建一个快照，并将其追加到双向链表的末尾。因为末尾代表最新的kv操作时间戳。
  SnapshotImpl* New(SequenceNumber sequence_number) {
    // 确保新的kv操作时间戳大于等于当前最新的kv操作时间戳
    assert(empty() || newest()->sequence_number_ <= sequence_number);

    SnapshotImpl* snapshot = new SnapshotImpl(sequence_number);

#if !defined(NDEBUG)
    snapshot->list_ = this;
#endif  // !defined(NDEBUG)
    snapshot->next_ = &head_;
    snapshot->prev_ = head_.prev_;
    snapshot->prev_->next_ = snapshot;
    snapshot->next_->prev_ = snapshot;
    return snapshot;
  }

  // Removes a SnapshotImpl from this list.
  // 从双向链表中删除一个快照。
  //
  // The snapshot must have been created by calling New() on this list.
  // 快照必须是通过调用New()方法创建的。
  //
  // The snapshot pointer should not be const, because its memory is
  // deallocated. However, that would force us to change DB::ReleaseSnapshot(),
  // which is in the API, and currently takes a const Snapshot.
  // 快照指针不应该是const，因为它的内存会被释放。但是，这将迫使我们更改DB::ReleaseSnapshot()，
  // 它在API中，并且当前接受一个const快照。
  void Delete(const SnapshotImpl* snapshot) {
    // 确保待删除的快照是由当前链表创建的
#if !defined(NDEBUG)
    assert(snapshot->list_ == this);
#endif  // !defined(NDEBUG)
    // 从双向链表中移除快照并释放内存
    snapshot->prev_->next_ = snapshot->next_;
    snapshot->next_->prev_ = snapshot->prev_;
    delete snapshot;
  }

 private:
  // Dummy head of doubly-linked list of snapshots
  // 快照双向链表的哨兵节点，不存储数据
  SnapshotImpl head_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SNAPSHOT_H_
