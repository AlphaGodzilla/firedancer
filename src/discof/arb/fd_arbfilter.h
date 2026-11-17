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

/* FD_ARBFILTER_MAGIC 标识一个已格式化的 arbfilter 内存区域。结尾的数字是
   内存布局版本：只要结构体布局发生变化（表项加字段、计数器增删等）就必须
   把版本号加一，否则新二进制 join 旧验证器的共享内存时校验会通过，而按新
   布局解读旧数据会静默错位。当前版本对应 fd_arbfilter_ent 带 tag 字段。 */
#define FD_ARBFILTER_MAGIC (0x415242464C545232UL) /* "ARBFLTR2" */

/* FD_ARBFILTER_ALIGN 是 arbfilter 内存区域的对齐要求。 */

#define FD_ARBFILTER_ALIGN (128UL)

/* 每张表的「槽数」与「可配置条目上限」是分开的两个量：槽数固定且是 2 的
   幂（由本对象格式化时决定，之后不再改变），条目上限是调用方一次能提交的
   key 个数。条目上限不超过槽数的一半，表因此永远不会进入高装载率状态。

   装载率直接决定热路径的代价：开放寻址线性探测在装载率 α 下，未命中查询
   的期望探测次数约为 (1+1/(1-α)^2)/2，α=0.5 时约 2.5 次，α=1.0 时要扫完
   整张表。热路径上绝大多数交易是未命中，所以上限必须钉在 0.5。

   程序表 128 槽 / 64 条目，整张表约 4.25 KiB；热账户表 2048 槽 / 1024
   条目，约 68 KiB，常驻 L2。 */

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

/* fd_arbfilter_hit_t 是一个命中的 key 与它所在表项的 tag。fd_arbfilter_collect
   把会被提交进 bank 的命中 key 按判定顺序去重后写进这种元素的数组，调用方
   据此把 key 与 tag 发布给下游。

   tag 只随记录发布，不参与任何判定：同一个 key 无论 tag 是什么，命中结论与
   位掩码都相同。 */

typedef struct {
  uchar key[ 32UL ];
  uchar tag;
} fd_arbfilter_hit_t;

/* fd_arbfilter_ent 是一张 key 表里的一个槽：32 字节 key、一个可选 tag 与一个
   占用标志。两张表共用这个元素类型，探测逻辑因此只需要写一份。

   tag 由安装表的一方给出，0 表示未设置。它不参与探测与判定，只在命中时随
   该表项的 key 一起发布出去。 */
struct fd_arbfilter_ent {
  uchar key[ 32UL ];
  uchar tag;
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
     耗时的累计器。

     stat_emit 按交易累加命中，stat_skip 按交易累加未命中，两者互斥且覆盖
     全部交易。命中的交易再分两类：没有任何可提交账户（例如只命中了程序
     账户）的不发布记录，进 stat_noacct；发布了记录的进 stat_published，
     stat_published 按交易累加（一笔交易无论发几片都只加一），因此恒等式
     stat_emit == stat_published + stat_noacct 恒成立。stat_records 按环里
     实际的记录条数累加（一片加一），跨片交易多于一条，故
     stat_records >= stat_published，两者相等说明没有任何交易需要跨片。 */
  ulong stat_emit;
  ulong stat_skip;
  ulong stat_ns;
  ulong stat_published;
  ulong stat_noacct;
  ulong stat_records;
};

typedef struct fd_arbfilter_shmem fd_arbfilter_shmem_t;

/* arbfilter_hash 把 32 字节 key 哈希成探测起点。它对 key 的四个 64 位字
   依次做 FNV-1a 混合：足以把池子 key 打散到整张表，代价是四次装载而不是
   32 次逐字节轮。它不是密码学哈希：唯一的写方是本地受信的 `arb` 动作，
   能构造 key 的一方最多只能拉长自己账户的探测链。

   实测（test_arbfilter bench，同机同轮）：换成 fd_hash32 后平均探测格数从
   2.02 降到 1.96，但每次查询反而更慢——在这份构建里 fd_hash32 走的是「每字
   一次 fd_ulong_hash」的 fallback，多出来的乘法延迟盖过了少走的那一格。改
   哈希之前必须重新跑这个基准，不要只看混合质量。 */

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

   比较用 fd_memeq：x86 上它编译成 `repe cmpsb`，有 FSRM（Fast Short REP）
   的机器上对 32 字节定长比较比展开成 4 次 64 位装载更快——微基准实测 5.54 ns
   对 7.47 ns。不要想当然地把它换成手写整数比较。

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
   （cnt<=FD_ARBFILTER_PROG_ENTRY_MAX）。tags 是 cnt 个与 ids 逐条对应的 tag，
   NULL 表示全部为 0。重复的 id 只存一份，且首个出现的 tag 生效。成功返回 0；
   cnt 越界时返回 -1，此时表保持原样。在 exec tile 执行交易期间调用是安全的。
   写方之间用序号的 CAS 互斥，并发调用不会让两张表交错写入。 */

int
fd_arbfilter_prog_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*ids)[ 32UL ],
                       uchar const *          tags );

/* fd_arbfilter_acct_set 用 cnt 个 key 重建热账户表
   （cnt<=FD_ARBFILTER_ACCT_ENTRY_MAX）。tags 是 cnt 个与 keys 逐条对应的 tag，
   NULL 表示全部为 0。重复的 key 只存一份，且首个出现的 tag 生效。成功返回 0；
   cnt 越界时返回 -1，此时表保持原样。在 exec tile 执行交易期间调用是
   安全的。 */

int
fd_arbfilter_acct_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*keys)[ 32UL ],
                       uchar const *          tags );

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

/* arbfilter_hit_push 把 key 与它的 tag 追加到 hits[0..*hit_cnt) 的尾部。已经
   在这个前缀里的 key 不重复写入，因此同一笔交易反复触到同一个 key 时只留
   一份，tag 随它第一次出现时的那份。hits_cap 用尽后新 key 直接丢弃：截断只
   影响 key 列表，不影响匹配结论。调用方 arbfilter_hit_take 在调用前已经确认
   还有空位。 */

static inline void
arbfilter_hit_push( fd_arbfilter_hit_t * hits,
                    ulong *              hit_cnt,
                    ulong                hits_cap,
                    uchar const *        key,
                    uchar                tag ) {
  ulong cnt = *hit_cnt;
  for( ulong i=0UL; i<cnt; i++ ) if( FD_UNLIKELY( fd_memeq( hits[ i ].key, key, 32UL ) ) ) return;
  if( FD_UNLIKELY( cnt>=hits_cap ) ) return;
  fd_memcpy( hits[ cnt ].key, key, 32UL );
  hits[ cnt ].tag = tag;
  *hit_cnt = cnt+1UL;
}

/* arbfilter_committable 判断第 i 个账户是否会被提交进 bank，判据与
   fd_runtime_commit_txn 的提交循环逐条对应：交易本身没有失败，账户可写、
   执行期被改动过，并且本笔交易持有它的 accdb 引用（bundle 里只有最后一笔
   可写交易提交同一账户）。任一掩码为 NULL 时该维度不设限，供纯判定与单测
   调用。 */

static inline int
arbfilter_committable( uchar const * writable,
                       uchar const * touched,
                       uchar const * acquired,
                       int           commit_ok,
                       ulong         i ) {
  return commit_ok &&
         ( !writable || writable[ i ] ) &&
         ( !touched  || touched [ i ] ) &&
         ( !acquired || acquired[ i ] );
}

/* arbfilter_emitted_test / arbfilter_emitted_set 查询与置位「索引 i 已经
   发布过」的位。emitted 是 64 位索引位图，acc_cnt 必须不超过
   MAX_TX_ACCOUNT_LOCKS（64）；超出 64 的索引不参与位图，这些账户在分片
   之间可能被重复发布。 */

static inline int
arbfilter_emitted_test( ulong const * emitted,
                        ulong         i ) {
  return emitted && i<64UL && !!( *emitted & (1UL<<i) );
}

static inline void
arbfilter_emitted_set( ulong * emitted,
                       ulong   i ) {
  if( emitted && i<64UL ) *emitted |= 1UL<<i;
}

/* arbfilter_hit_take 把一个可提交的命中 key 与它的 tag 收进本片：已经发布过
   的索引跳过；本片已满时置 *more 并把该索引留给下一片；否则去重写入并置位
   索引。位掩码与这里无关，调用方只在命中且满足提交判据时才调用。 */

static inline void
arbfilter_hit_take( fd_arbfilter_hit_t * hits,
                    ulong *              hit_cnt,
                    ulong                hits_cap,
                    ulong *              emitted,
                    ulong                i,
                    uchar const *        key,
                    uchar                tag,
                    ulong *              more ) {
  /* 位图只有 64 位：索引装不下的账户既测不到也置不上，必须直接跳过。否则
     这类索引每一片都会被当成「未发布」，*more 永远为 1，调用方的分片循环
     不会终止。运行时保证 acc_cnt 不超过 64，这里仍然显式挡住。 */
  if( FD_UNLIKELY( i>=64UL ) ) return;
  if( arbfilter_emitted_test( emitted, i ) ) return;
  if( FD_UNLIKELY( *hit_cnt>=hits_cap ) ) { if( more ) *more = 1UL; return; }
  arbfilter_hit_push( hits, hit_cnt, hits_cap, key, tag );
  arbfilter_emitted_set( emitted, i );
}

/* arbfilter_prog_tag_add 把一个关注程序表项的 tag 追加到 tags[0..*cnt)，去重。
   tag 为 0 表示该表项没有设置标签，直接忽略：记录里的 prog_tag 用于区分关注
   程序，全为 0 时携带它没有意义。容量用尽后新 tag 丢弃，不影响命中判定与
   其他字段。 */

static inline void
arbfilter_prog_tag_add( uchar * tags,
                        ulong * cnt,
                        ulong   cap,
                        uchar   tag ) {
  if( FD_UNLIKELY( !tag ) ) return;
  ulong c = *cnt;
  for( ulong i=0UL; i<c; i++ ) if( FD_UNLIKELY( tags[ i ]==tag ) ) return;
  if( FD_UNLIKELY( c>=cap ) ) return;
  tags[ c ] = tag;
  *cnt = c+1UL;
}

/* fd_arbfilter_collect 用过滤器判定一笔交易，返回 FD_ARBFILTER_BIT_* 的
   位掩码，同时把「会提交进 bank 的命中账户」收集到 hits（判定规则与下面
   fd_arbfilter_match 完全一致）：

     R1（PROG_DIRECT）：某条指令调用的程序在程序集合中；
     R2（PROG_ACCOUNT）：程序集合中的某个程序出现在静态账户列表里，即该
                         交易对 DEX 程序发起了 CPI（跨程序调用）；
     R3（HOT_ACCOUNT）：  热账户表命中一个会被提交进 bank 的账户。

   判定分三段，顺序即短路顺序：
     1. 先遍历指令的 program_id 查程序表（R1）。program_id 是静态账户列表的
        下标，因此这一步不碰账户表。
     2. 再扫静态账户区间查程序表（R2）。CPI 内嵌调用的程序不会出现在顶层指令
        里，只能靠它被列进账户来发现，所以这一遍不能省。
     3. 前两段都没命中任何关注程序时**立即返回 0**，热账户表一条也不探测。池
        账户只会被它自己的程序改动，而那个程序必然出现在账户列表里（即第 2 段
        已命中），所以这个短路不会漏掉真实的池子更新。命中程序之后，只对满足
        提交判据的账户探测热账户表（R3），不对全部账户做哈希探测。

   由此，只有热账户变动、完全不涉及关注程序的交易**不算命中**（返回 0）：
   这是「程序表先行」的直接结果，也是热路径上最省的一种取舍。

   txn 是解析后的交易，payload 是交易的线上字节。accs 是该交易已解析
   的账户 key 列表（静态 key 加上地址查找表解析出的 key），按 32 字节
   步长紧密排列，acc_cnt 是其长度；accs 为 NULL 时改用交易的静态账户
   列表。用字节指针而不是 fd_acct_addr_t 指针，是为了让调用方能直接
   传入 fd_pubkey_t 数组而无需强制类型转换（那会触犯严格别名规则）。

   writable / touched / acquired 是 fd_txn_out_t.accounts 里同名 per-account
   标志（is_writable / touched / account_acquired），commit_ok 表示这笔交易
   会提交。三者与 commit_ok 一起复刻 fd_runtime_commit_txn 的提交判据：
   只有 commit_ok && writable[i] && touched[i] && acquired[i] 的命中账户才进
   hits。任一掩码为 NULL 时该维度不设限，供纯判定与单测使用。命中但不满足
   提交判据的账户不收集，位掩码不受影响。
   hits 按判定顺序接收命中 key 与它的 tag，去重，最多写 hits_cap 条，实际条
   数写进 *hit_cnt；hits 为 NULL 时既不读也不写 hit_cnt，此时本函数就是纯判
   定。tag 取表项上的 tag：R1 与 R2 命中取程序表项，R3 命中取热账户表项；同
   一个 key 同时命中两张表时取热账户表的 tag，该 tag 为 0 时退回程序表的
   tag。tag 不参与判定，位掩码与返回值不受它影响。

   emitted 是 64 位已发布索引位图：分片发布时调用方传同一份，已置位的索引
   本片跳过，因此分片之间不重复；acc_cnt 必须不超过 64（该位图是一整字）。
   去重只发生在一条记录之内：同一个 key 出现在多个账户下标、而这些下标被分
   到不同片时，该 key 会在每片各发布一次。
   收集满 hits_cap 而仍有未发布的可提交命中时 *more 置 1，调用方据此继续
   下一片；收集时 *more 每次调用都重写为 0 或 1。收集 key 时必须扫完整个
   账户列表而不提前退出，否则同一笔交易触到的后续关注 key 会丢失。

   本函数运行在 exec 热路径上：不发起系统调用、不分配内存、不打日志、
   不加锁、不写共享内存。 */

static inline ulong
fd_arbfilter_collect( fd_arbfilter_shmem_t const * filter,
                      fd_txn_t const *             txn,
                      uchar const *                payload,
                      uchar const *                accs,
                      ulong                        acc_cnt,
                      uchar const *                writable,
                      uchar const *                touched,
                      uchar const *                acquired,
                      int                          commit_ok,
                      ulong *                      emitted,
                      fd_arbfilter_hit_t *         hits,
                      ulong *                      hit_cnt,
                      ulong *                      more,
                      uchar *                      prog_tags,
                      ulong *                      prog_tag_cnt,
                      ulong                        prog_tag_cap,
                      ulong                        hits_cap ) {
  if( FD_UNLIKELY( !accs ) ) {
    accs    = (uchar const *)fd_txn_get_acct_addrs( txn, payload );
    acc_cnt = (ulong)txn->acct_addr_cnt;
  }

  /* accs 是解析后的账户列表：前 acct_addr_cnt 条是交易自带的静态 key，地址
     查找表加载出来的 key 排在后面。program_id 是静态 key 的下标，因此程序表
     的探测只需要扫静态区间。 */
  ulong static_cnt = fd_ulong_min( (ulong)txn->acct_addr_cnt, acc_cnt );

  ulong bits = 0UL;
  ulong cnt  = 0UL;
  if( hits && more ) *more = 0UL; /* 纯判定（hits 为 NULL）时不写调用方的变量 */
  if( prog_tag_cnt ) *prog_tag_cnt = 0UL;

  /* 第一遍：指令直调的程序（R1）。program_id 直接给出账户下标，不必扫账户
     表。命中后把该程序表项的 tag 记进 prog_tag。 */
  for( ulong i=0UL; i<(ulong)txn->instr_cnt; i++ ) {
    ulong prog_idx = (ulong)txn->instr[ i ].program_id;
    if( FD_UNLIKELY( prog_idx>=static_cnt ) ) continue; /* 交易数据不一致 */
    uchar const * key  = accs + prog_idx*FD_TXN_ACCT_ADDR_SZ;
    ulong         slot = fd_arbfilter_prog_idx( filter, key );
    if( FD_UNLIKELY( slot!=ULONG_MAX ) ) {
      bits |= FD_ARBFILTER_BIT_PROG_DIRECT;
      /* 程序账户永远只读、不会被提交，它的 key 进不了 keys[]；这里改记表项
         的 tag，读者据此知道这笔交易调用了哪个关注程序。 */
      if( FD_UNLIKELY( prog_tag_cnt && prog_tags ) )
        arbfilter_prog_tag_add( prog_tags, prog_tag_cnt, prog_tag_cap, filter->prog[ slot ].tag );
      if( FD_UNLIKELY( hits && arbfilter_committable( writable, touched, acquired, commit_ok, prog_idx ) ) )
        arbfilter_hit_take( hits, &cnt, hits_cap, emitted, prog_idx, key, filter->prog[ slot ].tag, more );
      break;
    }
  }

  /* 第二遍：关注程序作为账户出现在交易里（R2）。CPI 内嵌调用的程序不会出现在
     顶层指令里，只能靠它是否被列进账户来发现，所以这一遍不能省。同样只扫静
     态区间：程序 key 不可能由地址查找表加载。

     这里**不能提前退出**：一笔多跳交易可能同时把几个关注程序列进账户，prog_tag
     必须把它们都记下来；只有位掩码一位就够，但 tag 列表要完整。 */
  for( ulong i=0UL; i<static_cnt; i++ ) {
    uchar const * key  = accs + i*FD_TXN_ACCT_ADDR_SZ;
    ulong         slot = fd_arbfilter_prog_idx( filter, key );
    if( FD_UNLIKELY( slot==ULONG_MAX ) ) continue;
    bits |= FD_ARBFILTER_BIT_PROG_ACCOUNT;
    if( FD_UNLIKELY( prog_tag_cnt && prog_tags ) )
      arbfilter_prog_tag_add( prog_tags, prog_tag_cnt, prog_tag_cap, filter->prog[ slot ].tag );
  }

  /* 短路：两遍都没有命中任何关注程序，这笔交易与关注的程序无关，热账户表一
     条也不必探测。绝大多数交易在这里直接返回。

     已知且刻意的缺口：写账户的程序不在程序表里时，即使账户本身在热账户表内
     也不会发布。最现实的一种是直接给池子账户转 SOL——交易里只有 system 程
     序，池账户的 lamports 变了（对以 lamports 计价的池子就是真实的价格变
     动），本函数仍会返回 0。要覆盖这种情况，只需去掉这个短路：第三段本来就
     只对「会被提交」的账户探测，代价是每笔交易多几次哈希探测。 */
  if( FD_UNLIKELY( !( bits & ( FD_ARBFILTER_BIT_PROG_DIRECT | FD_ARBFILTER_BIT_PROG_ACCOUNT ) ) ) ) {
    if( FD_LIKELY( hits ) ) *hit_cnt = 0UL;
    return 0UL;
  }

  /* 第三遍：只在「会被提交进 bank」的账户里查热账户表。声明可写但执行期没
     被改动、以及 bundle 里不持有 accdb 引用的账户都不会提交，对它们做哈希探
     测纯属浪费。 */
  for( ulong i=0UL; i<acc_cnt; i++ ) {
    if( FD_UNLIKELY( !arbfilter_committable( writable, touched, acquired, commit_ok, i ) ) ) continue;

    uchar const * key       = accs + i*FD_TXN_ACCT_ADDR_SZ;
    ulong         acct_slot = fd_arbfilter_acct_idx( filter, key );
    if( FD_UNLIKELY( acct_slot==ULONG_MAX ) ) continue;

    bits |= FD_ARBFILTER_BIT_HOT_ACCOUNT;
    if( FD_UNLIKELY( hits ) ) {
      /* 两张表都命中时以热账户表的 tag 为准（池账户离套利标的更近）；它的 tag
         为 0 时退回程序表的 tag。程序表的 tag 已经在上面记进 prog_tag，这里
         只影响 key 自己的 tag。 */
      ulong prog_slot = fd_arbfilter_prog_idx( filter, key );
      uchar tag       = filter->acct[ acct_slot ].tag;
      if( FD_UNLIKELY( !tag && prog_slot!=ULONG_MAX ) ) tag = filter->prog[ prog_slot ].tag;
      arbfilter_hit_take( hits, &cnt, hits_cap, emitted, i, key, tag, more );
    }
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
  return fd_arbfilter_collect( f, txn, payload, accs, acc_cnt,
                               NULL, NULL, NULL, 1, NULL,
                               NULL, NULL, NULL, NULL, NULL, 0UL, 0UL );
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_discof_arb_fd_arbfilter_h */
