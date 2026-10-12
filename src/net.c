/*
 * TinyPHP.rt - net.c - M4-d DNS + TCP（真异步，uv loop 直驱非线程池）
 *
 * gethostbyname/gethostbynamel：uv_getaddrinfo（线程池）挂起等待。
 * fsockopen：DNS → uv_tcp_connect → uv_read_start（连接后持续收数据，
 *   到达块入 rcv 队列，fread/fgets 消费；空则挂起）。
 * stream_socket_server + stream_socket_accept：uv_tcp_bind/listen，
 *   连接到达入 accept 队列，accept() 取走包装成 TStream。
 *
 * 读写语义对齐 PHP：fread 一次可用块（≤len）；fgets 按行（跨块拼接）；
 * 对端关闭 → eof；fwrite uv_write 全量写完返回。
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "sched.h"
#include "tio.h"
#include <uv.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cacert.c（脚本生成：Mozilla CA bundle 内嵌） */
extern const unsigned char tphp_cacert_pem[];
extern const size_t tphp_cacert_pem_len;

/* sched.c / io.c */
TStr *tstr_new(const char *s, size_t len);
void tphp_vm_raise(VM *vm, const char *cls, const char *fmt, ...);
TObj *tphp_stream_obj_new(TStreamCtx *s);
TStreamCtx *tphp_stream_from_obj(VM *vm, TVal v, const char *who);

#define TCP_RCV_BUF (64 * 1024)

/* ---- 到达队列 ---- */

static void tcp_enqueue(TStreamCtx *s, char *data, int len, bool eof) {
    TcpChunk *ck = tm_malloc(sizeof *ck);
    ck->next = NULL;
    ck->data = data;
    ck->len = len;
    ck->eof = eof;
    if (s->rcv_tail) s->rcv_tail->next = ck;
    else s->rcv_head = ck;
    s->rcv_tail = ck;
    if (s->rcv_waiter) {
        TCont *w = s->rcv_waiter;
        s->rcv_waiter = NULL;
        tphp_sched_wake(w);
    }
}

/* ---- 读回调（uv loop 直驱） ---- */

static void tcp_alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    (void)h; (void)suggested;
    buf->base = tm_malloc(TCP_RCV_BUF);
    buf->len = TCP_RCV_BUF;
}

static void tcp_read_cb(uv_stream_t *stream, ssize_t nread,
                        const uv_buf_t *buf) {
    TStreamCtx *s = (TStreamCtx *)stream->data;
    if (nread > 0) {
        tcp_enqueue(s, buf->base, (int)nread, false);
        return;                       /* data 归队列所有 */
    }
    tm_free(buf->base);
    if (nread == UV_EOF) {
        uv_read_stop(stream);
        tcp_enqueue(s, NULL, 0, true);
    } else if (nread < 0) {
        uv_read_stop(stream);
        tcp_enqueue(s, NULL, 0, true);    /* 错误按 EOF 处理（简版） */
    }
}

/* ---- DNS：gethostbyname / gethostbynamel ---- */

typedef struct DnsOp {
    uv_getaddrinfo_t req;      /* req.data = DnsOp* */
    SchedEvent ev;
} DnsOp;

static void dns_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    (void)status; (void)res;
    DnsOp *op = (DnsOp *)req->data;
    tphp_sched_event_fire(&op->ev);
}

/* 挂起解析 IPv4 地址；失败 warning + NULL（结果由调用方 uv_freeaddrinfo） */
static struct addrinfo *dns_resolve(VM *vm, const char *host) {
    DnsOp op;
    memset(&op, 0, sizeof op);
    op.req.data = &op;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;        /* 首版只走 IPv4 */
    tphp_sched_event_reset(&op.ev);
    int rc = uv_getaddrinfo(tphp_sched_loop(), &op.req, dns_cb, host, NULL,
                            &hints);
    if (rc != 0) {
        char what[256];
        snprintf(what, sizeof what, "gethostbyname(%s)", host);
        fprintf(stderr, "PHP Warning: %s: %s\n", what, uv_strerror(rc));
        return NULL;
    }
    tphp_sched_event_wait(&op.ev);
    return op.req.addrinfo;
}

TVal tphp_net_gethostbyname(VM *vm, TVal *args, int n) {
    (void)n;
    if (args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "gethostbyname() expects a string");
    const char *host = tstr_val((TStr *)args[0].u.p);
    struct addrinfo *res = dns_resolve(vm, host);
    if (!res) return TV_FALSE;
    char ip[64] = "";
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            uv_ip4_name((struct sockaddr_in *)ai->ai_addr, ip, sizeof ip);
            break;
        }
    }
    uv_freeaddrinfo(res);
    if (!ip[0]) return TV_FALSE;
    return TV_STR(tstr_new(ip, strlen(ip)));
}

TVal tphp_net_gethostbynamel(VM *vm, TVal *args, int n) {
    (void)n;
    if (args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "gethostbynamel() expects a string");
    const char *host = tstr_val((TStr *)args[0].u.p);
    struct addrinfo *res = dns_resolve(vm, host);
    TArr *out = tarr_new();
    if (res) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET) continue;
            char ip[64];
            if (uv_ip4_name((struct sockaddr_in *)ai->ai_addr, ip,
                            sizeof ip) == 0) {
                bool dup = false;
                for (uint32_t i = 0; i < out->dused; i++) {
                    TVal prev = out->data[i];
                    if (prev.t == T_STR &&
                        strcmp(tstr_val((TStr *)prev.u.p), ip) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) tarr_append(out, TV_STR(tstr_new(ip, strlen(ip))));
            }
        }
        uv_freeaddrinfo(res);
    }
    return TV_ARR(out);
}

/* ---- TCP 连接 ---- */

typedef struct TcpOp {
    uv_connect_t creq;         /* creq.data = TcpOp* */
    SchedEvent ev;
    int status;
} TcpOp;

static void connect_cb(uv_connect_t *req, int status) {
    TcpOp *op = (TcpOp *)req->data;
    op->status = status;
    tphp_sched_event_fire(&op->ev);
}

static void tcp_free_cb(uv_handle_t *h) { tm_free(h); }   /* uv_close 异步释放 */

static void tcp_start_read(uv_tcp_t *tcp, TStreamCtx *s) {
    tcp->data = s;
    uv_read_start((uv_stream_t *)tcp, tcp_alloc_cb, tcp_read_cb);
}

/* 挂起等待 TCP 连接；成功返回 uv_tcp_t（未 read_start），失败 warning+NULL */
static uv_tcp_t *tcp_connect(VM *vm, const char *host, int port,
                             const char *who) {
    (void)vm;
    struct addrinfo *res = dns_resolve(vm, host);
    if (!res) return NULL;
    struct sockaddr_in addr;
    bool have = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            memcpy(&addr, ai->ai_addr, sizeof addr);
            have = true;
            break;
        }
    }
    uv_freeaddrinfo(res);
    if (!have) {
        fprintf(stderr, "PHP Warning: %s(): no IPv4 address for \"%s\"\n",
                who, host);
        return NULL;
    }
    addr.sin_port = htons((unsigned short)port);

    uv_tcp_t *tcp = tm_malloc(sizeof *tcp);
    memset(tcp, 0, sizeof *tcp);
    uv_tcp_init(tphp_sched_loop(), tcp);
    tcp->data = NULL;

    TcpOp op;
    memset(&op, 0, sizeof op);
    op.creq.data = &op;
    tphp_sched_event_reset(&op.ev);
    int rc = uv_tcp_connect(&op.creq, tcp, (const struct sockaddr *)&addr,
                            connect_cb);
    if (rc != 0) {
        char what[256];
        snprintf(what, sizeof what, "%s(): connect %s:%d", who, host, port);
        fprintf(stderr, "PHP Warning: %s: %s\n", what, uv_strerror(rc));
        uv_close((uv_handle_t *)tcp, tcp_free_cb);   /* 异步释放 */
        return NULL;
    }
    tphp_sched_event_wait(&op.ev);
    tphp_sched_check_cancel();
    if (op.status != 0) {
        char what[256];
        snprintf(what, sizeof what, "%s(): unable to connect to %s:%d",
                 who, host, port);
        fprintf(stderr, "PHP Warning: %s: %s\n", what,
                uv_strerror(op.status));
        uv_close((uv_handle_t *)tcp, tcp_free_cb);   /* 异步释放 */
        return NULL;
    }
    return tcp;
}

/* fsockopen($host, $port): TStream|false */
TVal tphp_net_fsockopen(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError",
                      "fsockopen() expects a hostname and a port");
    const char *host = tstr_val((TStr *)args[0].u.p);
    int64_t port = tval_to_int(args[1]);
    if (port <= 0 || port > 65535)
        tphp_vm_raise(vm, "ValueError", "fsockopen(): invalid port %lld",
                      (long long)port);
    uv_tcp_t *tcp = tcp_connect(vm, host, (int)port, "fsockopen");
    if (!tcp) return TV_FALSE;
    TStreamCtx *s = tm_malloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->is_tcp = true;
    s->tcp = tcp;
    tcp_start_read(tcp, s);
    TObj *o = tphp_stream_obj_new(s);
    return TV_OBJ(o);
}

/* ---- stream_socket_server + stream_socket_accept ---- */

typedef struct AcceptOp {
    SchedEvent ev;
    bool has_conn;
} AcceptOp;

static void accept_cb(uv_stream_t *server, int status) {
    AcceptOp *op = (AcceptOp *)server->data;
    if (op) {
        op->has_conn = (status == 0);
        tphp_sched_event_fire(&op->ev);   /* 单连接即醒（简版） */
    }
}

/* stream_socket_server("tcp://127.0.0.1:8080"): TStream(server)|false */
TVal tphp_net_server(VM *vm, TVal *args, int n) {
    (void)n;
    if (args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError",
                      "stream_socket_server() expects a tcp://host:port URL");
    const char *url = tstr_val((TStr *)args[0].u.p);
    const char *p = url;
    if (strncmp(p, "tcp://", 6) != 0)
        tphp_vm_raise(vm, "ValueError",
                      "stream_socket_server(): only tcp:// is supported");
    p += 6;
    char host[128];
    int port = 0;
    if (sscanf(p, "%127[^:]:%d", host, &port) != 2 || port <= 0 || port > 65535)
        tphp_vm_raise(vm, "ValueError",
                      "stream_socket_server(): malformed URL \"%s\"", url);

    struct sockaddr_in addr;
    int rc = uv_ip4_addr(host, port, &addr);
    if (rc != 0) {
        fprintf(stderr,
                "PHP Warning: stream_socket_server(): bad address: %s\n",
                uv_strerror(rc));
        return TV_FALSE;
    }
    uv_tcp_t *srv = tm_malloc(sizeof *srv);
    memset(srv, 0, sizeof *srv);
    uv_tcp_init(tphp_sched_loop(), srv);
    srv->data = NULL;
    if ((rc = uv_tcp_bind(srv, (const struct sockaddr *)&addr, 0)) != 0 ||
        (rc = uv_listen((uv_stream_t *)srv, 64, accept_cb)) != 0) {
        fprintf(stderr, "PHP Warning: stream_socket_server(): %s\n",
                uv_strerror(rc));
        uv_close((uv_handle_t *)srv, tcp_free_cb);   /* 异步释放 */
        return TV_FALSE;
    }
    TStreamCtx *s = tm_malloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->is_tcp = true;
    s->is_server = true;
    s->tcp = srv;
    srv->data = s;
    TObj *o = tphp_stream_obj_new(s);
    return TV_OBJ(o);
}

/* stream_socket_accept($server): TStream|false —— 挂起等连接 */
TVal tphp_net_accept(VM *vm, TVal *args, int n) {
    (void)n;
    TStreamCtx *s = tphp_stream_from_obj(vm, args[0], "stream_socket_accept");
    if (s && !s->is_server)
        tphp_vm_raise(vm, "TypeError",
                      "stream_socket_accept() expects a server TStream");

    AcceptOp op;
    memset(&op, 0, sizeof op);
    s->tcp->data = &op;
    tphp_sched_event_reset(&op.ev);
    tphp_sched_event_wait(&op.ev);
    s->tcp->data = s;

    if (!op.has_conn) return TV_FALSE;
    uv_tcp_t *client = tm_malloc(sizeof *client);
    memset(client, 0, sizeof *client);
    uv_tcp_init(tphp_sched_loop(), client);
    if (uv_accept((uv_stream_t *)s->tcp, (uv_stream_t *)client) != 0) {
        uv_close((uv_handle_t *)client, NULL);
        tm_free(client);
        return TV_FALSE;
    }
    TStreamCtx *cs = tm_malloc(sizeof *cs);
    memset(cs, 0, sizeof *cs);
    cs->is_tcp = true;
    cs->tcp = client;
    tcp_start_read(client, cs);
    TObj *o = tphp_stream_obj_new(cs);
    return TV_OBJ(o);
}

/* ---- TCP 流的读写（io.c 分派进来） ---- */

static void write_cb(uv_write_t *req, int status) {
    (void)status;                       /* 简版：错误按短写处理 */
    SchedEvent *ev = (SchedEvent *)req->data;
    tm_free(req);
    tphp_sched_event_fire(ev);
}

/* 窥看队首数据（不动队列）；EOF 块消费置 eof 并返回 NULL。
 * 调用方拷贝走数据后必须调 tcp_drop(s, taken) 前移队列。
 * （旧 tcp_take 部分取时先 memmove 再让调用方拷贝 → 数据错位，
 *   TLS 握手读 5 字节 record 头时暴露） */
static TcpChunk *tcp_peek(TStreamCtx *s, int64_t want, int *taken) {
    TcpChunk *ck = s->rcv_head;
    if (ck->eof) {
        s->rcv_head = ck->next;
        if (!s->rcv_head) s->rcv_tail = NULL;
        tm_free(ck);
        s->eof = true;
        *taken = 0;
        return NULL;
    }
    *taken = ck->len > want ? (int)want : ck->len;
    return ck;
}

/* 消费队首 taken 字节（配合 tcp_peek 使用） */
static void tcp_drop(TStreamCtx *s, int taken) {
    TcpChunk *ck = s->rcv_head;
    if (taken == ck->len) {
        s->rcv_head = ck->next;
        if (!s->rcv_head) s->rcv_tail = NULL;
        tm_free(ck->data);
        tm_free(ck);
    } else {
        memmove(ck->data, ck->data + taken, ck->len - taken);
        ck->len -= taken;
    }
}

TVal tcp_fread_impl(VM *vm, TStreamCtx *s, int64_t len) {
    if (len <= 0) return TV_STR(tstr_new("", 0));
    for (;;) {
        if (s->rcv_head) {
            int taken = 0;
            TcpChunk *ck = tcp_peek(s, len, &taken);
            if (!ck) return TV_STR(tstr_new("", 0));   /* EOF → "" */
            TStr *r = tstr_new(ck->data, (size_t)taken);
            tcp_drop(s, taken);
            return TV_STR(r);
        }
        if (s->eof) return TV_STR(tstr_new("", 0));
        /* 空队列：挂起等数据到达 */
        if (s->rcv_waiter)
            tphp_vm_raise(vm, "Error", "another fread is already waiting");
        s->rcv_waiter = tphp_sched_current();
        tphp_sched_wait_begin();
        tphp_sched_wait_suspend();
        s->rcv_waiter = NULL;
        tphp_sched_check_cancel();
    }
}

TVal tcp_fwrite_impl(VM *vm, TStreamCtx *s, TStr *data) {
    (void)vm;
    if (!tstr_len(data)) return TV_INT(0);
    /* data 的存活：挂起期间协程栈冻结，data 由调用 frame 持有 ✓ */
    SchedEvent ev;
    tphp_sched_event_reset(&ev);
    uv_write_t *wreq = tm_malloc(sizeof *wreq);
    memset(wreq, 0, sizeof *wreq);
    wreq->data = &ev;
    uv_buf_t b = uv_buf_init((char *)tstr_val(data),
                             (unsigned int)tstr_len(data));
    int rc = uv_write(wreq, (uv_stream_t *)s->tcp, &b, 1, write_cb);
    if (rc != 0) {
        tm_free(wreq);
        fprintf(stderr, "PHP Warning: fwrite(): %s\n", uv_strerror(rc));
        return TV_FALSE;
    }
    tphp_sched_event_wait(&ev);
    tphp_sched_check_cancel();
    return TV_INT((int64_t)tstr_len(data));
}

TVal tcp_fgets_impl(VM *vm, TStreamCtx *s) {
    size_t cap = 128, len = 0;
    char *out = tm_malloc(cap);
    bool hit_eof = false;
    for (;;) {
        /* 拼到 '\n' 或队列空 */
        while (s->rcv_head) {
            TcpChunk *ck = s->rcv_head;
            if (ck->eof) {
                s->rcv_head = ck->next;
                if (!s->rcv_head) s->rcv_tail = NULL;
                tm_free(ck);
                s->eof = true;
                hit_eof = true;
                break;
            }
            int nl = -1;
            for (int i = 0; i < ck->len; i++)
                if (ck->data[i] == '\n') { nl = i; break; }
            if (nl >= 0) {
                /* 含换行的 [0..nl] */
                while (len + (size_t)nl + 1 > cap) cap *= 2;
                out = tm_realloc(out, cap);
                memcpy(out + len, ck->data, (size_t)nl + 1);
                len += (size_t)nl + 1;
                if (nl + 1 == ck->len) {
                    s->rcv_head = ck->next;
                    if (!s->rcv_head) s->rcv_tail = NULL;
                    tm_free(ck->data);
                    tm_free(ck);
                } else {
                    memmove(ck->data, ck->data + nl + 1, ck->len - nl - 1);
                    ck->len -= nl + 1;
                }
                TStr *r = tstr_new(out, len);
                tm_free(out);
                return TV_STR(r);
            }
            /* 整块无换行：拼上继续 */
            while (len + (size_t)ck->len > cap) cap *= 2;
            out = tm_realloc(out, cap);
            memcpy(out + len, ck->data, (size_t)ck->len);
            len += (size_t)ck->len;
            s->rcv_head = ck->next;
            if (!s->rcv_head) s->rcv_tail = NULL;
            tm_free(ck->data);
            tm_free(ck);
        }
        if (hit_eof || s->eof) break;
        if (s->rcv_waiter)
            tphp_vm_raise(vm, "Error", "another fgets is already waiting");
        s->rcv_waiter = tphp_sched_current();
        tphp_sched_wait_begin();
        tphp_sched_wait_suspend();
        s->rcv_waiter = NULL;
        tphp_sched_check_cancel();
    }
    if (len == 0) {
        tm_free(out);
        return TV_FALSE;              /* EOF：PHP fgets 返回 false */
    }
    TStr *r = tstr_new(out, len);
    tm_free(out);
    return TV_STR(r);
}

TVal tcp_feof_impl(TStreamCtx *s) {
    /* 对齐「先读到 false/"" 再 feof」的 PHP 习惯：EOF 块被消费后才真 eof */
    return (s->eof && !s->rcv_head) ? TV_TRUE : TV_FALSE;
}

static void tcp_close_cb(uv_handle_t *h) {
    SchedEvent *ev = (SchedEvent *)h->data;
    tm_free(h);
    if (ev) tphp_sched_event_fire(ev);
}

void tcp_close_impl(VM *vm, TStreamCtx *s) {
    (void)vm;
    if (s->closed) return;
    /* 挂起中的读者：入 EOF 块并唤醒（恢复后得到 ""/false） */
    if (s->rcv_waiter) {
        s->rcv_waiter = NULL;
        tcp_enqueue(s, NULL, 0, true);
    }
    SchedEvent ev;
    tphp_sched_event_reset(&ev);
    s->tcp->data = &ev;
    uv_close((uv_handle_t *)s->tcp, tcp_close_cb);
    tphp_sched_event_wait(&ev);
    s->tcp = NULL;
    s->closed = true;
    /* 丢弃未消费数据 */
    TcpChunk *ck = s->rcv_head;
    while (ck) {
        TcpChunk *n = ck->next;
        tm_free(ck->data);
        tm_free(ck);
        ck = n;
    }
    s->rcv_head = s->rcv_tail = NULL;
}

/* ---- http_get（M4-e/f）----
 * 简版客户端：Connection: close + 全量读 → 解析状态行/头 →
 * 3xx Location 重定向（≤5 次，相对/绝对）→ chunked 解码 /
 * Content-Length 截断。自定义头经 opts['headers']（"Name: value" 数组）。
 * https 走 mbedtls（挂起式 BIO）。 */

static TStr *http_parse_response(VM *vm, char *raw, size_t raw_len,
                                 const char *url, int scheme_https, int port,
                                 TArr *extra_headers, int depth);
static TStr *http_fetch_any(VM *vm, const char *url, TArr *extra, int depth);
static const char *host_of(const char *url);

/* 解析 "http(s)://host[:port]/path"；scheme_https 由调用方按前缀判定。
 * 返回 false=不支持。缺省 path="/"。 */
static bool http_parse_url(const char *url, char *host, int host_sz,
                           int *port, char *path, int path_sz) {
    const char *p;
    if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
        if (*port == 0) *port = 80;
    } else if (strncmp(url, "https://", 8) == 0) {
        p = url + 8;
        if (*port == 0) *port = 443;
    } else {
        return false;
    }
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl == 0 || hl >= (size_t)host_sz) return false;
        memcpy(host, p, hl);
        host[hl] = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) return false;
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl == 0 || hl >= (size_t)host_sz) return false;
        memcpy(host, p, hl);
        host[hl] = 0;
        if (*port == 0) *port = 80;
    }
    if (slash) {
        snprintf(path, (size_t)path_sz, "%s", slash);
    } else {
        snprintf(path, (size_t)path_sz, "/");
    }
    return true;
}

/* 读响应直到 EOF，拼成 malloc 字符串（调用方释放） */
static char *http_read_all(TStreamCtx *s, size_t *out_len) {
    size_t cap = 64 * 1024, len = 0;
    char *buf = tm_malloc(cap);
    for (;;) {
        int taken = 0;
        if (s->rcv_head) {
            TcpChunk *ck = tcp_peek(s, 1 << 30, &taken);
            if (!ck) break;               /* EOF 块 */
            if (len + (size_t)taken > cap) {
                while (len + (size_t)taken > cap) cap *= 2;
                buf = tm_realloc(buf, cap);
            }
            memcpy(buf + len, ck->data, (size_t)taken);
            len += (size_t)taken;
            tcp_drop(s, taken);
            continue;
        }
        if (s->eof) break;
        if (s->rcv_waiter) { tm_free(buf); *out_len = 0; return NULL; }
        s->rcv_waiter = tphp_sched_current();
        tphp_sched_wait_begin();
        tphp_sched_wait_suspend();
        s->rcv_waiter = NULL;
        tphp_sched_check_cancel();
    }
    *out_len = len;
    return buf;
}

/* 大小写不敏感的头部行查找：返回 ":" 后的值指针（行内）或 NULL */
static const char *http_header_find(const char *hdrs, size_t hdrs_len,
                                    const char *name, size_t *val_len) {
    size_t nlen = strlen(name);
    const char *p = hdrs;
    const char *end = hdrs + hdrs_len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (ll > 0 && p[ll - 1] == '\r') ll--;
        if (ll > nlen + 1 && (p[nlen] == ':') &&
            strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(p + ll - v);
            *val_len = vl;
            return v;
        }
        p = eol ? eol + 1 : end;
    }
    return NULL;
}

/* chunked 解码：返回 malloc body（调用方释放），*out_len 长度 */
static char *http_decode_chunked(const char *in, size_t in_len,
                                 size_t *out_len) {
    size_t cap = in_len ? in_len : 16, len = 0;
    char *out = tm_malloc(cap);
    size_t pos = 0;
    for (;;) {
        /* 行：hex 长度（可带 ; 扩展）\r\n */
        char line[64];
        size_t ll = 0;
        while (pos < in_len && ll < sizeof line - 1) {
            char c = in[pos++];
            if (c == '\n') break;
            if (c != '\r') line[ll++] = c;
        }
        line[ll] = 0;
        long chunk_len = strtol(line, NULL, 16);
        if (chunk_len <= 0) break;
        if (pos + (size_t)chunk_len > in_len) {
            chunk_len = (long)(in_len - pos);   /* 截断保护 */
        }
        if (len + (size_t)chunk_len > cap) {
            while (len + (size_t)chunk_len > cap) cap *= 2;
            out = tm_realloc(out, cap);
        }
        memcpy(out + len, in + pos, (size_t)chunk_len);
        len += (size_t)chunk_len;
        pos += (size_t)chunk_len;
        while (pos < in_len && (in[pos] == '\r' || in[pos] == '\n')) pos++;
        if (chunk_len == 0) break;
    }
    *out_len = len;
    return out;
}

/* 一次 HTTP 请求（含重定向递归） */
static TStr *http_fetch(VM *vm, const char *url, TArr *extra_headers,
                        int depth) {
    char host[128], path[512];
    int port = 0;                     /* 0 = 按 scheme 缺省（parse 内补） */
    if (depth > 5) {
        fprintf(stderr,
                "PHP Warning: http_get(): too many redirects for %s\n", url);
        return NULL;
    }
    if (!http_parse_url(url, host, sizeof host, &port, path, sizeof path)) {
        if (strncmp(url, "https://", 8) == 0)
            fprintf(stderr,
                    "PHP Warning: http_get(): https requires mbedtls"
                    " (not yet bundled) for %s\n", url);
        else
            fprintf(stderr, "PHP Warning: http_get(): unsupported URL %s\n",
                    url);
        return NULL;
    }

    uv_tcp_t *tcp = tcp_connect(vm, host, port, "http_get");
    if (!tcp) return NULL;

    /* 请求（Host 头：port==80 省略端口） */
    char req[2048];
    int ql;
    if (port == 80)
        ql = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                      "User-Agent: TinyPHP.rt/0.4\r\n", path, host);
    else
        ql = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
                      "User-Agent: TinyPHP.rt/0.4\r\n", path, host, port);
    if (extra_headers) {
        for (uint32_t i = 0; i < extra_headers->dused; i++) {
            TVal h = extra_headers->data[i];
            if (h.t != T_STR) continue;
            TStr *hs = (TStr *)h.u.p;
            ql += snprintf(req + ql, sizeof req - (size_t)ql, "%s\r\n",
                           tstr_val(hs));
            if (ql >= (int)sizeof req - 2) break;
        }
    }
    ql += snprintf(req + ql, sizeof req - (size_t)ql, "\r\n");

    /* 写请求（直接 uv_write，不建 TStream） */
    {
        SchedEvent ev;
        tphp_sched_event_reset(&ev);
        uv_write_t *wreq = tm_malloc(sizeof *wreq);
        memset(wreq, 0, sizeof *wreq);
        wreq->data = &ev;
        uv_buf_t b = uv_buf_init(req, (unsigned int)ql);
        int rc = uv_write(wreq, (uv_stream_t *)tcp, &b, 1, write_cb);
        if (rc != 0) {
            tm_free(wreq);
            fprintf(stderr, "PHP Warning: http_get(): %s\n", uv_strerror(rc));
            uv_close((uv_handle_t *)tcp, tcp_free_cb);
            return NULL;
        }
        tphp_sched_event_wait(&ev);
        tphp_sched_check_cancel();
    }

    /* 流上下文借用（读数据用 rcv 队列机制） */
    TStreamCtx tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.is_tcp = true;
    tmp.tcp = tcp;
    tcp->data = &tmp;
    uv_read_start((uv_stream_t *)tcp, tcp_alloc_cb, tcp_read_cb);

    size_t raw_len = 0;
    char *raw = http_read_all(&tmp, &raw_len);
    uv_close((uv_handle_t *)tcp, tcp_free_cb);
    if (!raw) {
        fprintf(stderr, "PHP Warning: http_get(): read failed for %s\n", url);
        return NULL;
    }
    return http_parse_response(vm, raw, raw_len, url, 0, port, extra_headers,
                               depth);
}

/* ---- TLS（M4-f）：挂起式 BIO + https:// ---- */

/* CA bundle 进程级只读共享（mbedtls_x509_crt 解析后仅读）；uv_once 保证
 * 多线程首次访问只初始化一次（M5） */
static mbedtls_x509_crt *g_tls_cacert;
static uv_once_t g_tls_ca_once = UV_ONCE_INIT;

static void tls_cacert_load(void) {
    mbedtls_x509_crt *ca = tm_malloc(sizeof *ca);
    mbedtls_x509_crt_init(ca);
    int rc = mbedtls_x509_crt_parse(ca, tphp_cacert_pem, tphp_cacert_pem_len);
    if (rc != 0) {
        mbedtls_x509_crt_free(ca);
        tm_free(ca);
        return;                   /* g_tls_cacert 保持 NULL：调用方报错 */
    }
    g_tls_cacert = ca;
}

static int tls_cacert_init(void) {
    uv_once(&g_tls_ca_once, tls_cacert_load);
    return g_tls_cacert ? 0 : -8576;
}

typedef struct TlsSession {
    uv_tcp_t *tcp;
    TStreamCtx stream;          /* rcv 队列复用（密文到达排队） */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} TlsSession;

/* BIO send：挂起等待 uv_write 完成（协程冻结在回调里，握手期间安全） */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    TStreamCtx *s = (TStreamCtx *)ctx;
    if (!len) return 0;
    SchedEvent ev;
    tphp_sched_event_reset(&ev);
    uv_write_t *wreq = tm_malloc(sizeof *wreq);
    memset(wreq, 0, sizeof *wreq);
    wreq->data = &ev;
    uv_buf_t b = uv_buf_init((char *)buf, (unsigned int)len);
    int rc = uv_write(wreq, (uv_stream_t *)s->tcp, &b, 1, write_cb);
    if (rc != 0) {
        tm_free(wreq);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    tphp_sched_event_wait(&ev);
    tphp_sched_check_cancel();
    return (int)len;
}

/* BIO recv：从 rcv 队列取一块；空 → 挂起等到达；EOF → CONN_EOF */
static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    TStreamCtx *s = (TStreamCtx *)ctx;
    for (;;) {
        if (s->rcv_head) {
            int taken = 0;
            TcpChunk *ck = tcp_peek(s, (int64_t)len, &taken);
            if (!ck) return MBEDTLS_ERR_SSL_CONN_EOF;   /* 对端关闭 */
            memcpy(buf, ck->data, (size_t)taken);       /* taken ≤ len */
            tcp_drop(s, taken);
            return taken;
        }
        if (s->eof) return MBEDTLS_ERR_SSL_CONN_EOF;
        if (s->rcv_waiter) return MBEDTLS_ERR_SSL_WANT_READ;
        s->rcv_waiter = tphp_sched_current();
        tphp_sched_wait_begin();
        tphp_sched_wait_suspend();
        s->rcv_waiter = NULL;
        tphp_sched_check_cancel();
    }
}

/* 挂起握手 → 成功返回 TlsSession（失败 warning + NULL，资源已清理） */
static TlsSession *tls_connect(VM *vm, const char *host, int port) {
    (void)vm;
    int rc = tls_cacert_init();
    if (rc != 0) {
        fprintf(stderr, "PHP Warning: https: CA bundle parse failed (%d)\n",
                rc);
        return NULL;
    }
    uv_tcp_t *tcp = tcp_connect(vm, host, port, "https");
    if (!tcp) return NULL;

    TlsSession *ts = tm_malloc(sizeof *ts);
    memset(ts, 0, sizeof *ts);
    ts->tcp = tcp;
    ts->stream.is_tcp = true;
    ts->stream.tcp = tcp;
    tcp->data = &ts->stream;
    tcp_start_read(tcp, &ts->stream);   /* 启动 uv_read_start：密文入 rcv 队列 */

    mbedtls_ssl_init(&ts->ssl);
    mbedtls_ssl_config_init(&ts->conf);
    mbedtls_entropy_init(&ts->entropy);
    mbedtls_ctr_drbg_init(&ts->drbg);

    do {
        rc = mbedtls_ctr_drbg_seed(&ts->drbg, mbedtls_entropy_func,
                                   &ts->entropy, NULL, 0);
        if (rc != 0) break;
        rc = mbedtls_ssl_config_defaults(&ts->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) break;
        mbedtls_ssl_conf_rng(&ts->conf, mbedtls_ctr_drbg_random, &ts->drbg);
        mbedtls_ssl_conf_ca_chain(&ts->conf, g_tls_cacert, NULL);
        mbedtls_ssl_conf_authmode(&ts->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        rc = mbedtls_ssl_setup(&ts->ssl, &ts->conf);
        if (rc != 0) break;
        rc = mbedtls_ssl_set_hostname(&ts->ssl, host);
        if (rc != 0) break;
        mbedtls_ssl_set_bio(&ts->ssl, &ts->stream, tls_bio_send, tls_bio_recv,
                            NULL);
        /* 挂起式握手：BIO 回调阻塞，循环直到成功 */
        while ((rc = mbedtls_ssl_handshake(&ts->ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char err[192];
                mbedtls_strerror(rc, err, sizeof err);
                fprintf(stderr,
                        "PHP Warning: https: handshake with %s failed: %s\n",
                        host, err);
                goto fail;
            }
        }
        return ts;
    } while (0);

fail:
    mbedtls_ssl_free(&ts->ssl);
    mbedtls_ssl_config_free(&ts->conf);
    mbedtls_ctr_drbg_free(&ts->drbg);
    mbedtls_entropy_free(&ts->entropy);
    uv_close((uv_handle_t *)tcp, tcp_free_cb);
    tm_free(ts);
    return NULL;
}

static void tls_session_free(TlsSession *ts) {
    mbedtls_ssl_free(&ts->ssl);
    mbedtls_ssl_config_free(&ts->conf);
    mbedtls_ctr_drbg_free(&ts->drbg);
    mbedtls_entropy_free(&ts->entropy);
    uv_close((uv_handle_t *)ts->tcp, tcp_free_cb);
    tm_free(ts);
}

/* TLS 写请求（mbedtls_ssl_write 挂起式）；返回 0 或 mbedtls 错误码 */
static int tls_write_all(TlsSession *ts, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = mbedtls_ssl_write(&ts->ssl, (const unsigned char *)buf + off,
                                  len - off);
        if (w <= 0) return w;
        off += (size_t)w;
    }
    return 0;
}

/* TLS 读到 EOF，拼成 malloc 缓冲（调用方释放） */
static char *tls_read_all(TlsSession *ts, size_t *out_len) {
    size_t cap = 64 * 1024, len = 0;
    char *buf = tm_malloc(cap);
    for (;;) {
        unsigned char chunk[TCP_RCV_BUF];
        int r = mbedtls_ssl_read(&ts->ssl, chunk, sizeof chunk);
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
            r == MBEDTLS_ERR_SSL_CONN_EOF)
            break;                        /* 对端关闭 */
        if (r == MBEDTLS_ERR_SSL_WANT_READ ||
            r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r < 0) {
            char err[192];
            mbedtls_strerror(r, err, sizeof err);
            fprintf(stderr, "PHP Warning: https: read failed: %s\n", err);
            tm_free(buf);
            *out_len = 0;
            return NULL;
        }
        if (len + (size_t)r > cap) {
            while (len + (size_t)r > cap) cap *= 2;
            buf = tm_realloc(buf, cap);
        }
        memcpy(buf + len, chunk, (size_t)r);
        len += (size_t)r;
    }
    *out_len = len;
    return buf;
}

/* https:// 传输层（重定向可能切协议 → 递归经 http_fetch_any） */
static TStr *https_fetch(VM *vm, const char *url, TArr *extra_headers,
                         int depth);

/* http/https 分派入口 */
static TStr *http_fetch_any(VM *vm, const char *url, TArr *extra,
                            int depth) {
    if (strncmp(url, "https://", 8) == 0)
        return https_fetch(vm, url, extra, depth);
    return http_fetch(vm, url, extra, depth);
}

static TStr *https_fetch(VM *vm, const char *url, TArr *extra_headers,
                         int depth) {
    char host[128], path[512];
    int port = 0;                     /* 0 = 按 scheme 缺省（parse 内补） */
    if (depth > 5) {
        fprintf(stderr,
                "PHP Warning: http_get(): too many redirects for %s\n", url);
        return NULL;
    }
    if (!http_parse_url(url, host, sizeof host, &port, path, sizeof path)) {
        fprintf(stderr, "PHP Warning: http_get(): unsupported URL %s\n", url);
        return NULL;
    }
    TlsSession *ts = tls_connect(vm, host, port);
    if (!ts) return NULL;

    char req[2048];
    int ql = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                      "User-Agent: TinyPHP.rt/0.4\r\n", path, host);
    if (port != 443)
        ql = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
                      "User-Agent: TinyPHP.rt/0.4\r\n", path, host, port);
    if (extra_headers) {
        for (uint32_t i = 0; i < extra_headers->dused; i++) {
            TVal h = extra_headers->data[i];
            if (h.t != T_STR) continue;
            ql += snprintf(req + ql, sizeof req - (size_t)ql, "%s\r\n",
                           tstr_val((TStr *)h.u.p));
            if (ql >= (int)sizeof req - 2) break;
        }
    }
    ql += snprintf(req + ql, sizeof req - (size_t)ql, "\r\n");

    int wrc = tls_write_all(ts, req, (size_t)ql);
    if (wrc != 0) {
        char err[192];
        mbedtls_strerror(wrc, err, sizeof err);
        fprintf(stderr, "PHP Warning: https: write failed to %s: %s\n",
                url, err);
        tls_session_free(ts);
        return NULL;
    }
    size_t raw_len = 0;
    char *raw = tls_read_all(ts, &raw_len);
    tls_session_free(ts);
    if (!raw) {
        fprintf(stderr, "PHP Warning: http_get(): read failed for %s\n", url);
        return NULL;
    }
    return http_parse_response(vm, raw, raw_len, url, 1, port, extra_headers,
                               depth);
}

/* 解析响应：状态行/头块/重定向/chunked/Content-Length → TStr body */
static TStr *http_parse_response(VM *vm, char *raw, size_t raw_len,
                                 const char *url, int scheme_https, int port,
                                 TArr *extra_headers, int depth) {
    /* 解析：状态行 \r\n 头块 \r\n\r\n body */
    const char *eol = memchr(raw, '\n', raw_len);
    if (!eol) {
        tm_free(raw);
        fprintf(stderr, "PHP Warning: http_get(): malformed response from %s\n",
                url);
        return NULL;
    }
    int status = 0;
    sscanf(raw, "HTTP/%*d.%*d %d", &status);
    const char *hdr_start = eol + 1;
    /* 头块结束：\r\n\r\n（或裸 \n\n） */
    size_t rest_len = raw_len - (size_t)(hdr_start - raw);
    const char *body = NULL;
    size_t sep = 0;
    for (size_t i = 0; i + 4 <= rest_len; i++)
        if (hdr_start[i] == '\r' && hdr_start[i+1] == '\n' &&
            hdr_start[i+2] == '\r' && hdr_start[i+3] == '\n') {
            body = hdr_start + i + 4; sep = 4;
            break;
        }
    if (!body)
        for (size_t i = 0; i + 2 <= rest_len; i++)
            if (hdr_start[i] == '\n' && hdr_start[i+1] == '\n') {
                body = hdr_start + i + 2; sep = 2;
                break;
            }
    if (!body) {
        tm_free(raw);
        fprintf(stderr, "PHP Warning: http_get(): no body from %s\n", url);
        return NULL;
    }
    size_t hdrs_len = (size_t)(body - hdr_start) - sep;

    /* 重定向（3xx + Location）：相对 Location 按当前 scheme/host 拼接 */
    if (status >= 300 && status < 400) {
        size_t ll = 0;
        const char *loc = http_header_find(hdr_start, hdrs_len, "location",
                                           &ll);
        if (loc) {
            char next[600];
            if ((ll >= 8 && strncmp(loc, "http://", 7) == 0) ||
                (ll >= 9 && strncmp(loc, "https://", 8) == 0)) {
                snprintf(next, sizeof next, "%.*s", (int)ll, loc);
            } else {
                char base[256];
                if (scheme_https)
                    snprintf(base, sizeof base, "https://%s%s%d", host_of(url),
                             port == 443 ? "" : ":",
                             port == 443 ? 443 : port);
                else
                    snprintf(base, sizeof base, "http://%s%s%d", host_of(url),
                             port == 80 ? "" : ":",
                             port == 80 ? 80 : port);
                if (loc[0] == '/')
                    snprintf(next, sizeof next, "%s%.*s", base, (int)ll, loc);
                else
                    snprintf(next, sizeof next, "%s/%.*s", base, (int)ll, loc);
            }
            tm_free(raw);
            return http_fetch_any(vm, next, extra_headers, depth + 1);
        }
    }

    /* body：chunked / Content-Length 截断 / 剩余全部 */
    TStr *out;
    size_t vl = 0;
    const char *te = http_header_find(hdr_start, hdrs_len,
                                      "transfer-encoding", &vl);
    if (te && vl >= 7 && strncasecmp(te, "chunked", 7) == 0) {
        size_t bl = 0;
        char *body_dec = http_decode_chunked(body, raw_len - (size_t)(body - raw),
                                             &bl);
        out = tstr_new(body_dec, bl);
        tm_free(body_dec);
    } else {
        const char *cl = http_header_find(hdr_start, hdrs_len,
                                          "content-length", &vl);
        size_t body_len = raw_len - (size_t)(body - raw);
        if (cl) {
            long want = strtol(cl, NULL, 10);
            if (want >= 0 && (size_t)want <= body_len) body_len = (size_t)want;
        }
        out = tstr_new(body, body_len);
    }
    tm_free(raw);
    return out;
}

/* host_of：从 URL 取 host（重定向相对拼接用） */
static const char *host_of(const char *url) {
    static char hb[128];
    const char *p = strstr(url, "://");
    if (!p) return url;
    p += 3;
    size_t i = 0;
    for (; p[i] && p[i] != '/' && p[i] != ':' && i < sizeof hb - 1; i++)
        hb[i] = p[i];
    hb[i] = 0;
    return hb;
}

TVal tphp_net_http_get(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "http_get() expects a URL string");
    const char *url = tstr_val((TStr *)args[0].u.p);
    TArr *extra = NULL;
    if (n >= 2 && args[1].t == T_ARR) {
        extra = (TArr *)args[1].u.p;
        /* opts['headers']（可选） */
        TVal key = TV_STR(tstr_new("headers", 7));
        TVal *hv = tarr_get(extra, key);
        tval_release(&key);
        if (hv && hv->t == T_ARR) extra = (TArr *)hv->u.p;
        else extra = NULL;
    }
    TStr *body = http_fetch_any(vm, url, extra, 0);
    if (!body) return TV_FALSE;
    return TV_STR(body);
}
