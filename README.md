# LevelDB

LevelDB结构简单,不依赖于任何第三方库,具有很好的独立,虽然其有针对性的对BigTable做了一定程度的简化,然而BigTable的主要技术思想与数据结构都体现在了LevelDB中。因此,LevelDB可以看作是BigTable的简化版或单机版。

通过阅读LevelDB源码，加深对LevelDB的理解，相关注释都加到了源码上。下面是阅读的目录，方便后续复习。

# 基本数据结构

## Slice

重点了解为什么使用Slice而不使用String。

## Comparator

Comparator本身是一个纯虚类（类似于Go中的Interface），可以以子类BytewiseComparatorImpl和InternalKeyComparator为例进行开始阅读，其中AdvanceFucntions部分可能刚开始不知道是做什么用的，后面读到LevelDB的一些功能后，就知道其适用场景了。

## Iterator

LevelDB的Iterator十分强大，同样Iterator本身是一个纯虚类，唯一的非虚函数是CleanUp函数，用于回收迭代器的资源，由Iterator的析构函数调用相关cleanup函数实现资源的释放与清除。

## 系统参数（TODO）

详情请见头文件options.h中的内容。里面介绍了一些数据压缩方式，以及在使用DB::Open启动数据库实例时可以指定的一些参数。

