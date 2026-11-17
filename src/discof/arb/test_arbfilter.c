#include "fd_arbfilter.h"

#include <string.h>
#if FD_HAS_THREADS
#include <pthread.h>
#endif

/* 本文件验证 arbfilter 的两张配置表、seqlock 序号与匹配规则。所有用例
   共用一块从工作区分配的内存区域，每个用例自己 new 一遍把它格式化。 */

/* test_key 把序号 i 写成一个可辨认的 32 字节 key，末尾字节让相邻 i 的
   key 不同，避免探针链上出现意外重复。 */

static void
test_key( uchar *         key,
          ulong           i,
          uchar           salt ) {
  memset( key, 0, 32UL );
  key[ 0 ]  = (uchar)i;
  key[ 1 ]  = (uchar)(i>>8);
  key[ 2 ]  = salt;
  key[ 31 ] = (uchar)(i*salt+1UL);
}

/* test_check_version 断言序号处于稳定状态（偶数），并且比上一次观察到的
   序号大。写方每次替换配置表都应该让序号前进 2。 */

static ulong
test_check_version( fd_arbfilter_shmem_t const * f,
                    ulong                        prev ) {
  ulong ver = f->version;
  FD_TEST( !( ver & 1UL ) );    /* 稳定 */
  FD_TEST( ver>prev );          /* 单调递增 */
  FD_TEST( !( (ver-prev) & 1UL ) );
  return ver;
}

static void
test_new_join( void * mem ) {
  FD_TEST( fd_arbfilter_footprint()>=sizeof(fd_arbfilter_shmem_t) );
  FD_TEST( fd_arbfilter_align()>=alignof(fd_arbfilter_shmem_t) );

  FD_TEST( fd_arbfilter_new( mem ) );
  fd_arbfilter_shmem_t const * f = fd_arbfilter_join( mem );
  FD_TEST( f );
  FD_TEST( f==(fd_arbfilter_shmem_t const *)mem );
  FD_TEST( f->magic==FD_ARBFILTER_MAGIC );
  FD_TEST( f->version==0UL );
  FD_TEST( f->prog_mask==FD_ARBFILTER_PROG_SLOT_MAX-1UL );
  FD_TEST( f->prog_cnt==0UL );
  FD_TEST( f->acct_mask==FD_ARBFILTER_ACCT_SLOT_MAX-1UL );
  FD_TEST( f->acct_cnt==0UL );
  FD_TEST( f->stat_emit==0UL && f->stat_skip==0UL && f->stat_ns==0UL );

  /* 未格式化的内存与错位的指针都必须被 join 拒绝。 */
  FD_TEST( !fd_arbfilter_join( NULL ) );
  uchar junk[ 4096 ] __attribute__((aligned(FD_ARBFILTER_ALIGN)));
  memset( junk, 0, sizeof(junk) );
  FD_TEST( !fd_arbfilter_join( junk ) );
  FD_TEST( !fd_arbfilter_join( (uchar *)mem+1UL ) );
  FD_TEST( !fd_arbfilter_new( NULL ) );
  FD_TEST( !fd_arbfilter_new( (uchar *)mem+1UL ) );
}

static void
test_prog_set( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar ids[ FD_ARBFILTER_PROG_ENTRY_MAX ][ 32UL ];
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) test_key( ids[ i ], i, 7U );

  ulong ver = f->version;

  /* 空集合不命中任何 key。 */
  uchar probe[ 32UL ];
  test_key( probe, 0UL, 7U );
  FD_TEST( fd_arbfilter_prog_idx( f, probe )==ULONG_MAX );

  /* 逐个加入，每次全部成员都能查到、未加入的都查不到。槽号是哈希探测
     结果，与写入顺序无关，因此只断言命中与否。 */
  for( ulong cnt=1UL; cnt<=FD_ARBFILTER_PROG_ENTRY_MAX; cnt++ ) {
    FD_TEST( fd_arbfilter_prog_set( f, cnt, (uchar const (*)[32UL])ids )==0 );
    FD_TEST( f->prog_cnt==cnt );
    ver = test_check_version( f, ver );
    for( ulong i=0UL; i<cnt; i++ )         FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )!=ULONG_MAX );
    for( ulong i=cnt; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )==ULONG_MAX );
  }

  /* 装到上限（64 个）之后，表里没有任何空槽以外的残留：所有成员命中。 */
  FD_TEST( f->prog_cnt==FD_ARBFILTER_PROG_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )!=ULONG_MAX );

  /* 越界：返回 -1 且表保持原样。 */
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX+1UL, (uchar const (*)[32UL])ids )==-1 );
  FD_TEST( f->prog_cnt==FD_ARBFILTER_PROG_ENTRY_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ FD_ARBFILTER_PROG_ENTRY_MAX-1UL ] )!=ULONG_MAX );
  FD_TEST( f->version==ver );

  /* 重复 id 只存一份。 */
  uchar dup[ 4UL ][ 32UL ];
  memcpy( dup[ 0 ], ids[ 0 ], 32UL );
  memcpy( dup[ 1 ], ids[ 3 ], 32UL );
  memcpy( dup[ 2 ], ids[ 0 ], 32UL );
  memcpy( dup[ 3 ], ids[ 3 ], 32UL );
  FD_TEST( fd_arbfilter_prog_set( f, 4UL, (uchar const (*)[32UL])dup )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==2UL );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 0 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 3 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 1 ] )==ULONG_MAX );

  /* 整表替换成单项：旧成员立刻失效。 */
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])&ids[ 5 ] )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==1UL );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 5 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 6 ] )==ULONG_MAX );

  /* 清空。 */
  FD_TEST( fd_arbfilter_prog_set( f, 0UL, NULL )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==0UL );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 5 ] )==ULONG_MAX );
}

static void
test_acct_set( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar keys[ FD_ARBFILTER_ACCT_ENTRY_MAX ][ 32UL ];
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) test_key( keys[ i ], i, 11U );

  ulong ver = f->version;

  /* 少量热账户：全部命中，未列出的 key 不命中。 */
  ulong cnt = 64UL;
  FD_TEST( fd_arbfilter_acct_set( f, cnt, (uchar const (*)[32UL])keys )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==cnt );
  for( ulong i=0UL; i<cnt; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )!=ULONG_MAX );
  for( ulong i=cnt; i<cnt+64UL; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )==ULONG_MAX );

  /* 重复 key 只存一份。 */
  uchar dup[ 4UL ][ 32UL ];
  memcpy( dup[ 0 ], keys[ 0 ], 32UL );
  memcpy( dup[ 1 ], keys[ 7 ], 32UL );
  memcpy( dup[ 2 ], keys[ 0 ], 32UL );
  memcpy( dup[ 3 ], keys[ 7 ], 32UL );
  FD_TEST( fd_arbfilter_acct_set( f, 4UL, (uchar const (*)[32UL])dup )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==2UL );
  FD_TEST( fd_arbfilter_acct_idx( f, keys[ 0 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_acct_idx( f, keys[ 7 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_acct_idx( f, keys[ 1 ] )==ULONG_MAX );

  /* 边界：装到条目上限。装载率因此只有 0.5，所有成员仍然命中，表外的
     key 在走完它所在的那条探测链之后判为未命中。 */
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==FD_ARBFILTER_ACCT_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )!=ULONG_MAX );
  uchar absent[ 32UL ];
  test_key( absent, FD_ARBFILTER_ACCT_ENTRY_MAX+17UL, 11U );
  FD_TEST( fd_arbfilter_acct_idx( f, absent )==ULONG_MAX );

  /* 越界：返回 -1 且表保持原样。 */
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX+1UL, (uchar const (*)[32UL])keys )==-1 );
  FD_TEST( f->acct_cnt==FD_ARBFILTER_ACCT_ENTRY_MAX );
  FD_TEST( f->version==ver );

  /* 清空。 */
  FD_TEST( fd_arbfilter_acct_set( f, 0UL, NULL )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==0UL );
  for( ulong i=0UL; i<8UL; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )==ULONG_MAX );
}

/* test_txn_init 把 mem 清零成一条只填了解析结果的交易：指令数与静态账户
   数由调用方指定，其余字段（包括账户地址区偏移）由用例自己补齐。 */

static fd_txn_t *
test_txn_init( uchar * mem,
               ulong   instr_cnt,
               ulong   acct_addr_cnt ) {
  fd_txn_t * txn = (fd_txn_t *)mem;
  memset( txn, 0, fd_txn_footprint( instr_cnt, 0UL ) );
  txn->transaction_version = FD_TXN_VLEGACY;
  txn->instr_cnt           = (ushort)instr_cnt;
  txn->acct_addr_cnt       = (ushort)acct_addr_cnt;
  return txn;
}

static void
test_match( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_key [ 32UL ]; /* 在程序表里 */
  uchar hot_key  [ 32UL ]; /* 在热账户表里 */
  uchar pad_key  [ 32UL ]; /* 只在账户列表里 */
  uchar pad2_key [ 32UL ];
  test_key( prog_key,  3UL, 21U );
  test_key( hot_key,   4UL, 21U );
  test_key( pad_key,   5UL, 21U );
  test_key( pad2_key,  6UL, 21U );

  uchar prog_ids[ 1UL ][ 32UL ];
  uchar hot_keys[ 1UL ][ 32UL ];
  memcpy( prog_ids[ 0 ], prog_key,  32UL );
  memcpy( hot_keys[ 0 ],  hot_key,  32UL );
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_ids )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 1UL, (uchar const (*)[32UL])hot_keys  )==0 );

  /* 每个用例用最小账户列表，只让被测规则成立，避免其它规则顺带命中。 */

  /* R1 + R2：账户列表 [0]=付款方 [1]=指令 0 的程序，该程序在程序表里，
     因此它同时让 R1（指令直调）与 R2（程序出现在账户列表里）成立；列表
     里没有热账户，R3 不成立。 */
  fd_acct_addr_t list_r1[ 2UL ];
  memset( list_r1, 0, sizeof(list_r1) );
  memcpy( list_r1[ 0 ].b, pad_key,  32UL );
  memcpy( list_r1[ 1 ].b, prog_key, 32UL );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, 2UL );
  txn->instr[ 0 ].program_id = 1U;
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)list_r1, 2UL )==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT ) );

  /* R2 单独成立：顶层指令调用列表里的另一个程序，而程序表成员只是被
     传进账户列表，等价于这笔交易对 DEX 程序发起了 CPI。 */
  txn->instr[ 0 ].program_id = 0U;
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)list_r1, 2UL )==FD_ARBFILTER_BIT_PROG_ACCOUNT );

  /* R3 单独成立：列表里只有付款方与热账户，且顶层指令调用的是付款方。 */
  fd_acct_addr_t list_r3[ 2UL ];
  memset( list_r3, 0, sizeof(list_r3) );
  memcpy( list_r3[ 0 ].b, pad_key, 32UL );
  memcpy( list_r3[ 1 ].b, hot_key, 32UL );
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)list_r3, 2UL )==FD_ARBFILTER_BIT_HOT_ACCOUNT );

  /* 三条同时成立：指令直调程序表成员，它同时被记作 R2，列表里还有热账户。 */
  fd_acct_addr_t list_all[ 2UL ];
  memset( list_all, 0, sizeof(list_all) );
  memcpy( list_all[ 0 ].b, hot_key,  32UL );
  memcpy( list_all[ 1 ].b, prog_key, 32UL );
  txn->instr[ 0 ].program_id = 1U;
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)list_all, 2UL )==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );

  /* 三条规则都不成立。 */
  fd_acct_addr_t unrelated[ 2UL ];
  memset( unrelated, 0, sizeof(unrelated) );
  memcpy( unrelated[ 0 ].b, pad_key,  32UL );
  memcpy( unrelated[ 1 ].b, pad2_key, 32UL );
  txn->instr[ 0 ].program_id = 0U;
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)unrelated, 2UL )==0UL );

  /* 空账户列表不命中，且不会越界读。 */
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)unrelated, 0UL )==0UL );

  /* accs 为 NULL 时改用交易线上的静态账户列表：R3 通过 payload 命中。 */
  uchar payload[ 256UL ];
  memset( payload, 0, sizeof(payload) );
  memcpy( payload+64UL,      pad_key, 32UL );
  memcpy( payload+64UL+32UL, hot_key,   32UL );
  txn = test_txn_init( txn_mem, 1UL, 2UL );
  txn->acct_addr_off = 64U;
  txn->instr[ 0 ].program_id = 0U;
  FD_TEST( fd_arbfilter_match( f, txn, payload, NULL, 0UL )==FD_ARBFILTER_BIT_HOT_ACCOUNT );

  /* 线上账户列表里的程序 id 同样能让 R1 成立。 */
  memcpy( payload+64UL+32UL, prog_key, 32UL );
  txn->instr[ 0 ].program_id = 1U;
  FD_TEST( fd_arbfilter_match( f, txn, payload, NULL, 0UL )==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT ) );

  /* 指令的 program_id 越界（交易数据不一致）时跳过该指令，不读取账户
     列表之外的内存。此时账户列表里也没有两张表的成员，结果为 0。 */
  memset( payload+64UL, 0, 64UL );
  test_key( payload+64UL,      40UL, 21U );
  test_key( payload+64UL+32UL, 41UL, 21U );
  txn->instr[ 0 ].program_id = 200U;
  FD_TEST( fd_arbfilter_match( f, txn, payload, NULL, 0UL )==0UL );
}

/* test_match_empty 验证空过滤器对任何交易都返回 0，execrp 因此只累加
   stat_skip。 */

static void
test_match_empty( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  fd_acct_addr_t accs[ 3UL ];
  memset( accs, 0, sizeof(accs) );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 2UL, 3UL );
  txn->instr[ 0 ].program_id = 0U;
  txn->instr[ 1 ].program_id = 2U;

  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)accs, 3UL )==0UL );
}


/* test_probe_bound 钉住热路径代价：把两张表都装到可配置条目上限（装载率
   0.5），断言未命中查询的探测次数仍是个位数量级，而不是扫完整张表。

   未命中是热路径上的常见情形，所以这条上界不是优化目标而是正确性要求：
   一旦有人把条目上限抬到接近槽数，探测次数会退化成 O(槽数)，这里会失败。 */

static void
test_probe_bound( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar keys[ FD_ARBFILTER_ACCT_ENTRY_MAX ][ 32UL ];
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) test_key( keys[ i ], i, 71U );
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys )==0 );

  uchar prog_ids[ FD_ARBFILTER_PROG_ENTRY_MAX ][ 32UL ];
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) test_key( prog_ids[ i ], i, 73U );
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX, (uchar const (*)[32UL])prog_ids )==0 );

  /* 热账户表：装载率 0.5。这组 key 实测未命中最多 3 次探测、平均 1.6 次，
     这里把上界定在 8 / 3（扫完整张表需要 2048 次），留出换哈希函数或换
     key 集合的余量，同时任何装载率回退都会立刻触发失败。 */
  ulong max_probes = 0UL;
  ulong tot_probes = 0UL;
  ulong miss_cnt   = 0UL;
  uchar absent[ 32UL ];
  for( ulong i=0UL; i<4096UL; i++ ) {
    test_key( absent, FD_ARBFILTER_ACCT_ENTRY_MAX+i, 71U );
    ulong probes = 0UL;
    FD_TEST( arbfilter_tbl_find( f->acct, f->acct_mask, absent, &probes )==ULONG_MAX );
    max_probes = fd_ulong_max( max_probes, probes );
    tot_probes += probes;
    miss_cnt++;
  }
  FD_TEST( max_probes<=8UL );
  FD_TEST( tot_probes<=miss_cnt*3UL );

  /* 程序表：128 槽 / 64 条目，同样是 0.5；实测未命中最多 6 次探测。 */
  ulong max_prog_probes = 0UL;
  for( ulong i=0UL; i<4096UL; i++ ) {
    test_key( absent, FD_ARBFILTER_PROG_ENTRY_MAX+i, 73U );
    ulong probes = 0UL;
    FD_TEST( arbfilter_tbl_find( f->prog, f->prog_mask, absent, &probes )==ULONG_MAX );
    max_prog_probes = fd_ulong_max( max_prog_probes, probes );
  }
  FD_TEST( max_prog_probes<=8UL );

  /* 命中查询只会更便宜：同一批 key 的命中探测次数不超过未命中的上界。 */
  ulong max_hit_probes = 0UL;
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) {
    ulong probes = 0UL;
    FD_TEST( arbfilter_tbl_find( f->acct, f->acct_mask, keys[ i ], &probes )!=ULONG_MAX );
    max_hit_probes = fd_ulong_max( max_hit_probes, probes );
  }
  FD_TEST( max_hit_probes<=8UL );
}

/* test_stuck_writer 模拟一个卡在写入中的写方（序号为奇数）：读方必须在有限
   次重试后用尽预算按未命中返回，既不能接受半写的表，也不能无界自旋。 */

static void
test_stuck_writer( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_ids[ 1UL ][ 32UL ];
  uchar hot_keys[ 1UL ][ 32UL ];
  test_key( prog_ids[ 0 ], 21UL, 41U );
  test_key( hot_keys[ 0 ],  22UL, 41U );
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_ids )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 1UL, (uchar const (*)[32UL])hot_keys )==0 );

  ulong ver = f->version;
  FD_TEST( fd_arbfilter_prog_idx( f, prog_ids[ 0 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_acct_idx( f, hot_keys[ 0 ] )!=ULONG_MAX );

  /* 序号变奇数：等价于某个写方已经 CAS 抢到写权限但还没写完。 */
  f->version = ver+1UL;
  FD_TEST( fd_arbfilter_prog_idx( f, prog_ids[ 0 ] )==ULONG_MAX );
  FD_TEST( fd_arbfilter_acct_idx( f, hot_keys[ 0 ] )==ULONG_MAX );

  /* 写方写完：序号回到偶数，查询恢复。 */
  f->version = ver+2UL;
  FD_TEST( fd_arbfilter_prog_idx( f, prog_ids[ 0 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_acct_idx( f, hot_keys[ 0 ] )!=ULONG_MAX );
}

#if FD_HAS_THREADS

/* test_writer_exclusion 用两个线程验证写权限的互斥：主线程把序号手工置成
   奇数模拟「写方 A 正在改表」，写方 B 的 prog_set 必须原地等待；只有序号
   回到偶数之后 B 才能拿到写权限并把序号推进 +2。 */

static fd_arbfilter_shmem_t * test_writer_filter;
static uchar                  test_writer_ids[ 1UL ][ 32UL ];
static volatile int           test_writer_started;
static volatile int           test_writer_done;

static void *
test_writer_thread( void * arg FD_FN_UNUSED ) {
  test_writer_started = 1;
  FD_TEST( fd_arbfilter_prog_set( test_writer_filter, 1UL, (uchar const (*)[32UL])test_writer_ids )==0 );
  test_writer_done = 1;
  return NULL;
}

static void
test_writer_exclusion( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  test_writer_filter  = f;
  test_writer_started = 0;
  test_writer_done    = 0;
  test_key( test_writer_ids[ 0 ], 31UL, 51U );

  /* 写方 A 已抢占写权限：序号为 1（偶数 0 +1）。 */
  f->version = 1UL;

  pthread_t writer;
  FD_TEST( !pthread_create( &writer, NULL, test_writer_thread, NULL ) );
  while( !test_writer_started ) FD_SPIN_PAUSE();

  /* 给写方 B 足够的时间撞上奇数序号：它必须还没有完成，也没有动表。 */
  for( ulong i=0UL; i<1000000UL && !test_writer_done; i++ ) FD_SPIN_PAUSE();
  FD_TEST( !test_writer_done );
  FD_TEST( f->version==1UL );
  FD_TEST( f->prog_cnt==0UL );

  /* 写方 A 写完：序号回到偶数，B 才能 CAS 成功。 */
  f->version = 2UL;
  FD_TEST( !pthread_join( writer, NULL ) );
  FD_TEST( test_writer_done );
  FD_TEST( f->version==4UL );   /* 2 --CAS--> 3 --end--> 4 */
  FD_TEST( f->prog_cnt==1UL );
  FD_TEST( fd_arbfilter_prog_idx( f, test_writer_ids[ 0 ] )!=ULONG_MAX );
}

#endif /* FD_HAS_THREADS */

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  char const * _page_sz = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",  NULL, "gigantic"               );
  ulong        page_cnt = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt", NULL, 1UL                      );
  ulong        numa_idx = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx", NULL, fd_shmem_numa_idx( 0UL ) );

  fd_wksp_t * wksp = fd_wksp_new_anonymous( fd_cstr_to_shmem_page_sz( _page_sz ), page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  void * mem = fd_wksp_alloc_laddr( wksp, fd_arbfilter_align(), fd_arbfilter_footprint(), 1UL );
  FD_TEST( mem );

  test_new_join       ( mem );
  test_prog_set       ( mem );
  test_acct_set       ( mem );
  test_match          ( mem );
  test_match_empty    ( mem );
  test_probe_bound    ( mem );
  test_stuck_writer   ( mem );
#if FD_HAS_THREADS
  test_writer_exclusion( mem );
#endif
  fd_wksp_free_laddr( mem );
  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
