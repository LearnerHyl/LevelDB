// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
// TableBuilder提供了用于构建Table的接口(从键到值的不可变且排序的映射，即SSTable)。
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.
// 多线程可以在没有外部同步的情况下调用TableBuilder的const方法，但是如果任何线程
// 可能调用非const方法，则访问相同TableBuilder的所有线程必须使用外部同步。
//
// TableBuilder中的API是有一定的调用逻辑的，整体来说是：
// Add() -> Flush() -> WriteBlock() -> WriteRawBlock() -> Finish()
// 1.
// Add()方法用于向Table中添加键值对，要求key是按照Comparator排序的，即key是递增的。
// 2. 当键值对的数量达到一个阈值时，调用Flush()方法，将kv对放到WritableFile中。
// 3. 接着调用WriteBlock()方法，将Data Block写入WritableFile中。

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

// 用于构建一个SSTable(Sorted String Table)的Table(即SSTable)的接口。
// 因此不难理解，在一个TableBuilder中会包含多个BlockBuilder，用于构建
// Data Block、Filter Block、Data Index Block、Filter Index Block这些，
// 最后加上Footer，就构成了一个完整的SSTable。
// 注意一个SSTable只有一个Index Block，一个filter block，一个meta index block，
// 只有Data Block的数量会有多个。
class LEVELDB_EXPORT TableBuilder {
 public:
  // Create a builder that will store the contents of the table it is
  // building in *file.  Does not close the file.  It is up to the
  // caller to close the file after calling Finish().
  // 创建一个TableBuilder对象，用于存储正在*file中构建的Table的内容。
  // 不要关闭文件，调用Finish()之后，由调用者决定是否关闭文件。
  // WritableFile通常为PosixWritableFile
  TableBuilder(const Options& options, WritableFile* file);

  // 同样禁用拷贝构造函数和赋值运算符
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: Either Finish() or Abandon() has been called.
  // 只有在调用Finish()或Abandon()之后才能销毁TableBuilder对象
  ~TableBuilder();

  // Change the options used by this builder.  Note: only some of the
  // option fields can be changed after construction.  If a field is
  // not allowed to change dynamically and its value in the structure
  // passed to the constructor is different from its value in the
  // structure passed to this method, this method will return an error
  // without changing any fields.
  // 更改此构建器使用的选项。注意：只有在构造之后才能更改一些选项字段。
  // 如果一个字段不允许动态更改，并且在传递给构造函数的结构中的值与传递给此方法的结构中的值不同，
  // 则此方法将返回错误，而不更改任何字段。
  Status ChangeOptions(const Options& options);

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  // 向正在构建的Table中添加键值对。在添加的过程中，除了总大小到达阈值后会生成一个Data
  // Block， filter block与data index block也在被同步构建，每当一个Data
  // Block构建完成后，对应的会 在filter block中生成若干个filter
  // data,同样会在data index block中生成一个index entry。
  // 要求：根据比较器，key在之前添加的任何键之后。
  // 要求：Finish()或Abandon()未被调用。
  void Add(const Slice& key, const Slice& value);

  // Advanced operation: flush any buffered key/value pairs to file.
  // Can be used to ensure that two adjacent entries never live in
  // the same data block.  Most clients should not need to use this method.
  // REQUIRES: Finish(), Abandon() have not been called
  // 高级操作：将任何缓冲的键值对刷新到文件。可以用来确保两个相邻的条目永远不会存在于同一个
  // 数据块中。大多数客户端不需要使用此方法。
  // 要求：Finish()或Abandon()未被调用
  //
  // 意味着当前构建的Data Block的Data部分大小已经达到了阈值，需要将这个Data
  // Block写入文件，这需要经历
  // 压缩data部分、计算data部分CRC、将其序列化为Block格式、写入WritableFile、刷新到磁盘(Flush(),内核
  // 缓冲区->磁盘)、检查是否需要生成新的Filter Data等一系列操作。
  //
  // 最后，在开始写入下一个块的第一个key之前，根据新块中待写入的第一个key
  // 来生成上一个已完成的data block的filter entry的key。
  void Flush();

  // Return non-ok iff some error has been detected.
  // 返回非ok值，如果检测到某些错误
  Status status() const;

  // Finish building the table.  Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  // 完成构建Table。在此函数返回后，停止使用传递给构造函数的file对象。表示已经写入了所有的KV对，但注意这并不
  // 意味着所有的Data Block构建完毕，可能最后写入的这批数据并没有达到Data
  // Block的大小阈值，因此需要我们手动 调用Flush()方法，将最后的kv对也以一个Data
  // Block的形式写入文件。 之后，所有的Data
  // Block已经构建完毕，这也意味着与之同时构建的filter block、data index block
  // 也已经构建完毕，只剩下Footer和meta index
  // block没有构建。而Finish()方法就是负责收尾工作，同时 也会构建Footer和meta
  // index block，从而形成一个完整的SSTable。 写入数据的顺序严格按照：Data Block
  // -> Filter Block -> meta index block -> data index block -> Footer。
  // 要求：Finish()或Abandon()未被调用
  Status Finish();

  // Indicate that the contents of this builder should be abandoned.  Stops
  // using the file passed to the constructor after this function returns.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  // 表示应放弃此builder的内容。在此函数返回后，停止使用传递给构造函数的文件。
  // 如果调用者不打算调用Finish()，则必须在销毁此builder之前调用Abandon()。
  // 要求：Finish()或Abandon()未被调用
  void Abandon();

  // Number of calls to Add() so far.
  // 到目前为止调用Add()的次数
  uint64_t NumEntries() const;

  // Size of the file generated so far.  If invoked after a successful
  // Finish() call, returns the size of the final generated file.
  // 到目前为止生成的文件大小。如果在成功调用Finish()之后调用，则返回最终生成文件的大小。
  uint64_t FileSize() const;

 private:
  bool ok() const { return status().ok(); }
  // 序列化当前需要写入的Data Block，同时对Data Block进行压缩，
  // 若压缩率低于options_.compression_ratio，则不压缩，直接存储原数据；之后调用
  // WriteRawBlock()将Data Block写入WritableFile中
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  // 在WriteBlock()中调用，用于将压缩后的Data Block的Data部分，并为data +
  // type生成 对应的CRC后，按照Block的格式将其写入WritableFile中
  void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

  // Rep的作用是隐藏TableBuilder的实现细节，具体的实现细节在Rep中
  struct Rep;
  Rep* rep_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
