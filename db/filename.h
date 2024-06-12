// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// File names used by DB code

#ifndef STORAGE_LEVELDB_DB_FILENAME_H_
#define STORAGE_LEVELDB_DB_FILENAME_H_

#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "port/port.h"

namespace leveldb {

class Env;

/**
 * LevelDB有两种日志文件，KLogFile负责保存WAL日志，具体的操作日志。
 * kDescriptorFile也是保存的日志，日志格式与WAL相同，只是Content内容字段
 * 用于记录每次Compaction操作产生的VersionEdit信息。
 */

enum FileType {
  kLogFile, // WAL日志文件，文件名为[0-9]+.log
  kDBLockFile, // db锁文件，文明名为LOCK，通过LOCK文件加文件锁（flock）来实现只有一个实例能操作db
  kTableFile, // sstable文件，文件名为[0-9]+.sst
  // db元数据文件，存储系统中version信息，文件名为MANIFEST-[0-9]+，每当db发生compaction时，
  // 对应的versionedit会记录到descriptor文件中
  kDescriptorFile,
  // 记录当前使用的descriptor文件名，文件名为CURRENT
  kCurrentFile,
  // 临时文件，db在修复过程中会产生临时文件，文件名为[0-9]+.dbtmp
  kTempFile,
  // db运行过程中的日志文件，文件名为LOG
  kInfoLogFile  // Either the current one, or an old one
};

// Return the name of the log file with the specified number
// in the db named by "dbname".  The result will be prefixed with
// "dbname".
std::string LogFileName(const std::string& dbname, uint64_t number);

// Return the name of the sstable with the specified number
// in the db named by "dbname".  The result will be prefixed with
// "dbname".
std::string TableFileName(const std::string& dbname, uint64_t number);

// Return the legacy file name for an sstable with the specified number
// in the db named by "dbname". The result will be prefixed with
// "dbname".
std::string SSTTableFileName(const std::string& dbname, uint64_t number);

// Return the name of the descriptor file for the db named by
// "dbname" and the specified incarnation number.  The result will be
// prefixed with "dbname".
std::string DescriptorFileName(const std::string& dbname, uint64_t number);

// Return the name of the current file.  This file contains the name
// of the current manifest file.  The result will be prefixed with
// "dbname".
std::string CurrentFileName(const std::string& dbname);

// Return the name of the lock file for the db named by
// "dbname".  The result will be prefixed with "dbname".
std::string LockFileName(const std::string& dbname);

// Return the name of a temporary file owned by the db named "dbname".
// The result will be prefixed with "dbname".
std::string TempFileName(const std::string& dbname, uint64_t number);

// Return the name of the info log file for "dbname".
std::string InfoLogFileName(const std::string& dbname);

// Return the name of the old info log file for "dbname".
std::string OldInfoLogFileName(const std::string& dbname);

// If filename is a leveldb file, store the type of the file in *type.
// The number encoded in the filename is stored in *number.  If the
// filename was successfully parsed, returns true.  Else return false.
// 如果文件名是一个leveldb文件，将文件的类型存储在*type中。
// 文件名中编码的数字存储在*number中。如果成功解析文件名，则返回true。
// 否则返回false。
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type);

// Make the CURRENT file point to the descriptor file with the
// specified number.
Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_FILENAME_H_
