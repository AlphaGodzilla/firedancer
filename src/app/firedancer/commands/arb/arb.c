#include "../../../shared/fd_config.h"
#include "../../../shared/fd_action.h"
#include "../../../shared/fd_bootinfo.h"

#include "../../../../disco/topo/fd_topo.h"
#include "../../../../tango/fseq/fd_fseq.h"
#include "../../../../discof/arb/fd_arbfilter.h"
#include "../../../../discof/arb/fd_arbnotify.h"
#include "../../../../tango/mcache/fd_mcache.h"
#include "../../../../tango/dcache/fd_dcache.h"
#include "../../../../flamenco/accdb/fd_accdb.h"
#include "../../../../flamenco/accdb/fd_accdb_shmem.h"
#include "../../../../flamenco/accdb/fd_accdb_private.h" /* fd_accdb_shmem_t::root_fork_id */
#include "../../../../flamenco/runtime/fd_runtime_const.h"
#include "../../../../ballet/base58/fd_base58.h"
#include "../../../../util/pod/fd_pod.h"
#include "../../../../util/pod/fd_pod_format.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* arb_cmd_fn 在没有子命令时打印本动作自己的帮助。 */
extern action_t fd_action_arb;

/* `arb` 是验证器之外的诊断工具，直接读写运行中验证器的共享内存：
   filter-dump 转储过滤器配置与统计，filter-set 热替换配置，account 经
   只读 accdb join 读出一个账户。三条路径都不启动 tile、不建立链接。 */

/* 命令行上限定义在 fd_action.h（fdctl_shared 层，不依赖 discof），这里
   把它们与 arbfilter 对象的实际容量钉在一起：任何一侧改动都会编译失败。 */

FD_STATIC_ASSERT( FD_ACTION_ARB_PROG_MAX==FD_ARBFILTER_PROG_ENTRY_MAX,   arb_prog_max  );
FD_STATIC_ASSERT( FD_ACTION_ARB_ACCT_MAX==FD_ARBFILTER_ACCT_ENTRY_MAX,   arb_acct_max  );

/* arb_join_filter 把 arbfilter 工作区按 mode 映射进本进程，返回其中的
   过滤器对象。mode 为 FD_SHMEM_JOIN_MODE_READ_ONLY 时只允许读取。 */

static fd_arbfilter_shmem_t *
arb_join_filter( config_t * config,
                 int        mode ) {
  fd_topo_t * topo = &config->topo;

  ulong wksp_id = fd_topo_find_wksp( topo, "arbfilter" );
  if( FD_UNLIKELY( wksp_id==ULONG_MAX ) )
    FD_LOG_ERR(( "workspace `arbfilter` not found in the topology, the running validator was built "
                 "before arbfilter existed or is running a different binary" ));

  ulong obj_id = fd_pod_query_ulong( topo->props, "arbfilter", ULONG_MAX );
  if( FD_UNLIKELY( obj_id==ULONG_MAX ) )
    FD_LOG_ERR(( "arbfilter object not found in the topology properties" ));

  fd_topo_join_workspace( topo, &topo->workspaces[ wksp_id ], mode, FD_TOPO_CORE_DUMP_LEVEL_DISABLED );

  fd_arbfilter_shmem_t * filter = fd_arbfilter_join( fd_topo_obj_laddr( topo, obj_id ) );
  if( FD_UNLIKELY( !filter ) )
    FD_LOG_ERR(( "failed to join the arbfilter object, the running validator is probably using an "
                 "incompatible version of this binary" ));

  return filter;
}

/* arb_print_filter 打印过滤器当前的配置与统计。stat_ns 保留在对象里但恒为
   0，打印出来会被读成「匹配耗时 0 ns」，因此不输出。统计由 exec tile 并发
   累加，这里按 relaxed 语义各读一次，得到的是转储瞬间的近似值。 */

static void
arb_print_filter( fd_arbfilter_shmem_t const * filter ) {
  printf( "version   %lu\n", filter->version );
  printf( "programs  %lu / %lu entries\n", filter->prog_cnt, FD_ARBFILTER_PROG_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_SLOT_MAX; i++ ) {
    if( !filter->prog[ i ].used ) continue;
    FD_BASE58_ENCODE_32_BYTES( filter->prog[ i ].key, prog_b58 );
    printf( "  [%lu] %s", i, prog_b58 );
    if( filter->prog[ i ].tag ) printf( " tag=%u", (uint)filter->prog[ i ].tag );
    printf( "\n" );
  }

  printf( "accounts  %lu / %lu entries\n", filter->acct_cnt, FD_ARBFILTER_ACCT_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_SLOT_MAX; i++ ) {
    if( !filter->acct[ i ].used ) continue;
    FD_BASE58_ENCODE_32_BYTES( filter->acct[ i ].key, acct_b58 );
    printf( "  [%lu] %s", i, acct_b58 );
    if( filter->acct[ i ].tag ) printf( " tag=%u", (uint)filter->acct[ i ].tag );
    printf( "\n" );
  }

  /* 四个计数器分成两组看：stat_emit / stat_skip 按交易互斥累加，恒有
     stat_emit + stat_skip == 执行过的交易数；命中的交易再分两类，
     stat_emit == stat_published + stat_noacct 恒成立（stat_noacct 是命中了
     但没有任何可提交账户、因而没有发布记录的交易）。
     stat_records 按环里实际的记录条数累加：一笔交易的命中账户超过一条记录
     的容量时按片发布，因此 stat_records >= stat_published，两者相等说明没有
     任何交易跨片。 */
  printf( "stat_emit %lu\n",      FD_VOLATILE_CONST( filter->stat_emit ) );
  printf( "stat_skip %lu\n",      FD_VOLATILE_CONST( filter->stat_skip ) );
  printf( "stat_published %lu\n", FD_VOLATILE_CONST( filter->stat_published ) );
  printf( "stat_noacct %lu\n",    FD_VOLATILE_CONST( filter->stat_noacct ) );
  printf( "stat_records %lu\n",   FD_VOLATILE_CONST( filter->stat_records ) );
}

static void
arb_filter_dump( config_t * config ) {
  fd_arbfilter_shmem_t const * filter = arb_join_filter( config, FD_SHMEM_JOIN_MODE_READ_ONLY );
  arb_print_filter( filter );
}

/* arb_decode_keys 把 cnt 个 `base58` 或 `base58:tag` 参数解码进 out（out 由
   调用方分配 cnt*32 字节），并把每个参数的 tag 写进 tags[i]（没有后缀时为
   0）。任何一条解不出来或后缀非法都直接退出，此时运行中的过滤表还没有被改
   动。

   tag 是 0..255 的十进制数。base58 字母表不含 `:`，因此按 `:` 切分是安全
   的；空后缀、非数字后缀、超出 0..255 的值与多于一个 `:` 一律报错，禁止猜
   测调用方想表达什么。 */

static void
arb_decode_keys( char const * const * vals,
                 ulong                cnt,
                 uchar                (*out)[ 32UL ],
                 uchar *              tags,
                 char const *         flag ) {
  for( ulong i=0UL; i<cnt; i++ ) {
    char const * val   = vals[ i ];
    char const * colon = strchr( val, ':' );

    /* 只用 `:` 之前的部分解码：fd_base58_decode_32 要求以 nul 结尾的字符串，
       因此先拷进栈上缓冲区。 */
    ulong key_len = colon ? (ulong)( colon-val ) : strlen( val );
    if( FD_UNLIKELY( key_len>=FD_BASE58_ENCODED_32_SZ ) )
      FD_LOG_ERR(( "%s: invalid base58 public key `%s`", flag, val ));

    char encoded[ FD_BASE58_ENCODED_32_SZ ];
    memcpy( encoded, val, key_len );
    encoded[ key_len ] = '\0';
    if( FD_UNLIKELY( !fd_base58_decode_32( encoded, out[ i ] ) ) )
      FD_LOG_ERR(( "%s: invalid base58 public key `%s`", flag, val ));

    tags[ i ] = 0U;
    if( !colon ) continue;

    char const * suffix = colon+1;
    if( FD_UNLIKELY( !*suffix ) )
      FD_LOG_ERR(( "%s: `%s` has an empty tag", flag, val ));
    if( FD_UNLIKELY( strchr( suffix, ':' ) ) )
      FD_LOG_ERR(( "%s: `%s` has more than one tag separator", flag, val ));

    ulong tag = 0UL;
    for( char const * p=suffix; *p; p++ ) {
      if( FD_UNLIKELY( *p<'0' || *p>'9' ) )
        FD_LOG_ERR(( "%s: `%s` tag `%s` is not a decimal number", flag, val, suffix ));
      tag = tag*10UL + (ulong)( *p-'0' );
      if( FD_UNLIKELY( tag>255UL ) )
        FD_LOG_ERR(( "%s: `%s` tag `%s` must be in 0..255", flag, val, suffix ));
    }
    tags[ i ] = (uchar)tag;
  }
}

static void
arb_filter_set( args_t *   args,
                config_t * config ) {
  if( FD_UNLIKELY( !args->arb.prog_cnt && !args->arb.acct_cnt ) )
    FD_LOG_ERR(( "filter-set requires at least one --prog or --acct" ));

  /* 命令行只记 base58 指针，解码与 tag 解析在这里做：失败时两张表都还没
     动。 */
  uchar (*prog_ids )[ 32UL ] = NULL;
  uchar (*acct_keys)[ 32UL ] = NULL;
  uchar * prog_tags = NULL;
  uchar * acct_tags = NULL;
  if( args->arb.prog_cnt ) {
    prog_ids  = (uchar (*)[32UL])malloc( args->arb.prog_cnt*32UL );
    prog_tags = (uchar *)malloc( args->arb.prog_cnt );
    if( FD_UNLIKELY( !prog_ids || !prog_tags ) ) FD_LOG_ERR(( "out of memory" ));
    arb_decode_keys( args->arb.prog, args->arb.prog_cnt, prog_ids, prog_tags, "--prog" );
  }
  if( args->arb.acct_cnt ) {
    acct_keys = (uchar (*)[32UL])malloc( args->arb.acct_cnt*32UL );
    acct_tags = (uchar *)malloc( args->arb.acct_cnt );
    if( FD_UNLIKELY( !acct_keys || !acct_tags ) ) FD_LOG_ERR(( "out of memory" ));
    arb_decode_keys( args->arb.acct, args->arb.acct_cnt, acct_keys, acct_tags, "--acct" );
  }

  fd_arbfilter_shmem_t * filter = arb_join_filter( config, FD_SHMEM_JOIN_MODE_READ_WRITE );

  /* 两张表各自独立：只替换调用方给出的那张，另一张保持原样，这样配置
     程序 id 与配置热账户互不干扰。exec tile 可以在替换期间继续匹配，
     读到写入中的表时按未命中处理。 */
  if( args->arb.prog_cnt ) {
    if( FD_UNLIKELY( fd_arbfilter_prog_set( filter, args->arb.prog_cnt, (uchar const (*)[32UL])prog_ids, prog_tags ) ) )
      FD_LOG_ERR(( "failed to install %lu program ids", args->arb.prog_cnt ));
  }
  if( args->arb.acct_cnt ) {
    if( FD_UNLIKELY( fd_arbfilter_acct_set( filter, args->arb.acct_cnt, (uchar const (*)[32UL])acct_keys, acct_tags ) ) )
      FD_LOG_ERR(( "failed to install %lu hot accounts", args->arb.acct_cnt ));
  }

  printf( "version   %lu\n", filter->version );
  printf( "programs  %lu / %lu entries\n", filter->prog_cnt, FD_ARBFILTER_PROG_ENTRY_MAX );
  printf( "accounts  %lu / %lu entries\n", filter->acct_cnt, FD_ARBFILTER_ACCT_ENTRY_MAX );

  free( acct_tags );
  free( acct_keys );
  free( prog_tags );
  free( prog_ids );
}

/* arb_print_prefix 打印数据区的前 64 字节十六进制，便于人工确认账户内容
   是否为预期的那一类账户。 */

static void
arb_print_prefix( uchar const * data,
                  ulong         data_len ) {
  ulong prefix = fd_ulong_min( data_len, 64UL );
  printf( "data      %lu bytes, prefix:", data_len );
  for( ulong i=0UL; i<prefix; i++ ) printf( " %02x", data[ i ] );
  if( prefix<data_len ) printf( " ..." );
  printf( "\n" );
}

/* arb_accdb_ro_t 收集一次只读 accdb 映射所需的全部资源。`arb account` 与
   `arb watch` 都用这一套映射读账户，创建与释放因此收在两个函数里共享。

   accdb 数据区与索引区按只读方式映射，唯一的可写映射是本进程独占的
   epoch fseq：读取期间把当前 epoch 发布进去，accdb tile 的压缩扫描据此
   推迟回收分区。 */

typedef struct {
  fd_accdb_t *       accdb;
  fd_accdb_shmem_t * shmem;      /* 根 fork 的编号要从共享内存对象读 */
  void *             data;       /* 账户数据缓冲，容量 FD_RUNTIME_ACC_SZ_MAX */
  void *             join_alloc; /* 只读 join 状态，退出前释放 */
  void *             data_alloc;
  int                fd_ro;
} arb_accdb_ro_t;

static arb_accdb_ro_t
arb_accdb_join( config_t * config ) {
  fd_topo_t * topo = &config->topo;

  ulong epoch_obj_id = fd_pod_query_ulong( topo->props, "accdb_epoch.arb", ULONG_MAX );
  if( FD_UNLIKELY( epoch_obj_id==ULONG_MAX ) )
    FD_LOG_ERR(( "accdb epoch fseq `accdb_epoch.arb` not found in the topology properties" ));
  ulong accdb_obj_id = fd_pod_query_ulong( topo->props, "accdb", ULONG_MAX );
  if( FD_UNLIKELY( accdb_obj_id==ULONG_MAX ) )
    FD_LOG_ERR(( "accdb object not found in the topology properties" ));

  /* epoch 槽必须映射成可写：它是只读 joiner 向 accdb tile 发布读取序号
     的唯一通道。它所在的 metric 工作区因此按读写映射，其余按只读。 */
  ulong metric_wksp_id = fd_topo_find_wksp( topo, "metric" );
  if( FD_UNLIKELY( metric_wksp_id==ULONG_MAX ) )
    FD_LOG_ERR(( "workspace `metric` not found in the topology" ));
  fd_topo_join_workspace( topo, &topo->workspaces[ metric_wksp_id ], FD_SHMEM_JOIN_MODE_READ_WRITE, FD_TOPO_CORE_DUMP_LEVEL_DISABLED );

  ulong accdb_wksp_id = fd_topo_find_wksp( topo, "accdb_data" );
  if( FD_UNLIKELY( accdb_wksp_id==ULONG_MAX ) )
    FD_LOG_ERR(( "workspace `accdb_data` not found in the topology" ));
  fd_topo_join_workspace( topo, &topo->workspaces[ accdb_wksp_id ], FD_SHMEM_JOIN_MODE_READ_ONLY, FD_TOPO_CORE_DUMP_LEVEL_DISABLED );

  ulong * epoch_slot = fd_fseq_join( fd_topo_obj_laddr( topo, epoch_obj_id ) );
  if( FD_UNLIKELY( !epoch_slot ) ) FD_LOG_ERR(( "failed to join the accdb epoch fseq" ));

  fd_accdb_shmem_t * shmem_ro = fd_accdb_shmem_join( fd_topo_obj_laddr( topo, accdb_obj_id ) );
  if( FD_UNLIKELY( !shmem_ro ) )
    FD_LOG_ERR(( "failed to join the accdb shmem, the running validator is probably using an "
                 "incompatible version of this binary" ));

  int fd_ro = open( config->paths.accounts, O_RDONLY );
  if( FD_UNLIKELY( -1==fd_ro ) )
    FD_LOG_ERR(( "open(%s) failed (%i-%s)", config->paths.accounts, errno, fd_io_strerror( errno ) ));

  /* 只读 join 状态与账户数据缓冲各分配一次，退出前释放。对齐由
     fd_accdb_align 决定，因此多分配一份对齐余量再手工上取整。 */
  ulong  join_align  = fd_accdb_align();
  ulong  join_sz     = fd_accdb_footprint( shmem_ro->max_live_slots, 0 );
  void * join_alloc  = malloc( join_sz+join_align );
  void * data_alloc  = malloc( FD_RUNTIME_ACC_SZ_MAX );
  if( FD_UNLIKELY( !join_alloc || !data_alloc ) ) FD_LOG_ERR(( "out of memory" ));
  void * join_laddr = (void *)fd_ulong_align_up( (ulong)join_alloc, join_align );

  fd_accdb_t * accdb = fd_accdb_join_readonly( join_laddr, shmem_ro, epoch_slot, fd_ro );
  if( FD_UNLIKELY( !accdb ) ) FD_LOG_ERR(( "fd_accdb_join_readonly failed" ));

  return (arb_accdb_ro_t){
    .accdb      = accdb,
    .shmem      = shmem_ro,
    .data       = data_alloc,
    .join_alloc = join_alloc,
    .data_alloc = data_alloc,
    .fd_ro      = fd_ro };
}

static void
arb_accdb_leave( config_t *       config,
                 arb_accdb_ro_t * ro ) {
  free( ro->data_alloc );
  free( ro->join_alloc );
  ro->accdb      = NULL;
  ro->shmem      = NULL;
  ro->data       = NULL;
  ro->join_alloc = NULL;
  ro->data_alloc = NULL;
  if( FD_UNLIKELY( close( ro->fd_ro ) ) ) FD_LOG_WARNING(( "close(%s) failed (%i-%s)", config->paths.accounts, errno, fd_io_strerror( errno ) ));
  ro->fd_ro = -1;
}

static void
arb_account( args_t *   args,
             config_t * config ) {
  uchar pubkey[ 32UL ];
  if( FD_UNLIKELY( !args->arb.pubkey ) )
    FD_LOG_ERR(( "account requires a base58 account address" ));
  if( FD_UNLIKELY( !fd_base58_decode_32( args->arb.pubkey, pubkey ) ) )
    FD_LOG_ERR(( "invalid base58 account address `%s`", args->arb.pubkey ));

  arb_accdb_ro_t ro = arb_accdb_join( config );

  /* 只读当前根 fork：它的内容已经根化、不会再被回滚。fork 池里的槽号会
     被回收复用，所以不能随便挑一个号去读。 */
  fd_accdb_fork_id_t fork_id = ro.shmem->root_fork_id;

  uchar owner[ 32UL ];
  ulong lamports   = 0UL;
  ulong data_len   = 0UL;
  int   executable = 0;
  int   res = fd_accdb_read_one_nocache( ro.accdb, fork_id, pubkey, &lamports, &executable, owner, ro.data, &data_len );

  if( FD_UNLIKELY( res==FD_ACCDB_READ_ONE_NOCACHE_MISS ) ) {
    FD_BASE58_ENCODE_32_BYTES( pubkey, pubkey_b58 );
    printf( "account      %s\n", pubkey_b58 );
    printf( "fork         %u\n", (uint)fork_id.val );
    printf( "not found\n" );
  } else {
    FD_BASE58_ENCODE_32_BYTES( pubkey, pubkey_b58 );
    FD_BASE58_ENCODE_32_BYTES( owner,  owner_b58  );
    printf( "account      %s\n", pubkey_b58 );
    printf( "fork         %u\n", (uint)fork_id.val );
    printf( "lamports     %lu\n", lamports );
    printf( "owner        %s\n", owner_b58 );
    printf( "executable   %s\n", executable ? "true" : "false" );
    printf( "source       %s\n", res==FD_ACCDB_READ_ONE_NOCACHE_CACHE ? "cache" : "disk" );
    arb_print_prefix( ro.data, data_len );
  }

  arb_accdb_leave( config, &ro );
}

/* arb watch 是命中通知环的参考实现：它裸消费 `arb_notify` /
   `arb_notify_rp`，用记录里的 key 从 accdb 只读映射读账户，每个 key 打一
   行。环没有消费者、也就没有流控，生产者不等本程序：落后时只能按 mcache
   里还留着的序号跳号，丢掉陈旧记录，因此每个环各自维护本进程内的序号。

   --limit <n> 收到 n 条记录后退出（便于脚本验收），--keys-only 只打 key
   与命中信息、不读账户数据。这两个选项只对本子命令有意义，因此留在本文件
   而不进 fd_action.h 的 args_t。 */

static ulong arb_watch_limit     = 0UL; /* 0 表示不限条数 */
static int   arb_watch_keys_only = 0;

#define ARB_WATCH_RING_MAX   (32UL)
#define ARB_WATCH_POLL_NANOS (200000L)

typedef struct {
  char const *           link_name;
  ulong                  kind_id;
  fd_frag_meta_t const * mcache;
  ulong                  depth;
  void const *           chunk0;  /* 记录所在工作区的首地址 */
  ulong                  seq;     /* 本进程内下一个要读的序号 */
  ulong                  skipped; /* 因为落后被覆盖掉的记录条数 */
} arb_watch_ring_t;

/* arb_watch_check_rec_sz 用 props 里记录的记录尺寸校验本进程编译进来的
   fd_arbnotify_t：记录是定长裸布局，尺寸不符说明 `arb` 与运行中的验证器来自
   不同的构建，此时字段偏移与 key 容量都不可信，禁止按自己的布局硬解记录。 */

static void
arb_watch_check_rec_sz( fd_topo_t *  topo,
                        char const * link_name ) {
  ulong rec_sz = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "%s.rec_sz", link_name );
  if( FD_UNLIKELY( rec_sz==ULONG_MAX ) )
    FD_LOG_ERR(( "`%s.rec_sz` not found in the topology properties, the running validator was built "
                 "before hit records carried a layout size; rebuild `%s` from the same source tree",
                 link_name, FD_BINARY_NAME ));
  if( FD_UNLIKELY( rec_sz!=sizeof(fd_arbnotify_t) ) )
    FD_LOG_ERR(( "`%s` records are %lu bytes in the running validator but %lu bytes in this binary: "
                 "incompatible versions, rebuild `%s` from the same source tree as the validator",
                 link_name, rec_sz, (ulong)sizeof(fd_arbnotify_t), FD_BINARY_NAME ));
}

/* arb_watch_ring_add 按拓扑 props 里的对象 id 把一条 link 的全部实例（每个
   执行 tile 一条）加进 rings。 */

static void
arb_watch_ring_add( fd_topo_t *        topo,
                    char const *       link_name,
                    arb_watch_ring_t * rings,
                    ulong *            ring_cnt,
                    ulong              ring_cap ) {
  ulong cnt = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "%s.cnt", link_name );
  if( FD_UNLIKELY( cnt==ULONG_MAX ) )
    FD_LOG_ERR(( "link `%s` not found in the topology properties", link_name ));
  if( FD_UNLIKELY( cnt + *ring_cnt > ring_cap ) )
    FD_LOG_ERR(( "too many `%s` links (%lu)", link_name, cnt ));

  for( ulong i=0UL; i<cnt; i++ ) {
    ulong mcache_obj_id = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "%s.%lu.mcache", link_name, i );
    ulong dcache_obj_id = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "%s.%lu.dcache", link_name, i );
    if( FD_UNLIKELY( mcache_obj_id==ULONG_MAX || dcache_obj_id==ULONG_MAX ) )
      FD_LOG_ERR(( "link `%s`:%lu objects not found in the topology properties", link_name, i ));

    fd_frag_meta_t const * mcache = fd_mcache_join( fd_topo_obj_laddr( topo, mcache_obj_id ) );
    if( FD_UNLIKELY( !mcache ) ) FD_LOG_ERR(( "failed to join `%s`:%lu mcache", link_name, i ));
    if( FD_UNLIKELY( !fd_dcache_join( fd_topo_obj_laddr( topo, dcache_obj_id ) ) ) )
      FD_LOG_ERR(( "failed to join `%s`:%lu dcache", link_name, i ));

    arb_watch_ring_t * ring = &rings[ *ring_cnt ];
    ring->link_name = link_name;
    ring->kind_id   = i;
    ring->mcache    = mcache;
    ring->depth     = fd_mcache_depth( mcache );
    ring->chunk0    = (void const *)fd_topo_obj_wksp_base( topo, dcache_obj_id );

    /* 序号从生产者已发布位置的下界起算：更早的记录可能已经被覆盖。 */
    ring->seq     = fd_mcache_seq_query( fd_mcache_seq_laddr_const( mcache ) );
    ring->skipped = 0UL;
    (*ring_cnt)++;
  }
}

/* arb_watch_print 打印一条记录里的第 key_idx 个 key 与它的 tag；--keys-only
   时不读账户数据。账户从记录自带的 fork 读，因此读到的是那笔交易执行时看到
   的那条链上的账户。tag 为 0（表项没有 tag）时省略，输出与不带 tag 时相同。 */

static void
arb_watch_print( arb_accdb_ro_t *       ro,
                 fd_arbnotify_t const * rec,
                 ulong                  key_idx ) {
  FD_BASE58_ENCODE_32_BYTES( rec->keys[ key_idx ], key_b58 );
  /* part/more 指出这条记录是同一笔交易的第几片、后面还有没有片：可提交
     命中超过一条记录的容量时一笔交易会占多条记录，读者不能当成重复记录。 */
  printf( "slot=%lu bank=%lu fork=%u part=%u more=%u bits=0x%x key=%s",
          rec->slot, rec->bank_seq, (uint)rec->accdb_fork_id,
          (uint)rec->part, (uint)rec->more, (uint)rec->matched, key_b58 );
  if( rec->tags[ key_idx ] ) printf( " tag=%u", (uint)rec->tags[ key_idx ] );

  if( arb_watch_keys_only ) { printf( "\n" ); return; }

  fd_accdb_fork_id_t fork_id = { .val = rec->accdb_fork_id };

  uchar owner[ 32UL ];
  ulong lamports   = 0UL;
  ulong data_len   = 0UL;
  int   executable = 0;
  int   res = fd_accdb_read_one_nocache( ro->accdb, fork_id, rec->keys[ key_idx ],
                                         &lamports, &executable, owner, ro->data, &data_len );
  if( FD_UNLIKELY( res==FD_ACCDB_READ_ONE_NOCACHE_MISS ) ) {
    printf( " miss\n" );
    return;
  }

  FD_BASE58_ENCODE_32_BYTES( owner, owner_b58 );
  printf( " lamports=%lu owner=%s datalen=%lu src=%s\n",
          lamports, owner_b58, data_len,
          res==FD_ACCDB_READ_ONE_NOCACHE_CACHE ? "cache" : "db" );
}

static void
arb_watch( config_t * config ) {
  fd_topo_t * topo = &config->topo;

  /* 过滤器与两条环都在 `arbfilter` 工作区：映射一次就够，顺带确认对象
     布局来自运行中的验证器。 */
  (void)arb_join_filter( config, FD_SHMEM_JOIN_MODE_READ_ONLY );

  /* 解读记录之前必须确认两边的记录尺寸一致，禁止按本进程的布局硬解。 */
  arb_watch_check_rec_sz( topo, FD_ARBNOTIFY_LINK_LE );
  arb_watch_check_rec_sz( topo, FD_ARBNOTIFY_LINK_RP );

  arb_watch_ring_t rings[ ARB_WATCH_RING_MAX ];
  ulong ring_cnt = 0UL;
  arb_watch_ring_add( topo, FD_ARBNOTIFY_LINK_LE, rings, &ring_cnt, ARB_WATCH_RING_MAX );
  arb_watch_ring_add( topo, FD_ARBNOTIFY_LINK_RP, rings, &ring_cnt, ARB_WATCH_RING_MAX );
  if( FD_UNLIKELY( !ring_cnt ) )
    FD_LOG_ERR(( "no arb_notify / arb_notify_rp links in the topology, the running validator was built "
                 "before the hit notification rings existed" ));

  arb_accdb_ro_t ro = arb_accdb_join( config );

  ulong records    = 0UL;
  ulong unverified = 0UL;
  int   done       = 0;
  while( !done ) {
    int got = 0;
    for( ulong r=0UL; r<ring_cnt && !done; r++ ) {
      arb_watch_ring_t * ring = rings + r;
      for(;;) {
        ulong found = fd_mcache_query( ring->mcache, ring->depth, ring->seq );
        long  diff  = fd_seq_diff( found, ring->seq );
        if( FD_LIKELY( diff<0L ) ) break; /* 这条环还没有新记录 */

        if( FD_UNLIKELY( diff>0L ) ) { /* 落后：陈旧记录已被覆盖，跳号 */
          ring->skipped += (ulong)diff;
          ring->seq      = found;
        }

        /* 记录没有流控保护，元数据可能已被新记录覆盖：按当前读到的值取
           一条，不重试。 */
        fd_frag_meta_t const * meta  = ring->mcache + fd_mcache_line_idx( ring->seq, ring->depth );
        ulong                  chunk = meta->chunk;
        ring->seq = fd_seq_inc( ring->seq, 1UL );

        fd_arbnotify_t const * rec = fd_chunk_to_laddr_const( ring->chunk0, chunk );

        /* 记录带着生产者写下的 fork generation。当前 fork 池里该槽位的
           generation 与之不符时，fork 已被回收重用，记录指向的是另一个
           分支，必须丢弃而不是拿它去读账户。 */
        uint gen_now = fd_accdb_fork_gen( ro.accdb, (fd_accdb_fork_id_t){ .val = rec->accdb_fork_id } );
        if( FD_UNLIKELY( !gen_now || gen_now!=rec->accdb_generation ) ) { unverified++; continue; }

        /* key_cnt 来自共享内存：按记录容量截断，避免读到其它版本写下的
           越界条数。 */
        ulong key_cnt = fd_ulong_min( (ulong)rec->key_cnt, (ulong)FD_ARBNOTIFY_KEY_MAX );
        for( ulong k=0UL; k<key_cnt; k++ ) arb_watch_print( &ro, rec, k );
        records++;
        got = 1;

        if( arb_watch_limit && records>=arb_watch_limit ) { done = 1; break; }
      }
    }

    if( FD_LIKELY( !done && !got ) ) {
      struct timespec ts = { .tv_sec = 0L, .tv_nsec = ARB_WATCH_POLL_NANOS };
      nanosleep( &ts, NULL );
    }
  }

  ulong skipped = 0UL;
  for( ulong r=0UL; r<ring_cnt; r++ ) skipped += rings[ r ].skipped;
  FD_LOG_NOTICE(( "arb watch: records=%lu skipped=%lu unverified=%lu", records, skipped, unverified ));

  arb_accdb_leave( config, &ro );
}

static void
arb_cmd_fn( args_t *   args,
            config_t * config ) {
  if( FD_UNLIKELY( !args->arb.cmd ) ) {
    fd_action_help_print( &fd_action_arb );
    return;
  }

  /* 先接管运行中验证器发布的那份配置：拓扑里的对象偏移、工作区名字与
     accounts.db 路径都必须来自它，否则映射到的是另一份布局。 */
  fd_bootinfo_adopt  ( config );
  fd_bootinfo_check_layout( config );

  if     ( !strcmp( args->arb.cmd, "filter-dump" ) ) arb_filter_dump( config );
  else if( !strcmp( args->arb.cmd, "filter-set"  ) ) arb_filter_set ( args, config );
  else if( !strcmp( args->arb.cmd, "account"     ) ) arb_account    ( args, config );
  else if( !strcmp( args->arb.cmd, "watch"       ) ) arb_watch      ( config );
  else FD_LOG_ERR(( "unknown subcommand `%s`", args->arb.cmd ));
}

static void
arb_cmd_args( int *    pargc,
              char *** pargv,
              args_t * args ) {
  args->arb.cmd      = NULL;
  args->arb.pubkey   = NULL;
  args->arb.prog_cnt = 0UL;
  args->arb.acct_cnt = 0UL;

  arb_watch_limit     = 0UL;
  arb_watch_keys_only = 0;

  /* 位置参数按出现顺序取：先是子命令，`account` 再带一个 base58 地址。
     --prog 与 --acct 可重复，因此不能用 fd_env_strip_cmdline_* 剥离
     （它只保留最后一个值），这里自己扫一遍并压实 argv，剩下的未知参数
     留给 fd_main 报错。base58 与可选的 `:tag` 后缀只记指针，由命令自己解
     码与解析。
     --limit 与 --keys-only 只属于 `watch`，同样在这里剥离。 */
  int new_argc = 0;
  for( int i=0; i<*pargc; i++ ) {
    char const * arg = (*pargv)[ i ];

    if( !strcmp( arg, "--prog" ) || !strcmp( arg, "--acct" ) ) {
      int is_prog = !strcmp( arg, "--prog" );
      if( FD_UNLIKELY( i+1>=*pargc ) ) FD_LOG_ERR(( "%s requires a base58 public key argument", arg ));

      char const * val = (*pargv)[ ++i ];
      ulong        cnt = is_prog ? args->arb.prog_cnt : args->arb.acct_cnt;
      ulong        max = is_prog ? FD_ACTION_ARB_PROG_MAX : FD_ACTION_ARB_ACCT_MAX;
      if( FD_UNLIKELY( cnt>=max ) ) FD_LOG_ERR(( "%s accepts at most %lu keys", arg, max ));

      if( is_prog ) { args->arb.prog[ cnt ] = val; args->arb.prog_cnt = cnt+1UL; }
      else          { args->arb.acct[ cnt ] = val; args->arb.acct_cnt = cnt+1UL; }
      continue;
    }

    if( !strcmp( arg, "--limit" ) ) {
      if( FD_UNLIKELY( i+1>=*pargc ) ) FD_LOG_ERR(( "--limit requires a record count" ));
      char const * val = (*pargv)[ ++i ];
      char *       end = NULL;
      ulong        cnt = strtoul( val, &end, 10 );
      if( FD_UNLIKELY( !end || *end || !cnt ) ) FD_LOG_ERR(( "--limit: invalid record count `%s`", val ));
      arb_watch_limit = cnt;
      continue;
    }
    if( !strcmp( arg, "--keys-only" ) ) { arb_watch_keys_only = 1; continue; }

    if( FD_UNLIKELY( !strncmp( arg, "--", 2UL ) ) ) { /* 未知选项，交给 fd_main 报错 */
      (*pargv)[ new_argc++ ] = (*pargv)[ i ];
      continue;
    }

    if     ( !args->arb.cmd    ) args->arb.cmd    = arg;
    else if( !args->arb.pubkey ) args->arb.pubkey = arg;
    else                         (*pargv)[ new_argc++ ] = (*pargv)[ i ]; /* 多余的位置参数 */
  }
  *pargc = new_argc;
  (*pargv)[ new_argc ] = NULL; /* argv 以 NULL 结尾 */
}

static void
arb_args_help( fd_action_help_t * help ) {
  fd_action_help_arg( help, "filter-dump", NULL, "Print the filter configuration and the exec tile counters" );
  fd_action_help_arg( help, "filter-set",  NULL, "Replace the program id set and/or the hot account table" );
  fd_action_help_arg( help, "account",     "<address>", "Read one account out of the running validator's accdb" );
  fd_action_help_arg( help, "watch",       NULL, "Consume the arbitrage hit rings and read the hit accounts" );
  fd_action_help_arg( help, "--prog", "<address>[:tag]", "Program id to add to the filter (repeatable, replaces the whole set)" );
  fd_action_help_arg( help, "--acct", "<address>[:tag]", "Hot account key to add to the filter (repeatable, replaces the whole table)" );
  fd_action_help_arg( help, "--limit", "<n>", "watch: stop after n records (default: run until killed)" );
  fd_action_help_arg( help, "--keys-only", NULL, "watch: print the hits without reading any account data" );
}

action_t fd_action_arb = {
  .name           = "arb",
  .args           = arb_cmd_args,
  .fn             = arb_cmd_fn,
  .require_config = 0, /* fd_bootinfo_adopt replaces config with the running validator's */
  .perm           = NULL,
  .description    = "Configure and inspect the arbitrage pre-filter",
  .detail         = "Attaches to a running validator's shared memory and operates on the arbfilter\n"
                    "object, which the exec tiles consult after executing each transaction:\n"
                    "\n"
                    "  filter-dump                   print the configured program ids, hot accounts\n"
                    "                                and the emit/skip/published counters\n"
                    "  filter-set --prog <a>[:tag]   replace the program id set (DEX programs)\n"
                    "             --acct <a>[:tag]   replace the hot account table (pool accounts)\n"
                    "  account <address>             print lamports, owner, executable and data of\n"
                    "                                one account at the current root fork\n"
                    "  watch [--limit <n>] [--keys-only]\n"
                    "                                consume the arb_notify / arb_notify_rp rings and\n"
                    "                                read every hit key out of the accdb\n"
                    "\n"
                    "The optional `:tag` suffix on --prog / --acct attaches a 1-byte tag\n"
                    "(0..255) to that entry.  Tags are published with every hit key and never\n"
                    "affect whether a transaction matches.\n"
                    "\n"
                    "filter-set takes effect immediately; exec tiles keep matching while the tables\n"
                    "are replaced.  filter-dump and account read only.  watch is a reference\n"
                    "consumer of the hit rings: they have no in-validator consumer, so a slow\n"
                    "reader skips stale records instead of blocking the exec tiles.",
  .usage          = "arb <filter-dump|filter-set|account|watch> [OPTIONS]",
  .args_help      = arb_args_help,
};
