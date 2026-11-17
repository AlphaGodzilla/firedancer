#ifndef HEADER_fd_src_discof_arb_fd_arbfilter_h
#define HEADER_fd_src_discof_arb_fd_arbfilter_h

/* fd_arbfilter 是一个共享内存对象，存放「这笔交易是否与套利相关」的
   过滤器：

     - 一组 DEX 程序 id；
     - 一组热池账户 key；
     - execrp tile 写入的计数器。

   前两张表都用开放寻址（open addressing：哈希冲突时沿线性探测向后找空
   槽）存放，单次查询是 O(1) 次探测，表本身常驻 L1/L2。

   过滤器由验证器之外的进程配置（见 src/app/firedancer/commands/arb/arb.c
   的 `arb` 动作），execrp tile 在交易热路径上通过 fd_arbfilter_match
   读取它。

   两张配置表由 seqlock（序号锁：写方先用 CAS 把序号从偶数改成奇数表示
   正在写入，写完再改成偶数表示稳定）保护，写方可以在 exec tile 持续执行
   交易的同时整表替换。CAS 保证任一时刻只有一个写方，其余写方自旋等待；
   读方不加锁、不等待，只在序号显示写入尚未完成时按固定次数重试。

   匹配只读两张表：不发起系统调用、不分配内存、不打日志、不做原子读
   改写。本文件中唯一的共享内存写入是写方维护的序号。账户数据不进入
   本对象，这里只存 32 字节 key。 */

#include "../../ballet/txn/fd_txn.h"

/* FD_ARBFILTER_MAGIC 标识一个已格式化的 arbfilter 内存区域。 */
#define FD_ARBFILTER_MAGIC (0x415242464C545231UL) /* "ARBFLTR1" */

/* FD_ARBFILTER_ALIGN 是 arbfilter 内存区域的对齐要求。 */

#define FD_ARBFILTER_ALIGN (128UL)

/* 每张表的「槽数」与「可配置条目上限」是分开的两个量：槽数固定且是 2 的
   幂（由本对象格式化时决定，之后不再改变），条目上限是调用方一次能提交的
   key 个数。条目上限不超过槽数的一半，表因此永远不会进入高装载率状态。

   装载率直接决定热路径的代价：开放寻址线性探测在装载率 α 下，未命中查询
   的期望探测次数约为 (1+1/(1-α)^2)/2，α=0.5 时约 2.5 次，α=1.0 时要扫完
   整张表。热路径上绝大多数交易是未命中，所以上限必须钉在 0.5。

   程序表 128 槽 / 64 条目，整张表 4 KiB；热账户表 2048 槽 / 1024 条目，
   约 66 KiB，常驻 L2。 */

#define FD_ARBFILTER_PROG_SLOT_MAX  (128UL)  /* 槽数，必须是 2 的幂 */
#define FD_ARBFILTER_PROG_ENTRY_MAX (64UL)   /* 可配置的程序 id 上限 */
#define FD_ARBFILTER_ACCT_SLOT_MAX  (2048UL) /* 槽数，必须是 2 的幂 */
#define FD_ARBFILTER_ACCT_ENTRY_MAX (1024UL) /* 可配置的热账户上限 */

/* FD_ARBFILTER_SEQ_RETRY 限制读方在撞上并发写入时重试的次数。用尽后
   查询按未命中返回，这对过滤器是安全一侧的答案（交易最多不被标记）。
   次数必须有界，以免 exec 热路径出现无界自旋。 */

#define FD_ARBFILTER_SEQ_RETRY (8UL)

/* FD_ARBFILTER_BIT_* 是 fd_arbfilter_match 返回的位。 */

#define FD_ARBFILTER_BIT_PROG_DIRECT (1UL) /* R1：指令直接调用了过滤器中的程序 */
#define FD_ARBFILTER_BIT_PROG_ACCOUNT (2UL) /* R2：过滤器中的程序出现在账户列表里 */
#define FD_ARBFILTER_BIT_HOT_ACCOUNT (4UL) /* R3：触及了热池账户 */

/* fd_arbfilter_hit_t 是一个命中的 key。fd_arbfilter_collect 把命中的 key
   按判定顺序去重后写进这种元素的数组，调用方据此把 key 发布给下游。 */

typedef struct {
  uchar key[ 32UL ];
} fd_arbfilter_hit_t;

/* fd_arbfilter_ent 是一张 key 表里的一个槽：32 字节 key 加一个占用标志。
   两张表共用这个元素类型，探测逻辑因此只需要写一份。 */

struct fd_arbfilter_ent {
  uchar key[ 32UL ];
  uchar used;
};

typedef struct fd_arbfilter_ent fd_arbfilter_ent_t;

struct fd_arbfilter_shmem {
  ulong magic;

  /* seqlock 序号：偶数表示两张配置表稳定，奇数表示某个写方正在替换它们。
     写方用 CAS 从偶数抢占成奇数，写完再置成该偶数 +2，读方在查询结束后
     重新读取并与查询前的值比较。 */
  ulong version;

  ulong prog_mask; /* 恒为 FD_ARBFILTER_PROG_SLOT_MAX-1UL，格式化后不再改变 */
  ulong prog_cnt;  /* prog[] 中已使用的槽数 */

  fd_arbfilter_ent_t prog[ FD_ARBFILTER_PROG_SLOT_MAX ];

  ulong acct_mask; /* 恒为 FD_ARBFILTER_ACCT_SLOT_MAX-1UL，格式化后不再改变 */
  ulong acct_cnt;  /* acct[] 中已使用的槽数 */

  fd_arbfilter_ent_t acct[ FD_ARBFILTER_ACCT_SLOT_MAX ];

  /* 由 exec tile 写入，其它进程只读。这些字段是普通计数器，用 relaxed
     原子操作累加，热路径因此不需要加锁。stat_ns 目前恒为 0，保留给匹配
     耗时的累计器。stat_published 只统计命中后真的把记录发布到通知环的
     交易，与 stat_emit 在同一处累加点上区分。 */
  ulong stat_emit;
  ulong stat_skip;
  ulong stat_ns;
  ulong stat_published;
};

typedef struct fd_arbfilter_shmem fd_arbfilter_shmem_t;

/* arbfilter_hash 把 32 字节 key 哈希成探测起点。它对 key 的四个 64 位字
   依次做 FNV-1a 混合：足以把池子 key 打散到整张表，代价是四次装载而不是
   32 次逐字节轮。它不是密码学哈希：唯一的写方是本地受信的 `arb` 动作，
   能构造 key 的一方最多只能拉长自己账户的探测链。 */

FD_FN_PURE static inline ulong
arbfilter_hash( uchar const * key ) {
  ulong h = 0xcbf29ce484222325UL;
  for( ulong i=0UL; i<4UL; i++ ) {
    h ^= FD_LOAD( ulong, key + 8UL*i );
    h *= 0x100000001b3UL;
  }
  return h;
}

/* arbfilter_tbl_find 在槽数为 mask+1 的表里查 key，命中返回槽号，未命中
   返回 ULONG_MAX。空槽是探测链的终点，因此未命中只走完 key 所在的那条
   链，不会扫整张表（装载率上限见文件开头）。

   probe_cnt 只给单测用：传非 NULL 时累加探测次数，正式代码的两个调用点都
   传 NULL，编译器据此把计数分支整段消掉，热路径没有额外指令。

   本函数不做 FD_FN_PURE 标注：读方在 seqlock 重试循环里反复调用它，若允许
   编译器把多次调用的装载合并，重试就失去意义。调用点紧跟在 acquire 装载
   之后，acquire 保证这些装载不会被提到它前面。 */

static inline ulong
arbfilter_tbl_find( fd_arbfilter_ent_t const * ent,
                    ulong                      mask,
                    uchar const *              key,
                    ulong *                    probe_cnt ) {
  ulong idx = arbfilter_hash( key ) & mask;
  for( ulong probe=0UL; probe<=mask; probe++ ) {
    if( FD_UNLIKELY( probe_cnt ) ) (*probe_cnt)++;
    if( !ent[ idx ].used ) break;
    if( FD_UNLIKELY( fd_memeq( ent[ idx ].key, key, 32UL ) ) ) return idx;
    idx = (idx+1UL) & mask;
  }
  return ULONG_MAX;
}

FD_PROTOTYPES_BEGIN

/* fd_arbfilter_{align,footprint} 返回可用作 arbfilter 的内存区域所需
   的对齐与占用字节数。 */

FD_FN_CONST ulong
fd_arbfilter_align( void );

FD_FN_CONST ulong
fd_arbfilter_footprint( void );

/* fd_arbfilter_new 把一块未使用的内存区域格式化为 arbfilter：清零该
   区域，写入 magic，把序号置为 0，并初始化账户表的掩码。成功返回
   shmem，失败返回 NULL 并打印原因。调用方不因此获得 join。 */

void *
fd_arbfilter_new( void * shmem );

/* fd_arbfilter_join 把调用方接入一块 arbfilter 内存区域，成功返回该
   对象在本进程地址空间中的指针，失败返回 NULL 并打印原因（指针为空、
   区域未对齐或 magic 不符）。 */

fd_arbfilter_shmem_t *
fd_arbfilter_join( void * shmem );

/* fd_arbfilter_prog_set 用 cnt 个程序 id 重建程序表
   （cnt<=FD_ARBFILTER_PROG_ENTRY_MAX）。重复的 id 只存一份。成功返回 0；cnt
   越界时返回 -1，此时表保持原样。在 exec tile 执行交易期间调用是安全的。
   写方之间用序号的 CAS 互斥，并发调用不会让两张表交错写入。 */

int
fd_arbfilter_prog_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*ids)[ 32UL ] );

/* fd_arbfilter_acct_set 用 cnt 个 key 重建热账户表
   （cnt<=FD_ARBFILTER_ACCT_ENTRY_MAX）。重复的 key 只存一份。成功返回 0；
   cnt 越界时返回 -1，此时表保持原样。在 exec tile 执行交易期间调用是
   安全的。 */

int
fd_arbfilter_acct_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*keys)[ 32UL ] );

/* fd_arbfilter_prog_idx 返回 key 在程序表中的槽号；key 不在表中，或者
   整个重试预算内都撞上并发替换时返回 ULONG_MAX。查询是 O(1) 次探测：
   与热账户表同为开放寻址，每个被检查的槽只产生一次数据装载。 */

ulong
fd_arbfilter_prog_idx( fd_arbfilter_shmem_t const * f,
                       uchar const *                key );

/* fd_arbfilter_acct_idx 返回 key 在热账户表中的槽号；key 不在表中，
   或者整个重试预算内都撞上并发替换时返回 ULONG_MAX。每个被检查的槽
   只产生一次数据装载，表常驻缓存。 */

ulong
fd_arbfilter_acct_idx( fd_arbfilter_shmem_t const * f,
                       uchar const *                key );

/* arbfilter_hit_push 把 key 追加到 hits[0..*hit_cnt) 的尾部。已经在这个
   前缀里的 key 不重复写入，因此同一笔交易反复触到同一个 key 时只留一份。
   hits_cap 用尽后新 key 直接丢弃：截断只影响 key 列表，不影响匹配结论，
   所以位掩码仍然完整。 */

static inline void
arbfilter_hit_push( fd_arbfilter_hit_t * hits,
                    ulong *              hit_cnt,
                    ulong                hits_cap,
                    uchar const *        key ) {
  ulong cnt = *hit_cnt;
  for( ulong i=0UL; i<cnt; i++ ) if( FD_UNLIKELY( fd_memeq( hits[ i ].key, key, 32UL ) ) ) return;
  if( FD_UNLIKELY( cnt>=hits_cap ) ) return;
  fd_memcpy( hits[ cnt ].key, key, 32UL );
  *hit_cnt = cnt+1UL;
}

/* fd_arbfilter_collect 用过滤器判定一笔交易，返回 FD_ARBFILTER_BIT_* 的
   位掩码，同时把命中的 key 收集到 hits（判定规则与下面 fd_arbfilter_match
   完全一致）：

     R1（PROG_DIRECT）：某条指令调用的程序在程序集合中；
     R2（PROG_ACCOUNT）：程序集合中的某个程序出现在账户列表里，即该
                         交易对 DEX 程序发起了 CPI（跨程序调用）；
     R3（HOT_ACCOUNT）：  账户列表中有热池账户。

   txn 是解析后的交易，payload 是交易的线上字节。accs 是该交易已解析
   的账户 key 列表（静态 key 加上地址查找表解析出的 key），按 32 字节
   步长紧密排列，acc_cnt 是其长度；accs 为 NULL 时改用交易的静态账户
   列表。用字节指针而不是 fd_acct_addr_t 指针，是为了让调用方能直接
   传入 fd_pubkey_t 数组而无需强制类型转换（那会触犯严格别名规则）。

   hits 按判定顺序接收命中的 key，去重，最多写 hits_cap 条，实际条数写
   进 *hit_cnt；hits 为 NULL 时既不读也不写 hit_cnt，此时本函数就是纯判
   定。超过 hits_cap 的命中只丢弃 key，位掩码不受影响。收集 key 时必须扫完
   整个账户列表而不提前退出，否则同一笔交易触到的后续关注 key 会丢失。

   本函数运行在 exec 热路径上：不发起系统调用、不分配内存、不打日志、
   不加锁、不写共享内存。 */

static inline ulong
fd_arbfilter_collect( fd_arbfilter_shmem_t const * filter,
                      fd_txn_t const *             txn,
                      uchar const *                payload,
                      uchar const *                accs,
                      ulong                        acc_cnt,
                      fd_arbfilter_hit_t *         hits,
                      ulong *                      hit_cnt,
                      ulong                        hits_cap ) {
  if( FD_UNLIKELY( !accs ) ) {
    accs    = (uchar const *)fd_txn_get_acct_addrs( txn, payload );
    acc_cnt = (ulong)txn->acct_addr_cnt;
  }

  ulong bits = 0UL;
  ulong cnt  = 0UL;

  /* R1：程序通过账户数组下标直接给出。 */
  for( ulong i=0UL; i<(ulong)txn->instr_cnt; i++ ) {
    ulong prog_idx = (ulong)txn->instr[ i ].program_id;
    if( FD_UNLIKELY( prog_idx>=acc_cnt ) ) continue; /* 交易数据不一致 */
    uchar const * key = accs + prog_idx*FD_TXN_ACCT_ADDR_SZ;
    if( FD_UNLIKELY( fd_arbfilter_prog_idx( filter, key )!=ULONG_MAX ) ) {
      bits |= FD_ARBFILTER_BIT_PROG_DIRECT;
      if( FD_UNLIKELY( hits ) ) arbfilter_hit_push( hits, &cnt, hits_cap, key );
      break;
    }
  }

  /* R2/R3：对解析后的账户列表只遍历一遍。 */
  ulong want = FD_ARBFILTER_BIT_PROG_ACCOUNT | FD_ARBFILTER_BIT_HOT_ACCOUNT;
  for( ulong i=0UL; i<acc_cnt; i++ ) {
    uchar const * key     = accs + i*FD_TXN_ACCT_ADDR_SZ;
    int           in_prog = fd_arbfilter_prog_idx( filter, key )!=ULONG_MAX;
    int           in_acct = fd_arbfilter_acct_idx( filter, key )!=ULONG_MAX;
    if( FD_UNLIKELY( in_prog ) ) bits |= FD_ARBFILTER_BIT_PROG_ACCOUNT;
    if( FD_UNLIKELY( in_acct ) ) bits |= FD_ARBFILTER_BIT_HOT_ACCOUNT;
    if( FD_UNLIKELY( hits && ( in_prog || in_acct ) ) ) arbfilter_hit_push( hits, &cnt, hits_cap, key );
    /* 只有纯判定（hits 为 NULL）时才提前退出：收集 key 的调用方要拿到这
       笔交易触到的全部关注 key，提前退出会让后面的命中丢失。 */
    if( FD_UNLIKELY( !hits && bits==( FD_ARBFILTER_BIT_PROG_DIRECT|want ) ) ) break;
  }

  if( FD_LIKELY( hits ) ) *hit_cnt = cnt;
  return bits;
}

/* fd_arbfilter_match 只给出判定结果：它是 fd_arbfilter_collect 不收集 key
   的薄封装，命中规则与返回值逐位相同，供只需要「是否相关」的调用点使用。
   本函数同样不发起系统调用、不分配内存、不打日志、不加锁、不写共享内存。 */

static inline ulong
fd_arbfilter_match( fd_arbfilter_shmem_t const * f,
                    fd_txn_t const *              txn,
                    uchar const *                 payload,
                    uchar const *                 accs,
                    ulong                         acc_cnt ) {
  return fd_arbfilter_collect( f, txn, payload, accs, acc_cnt, NULL, NULL, 0UL );
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_discof_arb_fd_arbfilter_h */
