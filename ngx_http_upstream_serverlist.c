#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include "picohttpparser.h"

#define NGX_PAGE_SIZE 4 * 1024
#define NGX_PAGE_NUMBER 1024

typedef struct {
    ngx_pool_t                   *new_pool;
    ngx_pool_t                   *pool;
    ngx_http_upstream_srv_conf_t *upstream_conf;
    ngx_str_t                     name;
    ngx_event_t                   refresh_timer;
    ngx_event_t                   timeout_timer;

    ngx_peer_connection_t         peer_conn;
    ngx_buf_t                     send;
    ngx_buf_t                     recv;
    ngx_str_t                     body;
    ngx_shmtx_t                   dump_file_lock; // to avoid parrallel write.
} ngx_http_upstream_serverlist_t;

typedef struct {
    ngx_url_t                     service_url;
    ngx_str_t                     conf_dump_dir;
    ngx_array_t                   serverlists;
    ngx_http_conf_ctx_t          *conf_ctx;
} ngx_http_upstream_serverlist_main_conf_t;

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
refresh_timeout_clean(ngx_event_t *ev);

static void
connect_to_service(ngx_event_t *ev);

static void
send_to_service(ngx_event_t *ev);

static void
recv_from_service(ngx_event_t *ev);

static void
empty_handler(ngx_event_t *ev);

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

static ngx_int_t refresh_interval_ms = 5000;

static ngx_int_t
random_interval() {
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
    ngx_http_upstream_serverlist_main_conf_t *main_cf;

    main_cf = ngx_pcalloc(cf->pool, sizeof *main_cf);
    if (main_cf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&main_cf->serverlists, cf->pool, 1,
                       sizeof(ngx_http_upstream_serverlist_t)) != NGX_OK) {
        return NULL;
    }

    ngx_memzero(&main_cf->service_url, sizeof main_cf->service_url);
    ngx_str_set(&main_cf->service_url.url, "127.84.10.13/");
    ngx_memzero(&main_cf->conf_dump_dir, sizeof main_cf->conf_dump_dir);

    return main_cf;
}

static char *
merge_server_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_serverlist_module);
    ngx_int_t                                 ret = -1;
    u_char                                    conf_dump_dir[512] = {0};
    struct stat                               statbuf = {0};
    main_cf->service_url.default_port = 80;
    main_cf->service_url.uri_part = 1;
    ret = ngx_parse_url(cf->pool, &main_cf->service_url);
    if (ret != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: parse service url failed: %s",
            main_cf->service_url.err);
        return NGX_CONF_ERROR;
    }

    if (main_cf->conf_dump_dir.len > 0) {
        ngx_cpystrn(conf_dump_dir, main_cf->conf_dump_dir.data,
            main_cf->conf_dump_dir.len);
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

static char *
serverlist_service_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_serverlist_module);
    ngx_str_t                                *s = NULL;
    ngx_uint_t                                i;

    if (cf->args->nelts <= 1) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: serverlist_service need at least 1 arg",
            main_cf->service_url.err);
        return NGX_CONF_ERROR;
    }

    for (i = 1; i < cf->args->nelts; i++) {
        s = (ngx_str_t *)cf->args->elts + i;

        if (s->len > 4 && ngx_strncmp(s->data, "url=", 4) == 0) {
            if (s->len > 4 + 7 && ngx_strncmp(s->data + 4, "http://", 7) != 0) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: serverlist_service only support http url");
                return NGX_CONF_ERROR;
            }

            main_cf->service_url.url.data = s->data + 4 + 7;
            main_cf->service_url.url.len = s->len - 4 - 7;
        } else if (s->len > 14 && ngx_strncmp(s->data, "conf_dump_dir=", 14) == 0) {
            main_cf->conf_dump_dir.data = s->data + 14;
            main_cf->conf_dump_dir.len = s->len - 14;
            if (ngx_conf_full_name(cf->cycle, &main_cf->conf_dump_dir, 1) != NGX_OK) {
                ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                    "upstream-serverlist: get full path of conf_dump_dir failed");
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
    ngx_http_upstream_srv_conf_t             *uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_serverlist_t           *serverlist = NULL;

    if (cf->args->nelts > 2) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
            "upstream-serverlist: serverlist only need 0 or 1 args",
            main_cf->service_url.err);
        return NGX_CONF_ERROR;
    }

    serverlist = ngx_array_push(&main_cf->serverlists);
    if (serverlist == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(serverlist, sizeof *serverlist);
    serverlist->upstream_conf = uscf;
    serverlist->name = cf->args->nelts <= 1 ? uscf->host :
        ((ngx_str_t *)cf->args->elts)[1];

    return NGX_CONF_OK;
}

static ngx_int_t
init_module(ngx_cycle_t *cycle) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_serverlist_t           *sl = main_cf->serverlists.elts;
    ngx_shm_t shm = {0};
    ngx_uint_t i = 0, cache_line_size = 128;
    ngx_int_t ret = -1;

#if !(NGX_HAVE_ATOMIC_OPS)
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
        "upstream-serverlist: this module need ATOMIC_OPS support!!!");
    return NGX_ERROR;
#endif

    // align to cache line to avoid false sharing.
    shm.size = cache_line_size * main_cf->serverlists.nelts;
    shm.log = cycle->log;
    ngx_str_set(&shm.name, "upstream-serverlist-shared-zone");
    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < main_cf->serverlists.nelts; i++) {
        sl = (ngx_http_upstream_serverlist_t *)main_cf->serverlists.elts + i;
        ret = ngx_shmtx_create(&sl->dump_file_lock,
            (ngx_shmtx_sh_t *)(shm.addr + cache_line_size * i), NULL);
        if ( ret != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
init_process(ngx_cycle_t *cycle) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_serverlist_t           *serverlists = main_cf->serverlists.elts;
    ngx_uint_t i;
    ngx_event_t *refresh_timer = NULL, *timeout_timer = NULL;

    for (i = 0; i < main_cf->serverlists.nelts; i++) {
        refresh_timer = &serverlists[i].refresh_timer;
        refresh_timer->handler = connect_to_service;
        refresh_timer->log = cycle->log;
        refresh_timer->data = &serverlists[i];

        timeout_timer = &serverlists[i].timeout_timer;
        timeout_timer->handler = refresh_timeout_clean;
        timeout_timer->log = cycle->log;
        timeout_timer->data = &serverlists[i];

        ngx_log_error(NGX_LOG_INFO, cycle->log, 0,
            "upstream-serverlist: add connect timer for serverlist %V",
            &serverlists[i].name);
        ngx_add_timer(refresh_timer, random_interval());
    }

    return NGX_OK;
}

static void
exit_process(ngx_cycle_t *cycle) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_serverlist_t           *serverlists = main_cf->serverlists.elts;
    ngx_uint_t i;

    for (i = 0; i < main_cf->serverlists.nelts; i++) {
        if (serverlists[i].pool) {
            ngx_destroy_pool(serverlists[i].pool);
            serverlists[i].pool = NULL;
        }

        if (serverlists[i].peer_conn.connection) {
            ngx_close_connection(serverlists[i].peer_conn.connection);
            serverlists[i].peer_conn.connection = NULL;
        }
    }
}

static void
refresh_timeout_clean(ngx_event_t *ev) {
    ngx_http_upstream_serverlist_t *serverlist = ev->data;

    ngx_log_error(NGX_LOG_ERR, ev->log, 0,
        "upstream-serverlist: serverlist %V refresh timeout",
        &serverlist->name);

    if (serverlist->peer_conn.connection != NULL) {
        ngx_close_connection(serverlist->peer_conn.connection);
        serverlist->peer_conn.connection = NULL;
    }

    if (serverlist->new_pool != NULL) {
        ngx_destroy_pool(serverlist->new_pool);
        serverlist->new_pool = NULL;
    }

    ngx_add_timer(&serverlist->refresh_timer, random_interval());
}

static void
connect_to_service(ngx_event_t *ev) {
    ngx_int_t                                 ret = -1;
    ngx_connection_t                         *c = NULL;
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_serverlist_t           *serverlist = ev->data;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_debug(NGX_LOG_DEBUG_ALL, ev->log, 0,
        "upstream-serverlist: start refresh serverlist %V", &serverlist->name);

    if (serverlist->new_pool != NULL) {
        // unlikely, must be a critical bug if reached here.
        ngx_log_error(NGX_LOG_CRIT, ev->log, 0,
            "upstream-serverlist: new pool of serverlist %V is existing",
            &serverlist->name);
        ngx_destroy_pool(serverlist->new_pool);
        serverlist->new_pool = NULL;
    }

    if (serverlist->peer_conn.connection != NULL) {
        // unlikely and critical too.
        ngx_log_error(NGX_LOG_CRIT, ev->log, 0,
            "upstream-serverlist: connection of serverlist %V is existing",
            &serverlist->name);
        ngx_close_connection(serverlist->peer_conn.connection);
        serverlist->peer_conn.connection = NULL;
    }

    ngx_memzero(&serverlist->peer_conn, sizeof serverlist->peer_conn);
    ngx_memzero(&serverlist->send, sizeof serverlist->send);
    ngx_memzero(&serverlist->recv, sizeof serverlist->recv);
    ngx_memzero(&serverlist->body, sizeof serverlist->body);
    serverlist->peer_conn.get = ngx_event_get_peer;
    serverlist->peer_conn.log = ev->log;
    serverlist->peer_conn.log_error = NGX_ERROR_ERR;
    serverlist->peer_conn.cached = 0;
    serverlist->peer_conn.connection = NULL;
    serverlist->peer_conn.name = &main_cf->service_url.host;
    serverlist->peer_conn.sockaddr = &main_cf->service_url.sockaddr.sockaddr;
    serverlist->peer_conn.socklen = main_cf->service_url.socklen;

    ngx_add_timer(&serverlist->timeout_timer, 10000);
    ret = ngx_event_connect_peer(&serverlist->peer_conn);
    if (ret == NGX_ERROR || ret == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: connect to service_url failed: %V",
            &serverlist->peer_conn.name);
        goto fail;
    }

    serverlist->new_pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ev->log);
    if (serverlist->new_pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: create new pool failed");
        goto fail;
    }

    c = serverlist->peer_conn.connection;
    c->data = serverlist;
    c->log = serverlist->peer_conn.log;
    c->sendfile = 0;
    c->idle = 1; // for quick exit.
    c->read->log = c->log;
    c->write->log = c->log;
    c->write->handler = send_to_service;
    c->read->handler = recv_from_service;

    /* The kqueue's loop interface needs it. */
    if (ret == NGX_OK) {
        c->write->handler(c->write);
    }

    return;

fail:
    ngx_close_connection(serverlist->peer_conn.connection);
    serverlist->peer_conn.connection = NULL;
    ngx_del_timer(&serverlist->timeout_timer);
    ngx_add_timer(&serverlist->refresh_timer, random_interval());
}

static void
send_to_service(ngx_event_t *ev) {
    ssize_t                                   size;
    ngx_connection_t                         *c = ev->data;
    ngx_http_upstream_serverlist_t           *serverlist = c->data;
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_serverlist_module);

    if (whole_world_exiting()) {
        return;
    }

    u_char req[ngx_pagesize];
    ngx_memzero(req, sizeof req);
    ngx_sprintf(req, "GET %V?serverlist=%V HTTP/1.0\r\nHost: %V\r\n\r\n",
        &main_cf->service_url.uri, &serverlist->name,
        &main_cf->service_url.host);

    serverlist->send.pos = req;
    serverlist->send.last = serverlist->send.pos + ngx_strlen(req);
    while (serverlist->send.pos < serverlist->send.last) {
        size = c->send(c, serverlist->send.pos,
            serverlist->send.last - serverlist->send.pos);
        if (size > 0) {
            serverlist->send.pos += size;
        } else if (size == 0 || size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: send error");
            goto fail;
        }
    }

    c->write->handler = empty_handler;
    return;

fail:
    ngx_close_connection(serverlist->peer_conn.connection);
    serverlist->peer_conn.connection = NULL;
    ngx_destroy_pool(serverlist->new_pool);
    serverlist->new_pool = NULL;
    ngx_del_timer(&serverlist->timeout_timer);
    ngx_add_timer(&serverlist->refresh_timer, random_interval());
}

static u_char *
get_one_arg(u_char *buf, u_char *buf_end, ngx_str_t *arg) {
    u_char *pos = NULL, *arg_end = NULL;

    for (pos = buf; pos < buf_end; pos++) {
        if (isalnum(*pos) ||
            *pos == '=' || *pos == '.' || *pos == '-' || *pos == '_') {
            break;
        }
    }

    if (pos >= buf_end) {
        return NULL;
    }

    for (arg_end = pos; arg_end < buf_end; arg_end++) {
        if (!isalnum(*arg_end) &&
            *arg_end != '=' && *arg_end != '.' && *arg_end != '-' && *arg_end != '_') {
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
get_servers(ngx_http_upstream_serverlist_t *serverlist) {
    ngx_int_t    ret = -1;
    ngx_array_t *servers = ngx_array_create(serverlist->new_pool, 2,
                                            sizeof(ngx_http_upstream_server_t));
    ngx_http_upstream_server_t *server = NULL;
    ngx_url_t u = {0};
    ngx_str_t curr_line = {0};
    ngx_str_t curr_arg = {0};

    u_char *body_pos = serverlist->body.data;
    u_char *body_end = serverlist->body.data + serverlist->body.len;
    do {
        ngx_memzero(&curr_line, sizeof curr_line);
        body_pos = get_one_line(body_pos, body_end, &curr_line);
        ngx_int_t first_arg_found = 0;
        ngx_int_t second_arg_found = 0;
        u_char *line_pos = curr_line.data;
        u_char *line_end = curr_line.data + curr_line.len;
        while ((line_pos = get_one_arg(line_pos, line_end, &curr_arg)) != NULL) {
            if (!first_arg_found) {
                if (ngx_strncmp(curr_arg.data, "server", curr_arg.len) != 0) {
                    ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log, 0,
                        "upstream-serverlist: serverlist %V: expect 'server' prefix",
                        &serverlist->name);
                    break;
                }

                first_arg_found = 1;
            } else if (!second_arg_found) {
                ngx_memzero(&u, sizeof u);
                u.url = curr_arg;
                u.no_resolve = 1;
                u.default_port = 80;
                ret = ngx_parse_url(serverlist->new_pool, &u);
                if (ret != NGX_OK) {
                    ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log, 0,
                        "upstream-serverlist: serverlist %V: parse addr failed",
                        &serverlist->name);
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
            } else {
                if (ngx_strncmp(curr_arg.data, "weight=", 7) == 0) {
                    ret = ngx_atoi(curr_arg.data + 7, curr_arg.len - 7);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log,
                            0,
                            "upstream-serverlist: serverlist %V: weight invalid",
                            &serverlist->name);
                        continue;
                    }

                    server->weight = ret;
#if nginx_version >= 1011005
                } else if (ngx_strncmp(curr_arg.data, "max_conns=", 10) == 0) {
                    ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log,
                            0,
                            "upstream-serverlist: serverlist %V: max_conns invalid",
                            &serverlist->name);
                        continue;
                    }

                    server->max_conns = ret;
#endif
                } else if (ngx_strncmp(curr_arg.data, "max_fails=", 10) == 0) {
                    ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log,
                            0,
                            "upstream-serverlist: serverlist %V: max_fails invalid",
                            &serverlist->name);
                        continue;
                    }

                    server->max_fails = ret;
                } else if (ngx_strncmp(curr_arg.data, "fail_timeout=", 13) == 0) {
                    ngx_str_t time_str = {.data = curr_arg.data + 13, .len = curr_arg.len - 13};
                    ret = ngx_parse_time(&time_str, 1);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log,
                            0,
                            "upstream-serverlist: serverlist %V: fail_timeout invalid",
                            &serverlist->name);
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
                    ngx_log_error(NGX_LOG_ERR, serverlist->refresh_timer.log, 0,
                        "upstream-serverlist: serverlist %V: unknown server option %V",
                        &serverlist->name, &curr_arg);
                }
            }
        }
    } while (body_pos < body_end);

    return servers;
}

static ngx_int_t
upstream_servers_changed(const ngx_array_t *old, const ngx_array_t *new) {
    ngx_http_upstream_server_t *s1 = NULL, *s2 = NULL;
    ngx_addr_t *a1 = NULL, *a2 = NULL;
    ngx_uint_t i, j, k, l;

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
                        ngx_memcmp(a1->name.data, a2->name.data, a1->name.len) == 0 &&
                        a1->socklen == a2->socklen &&
                        ngx_memcmp(a1->sockaddr, a2->sockaddr, sizeof *a1->sockaddr) == 0) {
                        break;
                    }
                }

                if (l >= s2->naddrs) {
                    return 1;
                }
            }
        }

        if (j >= new->nelts) {
            return 1;
        }
    }

    return 0;
}

static u_char *
build_server_line(u_char *buf, size_t bufsize, const ngx_http_upstream_server_t *s) {
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
dump_serverlist(ngx_http_upstream_serverlist_t *serverlist) {
    ngx_http_upstream_server_t               *s = NULL;
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_serverlist_module);
    ngx_uint_t i = 0;
    ssize_t ret = 0;
    ngx_fd_t fd = 0;
    u_char buf[512] = {0}, tmpfile[512] = {0}, *p = NULL;

    if (main_cf->conf_dump_dir.len <= 0 ||
        !ngx_shmtx_trylock(&serverlist->dump_file_lock)) {
        return;
    }

    ngx_snprintf(tmpfile, sizeof tmpfile, "%V/.%V.conf.tmp",
        &main_cf->conf_dump_dir, &serverlist->name);
    fd = ngx_open_file(tmpfile, NGX_FILE_WRONLY, NGX_FILE_TRUNCATE,
        NGX_FILE_DEFAULT_ACCESS);
    if (fd < 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
            "upstream-serverlist: open dump file %s failed", tmpfile);
        goto unlock;
    }

    for (i = 0; i < serverlist->upstream_conf->servers->nelts; i++) {
        s = (ngx_http_upstream_server_t *)serverlist->upstream_conf->servers->elts + i;

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
    ngx_snprintf(buf, (sizeof buf) - 1, "%V/%V.conf", &main_cf->conf_dump_dir,
        &serverlist->name);
    ret = ngx_rename_file(tmpfile, buf);
    if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
            "upstream-serverlist: rename dump file %s failed", tmpfile);
        goto unlock;
    }

unlock:
    ngx_shmtx_unlock(&serverlist->dump_file_lock);
}

static void
refresh_upstream(ngx_http_upstream_serverlist_t *serverlist) {
    ngx_http_upstream_serverlist_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_serverlist_module);
    ngx_http_upstream_srv_conf_t             *uscf = serverlist->upstream_conf;
    ngx_array_t                              *new_servers = NULL, *old_servers = NULL;
    ngx_conf_t                                cf = {0};

    if (whole_world_exiting()) {
        return;
    }

    new_servers = get_servers(serverlist);
    if (new_servers == NULL || new_servers->nelts <= 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
            "upstream-serverlist: get serverlist %V failed", &serverlist->name);
        goto fail;
    }

    if (!upstream_servers_changed(uscf->servers, new_servers)) {
        ngx_log_debug(NGX_LOG_DEBUG_ALL, ngx_cycle->log, 0,
            "upstream-serverlist: serverlist %V nothing changed",
            &serverlist->name);
        goto fail;
    }

    ngx_http_upstream_init_pt init;
    init = serverlist->upstream_conf->peer.init_upstream ?
        serverlist->upstream_conf->peer.init_upstream :
        ngx_http_upstream_init_round_robin;

    ngx_memzero(&cf, sizeof cf);
    cf.name = "serverlist_init_upstream";
    cf.cycle = (ngx_cycle_t *) ngx_cycle;
    cf.pool = serverlist->new_pool;
    cf.module_type = NGX_HTTP_MODULE;
    cf.cmd_type = NGX_HTTP_MAIN_CONF;
    cf.log = ngx_cycle->log;
    cf.ctx = main_cf->conf_ctx;

    old_servers = uscf->servers;
    uscf->servers = new_servers;

    if (init(&cf, uscf) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
            "upstream-serverlist: refresh upstream %V failed, rollback it",
            &uscf->host);
        cf.pool = serverlist->pool;
        uscf->servers = old_servers;
        init(&cf, uscf);
        goto fail;
    }

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
        "upstream-serverlist: upstream %V refreshed", &serverlist->name);
    dump_serverlist(serverlist);

    if (serverlist->pool != NULL) {
        ngx_destroy_pool(serverlist->pool);
    }
    serverlist->pool = serverlist->new_pool;
    serverlist->new_pool = NULL;

    ngx_del_timer(&serverlist->timeout_timer);
    ngx_add_timer(&serverlist->refresh_timer, random_interval());
    return;

fail:
    ngx_destroy_pool(serverlist->new_pool);
    serverlist->new_pool = NULL;
    ngx_del_timer(&serverlist->timeout_timer);
    ngx_add_timer(&serverlist->refresh_timer, random_interval());
}

static void
recv_from_service(ngx_event_t *ev) {
    ngx_int_t                       ret = -1;
    u_char                         *new_buf;
    ssize_t                         size, n;
    ngx_connection_t               *c = ev->data;
    ngx_http_upstream_serverlist_t *serverlist = c->data;

    int minor_version = 0, status = 0;
    struct phr_header headers[32] = {0};
    const char *msg = NULL;
    size_t msg_len = 0, num_headers = sizeof headers / sizeof headers[0];

    if (whole_world_exiting()) {
        return;
    }

    if (serverlist->recv.start == NULL) {
        /* 1 of the page_size, is it enough? */
        serverlist->recv.start = ngx_pcalloc(serverlist->new_pool, ngx_pagesize);
        if (serverlist->recv.start == NULL) {
            goto fail;
        }

        serverlist->recv.last = serverlist->recv.pos = serverlist->recv.start;
        serverlist->recv.end = serverlist->recv.start + ngx_pagesize;
    }

    while (1) {
        n = serverlist->recv.end - serverlist->recv.last;

        /* buffer not big enough? enlarge it by twice */
        if (n == 0) {
            size = serverlist->recv.end - serverlist->recv.start;
            new_buf = ngx_pcalloc(serverlist->new_pool, size * 2);
            if (new_buf == NULL) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                    "upstream-serverlist: allocate recv buf failed");
                goto fail;
            }
            ngx_memcpy(new_buf, serverlist->recv.start, size);

            serverlist->recv.pos = serverlist->recv.start = new_buf;
            serverlist->recv.last = new_buf + size;
            serverlist->recv.end = new_buf + size * 2;

            n = serverlist->recv.end - serverlist->recv.last;
        }

        size = c->recv(c, serverlist->recv.last, n);
        if (size > 0) {
            serverlist->recv.last += size;
            continue;
        } else if (size == 0) {
            break;
        } else if (size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "upstream-serverlist: recv error");
            goto fail;
        }
    }

    ret = phr_parse_response((const char *)serverlist->recv.pos,
                             serverlist->recv.last - serverlist->recv.pos,
                             &minor_version, &status, &msg, &msg_len, headers,
                             &num_headers, 0);
    if (ret == -1) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: parse http headers of serverlist %V error",
            &serverlist->name);
        goto fail;
    } else if (ret == -2) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: response of serverlist %V is incomplete",
            &serverlist->name);
        goto fail;
    } else if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: unknown picohttpparser error in serverlist %V",
            &serverlist->name);
        goto fail;
    } else if (status != 200) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
            "upstream-serverlist: response of serverlist %V is not 200: %d",
            &serverlist->name, status);
        goto fail;
    }
    serverlist->body.data = (u_char *)serverlist->recv.pos + ret;
    serverlist->body.len = serverlist->recv.last - serverlist->recv.pos - ret;

    c->read->handler = empty_handler;
    ngx_close_connection(serverlist->peer_conn.connection);
    serverlist->peer_conn.connection = NULL;
    refresh_upstream(serverlist);
    return;

fail:
    ngx_close_connection(serverlist->peer_conn.connection);
    serverlist->peer_conn.connection = NULL;
    ngx_destroy_pool(serverlist->new_pool);
    serverlist->new_pool = NULL;
    ngx_del_timer(&serverlist->timeout_timer);
    ngx_add_timer(&serverlist->refresh_timer, random_interval());
}

static void
empty_handler(ngx_event_t *ev) {
    ngx_log_debug(NGX_LOG_DEBUG_ALL, ev->log, 0,
        "upstream-serverlist: empty handler");
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
                               "  server %V weight=%d max_fails=%d fail_timeout=%ds",
                               &peer->name, peer->weight, peer->max_fails,
                               peer->fail_timeout);
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
    ngx_http_upstream_srv_conf_t            **uscfp = NULL;
    ngx_http_upstream_main_conf_t            *umcf;
    ngx_http_upstream_serverlist_main_conf_t *main_cf;
    ngx_http_upstream_serverlist_t           *serverlist;
    size_t                                    buf_size = NGX_PAGE_SIZE * NGX_PAGE_NUMBER;

    umcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                               ngx_http_upstream_module);

    main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                                  ngx_http_upstream_serverlist_module);

    uscfp = umcf->upstreams.elts;

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
                           &main_cf->service_url.url);

    for (i = 0; i < main_cf->serverlists.nelts; i++) {
        serverlist = (ngx_http_upstream_serverlist_t *)main_cf->serverlists.elts + i;
        b->last = ngx_snprintf(b->last, b->end - b->last,
                               "  upstream %V serverlist to %V;\n",
                               &serverlist->upstream_conf->host, &serverlist->name);
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
