// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace leveldb {

struct Table::Rep { // 一个Rep对象存储了访问一个SSTable所需的所有信息,是一个Table对象的内部类
  // 析构函数，销毁 Rep 对象时会调用
  // 释放分配给 filter、filter_data 和 index_block 的内存
  ~Rep() {
    delete filter;          // 删除过滤器对象
    delete[] filter_data;   // 删除过滤器数据数组
    delete index_block;     // 删除索引块对象
  }

  Options options;               // 用于配置表的选项
  Status status;                 // 表示表的当前状态
  RandomAccessFile* file;        // 指向表文件的指针
  uint64_t cache_id;             // 缓存 ID，用于标识该表在缓存中的位置
  FilterBlockReader* filter;     // 用于读取当前SSTable的filter block的对象
  const char* filter_data;       // 指向filter block的data部分的指针，意味着是堆分配的，需要手动释放

  // metaindex_handle 是 metaindex_block 的句柄，保存在文件的尾部（footer）
  BlockHandle metaindex_handle;
  
  Block* index_block;            // 指向索引块的指针，用于定位数据块
};

Status Table::Open(const Options& options, RandomAccessFile* file,
                   uint64_t size, Table** table) {
  // 初始化 table 指针为 nullptr
  *table = nullptr;
  
  // 检查文件大小是否足够容纳一个有效的 Footer
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  // 为 Footer 预留空间
  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  
  // 从文件的末尾读取 Footer
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;  // 如果读取失败，则返回错误状态

  Footer footer;
  // 解码 Footer，从而获取该SSTable的index block和metaindex block的BlockHandle
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;  // 如果解码失败，则返回错误状态

  // 读取索引块
  BlockContents index_block_contents;
  ReadOptions opt;
  
  // 如果启用了严格检查，则设置校验和验证选项
  if (options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  // 根据 Footer 中的 index_handle 读取索引块，将index block中的data部分存储到 index_block_contents 中
  s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);
  if (s.ok()) {
    // 使用读取到的 index_block_contents 创建对应的 Block 对象
    Block* index_block = new Block(index_block_contents);
    
    // 创建 Table::Rep 对象并初始化其成员
    Rep* rep = new Table::Rep;
    rep->options = options;                            // 设置选项
    rep->file = file;                                  // 设置文件指针
    rep->metaindex_handle = footer.metaindex_handle(); // 设置 metaindex_handle
    rep->index_block = index_block;                    // 设置索引块指针
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0); // 设置缓存 ID
    rep->filter_data = nullptr;                        // 初始化 filter_data 为 nullptr
    rep->filter = nullptr;                             // 初始化 filter 为 nullptr
    
    // 可以认为 Table对象就是这个SSTable的一个抽象，它持有一个 Table::Rep 对象
    // 创建新的 Table 对象并将其指针赋值给 table
    *table = new Table(rep);
    // 读取 Meta 信息
    (*table)->ReadMeta(footer);
  }

  // 返回状态
  return s;
}

void Table::ReadMeta(const Footer& footer) {
  // 如果没有设置过滤策略，则不需要读取任何元数据
  if (rep_->options.filter_policy == nullptr) {
    return;  // 不需要元数据
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  // TODO: 如果 footer.metaindex_handle() 的大小表示它是一个空块，则跳过此步骤
  ReadOptions opt;
  // 如果启用了严格检查，则设置校验和验证选项
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  // 读取meta index block(filter block index block)的内容，存储到contents中
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // 不传播错误，因为元数据不是操作所必需的
    return;
  }
  // 构建一个Block对象，用于遍历meta index block，meta idx block中只有一个键值对，
  // key是"filter." + filter_policy->Name()，value是filter block的BlockHandle
  Block* meta = new Block(contents);

  // 使用字节序比较器创建一个迭代器来遍历元数据块
  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  // 将过滤策略的名称添加到key中，一般是"filter.LevelDB.BuiltinBloomFilter2"
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);  // 在元数据块中查找过滤器信息
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());  // 如果找到过滤器信息，则读取过滤器数据
  }
  delete iter;  // 删除迭代器
  delete meta;  // 删除元数据块
}


void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  // 定义一个BlockHandle对象
  BlockHandle filter_handle;
  // 解码filter_handle_value并赋值给filter_handle，如果解码失败则返回
  // 解码的结果是filter block的BlockHandle
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // 我们可能希望在Table::Open中开始要求校验和验证时与ReadBlock()统一。
  ReadOptions opt;
  // 如果开启了paranoid_checks选项，则设置opt.verify_checksums为true
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  // 定义一个BlockContents对象
  BlockContents block;
  // 从文件中读取filter block的内容，存储到BlockContents对象中
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  // 如果block是堆分配的，即不属于Arena分配的，则后续需要用户手动释放
  if (block.heap_allocated) {
    // 将block数据赋值给rep_->filter_data，以便以后删除
    rep_->filter_data = block.data.data();  // 将需要稍后删除
  }
  // 创建一个新的FilterBlockReader对象并赋值给rep_->filter
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

// Table类的析构函数，删除rep_指针
Table::~Table() { delete rep_; }

// 静态函数，用于删除Block对象
static void DeleteBlock(void* arg, void* ignored) {
  // 将arg转换为Block指针并删除
  delete reinterpret_cast<Block*>(arg);
}

// 静态函数，用于删除缓存的Block对象
static void DeleteCachedBlock(const Slice& key, void* value) {
  // 将value转换为Block指针并删除
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

// 静态函数，用于释放缓存的Block
static void ReleaseBlock(void* arg, void* h) {
  // 将arg转换为Cache指针
  Cache* cache = reinterpret_cast<Cache*>(arg);
  // 将h转换为Cache::Handle指针
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  // 释放handle所指向的Block对象占用的缓存
  cache->Release(handle);
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
// 将索引迭代器的值（即编码的BlockHandle）转换为对应块内容的迭代器
// arg是一个Table指针，代表一个SSTable对象
// options是读取选项；index_value是从index block中读取的压缩后的BlockHandle
Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  // 将arg转换为Table指针，代表一个SSTable对象
  Table* table = reinterpret_cast<Table*>(arg);
  // 获取块缓存
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  BlockHandle handle;
  Slice input = index_value;
  // 从index_value解码BlockHandle
  Status s = handle.DecodeFrom(&input);
  // 我们有意在index_value中允许额外的内容，以便将来可以添加更多功能。

  if (s.ok()) {
    BlockContents contents;
    // 如果块缓存不为空
    if (block_cache != nullptr) {
      char cache_key_buffer[16];
      // 这里使用到了BlockCache，对接上了TableCache。
      // 每个DB实例有一个TableCache对象，TableCache内部采用了ShardedLRUCache对象，因此每个
      // TableCache对象内部有多个LRUCache对象，每个LRUCache对象都有自己的cache_id，用于标识。
      // 编码缓存键，前8个字节是cache_id，后8个字节是该block在SSTable中的起始偏移量。
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      // 查找缓存中的块，根据key获取目标block的cache_handle
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        // 如果缓存命中，根据cache_handle获取对应的Block对象。
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        // 如果缓存未命中，从文件中读取块
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          // 创建新的块对象
          block = new Block(contents);
          // 如果块是可缓存的，并且fill_cache选项为true，将块插入缓存
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(key, block, block->size(),
                                               &DeleteCachedBlock);
          }
        }
      }
    } else {
      // 如果没有块缓存，直接从文件中读取块
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }
  // 至此，已经从BlockCache或者SSTable文件中读取到了Block对象，注意Block对象中只会存储Block的data部分，
  // 而不包含Block的type和crc校验和
  Iterator* iter;
  if (block != nullptr) {
    // 如果块不为空，为块创建新的迭代器
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == nullptr) {
      // 如果没有缓存句柄，注册清理函数以删除块，意味着块是堆分配的，需要手动释放
      iter->RegisterCleanup(&DeleteBlock, block, nullptr);
    } else {
      // 如果有缓存句柄，注册清理函数以释放缓存块，意味着块是缓存的，交给LRUCache管理
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    // 如果块为空，创建一个错误迭代器
    iter = NewErrorIterator(s);
  }
  return iter;
}

// 创建一个新的迭代器，用于遍历表中的数据
Iterator* Table::NewIterator(const ReadOptions& options) const {
  // 使用TwoLevelIterator遍历表中的块，两层迭代器，第一层是索引块迭代器，第二层是数据块迭代器
  // 需要先在index block中找到对应的data block的BlockHandle，然后根据BlockHandle读取data block，
  // 之后根据data block的内容构建BlockReader对象(第二层迭代器)，用于遍历data block中的所有kv
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

// 从表中获取数据
Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&,
                                                const Slice&)) {
  Status s;
  // 创建一个新的索引块迭代器
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  // 定位到第一个键大于等于k的位置，在index block中，意味着该value对应的data block可能包含k
  iiter->Seek(k);
  if (iiter->Valid()) {
    // 获取index block中的value，即待解码BlockHandle，对应了目标data block的位置和大小
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    // 如果有过滤器且键可能存在，则继续查找
    // 由于一个Data Block默认为4KB，而默认每2KB数据创建一个filter data，因此一个Data Block一般有
    // 2个filter data，但是这里只会使用第一个filter data,为什么?
    // TODO: 为什么只使用第一个filter data，看完BloomFilter后再回来看
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // 键未找到，根据BloomFilter的原理，若key不存在，则一定不存在，直接返回
    } else {
      // 从块中读取数据，根据block_handle构造目标data block的迭代器
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        // 调用回调函数处理结果
        (*handle_result)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

// 近似获取键的偏移量
uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  // 创建一个新的索引块迭代器
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  // 定位到键的位置
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    // 获取索引块中的value，即待解码BlockHandle，对应了目标data block的位置和大小
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      // 我们无法解码索引块中的块句柄。
      // 我们只是返回metaindex块的偏移量，它接近文件的总大小
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    // 键在文件的最后一个键之后，返回metaindex块的偏移量
    // 它接近文件的末尾
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
