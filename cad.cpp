/**
 * 当向Memtable或者Immutable Memtable、SSTable查找数据时，是构造了一个LookupKey对象，然后调用Get()方法进行查找的。
 * Lookupkey = InternalKey + SequenceNumber。而sequenceNumber是来自Get()方法选项参数中的snapshot。
*/