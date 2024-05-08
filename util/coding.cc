// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

namespace leveldb {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

// 将v值用变长编码方式存储到dst中，返回值是dst的尾部指针
char* EncodeVarint32(char* dst, uint32_t v) {
  // Operate on characters as unsigneds
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;
  if (v < (1 << 7)) { // 若v小于128，则直接存储
    *(ptr++) = v; // 在ptr指向的位置存储v，之后ptr指向下一个位置
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B; // 在ptr指向的位置存储v | B，即v的低7位存储在ptr指向的位置，高1位存储在ptr指向的位置的最高位，该字节的MSB自然地被置为1，说明后面还有数据
    *(ptr++) = v >> 7; // 在ptr指向的位置存储v >> 7，即v的高7位存储在ptr指向的位置，该字节的MSB自然地被置为0，说明后面没有数据了
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}

// 将v值用变长编码方式存储到dst中
void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  // 获取v的变长编码，存储到buf中，最终返回的的是buf的尾部指针
  char* ptr = EncodeVarint32(buf, v);
  // 将v的变长编码追加到dst中
  dst->append(buf, ptr - buf);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  static const int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= B) {
    *(ptr++) = v | B;
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, ptr - buf);
}

// 将value的长度值进行变长编码后追加到dst中，然后将value的内容追加到dst中
void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, value.size());
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    len++;
  }
  return len;
}

// 当value只用一个字节存储时，value的最高位为0，使用GetVarint32Ptr解析即可
// 当value用多个字节存储时，value的最高位为1，使用GetVarint32PtrFallback解析

// 这里要说明一点，多字节的变长编码在排序每个字节的时候用的是小端存储，即最低有效字节在最前面
// 单个字节该怎么存就怎么存，不用考虑大小端问题，别混淆了
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  // 4个字节存储value，每个字节的低7位存储value的一部分，最高1位表示后面是否还有数据
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) { // 若byte的最高位为1，则说明后面还有数据
      // More bytes are present
      result |= ((byte & 127) << shift); // 取byte的数据部分(低7位)，然后左移shift位(相当于将数据部分放到正确的位置)
    } else { // 若byte的最高位为0，则说明后面没有数据了
      result |= (byte << shift);
      *value = result;
      // p指向下一个待操作的字节
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  if (GetVarint32(input, &len) && input->size() >= len) {
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb
