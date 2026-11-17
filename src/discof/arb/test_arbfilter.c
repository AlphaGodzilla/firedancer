#include "fd_arbfilter.h"
#include "fd_arbnotify.h"

#include "../../util/fd_hash32.h"

#include <string.h>
#include <time.h>
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

/* test_key_find 返回 key 在 keys[0..cnt) 里的下标，找不到返回 ULONG_MAX。 */

static ulong
test_key_find( uchar const (*keys)[ 32UL ],
               ulong                  cnt,
               uchar const *          key ) {
  for( ulong i=0UL; i<cnt; i++ ) if( !memcmp( keys[ i ], key, 32UL ) ) return i;
  return ULONG_MAX;
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
  FD_TEST( f->stat_emit==0UL && f->stat_skip==0UL && f->stat_ns==0UL && f->stat_published==0UL && f->stat_noacct==0UL );

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
    FD_TEST( fd_arbfilter_prog_set( f, cnt, (uchar const (*)[32UL])ids, NULL )==0 );
    FD_TEST( f->prog_cnt==cnt );
    ver = test_check_version( f, ver );
    for( ulong i=0UL; i<cnt; i++ )         FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )!=ULONG_MAX );
    for( ulong i=cnt; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )==ULONG_MAX );
  }

  /* 装到上限（64 个）之后，表里没有任何空槽以外的残留：所有成员命中。 */
  FD_TEST( f->prog_cnt==FD_ARBFILTER_PROG_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_prog_idx( f, ids[ i ] )!=ULONG_MAX );

  /* 越界：返回 -1 且表保持原样。 */
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX+1UL, (uchar const (*)[32UL])ids, NULL )==-1 );
  FD_TEST( f->prog_cnt==FD_ARBFILTER_PROG_ENTRY_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ FD_ARBFILTER_PROG_ENTRY_MAX-1UL ] )!=ULONG_MAX );
  FD_TEST( f->version==ver );

  /* 重复 id 只存一份，且首个出现的 tag 生效。 */
  uchar dup[ 4UL ][ 32UL ];
  memcpy( dup[ 0 ], ids[ 0 ], 32UL );
  memcpy( dup[ 1 ], ids[ 3 ], 32UL );
  memcpy( dup[ 2 ], ids[ 0 ], 32UL );
  memcpy( dup[ 3 ], ids[ 3 ], 32UL );
  uchar dup_tags[ 4UL ] = { 5U, 6U, 7U, 8U };
  FD_TEST( fd_arbfilter_prog_set( f, 4UL, (uchar const (*)[32UL])dup, dup_tags )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==2UL );
  ulong dup_slot0 = fd_arbfilter_prog_idx( f, ids[ 0 ] );
  ulong dup_slot3 = fd_arbfilter_prog_idx( f, ids[ 3 ] );
  FD_TEST( dup_slot0!=ULONG_MAX );
  FD_TEST( dup_slot3!=ULONG_MAX );
  FD_TEST( f->prog[ dup_slot0 ].tag==5U );
  FD_TEST( f->prog[ dup_slot3 ].tag==6U );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 1 ] )==ULONG_MAX );

  /* tag 随表项落位：装到条目上限后每个成员槽里的 tag 都是它自己的那个。
     fd_arbfilter_prog_idx 给出的槽号就是要读 tag 的那个槽。 */
  uchar tags[ FD_ARBFILTER_PROG_ENTRY_MAX ];
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) tags[ i ] = (uchar)( 3UL*i+1UL );
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX, (uchar const (*)[32UL])ids, tags )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==FD_ARBFILTER_PROG_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) {
    ulong slot = fd_arbfilter_prog_idx( f, ids[ i ] );
    FD_TEST( slot!=ULONG_MAX );
    FD_TEST( f->prog[ slot ].tag==tags[ i ] );
  }

  /* tags 为 NULL 时所有表项的 tag 都是 0。 */
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX, (uchar const (*)[32UL])ids, NULL )==0 );
  ver = test_check_version( f, ver );
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) {
    ulong slot = fd_arbfilter_prog_idx( f, ids[ i ] );
    FD_TEST( slot!=ULONG_MAX );
    FD_TEST( !f->prog[ slot ].tag );
  }

  /* 整表替换成单项：旧成员立刻失效。 */
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])&ids[ 5 ], NULL )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->prog_cnt==1UL );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 5 ] )!=ULONG_MAX );
  FD_TEST( fd_arbfilter_prog_idx( f, ids[ 6 ] )==ULONG_MAX );

  /* 清空。 */
  FD_TEST( fd_arbfilter_prog_set( f, 0UL, NULL, NULL )==0 );
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
  FD_TEST( fd_arbfilter_acct_set( f, cnt, (uchar const (*)[32UL])keys, NULL )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==cnt );
  for( ulong i=0UL; i<cnt; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )!=ULONG_MAX );
  for( ulong i=cnt; i<cnt+64UL; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )==ULONG_MAX );

  /* 重复 key 只存一份，且首个出现的 tag 生效。 */
  uchar dup[ 4UL ][ 32UL ];
  memcpy( dup[ 0 ], keys[ 0 ], 32UL );
  memcpy( dup[ 1 ], keys[ 7 ], 32UL );
  memcpy( dup[ 2 ], keys[ 0 ], 32UL );
  memcpy( dup[ 3 ], keys[ 7 ], 32UL );
  uchar dup_tags[ 4UL ] = { 11U, 12U, 13U, 14U };
  FD_TEST( fd_arbfilter_acct_set( f, 4UL, (uchar const (*)[32UL])dup, dup_tags )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==2UL );
  ulong dup_slot0 = fd_arbfilter_acct_idx( f, keys[ 0 ] );
  ulong dup_slot7 = fd_arbfilter_acct_idx( f, keys[ 7 ] );
  FD_TEST( dup_slot0!=ULONG_MAX );
  FD_TEST( dup_slot7!=ULONG_MAX );
  FD_TEST( f->acct[ dup_slot0 ].tag==11U );
  FD_TEST( f->acct[ dup_slot7 ].tag==12U );
  FD_TEST( fd_arbfilter_acct_idx( f, keys[ 1 ] )==ULONG_MAX );

  /* 边界：装到条目上限。装载率因此只有 0.5，所有成员仍然命中，表外的
     key 在走完它所在的那条探测链之后判为未命中。 */
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys, NULL )==0 );
  ver = test_check_version( f, ver );
  FD_TEST( f->acct_cnt==FD_ARBFILTER_ACCT_ENTRY_MAX );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) FD_TEST( fd_arbfilter_acct_idx( f, keys[ i ] )!=ULONG_MAX );
  uchar absent[ 32UL ];
  test_key( absent, FD_ARBFILTER_ACCT_ENTRY_MAX+17UL, 11U );
  FD_TEST( fd_arbfilter_acct_idx( f, absent )==ULONG_MAX );

  /* tag 随表项落位：整表带 tags 安装后每个槽里的 tag 就是它自己的那个；tags
     为 NULL 时全部为 0。 */
  uchar tags[ FD_ARBFILTER_ACCT_ENTRY_MAX ];
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) tags[ i ] = (uchar)( (7UL*i+3UL) & 0xFFUL );
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys, tags )==0 );
  ver = test_check_version( f, ver );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) {
    ulong slot = fd_arbfilter_acct_idx( f, keys[ i ] );
    FD_TEST( slot!=ULONG_MAX );
    FD_TEST( f->acct[ slot ].tag==tags[ i ] );
  }
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys, NULL )==0 );
  ver = test_check_version( f, ver );
  for( ulong i=0UL; i<FD_ARBFILTER_ACCT_ENTRY_MAX; i++ ) {
    ulong slot = fd_arbfilter_acct_idx( f, keys[ i ] );
    FD_TEST( slot!=ULONG_MAX );
    FD_TEST( !f->acct[ slot ].tag );
  }

  /* 越界：返回 -1 且表保持原样。 */
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX+1UL, (uchar const (*)[32UL])keys, NULL )==-1 );
  FD_TEST( f->acct_cnt==FD_ARBFILTER_ACCT_ENTRY_MAX );
  FD_TEST( f->version==ver );

  /* 清空。 */
  FD_TEST( fd_arbfilter_acct_set( f, 0UL, NULL, NULL )==0 );
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
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_ids, NULL )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 1UL, (uchar const (*)[32UL])hot_keys , NULL )==0 );

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

  /* 只有热账户命中、完全不涉及关注程序：过滤逻辑先查程序表，程序表没命中就
     短路，热账户表一条也不探测，因此结果为 0。池账户只会被它自己的程序改
     动，而那个程序必然出现在账户列表里，所以这类短路不会漏掉真实的池子更新。 */
  fd_acct_addr_t list_r3[ 2UL ];
  memset( list_r3, 0, sizeof(list_r3) );
  memcpy( list_r3[ 0 ].b, pad_key, 32UL );
  memcpy( list_r3[ 1 ].b, hot_key, 32UL );
  FD_TEST( fd_arbfilter_match( f, txn, NULL, (uchar const *)list_r3, 2UL )==0UL );

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

  /* accs 为 NULL 时改用交易线上的静态账户列表。只有热账户时不命中（程序表先行
     短路）；把账户 1 换成程序表成员后 R1/R2 成立。 */
  uchar payload[ 256UL ];
  memset( payload, 0, sizeof(payload) );
  memcpy( payload+64UL,      pad_key, 32UL );
  memcpy( payload+64UL+32UL, hot_key,   32UL );
  txn = test_txn_init( txn_mem, 1UL, 2UL );
  txn->acct_addr_off = 64U;
  txn->instr[ 0 ].program_id = 0U;
  FD_TEST( fd_arbfilter_match( f, txn, payload, NULL, 0UL )==0UL );

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

/* test_collect 验证 fd_arbfilter_collect 的 key 收集：位掩码与
   fd_arbfilter_match 完全一致，命中的 key 按判定顺序去重写进 hits，容量不
   够时只截断 key 列表。 */

static void
test_collect( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_key[ 2UL ][ 32UL ];
  uchar hot_key [ 2UL ][ 32UL ];
  uchar pad_key [ 32UL ];
  test_key( prog_key[ 0 ], 60UL, 91U );
  test_key( prog_key[ 1 ], 61UL, 91U );
  test_key( hot_key [ 0 ], 62UL, 91U );
  test_key( hot_key [ 1 ], 63UL, 91U );
  test_key( pad_key,       64UL, 91U );

  FD_TEST( fd_arbfilter_prog_set( f, 2UL, (uchar const (*)[32UL])prog_key, NULL )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 2UL, (uchar const (*)[32UL])hot_key , NULL )==0 );

  /* 账户列表：[0]=付款方 [1]=程序成员 [2][3]=热账户 [4]=与 [1] 重复的 key。 */
  fd_acct_addr_t accs[ 5UL ];
  memset( accs, 0, sizeof(accs) );
  memcpy( accs[ 0 ].b, pad_key,       32UL );
  memcpy( accs[ 1 ].b, prog_key[ 0 ], 32UL );
  memcpy( accs[ 2 ].b, hot_key[ 0 ],  32UL );
  memcpy( accs[ 3 ].b, hot_key[ 1 ],  32UL );
  memcpy( accs[ 4 ].b, prog_key[ 0 ], 32UL );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, 5UL );

  fd_arbfilter_hit_t hits[ 8UL ];
  ulong              hit_cnt = 0UL;
  memset( hits, 0xAA, sizeof(hits) );

  /* 顶层指令调用付款方：R1 不成立，程序成员靠出现在静态账户列表里让 R2 成立，
     短路因此放行；热账户表命中 2 个可提交账户。程序成员本身只算程序命中，不进
     keys[]（keys[] 只放会被提交的账户）。 */
  txn->instr[ 0 ].program_id = 0U;
  ulong bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 5UL,
                                     NULL, NULL, NULL, 1, NULL, hits, &hit_cnt, NULL, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( bits==fd_arbfilter_match( f, txn, NULL, (uchar const *)accs, 5UL ) );
  FD_TEST( hit_cnt==2UL );
  FD_TEST( !memcmp( hits[ 0 ].key, hot_key[ 0 ],  32UL ) );
  FD_TEST( !memcmp( hits[ 1 ].key, hot_key[ 1 ],  32UL ) );
  FD_TEST( hits[ 2 ].key[ 0 ]==0xAA ); /* 有效条数之外不写 */

  /* 顶层指令直调程序成员：R1 成立。提交判据不设限时程序账户也算「可提交」，
     因此它进 keys[] 排在最前（真实执行里程序账户只读、不会被提交，永远进不了
     keys[]，命中身份走 prog_tag）。 */
  txn->instr[ 0 ].program_id = 1U;
  hit_cnt = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 5UL,
                               NULL, NULL, NULL, 1, NULL, hits, &hit_cnt, NULL, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==3UL );
  FD_TEST( !memcmp( hits[ 0 ].key, prog_key[ 0 ], 32UL ) );
  FD_TEST( !memcmp( hits[ 1 ].key, hot_key[ 0 ],  32UL ) );

  /* 容量只有 2 条：key 列表截断，位掩码不受影响。 */
  hit_cnt = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 5UL,
                               NULL, NULL, NULL, 1, NULL, hits, &hit_cnt, NULL, NULL, NULL, 0UL, 2UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==2UL );

  /* 不收集 key 时结果与 fd_arbfilter_match 相同。 */
  FD_TEST( fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 5UL,
                                 NULL, NULL, NULL, 1, NULL, NULL, NULL, NULL, NULL, NULL, 0UL, 0UL )
           ==fd_arbfilter_match( f, txn, NULL, (uchar const *)accs, 5UL ) );

  /* 未命中：不写任何 key。 */
  fd_acct_addr_t none[ 1UL ];
  memset( none, 0, sizeof(none) );
  txn->instr[ 0 ].program_id = 0U;
  hit_cnt = 99UL;
  FD_TEST( fd_arbfilter_collect( f, txn, NULL, (uchar const *)none, 1UL,
                                 NULL, NULL, NULL, 1, NULL, hits, &hit_cnt, NULL, NULL, NULL, 0UL, 8UL )==0UL );
  FD_TEST( hit_cnt==0UL );
}

/* test_collect_commit 验证收集只取「会被提交进 bank」的命中账户：只读命中、
   可写但执行期没被改动、本笔没有 accdb 引用的命中都不进 hits，位掩码仍然
   包含它们；交易失败（commit_ok==0）时 hits 为空而位掩码照常。 */

static void
test_collect_commit( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_id[ 1UL ][ 32UL ];
  uchar hot_key[ 5UL ][ 32UL ];
  test_key( prog_id[ 0 ], 80UL, 93U );
  for( ulong i=0UL; i<5UL; i++ ) test_key( hot_key[ i ], 81UL+i, 93U );
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_id, NULL )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 5UL, (uchar const (*)[32UL])hot_key, NULL )==0 );

  /* 账户列表：[0]=付款方（不在过滤器里）[1]=A [2]=B（只读）[3]=C（没被改动）
     [4]=程序账户 [5]=D（本笔没有 accdb 引用）[6]=E。 */
  fd_acct_addr_t accs[ 7UL ];
  memset( accs, 0, sizeof(accs) );
  memcpy( accs[ 1 ].b, hot_key[ 0 ], 32UL );
  memcpy( accs[ 2 ].b, hot_key[ 1 ], 32UL );
  memcpy( accs[ 3 ].b, hot_key[ 2 ], 32UL );
  memcpy( accs[ 4 ].b, prog_id[ 0 ], 32UL );
  memcpy( accs[ 5 ].b, hot_key[ 3 ], 32UL );
  memcpy( accs[ 6 ].b, hot_key[ 4 ], 32UL );

  uchar writable[ 7UL ] = { 1U, 1U, 0U, 1U, 0U, 1U, 1U };
  uchar touched [ 7UL ] = { 1U, 1U, 1U, 0U, 0U, 1U, 1U };
  uchar acquired[ 7UL ] = { 1U, 1U, 1U, 1U, 0U, 0U, 1U };

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, 7UL );
  txn->instr[ 0 ].program_id = 0U; /* 付款方不在程序表里，R1 不成立 */

  fd_arbfilter_hit_t hits[ 8UL ];
  ulong hit_cnt = 0UL;
  ulong more    = 0UL;
  ulong emitted = 0UL;

  /* 只有 [1] 与 [6] 同时满足三个判据；R2 由程序账户 [4] 给出、R3 由热账户
     给出，位掩码因此是完整的。 */
  ulong bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 7UL,
                                     writable, touched, acquired, 1,
                                     &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==2UL );
  FD_TEST( !memcmp( hits[ 0 ].key, hot_key[ 0 ], 32UL ) );
  FD_TEST( !memcmp( hits[ 1 ].key, hot_key[ 4 ], 32UL ) );
  FD_TEST( more==0UL );
  FD_TEST( emitted==( (1UL<<1)|(1UL<<6) ) );

  /* 三个判据掩码都传 NULL 时提交集不设限：hits 等于热账户表的全部命中（含只读
     的 [2] 与没有 accdb 引用的 [5]）。程序账户 [4] 只算程序命中，不进 keys[]。 */
  hit_cnt = 0UL; more = 0UL; emitted = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 7UL,
                               NULL, NULL, NULL, 1, &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==5UL );
  /* 索引 1,2,3,5,6 命中热账户表；索引 4 是程序账户，只算程序命中，不进 keys[]。 */
  FD_TEST( emitted==( ( (1UL<<7)-2UL ) & ~(1UL<<4) ) );

  /* 交易失败：什么都提交不了。程序命中照常（它由账户列表决定），热账户一位也
     不置——第三段只探测会提交的账户，失败交易没有这种账户。 */
  hit_cnt = 0UL; more = 0UL; emitted = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 7UL,
                               writable, touched, acquired, 0,
                               &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==FD_ARBFILTER_BIT_PROG_ACCOUNT );
  FD_TEST( hit_cnt==0UL );
  FD_TEST( more==0UL );
  FD_TEST( emitted==0UL );

  /* 只有 touched 不设限：被改动过的只读账户 [2] 仍然不进 hits，三个判据必
     须同时成立。 */
  uchar touched_all[ 7UL ] = { 1U, 1U, 1U, 1U, 1U, 1U, 1U };
  hit_cnt = 0UL; more = 0UL; emitted = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 7UL,
                               writable, touched_all, acquired, 1,
                               &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, 8UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==3UL );
  FD_TEST( emitted==( (1UL<<1)|(1UL<<3)|(1UL<<6) ) );
  FD_TEST( !memcmp( hits[ 0 ].key, hot_key[ 0 ], 32UL ) );
  FD_TEST( !memcmp( hits[ 1 ].key, hot_key[ 2 ], 32UL ) );
  FD_TEST( !memcmp( hits[ 2 ].key, hot_key[ 4 ], 32UL ) );
}

/* test_collect_tags 验证命中 key 上的 tag：R1 命中取程序表项的 tag，R3 命中取
   热账户表项的 tag，两表都命中时取热账户表的 tag、它为 0 时退回程序表的 tag。
   tag 不参与判定：同样的两张表不带 tag 安装时，位掩码与
   fd_arbfilter_match 的返回值逐位相同，收集到的 tag 全为 0。 */

static void
test_collect_tags( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_key[ 3UL ][ 32UL ];
  uchar acct_key[ 3UL ][ 32UL ];
  test_key( prog_key[ 0 ], 140UL, 131U ); /* 只在程序表里 */
  test_key( prog_key[ 1 ], 141UL, 131U ); /* 两表都在，热账户表的 tag 为 0 */
  test_key( prog_key[ 2 ], 142UL, 131U ); /* 两表都在，热账户表的 tag 非 0 */
  test_key( acct_key[ 0 ], 143UL, 131U ); /* 只在热账户表里 */
  memcpy( acct_key[ 1 ], prog_key[ 1 ], 32UL );
  memcpy( acct_key[ 2 ], prog_key[ 2 ], 32UL );

  uchar prog_tags[ 3UL ] = { 7U, 21U, 30U };
  uchar acct_tags[ 3UL ] = { 5U,  0U,  9U };

  /* 每个用例只放「付款方 + 一个关注 key」，位掩码只由被测规则决定。 */
  uchar pad_key[ 32UL ];
  test_key( pad_key, 144UL, 131U );

  fd_acct_addr_t accs[ 2UL ];
  memset( accs, 0, sizeof(accs) );
  memcpy( accs[ 0 ].b, pad_key, 32UL );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, 2UL );

  /* case_pid 是顶层指令的 program_id：1 表示账户 1 就是指令直调的程序（R1 成
     立），0 表示指令调用的是付款方，R1 不成立。 */
  uchar const * case_key [ 4UL ] = { prog_key[ 0 ], acct_key[ 0 ], prog_key[ 1 ], prog_key[ 2 ] };
  uchar         case_pid [ 4UL ] = { 1U, 0U, 0U, 0U };
  /* 第 2 个用例只有热账户命中、不涉及任何关注程序，程序表先行会短路，因此位掩
     码与命中数都是 0：这是过滤逻辑的既定语义，不是缺陷。 */
  ulong         case_bits[ 4UL ] = {
    FD_ARBFILTER_BIT_PROG_DIRECT |FD_ARBFILTER_BIT_PROG_ACCOUNT,
    0UL,
    FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT,
    FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT };
  uchar         case_tag [ 4UL ] = { 7U, 0U, 21U, 9U };
  ulong         case_hits[ 4UL ] = { 1UL, 0UL, 1UL, 1UL };

  for( int pass=0; pass<2; pass++ ) {
    /* pass 0 带 tag 安装，pass 1 不带 tag 安装。 */
    FD_TEST( fd_arbfilter_prog_set( f, 3UL, (uchar const (*)[32UL])prog_key, pass ? NULL : prog_tags )==0 );
    FD_TEST( fd_arbfilter_acct_set( f, 3UL, (uchar const (*)[32UL])acct_key, pass ? NULL : acct_tags )==0 );

    for( ulong c=0UL; c<4UL; c++ ) {
      memcpy( accs[ 1 ].b, case_key[ c ], 32UL );
      txn->instr[ 0 ].program_id = case_pid[ c ];

      fd_arbfilter_hit_t hits[ 4UL ];
      ulong              hit_cnt = 0UL;
      memset( hits, 0xAA, sizeof(hits) );

      ulong bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 2UL,
                                        NULL, NULL, NULL, 1, NULL, hits, &hit_cnt, NULL, NULL, NULL, 0UL, 4UL );
      FD_TEST( bits==case_bits[ c ] );
      FD_TEST( bits==fd_arbfilter_match( f, txn, NULL, (uchar const *)accs, 2UL ) );
      FD_TEST( hit_cnt==case_hits[ c ] );
      if( case_hits[ c ] ) {
        FD_TEST( hits[ 0 ].tag==( pass ? 0U : case_tag[ c ] ) );
        FD_TEST( hits[ 1 ].tag==0xAA ); /* 有效条数之外不写 */
      }
    }
  }
}

/* test_collect_prog_tags 验证「命中的关注程序以 tag 形式随记录发布」：程序账户
   永远只读、不会被提交，所以它的 key 不进 hits，命中身份只能由 prog_tag 表达。
   R1 与 R2 命中都要记，且同一 tag 只出现一次；表项没设 tag 时不记。 */

static void
test_collect_prog_tags( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  uchar prog_key[ 3UL ][ 32UL ];
  test_key( prog_key[ 0 ], 200UL, 141U );
  test_key( prog_key[ 1 ], 201UL, 141U );
  test_key( prog_key[ 2 ], 202UL, 141U );

  uchar pad_key[ 32UL ];
  test_key( pad_key, 203UL, 141U );

  uchar prog_tags[ 3UL ] = { 7U, 21U, 30U };
  FD_TEST( fd_arbfilter_prog_set( f, 3UL, (uchar const (*)[32UL])prog_key, prog_tags )==0 );

  /* 账户列表：[0]=付款方 [1]=被直调的程序 [2]=出现在账户表里的另一个程序。 */
  fd_acct_addr_t accs[ 3UL ];
  memset( accs, 0, sizeof(accs) );
  memcpy( accs[ 0 ].b, pad_key,       32UL );
  memcpy( accs[ 1 ].b, prog_key[ 0 ], 32UL );
  memcpy( accs[ 2 ].b, prog_key[ 2 ], 32UL );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, 3UL );

  /* 指令直调账户 1（tag 7）；账户 2 是另一个关注程序（tag 30）出现在账户表里。
     两条规则同时命中，prog_tag 应依次是 {7,30}，去重且不重复。 */
  txn->instr[ 0 ].program_id = 1U;

  uchar prog_tag[ 8UL ];
  ulong prog_tag_cnt = 0UL;
  memset( prog_tag, 0xAA, sizeof(prog_tag) );

  ulong bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 3UL,
                                     NULL, NULL, NULL, 1, NULL,
                                     NULL, NULL, NULL,
                                     prog_tag, &prog_tag_cnt, 8UL, 0UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT ) );
  FD_TEST( prog_tag_cnt==2UL );
  FD_TEST( prog_tag[ 0 ]==7U );
  FD_TEST( prog_tag[ 1 ]==30U );
  FD_TEST( prog_tag[ 2 ]==0xAA ); /* 有效条数之外不写 */

  /* 程序表不带 tag 安装：命中仍然成立，但 prog_tag 列表为空。 */
  FD_TEST( fd_arbfilter_prog_set( f, 3UL, (uchar const (*)[32UL])prog_key, NULL )==0 );
  prog_tag_cnt = 99UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, 3UL,
                               NULL, NULL, NULL, 1, NULL,
                               NULL, NULL, NULL,
                               prog_tag, &prog_tag_cnt, 8UL, 0UL );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_DIRECT|FD_ARBFILTER_BIT_PROG_ACCOUNT ) );
  FD_TEST( prog_tag_cnt==0UL );
}

/* test_collect_parts 验证分片收集：可提交命中超过 hits_cap 时 *more 置 1、
   已发布的索引记进 emitted；用同一份 emitted 再调一次拿到下一片，两片合起
   来覆盖全部可提交命中且不重复。 */

static void
test_collect_parts( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  /* 10 个可提交热账户命中，一条记录只装得下 8 个：按 8+2 分两片发布。 */
  enum { HOT_CNT = 10UL };

  uchar hot_key[ HOT_CNT ][ 32UL ];
  for( ulong i=0UL; i<HOT_CNT; i++ ) test_key( hot_key[ i ], 100UL+i, 113U );
  FD_TEST( fd_arbfilter_acct_set( f, HOT_CNT, (uchar const (*)[32UL])hot_key, NULL )==0 );

  /* 过滤逻辑先查程序表、没命中就短路，所以这里必须放一个关注程序账户，否则
     热账户一条也不会被探测。程序账户放在索引 1，占一个只读槽位。 */
  uchar prog_id[ 1UL ][ 32UL ];
  test_key( prog_id[ 0 ], 111UL, 113U );
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_id, NULL )==0 );

  fd_acct_addr_t accs[ HOT_CNT+2UL ];
  memset( accs, 0, sizeof(accs) );
  memcpy( accs[ 1 ].b, prog_id[ 0 ], 32UL );
  for( ulong i=0UL; i<HOT_CNT; i++ ) memcpy( accs[ i+2UL ].b, hot_key[ i ], 32UL );

  uchar all_set[ HOT_CNT+2UL ];
  memset( all_set, 1, sizeof(all_set) );

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 1UL, HOT_CNT+2UL );
  txn->instr[ 0 ].program_id = 0U; /* 付款方不在程序表里；程序账户靠 R2 命中 */

  fd_arbfilter_hit_t hits[ FD_ARBNOTIFY_KEY_MAX ];
  ulong hit_cnt = 0UL;
  ulong more    = 0UL;
  ulong emitted = 0UL;

  /* 第一片：收满 FD_ARBNOTIFY_KEY_MAX 条，more 置 1。 */
  ulong bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, HOT_CNT+2UL,
                                     all_set, all_set, all_set, 1,
                                     &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, FD_ARBNOTIFY_KEY_MAX );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==FD_ARBNOTIFY_KEY_MAX );
  FD_TEST( more==1UL );
  /* 热账户从索引 2 开始，第一片收的是索引 2..9（程序账户在索引 1，不算热账户）。 */
  FD_TEST( emitted==( ( (1UL<<(FD_ARBNOTIFY_KEY_MAX+2UL))-1UL ) & ~3UL ) );

  ulong seen = 0UL; /* 第 k 位表示 hot_key[k] 已经出现在某一片里 */
  for( ulong i=0UL; i<hit_cnt; i++ ) {
    ulong k = test_key_find( (uchar const (*)[32UL])hot_key, HOT_CNT, hits[ i ].key );
    FD_TEST( k!=ULONG_MAX );
    FD_TEST( !( seen & (1UL<<k) ) );
    seen |= 1UL<<k;
  }
  FD_TEST( seen==( (1UL<<FD_ARBNOTIFY_KEY_MAX)-1UL ) ); /* hot_key[0..7] */

  /* 第二片：带上第一片的 emitted，只剩后 2 个可提交命中。 */
  hit_cnt = 0UL;
  more    = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, HOT_CNT+2UL,
                               all_set, all_set, all_set, 1,
                               &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, FD_ARBNOTIFY_KEY_MAX );
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==HOT_CNT-FD_ARBNOTIFY_KEY_MAX );
  FD_TEST( more==0UL );
  FD_TEST( emitted==( ( (1UL<<(HOT_CNT+2UL))-1UL )&~3UL ) ); /* 索引 2..11 */

  for( ulong i=0UL; i<hit_cnt; i++ ) {
    ulong k = test_key_find( (uchar const (*)[32UL])hot_key, HOT_CNT, hits[ i ].key );
    FD_TEST( k!=ULONG_MAX );
    FD_TEST( !( seen & (1UL<<k) ) );
    seen |= 1UL<<k;
  }
  FD_TEST( seen==( (1UL<<HOT_CNT)-1UL ) ); /* 两片合起来覆盖全部热账户 */

  /* 第三片：没有剩余命中，more 保持 0。 */
  hit_cnt = 0UL;
  more    = 0UL;
  bits = fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, HOT_CNT+2UL,
                               all_set, all_set, all_set, 1,
                               &emitted, hits, &hit_cnt, &more, NULL, NULL, 0UL, FD_ARBNOTIFY_KEY_MAX );
  /* 第三片：热账户都已发布，但位掩码描述的是「这笔交易命中了哪些规则」，与分片
     进度无关，因此仍是程序 + 热账户两位。 */
  FD_TEST( bits==( FD_ARBFILTER_BIT_PROG_ACCOUNT|FD_ARBFILTER_BIT_HOT_ACCOUNT ) );
  FD_TEST( hit_cnt==0UL );
  FD_TEST( more==0UL );
}

/* test_notify_fill 验证通知记录的字段落位与截断：part/more 与 key_cnt 按入
   参落位，key_cnt 与实际写入的条数一致，tag 逐条随 key 抄进 tags[]，未用
   到的 key 槽与 tag 槽、填充字节与缺省签名都是确定的 0。 */

static void
test_notify_fill( void ) {
  /* 尺寸直接决定 dcache 的槽大小，必须由静态断言钉住；这里再断一次，让运行
     时也看得见。 */
  FD_TEST( sizeof(fd_arbnotify_t)==376UL );
  FD_TEST( sizeof(fd_arbnotify_t)%8UL==0UL );
  /* dcache 的槽按 128 字节对齐：加上 index_in_slot 后记录是 376 字节，仍落在
     384 字节的槽里，环占用的内存不变。 */
  FD_TEST( ( ( sizeof(fd_arbnotify_t)+127UL )/128UL )*128UL==384UL );

  fd_arbfilter_hit_t hits[ FD_ARBNOTIFY_KEY_MAX+2UL ];
  for( ulong i=0UL; i<FD_ARBNOTIFY_KEY_MAX+2UL; i++ ) {
    test_key( hits[ i ].key, 70UL+i, 97U );
    hits[ i ].tag = (uchar)( 20UL*i+3UL );
  }

  uchar sig[ 64UL ];
  for( ulong i=0UL; i<64UL; i++ ) sig[ i ] = (uchar)i;

  fd_arbnotify_t rec[1];
  memset( rec, 0xFF, sizeof(fd_arbnotify_t) );

  fd_arbnotify_t * ret = fd_arbnotify_fill( rec, 12345UL, 7UL, 4242UL, (ushort)3U, 0U, (ushort)0x6U, sig, hits, 3UL, NULL, 0UL, 2U, 1U );
  FD_TEST( ret==rec );
  FD_TEST( rec->slot==12345UL );
  FD_TEST( rec->bank_seq==7UL );
  FD_TEST( rec->accdb_fork_id==3U );
  FD_TEST( rec->accdb_generation==0U );
  FD_TEST( rec->matched==0x6U );
  FD_TEST( rec->key_cnt==3UL );
  FD_TEST( rec->part==2U );
  FD_TEST( rec->more==1U );
  FD_TEST( !memcmp( rec->sig, sig, 64UL ) );
  for( ulong i=0UL; i<3UL; i++ ) {
    FD_TEST( !memcmp( rec->keys[ i ], hits[ i ].key, 32UL ) );
    FD_TEST( rec->tags[ i ]==hits[ i ].tag );
  }

  /* fill 先清零整条记录：容量之外的 key 槽、tag 槽与填充字节都是 0。 */
  for( ulong i=3UL; i<FD_ARBNOTIFY_KEY_MAX; i++ ) {
    for( ulong j=0UL; j<32UL; j++ ) FD_TEST( !rec->keys[ i ][ j ] );
    FD_TEST( !rec->tags[ i ] );
  }
  FD_TEST( !rec->_pad );

  /* 超过上限只写满 FD_ARBNOTIFY_KEY_MAX 条，key_cnt 报上限。 */
  ret = fd_arbnotify_fill( rec, 1UL, 2UL, 9UL, (ushort)0U, 0U, (ushort)0x2U, NULL, hits, FD_ARBNOTIFY_KEY_MAX+2UL, NULL, 0UL, 0U, 0U );
  FD_TEST( ret->key_cnt==FD_ARBNOTIFY_KEY_MAX );
  for( ulong i=0UL; i<FD_ARBNOTIFY_KEY_MAX; i++ ) {
    FD_TEST( !memcmp( rec->keys[ i ], hits[ i ].key, 32UL ) );
    FD_TEST( rec->tags[ i ]==hits[ i ].tag );
  }

  /* sig 为 NULL 时签名区写全 0。 */
  for( ulong i=0UL; i<64UL; i++ ) FD_TEST( !rec->sig[ i ] );
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
  FD_TEST( fd_arbfilter_acct_set( f, FD_ARBFILTER_ACCT_ENTRY_MAX, (uchar const (*)[32UL])keys, NULL )==0 );

  uchar prog_ids[ FD_ARBFILTER_PROG_ENTRY_MAX ][ 32UL ];
  for( ulong i=0UL; i<FD_ARBFILTER_PROG_ENTRY_MAX; i++ ) test_key( prog_ids[ i ], i, 73U );
  FD_TEST( fd_arbfilter_prog_set( f, FD_ARBFILTER_PROG_ENTRY_MAX, (uchar const (*)[32UL])prog_ids, NULL )==0 );

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
  FD_TEST( fd_arbfilter_prog_set( f, 1UL, (uchar const (*)[32UL])prog_ids, NULL )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, 1UL, (uchar const (*)[32UL])hot_keys, NULL )==0 );

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
  FD_TEST( fd_arbfilter_prog_set( test_writer_filter, 1UL, (uchar const (*)[32UL])test_writer_ids, NULL )==0 );
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

/* ===== 微基准（只在 `test_arbfilter bench` 时运行） =====

   目的是把「32 字节 key 的比较与哈希」从整条链路里单独拉出来量，并在同一次
   运行、同一台机器上对比四种组合：

     FNV-1a + fd_memeq   改造前的实现
     FNV-1a + 字比较     只换比较
     fd_hash32 + fd_memeq  只换哈希
     fd_hash32 + 字比较  当前实现

   表布局与生产一致（2048 槽、1024 项、线性探测），查询一半命中表内 key、一半
   是表外随机 key。输出每次查询的纳秒数与平均探测格数——后者反映哈希质量，混合
   差会让未命中多走几格，而每多走一格就是一次随机访存。 */

typedef ulong (*arb_bench_hash_t)( uchar const * );
typedef int   (*arb_bench_eq_t)  ( uchar const *, uchar const * );

static ulong
bench_hash_fnv( uchar const * key ) {
  ulong h = 0xcbf29ce484222325UL;
  for( ulong i=0UL; i<4UL; i++ ) {
    h ^= FD_LOAD( ulong, key + 8UL*i );
    h *= 0x100000001b3UL;
  }
  return h;
}

static ulong
bench_hash_fd32( uchar const * key ) {
  return fd_hash32( key, 0x61726266696c7465UL );
}

static int
bench_eq_cmpsb( uchar const * a,
                uchar const * b ) {
  return fd_memeq( a, b, 32UL );
}

/* 微基准自带一份「字比较」实现：它曾经进过生产代码，实测比 fd_memeq 慢，于是
   被回退。留在基准里是为了以后有人想再试时能直接量，而不必改生产路径。 */
static int
bench_eq_words( uchar const * a,
                uchar const * b ) {
  return !( ( FD_LOAD( ulong, a      )^FD_LOAD( ulong, b      ) ) |
            ( FD_LOAD( ulong, a+ 8UL )^FD_LOAD( ulong, b+ 8UL ) ) |
            ( FD_LOAD( ulong, a+16UL )^FD_LOAD( ulong, b+16UL ) ) |
            ( FD_LOAD( ulong, a+24UL )^FD_LOAD( ulong, b+24UL ) ) );
}

static ulong
bench_rand( ulong * st ) {
  ulong x = *st;
  x ^= x<<13; x ^= x>>7; x ^= x<<17;
  *st = x;
  return x;
}

static void
bench_one( char const *    name,
           arb_bench_hash_t hash,
           arb_bench_eq_t   eq ) {
  enum { SLOT_CNT = 2048UL, ENT_CNT = 1024UL, LOOKUP_CNT = 2000000UL };

  static uchar ent_key[ SLOT_CNT ][ 32UL ];
  static uchar ent_used[ SLOT_CNT ];
  static uchar probes [ ENT_CNT ][ 32UL ]; /* 一半查询用表内 key，一半用表外 key */

  memset( ent_used, 0, sizeof(ent_used) );

  ulong mask = SLOT_CNT - 1UL;
  ulong rng  = 0x9e3779b97f4a7c15UL;

  /* 装表：与生产一致的线性探测插入。 */
  for( ulong i=0UL; i<ENT_CNT; i++ ) {
    uchar key[ 32UL ];
    for( ulong w=0UL; w<4UL; w++ ) {
      ulong v = bench_rand( &rng );
      memcpy( key + 8UL*w, &v, 8UL );
    }
    ulong idx = hash( key ) & mask;
    while( ent_used[ idx ] ) idx = (idx + 1UL) & mask;
    ent_used[ idx ] = 1;
    memcpy( ent_key[ idx ], key, 32UL );
    memcpy( probes[ i ], key, 32UL );
  }

  /* 查询序列：偶数下标的查询改用表外 key，保证一半未命中。 */
  for( ulong i=0UL; i<ENT_CNT; i++ ) {
    if( i & 1UL ) continue;
    uchar miss[ 32UL ];
    for( ulong w=0UL; w<4UL; w++ ) {
      ulong v = bench_rand( &rng );
      memcpy( miss + 8UL*w, &v, 8UL );
    }
    memcpy( probes[ i ], miss, 32UL );
  }

  ulong probes_total = 0UL;
  ulong found        = 0UL;

  struct timespec t0, t1;
  clock_gettime( CLOCK_MONOTONIC, &t0 );
  for( ulong n=0UL; n<LOOKUP_CNT; n++ ) {
    uchar const * key = probes[ n % ENT_CNT ];
    ulong         idx = hash( key ) & mask;
    for( ulong probe=0UL; probe<=mask; probe++ ) {
      probes_total++;
      if( !ent_used[ idx ] ) break;
      if( FD_UNLIKELY( eq( ent_key[ idx ], key ) ) ) { found++; break; }
      idx = (idx + 1UL) & mask;
    }
  }
  clock_gettime( CLOCK_MONOTONIC, &t1 );

  double ns = ( (double)( t1.tv_sec - t0.tv_sec )*1e9 + (double)( t1.tv_nsec - t0.tv_nsec ) ) / (double)LOOKUP_CNT;

  /* 表里每个 key 都在，偶数位是表外 key：命中数应约为一半。 */
  FD_LOG_NOTICE(( "BENCH %-22s %6.2f ns/查询  平均 %.2f 格  命中 %lu/%lu",
                  name, ns, (double)probes_total/(double)LOOKUP_CNT,
                  (ulong)found, (ulong)LOOKUP_CNT ));
}

/* bench_collect 量「一笔交易走完三轮过滤」的实际耗时，而不是单次查找的价格：
   前两轮查的是程序表（128 槽，全在 L1），第三轮查的是热账户表（2048 槽，68 KiB，
   主要在 L2），所以单次查找的 ns 不能直接乘次数。

   两个场景各量一次：
     miss  第①②轮都没命中关注程序 → 短路返回，第三轮一次都不探测
     hit   第②轮命中程序、第③轮在 4 个可提交账户里命中 1 个热账户 */

static void
bench_collect( void * mem ) {
  fd_arbfilter_shmem_t * f = fd_arbfilter_join( fd_arbfilter_new( mem ) );
  FD_TEST( f );

  enum { PROG_CNT = 64UL, ACCT_CNT = 512UL, ACCT_TOTAL = 20UL, LOOKUPS = 1000000UL };

  /* 两张表都按生产的装载率 0.5 装。 */
  uchar prog_key[ PROG_CNT ][ 32UL ];
  uchar acct_key[ ACCT_CNT ][ 32UL ];
  for( ulong i=0UL; i<PROG_CNT; i++ ) test_key( prog_key[ i ], 300UL+i, 151U );
  for( ulong i=0UL; i<ACCT_CNT; i++ ) test_key( acct_key[ i ], 400UL+i, 151U );
  FD_TEST( fd_arbfilter_prog_set( f, PROG_CNT, (uchar const (*)[32UL])prog_key, NULL )==0 );
  FD_TEST( fd_arbfilter_acct_set( f, ACCT_CNT, (uchar const (*)[32UL])acct_key, NULL )==0 );

  /* 一笔典型的 DeFi 交易：20 个静态账户、2 条指令、4 个可提交可写账户。
     账户 [1] 是关注程序（CPI 形态），账户 [2] 是热账户，其余是无关账户。 */
  uchar accs[ ACCT_TOTAL ][ 32UL ];
  for( ulong i=0UL; i<ACCT_TOTAL; i++ ) test_key( accs[ i ], 500UL+i, 151U );
  memcpy( accs[ 1 ], prog_key[ 3 ], 32UL );
  memcpy( accs[ 2 ], acct_key[ 7 ], 32UL );

  uchar writable[ ACCT_TOTAL ];
  uchar touched [ ACCT_TOTAL ];
  uchar acquired[ ACCT_TOTAL ];
  memset( writable, 0, sizeof(writable) );
  memset( touched,  0, sizeof(touched ) );
  memset( acquired, 0, sizeof(acquired) );
  for( ulong i=0UL; i<4UL; i++ ) { writable[ i ]=1U; touched[ i ]=1U; acquired[ i ]=1U; }
  writable[ 2 ] = 1U; /* 热账户可提交 */
  touched [ 2 ] = 1U;
  acquired[ 2 ] = 1U;

  uchar txn_mem[ 512UL ] __attribute__((aligned(alignof(fd_txn_t))));
  fd_txn_t * txn = test_txn_init( txn_mem, 2UL, ACCT_TOTAL );
  txn->instr[ 0 ].program_id = 0U; /* 付款方，不在程序表里 */
  txn->instr[ 1 ].program_id = 0U;

  fd_arbfilter_hit_t hits[ FD_ARBNOTIFY_KEY_MAX ];
  struct timespec t0, t1;
  ulong sink = 0UL;

  /* 场景 1：全部命中程序（账户 [1] 是关注程序），第三轮探 4 个账户。 */
  clock_gettime( CLOCK_MONOTONIC, &t0 );
  for( ulong n=0UL; n<LOOKUPS; n++ ) {
    ulong cnt = 0UL, more = 0UL, emitted = 0UL;
    sink += fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, ACCT_TOTAL,
                                  writable, touched, acquired, 1,
                                  &emitted, hits, &cnt, &more, NULL, NULL, 0UL, FD_ARBNOTIFY_KEY_MAX );
    sink += cnt;
  }
  clock_gettime( CLOCK_MONOTONIC, &t1 );
  double ns_hit = ( (double)( t1.tv_sec - t0.tv_sec )*1e9 + (double)( t1.tv_nsec - t0.tv_nsec ) ) / (double)LOOKUPS;

  /* 场景 2：把关注程序账户换掉，第①②轮都不命中 → 短路。 */
  memcpy( accs[ 1 ], accs[ 5 ], 32UL );
  clock_gettime( CLOCK_MONOTONIC, &t0 );
  for( ulong n=0UL; n<LOOKUPS; n++ ) {
    ulong cnt = 0UL, more = 0UL, emitted = 0UL;
    sink += fd_arbfilter_collect( f, txn, NULL, (uchar const *)accs, ACCT_TOTAL,
                                  writable, touched, acquired, 1,
                                  &emitted, hits, &cnt, &more, NULL, NULL, 0UL, FD_ARBNOTIFY_KEY_MAX );
    sink += cnt;
  }
  clock_gettime( CLOCK_MONOTONIC, &t1 );
  double ns_miss = ( (double)( t1.tv_sec - t0.tv_sec )*1e9 + (double)( t1.tv_nsec - t0.tv_nsec ) ) / (double)LOOKUPS;

  FD_TEST( sink!=ULONG_MAX ); /* 防止整个循环被优化掉 */

  FD_LOG_NOTICE(( "TRANSACTION 三轮全走（程序命中 + 4 个可提交账户）: %6.1f ns", ns_hit ));
  FD_LOG_NOTICE(( "TRANSACTION 短路返回（程序表未命中）:                 %6.1f ns", ns_miss ));

  (void)mem;
}

static void
bench_all( void * mem ) {
  bench_one( "FNV-1a + fd_memeq",   bench_hash_fnv,  bench_eq_cmpsb );
  bench_one( "FNV-1a + 字比较",     bench_hash_fnv,  bench_eq_words );
  bench_one( "fd_hash32 + fd_memeq",bench_hash_fd32, bench_eq_cmpsb );
  bench_one( "fd_hash32 + 字比较",  bench_hash_fd32, bench_eq_words );
  bench_collect( mem );
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  /* 微基准不依赖工作区，单独一条路径，不进 CI 的常规执行。 */
  if( argc>1 && !strcmp( argv[ 1 ], "bench" ) ) {
    /* 三轮过滤要一块格式化的过滤器内存，借用工作区之前先自己 new 一块普通内存。 */
    static uchar bench_mem[ 1UL<<20 ] __attribute__((aligned(4096)));
    memset( bench_mem, 0, sizeof(bench_mem) );
    bench_all( bench_mem );
    fd_halt();
    return 0;
  }

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
  test_collect        ( mem );
  test_collect_commit ( mem );
  test_collect_tags   ( mem );
  test_collect_prog_tags( mem );
  test_collect_parts  ( mem );
  test_notify_fill    ( );
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
