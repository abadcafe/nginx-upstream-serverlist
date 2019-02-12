#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include "picohttpparser.h"

#if (NGX_HTTP_UPSTREAM_CHECK)
#include "ngx_http_upstream_check_module.h"
#endif

#define MAX_CONF_DUMP_PATH_LENGTH 512
#define MAX_HTTP_REQUEST_SIZE 1024
#define MAX_HTTP_RECEIVED_HEADERS 32
#define DEFAULT_REFRESH_TIMEOUT_MS 1000
#define DEFAULT_REFRESH_INTERVAL_MS 5000
#define DEFAULT_SERVICE_CONCURRENCY 1
#define DUMP_BUFFER_SIZE 512
#define CACHE_LINE_SIZE 128
#define DEFAULT_SERVERLIST_POOL_SIZE 1024

typedef struct {
    ngx_pool_t                   *new_pool;
    ngx_pool_t                   *pool;
    ngx_http_upstream_srv_conf_t *upstream_conf; // TODO: should be a array to
                                                 // store all upstreams which
                                                 // shared one serverlist.
    ngx_str_t                     name;
    ngx_shmtx_t                   dump_file_lock; // to avoid parrallel write.

    time_t                        last_modified;
    ngx_str_t                     etag;
} serverlist;

typedef struct {
    ngx_peer_connection_t         peer_conn;
    ngx_buf_t                     send; // never exceed 1024.
    ngx_buf_t                     recv;
    ngx_str_t                     body;
    ngx_int_t                     content_length;
    ngx_event_t                   refresh_timer;
    ngx_event_t                   timeout_timer;
    ngx_uint_t                    serverlists_start;
    ngx_uint_t                    serverlists_end;
    ngx_uint_t                    serverlists_curr;
    ngx_time_t                    start_time;
} service_conn;

typedef struct {
    ngx_http_conf_ctx_t          *conf_ctx;
    ngx_pool_t                   *conf_pool;
    ngx_array_t                   service_conns;
    ngx_array_t                   serverlists;

    ngx_uint_t                    service_concurrency;
    ngx_url_t                     service_url;
    ngx_str_t                     conf_dump_dir;
} main_conf;

static void *
create_main_conf(ngx_conf_t *cf);

static char *
merge_server_conf(ngx_conf_t *cf, void *parent, void *child);

static char *
serverlist_service_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy);

static char *
serverlist_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy);

static char *
dump_upstreams_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t
init_module(ngx_cycle_t *cycle);

static ngx_int_t
init_process(ngx_cycle_t *cycle);

static void
exit_process(ngx_cycle_t *cycle);

static void
refresh_timeout_handler(ngx_event_t *ev);

static void
connect_to_service(ngx_event_t *ev);

static void
send_to_service(ngx_event_t *ev);

static void
recv_from_service(ngx_event_t *ev);

static ngx_command_t module_commands[] = {
    {
        ngx_string("serverlist"),
        NGX_HTTP_UPS_CONF | NGX_CONF_ANY,
        serverlist_directive,
        0,
        0,
        NULL
    },
    {
        ngx_string("serverlist_service"),
        NGX_HTTP_MAIN_CONF | NGX_CONF_1MORE,
        serverlist_service_directive,
        0,
        0,
        NULL
    },
    {  ngx_string("dump_upstreams"),
        NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
        dump_upstreams_directive,
        0,
        0,
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    create_main_conf,                      /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    merge_server_conf,                     /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t ngx_http_upstream_serverlist_module = {
    NGX_MODULE_V1,
    &module_ctx,                           /* module context */
    module_commands,                       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    init_module,                           /* init module */
    init_process,                          /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    exit_process,                          /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t refresh_interval_ms = DEFAULT_REFRESH_INTERVAL_MS;

static ngx_int_t
random_interval_ms() {
    return refresh_interval_ms + ngx_random() % 500;
}

static ngx_int_t
whole_world_exiting() {
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        return 1;
    }

    return 0;
}

static void *
create_main_conf(ngx_conf_t *cf) {
    main_conf *mcf = NULL;

    mcf = ngx_pcalloc(cf->pool, sizeof *mcf);
    if (mcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&mcf->serverlists, cf->pool, 1,
            sizeof(serverlist)) != NGX_OK) {
        return NULL;
    }

     if (ngx_array_init(&mcf->service_conns, cf->pool, 1,
            sizeof(service_conn)) != NGX_OK) {
        return NULL;
    }

    ngx_memzero(&mcf->conf_dump_dir, sizeof mcf->conf_dump_dir);
    ngx_memzero(&mcf->service_url, sizeof mcf->service_url);
    ngx_str_set(&mcf->service_url.url, "127.84.10.13/");
    mcf->service_url.default_port = 80;
    mcf->service_url.uri_part = 1;
    mcf->service_concurrency = DEFAULT_SERVICE_CONCURRENCY;
    mcf->conf_ctx = cf->ctx;
    mcf->conf_pool = cf->pool;
    return mcf;
}

static char *
serverlist_service_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {
    main_conf *mcf = ngx_http_conf_get_module_main_conf(cf,
        ngx_http_upstream_serverlist_module);
    ngx_str_t *s = NULL;
    ngx_uint_t i = 1;
    ngx_int_t ret = -1;

    if (cf->args->nelts <= 1) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: serverlist_service need at least 1 arg");
        return NGX_CONF_ERROR;
    }

    for (i = 1; i < cf->args->nelts; i++) {
        s = (ngx_str_t *)cf->args->elts + i;

        if (s->len > 4 && ngx_strncmp(s->data, "url=", 4) == 0) {
            if (s->len > 4 + 7 && ngx_strncmp(s->data + 4, "http://", 7) != 0) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: serverlist_service only support "
                    "http url");
                return NGX_CONF_ERROR;
            }

            mcf->service_url.url.data = s->data + 4 + 7;
            mcf->service_url.url.len = s->len - 4 - 7;
        } else if (s->len > 14 && ngx_strncmp(s->data, "conf_dump_dir=",
                14) == 0) {
            mcf->conf_dump_dir.data = s->data + 14;
            mcf->conf_dump_dir.len = s->len - 14;
            if (ngx_conf_full_name(cf->cycle, &mcf->conf_dump_dir,
                    1) != NGX_OK) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: get full path of 'conf_dump_dir' "
                    "failed");
                return NGX_CONF_ERROR;
            }
        } else if (s->len > 9 && ngx_strncmp(s->data, "interval=", 9) == 0) {
            ngx_str_t itv_str = {.data = s->data + 9, .len = s->len - 9};
            ngx_int_t itv = 0;
            itv = ngx_parse_time(&itv_str, 0);
            if (itv == NGX_ERROR || itv == 0) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: argument 'interval' value invalid");
                return NGX_CONF_ERROR;
            }

            refresh_interval_ms = itv;
        } else if (s->len > 12 && ngx_strncmp(s->data, "concurrency=",
                12) == 0) {
            ret = ngx_atoi(s->data + 12, s->len - 12);
            if (ret == NGX_ERROR || ret == 0) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: argument 'concurrency' value "
                    "invalid");
                continue;
            }

            mcf->service_concurrency = ret;
        } else {
            ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                "upstream-serverlist: argument '%V' format error", s);
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static char *
serverlist_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {
    ngx_http_upstream_srv_conf_t *uscf = ngx_http_conf_get_module_srv_conf(cf,
        ngx_http_upstream_module);
    main_conf *mcf = ngx_http_conf_get_module_main_conf(cf,
        ngx_http_upstream_serverlist_module);
    serverlist *sl = NULL;

    if (cf->args->nelts > 2) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: serverlist only need 0 or 1 args");
        return NGX_CONF_ERROR;
    }

    sl = ngx_array_push(&mcf->serverlists);
    if (sl == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(sl, sizeof *sl);
    sl->upstream_conf = uscf;
    sl->last_modified = -1;
    sl->name = cf->args->nelts <= 1 ? uscf->host
        : ((ngx_str_t *)cf->args->elts)[1];

    return NGX_CONF_OK;
}

static char *
merge_server_conf(ngx_conf_t *cf, void *parent, void *child) {
    main_conf *mcf = ngx_http_conf_get_module_main_conf(cf,
        ngx_http_upstream_serverlist_module);
    u_char conf_dump_dir[MAX_CONF_DUMP_PATH_LENGTH] = {0};
    ngx_int_t ret = -1;

    ret = ngx_parse_url(cf->pool, &mcf->service_url);
    if (ret != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: parse service url failed: %s",
            mcf->service_url.err);
        return NGX_CONF_ERROR;
    } else if (mcf->service_url.uri.len <= 0) {
        ngx_str_set(&mcf->service_url.uri, "/");
    }

    if (mcf->conf_dump_dir.len > sizeof conf_dump_dir) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
            "upstream-serverlist: conf dump path %s is too long",
            conf_dump_dir);
        return NGX_CONF_ERROR;
    } else if (mcf->conf_dump_dir.len > 0) {
        struct stat statbuf = {0};

        ngx_memzero(conf_dump_dir, sizeof conf_dump_dir);
        ngx_memzero(&statbuf, sizeof statbuf);
        ngx_memmove(conf_dump_dir, mcf->conf_dump_dir.data,
            mcf->conf_dump_dir.len);
        ret = stat((const char *)conf_dump_dir, &statbuf);
        if (ret < 0) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                "upstream-serverlist: conf dump dir %s is not exists",
                conf_dump_dir);
            return NGX_CONF_ERROR;
        } else if (!S_ISDIR(statbuf.st_mode)) {
            ngx_conf_log_error(NGX_LOG_ERR, cf, ngx_errno,
                "upstream-serverlist: conf dump path %s is not a dir",
                conf_dump_dir);
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

static ngx_int_t
init_module(ngx_cycle_t *cycle) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(cycle,
        ngx_http_upstream_serverlist_module);
    serverlist *sl = NULL;
    ngx_shm_t shm = {0};
    ngx_uint_t i = 0;
    ngx_int_t ret = -1;

#if !(NGX_HAVE_ATOMIC_OPS)
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
        "upstream-serverlist: this module need ATOMIC_OPS support!!!");
    return NGX_ERROR;
#endif

    if (mcf->serverlists.nelts <= 0) {
        return NGX_OK;
    }

    // align to cache line to avoid false sharing.
    shm.size = CACHE_LINE_SIZE * mcf->serverlists.nelts;
    shm.log = cycle->log;
    ngx_str_set(&shm.name, "upstream-serverlist-shared-zone");
    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < mcf->serverlists.nelts; i++) {
        sl = (serverlist *)mcf->serverlists.elts + i;
        ret = ngx_shmtx_create(&sl->dump_file_lock,
            (ngx_shmtx_sh_t *)(shm.addr + CACHE_LINE_SIZE * i), NULL);
        if ( ret != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
init_process(ngx_cycle_t *cycle) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(cycle,
        ngx_http_upstream_serverlist_module);
    ngx_uint_t i = 0;
    ngx_uint_t blocksize = 0;

    if (ngx_process != NGX_PROCESS_WORKER
            && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }

    if (mcf->serverlists.nelts >= mcf->service_concurrency) {
        blocksize = (mcf->serverlists.nelts + (mcf->service_concurrency - 1))
            / mcf->service_concurrency;
    } else {
        blocksize = 1;
    }

    for (i = 0; i < mcf->service_concurrency; i++) {
        // todo: fix bug.
        service_conn *sc = ngx_array_push(&mcf->service_conns);
        ngx_memzero(sc, sizeof *sc);

        sc->send.start = ngx_pcalloc(mcf->conf_pool, MAX_HTTP_REQUEST_SIZE);
        if (sc->send.start == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "upstream-serverlist: allocate send buffer failed");
            return NGX_ERROR;
        }
        sc->send.end = sc->send.start + MAX_HTTP_REQUEST_SIZE;
        sc->send.last = sc->send.pos = sc->send.start;

        sc->recv.start = ngx_pcalloc(mcf->conf_pool, ngx_pagesize);
        if (sc->recv.start == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "upstream-serverlist: allocate recv buffer failed");
            return NGX_ERROR;
        }
        sc->recv.end = sc->recv.start + MAX_HTTP_REQUEST_SIZE;
        sc->recv.last = sc->recv.pos = sc->recv.start;

        ngx_memzero(&sc->peer_conn, sizeof sc->peer_conn);
        sc->peer_conn.data = NULL;
        sc->peer_conn.log = cycle->log;
        sc->peer_conn.log_error = NGX_ERROR_ERR;
        sc->peer_conn.connection = NULL;
        sc->peer_conn.get = ngx_event_get_peer;
        sc->peer_conn.name = &mcf->service_url.host;
        sc->peer_conn.sockaddr = &mcf->service_url.sockaddr.sockaddr;
        sc->peer_conn.socklen = mcf->service_url.socklen;

        sc->serverlists_start = ngx_min(mcf->serverlists.nelts,
            0 + blocksize * i);
        sc->serverlists_end = ngx_min(mcf->serverlists.nelts,
            sc->serverlists_start + blocksize);
        sc->serverlists_curr = sc->serverlists_start;

        sc->timeout_timer.handler = refresh_timeout_handler;
        sc->timeout_timer.log = cycle->log;
        sc->timeout_timer.data = sc;
        sc->refresh_timer.handler = connect_to_service;
        sc->refresh_timer.log = cycle->log;
        sc->refresh_timer.data = sc;

        if ((ngx_uint_t)sc->serverlists_start < mcf->serverlists.nelts) {
            ngx_add_timer(&sc->refresh_timer, random_interval_ms());
        }
    }

    return NGX_OK;
}

static void
exit_process(ngx_cycle_t *cycle) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(cycle,
        ngx_http_upstream_serverlist_module);
    serverlist *sls = mcf->serverlists.elts;
    service_conn *scs = mcf->service_conns.elts;
    ngx_uint_t i;

    for (i = 0; i < mcf->serverlists.nelts; i++) {
        if (sls[i].pool) {
            ngx_destroy_pool(sls[i].pool);
            sls[i].pool = NULL;
        }
    }

    for (i = 0; i < mcf->service_conns.nelts; i++) {
        if (scs[i].peer_conn.connection) {
            ngx_close_connection(scs[i].peer_conn.connection);
            scs[i].peer_conn.connection = NULL;
        }
    }
}

static void
empty_handler(ngx_event_t *ev) {
    ngx_log_debug(NGX_LOG_DEBUG_ALL, ev->log, 0,
        "upstream-serverlist: empty handler");
}

static void
idle_conn_read_handler(ngx_event_t *ev) {
    ngx_connection_t *c = ev->data;
    service_conn *sc = c->data;
    ngx_int_t ret = -1;
    char junk;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0, "idle conn read handler");

    if (c->close || c->read->timedout) {
        goto close;
    }

    ret = recv(c->fd, &junk, 1, MSG_PEEK);
    if (ret < 0 && ngx_socket_errno == NGX_EAGAIN) {
        ev->ready = 0;

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            goto close;
        }

        return;
    }

close:
    ngx_close_connection(sc->peer_conn.connection);
    sc->peer_conn.connection = NULL;
}

static void
refresh_timeout_handler(ngx_event_t *ev) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    service_conn *sc = ev->data;
    serverlist *sl = NULL;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_error(NGX_LOG_ERR, ev->log, 0,
        "upstream-serverlist: refresh timeout start %d end %d curr %d",
        sc->serverlists_start, sc->serverlists_end, sc->serverlists_curr);

    if (sc->peer_conn.connection) {
        ngx_close_connection(sc->peer_conn.connection);
        sc->peer_conn.connection = NULL;
    }

    sl = (serverlist *)mcf->serverlists.elts + sc->serverlists_curr;
    if (sl->new_pool) {
        ngx_destroy_pool(sl->new_pool);
        sl->new_pool = NULL;
    }

    ngx_add_timer(&sc->refresh_timer, random_interval_ms());
}

static void
connect_to_service(ngx_event_t *ev) {
    ngx_int_t ret = -1;
    service_conn *sc = ev->data;
    ngx_connection_t *c = NULL;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
        "upstream-serverlist: create connection for serverlists from %d to %d, "
        "cursor %d", sc->serverlists_start, sc->serverlists_end,
        sc->serverlists_curr);

    if (sc->start_time.sec <= 0) {
        sc->start_time = *ngx_timeofday();
    }

    c = sc->peer_conn.connection;
    if (c && c->read->ready) {
        c->read->handler(c->read);
    }

    if (!c) {
        ret = ngx_event_connect_peer(&sc->peer_conn);
        if (ret != NGX_DONE && ret != NGX_OK && ret != NGX_AGAIN) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: connect to service url failed: %V",
                sc->peer_conn.name);
            ngx_add_timer(&sc->refresh_timer, random_interval_ms());
            return;
        }
    }

    ngx_memzero(&sc->body, sizeof sc->body);
    sc->recv.pos = sc->recv.last = sc->recv.start;
    sc->send.pos = sc->send.last = sc->send.start;
    sc->content_length = -1;

    c = sc->peer_conn.connection;
    c->data = sc;
    c->sendfile = 0;
    c->sent = 0;
    c->idle = 1; // for quick exit.
    c->log = sc->peer_conn.log;
    c->write->log = c->log;
    c->read->log = c->log;
    c->write->handler = send_to_service;
    c->read->handler = recv_from_service;

    if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: handle write event failed");
        goto fail;
    }

    if (ngx_del_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: del read event failed");
        goto fail;
    }

    if (ret == NGX_OK) {
        c->write->handler(c->write);
    }

    return;

fail:
    ngx_close_connection(sc->peer_conn.connection);
    sc->peer_conn.connection = NULL;
    ngx_del_timer(&sc->timeout_timer);
    ngx_add_timer(&sc->refresh_timer, random_interval_ms());
}

// copy from ngx_http_ustream.c
static ngx_int_t
test_connect(ngx_connection_t *c) {
    int        err;
    socklen_t  len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT)  {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err,
                                    "kevent() reported that connect() failed");
            return NGX_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1)
        {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static void
send_to_service(ngx_event_t *ev) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    ngx_connection_t *c = ev->data;
    service_conn *sc = c->data;
    serverlist *sl = NULL;
    ssize_t ret = -1;

    if (whole_world_exiting()) {
        return;
    }

    if (sc->serverlists_curr >= sc->serverlists_end) {
        ngx_log_error(NGX_LOG_CRIT, ev->log, 0,
            "upstream-serverlist: cursor %d exceed serverlists upper "
            "bound %d", sc->serverlists_curr, sc->serverlists_end);
        sc->serverlists_curr = sc->serverlists_start;
        goto fail;
    }

    ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
        "upstream-serverlist: send begin cur %d start %d end %d act %d ready %d",
        sc->serverlists_curr, sc->serverlists_start, sc->serverlists_end,
        c->write->active, c->write->ready);

    c->write->ready = 0;
    ngx_add_timer(&sc->timeout_timer, DEFAULT_REFRESH_TIMEOUT_MS);

    if (sc->send.last == sc->send.start) {
        sl = (serverlist *)mcf->serverlists.elts + sc->serverlists_curr;
        if (sc->serverlists_curr == 0 && test_connect(c) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: serverlist %V test connect failed",
                &sl->name);
            goto fail;
        }

        // build request.
        sc->send.last = sc->send.pos = sc->send.start;
        sc->send.last = ngx_snprintf(sc->send.last,
            sc->send.end - sc->send.last,
            "GET %V%s%V HTTP/1.1\r\n", &mcf->service_url.uri,
            mcf->service_url.uri.data[mcf->service_url.uri.len - 1] == '/'
                ? "" : "/", &sl->name);

        if (mcf->service_url.family == AF_UNIX) {
            sc->send.last = ngx_snprintf(sc->send.last,
                sc->send.end - sc->send.last, "Host: localhost\r\n");
        } else {
            sc->send.last = ngx_snprintf(sc->send.last,
                sc->send.end - sc->send.last, "Host: %V\r\n",
                &mcf->service_url.host);
        }

        if (sl->last_modified >= 0) {
            u_char buf[64] = {0};

            ngx_memzero(buf, sizeof buf);
            ngx_http_time(buf, sl->last_modified);
            sc->send.last = ngx_snprintf(sc->send.last,
                sc->send.end - sc->send.last, "If-Modified-Since: %s\r\n", buf);
        }

        if (sl->etag.len > 0) {
            sc->send.last = ngx_snprintf(sc->send.last,
                sc->send.end - sc->send.last, "If-None-Match: %V\r\n",
                &sl->etag);
        }

        sc->send.last = ngx_snprintf(sc->send.last,
            sc->send.end - sc->send.last, "Connection: Keep-Alive\r\n\r\n");
    }

    while (sc->send.pos < sc->send.last) {
        ret = c->send(c, sc->send.pos, sc->send.last - sc->send.pos);
        if (ret > 0) {
            sc->send.pos += ret;
        } else if (ret == 0 || ret == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: send error");
            goto fail;
        }
    }

    // send is over, cleaning.
    sc->send.pos = sc->send.last = sc->send.start;

    ret = ngx_del_event(c->write, NGX_WRITE_EVENT, 0);
    if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: del write event failed");
        goto fail;
    }

    ret = ngx_handle_read_event(c->read, 0);
    if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: handle read event failed");
        goto fail;
    }

    ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
        "upstream-serverlist: send end cur %d start %d end %d act %d ready %d",
        sc->serverlists_curr, sc->serverlists_start, sc->serverlists_end,
        c->write->active, c->write->ready);
    return;

fail:
    ngx_close_connection(sc->peer_conn.connection);
    sc->peer_conn.connection = NULL;
    ngx_del_timer(&sc->timeout_timer);
    ngx_add_timer(&sc->refresh_timer, random_interval_ms());
}

static int
is_valid_arg_char(u_char c) {
    return isalnum(c) || c == '=' || c == '.' || c == '-' || c == '_' ||
        c == ':';
}

static u_char *
get_one_arg(u_char *buf, u_char *buf_end, ngx_str_t *arg) {
    u_char *pos = NULL, *arg_end = NULL;

    for (pos = buf; pos < buf_end; pos++) {
        if (is_valid_arg_char(*pos)) {
            break;
        }
    }

    if (pos >= buf_end) {
        return NULL;
    }

    for (arg_end = pos; arg_end < buf_end; arg_end++) {
        if (!is_valid_arg_char(*arg_end)) {
            break;
        }
    }

    arg->data = pos;
    arg->len = arg_end - pos;
    return arg_end;
}

static u_char *
get_one_line(u_char *buf, u_char *buf_end, ngx_str_t *line) {
    u_char *pos = ngx_strlchr(buf, buf_end, '\n');
    line->data = buf;
    line->len = pos == NULL ? buf_end - buf : pos - buf;
    return pos == NULL ? buf_end : pos + 1;
}

static ngx_array_t *
get_servers(ngx_pool_t *pool, ngx_str_t *body, ngx_log_t *log) {
    ngx_int_t ret = -1;
    ngx_array_t *servers = ngx_array_create(pool, 2,
        sizeof(ngx_http_upstream_server_t));
    ngx_http_upstream_server_t *server = NULL;
    ngx_url_t u;
    ngx_str_t curr_line = {0};
    ngx_str_t curr_arg = {0};

    u_char *body_pos = body->data;
    u_char *body_end = body->data + body->len;

    do {
        ngx_memzero(&curr_line, sizeof curr_line);
        body_pos = get_one_line(body_pos, body_end, &curr_line);
        ngx_int_t first_arg_found = 0;
        ngx_int_t second_arg_found = 0;
        u_char *line_pos = curr_line.data;
        u_char *line_end = curr_line.data + curr_line.len;
        while ((line_pos = get_one_arg(line_pos, line_end,
            &curr_arg)) != NULL) {
            if (!first_arg_found) {
                if (ngx_strncmp(curr_arg.data, "server", curr_arg.len) != 0) {
                    ngx_log_error(NGX_LOG_ERR, log, 0,
                        "upstream-serverlist: expect 'server' prefix");
                    break;
                }

                first_arg_found = 1;
            } else if (!second_arg_found) {
                ngx_memzero(&u, sizeof u);
                u.url = curr_arg;
                u.default_port = 80;
                ret = ngx_parse_url(pool, &u);
                if (ret != NGX_OK) {
                    ngx_log_error(NGX_LOG_ERR, log, 0,
                        "upstream-serverlist: parse addr failed");
                    break;
                }

                server = ngx_array_push(servers);
                ngx_memzero(server, sizeof *server);
                server->name = u.url;
                server->naddrs = u.naddrs;
                server->addrs = u.addrs;
                server->weight = 1;
#if nginx_version >= 1011005
                server->max_conns = 0;
#endif
                server->max_fails = 1;
                server->fail_timeout = 10;

                second_arg_found = 1;
            } else if (ngx_strncmp(curr_arg.data, "weight=", 7) == 0) {
                ret = ngx_atoi(curr_arg.data + 7, curr_arg.len - 7);
                if (ret == NGX_ERROR || ret <= 0) {
                    ngx_log_error(NGX_LOG_ERR, log, 0,
                        "upstream-serverlist: weight invalid");
                    continue;
                }

                server->weight = ret;
#if nginx_version >= 1011005
            } else if (ngx_strncmp(curr_arg.data, "max_conns=", 10) == 0) {
                ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                if (ret == NGX_ERROR) {
                    ngx_log_error(NGX_LOG_ERR, log, 0,
                        "upstream-serverlist: max_conns invalid");
                    continue;
                }

                server->max_conns = ret;
#endif
            } else if (ngx_strncmp(curr_arg.data, "max_fails=", 10) == 0) {
                ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                if (ret == NGX_ERROR) {
                    ngx_log_error(NGX_LOG_ERR, log,
                        0,
                        "upstream-serverlist: max_fails invalid");
                    continue;
                }

                server->max_fails = ret;
            } else if (ngx_strncmp(curr_arg.data, "fail_timeout=", 13) == 0) {
                ngx_str_t time_str = {.data = curr_arg.data + 13,
                    .len = curr_arg.len - 13};
                ret = ngx_parse_time(&time_str, 1);
                if (ret == NGX_ERROR) {
                    ngx_log_error(NGX_LOG_ERR, log, 0,
                        "upstream-serverlist: fail_timeout invalid");
                    continue;
                }

                server->fail_timeout = ret;
            } else if (ngx_strncmp(curr_arg.data, "down", 4) == 0) {
                server->down = 1;
            } else if (ngx_strncmp(curr_arg.data, "backup", 6) == 0) {
                server->backup = 1;
            } else if (curr_arg.len == 1 && curr_arg.data[0] == ';') {
                continue;
            } else {
                ngx_log_error(NGX_LOG_ERR, log, 0,
                    "upstream-serverlist: unknown server option %V", &curr_arg);
            }
        }
    } while (body_pos < body_end);

    return servers;
}

static ngx_int_t
upstream_servers_changed(const ngx_array_t *old, const ngx_array_t *new) {
    ngx_http_upstream_server_t *s1 = NULL, *s2 = NULL;
    ngx_addr_t *a1 = NULL, *a2 = NULL;
    ngx_uint_t i = 0, j = 0, k = 0, l = 0;

    if (old->nelts != new->nelts) {
        return 1;
    }

    for (i = 0; i < old->nelts; i++) {
        s1 = (ngx_http_upstream_server_t *)old->elts + i;
        for (j = 0; j < new->nelts; j++) {
            s2 = (ngx_http_upstream_server_t *)new->elts + j;
            if (s1->name.len != s2->name.len ||
                ngx_memcmp(s1->name.data, s2->name.data, s1->name.len) != 0 ||
                s1->weight != s2->weight ||
                s1->naddrs != s2->naddrs ||
#if nginx_version >= 1011005
                s1->max_conns != s2->max_conns ||
#endif
                s1->max_fails != s2->max_fails ||
                s1->fail_timeout != s2->fail_timeout ||
                s1->backup != s2->backup ||
                s1->down != s2->down) {
                continue;
            }

            for (k = 0; k < s1->naddrs; k++) {
                a1 = s1->addrs + k;
                for (l = 0; l < s2->naddrs; l++) {
                    a2 = s2->addrs + l;
                    if (a1->name.len == a2->name.len &&
                        ngx_memcmp(a1->name.data, a2->name.data,
                            a1->name.len) == 0 &&
                        a1->socklen == a2->socklen &&
                        ngx_memcmp(a1->sockaddr, a2->sockaddr,
                            sizeof *a1->sockaddr) == 0) {
                        break;
                    }
                }

                if (l >= s2->naddrs) {
                    return 1;
                }
            }

            break;
        }

        if (j >= new->nelts) {
            return 1;
        }
    }

    return 0;
}

static u_char *
build_server_line(u_char *buf, size_t bufsize,
    const ngx_http_upstream_server_t *s) {
    u_char *p = buf;

    p = ngx_snprintf(buf, bufsize,
        "server %V weight=%d max_fails=%d fail_timeout=%ds",&s->name, s->weight,
        s->max_fails, s->fail_timeout);
#if nginx_version >= 1011005
    p = ngx_snprintf(p, bufsize - (p - buf), " max_conns=%d", s->max_conns);
#endif

    if (s->down) {
        p = ngx_snprintf(p, bufsize - (p - buf), " down", s->down);
    }

    if (s->backup) {
        p = ngx_snprintf(p, bufsize - (p - buf), " backup", s->backup);
    }

    p = ngx_snprintf(p, bufsize - (p - buf), ";", s->backup);

    return p;
}

static void
dump_serverlist(serverlist *sl) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    u_char tmpfile[MAX_CONF_DUMP_PATH_LENGTH] = {0};
    ngx_fd_t fd = -1;
    ngx_http_upstream_server_t *s = NULL;
    u_char buf[DUMP_BUFFER_SIZE] = {0}, *p = NULL;
    ngx_uint_t i = 0;
    ssize_t ret = -1;

    if (mcf->conf_dump_dir.len <= 0) {
        return;
    } else if (!ngx_shmtx_trylock(&sl->dump_file_lock)) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
            "upstream-serverlist: another worker process %d is dumping",
            *sl->dump_file_lock.lock);
        return;
    }

    ngx_snprintf(tmpfile, sizeof tmpfile, "%V/.%V.conf.tmp",
        &mcf->conf_dump_dir, &sl->name);
    fd = ngx_open_file(tmpfile, NGX_FILE_WRONLY, NGX_FILE_TRUNCATE,
        NGX_FILE_DEFAULT_ACCESS);
    if (fd < 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
            "upstream-serverlist: open dump file %s failed", tmpfile);
        goto unlock;
    }

    for (i = 0; i < sl->upstream_conf->servers->nelts; i++) {
        s = (ngx_http_upstream_server_t *)sl->upstream_conf->servers->elts + i;

        // reserve the last char to ensure the server line has the last '\n'.
        p = build_server_line(buf, (sizeof buf) - 1, s);
        *p = '\n';
        p++;

        ret = ngx_write_fd(fd, buf, p - buf);
        if (ret < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                "upstream-serverlist: write dump file %s failed", tmpfile);
            ngx_close_file(fd);
            goto unlock;
        }
    }

    ngx_close_file(fd);
    ngx_memzero(buf, sizeof buf);
    ngx_snprintf(buf, (sizeof buf) - 1, "%V/%V.conf", &mcf->conf_dump_dir,
        &sl->name);
    ret = ngx_rename_file(tmpfile, buf);
    if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
            "upstream-serverlist: rename dump file %s failed", tmpfile);
        goto unlock;
    }

unlock:
    ngx_shmtx_unlock(&sl->dump_file_lock);
}

static ngx_int_t
refresh_upstream(serverlist *sl, ngx_str_t *body, ngx_log_t *log) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    ngx_http_upstream_srv_conf_t *uscf = sl->upstream_conf;
    ngx_http_upstream_init_pt init = NULL;
    ngx_conf_t cf = {0};
    ngx_array_t *new_servers = NULL;
    ngx_array_t *old_servers = uscf->servers;

    new_servers = get_servers(sl->new_pool, body, log);
    if (new_servers == NULL || new_servers->nelts <= 0) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
            "upstream-serverlist: parse serverlist %V failed", &sl->name);
        return -1;
    }

    if (!upstream_servers_changed(uscf->servers, new_servers)) {
        ngx_log_debug(NGX_LOG_INFO, log, 0,
            "upstream-serverlist: serverlist %V nothing changed",&sl->name);
        // once return -1, everything in the old pool will kept and the new pool
        // will discard, which is we hope for.
        return -1;
    }

    init = uscf->peer.init_upstream ? uscf->peer.init_upstream
        : ngx_http_upstream_init_round_robin;

    ngx_memzero(&cf, sizeof cf);
    cf.name = "serverlist_init_upstream";
    cf.cycle = (ngx_cycle_t *) ngx_cycle;
    cf.pool = sl->new_pool;
    cf.module_type = NGX_HTTP_MODULE;
    cf.cmd_type = NGX_HTTP_MAIN_CONF;
    cf.log = ngx_cycle->log;
    cf.ctx = mcf->conf_ctx;

    old_servers = uscf->servers;
    uscf->servers = new_servers;

    if (init(&cf, uscf) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
            "upstream-serverlist: refresh upstream %V failed, rollback it",
            &uscf->host);
        cf.pool = sl->pool;
        uscf->servers = old_servers;
        init(&cf, uscf);
        return -1;
    }

#if (NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_http_upstream_check_update_upstream_peers(uscf, cf.pool) !=
            NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
            "upstream-serverlist: update check module upstream %V failed",
            &uscf->host);
    }
#endif

    dump_serverlist(sl);
    return 0;
}

static struct phr_header *
get_header(struct phr_header *headers, size_t num_headers, const char * name) {
    struct phr_header *h = NULL;
    size_t i = 0;

    if (headers == NULL || num_headers <= 0 || name == NULL) {
        return NULL;
    }

    for (i = 0; i < num_headers; i++) {
        h = &headers[i];

        if (h->name == NULL && h->value == NULL) {
            break;
        }

        if (ngx_strncasecmp((u_char *)h->name, (u_char *)name,
            h->name_len) == 0) {
            return h;
        }
    }

    return NULL;
}

static time_t
get_last_modified_time(struct phr_header *headers, size_t num_headers) {
    struct phr_header *h = get_header(headers, num_headers, "Last-Modified");
    if (h == NULL) {
        return (time_t)-1;
    }

    return ngx_http_parse_time((u_char *)h->value, h->value_len);
}

static ngx_str_t
get_etag(struct phr_header *headers, size_t num_headers) {
    ngx_str_t etag = {0};
    struct phr_header *h = get_header(headers, num_headers, "Etag");
    if (h == NULL) {
        return etag;
    }

    etag.data = (u_char *)h->value;
    etag.len = h->value_len;
    return etag;
}

static ngx_int_t
get_content_length(struct phr_header *headers, size_t num_headers) {
    struct phr_header *h = get_header(headers, num_headers, "Content-Length");
    if (h == NULL) {
        return -1;
    }

    return ngx_atoi((u_char *)h->value, h->value_len);
}

static void
recv_from_service(ngx_event_t *ev) {
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    ngx_connection_t *c = ev->data;
    service_conn *sc = c->data;
    serverlist *sl = (serverlist *)mcf->serverlists.elts + sc->serverlists_curr;

    ngx_int_t ret = -1;
    u_char *new_buf = NULL;
    int minor_version = 0, status = 0;
    struct phr_header headers[MAX_HTTP_RECEIVED_HEADERS] = {{0}};
    const char *msg = NULL;
    size_t prev_recv = 0, msglen = 0, bufsize = 0, freesize = 0;
    size_t num_headers = sizeof headers / sizeof headers[0];

    ngx_str_t etag = {0};
    time_t last_modified = -1;
    ngx_int_t content_length = -1;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
        "upstream-serverlist: recv begin cur %d start %d end %d act %d ready %d",
        sc->serverlists_curr, sc->serverlists_start, sc->serverlists_end,
        c->read->active, c->read->ready);

    c->read->ready = 0;

    while (1) {
        freesize = sc->recv.end - sc->recv.last;
        if (freesize <= 0) {
            /* buffer not big enough? enlarge it by twice */
            bufsize = sc->recv.end - sc->recv.start;
            new_buf = ngx_pcalloc(mcf->conf_pool, bufsize * 2);
            if (new_buf == NULL) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: allocate recv buf failed");
                goto close_connection;
            }

            ngx_memcpy(new_buf, sc->recv.start, bufsize);
            sc->recv.pos = sc->recv.start = new_buf;
            sc->recv.last = new_buf + bufsize;
            sc->recv.end = new_buf + bufsize * 2;
            freesize = sc->recv.end - sc->recv.last;
        }

        ret = c->recv(c, sc->recv.last, freesize);
        if (ret > 0) {
            prev_recv = sc->recv.last - sc->recv.start;
            sc->recv.last += ret;

            if (sc->content_length >= 0) {
                sc->body.len += ret;
                if ((int)sc->body.len == sc->content_length) {
                    break;
                } else if ((int)sc->body.len > sc->content_length) {
                    ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                        "upstream-serverlist: serverlist %V body too big",
                        &sl->name, sc->content_length, sc->body.len);
                    goto close_connection;
                }
            }

            ret = phr_parse_response((const char *)sc->recv.start,
                sc->recv.last - sc->recv.start, &minor_version,
                &status, &msg, &msglen, headers, &num_headers, prev_recv);
            if (ret == -1) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: parse http headers of serverlist %V "
                    "error", &sl->name);
                goto close_connection;
            } else if (ret == -2) {
                ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
                    "upstream-serverlist: header incomplete");
                return;
            } else if (ret < 0) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: unknown picohttpparser error in "
                    "serverlist %V", &sl->name);
                goto close_connection;
            } else if (status == 304) {
                // serverlist not modified.
                goto exit;
            } else if (status != 200) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: response of serverlist %V is not "
                    "200: %d", &sl->name, status);
                goto close_connection;
            }

            content_length = get_content_length(headers, num_headers);
            if (content_length < 0) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: serverlist %V need content length",
                    &sl->name);
                goto close_connection;
            }

            sc->content_length = content_length;
            sc->body.data = sc->recv.start + ret;
            sc->body.len = sc->recv.last - sc->body.data;
            if ((int)sc->body.len == sc->content_length) {
                break;
            } else if ((int)sc->body.len > sc->content_length) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: serverlist %V body too big",
                    &sl->name, sc->content_length, sc->body.len);
                goto close_connection;
            }

            ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
                "upstream-serverlist: body incomplete: received %d, content "
                "length %d", (int)sc->body.len, sc->content_length);
            return;
        } else if (ret == 0 || ngx_socket_errno == NGX_ECONNRESET) {
            // remote peer closed, leading 2 results: 1) header incomplete. 2)
            // body incomplete. every result need discard the connection.
            ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
                "upstream-serverlist: connection closed");
            ngx_close_connection(sc->peer_conn.connection);
            sc->peer_conn.connection = NULL;
            ngx_del_timer(&sc->timeout_timer);
            ngx_add_timer(&sc->refresh_timer, 1);
            return;
        } else if (ret == NGX_AGAIN) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: try again");
            // just let epoll call this function again.
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: recv error");
            goto close_connection;
        }
    }

    if (sl->new_pool != NULL) {
        // unlikely, is a critical bug.
        ngx_log_error(NGX_LOG_CRIT, ev->log, 0,
            "upstream-serverlist: new pool of sl %V is existing",
            &sl->name);
        ngx_destroy_pool(sl->new_pool);
        sl->new_pool = NULL;
    }

    sl->new_pool = ngx_create_pool(DEFAULT_SERVERLIST_POOL_SIZE, ev->log);
    if (sl->new_pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: create new pool failed");
        goto close_connection;
    }

    etag = get_etag(headers, num_headers);
    if (etag.len > 0) {
        if (sl->etag.len <= 0 || ngx_strncasecmp(sl->etag.data, etag.data,
                ngx_min(sl->etag.len, etag.len)) != 0) {
            sl->etag.data = ngx_pstrdup(sl->new_pool, &etag);
            if (!sl->etag.data) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: allocate etag data failed");
                goto destroy_new_pool;
            }
            sl->etag.len = etag.len;
        } else {
            ngx_destroy_pool(sl->new_pool);
            sl->new_pool = NULL;
            goto exit;
        }
    } else if (sl->etag.len > 0) {
        ngx_memzero(&sl->etag, sizeof sl->etag);
    }

    last_modified = get_last_modified_time(headers, num_headers);
    if (last_modified >= 0) {
        if (last_modified > sl->last_modified) {
            sl->last_modified = last_modified;
        } else if (etag.len <= 0) {
            ngx_destroy_pool(sl->new_pool);
            sl->new_pool = NULL;
            goto exit;
        }
    } else {
        sl->last_modified = -1;
    }

    ret = refresh_upstream(sl, &sc->body, ev->log);
    if (ret < 0) {
        // ensure force refresh in next round, and clean pointers to new pool.
        sl->last_modified = -1;
        ngx_memzero(&sl->etag, sizeof sl->etag);
        ngx_destroy_pool(sl->new_pool);
        sl->new_pool = NULL;
        goto exit;
    }

    if (sl->pool != NULL) {
        // the pool is NULL at first run.
        ngx_destroy_pool(sl->pool);
    }
    sl->pool = sl->new_pool;
    sl->new_pool = NULL;

exit:
    if (sc->serverlists_curr + 1 >= sc->serverlists_end) {
        ngx_time_t *now = ngx_timeofday();
        ngx_log_error(NGX_LOG_INFO, ev->log, 0,
            "upstream-serverlist: finished refresh serverlists from %d to %d, "
            "elapsed: %dms", sc->serverlists_start, sc->serverlists_end,
            (now->sec - sc->start_time.sec) * 1000 + now->msec
                - sc->start_time.msec);

        sc->serverlists_curr = sc->serverlists_start;
        ngx_memzero(&sc->start_time, sizeof sc->start_time);
        c->write->handler = empty_handler;
        c->read->handler = idle_conn_read_handler;

        ret = ngx_handle_read_event(c->read, 0);
        if (ret < 0) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: handle read event failed");
            goto close_connection;
        }

        ngx_add_timer(&sc->refresh_timer, random_interval_ms());
    } else {
        // recv is over, cleaning.
        sc->serverlists_curr++;
        sc->content_length = -1;
        sc->recv.pos = sc->recv.last = sc->recv.start;
        ngx_memzero(&sc->body, sizeof sc->body);

        ret = ngx_del_event(c->read, NGX_READ_EVENT, 0);
        if (ret < 0) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: del read event failed");
            goto close_connection;
        }

        ret = ngx_handle_write_event(c->write, 0);
        if (ret < 0) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: handle write event failed");
            goto close_connection;
        }
    }

    ngx_del_timer(&sc->timeout_timer);

    ngx_log_error(NGX_LOG_DEBUG, ev->log, 0,
        "upstream-serverlist: recv end cur %d start %d end %d act %d ready %d",
        sc->serverlists_curr, sc->serverlists_start, sc->serverlists_end,
        c->read->active, c->read->ready);
    return;

destroy_new_pool:
    ngx_destroy_pool(sl->new_pool);
    sl->new_pool = NULL;

close_connection:
    ngx_close_connection(sc->peer_conn.connection);
    sc->peer_conn.connection = NULL;
    ngx_del_timer(&sc->timeout_timer);
    ngx_add_timer(&sc->refresh_timer, random_interval_ms());
}

static void
dump_one_upstream(ngx_http_upstream_srv_conf_t *uscf, ngx_buf_t *b) {
    ngx_str_t                       *host;
    ngx_http_upstream_rr_peer_t     *peer = NULL;
    ngx_http_upstream_rr_peers_t    *peers = NULL;

    host = &(uscf->host);

    b->last = ngx_snprintf(b->last, b->end - b->last, "Upstream: %V; ", host);

    if (uscf->peer.data == NULL) {
        b->last = ngx_snprintf(b->last, b->end - b->last,"Servers: 0;\n");
        return;
    }

    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    b->last = ngx_snprintf(b->last, b->end - b->last, "Servers: %d;\n",
                           peers->number);

    for (peer = peers->peer; peer; peer = peer->next) {
        b->last = ngx_snprintf(b->last, b->end - b->last,
            "  server %V weight=%d max_fails=%d fail_timeout=%ds", &peer->name,
            peer->weight, peer->max_fails, peer->fail_timeout);
        if (peer->down) {
            b->last = ngx_snprintf(b->last, b->end - b->last, " down");
        }

        b->last = ngx_snprintf(b->last, b->end - b->last, ";\n");
    }
}

static ngx_int_t
dump_upstreams(ngx_http_request_t *r) {
    ngx_buf_t                                *b;
    ngx_int_t                                 rc, ret;
    ngx_str_t                                *host;
    ngx_uint_t                                i;
    ngx_chain_t                               out;
    ngx_http_upstream_main_conf_t *umcf =
        ngx_http_cycle_get_module_main_conf(ngx_cycle,
            ngx_http_upstream_module);
    main_conf *mcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
        ngx_http_upstream_serverlist_module);
    ngx_http_upstream_srv_conf_t **uscfp = umcf->upstreams.elts;
    serverlist *sl = NULL;
    size_t buf_size = 1048576 * 16;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    ngx_str_set(&r->headers_out.content_type, "text/plain");
    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;
        rc = ngx_http_send_header(r);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    b = ngx_create_temp_buf(r->pool, buf_size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    out.buf = b;
    out.next = NULL;

    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "service_url: http://%V;\n",
                           &mcf->service_url.url);

    for (i = 0; i < mcf->serverlists.nelts; i++) {
        sl = (serverlist *)mcf->serverlists.elts + i;
        b->last = ngx_snprintf(b->last, b->end - b->last,
                               "  upstream %V serverlist is %V;\n",
                               &sl->upstream_conf->host, &sl->name);
    }

    b->last = ngx_snprintf(b->last, b->end - b->last, "\n");

    host = &r->args;
    if (host->len == 0 || host->data == NULL) {
        if (umcf->upstreams.nelts == 0) {
            b->last = ngx_snprintf(b->last, b->end - b->last,
                                   "No upstreams defined");
            goto end;
        }

        for (i = 0; i < umcf->upstreams.nelts; i++) {
            dump_one_upstream(uscfp[i], b);
            b->last = ngx_snprintf(b->last, b->end - b->last, "\n");
        }

        goto end;
    }

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->host.len == host->len &&
            ngx_strncasecmp(uscfp[i]->host.data, host->data, host->len) == 0) {
            dump_one_upstream(uscfp[i], b);
            goto end;
        }
    }

    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "The upstream you requested does not exist, "
                           "Please check the upstream name");

end:
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;
    r->connection->buffered |= NGX_HTTP_WRITE_BUFFERED;
    b->last_buf = (r == r->main) ? 1 : 0;
    ret = ngx_http_send_header(r);
    ret = ngx_http_output_filter(r, &out);
    return ret;
}

static char *
dump_upstreams_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = dump_upstreams;

    return NGX_CONF_OK;
}
