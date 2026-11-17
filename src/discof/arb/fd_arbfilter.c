#include "fd_arbfilter.h"

#include <string.h>

/* 槽数与条目上限的分工见头文件：槽数固定且是 2 的幂，条目上限不超过
   槽数的一半，装载率因此永远 <= 0.5，未命中查询的探测次数保持个位数。 */

FD_STATIC_ASSERT( (FD_ARBFILTER_PROG_SLOT_MAX & (FD_ARBFILTER_PROG_SLOT_MAX-1UL))==0UL, prog_slot_pow2   );
FD_STATIC_ASSERT( (FD_ARBFILTER_ACCT_SLOT_MAX & (FD_ARBFILTER_ACCT_SLOT_MAX-1UL))==0UL, acct_slot_pow2   );
FD_STATIC_ASSERT( FD_ARBFILTER_PROG_ENTRY_MAX>0UL,                                      prog_entry_nonzero );

/* 装载率上限：任何一张表装满可配置条目时，槽数仍有一半是空的。这是热路径
   上「未命中查询代价有界」的唯一依据，不能放宽。 */
FD_STATIC_ASSERT( FD_ARBFILTER_PROG_ENTRY_MAX*2UL<=FD_ARBFILTER_PROG_SLOT_MAX, prog_load_factor );
FD_STATIC_ASSERT( FD_ARBFILTER_ACCT_ENTRY_MAX*2UL<=FD_ARBFILTER_ACCT_SLOT_MAX, acct_load_factor );

/* arbfilter_tbl_insert 把 key 与它的 tag 放进表里，重复的 key 只存一份且首个
   出现的 tag 生效。返回 1 表示占用了新槽，0 表示 key 已经在表里（tag 保持
   不变）。槽数不小于待插入 key 上限时循环一定能找到空槽，因此不会丢 key。 */

static inline int
arbfilter_tbl_insert( fd_arbfilter_ent_t * ent,
                      ulong                mask,
                      uchar const *        key,
                      uchar                tag ) {
  ulong idx = arbfilter_hash( key ) & mask;
  for( ulong probe=0UL; probe<=mask; probe++ ) {
    if( FD_LIKELY( !ent[ idx ].used ) ) {
      memcpy( ent[ idx ].key, key, 32UL );
      ent[ idx ].tag  = tag;
      ent[ idx ].used = 1;
      return 1;
    }
    if( FD_UNLIKELY( fd_memeq( ent[ idx ].key, key, 32UL ) ) ) return 0; /* 重复 key */
    idx = (idx+1UL) & mask;
  }
  return 0;
}

/* arbfilter_ver_load 读取配置表的 seqlock 序号。奇数表示某个写方正在替换
   配置表。 */

static inline ulong
arbfilter_ver_load( fd_arbfilter_shmem_t const * f ) {
  return __atomic_load_n( &f->version, __ATOMIC_ACQUIRE );
}

/* arbfilter_ver_begin 抢占写权限：把序号从偶数 CAS 成该偶数+1。

   CAS 是写方之间的互斥手段：序号为奇数时说明另一个写方正在改表，后来者
   自旋等待它结束，不会有两张表交错写入，也不会出现两个写方各自把序号
   从同一个偶数推进一格、让读方误以为表已稳定的情况。抢占成功返回原来的
   偶数序号，arbfilter_ver_end 用它算出新的稳定序号。

   写方在持有写权限期间崩溃会让序号永久停在奇数：之后的写方会一直自旋，
   读方则按未命中返回直到验证器重启。这条路径只影响本地配置工具，代价是
   可接受的。 */

static inline ulong
arbfilter_ver_begin( fd_arbfilter_shmem_t * f ) {
  ulong ver = __atomic_load_n( &f->version, __ATOMIC_RELAXED );
  for(;;) {
    if( FD_UNLIKELY( ver & 1UL ) ) { /* 另一个写方正在替换 */
      FD_SPIN_PAUSE();
      ver = __atomic_load_n( &f->version, __ATOMIC_RELAXED );
      continue;
    }
    /* 弱 CAS 允许伪失败；失败时 ver 已被更新成当前值，回到循环顶部重新
       判断奇偶。 */
    if( FD_LIKELY( __atomic_compare_exchange_n( &f->version, &ver, ver+1UL, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED ) ) ) return ver;
  }
}

/* arbfilter_ver_end 结束写入：把序号推进到下一个偶数。写方独占奇数序号，
   因此这一次 store 不会与任何其它写方冲突。 */

static inline void
arbfilter_ver_end( fd_arbfilter_shmem_t * f,
                   ulong                  ver ) {
  __atomic_store_n( &f->version, ver+2UL, __ATOMIC_RELEASE ); /* 偶数：稳定 */
}

ulong
fd_arbfilter_align( void ) {
  return FD_ARBFILTER_ALIGN;
}

ulong
fd_arbfilter_footprint( void ) {
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, FD_ARBFILTER_ALIGN, sizeof(fd_arbfilter_shmem_t) );
  return FD_LAYOUT_FINI( l, FD_ARBFILTER_ALIGN );
}

void *
fd_arbfilter_new( void * shmem ) {
  if( FD_UNLIKELY( !shmem ) ) {
    FD_LOG_WARNING(( "NULL shmem" ));
    return NULL;
  }
  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, FD_ARBFILTER_ALIGN ) ) ) {
    FD_LOG_WARNING(( "misaligned shmem" ));
    return NULL;
  }

  memset( shmem, 0, fd_arbfilter_footprint() );

  fd_arbfilter_shmem_t * f = (fd_arbfilter_shmem_t *)shmem;
  f->magic     = FD_ARBFILTER_MAGIC;
  f->version   = 0UL;
  f->prog_mask = FD_ARBFILTER_PROG_SLOT_MAX-1UL;
  f->prog_cnt  = 0UL;
  f->acct_mask = FD_ARBFILTER_ACCT_SLOT_MAX-1UL;
  f->acct_cnt  = 0UL;

  return shmem;
}

fd_arbfilter_shmem_t *
fd_arbfilter_join( void * shmem ) {
  if( FD_UNLIKELY( !shmem ) ) {
    FD_LOG_WARNING(( "NULL shmem" ));
    return NULL;
  }
  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, FD_ARBFILTER_ALIGN ) ) ) {
    FD_LOG_WARNING(( "misaligned shmem" ));
    return NULL;
  }

  fd_arbfilter_shmem_t * f = (fd_arbfilter_shmem_t *)shmem;
  if( FD_UNLIKELY( f->magic!=FD_ARBFILTER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return NULL;
  }

  return f;
}

int
fd_arbfilter_prog_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*ids)[ 32UL ],
                       uchar const *          tags ) {
  if( FD_UNLIKELY( cnt>FD_ARBFILTER_PROG_ENTRY_MAX ) ) return -1;

  ulong ver  = arbfilter_ver_begin( f );
  ulong mask = f->prog_mask;

  /* 先把 prog_cnt 置 0 再填表：即使读方读到替换中的内容，也不会看到超出
     prog_cnt 的条目。 */
  f->prog_cnt = 0UL;
  memset( f->prog, 0, sizeof(f->prog) );

  ulong used = 0UL;
  for( ulong i=0UL; i<cnt; i++ )
    used += (ulong)arbfilter_tbl_insert( f->prog, mask, ids[ i ], tags ? tags[ i ] : 0U );

  f->prog_cnt = used;
  arbfilter_ver_end( f, ver );
  return 0;
}

int
fd_arbfilter_acct_set( fd_arbfilter_shmem_t * f,
                       ulong                  cnt,
                       uchar const            (*keys)[ 32UL ],
                       uchar const *          tags ) {
  if( FD_UNLIKELY( cnt>FD_ARBFILTER_ACCT_ENTRY_MAX ) ) return -1;

  ulong ver  = arbfilter_ver_begin( f );
  ulong mask = f->acct_mask;

  f->acct_cnt = 0UL;
  memset( f->acct, 0, sizeof(f->acct) );

  ulong used = 0UL;
  for( ulong i=0UL; i<cnt; i++ )
    used += (ulong)arbfilter_tbl_insert( f->acct, mask, keys[ i ], tags ? tags[ i ] : 0U );

  f->acct_cnt = used;
  arbfilter_ver_end( f, ver );
  return 0;
}

ulong
fd_arbfilter_prog_idx( fd_arbfilter_shmem_t const * f,
                       uchar const *                key ) {
  ulong mask = f->prog_mask; /* 格式化后不再改变 */

  for( ulong retry=0UL; retry<FD_ARBFILTER_SEQ_RETRY; retry++ ) {
    ulong ver = arbfilter_ver_load( f );
    if( FD_UNLIKELY( ver & 1UL ) ) { /* 写方正在替换 */
      FD_SPIN_PAUSE();
      continue;
    }

    ulong res = arbfilter_tbl_find( f->prog, mask, key, NULL );

    if( FD_LIKELY( arbfilter_ver_load( f )==ver ) ) return res;
  }
  return ULONG_MAX;
}

ulong
fd_arbfilter_acct_idx( fd_arbfilter_shmem_t const * f,
                       uchar const *                key ) {
  ulong mask = f->acct_mask; /* 格式化后不再改变 */

  for( ulong retry=0UL; retry<FD_ARBFILTER_SEQ_RETRY; retry++ ) {
    ulong ver = arbfilter_ver_load( f );
    if( FD_UNLIKELY( ver & 1UL ) ) { /* 写方正在替换 */
      FD_SPIN_PAUSE();
      continue;
    }

    ulong res = arbfilter_tbl_find( f->acct, mask, key, NULL );

    if( FD_LIKELY( arbfilter_ver_load( f )==ver ) ) return res;
  }
  return ULONG_MAX;
}
