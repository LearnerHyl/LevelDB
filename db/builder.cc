// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

/**
 * 根据传入的MemTable迭代器以及相关的元数据信息，构建一个SSTable文件
 * @param dbname 数据库名称
 * @param env 环境变量
 * @param options 选项
 * @param table_cache TableCache对象
 * @param iter MemTable迭代器，实际上是一个SkipList的迭代器
 * @param meta 文件元数据
*/
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  // 获取SSTable文件名
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) { // 如果迭代器有效
    WritableFile* file;
    // 创建一个新的SSTable文件
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    
    // 创建一个TableBuilder对象，用于构建SSTable文件
    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->key());
    Slice key;
    // 遍历MemTable迭代器，依次将每个键值对添加到TableBuilder中
    for (; iter->Valid(); iter->Next()) {
      key = iter->key();
      builder->Add(key, iter->value());
    }
    if (!key.empty()) {
      meta->largest.DecodeFrom(key);
    }

    // Finish and check for builder errors
    // 完成TableBuilder的构建，这一步实际生成了SSTable文件
    s = builder->Finish();
    if (s.ok()) {
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok()) { // 若SSTable文件构建成功，则调用sync方法将用户态的数据直接刷写到磁盘
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
    delete file;
    file = nullptr;

    if (s.ok()) { // 若SSTable文件构建成功，则将其加入到TableCache中
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) { // 检查MemTable迭代器是否有错误
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else { // 若SSTable文件构建失败，则删除该文件，确保数据库的一致性
    env->RemoveFile(fname);
  }
  return s;
}

}  // namespace leveldb
