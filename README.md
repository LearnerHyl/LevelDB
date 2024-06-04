# LevelDB

LevelDB结构简单,不依赖于任何第三方库,具有很好的独立,虽然其有针对性的对BigTable做了一定程度的简化,然而BigTable的主要技术思想与数据结构都体现在了LevelDB中。因此,LevelDB可以看作是BigTable的简化版或单机版。

阅读LevelDB是为了后续更好的学习RocksDB做准备，在阅读过程中结合源码，并在源码处加入了一些新的，以备后续复习。

[1. LevelDB公共基础类阅读](notes/LevelDB源码阅读1.md)

[2. LSM相关模块1](notes/LevelDB源码阅读2-LSM-part1.md)

[2.1 LSM相关模块2](notes/LevelDB源码阅读3-LSM-part2.md)

[3. LevelDB的Cache系统](notes/LevelDB源码阅读4-Cache系统.md)

[4. LevelDB的Compaction(TODO)](notes/LevelDB源码阅读5-Compaction.md)

[5. LevelDB的Multi-Version(TODO)](notes/LevelDB源码阅读6-Multi-Version.md)
