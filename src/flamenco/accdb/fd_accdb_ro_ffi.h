#ifndef HEADER_fd_src_flamenco_accdb_fd_accdb_ro_ffi_h
#define HEADER_fd_src_flamenco_accdb_fd_accdb_ro_ffi_h

/* fd_accdb_ro_ffi 是账户库只读 join 的进程外 C ABI：给验证器之外的进程（Rust 侧的
   `fd`）使用，把 `fd_accdb_join_readonly` 与 `fd_accdb_read_one_nocache` 包成
   四个只接受普通参数与指针的函数，调用方无需包含任何 firedancer 头文件。

   为什么需要这一层：只读 join 要用到 `fd_accdb_shmem_t` 的私有字段（`max_live_slots`）
   与 `fd_fseq_join` 的 join 语义，这些细节属于 firedancer 的实现，跨语言重复实现只会
   让两侧布局漂移。代价是本文件必须与 firedancer 用同一套编译参数构建，因此它放在
   firedancer 的源码树里、由它自己的构建系统编译（见 `Local.mk`）。

   生命周期：join 成功返回上下文指针，之后所有读取都通过它；leave 释放上下文与它持有
   的 join 状态，账户库共享内存映射与 `accounts.db` 文件描述符由调用方持有并负责关闭。 */

#include "fd_accdb_base.h"

FD_PROTOTYPES_BEGIN

struct fd_accdb_ro_ffi;
typedef struct fd_accdb_ro_ffi fd_accdb_ro_ffi_t;

/* fd_accdb_ro_ffi_join 建立只读 join。

     accdb_shmem_obj  `accdb` 对象在调用方地址空间中的首地址，必须是只读映射
     epoch_fseq_obj   `accdb_epoch.arb` fseq 对象在调用方地址空间中的首地址，必须可写：
                      只读 joiner 用它把自己读到第几个 epoch 发布给 accdb tile，压缩
                      扫描据此推迟回收分区
     fd_ro            `accounts.db` 的只读文件描述符，必须与写方打开的是同一个文件

   成功返回上下文指针；任一参数非法、对象 magic 不符或内存不足时返回 NULL（原因由
   fd 的日志给出）。 */

fd_accdb_ro_ffi_t *
fd_accdb_ro_ffi_join( void * accdb_shmem_obj,
                      void * epoch_fseq_obj,
                      int    fd_ro );

/* fd_accdb_ro_ffi_read 读一个账户，参数与返回码与 fd_accdb_read_one_nocache 相同：
   返回 FD_ACCDB_READ_ONE_NOCACHE_{MISS,CACHE,DISK}，out_data 必须指向至少
   FD_RUNTIME_ACC_SZ_MAX（10 MiB）字节的缓冲。ctx 为空时返回 -1。 */

int
fd_accdb_ro_ffi_read( fd_accdb_ro_ffi_t * ctx,
                      ushort              fork_id,
                      uchar const *       pubkey,
                      ulong *             out_lamports,
                      int *               out_executable,
                      uchar *             out_owner,
                      uchar *             out_data,
                      ulong *             out_data_len );

/* fd_accdb_ro_ffi_fork_gen 返回 fork_id 当前的 generation（fork 池槽位每次复用都会
   递增）。返回 0 表示该 fork id 不在池内，或 ctx 为空。 */

uint
fd_accdb_ro_ffi_fork_gen( fd_accdb_ro_ffi_t * ctx,
                          ushort              fork_id );

/* fd_accdb_ro_ffi_leave 释放 join 状态与上下文。ctx 可以为 NULL。 */

void
fd_accdb_ro_ffi_leave( fd_accdb_ro_ffi_t * ctx );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_accdb_fd_accdb_ro_ffi_h */
