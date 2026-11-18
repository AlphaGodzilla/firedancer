#include "fd_accdb_ro_ffi.h"

#include "fd_accdb.h"
#include "fd_accdb_shmem.h"
#include "fd_accdb_private.h" /* fd_accdb_shmem_t::max_live_slots */
#include "../../tango/fseq/fd_fseq.h"
#include "../../util/bits/fd_bits.h"

#include <stdlib.h>

/* 上下文只持有两样东西：只读 join 状态，以及那块 join 状态的分配基址（释放时要传回
   原始指针，而 join 需要对齐后的地址）。账户库共享内存映射、epoch 槽与 accounts.db 的
   文件描述符都由调用方持有。 */

struct fd_accdb_ro_ffi {
  fd_accdb_t * accdb;
  void *       join_alloc;
};

fd_accdb_ro_ffi_t *
fd_accdb_ro_ffi_join( void * accdb_shmem_obj,
                      void * epoch_fseq_obj,
                      int    fd_ro ) {
  if( FD_UNLIKELY( !accdb_shmem_obj || !epoch_fseq_obj || fd_ro<0 ) ) return NULL;

  fd_accdb_shmem_t * shmem = fd_accdb_shmem_join( accdb_shmem_obj );
  if( FD_UNLIKELY( !shmem ) ) return NULL;

  ulong * epoch_slot = fd_fseq_join( epoch_fseq_obj );
  if( FD_UNLIKELY( !epoch_slot ) ) return NULL;

  ulong  join_align = fd_accdb_align();
  ulong  join_sz    = fd_accdb_footprint( shmem->max_live_slots, 0 );
  void * join_alloc = malloc( join_sz+join_align );
  if( FD_UNLIKELY( !join_alloc ) ) return NULL;
  void * join_laddr = (void *)fd_ulong_align_up( (ulong)join_alloc, join_align );

  fd_accdb_t * accdb = fd_accdb_join_readonly( join_laddr, shmem, epoch_slot, fd_ro );
  if( FD_UNLIKELY( !accdb ) ) { free( join_alloc ); return NULL; }

  fd_accdb_ro_ffi_t * ctx = malloc( sizeof( fd_accdb_ro_ffi_t ) );
  if( FD_UNLIKELY( !ctx ) ) { free( join_alloc ); return NULL; }

  ctx->accdb      = accdb;
  ctx->join_alloc = join_alloc;
  return ctx;
}

int
fd_accdb_ro_ffi_read( fd_accdb_ro_ffi_t * ctx,
                      ushort              fork_id,
                      uchar const *       pubkey,
                      ulong *             out_lamports,
                      int *               out_executable,
                      uchar *             out_owner,
                      uchar *             out_data,
                      ulong *             out_data_len ) {
  if( FD_UNLIKELY( !ctx || !pubkey || !out_lamports || !out_executable ||
                   !out_owner || !out_data || !out_data_len ) ) return -1;

  fd_accdb_fork_id_t fork = { .val = fork_id };
  return fd_accdb_read_one_nocache( ctx->accdb, fork, pubkey,
                                    out_lamports, out_executable, out_owner,
                                    out_data, out_data_len );
}

uint
fd_accdb_ro_ffi_fork_gen( fd_accdb_ro_ffi_t * ctx,
                          ushort              fork_id ) {
  if( FD_UNLIKELY( !ctx ) ) return 0U;
  fd_accdb_fork_id_t fork = { .val = fork_id };
  return fd_accdb_fork_gen( ctx->accdb, fork );
}

void
fd_accdb_ro_ffi_leave( fd_accdb_ro_ffi_t * ctx ) {
  if( FD_UNLIKELY( !ctx ) ) return;
  free( ctx->join_alloc );
  free( ctx );
}
