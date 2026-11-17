#ifndef HEADER_fd_src_discof_arb_fd_arbnotify_h
#define HEADER_fd_src_discof_arb_fd_arbnotify_h

#include "fd_arbfilter.h"

/* fd_arbnotify_t 是「命中通知环」上的定长记录。

   执行 tile 在交易提交后用 fd_arbfilter_collect 判定这笔交易命中了哪些
   key；命中时把命中的 key 与这笔交易所属的账户库 fork 发布到一条没有消
   费者的 link。外部程序映射该 link 的 mcache/dcache，用记录里的 key 从
   accdb 的只读映射直接读账户数据，不必经过验证器内部的任何 tile。

   环是不可靠的：没有消费者就没有流控，生产者不会因为读者跟不上而阻塞，
   读者落后时只能跳过陈旧记录（见 `arb watch`）。 */

/* FD_ARBNOTIFY_KEY_MAX 是一条记录最多携带的命中 key 条数。命中的 key 多
   于这个数时按判定顺序截断，matched 位掩码仍然完整。 */

#define FD_ARBNOTIFY_KEY_MAX (8UL)

struct fd_arbnotify {
  ulong  slot;             /* 交易所在 slot */
  ulong  bank_seq;         /* bank 序号，同一 slot 内排序用 */
  ushort accdb_fork_id;    /* 该交易执行时 bank 的 accdb fork id */
  uint   accdb_generation; /* 该 fork 的 generation，与 fork id 一起标识一次 fork 生命周期 */
  ushort matched;          /* FD_ARBFILTER_BIT_* 位掩码 */
  uchar  key_cnt;          /* keys[] 中有效条数，已去重 */
  uchar  _pad[ 3 ];
  uchar  sig[ 64 ];        /* 交易签名，供外部程序对账 */
  uchar  keys[ FD_ARBNOTIFY_KEY_MAX ][ 32 ];
};

typedef struct fd_arbnotify fd_arbnotify_t;

/* 记录的定长决定了 link 的 mtu：mtu 必须直接取 sizeof(fd_arbnotify_t)，
   禁止在拓扑里另写数字。32 位字段后面的 2 字节隐式填充与 _pad 一起让
   sizeof 落在 8 的整数倍上。 */

FD_STATIC_ASSERT( sizeof(fd_arbnotify_t)==352UL, fd_arbnotify_layout );

/* FD_ARBNOTIFY_LINK_LE / FD_ARBNOTIFY_LINK_RP 是两个执行 tile 各自的出链
   路名：leader 路径的 execle tile 发布到前者，replay 路径的 execrp tile
   发布到后者。一条 link 只允许一个生产者，因此按 tile 实例各建一条。

   FD_ARBNOTIFY_DEPTH 是一条 link 的环深度，单位是记录条数。 */

#define FD_ARBNOTIFY_LINK_LE "arb_notify"
#define FD_ARBNOTIFY_LINK_RP "arb_notify_rp"
#define FD_ARBNOTIFY_DEPTH   (1024UL)

FD_PROTOTYPES_BEGIN

/* fd_arbnotify_fill 把一条记录写进 rec 并返回 rec。

   rec 由调用方从 dcache 取，chunk 已经按 fd_arbnotify_t 对齐。fill 先把
   整条记录清零，再写各字段与 hits[0..hit_cnt) 的 key，尾部未用到的 key
   槽与填充字节因此都是确定的 0。hit_cnt 超过 FD_ARBNOTIFY_KEY_MAX 时只写
   前 FD_ARBNOTIFY_KEY_MAX 条，key_cnt 记录实际写入的条数。

   sig 指向 64 字节签名，为 NULL 时签名区写全 0（交易没有签名）。hits 为
   NULL 时按 hit_cnt==0 处理。 */

static inline fd_arbnotify_t *
fd_arbnotify_fill( fd_arbnotify_t *           rec,
                   ulong                      slot,
                   ulong                      bank_seq,
                   ushort                     accdb_fork_id,
                   uint                       accdb_generation,
                   ushort                     matched,
                   uchar const *              sig,
                   fd_arbfilter_hit_t const * hits,
                   ulong                      hit_cnt ) {
  fd_memset( rec, 0, sizeof(fd_arbnotify_t) );

  rec->slot             = slot;
  rec->bank_seq         = bank_seq;
  rec->accdb_fork_id    = accdb_fork_id;
  rec->accdb_generation = accdb_generation;
  rec->matched          = matched;

  if( FD_LIKELY( sig ) ) fd_memcpy( rec->sig, sig, 64UL );

  ulong cnt = hits ? fd_ulong_min( hit_cnt, FD_ARBNOTIFY_KEY_MAX ) : 0UL;
  for( ulong i=0UL; i<cnt; i++ ) fd_memcpy( rec->keys[ i ], hits[ i ].key, 32UL );
  rec->key_cnt = (uchar)cnt;

  return rec;
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_discof_arb_fd_arbnotify_h */
