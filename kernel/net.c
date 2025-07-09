#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define MAX_UDP_PACKETS 16  // 每个端口最大缓存包数
#define MAX_PORT 65535      // 最大端口号

// 存储UDP数据包的结构
struct udp_packet {
  struct udp_packet *next;  // 链表指针
  uint32 src_ip;            // 源IP地址（主机字节序）
  uint16 src_port;          // 源端口（主机字节序）
  int len;                  // 数据长度
  char data[0];             // 柔性数组存储数据
};

// 端口绑定信息
struct port_queue {
  struct spinlock lock;     // 保护队列的锁
  struct udp_packet *head;  // 队列头
  struct udp_packet *tail;  // 队列尾
  int count;                // 当前包数量
  int bound;                // 是否已绑定
};

// 端口队列数组
static struct port_queue ports[MAX_PORT + 1];

void
netinit(void)
{
  initlock(&netlock, "netlock");
  // 初始化所有端口队列
  for (int i = 0; i <= MAX_PORT; i++) {
    initlock(&ports[i].lock, "port_queue");
    ports[i].head = ports[i].tail = 0;
    ports[i].count = 0;
    ports[i].bound = 0;
  }
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);

  // 检查端口有效性
  if (port < 0 || port > MAX_PORT)
    return -1;

  acquire(&ports[port].lock);
  // 检查是否已绑定
  if (ports[port].bound) {
    release(&ports[port].lock);
    return -1;
  }
  ports[port].bound = 1;
  release(&ports[port].lock);

  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  int dport, maxlen;
  uint64 src_addr, sport_addr, buf_addr;
  
  // 解析系统调用参数
  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);


  // 检查端口有效性
  if (dport < 0 || dport > MAX_PORT)
    return -1;

  struct port_queue *q = &ports[dport];
  acquire(&q->lock);

  // 检查端口是否已绑定
  if (!q->bound) {
    release(&q->lock);
    return -1;
  }

  // 等待队列中有数据包
  while (q->count == 0) {
    sleep(q, &q->lock);  // 释放锁并休眠，被唤醒时重新获得锁
  }

  // 取出队列头部的数据包
  struct udp_packet *p = q->head;
  q->head = p->next;
  if (q->head == 0) {
    q->tail = 0;  // 队列为空时更新尾指针
  }
  q->count--;

  release(&q->lock);  // 提前释放锁，减少持有时间

  // 复制源IP到用户空间
  int src_ip = p->src_ip;
  if (copyout(myproc()->pagetable, src_addr, (char *)&src_ip, sizeof(src_ip)) < 0) {
    kfree(p);
    return -1;
  }

  // 复制源端口到用户空间
  short src_port = p->src_port;
  if (copyout(myproc()->pagetable, sport_addr, (char *)&src_port, sizeof(src_port)) < 0) {
    kfree(p);
    return -1;
  }

  // 复制数据到用户空间
  int copy_len = p->len < maxlen ? p->len : maxlen;
  if (copyout(myproc()->pagetable, buf_addr, p->data, copy_len) < 0) {
    kfree(p);
    return -1;
  }

  // 释放数据包
  kfree(p);

  return copy_len;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // 不删除此打印，make grade依赖它
  static int seen_ip = 0;
  if (seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  // 检查缓冲区长度是否足够
  if (len < sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp)) {
    kfree(buf);
    return;
  }

  // 解析以太网头部
  struct eth *eth = (struct eth *)buf;
  
  // 解析IP头部
  struct ip *ip = (struct ip *)(eth + 1);
  int ip_header_len = (ip->ip_vhl & 0x0F) * 4;  // IP头部长度（字节）
  if (ip_header_len < sizeof(struct ip) || ip->ip_vhl >> 4 != 4) {
    kfree(buf);
    return;  // 不是IPv4或头部长度异常
  }

  // 只处理UDP协议
  if (ip->ip_p != IPPROTO_UDP) {
    kfree(buf);
    return;
  }

  // 解析UDP头部
  struct udp *udp = (struct udp *)((char *)ip + ip_header_len);
  uint16 dport = ntohs(udp->dport);  // 目的端口（网络字节序转主机字节序）
  uint16 sport = ntohs(udp->sport);  // 源端口
  uint16 udp_len = ntohs(udp->ulen); // UDP总长度（头部+数据）

  // 检查UDP长度有效性
  if (udp_len < sizeof(struct udp) || 
      (ip_header_len + udp_len) > (len - sizeof(struct eth))) {
    kfree(buf);
    return;
  }

  // 计算数据长度和数据位置
  int data_len = udp_len - sizeof(struct udp);
  char *data = (char *)udp + sizeof(struct udp);

  // 检查目的端口是否已绑定
  if (dport < 0 || dport > MAX_PORT || !ports[dport].bound) {
    kfree(buf);
    return;  // 端口未绑定，丢弃数据包
  }

  // 分配空间存储数据包
  struct udp_packet *p = kalloc();
  if (!p) {
    kfree(buf);
    return;  // 内存不足，丢弃
  }

  // 初始化数据包
  p->next = 0;
  p->src_ip = ntohl(ip->ip_src);  // 源IP（网络字节序转主机字节序）
  p->src_port = sport;
  p->len = data_len;
  memmove(p->data, data, data_len);  // 复制数据

  // 将数据包加入对应端口的队列
  struct port_queue *q = &ports[dport];
  acquire(&q->lock);

  // 若队列已满（超过MAX_UDP_PACKETS），丢弃新包
  if (q->count >= MAX_UDP_PACKETS) {
    release(&q->lock);
    kfree(p);
    kfree(buf);
    return;
  }

  // 将包加入队列尾部
  if (q->tail) {
    q->tail->next = p;
  } else {
    q->head = p;  // 队列为空时，头指针指向新包
  }
  q->tail = p;
  q->count++;

  // 唤醒等待该端口的进程
  wakeup(q);

  release(&q->lock);
  kfree(buf);  // 释放原始缓冲区
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
