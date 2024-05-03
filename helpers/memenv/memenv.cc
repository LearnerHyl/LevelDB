// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "helpers/memenv/memenv.h"

#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "leveldb/env.h"
#include "leveldb/status.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/mutexlock.h"

namespace leveldb {

namespace {

// FileState是引用计数的，初始引用计数为0，调用者必须至少调用一次Ref()
// 调用者不能通过delete删除FileState对象，而是通过引用计数的方式实现资源的自动释放
class FileState {
 public:
  // FileStates are reference counted. The initial reference count is zero
  // and the caller must call Ref() at least once.
  FileState() : refs_(0), size_(0) {}

  // No copying allowed.
  // 不允许拷贝，因此禁止拷贝构造函数和赋值操作符
  FileState(const FileState&) = delete;
  FileState& operator=(const FileState&) = delete;

  // Increase the reference count.
  void Ref() {
    MutexLock lock(&refs_mutex_);
    ++refs_;
  }

  // Decrease the reference count. Delete if this is the last reference.
  void Unref() {
    bool do_delete = false;

    {
      MutexLock lock(&refs_mutex_);
      --refs_;
      assert(refs_ >= 0);
      if (refs_ <= 0) {
        do_delete = true;
      }
    }

    if (do_delete) {
      delete this;
    }
  }

  uint64_t Size() const {
    MutexLock lock(&blocks_mutex_);
    return size_;
  }

  void Truncate() {
    MutexLock lock(&blocks_mutex_);
    for (char*& block : blocks_) {
      delete[] block;
    }
    blocks_.clear();
    size_ = 0;
  }

  // 从文件对象的offset位置开始读取n个字节到result中，result一般指向scratch缓冲区
  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const {
    MutexLock lock(&blocks_mutex_);
    if (offset > size_) {
      return Status::IOError("Offset greater than file size.");
    }
    const uint64_t available = size_ - offset;
    // offset处正好是文件末尾，返回空Slice
    if (n > available) {
      n = static_cast<size_t>(available);
    }
    if (n == 0) {
      *result = Slice();
      return Status::OK();
    }

    assert(offset / kBlockSize <= std::numeric_limits<size_t>::max());
    size_t block = static_cast<size_t>(offset / kBlockSize); // block是offset所在的block的索引
    size_t block_offset = offset % kBlockSize; // block_offset是offset在block中的偏移量
    size_t bytes_to_copy = n; // bytes_to_copy是需要拷贝的字节数
    char* dst = scratch;

    // 将blocks_[block]中从block_offset开始，拷贝bytes_to_copy个字节到dst中
    while (bytes_to_copy > 0) {
      size_t avail = kBlockSize - block_offset;
      if (avail > bytes_to_copy) {
        avail = bytes_to_copy;
      }
      std::memcpy(dst, blocks_[block] + block_offset, avail);

      bytes_to_copy -= avail;
      dst += avail;
      block++;
      block_offset = 0;
    }

    // 构造一个Slice对象，指向scratch缓冲区中的数据
    *result = Slice(scratch, n);
    return Status::OK();
  }

  // 将data追加到文件对象的末尾
  Status Append(const Slice& data) {
    const char* src = data.data(); // src指向data的数据
    size_t src_len = data.size(); // src_len是data的长度

    MutexLock lock(&blocks_mutex_);
    while (src_len > 0) {
      size_t avail;
      size_t offset = size_ % kBlockSize;

      // 检查最后一个block是否有剩余空间，如果有，avail为剩余空间大小，否则新建一个block
      if (offset != 0) {
        // There is some room in the last block.
        avail = kBlockSize - offset;
      } else {
        // No room in the last block; push new one.
        blocks_.push_back(new char[kBlockSize]);
        avail = kBlockSize;
      }

      if (avail > src_len) {
        avail = src_len;
      }
      // 将src中的avail个字节拷贝到blocks_的最后一个block中，并更新相关变量
      std::memcpy(blocks_.back() + offset, src, avail);
      src_len -= avail;
      src += avail;
      size_ += avail;
    }

    return Status::OK();
  }

 private:
  enum { kBlockSize = 8 * 1024 }; // 每个block的大小，对应于下面的blocks_数组中的每个元素

  // Private since only Unref() should be used to delete it.
  ~FileState() { Truncate(); }

  port::Mutex refs_mutex_;
  int refs_ GUARDED_BY(refs_mutex_);

  mutable port::Mutex blocks_mutex_;
  std::vector<char*> blocks_ GUARDED_BY(blocks_mutex_); // blocks_是一个char*数组，每个元素指向一个block
  uint64_t size_ GUARDED_BY(blocks_mutex_); // size_是文件的总大小
};

class SequentialFileImpl : public SequentialFile {
 public:
  explicit SequentialFileImpl(FileState* file) : file_(file), pos_(0) {
    file_->Ref();
  }

  ~SequentialFileImpl() override { file_->Unref(); }

  Status Read(size_t n, Slice* result, char* scratch) override {
    Status s = file_->Read(pos_, n, result, scratch);
    if (s.ok()) {
      pos_ += result->size();
    }
    return s;
  }

  Status Skip(uint64_t n) override {
    if (pos_ > file_->Size()) {
      return Status::IOError("pos_ > file_->Size()");
    }
    const uint64_t available = file_->Size() - pos_;
    if (n > available) {
      n = available;
    }
    pos_ += n;
    return Status::OK();
  }

 private:
  FileState* file_;
  uint64_t pos_;
};

class RandomAccessFileImpl : public RandomAccessFile {
 public:
  explicit RandomAccessFileImpl(FileState* file) : file_(file) { file_->Ref(); }

  ~RandomAccessFileImpl() override { file_->Unref(); }

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    return file_->Read(offset, n, result, scratch);
  }

 private:
  FileState* file_;
};

class WritableFileImpl : public WritableFile {
 public:
  WritableFileImpl(FileState* file) : file_(file) { file_->Ref(); }

  ~WritableFileImpl() override { file_->Unref(); }

  Status Append(const Slice& data) override { return file_->Append(data); }

  Status Close() override { return Status::OK(); }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }

 private:
  FileState* file_;
};

class NoOpLogger : public Logger {
 public:
  void Logv(const char* format, std::va_list ap) override {}
};

// 基于EnvWrapper的派生类，很容易实现一个在某一个Env派生类的基础上改写其中一部分接口的需求
// InMemoryEnv主要override了和文件操作相关的接口
class InMemoryEnv : public EnvWrapper {
 public:
  explicit InMemoryEnv(Env* base_env) : EnvWrapper(base_env) {}

  ~InMemoryEnv() override {
    for (const auto& kvp : file_map_) {
      kvp.second->Unref();
    }
  }

  // Partial implementation of the Env interface.
  Status NewSequentialFile(const std::string& fname,
                           SequentialFile** result) override {
    MutexLock lock(&mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      *result = nullptr;
      return Status::IOError(fname, "File not found");
    }

    *result = new SequentialFileImpl(file_map_[fname]);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             RandomAccessFile** result) override {
    MutexLock lock(&mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      *result = nullptr;
      return Status::IOError(fname, "File not found");
    }

    *result = new RandomAccessFileImpl(file_map_[fname]);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         WritableFile** result) override {
    MutexLock lock(&mutex_);
    FileSystem::iterator it = file_map_.find(fname);

    FileState* file;
    if (it == file_map_.end()) {
      // File is not currently open.
      file = new FileState();
      file->Ref();
      file_map_[fname] = file;
    } else {
      file = it->second;
      file->Truncate();
    }

    *result = new WritableFileImpl(file);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           WritableFile** result) override {
    MutexLock lock(&mutex_);
    FileState** sptr = &file_map_[fname];
    FileState* file = *sptr;
    if (file == nullptr) {
      file = new FileState();
      file->Ref();
    }
    *result = new WritableFileImpl(file);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    MutexLock lock(&mutex_);
    return file_map_.find(fname) != file_map_.end();
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    MutexLock lock(&mutex_);
    result->clear();

    for (const auto& kvp : file_map_) {
      const std::string& filename = kvp.first;

      if (filename.size() >= dir.size() + 1 && filename[dir.size()] == '/' &&
          Slice(filename).starts_with(Slice(dir))) {
        result->push_back(filename.substr(dir.size() + 1));
      }
    }

    return Status::OK();
  }

  void RemoveFileInternal(const std::string& fname)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    if (file_map_.find(fname) == file_map_.end()) {
      return;
    }

    file_map_[fname]->Unref();
    file_map_.erase(fname);
  }

  Status RemoveFile(const std::string& fname) override {
    MutexLock lock(&mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      return Status::IOError(fname, "File not found");
    }

    RemoveFileInternal(fname);
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override { return Status::OK(); }

  Status RemoveDir(const std::string& dirname) override { return Status::OK(); }

  Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    MutexLock lock(&mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      return Status::IOError(fname, "File not found");
    }

    *file_size = file_map_[fname]->Size();
    return Status::OK();
  }

  Status RenameFile(const std::string& src,
                    const std::string& target) override {
    MutexLock lock(&mutex_);
    if (file_map_.find(src) == file_map_.end()) {
      return Status::IOError(src, "File not found");
    }

    RemoveFileInternal(target);
    file_map_[target] = file_map_[src];
    file_map_.erase(src);
    return Status::OK();
  }

  Status LockFile(const std::string& fname, FileLock** lock) override {
    *lock = new FileLock;
    return Status::OK();
  }

  Status UnlockFile(FileLock* lock) override {
    delete lock;
    return Status::OK();
  }

  Status GetTestDirectory(std::string* path) override {
    *path = "/test";
    return Status::OK();
  }

  Status NewLogger(const std::string& fname, Logger** result) override {
    *result = new NoOpLogger;
    return Status::OK();
  }

 private:
  // Map from filenames to FileState objects, representing a simple file system.
  typedef std::map<std::string, FileState*> FileSystem;

  port::Mutex mutex_;
  FileSystem file_map_ GUARDED_BY(mutex_);
};

}  // namespace

Env* NewMemEnv(Env* base_env) { return new InMemoryEnv(base_env); }

}  // namespace leveldb
