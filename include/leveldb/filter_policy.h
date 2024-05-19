// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in leveldb and are consulted
// automatically by leveldb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
// 一个数据库可以配置一个自定义的FilterPolicy对象。这个对象负责从一组键中创建一个小的过滤器。
// 这些过滤器存储在leveldb中，并且leveldb会自动查询这些过滤器，以决定是否从磁盘读取一些信息。
// 在许多情况下，过滤器可以将每个DB::Get()调用的磁盘查找次数从几个减少到一个。
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).
// 大多数人都希望使用内置的布隆过滤器支持(请参阅下面的NewBloomFilterPolicy())。

#ifndef STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
#define STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

class LEVELDB_EXPORT FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  // 返回此策略的名称。请注意，如果过滤器编码以不兼容的方式更改，则此方法返回的名称必须更改。
  // 否则，可能会将旧的不兼容过滤器传递给此类型的方法。
  virtual const char* Name() const = 0;

  // keys[0,n-1] contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys[0,n-1] to *dst.
  // keys[0,n-1]包含一个键列表(可能包含重复项)，这些键根据用户提供的比较器进行排序。
  // 把涵盖了keys[0,n-1]的过滤器附加到*dst。
  //
  // Warning: do not change the initial contents of *dst.  Instead,
  // append the newly constructed filter to *dst.
  // 警告:不要更改*dst的初始内容。相反，将新构造的过滤器附加到*dst。
  // TODO:生成的原理需要看布隆过滤器的原理，这部分到后面具体学习布隆过滤器时再来看
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class.  This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  // “filter”包含在此类上的先前调用CreateFilter()附加的数据。
  // 如果键在传递给CreateFilter()的键列表中，则此方法必须返回true。
  // 如果键不在列表中，则此方法可能返回true或false，但它应该以很高的概率返回false。
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key.  A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
// 返回一个新的过滤策略，该策略使用一个布隆过滤器，每个键大约有指定数量的位。
// bits_per_key的一个很好的值是10，这将产生一个大约1%的误报率的过滤器。
//
// Callers must delete the result after any database that is using the
// result has been closed.
// 当任何使用结果的数据库已关闭后，调用者必须删除结果。
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
// 注意:如果您使用的是忽略正在比较的键的某些部分的自定义比较器，则不得使用NewBloomFilterPolicy()，
// 并且必须提供自己的FilterPolicy，该FilterPolicy也忽略键的相应部分。
// 例如，如果比较器忽略尾部的空格，则使用不忽略键中的尾部空格的FilterPolicy(如NewBloomFilterPolicy)是不正确的。
LEVELDB_EXPORT const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
