// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/memtable.h"
#include "db/dbformat.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "util/coding.h"

namespace leveldb {

static Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  // data的编码格式是：值长度+值，这由下面的EncodeKey()函数中的编码方式决定
  // 所以这里先解析出值的长度，之后的部分就代表真正的值
  p = GetVarint32Ptr(p, p + 5, &len);  // +5: we assume "p" is not corrupted
  return Slice(p, len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

int MemTable::KeyComparator::operator()(const char* aptr,
                                        const char* bptr) const {
  // Internal keys are encoded as length-prefixed strings.
  // key被编码为长度前缀字符串
  Slice a = GetLengthPrefixedSlice(aptr);
  Slice b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
// 为“target”编码一个合适的内部键目标并返回它。
// 使用*scratch作为临时空间，返回的指针将指向此临时空间。
// scratch中存储的是数据长度+数据
static const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, target.size());
  scratch->append(target.data(), target.size());
  return scratch->data();
}

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

  MemTableIterator(const MemTableIterator&) = delete;
  MemTableIterator& operator=(const MemTableIterator&) = delete;

  ~MemTableIterator() override = default;

  bool Valid() const override { return iter_.Valid(); }
  void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  Slice value() const override {
    Slice key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;  // For passing to EncodeKey
};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  tag          : uint64((sequence << 8) | type)
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  size_t key_size = key.size(); // key的长度
  size_t val_size = value.size(); // value的长度
  size_t internal_key_size = key_size + 8; // key的长度+tag的长度
  // 这里的encoded_len是整个entry的长度，包括key_size、key、tag、value_size、value
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  // 从arena_内存池中申请encoded_len大小的内存
  char* buf = arena_.Allocate(encoded_len);
  // 将key的长度编码后保存到buf中
  char* p = EncodeVarint32(buf, internal_key_size);
  // 将key的数据保存到buf中
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  // 将tag的数据保存到buf中,tag本质上是sequenceNumber和valueType的组合
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  // 将value的长度编码后保存到buf中
  p = EncodeVarint32(p, val_size);
  // 将value的数据保存到buf中
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);
  // 最后将entry的首地址buf插入到底层数据结构table_中(table_是一个跳表)
  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Slice memkey = key.memtable_key();
  // 构造一个基于跳表的迭代器
  Table::Iterator iter(&table_);
  // 在跳表中查找是否存在key
  iter.Seek(memkey.data());
  if (iter.Valid()) { // 目标key存在
    // entry format is:
    //    klength  varint32           // klength=userkey.length()+tag.length()=key.length()+8
    //    userkey  char[klength]
    //    tag      uint64            // tag=(sequenceNumber<<8)|valueType)
    //    vlength  varint32
    //    value    char[vlength]
    // Check that it belongs to same user key.  We do not check the
    // sequence number since the Seek() call above should have skipped
    // all entries with overly large sequence numbers.
    // 检查它是否属于相同的用户键。我们不检查序列号，因为上面的Seek()调用应该已经跳过了所有序列号过大的条目。
    const char* entry = iter.key(); // 获取entry的首地址，对应了add操作中写入时的格式
    uint32_t key_length;
    // 获取klengh的值
    const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
    if (comparator_.comparator.user_comparator()->Compare(
            Slice(key_ptr, key_length - 8), key.user_key()) == 0) { // 对比memtable中的key和传入的key是否相等
      // Correct user key
      // 获取tag值
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
      switch (static_cast<ValueType>(tag & 0xff)) { // 获取valueType
        case kTypeValue: { // 如果是kTypeValue，表示当前key对应的value存在，因为这是插入操作的结果
          // v存储了value的长度和值
          Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
          value->assign(v.data(), v.size());
          return true;
        }
        case kTypeDeletion: // 如果是kTypeDeletion，表示这个kv对已经被删除
          *s = Status::NotFound(Slice()); // 返回一个NotFound的状态，表示当前key对应的value已经被删除
          return true;
      }
    }
  }
  return false; // 说明没有找到key对应的value，返回false
}

}  // namespace leveldb
