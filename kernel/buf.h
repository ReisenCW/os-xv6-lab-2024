struct buf {
  int valid;              // 数据是否有效
  int disk;               // 缓冲区数据是否需要写回磁盘
  uint dev;               // 设备号, 标识数据来自哪个物理设备
  uint blockno;           // 磁盘块号，标识设备上的具体物理块位置
  struct sleeplock lock;  // 当一个进程需要读写缓冲区时，需先获取该锁。若锁被占用，进程会休眠（而非自旋），直到锁可用
  uint refcnt;            // 当前有多少进程正在使用该缓冲区
  // struct buf *prev;    // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

