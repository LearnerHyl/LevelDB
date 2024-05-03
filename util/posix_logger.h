// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Logger implementation that can be shared by all environments
// where enough posix functionality is available.

#ifndef STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
#define STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_

#include <sys/time.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <thread>

#include "leveldb/env.h"

namespace leveldb {

// A logger that writes to a file descriptor.
// PosixLogger用来记录线程操作的日志，日志的格式见下。
class PosixLogger final : public Logger {
 public:
  // Creates a logger that writes to the given file.
  //
  // The PosixLogger instance takes ownership of the file handle.
  explicit PosixLogger(std::FILE* fp) : fp_(fp) { assert(fp != nullptr); }

  ~PosixLogger() override { std::fclose(fp_); }

  void Logv(const char* format, std::va_list arguments) override {
    // Record the time as close to the Logv() call as possible.
    // 一旦调用Logv()，就记录当前时间。
    struct ::timeval now_timeval;
    ::gettimeofday(&now_timeval, nullptr);
    const std::time_t now_seconds = now_timeval.tv_sec;
    struct std::tm now_components;
    ::localtime_r(&now_seconds, &now_components);

    // Record the thread ID.
    // 记录当前线程的ID。
    constexpr const int kMaxThreadIdSize = 32;
    std::ostringstream thread_stream;
    thread_stream << std::this_thread::get_id();
    std::string thread_id = thread_stream.str();
    if (thread_id.size() > kMaxThreadIdSize) {
      thread_id.resize(kMaxThreadIdSize);
    }
    /**
     * 日志保存原则：首先采用一个512字节的栈内存，如果栈内存不够，再采用动态内存。
    */

    // We first attempt to print into a stack-allocated buffer. If this attempt
    // fails, we make a second attempt with a dynamically allocated buffer.
    constexpr const int kStackBufferSize = 512;
    char stack_buffer[kStackBufferSize];
    static_assert(sizeof(stack_buffer) == static_cast<size_t>(kStackBufferSize),
                  "sizeof(char) is expected to be 1 in C++");

    int dynamic_buffer_size = 0;  // Computed in the first iteration.
    // 两次循环，第一次使用栈内存，第二次使用动态内存。
    for (int iteration = 0; iteration < 2; ++iteration) {
      const int buffer_size =
          (iteration == 0) ? kStackBufferSize : dynamic_buffer_size;
      char* const buffer =
          (iteration == 0) ? stack_buffer : new char[dynamic_buffer_size];

      // Print the header into the buffer.
      // 打印日志头部信息，包括日期、时间、线程ID。
      int buffer_offset = std::snprintf(
          buffer, buffer_size, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %s ",
          now_components.tm_year + 1900, now_components.tm_mon + 1,
          now_components.tm_mday, now_components.tm_hour, now_components.tm_min,
          now_components.tm_sec, static_cast<int>(now_timeval.tv_usec),
          thread_id.c_str());

      // The header can be at most 28 characters (10 date + 15 time +
      // 3 delimiters) plus the thread ID, which should fit comfortably into the
      // static buffer.
      // 28个字符是日期、时间、线程ID的长度，加上3个分隔符。这个长度应该可以放到栈内存中。
      assert(buffer_offset <= 28 + kMaxThreadIdSize);
      static_assert(28 + kMaxThreadIdSize < kStackBufferSize,
                    "stack-allocated buffer may not fit the message header");
      assert(buffer_offset < buffer_size);

      // Print the message into the buffer.
      // 将日志信息写入buffer。
      std::va_list arguments_copy;
      va_copy(arguments_copy, arguments);
      buffer_offset +=
          std::vsnprintf(buffer + buffer_offset, buffer_size - buffer_offset,
                         format, arguments_copy);
      va_end(arguments_copy);

      // The code below may append a newline at the end of the buffer, which
      // requires an extra character.
      // 下面的代码可能在buffer的末尾追加一个新行，所以需要一个额外的字符。
      if (buffer_offset >= buffer_size - 1) {
        // The message did not fit into the buffer.
        // 意味着这条日志信息大于栈buffer的大小(512字节)
        if (iteration == 0) {
          // Re-run the loop and use a dynamically-allocated buffer. The buffer
          // will be large enough for the log message, an extra newline and a
          // null terminator.
          // 重新运行循环，并使用动态分配的缓冲区。缓冲区将足够大，以容纳日志消息、额外的换行符和空终止符。
          dynamic_buffer_size = buffer_offset + 2;
          continue;
        }

        // The dynamically-allocated buffer was incorrectly sized. This should
        // not happen, assuming a correct implementation of std::(v)snprintf.
        // Fail in tests, recover by truncating the log message in production.
        // 动态分配的缓冲区大小错误，这不应该发生，假设std::(v)snprintf的实现是正确的。
        // 在测试中失败，在生产中通过截断日志消息来恢复。
        assert(false);
        buffer_offset = buffer_size - 1;
      }

      // Add a newline if necessary.
      if (buffer[buffer_offset - 1] != '\n') {
        buffer[buffer_offset] = '\n';
        ++buffer_offset;
      }

      assert(buffer_offset <= buffer_size);
      std::fwrite(buffer, 1, buffer_offset, fp_);
      std::fflush(fp_);

      if (iteration != 0) {
        delete[] buffer;
      }
      break;
    }
  }

 private:
  std::FILE* const fp_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
