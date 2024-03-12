// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_
#define STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_

#include "leveldb/export.h"

namespace leveldb {

class Env;

// Returns a new environment that stores its data in memory and delegates
// all non-file-storage tasks to base_env. The caller must delete the result
// when it is no longer needed.
// *base_env must remain live while the result is in use.
// 返回一个新的Env对象，它将数据存储在内存中，并将所有非文件存储任务委托给base_env。
// 当不再需要结果时，调用者必须删除结果。
// *base_env必须在结果使用期间保持活动。
LEVELDB_EXPORT Env* NewMemEnv(Env* base_env);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_HELPERS_MEMENV_MEMENV_H_
