/*
 * TinyPHP.rt - tio.h - M4-b 透明文件 I/O（uv_fs 线程池）
 *
 * 同步写法透明挂起：协程内发起 uv_fs 异步操作 → 挂起；回调唤醒。
 * 语义对齐 PHP 8（返回值 false/int/string；错误打 PHP Warning 到 stderr）。
 * 注意：文件名带 t 前缀，避免与 CRT 的 <io.h> 撞名（尖括号 include 会搜
 * -I 路径，src/ 里的 io.h 会顶掉系统头）。
 */
#ifndef TPHP_TIO_H
#define TPHP_TIO_H

#include "opcode.h"
#include <uv.h>

/* TStream 内部类注册（幂等；必须在 tphp_compile 之前调用） */
void tphp_io_init(void);

/* ---- 流上下文（io.c 持有文件流，net.c 持有 TCP 流；fread 等按 is_tcp
 * 分派）---- */
typedef struct TcpChunk {
    struct TcpChunk *next;
    char *data;            /* tm_malloc，取走后释放 */
    int len;
    bool eof;              /* 对端关闭（或错误终结队列） */
} TcpChunk;

typedef struct TStreamCtx {
    int fd;                /* 文件流（内存流为 -1） */
    uv_tcp_t *tcp;         /* TCP 流（is_tcp 时；close 时 uv_close+free） */
    bool is_tcp;
    bool is_server;        /* 监听 socket（accept 用，不可读写） */
    int open_flags;
    int64_t cur_off;       /* 逻辑文件偏移（文件流/内存流） */
    char rbuf[8192];       /* fgets 行缓冲（文件流） */
    int rbuf_len, rbuf_pos;
    bool eof;
    bool closed;
    /* 内存流（bundle 资产，只读）：pack_read 的 malloc 缓冲，fclose 释放 */
    char *mem;
    size_t mem_len;
    /* TCP 流：uv_read_start 到达的数据排队，fread/fgets 消费 */
    TcpChunk *rcv_head, *rcv_tail;
    TCont *rcv_waiter;     /* 空队列挂起的读者（单等待者） */
} TStreamCtx;

/* net.c 的 TCP 流实现（io.c 的 fread/fwrite/fgets/fclose/feof 分派调用） */
TVal tcp_fread_impl(VM *vm, TStreamCtx *s, int64_t len);
TVal tcp_fwrite_impl(VM *vm, TStreamCtx *s, TStr *data);
TVal tcp_fgets_impl(VM *vm, TStreamCtx *s);
TVal tcp_feof_impl(TStreamCtx *s);
void tcp_close_impl(VM *vm, TStreamCtx *s);

/* io.c：TVal → TStreamCtx（校验 TStream 类型） */
TStreamCtx *tphp_stream_from_obj(VM *vm, TVal v, const char *who);

TVal tphp_io_file_get_contents(VM *vm, TVal *args, int n);
TVal tphp_io_file_put_contents(VM *vm, TVal *args, int n);
TVal tphp_io_fopen(VM *vm, TVal *args, int n);
TVal tphp_io_fread(VM *vm, TVal *args, int n);
TVal tphp_io_fwrite(VM *vm, TVal *args, int n);
TVal tphp_io_fgets(VM *vm, TVal *args, int n);
TVal tphp_io_fclose(VM *vm, TVal *args, int n);
TVal tphp_io_feof(VM *vm, TVal *args, int n);
TVal tphp_io_fseek(VM *vm, TVal *args, int n);
TVal tphp_io_file_exists(VM *vm, TVal *args, int n);
TVal tphp_io_is_file(VM *vm, TVal *args, int n);
TVal tphp_io_is_dir(VM *vm, TVal *args, int n);
TVal tphp_io_filesize(VM *vm, TVal *args, int n);
TVal tphp_io_unlink(VM *vm, TVal *args, int n);
TVal tphp_io_rename(VM *vm, TVal *args, int n);
TVal tphp_io_mkdir(VM *vm, TVal *args, int n);
TVal tphp_io_rmdir(VM *vm, TVal *args, int n);
TVal tphp_io_scandir(VM *vm, TVal *args, int n);

/* net.c：M4-d DNS + TCP */
TVal tphp_net_gethostbyname(VM *vm, TVal *args, int n);
TVal tphp_net_gethostbynamel(VM *vm, TVal *args, int n);
TVal tphp_net_fsockopen(VM *vm, TVal *args, int n);
TVal tphp_net_server(VM *vm, TVal *args, int n);
TVal tphp_net_accept(VM *vm, TVal *args, int n);
/* net.c：M4-e http_get（http://；https 待 M4-f mbedtls） */
TVal tphp_net_http_get(VM *vm, TVal *args, int n);

#endif /* TPHP_TIO_H */
