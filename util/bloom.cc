// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"

#include "util/hash.h"

namespace leveldb {

namespace {
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    // 我们故意向下舍入，以减少探测成本，根据公式k = ln(2) *
    // bits_per_key，计算出k的值。bits_per_key是每个key占用的bit数。
    // 假设有一共有m bit，该过滤器需要过滤n个key，那么bits_per_key = m / n。
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    // k_是每个键无论读写都要经过的哈希函数的个数，最小值为1，最大值为30。
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  // 返回过滤器的名称，SSTable中的meta index
  // block中存储一条kv记录，key就是这个名称，value就是filter block的block
  // handle。
  const char* Name() const override { return "leveldb.BuiltinBloomFilter2"; }

  // 创建一个过滤器，keys是需要过滤的key的集合，n是keys的个数，dst存储生成的过滤器。
  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // Compute bloom filter size (in both bits and bytes)
    // 计算该过滤器的bit数组的大小，bits数量
    size_t bits = n * bits_per_key_;

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    // 对于小的n，我们可能会看到非常高的误报率。通过强制最小布隆过滤器长度来修复它。
    if (bits < 64) bits = 64;

    // 转化为字节数，这里+7是为了向上取整，因为一个字节有8位。
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    // 这批keys预计占用bytes个字节，所以先将dst的大小设置为bytes，然后将dst的内容全部设置为0。
    // 在原有的内容上再扩容增加bytes个字节，然后将新增的bytes个字节全部设置为0。
    const size_t init_size = dst->size();
    // 将dst大小新增bytes个字节，然后将新增的bytes个字节全部设置为0。
    dst->resize(init_size + bytes, 0);
    // 将该过滤器的k_(哈希函数个数)写入dst中，用于后续的读取。
    dst->push_back(static_cast<char>(k_));
    // 将dst的地址赋值给array，然后将keys中的每个key都经过k_个哈希函数，然后将对应的bit位置设置为1。
    // 这里对应了BloomFilter写入元素的过程。
    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      // 使用双哈希生成一系列哈希值，参见[Kirsch，Mitzenmacher 2006]中的分析。
      // 对当前key生成初始化哈希值
      uint32_t h = BloomHash(keys[i]);
      // 计算增量delta，delta是h右移17位或左移15位的结果
      // 用于双哈希法的哈希值生成
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      // 当前bloom filter需要对每个key经过k_个哈希函数，这里通过将h不断增加delta得到新的哈希值来模拟k_个哈希函数的运算结果
      // 这种方法的理论来自于[Kirsch，Mitzenmacher 2006]中的分析
      for (size_t j = 0; j < k_; j++) {
        // 计算哈希值对应的bit位置
        const uint32_t bitpos = h % bits;
        // 将对应bit位置1，使用bit操作
        // bitpos/8代表该bit在数组中属于哪个字节，1 << (bitpos % 8)代表该bit在字节中的位置，
        // 通过或操作将该key对应的bit位设置为1，这样不会影响原来的结果
        array[bitpos / 8] |= (1 << (bitpos % 8));
        // 增加h值，为下一次迭代计算新的哈希值
        h += delta;
      }
    }
  }

bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
  const size_t len = bloom_filter.size();
  // 如果长度小于2，说明过滤器无效，直接返回false
  if (len < 2) return false;

  const char* array = bloom_filter.data();
  // 计算该过滤器的bit数组占用的bit数
  const size_t bits = (len - 1) * 8;
  // 每个filter data的尾部存储了该过滤器的k_值，即使用的哈希函数个数
  // 意味着对该过滤器进行读写操作都需要经过k_个哈希函数
  const size_t k = array[len - 1];
  
  // Use the encoded k so that we can read filters generated by
  // bloom filters created using different parameters.
  // 使用编码的k，以便我们可以读取使用不同参数创建的布隆过滤器生成的过滤器。
  // 每个过滤器都可以根据实际情况来决定使用的哈希函数个数
  if (k > 30) {
    // 保留用于可能的新编码格式的短布隆过滤器，认为是匹配的
    // 在CreateFilter中，k_的范围被限制在[1, 30]之间，因此当前不会进入这个分支
    // Reserved for potentially new encodings for short bloom filters.
    // Consider it a match.
    return true;
  }

  // 下面的比较过程与CreateFilter中的写入过程对应，使用BloomHash和delta来模拟k_个哈希函数的运算结果
  // 对key生成初始哈希值
  uint32_t h = BloomHash(key);
  // 计算增量，通过将h右旋转17位或左旋转15位来得到
  const uint32_t delta = (h >> 17) | (h << 15);  // 右旋转17位
  
  // 对布隆过滤器的每个哈希函数进行迭代
  for (size_t j = 0; j < k; j++) {
    // 计算当前哈希值对应的bit位置
    const uint32_t bitpos = h % bits;    
    // 检查bitpos位置的bit是否为0，如果是0，说明不匹配
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    // 增加h值，为下一次迭代计算新的哈希值
    h += delta;
  }

  // 所有bit位置都匹配，返回true
  return true;
}

 private:
  // bits_per_key_是每个key占用的bit数，在已知有比特数组大小为m
  // 个比特位，需要过滤n个key的情况下，bits_per_key_ = m / n。
  size_t bits_per_key_;
  // k_是每个键无论读写都要经过的哈希函数的个数，最小值为1，最大值为30。
  size_t k_;
};
}  // namespace

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
