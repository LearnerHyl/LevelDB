# LevelDB-LSM-Tree整体介绍

leveldb之所以名字里面有level这个词，主要是因为leveldb将磁盘上的sstable文件分层(level)存储。每个内存中的Memtable所生成的sstable文件在level 0 中，高一层的level文件由低一层的level文件compact而成，或者说合并而成。最开始的时候只有level0中有文件。

随着memtable不断生成sstable文件，level0中的文件最终会达到一个数量，当这个数量大于某个阀值时，就选择level0中的若干个sstable文件进行合并，并把合并后的文件放入到level1中，当level1中的文件过多时，level1中的部分文件也将会合成新的sstable文件，放入level2中，以此类推。

除了level0之外，其他level中的SSTable文件中是不允许存在重复的key的，因此我们这里所说的level i中的部分文件合成放入level (i+1) i>1，是指将level i中的那部分文件和level i+1中和它的key重合的文件进行合并，而不是至单独level i中的文件合并。后面我们会仔细介绍不同level中的文件是怎么合并的。

## 模块组成

因此，LSM树应该包含MemTable、SSTable这些关键模块，以及MemTable如何通过写盘转化为SSTable，Level i是如何经过compact操作后被压缩到compat i+1层的。下面依次介绍。

# MemTable模块

## LevelDB读写与MemTable的关系

在DB::Get()的操作流程中我们可知，在读取操作时首先读取MemTable，然后读取Immutable MemTable，这两者都是在内存中的，最后才会去磁盘上的SSTable中进行读取。而在进行写入操作时，会先将操作以WAL的形式写入到Log之中，之后会按照Log写入的顺序，依次解析每一条Log，将Log中的操作应用于该Log文件对应的Memtable中。

Log文件和MemTable是一一对应的关系，只有当MemTable的大小超过设定的阈值，并成功生成SSTable且刷新到磁盘上之后(fsync)，对应的Log文件才能被删除。之后会使用一个新的MemTable对象。

还有我们上面介绍到的崩溃恢复部分，会从上次未持久化的Log文件（没有成功生成新的SSTable，以日志文件形式存在于磁盘中）中解析并应用Log，该过程中同样会生成一个新的MemTable。

这里主要阅读与MemTable相关的代码，关于MemTable的定义、表示和具体实现，以及相关用法。 在LevelDB中，MemTable其实是对**SkipList**（跳表）数据结构的封装。MemTable相关的定义放在了MemTable.h和MemTable.cc文件中，主要包括了Get、Add、NewIterator等的点查找、添加、新建迭代器访问等操作

## 为什么要有immutable-MemTable

**为了防止写入kv时被阻塞。**

设想，如果没有immutable memtable，当memtable满了之后后台线程需要将memtable 立即flush到新建的sst中，在flush的过程中，新的KV记录是无法写入的，只能等待，就会造成新写入的KV记录被阻塞。

![image-20240521100846045](./assets/image-20240521100846045.png)

### RocksDB的MemTable

在leveldb中只有两个memtable：memtable和immutable memtable；但是在rocksdb中，memtable的数量是可以配置的，当memtable中的数据量超过设定值后，默认是64MB，就会转变为一个immutable memtable，后台线程就会将immutable memtable通过pipe形式，以异步批量的方式flush到level0层中新建的sst文件，如果该新建的sst文件与level 0层其他sst文件中的key的范围有重叠的就会留在level 0层中，否则就会下沉到level 1层，最高会下沉到level 2层中。

## MemTable::Add()

MemTable中的add操作代表了一次修改，这个修改可能是Insert也可能是Delete。统一以一条Entry的格式插入到MemTable中，Entry的具体格式如下：

```c++
// Format of an entry is concatenation of:
//  key_size     : varint32 of internal_key.size()，是key+tag的总长度
//  key bytes    : char[internal_key.size()]
//  tag          : uint64((sequence << 8) | type)
//  value_size   : varint32 of value.size()
//  value bytes  : char[value.size()]
```

Add操作会根据传入的参数信息，用压缩编码的方式构造出一个Entry，最后将其插入到底层数据结构跳表中。每一个MemTable都会使用一个Arena内存池进行内存管理，

## MemTable查找

Memtable的查找分为Get()和Iterator查找两种，前者是点查询，后者可以遍历。首先一定要知道LookUpKey，具体请见dbformat.h文件中的定义，LookUpKey=userkey_len+userkey+sequencenumber(Tag)，在MemTable中查找kv对时需要通过LookUpKey，Get和Iterator方法皆是如此。

### MemTable::Get()

MemTable基于自身的跳表数据结构构造了一个迭代器，输入Lookupkey::memtable_key，进行点查询。

- 如果发现Memtable中存在该key对应的entry，则进一步判断该entry当前是还存在还是已经被delete了，若存在则返回查找结果，若已经被delete则返回NotFound。
- 若Memtable中不存在该key对应的entry，则说明没存在过，返回false。

重点在于构造的迭代器，迭代器本身是基于skiplist进行构造的，因此需要了解SkipList的插入、删除、查找原理。

### MemTable::Iterator()

基于跳表生成一个迭代器，本质上是调用的跳表中的迭代器，下面会单独开一个章节介绍跳表。

# SkipList

LevelDB 是一个单机的 KV 存储引擎。KV 引擎在本质上可认为只提供对数据条目（key，val） `Put(key, val), Get(key) val, Delete(key)` 操作的三个接口。而在实现上，LevelDB 在收到删除请求时不会真正删除数据，而是为该 Key 写一个特殊标记，以备读取时发现该 Key 不存在，从而将 `Delete` 转为 `Put` ，进而将三个接口简化为两个。砍完这一刀后，剩下的就是在 `Put` 和 `Get` 间进行性能取舍，LevelDB 的选择是：**牺牲部分 `Get` 性能，换取强悍 `Put` 性能，再极力优化 `Get`**。

我们知道，在存储层次体系（[Memory hierarchy](https://en.wikipedia.org/wiki/Memory_hierarchy)）中，内存访问远快于磁盘，因此 LevelDB 为了达到目标做了以下设计：

1. *写入（Put）*：让所有写入都发生在**内存**中，然后达到一定尺寸后将其批量刷**磁盘**。
2. *读取（Get）*：随着时间推移，数据不断写入，内存中会有一小部分数据，磁盘中有剩余大部分数据。读取时，如果在内存中没命中，就需要去磁盘查找。

为了保证写入性能，同时优化读取性能，需要内存中的存储结构能够同时支持高效的**插入**和**查找**。

之前听说 LevelDB 时，最自然的想法，以为该内存结构（memtable）为是[平衡树](https://en.wikipedia.org/wiki/Self-balancing_binary_search_tree)，比如[红黑树](https://en.wikipedia.org/wiki/Red–black_tree)、[AVL 树](https://en.wikipedia.org/wiki/AVL_tree)等，可以保证插入和查找的时间复杂度都是 lg (n)，看源码才知道用了跳表。相比平衡树，跳表优势在于，在保证读写性能的同时，大大简化了实现。

此外，为了将数据定期 dump 到磁盘，还需要该数据结构支持高效的顺序遍历。总结一下 LevelDB 内存数据结构（memtable）需求点：

1. 高效查找
2. 高效插入
3. 高效顺序遍历

## 原理

跳表由 William Pugh 在 1990 年提出，相关论文为：[Skip Lists: A Probabilistic Alternative to Balanced Trees](https://15721.courses.cs.cmu.edu/spring2018/papers/08-oltpindexes1/pugh-skiplists-cacm1990.pdf)。从题目可以看出，作者旨在设计一种能够替换平衡树的数据结构，正如他在开篇提到：

跳表是一种可以取代平衡树的数据结构。跳表使用**概率均衡**而非严格均衡策略，从而相对于平衡树，大大简化和加速了元素的插入和删除。

### 链表与跳表

简言之，**跳表就是带有额外指针的链表**。为了理解这个关系，我们来思考一下优化有序链表查找性能的过程。

假设我们有个有序链表，可知其查询和插入复杂度都为 O (n)。相比数组，链表不能进行二分查找的原因在于，不能用下标索引进行常数复杂度数据访问，从而不能每次每次快速的筛掉现有规模的一半。那么如何改造一下链表使之可以进行二分？

- 利用 map 构造一个下标到节点的映射？这样虽然可以进行二分查询了，但是每次插入都会引起后面所有元素的下标变动，从而需要在 map 中进行 O (n) 的更新。

- 增加指针使得从任何一个节点都能直接跳到其他节点？那得构造一个全连接图，指针耗费太多空间不说，每次插入指针的更新仍是 O (n) 的。

跳表给出了一种思路，**跳步采样，构建索引，逐层递减**。下面利用论文中的一张图来详细解释下。

![image-20240511194304769](./assets/image-20240511194304769.png)

如上图，初始我们有个带头结点的有序链表 a，其查找复杂度为 O (n)。然后，我们进行跳步采样，将采样出的节点按用指针依次串联上，得到表 b，此时的查找复杂度为 O (n/2 + 1) 。其后，我们在上次采样出的节点，再次进行跳步采样，并利用指针依次串联上，得到表 c，此时的查找复杂为 O (n/4 + 2)。此后，我们重复这个跳步采样、依次串联的过程，直到采样出的节点只剩一个，如图 e，此时的查找复杂度，可以看出为 O (log2n))。

代价是我们增加了一堆指针，增加了多少呢？我们来逐次考虑这个问题。从图中可以看出，每次采样出多少节点，便会增加多少个指针；我们的采样策略是，每次在上一次的节点集中间隔采样，初始节点数为 n，最后采到只剩一个节点为止。将其加和则为：(n/2 + n/4 + … + 1) = n。这和一个节点为 n 的二叉树的指针数是相同的。

### 跳表和平衡树

在实践中，我们常用搜索二叉树作为字典表或者顺序表。在插入过程中，如果数据在 key 空间具有很好地随机性，那么二叉搜索树每次顺序插入就可以维持很好的查询性能。但如果我们顺序的插入数据，则会使得二叉搜索树严重失衡，从而使读写性能都大幅度退化。

AVL可以始终将左右子树的高度保持在1的差距，这就意味着插入有可能会触发大量的调整操作；因此出现了红黑树，红黑树相比于AVL进一步的放宽了平衡要求，他只要求根节点到每个叶子结点的黑色节点相同即可。在放弃一定的查找性能的同时，减少了调整操作的开销。但是AVL和红黑树的旋转策略和复杂度其实实现起来要时间的。

而跳表在保证同样查询效率的情况下，使用了一种很巧妙的转化，大大简化了插入的实现。我们不能保证所有的插入请求在 key 空间具有很好地随机性，或者说均衡性；但我们可以控制每个节点其他维度的均衡性。比如，跳表中每个节点的指针数分布的**概率均衡**。

### 概率均衡

为了更好地讲清楚这个问题，我们梳理一下跳表的结构和所涉及到概念。跳表每个节点都会有 1 ~ MaxLevel 个指针，有 k 个指针的节点称为 *k 层节点*（level k node）；所有节点的层次数的最大值为跳表的*最大层数*（MaxLevel）。**跳表带有一个空的头结点，头结点有 MaxLevel 个指针。**

按前面从有序链表构建跳表的思路，每次插入新节点就变成了难题，比如插入的节点需要有多少个指针？插入后如何才能保证查找性能不下降（即维持采样的均衡）？

为了解决这个问题， Pugh 进行了一个巧妙的转化：将*全局、静态*的构建索引拆解为*独立、动态*的构建索引。其中的关键在于**通过对跳表全局节点指针数概率分布的维持，达到对查询效率的保持**。分析上面见索引逐层采样的过程我们可以发现，建成的跳表中有 50% 的节点为 1 层节点，25% 的节点为 2 层节点，12.5% 的节点为三层节点，依次类推。若能维持我们构造的跳表中的节点具有同样的概率分布，就能保证差不多查询性能。这在直觉上似乎没问题，这里不去深入探讨背后的数学原理，感兴趣的同学可以去读一读论文。

经过这样的转化，就解决了上面提出的两个问题：

1. 插入新节点的指针数通过独立的计算一个概率值决定，使全局节点的指针数满足几何分布即可。
2. 插入时不需要做额外的节点调整，只需要先找到其需要放的位置，然后修改他和前驱的指向即可。

这样插入节点的时间复杂度为查找的时间复杂度 O(log~2~n)，与修改指针的复杂度 `O(1)` *[注 3]* 之和，即也为 O(log~2~n)，删除过程和插入类似，也可以转化为查找和修改指针值，因此复杂度也为 O(log~2~n)。

> 注意采样的概率p和总层数是可以根据具体情况调整的。

# LevelDB的跳表(内存屏障与Flexiable Array的用法)

LevelDB对跳表的实现增加了多线程并发访问方面的支持：

1. Write：在修改跳表时，需要在用户代码侧加锁。
2. Read：在访问跳表（查找、遍历）时，只需保证跳表不被其他线程销毁即可，不必额外加锁。

也就是说，用户侧只需要处理写写冲突，**LevelDB 跳表保证没有读写冲突**。这是因为在实现时，LevelDB 做了以下假设（Invariants）：

1. 除非跳表被销毁，跳表节点只会增加而不会被删除，因为跳表对外根本不提供删除接口。这一点是由MemTable的AppendOnly特性决定的。删除操作同样由一个新Node所代表。
2. 被插入到跳表中的节点，除了 next 指针其他域都是不可变的，并且只有插入操作会改变跳表。

具体内容需要看代码，这里还涉及到了Flexible Array(柔性数组)和std::atomic的内存屏障的使用。

[大白话解析LevelDB：SkipList（跳表）_leveldb skiplist-CSDN博客](https://blog.csdn.net/sinat_38293503/article/details/134643628)

[漫谈 LevelDB 数据结构（一）：跳表（Skip List） | 木鸟杂记 (qtmuniao.com)](https://www.qtmuniao.com/2020/07/03/leveldb-data-structures-skip-list/)

## 多线程同步-C++11的内存序

LevelDB的跳表使用了Template Class进行编写，因为我们需要考虑到多线程读写，所以需要考虑同步问题，而LevelDB在插入新节点的时候，通过内存屏障实现多线程同步。

我们知道，编译器 / CPU 在保在**达到相同效果**（最终的结果是相同的）的情况下会按需（加快速度、内存优化等）对指令进行重排，这对单线程来说的确没什么。但是对于多线程，指令重排会使得多线程间代码执行顺序变的各种反直觉。比如用下面的go程序举例：

```go
var a, b int

func f() {
  a = 1
  b = 2
}

func g() {
  print(b)
  print(a)
}

func main() {
  go f()
  g()
}
```

该代码片段可能会打印出 `2 0`。原因在于编译器 / CPU 将 `f()` 赋值顺序重排了或者将 `g()` 中打印顺序重排了。

这就意味着你跟随直觉写出的多线程代码，可能会出问题。因为你无形中默认了单个线程中执行顺序是代码序，多线程中虽然代码执行会产生交错，但仍然保持各自线程内的代码序。实则不然，由于编译器 / CPU 的指令重排，如果不做显式同步，你不能对多线程间代码执行顺序有任何假设。

C++11 中 atomic 标准库中定义了6种内存序列，规定了一些指令重排方面的限制，内存序一般配合std::atomic这种无锁类型的线程安全类一同使用：

1. `memory_order_relaxed` ：不提供任何顺序保证。 【**注意这个只保证单个操作的原子性不包括内存顺序**】
2. `memory_order_consume` - 仅限于依赖于原子操作的特定数据。**已经不怎么用了**
3. `std::memory_order_acquire`: 用于load操作（代表广义的读操作，因为任何读操作的底层都是一个load指令） 。保证同线程中该 load 之后的对相关内存读写语句不会被重排到 load 之前，并且其他线程中对同样内存用了 store 的release内存序的操作都对该线程可见。
4. `std::memory_order_release`：用于store 操作（代表广义的写操作，因为大多数写操作的底层都是一个store指令）。保证同线程中该 store 之后的对相关内存的读写语句不会被重排到 store 之前，并且该线程的所有修改对用了 load acquire 的其他线程都可见。
5. `memory_order_acq_rel` - 同时包含acquire和release的语义。
6. `memory_order_seq_cst` - 提供顺序一致性，是最强的内存序。

LevelDB中主要用到了`memory_order_relaxed` ，`std::memory_order_acquire`，`std::memory_order_release`这三种内存序列。

## SkipList::Node的内存序应用

Node的SetNext和Next方法提供了有内存屏障和无内存屏障的两个版本。具体请见skiplist.h文件。注意LevelDB采用了Arena内存池自行管理内存，因此有必要说明一下NewNode函数中的一个用法：

```c++
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
      // 这里分配的内存包含两部分:
      // 1. Node对象本身，大小为sizeof(Node)
      // 2. Node对象的next_数组，大小为sizeof(std::atomic<Node*>) * (height - 1)，
      // 因为Node对象中的next_数组本身已经包含了一个指针空间，所以这里只需要分配height - 1个指针空间即可
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
      // 与new Node(key)不同，这里是在arena_上分配内存，而不是在堆上分配内存
  return new (node_memory) Node(key);
}
```

我们可以注意到最后返回的是`new (node_memory) Node(key)`，而不是`new Node(key)`。

`new (node_memory) Node(key)` 是所谓的定位 `new` 运算符，它与普通的 `new` 运算符稍有不同。

普通的 `new` 运算符用于在动态内存中分配一个新的对象，并在分配的内存上调用对象的构造函数进行初始化。例如，`new Node(key)` 会分配足够的内存以存储一个 `Node` 对象，并调用 `Node` 类的构造函数将 `key` 传递给该构造函数以初始化新创建的节点对象。

而定位 `new` 运算符 `new (node_memory) Node(key)` 则是在给定的内存地址 `node_memory` 上调用对象的构造函数进行初始化。它允许程序员控制对象在内存中的位置。在这种情况下，`node_memory` 是一个指向预先分配好的内存块的指针，通过定位 `new` 运算符，我们告诉编译器在这块内存上构造一个 `Node` 对象，并使用给定的参数 `key` 调用 `Node` 类的构造函数。

总的来说，区别在于：
- 普通的 `new` 运算符用于在动态内存中分配对象，并进行初始化。
- 定位 `new` 运算符用于在给定的内存地址上进行对象的构造和初始化。

## MemTable生成SSTable-Compaction操作

LevelDB是基于LSM树的，对于写操作，会先将数据写入内存，当内存表大于某个阈值时，需要将其作为SSTable写入到磁盘。LevelDB中，当MemTable大小超出配置值后，则会触发生成SSTable并写盘的过程，具体功能函数请见`db/db_impl.cc`中的`WriteLevel0Table`函数。

这就与前面介绍的崩溃恢复接上了，当数据库读取磁盘中的log做恢复操作时，若MemTable大小超过了阈值，同样会触发MemTable的生成，写入磁盘成功之后，memtable的内存会被释放。RecoveLogFile->WriteLevel0Table。

这里涉及到了TableBuilder、TableCache、Meta等一系列的类，在后面的会逐步阅读这些代码。

# SSTable(Sorted Strings Table)

SSTable（Sorted Strings Table，有序字符串表）是一种用于存储键值对数据的数据结构，常用于分布式数据库系统中，在各种存储引擎中得到了广泛的应用，包括LevelDB、HBase、Cassandra等。SSTable基于磁盘上的有序文件，具有高效的读取性能和较小的内存开销。

如我们之前提到的，leveldb是典型的LSM树(Log Structured-Merge Tree)实现，即一次leveldb的写入过程并不是直接将数据持久化到磁盘文件中，而是将写操作首先写入日志文件中，其次将写操作应用在memtable上。

当leveldb达到checkpoint点（memtable中的数据量超过了预设的阈值），会将当前memtable冻结成一个不可更改的内存数据库（immutable memory db），并且创建一个新的memtable供系统继续使用。

immutable memory db会在后台进行一次minor compaction，即将内存数据库中的数据持久化到磁盘文件中。

> 在这里我们暂时不展开讨论minor compaction相关的内容，先简单地理解为将内存中的数据持久化到文件。

leveldb（或者说LSM树）设计Minor Compaction的目的是为了：

1. 有效地降低内存的使用率；
2. 避免日志文件过大，系统恢复时间过长；

当memory db的数据被持久化到文件中时，leveldb将以一定规则进行文件组织，这种文件格式成为sstable。

## SSTable文件格式

### SSTable物理结构

为了提高整体的读写效率，一个sstable文件按照固定大小进行块划分，默认每个块的大小为4KiB。每个Block中，除了存储数据以外，还会存储两个额外的辅助字段：

1. 压缩类型
2. CRC校验码

压缩类型说明了Block中存储的数据是否进行了数据压缩，若是，采用了哪种算法进行压缩。leveldb中默认采用[Snappy算法](https://github.com/google/snappy)进行压缩。CRC校验码是循环冗余校验校验码，校验范围包括数据以及压缩类型。下图表示了一个SSTable中包含了若干个block的组织图，每个Block从物理上看都有Data、压缩类型、CRC三个字段：

<img src="./assets/image-20240517170905577.png" alt="image-20240517170905577" style="zoom:67%;" />

### SSTable逻辑结构

在逻辑上，根据功能不同，leveldb在逻辑上又将sstable分为：

1. **data block**: 用来存储key value数据对；
2. **filter block**: 用来存储一些过滤器相关的数据（布隆过滤器），但是若用户不指定leveldb使用过滤器，leveldb在该block中不会存储任何内容；
3. **meta Index block**: 用来存储filter block的索引信息（索引信息指在该sstable文件中的偏移量以及数据长度）；
4. **index block**：index block中用来存储每个data block的索引信息；
5. **footer**: 用来存储meta index block及index block的索引信息；他是固定大小48B，下面会具体介绍footer部分的结构。

1-4类型的Block的物理结构都如上面介绍的那样，下面会具体对每种类型Block的Data部分进行具体介绍。

<img src="./assets/image-20240517165822024.png" alt="image-20240517165822024" style="zoom:67%;" />

具体源码实现可见LevelDB的block.h和block.cc文件中关于Block类的定义，LevelDB用BlockContents字段来识别Block到底存储了哪一类型的数据。此外，FilterBlock的结构较为特殊，LevelDB专门定义了FilterBlockBuilder和FilterBlockReader两个类来建立和读取filterblock。

format.h文件中存储了如BlockHandle、Footer、BlockContents等结构体，这些结构体对于理解Block的构成十分必要，因此也要读一下源码。

## footer结构

footer大小固定，为48字节，用来存储meta index block与index block在sstable中的索引信息，另外尾部还会存储一个magic word，内容为："http://code.google.com/p/leveldb/"字符串sha1哈希的前8个字节。

![image-20240517183325646](./assets/image-20240517183325646.png)

Meta index block's index和Index block's index这两个字段实际上都是BlockHandl结构体，用变长字段存储的，若前两个加起来不足40B（varint64的最长长度为10B，每个BlockHandle有offset_和size\_两个变量），则用padding填充到40B即可。具体代码请见format.h中的介绍。

## Data Block的结构

data block中存储的数据是leveldb中的keyvalue键值对。其中一个data block中的数据部分（不包括压缩类型、CRC校验码）按逻辑又以下图进行划分：

<img src="./assets/image-20240517172101378.png" alt="image-20240517172101378" style="zoom: 67%;" />

第一部分用来存储keyvalue数据。由于sstable中所有的keyvalue对都是严格按序存储的，为了节省存储空间，leveldb并不会为每一对keyvalue对都存储完整的key值，而是存储与**上一个key非共享的部分**，避免了key重复内容的存储。

每间隔若干个keyvalue对，将为该条记录重新存储一个完整的key。重复该过程（默认间隔值为16），每个重新存储完整key的点称之为Restart point。

> leveldb设计Restart point的目的是在读取sstable内容时，加速查找的过程。
>
> 由于每个Restart point存储的都是完整的key值，因此在sstable中进行数据查找时，可以首先利用restart point点的数据进行键值比较，以便于快速定位目标数据所在的区域；
>
> 当确定目标数据所在区域时，再依次对区间内所有数据项逐项比较key值，进行细粒度地查找；
>
> 该思想有点类似于跳表中利用高层数据迅速定位，底层数据详细查找的理念，降低查找的复杂度。
>
> restartpoint过大会导致存储效率降低，过小会导致读取效率降低，这里存在一个trade off。

每个kv entry都由5个字段组成：

![img](./assets/entry_format.jpeg)

1. 与前一条记录key共享部分的长度；可以从最近的restart点获取到共享数据的内容。
2. 与前一条记录key不共享部分的长度；
3. value长度；
4. 与前一条记录key非共享的内容；
5. value内容；

前三个字段都采用varint32编码，delta key和value部分的长度取决于前面解析出来的对应字段的值。

## filter block结构-布隆过滤器

为了加快sstable中数据查询的效率，在直接查询datablock中的内容之前，leveldb首先根据filter block中的过滤数据判断指定的datablock中是否有需要查询的数据，若判断不存在，则无需对这个datablock进行数据查找。

filter block存储的是data block数据的一些过滤信息。这些过滤数据一般指代布隆过滤器的数据，用于加快查询的速度，关于布隆过滤器的详细内容，需要详细的看这部分源码。

> 看完SSTable的整体流程后，会回过头来看布隆过滤器这块。

<img src="./assets/image-20240517175516318.png" alt="image-20240517175516318" style="zoom:67%;" />

![image-20240519112603059](./assets/image-20240519112603059.png)

filter block存储的数据主要由4个部分组成，图中少画了一个num字段：

- 过滤器内容部分：Filter Data i。在SSTable中，每2KB键值对数据会生成一个过滤器，即生成一个filter Data，过滤器内容实际保存在filter data中。
- 过滤器偏移量：`filter i offset`表示第i个filter data在整个filter block中的起始偏移量。每个Filter offset的大小固定为4个字节，采用4个字节定长编码。
- `filter offset's offset(all filter data size)`：表示filter block的索引数据在filter block中的起始偏移量。同时也代表了所有Filter Data加起来的总的数据大小（字节数）。本身也是固定4个字节大小。
- Base Lg：默认值为11，表示每2KB（2^11^ B）的数据，创建一个新的过滤器来存放过滤数据，即每个Filter Data都代表了2KB的实际数据。base本身占1个byte。

在读取filter block中的内容时，可以首先读出`filter offset's offset`的值，然后依次读取`filter i offset`，根据这些offset分别读出`filter data`。在Base Lg和filter offset's offset之间还有一个num字段，代表了Filter Data的数量，即过滤器的数量。

一个sstable只有一个filter block，其内存储了所有block的filter数据. 具体来说，filter_data_k 包含了所有起始位置处于 [base*k, base*(k+1)]范围内的block的key的集合的filter数据，按数据大小而非block切分主要是为了尽量均匀，以应对存在一些block的key很多，另一些block的key很少的情况。

此外，leveldb中，特殊的sstable文件格式设计简化了许多操作，例如：索引和BloomFilter等元数据可随文件一起创建和销毁，即直接存在文件里，不用加载时动态计算，不用维护更新。

这时应该已经明白从SSTable中读取到目标Filter Data的整体流程，即如果有一个已知偏移量的Data Block，我们应该可以根据该Data Block的偏移量，计算出该Block属于第几个过滤器，之后从Filter Block中获取到对应的过滤器。

> 由于每2KB数据会产生一个过滤器，因此只需要用偏移量除以2048即可知道该数据块（一个Block为4KB，他可能会横跨多个过滤器）属于第几个过滤器。这取决于我们要查找的具体范围。

具体逻辑要看filter_block.h和filter_block.cc、filter_policy文件中的源码，体验技术内涵。布隆过滤器的源码在bloom.cc中，这块到后面会具体介绍使用方法。

## meta index block结构

meta index block用来存储filter block在整个sstable中的索引信息。因为一个SSTable只有一个filter Block，因此meta index block只存储一条记录：

- 该记录的key为："filter."与过滤器名字组成的常量字符串，最常用的是布隆过滤器，即filter.leveldb.BuiltinBloomFilter2。
- 该记录的value为：filter block在sstable中的索引信息序列化后的内容，其本身是一个BlockHandle类型，BlockHandle保存了filter block在SSTable中的偏移量与大小。

每个SSTable中只有一个meta index block。

## index block结构

与meta index block类似，index block用来存储所有data block的相关索引信息。indexblock包含若干条记录，每一条记录代表一个data block的索引信息。

<img src="./assets/image-20240517183014170.png" alt="image-20240517183014170" style="zoom:67%;" />

上图可知，Index Block中的一条record包含三个字段：

1. data block i 中最大的key值。这个MaxKey 是一个大于等于当前 data block 中最大的 key 且小于下一个 block 中最小（短）的 key，这一块的逻辑可以参考 FindShortestSeparator 的调用和实现。这样做是为了减小 index block 的体积，毕竟我们希望程序运行的时候，index block 被尽可能 cache 在内存中。

   > 比如说，当前的block i中的最大key为abceg，下一个block中的最小键为abcqddh，那么block i的MaxKey就是abcf，abcf大于abceg且小于abcq，且是最短的符合条件的key。

2. 该data block起始地址在sstable中的偏移量；

3. 该data block的大小；

此外，data block i最大的key值同时也是该index record的key值，如此设计的目的是，依次比较index block中记录信息的key值即可实现快速定位目标数据在哪个data block中。

每个SSTable中只有一个index block。

