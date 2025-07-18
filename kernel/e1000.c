#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16 // 传输环
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16))); // 发送描述符环（16字节对齐，硬件要求）
static char *tx_bufs[TX_RING_SIZE]; // 存储待发送的数据包内容

#define RX_RING_SIZE 16 // 接收环
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16))); // 接收描述符环（16字节对齐）
static char *rx_bufs[RX_RING_SIZE]; // 存储接收的数据包内容

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD; // 初始状态：描述符“已完成” Descriptor Done，表示可用
    tx_bufs[i] = 0;                        // 发送缓冲区暂未分配（后续发送数据时动态分配）
  }
  regs[E1000_TDBAL] = (uint64)tx_ring; // 传输环基地址
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring); // 传输环长度
  regs[E1000_TDH] = regs[E1000_TDT] = 0; // 初始化发送头（TDH）和尾（TDT）指针为0（暂无待发送包）

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();                  // 为每个接收描述符分配缓冲区（内核内存）
    if (!rx_bufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64)rx_bufs[i];   // 描述符的addr指向缓冲区物理地址（供DMA写入）
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56 只接收目标为本机的数据包
  // MAC地址共6字节，分两次写入E1000_RA寄存器（32位寄存器，需拆分）
  regs[E1000_RA] = 0x12005452; // 低32位：0x52 0x54 0x00 0x12 → 对应“52:54:00:12”
  regs[E1000_RA + 1] = 0x5634 | (1 << 31); // 高16位：0x34 0x56（对应“34:56”），并设置最高位（1<<31）表示“地址有效”
  // multicast table 禁用组播，只接收单播和广播
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(char *buf, int len)
{
  // buf contains an ethernet frame; 
  // program it into the TX descriptor ring so that the e1000 sends it.
  // Stash a pointer so that it can be freed after send completes.

  // 检查帧长度是否合法（以太网帧最大长度为1518字节，包含头部和CRC）
  if (len > DATA_MAX)
  {
    printf("e1000: packet too large (%d > %d)\n", len, DATA_MAX);
    return -1;
  }
  acquire(&e1000_lock);
  // 查找下一个可用的发送描述符
  int tdt = regs[E1000_TDT]; // 获取当前尾指针位置
  struct tx_desc *dp = &tx_ring[tdt]; // 获取对应的发送描述符
  if( !dp->status & E1000_TXD_STAT_DD ) {
    // 如果描述符未完成，说明发送环已满，无法发送新包
    release(&e1000_lock);
    printf("e1000: transmit ring full\n");
    return -1;
  }
  // 如果之前的缓冲区不为空，释放它（因为这个描述符即将被重用）
  if (tx_bufs[tdt])
  {
    kfree(tx_bufs[tdt]);
  }
  tx_bufs[tdt] = buf; // 保存当前发送缓冲区指针
  dp->addr = (uint64)buf; // 设置描述符的地址为缓冲区物理地址
  dp->length = len; // 设置数据长度
  dp->cso = 0; // 无分段偏移
  dp->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS; // 设置命令：end of packet , reprot status
  dp->status = 0; // 清除状态位

  // 更新尾指针，指向下一个可用描述符
  tdt = (tdt + 1) % TX_RING_SIZE; // 环形
  regs[E1000_TDT] = tdt; // 更新硬件尾指针

  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  // Check for packets that have arrived from the e1000
  // Create and deliver a buf for each packet (using net_rx()).
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
