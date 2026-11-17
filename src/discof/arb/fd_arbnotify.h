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

/* FD_ARBNOTIFY_KEY_MAX 是一条记录最多携带的命中 key 条数。同一笔交易的可
   提交命中多于这个数时按片发布（见 part/more），命中不丢失；matched 位掩
   码始终完整。 */

#define FD_ARBNOTIFY_KEY_MAX (8UL)

/* FD_ARBNOTIFY_PROG_TAG_MAX 是一条记录最多携带的关注程序 tag 条数。程序账户
   永远是只读的、不会被提交，因此它的 key 不会出现在 keys[] 里；程序表的命
   中只以 tag 的形式随记录发布，读者据此知道这笔交易调用了哪些关注程序。 */

#define FD_ARBNOTIFY_PROG_TAG_MAX (8UL)

struct fd_arbnotify {
  ulong  slot;             /* 交易所在 slot */
  ulong  bank_seq;         /* bank 序号，同一 slot 内排序用 */
  ulong  index_in_slot;    /* 这笔交易在所属区块内的 0 起始位置，是权威排序键 */
  ushort accdb_fork_id;    /* 该交易执行时 bank 的 accdb fork id */
  uint   accdb_generation; /* 该 fork 的 generation，与 fork id 一起标识一次 fork 生命周期 */
  ushort matched;          /* FD_ARBFILTER_BIT_* 位掩码，含 readonly 命中 */
  uchar  key_cnt;          /* keys[] 中有效条数，已去重 */
  uchar  part;             /* 同一笔交易的第几片，从 0 起 */
  uchar  more;             /* 1 表示该交易还有后续分片 */
  uchar  _pad;
  uchar  sig[ 64 ];        /* 交易签名，供外部程序对账 */
  uchar  keys[ FD_ARBNOTIFY_KEY_MAX ][ 32 ];
  uchar  tags[ FD_ARBNOTIFY_KEY_MAX ]; /* 与 keys 一一对应，0 表示无 tag */
  uchar  prog_tag_cnt;                                   /* prog_tag[] 中有效条数 */
  uchar  prog_tag[ FD_ARBNOTIFY_PROG_TAG_MAX ];          /* 命中的关注程序表项 tag，已去重 */
};

typedef struct fd_arbnotify fd_arbnotify_t;

/* 记录的定长决定了 link 的 mtu：mtu 必须直接取 sizeof(fd_arbnotify_t)，
   禁止在拓扑里另写数字。

   尺寸是 36（定长字段，含 index_in_slot）+ 64（sig）+ 8*32（key）+ 8（key tag）
   + 1（prog_tag_cnt）+ 8（prog_tag）= 373，按 8 字节对齐到 376。dcache 的槽按
   128 字节对齐，FD_DCACHE_SLOT_FOOTPRINT( 376 ) == 384，与更小的记录占用同样
   大的槽，因此 index_in_slot、key tag 与 prog tag 都不增加任何环内存。

   index_in_slot 是交易在区块里的位置，也是唯一能跨环恢复顺序的键：冲突的交易
   由调度器串行执行，但可能落在不同 execrp tile 的环里，读者按环轮询会把它们
   的观察顺序打乱。

   外部读者必须先用 props 里的 rec_sz 校验本结构体的尺寸再解读记录：记录是
   定长裸布局，尺寸不一致时字段偏移全部失效。 */

FD_STATIC_ASSERT( sizeof(fd_arbnotify_t)==376UL, fd_arbnotify_layout );

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
   整条记录清零，再写各字段与 hits[0..hit_cnt) 的 key 与 tag，尾部未用到的
   key 槽、tag 槽与填充字节因此都是确定的 0。hit_cnt 超过
   FD_ARBNOTIFY_KEY_MAX 时只写前 FD_ARBNOTIFY_KEY_MAX 条，key_cnt 记录实际
   写入的条数，调用方据 part/more 继续发布后续分片。

   part 是同一笔交易的第几片（从 0 起），more 为 1 表示该交易还有后续分片：
   可提交命中超过一条记录的容量时，调用方按片发布多条记录，读者据此识别
   同一笔交易的多片而不能当成重复记录。

   sig 指向 64 字节签名，为 NULL 时签名区写全 0（交易没有签名）。hits 为
   NULL 时按 hit_cnt==0 处理。 */

static inline fd_arbnotify_t *
fd_arbnotify_fill( fd_arbnotify_t *           rec,
                   ulong                      slot,
                   ulong                      bank_seq,
                   ulong                      index_in_slot,
                   ushort                     accdb_fork_id,
                   uint                       accdb_generation,
                   ushort                     matched,
                   uchar const *              sig,
                   fd_arbfilter_hit_t const * hits,
                   ulong                      hit_cnt,
                   uchar const *              prog_tags,
                   ulong                      prog_tag_cnt,
                   uchar                      part,
                   uchar                      more ) {
  fd_memset( rec, 0, sizeof(fd_arbnotify_t) );

  rec->slot             = slot;
  rec->bank_seq         = bank_seq;
  rec->index_in_slot    = index_in_slot;
  rec->accdb_fork_id    = accdb_fork_id;
  rec->accdb_generation = accdb_generation;
  rec->matched          = matched;
  rec->part             = part;
  rec->more             = more;

  if( FD_LIKELY( sig ) ) fd_memcpy( rec->sig, sig, 64UL );

  ulong cnt = hits ? fd_ulong_min( hit_cnt, FD_ARBNOTIFY_KEY_MAX ) : 0UL;
  for( ulong i=0UL; i<cnt; i++ ) {
    fd_memcpy( rec->keys[ i ], hits[ i ].key, 32UL );
    rec->tags[ i ] = hits[ i ].tag;
  }
  rec->key_cnt = (uchar)cnt;

  ulong prog_cnt = ( prog_tags && prog_tag_cnt ) ? fd_ulong_min( prog_tag_cnt, FD_ARBNOTIFY_PROG_TAG_MAX ) : 0UL;
  for( ulong i=0UL; i<prog_cnt; i++ ) rec->prog_tag[ i ] = prog_tags[ i ];
  rec->prog_tag_cnt = (uchar)prog_cnt;

  return rec;
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_discof_arb_fd_arbnotify_h */
