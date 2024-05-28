# LevelDB的写入流程

## kv的写入

<img src="./assets/Write.png" alt="Write" style="zoom: 80%;" />

这块前面介绍的基础类息息相关，LevelDB的写入流程大概会经历如下阶段，就是DBImpl::Write函数：

1. 写入的kv对会首先被封装到WriteBatch结构体中。
2. WriteBatch中封装了一批操作数据，之后将每一个WriteBatch对象封装到一个DBImpl::Writer对象中，DBImpl::Writer中添加了一些多线程同步操作所必需的同步原语。
3. 在2的基础之上，实际上写入流程调用的是DBImpl::Write，而每个Write操作中都会有一个writers队列，之后依次处理每个writer。当writer中的内容被写入到Log文件中时，释放锁，期间允许新的writer入队列，从而增加一定程度的并发性。
4. 当操作都写入到Log文件后，就可以将其写入到memtable中，之后用户就能看到这些操作了。

操作的log写盘，之后可以应用到memtable，这个过程没有锁的保护，但实际上writer队列中每次只允许头部的writer执行操作，因此可以保证每个时刻只会有一个writer在被写盘、应用到memtable。

leveldb用户通过调用write或者put函数向数据库中写入数据实际上是将数据写入到levedb的Memtable中。我们也曾经提到过，leveldb中有两个MemTable，分别是imm_和mem\_，其中imm\_是不可写的，若Imm\_不为null，则说明imm\_正在被compact为SSTable。

leveldb提供持久化，也就是需要将内存中的数据保存到磁盘上，也就是前面说的以sstable的形式将数据持久化。在leveldb中，内存中的每个memtable对应磁盘上的每个sstable，一般情况下我们不希望文件太大，因此必须控制memtable中的数据量，当达到一定的阀值时就要将其写盘。leveldb提供异步写盘的方式，这就是imm\_的作用，每次mem\_中的数据够多时，就将mem\_复制给imm\_，两者都是指针，所以复制操作很快)，并让mem\_指向一个重新申请的memTable。交换之前保证imm为空，然后mem\_就可以继续接受用户的数据，同时leveldb开启一个背景线程将imm_写入磁盘。

> DBImpl::MakeRoomForWrite函数起到了一个检查当前MemTable、Immutable_memtable、磁盘上Level0的SSTables的数量的状态的功能，以决定写入操作是否能继续执行，或者继续等待。
>
> 相关的规定限制都在dbformat.h中，里面规定了Level0层的文件数量达到多少时会延迟、会停止等待，等等。

具体的内容等看到compact操作部分时，会有更深的理解。

### 关于Background Thread

在DBImpl::MakeRoomForWrite函数中，可以看到，每个时刻，leveldb只允许一个background线程存在，这里需要加锁主要也是这个原因，防止某个瞬间两个线程同时开启背景线程。当确定当前数据库中没有背景线程，也不存在错误，同时确实有工作需要背景线程来完成，就通过env\_->Schedule(&DBImpl::BGWork, this)启动背景线程，前面的bg_compaction_scheduled_设置主要是告诉其他线程当前数据库中已经有一个背景线程在运行了。

## SSTable中data block的生成

承接上面，当kv的操作被写入到log文件后，就可以进一步的写到memtable中，当memtable的大小达到一定的阈值后，就会触发memtable->SSTable的转换，之前已经学习过了SSTable是以块为基本进行保存的，SSTable包括data block、fliter block、data index block、meta index block、footer几种类型，下面主要介绍块是如何生成的。

主要参考LevelDB中的Block_builder.h和block_builder.cc文件中的BlockBuilder类，该类会根据kv对，按照data block中的格式，生成一个data block。

### 读取Block

读取一个Data Block并且在该block中查找一个kv对，需要一个基于block实现一个iterator，具体可见Block.h和block.cc文件，根据data block的数据格式，就能明白如何读取。

# 生成SSTable

之前已经了解过了一个SSTable中各种block是如何生成，如何读取的，接下来进一步了解如何生成一个完整的SSTable文件，相关代码在table_builder.h和table_builder.cc中，主要关注TableBuilder类。TableBuilder是一个集大成的结构体，是用于构建一个SSTable(Sorted String Table)的Table(即SSTable)的接口。因此不难理解，在一个TableBuilder中会包含多个BlockBuilder，用于构建Data Block、Filter Block、Data Index Block、Filter Index Block这些，最后加上Footer，就构成了一个完整的SSTable。下图是TableBuilder中的API串联思路：

![image-20240528090558974](./assets/image-20240528090558974.png)

具体的设计思想还是要看TableBuilder中代码的注释，写在了代码上面。总体来说，在像TableBuilder添加键值对的过程中，除了总大小到达阈值后会生成一个DataBlock， filter block与data index block也在被同步构建；每当一个Data Block构建完成后，对应的会在filter block中生成若干个filter data,同样会在data index block中生成一个index entry。

当所有的kv对被写入后，首先把最后一批可能存在的不足一个Data Block大小的键值对数据生成一个data block。之后就可以根据当前信息，先后添加filter block，meta index block，data index block，footer这些，之后就生成了一个完整的SSTable，里面有很多细节值得学习，还是看源码。

# 读取SSTable

SSTable读取的相关代码在table.h和table.cc中，同样是根据Table类生成一个Iterator来进行读取，这里会涉及到BloomFilter的介绍。
