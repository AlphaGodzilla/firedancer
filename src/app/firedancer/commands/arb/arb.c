#include "../../../shared/fd_config.h"
#include "../../../shared/fd_action.h"
#include "../../../shared/fd_bootinfo.h"

#include "../../../../disco/topo/fd_topo.h"
#include "../../../../tango/fseq/fd_fseq.h"
#include "../../../../discof/arb/fd_arbfilter.h"
#include "../../../../flamenco/accdb/fd_accdb.h"
#include "../../../../flamenco/accdb/fd_accdb_shmem.h"
#include "../../../../flamenco/accdb/fd_accdb_private.h" /* fd_accdb_shmem_t::root_fork_id */
#include "../../../../flamenco/runtime/fd_runtime_const.h"
#include "../../../../ballet/base58/fd_base58.h"
#include "../../../../util/pod/fd_pod.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    printf( "  [%lu] %s\n", i, prog_b58 );
  }

  printf( "accounts  %lu / %lu entries\n", filter->acct_cnt, FD_ARBFILTER_ACCT_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_SLOT_MAX; i++ ) {
    if( !filter->acct[ i ].used ) continue;
    FD_BASE58_ENCODE_32_BYTES( filter->acct[ i ].key, acct_b58 );
    printf( "  [%lu] %s\n", i, acct_b58 );
  }

  printf( "stat_emit %lu\n", FD_VOLATILE_CONST( filter->stat_emit ) );
  printf( "stat_skip %lu\n", FD_VOLATILE_CONST( filter->stat_skip ) );
}

static void
arb_filter_dump( config_t * config ) {
  fd_arbfilter_shmem_t const * filter = arb_join_filter( config, FD_SHMEM_JOIN_MODE_READ_ONLY );
  arb_print_filter( filter );
}

/* arb_decode_keys 把 cnt 个 base58 参数解码进 out（out 由调用方分配
   cnt*32 字节）。任何一条解不出来都直接退出，此时运行中的过滤表还没有被
   改动。 */

static void
arb_decode_keys( char const * const * vals,
                 ulong                cnt,
                 uchar                (*out)[ 32UL ],
                 char const *         flag ) {
  for( ulong i=0UL; i<cnt; i++ ) {
    if( FD_UNLIKELY( !fd_base58_decode_32( vals[ i ], out[ i ] ) ) )
      FD_LOG_ERR(( "%s: invalid base58 public key `%s`", flag, vals[ i ] ));
  }
}

static void
arb_filter_set( args_t *   args,
                config_t * config ) {
  if( FD_UNLIKELY( !args->arb.prog_cnt && !args->arb.acct_cnt ) )
    FD_LOG_ERR(( "filter-set requires at least one --prog or --acct" ));

  /* 命令行只记 base58 指针，解码在这里做：失败时两张表都还没动。 */
  uchar (*prog_ids)[ 32UL ] = NULL;
  uchar (*acct_keys)[ 32UL ] = NULL;
  if( args->arb.prog_cnt ) {
    prog_ids = (uchar (*)[32UL])malloc( args->arb.prog_cnt*32UL );
    if( FD_UNLIKELY( !prog_ids ) ) FD_LOG_ERR(( "out of memory" ));
    arb_decode_keys( args->arb.prog, args->arb.prog_cnt, prog_ids, "--prog" );
  }
  if( args->arb.acct_cnt ) {
    acct_keys = (uchar (*)[32UL])malloc( args->arb.acct_cnt*32UL );
    if( FD_UNLIKELY( !acct_keys ) ) FD_LOG_ERR(( "out of memory" ));
    arb_decode_keys( args->arb.acct, args->arb.acct_cnt, acct_keys, "--acct" );
  }

  fd_arbfilter_shmem_t * filter = arb_join_filter( config, FD_SHMEM_JOIN_MODE_READ_WRITE );

  /* 两张表各自独立：只替换调用方给出的那张，另一张保持原样，这样配置
     程序 id 与配置热账户互不干扰。exec tile 可以在替换期间继续匹配，
     读到写入中的表时按未命中处理。 */
  if( args->arb.prog_cnt ) {
    if( FD_UNLIKELY( fd_arbfilter_prog_set( filter, args->arb.prog_cnt, (uchar const (*)[32UL])prog_ids ) ) )
      FD_LOG_ERR(( "failed to install %lu program ids", args->arb.prog_cnt ));
  }
  if( args->arb.acct_cnt ) {
    if( FD_UNLIKELY( fd_arbfilter_acct_set( filter, args->arb.acct_cnt, (uchar const (*)[32UL])acct_keys ) ) )
      FD_LOG_ERR(( "failed to install %lu hot accounts", args->arb.acct_cnt ));
  }

  printf( "version   %lu\n", filter->version );
  printf( "programs  %lu / %lu entries\n", filter->prog_cnt, FD_ARBFILTER_PROG_ENTRY_MAX );
  printf( "accounts  %lu / %lu entries\n", filter->acct_cnt, FD_ARBFILTER_ACCT_ENTRY_MAX );

  free( acct_keys );
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

/* arb_account 经 accdb 的只读接口读出账户。accdb 数据区与索引区按只读
   方式映射，唯一的可写映射是本进程独占的 epoch fseq：读取期间把当前
   epoch 发布进去，accdb tile 的压缩扫描据此推迟回收分区。 */

static void
arb_account( args_t *   args,
             config_t * config ) {
  fd_topo_t * topo = &config->topo;

  uchar pubkey[ 32UL ];
  if( FD_UNLIKELY( !args->arb.pubkey ) )
    FD_LOG_ERR(( "account requires a base58 account address" ));
  if( FD_UNLIKELY( !fd_base58_decode_32( args->arb.pubkey, pubkey ) ) )
    FD_LOG_ERR(( "invalid base58 account address `%s`", args->arb.pubkey ));

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

  /* 只读当前根 fork：它的内容已经根化、不会再被回滚。fork 池里的槽号会
     被回收复用，所以不能随便挑一个号去读。 */
  fd_accdb_fork_id_t fork_id = shmem_ro->root_fork_id;

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

  uchar owner[ 32UL ];
  ulong lamports   = 0UL;
  ulong data_len   = 0UL;
  int   executable = 0;
  int   res = fd_accdb_read_one_nocache( accdb, fork_id, pubkey, &lamports, &executable, owner, data_alloc, &data_len );

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
    arb_print_prefix( data_alloc, data_len );
  }

  free( data_alloc );
  free( join_alloc );
  if( FD_UNLIKELY( close( fd_ro ) ) ) FD_LOG_WARNING(( "close(%s) failed (%i-%s)", config->paths.accounts, errno, fd_io_strerror( errno ) ));
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

  /* 位置参数按出现顺序取：先是子命令，`account` 再带一个 base58 地址。
     --prog 与 --acct 可重复，因此不能用 fd_env_strip_cmdline_* 剥离
     （它只保留最后一个值），这里自己扫一遍并压实 argv，剩下的未知参数
     留给 fd_main 报错。base58 只记指针，由命令自己解码。 */
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
  fd_action_help_arg( help, "--prog", "<address>", "Program id to add to the filter (repeatable, replaces the whole set)" );
  fd_action_help_arg( help, "--acct", "<address>", "Hot account key to add to the filter (repeatable, replaces the whole table)" );
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
                    "                                and the emit/skip counters\n"
                    "  filter-set --prog <a> ...     replace the program id set (DEX programs)\n"
                    "             --acct <a> ...     replace the hot account table (pool accounts)\n"
                    "  account <address>             print lamports, owner, executable and data of\n"
                    "                                one account at the current root fork\n"
                    "\n"
                    "filter-set takes effect immediately; exec tiles keep matching while the tables\n"
                    "are replaced.  filter-dump and account read only.",
  .usage          = "arb <filter-dump|filter-set|account> [OPTIONS]",
  .args_help      = arb_args_help,
};
