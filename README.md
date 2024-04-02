# LevelDB

LevelDB结构简单,不依赖于任何第三方库,具有很好的独立,虽然其有针对性的对BigTable做了一定程度的简化,然而BigTable的主要技术思想与数据结构都体现在了LevelDB中。因此,LevelDB可以看作是BigTable的简化版或单机版。

# 基本数据结构

## Slice

重点了解为什么使用Slice而不使用String。

## Comparator

Comparator本身是一个纯虚类（类似于Go中的Interface），可以以子类BytewiseComparatorImpl和InternalKeyComparator为例进行开始阅读，其中AdvanceFucntions部分可能刚开始不知道是做什么用的，后面读到LevelDB的一些功能后，就知道其适用场景了。

## Iterator

LevelDB的Iterator十分强大，同样Iterator本身是一个纯虚类，唯一的非虚函数是CleanUp函数，用于回收迭代器的资源，由Iterator的析构函数调用相关cleanup函数实现资源的释放与清除。

## 系统参数

详情请见头文件options.h中的内容。里面介绍了一些数据压缩方式，以及在使用DB::Open启动数据库实例时可以指定的一些参数。具体分为如下几类参数：

- 影响DB行为的参数。
- 影响DB性能的参数。

这里有一个和DB::Write操作相关的sync参数需要提一下，sync\==false的DB写操作具有与“write()”系统调用相似的崩溃语义。sync==true的DB写操作具有与“write()”系统调用后的“fsync()”相似的崩溃语义。

传统的UNIX实现在内核中设有缓冲区高速缓存或页面高速缓存，大多数磁盘I/O都通过缓冲进行。当将数据写入文件时，内核通常先将该数据复制到其中一个缓冲区中，如果该缓冲区尚未写满，则并不将其排入输出队列，而是等待其写满或者当内核需要重用该缓冲区以便存放其他磁盘块数据时，再将该缓冲排入输出队列，然后待其到达队首时，才进行实际的I/O操作。这种输出方式被称为延迟写（delayed write）（Bach [1986]第3章详细讨论了缓冲区高速缓存）。

延迟写减少了磁盘读写次数，但是却降低了文件内容的更新速度，使得欲写到文件中的数据在一段时间内并没有写到磁盘上。当系统发生故障时，这种延迟可能造成文件更新内容的丢失。为了保证磁盘上实际文件系统与缓冲区高速缓存中内容的一致性，UNIX系统提供了sync、fsync和fdatasync三个函数。

sync函数只是将所有修改过的块缓冲区排入写队列，然后就返回，它并不等待实际写磁盘操作结束。
通常称为update的系统守护进程会周期性地（一般每隔30秒）调用sync函数。这就保证了定期冲洗内核的块缓冲区。命令sync(1)也调用sync函数。

fsync函数只对由文件描述符filedes指定的单一文件起作用，并且等待写磁盘操作结束，然后返回。fsync可用于数据库这样的应用程序，这种应用程序需要确保将修改过的块立即写到磁盘上。
fdatasync函数类似于fsync，但它只影响文件的数据部分。而除数据外，fsync还会同步更新文件的属性。

对于提供事务支持的数据库，在事务提交时，都要确保事务日志（包含该事务所有的修改操作以及一个提交记录）完全写到硬盘上，才认定事务提交成功并返回给应用层。

# LevelDB简介

## 源码结构

与数据库实现相关的数据结构和相关文件目录主要有如下几种：

- `class LEVELDB_EXPORT DB`：主要包含了数据库的一些基本接口操作和内部实现。一个DB对象是一个持久化的有序map，它将key映射到value。DB对象可以安全地被多个线程并发访问，而不需要外部同步机制。
- `class LEVELDB_EXPORT Table`：Table是SSTable(Sorted String Table)的主要实现载体。Table是一个有序的字符串到字符串的映射。Table是**不可变的**和**持久化**的。Table可以安全地被多个线程并发访问，而不需要外部同步机制。
- helpers目录：定义了将LevelDB稳定存储中的数据的介质变为内存环境的方法。（即将底层存储由磁盘介质完全变成内存），主要用于相关的测试场景或某些全内存的运行场景。
- util目录：包含一些通用的基础类函数，如内存管理、布隆过滤器、编码、CRC等相关函数。
- include目录：包含了LevelDB的库函数，可供外部访问的接口、以及一些基本的数据结构等等。
- port目录：定义了一个通用的底层文件，以及相关的多个进程操作接口，还有基于OS平台可移植性理念实现的各个OS平台相关的具体接口。

## 性能优化方案

### 启用压缩

**LevelDB的数据文件由一系列的压缩块组成**，每个块中存放了key相邻的键值对。读写操作均以块为单位进行。用户可以通过DB::Open中的Options参数改变块大小。块大小有利有弊，根据用户场景决定。

### 启用Cache

cache可以充分利用内存空间，将常用数据存到cache中，减少访问频率。此外，在使用Iterator进行访问的时候，用户可以自行规定本次访问是否会替换cache的数据。**缓存数据是以块为单位的。**

### 启用filter policy

FilterPolicy主要用于尽可能的减少度过程中磁盘I/O的操作次数，后续会详细介绍。

### key的命名设计

LevelDB中的键值对按照key的顺序进行存储。我们可以将经常同时访问的数据加上相同的前缀，从而将这些数据放到同一个块中，冷门数据放到别的块中，从而减少I/O次数。

# LevelDB总体架构与设计思想

## 主要模块功能

LevelDB是一个开源的键值存储库，具有高性能和轻量级的特点。其总体模块架构包括接口API、Utility公用基础类和LSM树三个主要部分，如下所述：

**接口API**：

- **DB API**：提供对数据库的基本操作接口，包括插入、删除、查询等功能。
- **POSIX API**：提供了对底层文件系统的基本操作接口，例如文件读写、文件定位等，用于实现数据库的持久化。

**Utility公用基础类**：

- 这些基础类提供了LevelDB内部实现所需的公共功能，例如内存管理、文件操作、日志记录、布隆过滤器、CRC校验、哈希表等。

**LSM树**（Log-Structured Merge-Tree）：

- **LOG**：日志结构组件，用于记录数据库的变更操作，提供了高效的写入能力。顺序写更快。
- **MemTable**：内存表，是一个基于内存的数据结构，用于存储最新的键值对，提供了快速的写入性能。
- **SSTable**（Sorted String Table）：有序字符串表，是将内存表中的数据持久化到磁盘的数据结构，采用了基于排序的方式，使得查询操作更加高效。

整个架构的设计使得LevelDB具有良好的写入性能和查询性能，并且能够有效地处理大规模数据集。

## 主要操作流程(Open、Get、Put、Delete)

LevelDB提供了很多API接口，这里选择Open()、Get()、Put()、Write()三种主要类型进行介绍。db.h中定义的是相关接口定义，而db_impl.h中的db_impl类是DB的一个子类，实现了包含上述4种等等的一些DB API，因此我们以db_impl类中的API定义去学习。

- db_impl.h中有DB类的一些默认实现，当然也包括上述4种操作，除了open()操作只有DB类定义了，剩下的操作db_impl和DB类都有，我们以db_impl为主。

### Manifest文件

**Manifest文件**在LevelDB中扮演着关键的角色。让我来详细解释一下：

1. **元数据存储**：Manifest文件保存了整个LevelDB实例的元数据，其中包括了每一层中存在哪些SSTable（Sorted String Table）。

2. **格式和记录**：实际上，Manifest文件就是一个日志文件，其中的每个日志记录都是一个VersionEdit。VersionEdit用于表示一次元数据的变更，例如新增、删除SSTable等操作。Manifest文件保存了VersionEdit序列化后的数据。

3. **VersionEdit**：VersionEdit记录了数据库元数据的变更，包括以下信息：
   - `comparator_`：比较器的名称，在创建LevelDB时确定，之后不可更改。
   - `log_number_`：最小的有效日志编号，    小于该编号的日志文件可以删除。
   - `prev_log_number_`：已废弃，代码保留用于兼容旧版本的LevelDB。
   - `next_file_number_`：下一个文件的编号。
   - `last_sequence_`：SSTable中的最大序列号。
   - `compact_pointers_`：记录每一层要进行下一次compaction的起始键。
   - `deleted_files_`：可以删除的SSTable（按层级和文件编号）。
   - `new_files_`：新增的SSTable。

4. **Version和VersionSet**：
   - **Version**：是应用`VersionEdit`后得到的数据库状态，即当前版本。它包含了哪些SSTable，并通过引用计数确保多线程并发访问的安全性。读操作需要在读取SSTable之前调用`Version::Ref`增加引用计数，不再使用时需要调用`Version::UnRef`减少引用计数。
   - **VersionSet**：是一组`Version`的集合。随着数据库状态的变化，LevelDB会生成新的VersionEdit，从而产生新的Version。同一时刻可能存在多个Version，VersionSet通过链表维护这些Version。

## LevelDB的写：DBImpl::write()

当某个写操作在刷盘的时候，可以暂时释放锁，因为&w当前负责记录日志，并保护记录操作免受并发的loggers和并发写入memtable的影响。刷盘之前已经完成了对相关共享变量的更新，此时可以释放锁，允许新的请求进来。

## 快照（Get操作的前置条件）

当向Memtable或者Immutable Memtable、SSTable查找数据时，是构造了一个LookupKey对象，然后调用Get()方法进行查找的。Lookupkey = InternalKey + SequenceNumber。而sequenceNumber是来自Get()方法选项参数中的snapshot。

这就实现了多版本读取机制。

# 公用基础类

LevelDB自行实现的一些独立于OS的API，Mutex和Condvar很常见，就不细说了，AtomicPtr是一种可以实现无锁原子读写操作的数据类型。可以类比一下OS提供的无锁原语：TAS、CAS等等。当前LevelDB已经移除了这个类型。

## MemoryBarrier(如smp_mb())

内存屏障在多核CPU中是非常重要，因为在多核CPU中，不同核之间的数据是不共享的，所以需要通过屏障来保证数据的一致性。

有关内存屏障相关的文章，[内存屏障（Memory Barrier）究竟是个什么鬼？ - 知乎 (zhihu.com)](https://zhuanlan.zhihu.com/p/125737864)这篇讲的很好，可以看看细节。

**smp_mb() 这个内存屏障的操作会在执行后续的store操作之前，首先flush store buffer（也就是将之前的值写入到cacheline中）。**这是一个偷懒的最直观总结，很容易理解。

