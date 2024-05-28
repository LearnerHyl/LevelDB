// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"

#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }

  // 构建Data Block时的Options
  Options options;
  // 构建Index Block时的Options
  Options index_block_options;
  // 通常为PosixWritableFile，决定如何进行文件的写入
  WritableFile* file;
  // SSTable中第一个空闲字节的索引，意味着之前的部分都已经写入了数据
  uint64_t offset;
  // 当前的操作状态
  Status status;
  // 构建Data Block的BlockBuilder
  BlockBuilder data_block;
  // 构建Index Block的BlockBuilder
  BlockBuilder index_block;
  // 当前Data Block中已经写入的最后一个key
  std::string last_key;
  // 当前Data Block中已经写入的kv对的数目
  int64_t num_entries;
  // 构建过程是否已经结束，可能是调用Finish()或Abandon()方法
  bool closed;
  // 构建Filter Block的FilterBlockBuilder
  FilterBlockBuilder* filter_block;

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  // 在我们看到下一个数据块的第一个键之前，我们不会发出当前块的索引条目。
  // 这使我们可以在索引块中使用更短的键。例如，考虑键“the quick brown fox”和“the
  // who”之间的块边界。 我们可以使用“the
  // r”作为索引块条目的键，因为它大于第一个块中的所有条目，并且小于后续块中的所有条目。
  // TODO:即最短索引键原则，这部分在之前看data index block时有详细介绍，
  // 原则就是在符合要求的情况下，尽量使用更短的key来作为index block的key。
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.
  // 不变量：只有在data_block为空时，r->pending_index_entry为true。
  // 当pending_index_entry为true，意味着可以为之前已经构建的Data Block生成Index
  // Block的entry。
  //
  // 生成时机：当前的DataBlock写满并Flush()后，在准备写入下一个Data
  // Block的第一个key之前， 可以根据当前Data Block的最后一个key和下一个Data
  // Block的第一个key生成Index Block的entry。
  // pending_index_entry在Flush()函数中设置为true，表示当前Data
  // Block已经构建完成，可以生成Index Block的entry。
  bool pending_index_entry;
  // 刚写入的Block在SSTable文件中的偏移量和大小。正在等待被添加到Index Block中。
  // 对应上面的pending_index_entry触发时，会生成该Block的Index Entry
  BlockHandle pending_handle;  // Handle to add to index block

  // 压缩后的Data Block的内容
  std::string compressed_output;
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  // 若该SSTable需要构建Filter Block，则在构造函数中初始化FilterBlockBuilder
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  // 注意：如果Options中添加了更多字段，请更新此函数，以捕获不应该在构建Table的过程中更改的更改。
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  // 请注意，任何活动的BlockBuilders都指向rep_->options，因此将自动选择更新的选项。
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  // 当前Build过程是否已经结束
  assert(!r->closed);
  if (!ok()) return;
  // 根据SSTable规则，key必须是递增的
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  // 若当前Data Block为空，则需要生成Index Block的entry
  // 具体实际: 当前一定是在写一个空的Data
  // Block的第一个key，但还未写入，因为我们要为上一个Block生成Index
  // Block的entry， 因此需要知道上一个Data Block的最后一个key和当前Data
  // Block的第一个key，以便生成Index Block的entry。 生成Index
  // Block的entry的原则是：尽量使用更短的key来作为Index Block的key。
  if (r->pending_index_entry) {
    // 根据上面的说明，data_block一定是空的
    assert(r->data_block.empty());
    // 根据当前Data Block的最后一个key和下一个Data Block的第一个key
    // 生成上一个已经构建的Data Block的Index Entry中的key
    // 最短索引键原则：即生成上一个Data Block中的最后一个key和下一个Data
    // Block的第一个key的最短key，
    // 比如，"abcd"和"adkmkl"，最短key为"ae"。结果保存到r->last_key中
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    // 将上一个Data Block的BlockHandle的数据序列化后保存到handle_encoding中
    // 因为data index
    // block的entry的value部分是BlockHandle的序列化数据，即定位Data
    // Block的位置(offset + size)
    r->pending_handle.EncodeTo(&handle_encoding);
    // 将上一个Data Block的Index Entry添加到Index Block中
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  // 在构建Data Block的同时，也同时构建Filter Block
  if (r->filter_block != nullptr) {
    // 在filter block中添加key
    r->filter_block->AddKey(key);
  }

  // 更新last_key和num_entries
  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  // 在Data Block中添加kv对
  r->data_block.Add(key, value);

  // 返回当前Data Block的Data部分使用的字节数的估计值，
  // kv对的大小+所有重启点的总大小+重启点数目值
  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  // 若当前Data Block的Data部分使用的字节数的估计值大于等于Data Block的大小，
  // 则结束当前Data Block的构建，将Data Block写入文件，并生成index entry等数据。
  if (estimated_block_size >= r->options.block_size) {
    // 具体功能实现由Flush()函数完成
    Flush();
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  // 当前Build过程是否已经结束
  assert(!r->closed);
  if (!ok()) return;
  // 若当前Data Block为空，则不需要进行Flush操作
  if (r->data_block.empty()) return;
  // 不能有未完成的data block的index entry没有添加
  assert(!r->pending_index_entry);
  // 将block的data部分压缩，生成CRC后，按照Block的物理格式写入文件
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {  // 若Block写入成功
    r->pending_index_entry = true;
    // 将file中的数据刷新到内核缓冲区中，之后由内核负责将数据写入磁盘
    r->status = r->file->Flush();
  }
  // 每次将一个Data Block写入文件后，更新一次Filter Block的状态，
  // 查看是否需要生成新的filter data
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  // 文件格式包含一个块序列，其中每个块从物理格式上看是：
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  // 获取当前Block中的Data部分的数据(kv对+若干重启点+重启点数目值)
  Slice raw = block->Finish();

  // block_contents主要用于存储压缩后的Data Block的Data部分的内容
  Slice block_contents;
  // 根据options中的compression类型，对Data Block进行压缩，默认为Snappy算法
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?
  switch (type) {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: {
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) &&
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else {
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        // 如果不支持Snapyy压缩，或者压缩率低于12.5%，则直接存储未压缩的形式
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }

    case kZstdCompression: {
      std::string* compressed = &r->compressed_output;
      if (port::Zstd_Compress(r->options.zstd_compression_level, raw.data(),
                              raw.size(), compressed) &&
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else {
        // Zstd not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        // 如果不支持Zstd压缩，或者压缩率低于12.5%，则直接存储未压缩的形式
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  // 此时block_contents是压缩完毕的Data Block的全部内容
  WriteRawBlock(block_contents, type, handle);
  // 当前Block写入完毕，重置BlockBuilder
  r->compressed_output.clear();
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  // handle存储当前Block在SSTable文件中的偏移量和大小
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  // 将当前Block的Data部分写入到SSTable文件中
  r->status = r->file->Append(block_contents);
  if (r->status
          .ok()) {  // 若Data部分写入成功，则将type与CRC打包为trailer进一步的追加到SSTable文件中
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    // 计算当前Block中的Data部分的CRC校验值
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    // 扩展crc值以进一步包含compression type字段
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    // 将crc值编码为定长4字节的字符串，之后追加到type的后面
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    // 此时trailer为type(1字节)+crc(4字节)，这里的crc计算的是Data+type的crc值
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    // 若写入成功，则更新当前待写入的Data Block的偏移量
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const { return rep_->status; }

Status TableBuilder::Finish() {
  Rep* r = rep_;
  // 剩余的kv对可能不足一个Data Block的大小，无法自动触发Flush()，因此需要手动调用Flush()方法
  // 将余下的KV对同样以一个Data Block的形式写入文件
  Flush();
  assert(!r->closed);
  // 此时，所有kv对已经写入文件，标记当前Build过程已经结束，set closed为true
  r->closed = true;

  // 根据之前学习SSTable的格式，index_block_handle和metaindex_block_handle都在Footer中
  // 而metaindex_block_handle中只有一条记录，就是filter block的handle
  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  // 将Filter Block写入文件
  if (ok() && r->filter_block != nullptr) {
    // 获取Filter Block的Data部分，之后将其追加到SSTable文件中
    // filter_block_handle保存了Filter Block的偏移量和大小，注意不包括type和crc字段
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // Write metaindex block
  // 因为metaindex block中只有一条记录，就是filter block的handle
  // key一般是filter.Name，value是filter block的handle
  // 默认是BloomFilter，所以key是"filter.Leveldb.BuiltinBloomFilter2"
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != nullptr) {
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      // 将<key, handle_encoding>添加到meta_index_block中
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    // 将meta_index_block写入文件
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // Write index block
  if (ok()) {
    // 需要为最后一个Data Block生成Index Block的entry
    if (r->pending_index_entry) {
      // 只需要大于最后一个Data Block的key，因此使用FindShortSuccessor 
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      // 将最后一个块的BlockHandle的数据序列化后保存到handle_encoding中
      r->pending_handle.EncodeTo(&handle_encoding);
      // 将最后一个Data Block的Index Entry添加到Index Block中
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    // 将Index Block写入到SSTable文件中
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  // 最后，构建Footer=metaindex block handle + index block handle (+ padding) + magic number
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    // 这一步确保metaindex_block_handle和index_block_handle的总大小一定为40字节
    // 即2*BlockHandle::kMaxEncodedLength，若不够则用padding补齐
    // 同时将kTableMagicNumber用固定长度编码方式存储到footer_encoding中
    footer.EncodeTo(&footer_encoding);
    // 将Footer写入到SSTable文件中
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace leveldb
