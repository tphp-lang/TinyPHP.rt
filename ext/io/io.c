/*
 * TinyPHP.rt - io.c - M4-b 透明文件 I/O（uv_fs 线程池）
 *
 * 模型：每个 uv_fs_* 操作包成一次「发起 → SchedEvent 挂起 → 回调 fire →
 * 取 req.result」。FsOp 放在等待协程的栈上——挂起期间栈冻结，uv 回调跨
 * fiber 只写 ev/结果，安全。scandir 的目录项必须在 uv_fs_req_cleanup 前
 * 拷贝，因此在回调里完成。
 *
 * TStream：fopen 返回的内部类对象（同 Async\Coroutine 模式），__handle
 * 属性存流上下文下标。文件指针用逻辑偏移 cur_off + 显式 offset 读写
 * （libuv 无 lseek），缓冲仅用于 fgets。
 *
 * 错误语义对齐 PHP：失败返回 false 并打 "PHP Warning: ..."（stderr）；
 * 参数类型错 raise TypeError；流上下文损坏 raise Error。
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "tsched.h"
#include "tio.h"
#include "pack.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sched.c / vm.c */
TStr *tstr_new(const char *s, size_t len);
void tphp_vm_raise(VM *vm, const char *cls, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

#define IO_RBUF 8192

static _Thread_local TClass *g_stream_cls;   /* TStream 内部类 */

/* ---- 流上下文注册表（文件流 + TCP 流共用；net.c 经 tio.h 的
 * tphp_stream_obj_new 创建 TCP 流）——M5: per-OS-thread ---- */

static _Thread_local TStreamCtx **g_streams;
static _Thread_local int g_n_streams, g_cap_streams;

static int stream_ctx_new(int fd, int flags) {
    if (g_n_streams >= g_cap_streams) {
        g_cap_streams = g_cap_streams ? g_cap_streams * 2 : 16;
        g_streams = tm_realloc(g_streams,
                               sizeof(TStreamCtx *) * (size_t)g_cap_streams);
    }
    TStreamCtx *s = tm_malloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->fd = fd;
    s->open_flags = flags;
    g_streams[g_n_streams] = s;
    return g_n_streams++;
}

/* net.c：注册一个 TCP 流上下文并包装成 TStream 对象（rc=1 给调用方） */
TObj *tphp_stream_obj_new(TStreamCtx *s) {
    if (g_n_streams >= g_cap_streams) {
        g_cap_streams = g_cap_streams ? g_cap_streams * 2 : 16;
        g_streams = tm_realloc(g_streams,
                               sizeof(TStreamCtx *) * (size_t)g_cap_streams);
    }
    g_streams[g_n_streams++] = s;
    TObj *o = tphp_obj_alloc(g_stream_cls);
    o->props[0] = TV_INT(g_n_streams - 1);
    return o;
}

static TStreamCtx *stream_of(VM *vm, TVal v, const char *who) {
    return tphp_stream_from_obj(vm, v, who);
}

/* net.c：TVal → TStreamCtx（校验 TStream 类型） */
TStreamCtx *tphp_stream_from_obj(VM *vm, TVal v, const char *who) {
    if (v.t != T_OBJ)
        tphp_vm_raise(vm, "TypeError", "%s() expects a TStream object", who);
    TObj *o = (TObj *)v.u.p;
    if (o->cls != g_stream_cls || o->props[0].t != T_INT)
        tphp_vm_raise(vm, "TypeError", "%s() expects a TStream object", who);
    TStreamCtx *s = g_streams[o->props[0].u.i];
    if (!s)
        tphp_vm_raise(vm, "Error", "%s(): stream context vanished", who);
    return s;
}

/* closed 流：PHP8 行为是 warning + false（不致命） */
static bool stream_live(VM *vm, TStreamCtx *s, const char *who) {
    (void)vm;
    if (s->closed) {
        fprintf(stderr, "PHP Warning: %s(): supplied resource is not a "
                        "valid stream resource\n", who);
        return false;
    }
    return true;
}

/* ---- 警告 ---- */

static void io_warn(const char *what, const char *detail) {
    fprintf(stderr, "PHP Warning: %s: %s\n", what, detail);
}

/* ---- bundle 虚拟资产路径（ASSET_ROOT = "tphp://asset"）----
 * 数据资产在内存 zip 里，不落盘；仅 DLL 类落盘（见 pack.c）。 */

#define ASSET_PREFIX "tphp://asset/"
#define ASSET_ROOT_S "tphp://asset"

/* 虚拟路径 → zip 内 rel（"" = 根）；非虚拟路径返回 NULL */
static const char *asset_rel(const char *path) {
    size_t n = strlen(ASSET_PREFIX);
    if (strncmp(path, ASSET_PREFIX, n) == 0) return path + n;
    if (strcmp(path, ASSET_ROOT_S) == 0) return "";
    return NULL;
}

/* T13: 磁盘未命中回落 —— path 在打包时记录的 source_root 之下时，剥离前缀
 * 得到 zip 内 key。需要静态缓冲（source_root 可能比 path 长），返回是否命中。 */
static bool src_root_rel(const char *path, char *out, size_t cap) {
    const char *root = tphp_pack_source_root();
    if (!root || !root[0]) return false;
    size_t rn = strlen(root);
    /* 去掉尾部斜杠便于前缀比对 */
    while (rn > 0 && (root[rn - 1] == '/' || root[rn - 1] == '\\')) rn--;
    if (strncmp(path, root, rn) != 0) return false;
    if (path[rn] != '/' && path[rn] != '\\') return false;
    const char *rel = path + rn;
    while (*rel == '/' || *rel == '\\') rel++;
    if (!rel[0]) return false;
    snprintf(out, cap, "%s", rel);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    return true;
}

static const char *asset_err(void) {
    return tphp_pack_has_bundle() ? "no such file in bundle"
                                  : "not a packed executable";
}

/* ---- 一次 uv_fs 操作 ---- */

typedef struct FsOp {
    uv_fs_t req;           /* req.data = FsOp* */
    SchedEvent ev;
    ssize_t result;        /* req.result 快照（cleanup 后仍可读） */
    int64_t stat_size;     /* stat/fstat 的 st_size 快照（cleanup 前取） */
    int stat_mode;         /* stat 的 st_mode 快照（is_file/is_dir 用） */
    bool want_names;       /* scandir：回调里拷贝目录项 */
    char **names;
    int n_names;
} FsOp;

static void fs_cb(uv_fs_t *req) {
    FsOp *op = (FsOp *)req->data;
    op->result = req->result;
    /* statbuf 在 uv_fs_req_cleanup 后失效：st_size/st_mode 先快照 */
    op->stat_size = req->result >= 0 ? (int64_t)req->statbuf.st_size : 0;
    op->stat_mode = req->result >= 0 ? (int)req->statbuf.st_mode : 0;
    if (op->want_names) {
        uv_dirent_t ent;
        while (uv_fs_scandir_next(req, &ent) == 0) {
            size_t n = strlen(ent.name);
            char *cp = tm_malloc(n + 1);
            memcpy(cp, ent.name, n + 1);
            op->names = tm_realloc(op->names,
                                   sizeof(char *) * (size_t)(op->n_names + 1));
            op->names[op->n_names++] = cp;
        }
        /* libuv 跨平台都不含 "." ".."；PHP scandir 含 → 统一手动前置 */
        const char *dots[2] = { ".", ".." };
        int base = op->n_names;
        op->names = tm_realloc(op->names,
                               sizeof(char *) * (size_t)(op->n_names + 2));
        op->n_names += 2;
        for (int i = op->n_names - 1; i >= 2; i--)
            op->names[i] = op->names[i - 2];
        op->names[0] = tm_malloc(2); memcpy(op->names[0], dots[0], 2);   /* ".\0" */
        op->names[1] = tm_malloc(3); memcpy(op->names[1], dots[1], 3);   /* "..\0" */
        (void)base;
    }
    uv_fs_req_cleanup(req);
    tphp_sched_event_fire(&op->ev);
}

/* 发起并等待一个 uv_fs_* 调用：FN(loop, req, ..., cb) 形状统一（req 第
 * 二参、cb 最后），__VA_ARGS__ 填中间参数。无调度器时退化为同步（cb=NULL），
 * 目录项/stat 快照同步路径同样处理。 */
#define FS_DO(op, FN, ...)                                           \
    do {                                                             \
        (op)->result = -1;                                           \
        (op)->stat_size = 0;                                         \
        (op)->stat_mode = 0;                                         \
        (op)->req.data = (op);                                       \
        if (tphp_sched_active()) {                                   \
            tphp_sched_event_reset(&(op)->ev);                       \
            FN(tphp_sched_loop(), &(op)->req, __VA_ARGS__, fs_cb);   \
            tphp_sched_event_wait(&(op)->ev);                        \
        } else {                                                     \
            FN(tphp_sched_loop(), &(op)->req, __VA_ARGS__, NULL);    \
            (op)->result = (op)->req.result;                         \
            (op)->stat_size = (op)->result >= 0                      \
                ? (int64_t)(op)->req.statbuf.st_size : 0;            \
            (op)->stat_mode = (op)->result >= 0                      \
                ? (int)(op)->req.statbuf.st_mode : 0;                \
            if ((op)->want_names) {                                  \
                uv_dirent_t ent;                                     \
                while (uv_fs_scandir_next(&(op)->req, &ent) == 0) {  \
                    size_t nn = strlen(ent.name);                    \
                    char *cp = tm_malloc(nn + 1);                    \
                    memcpy(cp, ent.name, nn + 1);                    \
                    (op)->names = tm_realloc((op)->names,            \
                        sizeof(char *) * (size_t)((op)->n_names + 1));\
                    (op)->names[(op)->n_names++] = cp;               \
                }                                                    \
                const char *dots[2] = { ".", ".." };                 \
                (op)->names = tm_realloc((op)->names,                \
                    sizeof(char *) * (size_t)((op)->n_names + 2));   \
                (op)->n_names += 2;                                  \
                for (int i_ = (op)->n_names - 1; i_ >= 2; i_--)      \
                    (op)->names[i_] = (op)->names[i_ - 2];           \
                (op)->names[0] = tm_malloc(2);                       \
                memcpy((op)->names[0], dots[0], 2);                  \
                (op)->names[1] = tm_malloc(3);                       \
                memcpy((op)->names[1], dots[1], 3);                  \
            }                                                        \
            uv_fs_req_cleanup(&(op)->req);                           \
        }                                                            \
    } while (0)

static void fs_op_clear(FsOp *op) {
    for (int i = 0; i < op->n_names; i++) tm_free(op->names[i]);
    tm_free(op->names);
    op->names = NULL;
    op->n_names = 0;
}

/* ---- 参数工具 ---- */

static const char *arg_str(VM *vm, TVal *args, int n, int i,
                           const char *who) {
    if (n <= i || args[i].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "%s() expects argument %d to be a "
                      "string", who, i + 1);
    return tstr_val((TStr *)args[i].u.p);
}

static int64_t arg_int(VM *vm, TVal *args, int n, int i, const char *who) {
    if (n <= i)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "%s() expects at least %d argument(s)", who, i + 1);
    return tval_to_int(args[i]);
}

/* ---- fopen 模式解析（PHP 语义；b/t 忽略） ---- */

static int parse_mode(const char *m, bool *ok) {
    *ok = strlen(m) >= 1;
    if (!*ok) return 0;
    int flags;
    switch (m[0]) {
    case 'r': flags = UV_FS_O_RDONLY; break;
    case 'w': flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC; break;
    case 'a': flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND; break;
    case 'x': flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_EXCL; break;
    default: *ok = false; return 0;
    }
    if (m[1] == '+')
        flags = (flags & ~(UV_FS_O_WRONLY | UV_FS_O_RDONLY)) | UV_FS_O_RDWR;
    return flags;
}

/* ---- 内部读写（协程内挂起） ---- */

/* 读一块：显式 offset（TStream 的文件指针是逻辑偏移 cur_off，fseek 靠
 * 它生效）；返回 0=eof，<0=错误。file_get_contents 的 read_all 传 -1
 * （内核指针顺序推进）。 */
static int64_t fs_read_chunk(FsOp *op, int fd, char *buf, size_t len,
                             int64_t off) {
    uv_buf_t b = uv_buf_init(buf, (unsigned int)len);
    FS_DO(op, uv_fs_read, fd, &b, 1, off);
    return op->result;
}

/* file_get_contents 的读循环：全部内容进 malloc 缓冲 */
static char *read_all(FsOp *op, int fd, size_t *out_len) {
    size_t cap = 64 * 1024, len = 0;
    int64_t off = 0;
    char *buf = tm_malloc(cap);
    for (;;) {
        char chunk[65536];
        int64_t n = fs_read_chunk(op, fd, chunk, sizeof chunk, off);
        if (n < 0) { tm_free(buf); *out_len = 0; return NULL; }
        if (n == 0) break;
        off += n;
        if (len + (size_t)n > cap) {
            while (len + (size_t)n > cap) cap *= 2;
            buf = tm_realloc(buf, cap);
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    *out_len = len;
    return buf;
}

/* ---- 内置实现 ---- */

TVal tphp_io_file_get_contents(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "file_get_contents");
    char what[256];
    snprintf(what, sizeof what, "file_get_contents(%s)", path);
    const char *rel = asset_rel(path);
    if (rel) {
        void *data = NULL;
        size_t len = 0;
        if (!tphp_pack_read(rel, &data, &len)) {
            io_warn(what, asset_err());
            return TV_FALSE;
        }
        TStr *s = tstr_new(data, len);
        free(data);
        return TV_STR(s);
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_open, path, UV_FS_O_RDONLY, 0);
    if (op.result < 0) {
        /* T13: 磁盘未命中 → 若在打包 source_root 下则回落 zip */
        char rel[1024];
        if (src_root_rel(path, rel, sizeof rel)) {
            void *data = NULL;
            size_t len = 0;
            if (tphp_pack_read(rel, &data, &len)) {
                TStr *s = tstr_new(data, len);
                free(data);
                return TV_STR(s);
            }
        }
        io_warn(what, uv_strerror((int)op.result));
        return TV_FALSE;
    }
    int fd = (int)op.result;
    size_t len = 0;
    char *data = read_all(&op, fd, &len);
    FS_DO(&op, uv_fs_close, fd);
    if (!data) {
        io_warn(what, "read failed");
        return TV_FALSE;
    }
    TStr *s = tstr_new(data, len);
    tm_free(data);
    return TV_STR(s);
}

TVal tphp_io_file_put_contents(VM *vm, TVal *args, int n) {
    const char *path = arg_str(vm, args, n, 0, "file_put_contents");
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "file_put_contents() expects at least 2 arguments");
    if (asset_rel(path)) {
        io_warn("file_put_contents()", "bundle assets are read-only");
        return TV_FALSE;
    }
    bool append = false;
    if (n >= 3 && args[2].t == T_INT && (args[2].u.i & 8)) append = true;
    char what[256];
    snprintf(what, sizeof what, "file_put_contents(%s)", path);

    TVal data = n >= 2 ? args[1] : TV_NULL;
    TStr *s;
    if (data.t == T_STR) {
        s = (TStr *)data.u.p;
        tval_addref(data);
    } else {
        s = tval_to_str(data);          /* 按值转换；返回值已 +1 */
    }
    int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT |
                (append ? UV_FS_O_APPEND : UV_FS_O_TRUNC);
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_open, path, flags, 0644);
    if (op.result < 0) {
        io_warn(what, uv_strerror((int)op.result));
        tval_release(&(TVal){ .u.p = s, .t = T_STR });
        return TV_FALSE;
    }
    int fd = (int)op.result;
    size_t total = tstr_len(s), done = 0;
    while (done < total) {
        uv_buf_t b = uv_buf_init((char *)tstr_val(s) + done,
                                 (unsigned int)(total - done));
        FS_DO(&op, uv_fs_write, fd, &b, 1, -1);
        if (op.result < 0) {
            io_warn(what, uv_strerror((int)op.result));
            done = 0;
            break;
        }
        done += (size_t)op.result;
    }
    FS_DO(&op, uv_fs_close, fd);
    tval_release(&(TVal){ .u.p = s, .t = T_STR });
    if (done == 0 && total > 0) return TV_FALSE;
    return TV_INT((int64_t)done);
}

TVal tphp_io_fopen(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "fopen");
    const char *mode = arg_str(vm, args, n, 1, "fopen");
    bool ok;
    int flags = parse_mode(mode, &ok);
    if (!ok) {
        tphp_vm_raise(vm, "ValueError", "fopen(): Invalid mode \"%s\"", mode);
    }
    const char *rel = asset_rel(path);
    if (rel) {
        if (flags & (UV_FS_O_WRONLY | UV_FS_O_RDWR | UV_FS_O_CREAT |
                     UV_FS_O_APPEND | UV_FS_O_TRUNC)) {
            io_warn("fopen()", "bundle assets are read-only");
            return TV_FALSE;
        }
        void *data = NULL;
        size_t len = 0;
        if (!tphp_pack_read(rel, &data, &len)) {
            fprintf(stderr, "PHP Warning: fopen(%s): failed to open stream: %s\n",
                    path, asset_err());
            return TV_FALSE;
        }
        int idx = stream_ctx_new(-1, flags);
        g_streams[idx]->mem = data;
        g_streams[idx]->mem_len = len;
        TObj *o = tphp_obj_alloc(g_stream_cls);
        o->props[0] = TV_INT(idx);
        return TV_OBJ(o);
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_open, path, flags, 0644);
    if (op.result < 0) {
        /* T13: 磁盘未命中 → 若在打包 source_root 下且为读模式则回落 zip */
        char srel[1024];
        if (!(flags & (UV_FS_O_WRONLY | UV_FS_O_RDWR | UV_FS_O_CREAT |
                       UV_FS_O_APPEND | UV_FS_O_TRUNC)) &&
            src_root_rel(path, srel, sizeof srel)) {
            void *data = NULL;
            size_t len = 0;
            if (tphp_pack_read(srel, &data, &len)) {
                int bidx = stream_ctx_new(-1, flags);
                g_streams[bidx]->mem = data;
                g_streams[bidx]->mem_len = len;
                TObj *bo = tphp_obj_alloc(g_stream_cls);
                bo->props[0] = TV_INT(bidx);
                return TV_OBJ(bo);
            }
        }
        char what[256];
        snprintf(what, sizeof what, "fopen(%s): failed to open stream", path);
        io_warn(what, uv_strerror((int)op.result));
        return TV_FALSE;
    }
    int idx = stream_ctx_new((int)op.result, flags);
    TObj *o = tphp_obj_alloc(g_stream_cls);
    o->props[0] = TV_INT(idx);
    return TV_OBJ(o);
}

TVal tphp_io_fread(VM *vm, TVal *args, int n) {
    const char *who = "fread";
    TStreamCtx *s = stream_of(vm, args[0], who);
    int64_t len = arg_int(vm, args, n, 1, who);
    if (len < 0)
        tphp_vm_raise(vm, "ValueError", "fread(): length must be >= 0");
    if (!stream_live(vm, s, who)) return TV_FALSE;
    if (s->is_tcp) return tcp_fread_impl(vm, s, len);
    if (s->is_server)
        tphp_vm_raise(vm, "Error", "fread(): cannot read from a server socket");
    if (len == 0) return TV_STR(tstr_new("", 0));
    char *buf = tm_malloc((size_t)len);
    size_t got = 0;
    /* 先出 fgets 缓冲 */
    int avail = s->rbuf_len - s->rbuf_pos;
    if (avail > 0) {
        size_t take = (size_t)avail < (size_t)len ? (size_t)avail : (size_t)len;
        memcpy(buf, s->rbuf + s->rbuf_pos, take);
        s->rbuf_pos += (int)take;
        got += take;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    while (got < (size_t)len && !s->eof) {
        char chunk[65536];
        size_t want = sizeof chunk < (size_t)len - got ? sizeof chunk
                                                       : (size_t)len - got;
        int64_t rd;
        if (s->mem) {                 /* 内存流：直读 bundle 缓冲 */
            if (s->cur_off >= (int64_t)s->mem_len) {
                rd = 0;
            } else {
                size_t left = s->mem_len - (size_t)s->cur_off;
                size_t cp = want < left ? want : left;
                memcpy(chunk, s->mem + s->cur_off, cp);
                rd = (int64_t)cp;
            }
        } else {
            rd = fs_read_chunk(&op, s->fd, chunk, want, s->cur_off);
        }
        if (rd < 0) {
            tm_free(buf);
            io_warn("fread", uv_strerror((int)rd));
            return TV_FALSE;
        }
        if (rd == 0) { s->eof = true; break; }
        s->cur_off += rd;
        memcpy(buf + got, chunk, (size_t)rd);
        got += (size_t)rd;
    }
    if (got == 0 && (size_t)len > 0) {
        tm_free(buf);
        return TV_STR(tstr_new("", 0));   /* PHP: fread at EOF → "" */
    }
    TStr *r = tstr_new(buf, got);
    tm_free(buf);
    return TV_STR(r);
}

TVal tphp_io_fwrite(VM *vm, TVal *args, int n) {
    const char *who = "fwrite";
    TStreamCtx *s = stream_of(vm, args[0], who);
    if (n < 2)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "fwrite() expects at least 2 arguments");
    if (args[1].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "fwrite() expects a string");
    TStr *data = (TStr *)args[1].u.p;
    if (!stream_live(vm, s, who)) return TV_FALSE;
    if (s->is_tcp) return tcp_fwrite_impl(vm, s, data);
    if (s->is_server)
        tphp_vm_raise(vm, "Error", "fwrite(): cannot write to a server socket");
    if (s->mem) {
        io_warn("fwrite()", "bundle asset stream is read-only");
        return TV_FALSE;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    size_t total = tstr_len(data), done = 0;
    while (done < total) {
        uv_buf_t b = uv_buf_init((char *)tstr_val(data) + done,
                                 (unsigned int)(total - done));
        /* 追加流 offset=-1（O_APPEND 内核保证末尾）；其余显式 cur_off */
        int64_t woff = (s->open_flags & UV_FS_O_APPEND) ? -1 : s->cur_off;
        FS_DO(&op, uv_fs_write, s->fd, &b, 1, woff);
        if (op.result < 0) {
            io_warn("fwrite", uv_strerror((int)op.result));
            return done ? TV_INT((int64_t)done) : TV_FALSE;
        }
        if (op.result == 0) break;
        s->cur_off += op.result;
        done += (size_t)op.result;
    }
    return TV_INT((int64_t)done);
}

TVal tphp_io_fgets(VM *vm, TVal *args, int n) {
    (void)n;
    const char *who = "fgets";
    TStreamCtx *s = stream_of(vm, args[0], who);
    if (!stream_live(vm, s, who)) return TV_FALSE;
    if (s->is_tcp) return tcp_fgets_impl(vm, s);
    size_t cap = 128, len = 0;
    char *out = tm_malloc(cap);
    for (;;) {
        /* 缓冲内找 '\n' */
        while (s->rbuf_pos < s->rbuf_len) {
            char c = s->rbuf[s->rbuf_pos++];
            if (len + 1 > cap) { cap *= 2; out = tm_realloc(out, cap); }
            out[len++] = c;
            if (c == '\n') {
                TStr *r = tstr_new(out, len);
                tm_free(out);
                return TV_STR(r);
            }
        }
        if (s->eof) break;
        /* 补缓冲：内存流从 bundle 缓冲切块 */
        if (s->mem) {
            if (s->cur_off >= (int64_t)s->mem_len) { s->eof = true; break; }
            size_t chunk = s->mem_len - (size_t)s->cur_off;
            if (chunk > IO_RBUF) chunk = IO_RBUF;
            memcpy(s->rbuf, s->mem + s->cur_off, chunk);
            s->rbuf_len = (int)chunk;
            s->rbuf_pos = 0;
            s->cur_off += (int64_t)chunk;
            continue;
        }
        /* 补缓冲 */
        FsOp op;
        memset(&op, 0, sizeof op);
        char chunk[IO_RBUF];
        int64_t rd = fs_read_chunk(&op, s->fd, chunk, sizeof chunk,
                                   s->cur_off);
        if (rd < 0) {
            tm_free(out);
            io_warn("fgets", uv_strerror((int)rd));
            return TV_FALSE;
        }
        if (rd == 0) { s->eof = true; break; }
        s->cur_off += rd;
        memcpy(s->rbuf, chunk, (size_t)rd);
        s->rbuf_len = (int)rd;
        s->rbuf_pos = 0;
    }
    if (len == 0) {
        tm_free(out);
        return TV_FALSE;             /* EOF：PHP fgets 返回 false */
    }
    TStr *r = tstr_new(out, len);
    tm_free(out);
    return TV_STR(r);
}

TVal tphp_io_fclose(VM *vm, TVal *args, int n) {
    (void)n;
    const char *who = "fclose";
    TStreamCtx *s = stream_of(vm, args[0], who);
    if (s->closed) return TV_TRUE;   /* PHP 幂等容忍 */
    if (s->is_tcp) {
        tcp_close_impl(vm, s);       /* 异步 close + 挂起等待 */
        return TV_TRUE;
    }
    if (s->mem) {                    /* 内存流：释放 bundle 缓冲，无 fd 可关 */
        free(s->mem);
        s->mem = NULL;
        s->closed = true;
        return TV_TRUE;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_close, s->fd);
    s->closed = true;
    return op.result < 0 ? TV_FALSE : TV_TRUE;
}

TVal tphp_io_feof(VM *vm, TVal *args, int n) {
    (void)n;
    const char *who = "feof";
    TStreamCtx *s = stream_of(vm, args[0], who);
    if (!stream_live(vm, s, who)) return TV_TRUE;
    if (s->is_tcp) return tcp_feof_impl(s);
    return (s->eof && s->rbuf_pos >= s->rbuf_len) ? TV_TRUE : TV_FALSE;
}

TVal tphp_io_fseek(VM *vm, TVal *args, int n) {
    const char *who = "fseek";
    TStreamCtx *s = stream_of(vm, args[0], who);
    int64_t off = arg_int(vm, args, n, 1, who);
    int whence = n >= 3 ? (int)tval_to_int(args[2]) : 0;
    if (!stream_live(vm, s, who)) return TV_FALSE;
    if (s->is_tcp || s->is_server)
        tphp_vm_raise(vm, "Error", "fseek(): sockets are not seekable");
    if (whence == 1) {
        s->cur_off += off;
    } else if (whence == 2) {
        if (s->mem) {                 /* 内存流：SEEK_END 用 mem_len */
            s->cur_off = (int64_t)s->mem_len + off;
        } else {
            /* SEEK_END：fstat 拿大小（FS_DO 已快照） */
            FsOp op;
            memset(&op, 0, sizeof op);
            FS_DO(&op, uv_fs_fstat, s->fd);
            if (op.result < 0) return TV_FALSE;
            s->cur_off = op.stat_size + off;
        }
    } else {
        s->cur_off = off;
    }
    s->rbuf_len = s->rbuf_pos = 0;
    s->eof = false;
    return TV_INT(0);                /* PHP: 0 成功 / -1 失败 */
}

TVal tphp_io_file_exists(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "file_exists");
    const char *rel = asset_rel(path);
    if (rel) return tphp_pack_stat(rel, NULL) ? TV_TRUE : TV_FALSE;
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_stat, path);
    if (op.result == 0) return TV_TRUE;
    /* T13: 磁盘未命中 → 若在打包 source_root 下则回落 zip */
    char srel[1024];
    if (src_root_rel(path, srel, sizeof srel))
        return tphp_pack_stat(srel, NULL) ? TV_TRUE : TV_FALSE;
    return TV_FALSE;
}

/* is_file：存在且是普通文件（PHP：不发警告；打包 zip 内条目即文件） */
TVal tphp_io_is_file(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "is_file");
    const char *rel = asset_rel(path);
    if (rel) return tphp_pack_stat(rel, NULL) ? TV_TRUE : TV_FALSE;
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_stat, path);
    if (op.result != 0) {
        char srel[1024];
        if (src_root_rel(path, srel, sizeof srel))
            return tphp_pack_stat(srel, NULL) ? TV_TRUE : TV_FALSE;
        return TV_FALSE;
    }
    return S_ISREG(op.stat_mode) ? TV_TRUE : TV_FALSE;
}

/* is_dir：存在且是目录（zip 资源包无目录条目 → 资产路径恒 false） */
TVal tphp_io_is_dir(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "is_dir");
    if (asset_rel(path)) return TV_FALSE;
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_stat, path);
    if (op.result != 0) return TV_FALSE;
    return S_ISDIR(op.stat_mode) ? TV_TRUE : TV_FALSE;
}

TVal tphp_io_filesize(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "filesize");
    const char *rel = asset_rel(path);
    if (rel) {
        size_t sz = 0;
        if (!tphp_pack_stat(rel, &sz)) {
            char what[256];
            snprintf(what, sizeof what, "filesize(): stat failed for %s", path);
            io_warn(what, asset_err());
            return TV_FALSE;
        }
        return TV_INT((int64_t)sz);
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_stat, path);
    if (op.result < 0) {
        /* T13: 磁盘未命中 → 若在打包 source_root 下则回落 zip */
        char srel[1024];
        size_t sz = 0;
        if (src_root_rel(path, srel, sizeof srel) &&
            tphp_pack_stat(srel, &sz))
            return TV_INT((int64_t)sz);
        char what[256];
        snprintf(what, sizeof what, "filesize(): stat failed for %s", path);
        io_warn(what, uv_strerror((int)op.result));
        return TV_FALSE;
    }
    int64_t sz = op.stat_size;       /* FS_DO 已在 cleanup 前快照 */
    return TV_INT(sz);
}

TVal tphp_io_unlink(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "unlink");
    if (asset_rel(path)) {
        io_warn("unlink()", "bundle assets are read-only");
        return TV_FALSE;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_unlink, path);
    if (op.result < 0) {
        io_warn(path, uv_strerror((int)op.result));
        return TV_FALSE;
    }
    return TV_TRUE;
}

TVal tphp_io_rename(VM *vm, TVal *args, int n) {
    (void)n;
    const char *from = arg_str(vm, args, n, 0, "rename");
    const char *to = arg_str(vm, args, n, 1, "rename");
    if (asset_rel(from) || asset_rel(to)) {
        io_warn("rename()", "bundle assets are read-only");
        return TV_FALSE;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_rename, from, to);
    if (op.result < 0) {
        io_warn(from, uv_strerror((int)op.result));
        return TV_FALSE;
    }
    return TV_TRUE;
}

TVal tphp_io_mkdir(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "mkdir");
    if (asset_rel(path)) {
        io_warn("mkdir()", "bundle assets are read-only");
        return TV_FALSE;
    }
    int mode = n >= 2 && args[1].t == T_INT ? (int)args[1].u.i : 0777;
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_mkdir, path, mode);
    if (op.result < 0) {
        io_warn("mkdir()", uv_strerror((int)op.result));
        return TV_FALSE;
    }
    return TV_TRUE;
}

TVal tphp_io_rmdir(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "rmdir");
    if (asset_rel(path)) {
        io_warn("rmdir()", "bundle assets are read-only");
        return TV_FALSE;
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    FS_DO(&op, uv_fs_rmdir, path);
    if (op.result < 0) {
        io_warn("rmdir()", uv_strerror((int)op.result));
        return TV_FALSE;
    }
    return TV_TRUE;
}

TVal tphp_io_scandir(VM *vm, TVal *args, int n) {
    (void)n;
    const char *path = arg_str(vm, args, n, 0, "scandir");
    const char *rel = asset_rel(path);
    if (rel) {
        char what[256];
        snprintf(what, sizeof what, "scandir(%s)", path);
        if (!tphp_pack_has_bundle()) {
            io_warn(what, "failed to open dir: not a packed executable");
            return TV_FALSE;
        }
        char **names = NULL;
        int cnt = tphp_pack_list(rel, &names);
        if (cnt == 0 && rel[0]) {     /* 根恒存在；子目录需有条目 */
            io_warn(what, "failed to open dir: no such dir in bundle");
            return TV_FALSE;
        }
        TArr *arr = tarr_new();
        tarr_append(arr, TV_STR(tstr_new(".", 1)));
        tarr_append(arr, TV_STR(tstr_new("..", 2)));
        for (int i = 0; i < cnt; i++) {
            tarr_append(arr, TV_STR(tstr_new(names[i], strlen(names[i]))));
            free(names[i]);
        }
        free(names);
        return TV_ARR(arr);
    }
    FsOp op;
    memset(&op, 0, sizeof op);
    op.want_names = true;
    FS_DO(&op, uv_fs_scandir, path, 0);
    if (op.result < 0) {
        /* T13: 磁盘未命中 → 若在打包 source_root 下则列 zip 内该目录 */
        char srel[1024];
        if (src_root_rel(path, srel, sizeof srel) && tphp_pack_has_bundle()) {
            char **snames = NULL;
            int scnt = tphp_pack_list(srel, &snames);
            if (scnt > 0 || !srel[0]) {
                TArr *sarr = tarr_new();
                tarr_append(sarr, TV_STR(tstr_new(".", 1)));
                tarr_append(sarr, TV_STR(tstr_new("..", 2)));
                for (int i = 0; i < scnt; i++) {
                    tarr_append(sarr, TV_STR(tstr_new(snames[i], strlen(snames[i]))));
                    free(snames[i]);
                }
                free(snames);
                fs_op_clear(&op);
                return TV_ARR(sarr);
            }
            free(snames);
        }
        char what[256];
        snprintf(what, sizeof what, "scandir(%s): failed to open dir", path);
        io_warn(what, uv_strerror((int)op.result));
        fs_op_clear(&op);
        return TV_FALSE;
    }
    TArr *arr = tarr_new();
    for (int i = 0; i < op.n_names; i++) {
        tarr_append(arr, TV_STR(tstr_new(op.names[i], strlen(op.names[i]))));
    }
    fs_op_clear(&op);
    return TV_ARR(arr);
}

/* ---- TStream 内部类 + 常量 ---- */

void tphp_io_init(void) {
    static _Thread_local bool inited;
    if (inited) return;
    inited = true;
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("TStream", 7);
    cls->kind = 0;                /* CK_CLASS */
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_new("__handle", 8);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    tphp_class_register(cls);
    g_stream_cls = cls;
}
