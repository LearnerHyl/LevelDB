// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_MERGER_H_
#define STORAGE_LEVELDB_TABLE_MERGER_H_

namespace leveldb {

class Comparator;
class Iterator;

// Return an iterator that provided the union of the data in
// children[0,n-1].  Takes ownership of the child iterators and
// will delete them when the result iterator is deleted.
// 返回一个迭代器，该迭代器提供了children[0,n-1]中数据的并集。接管了子迭代器的所有权，并在结果迭代器被删除时删除它们。
//
// The result does no duplicate suppression.  I.e., if a particular
// key is present in K child iterators, it will be yielded K times.
// 结果不会去重。也就是说，如果一个特定的key在K个子迭代器中存在，那么它将被产生K次。
//
// REQUIRES: n >= 0
Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children,
                             int n);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_MERGER_H_
