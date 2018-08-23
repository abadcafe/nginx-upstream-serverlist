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
    ngx_event_t                   connect_timer;
    ngx_event_t                   timeout_timer;

    ngx_peer_connection_t         peer_conn;
    ngx_buf_t                     send;
    ngx_buf_t                     recv;
    ngx_str_t                     body;
} ngx_http_upstream_alias_t;

typedef struct {
    ngx_url_t                     alias_service_url;
    ngx_array_t                   aliases;
    ngx_http_conf_ctx_t          *conf_ctx;
} ngx_http_upstream_alias_main_conf_t;

static void *
create_main_conf(ngx_conf_t *cf);

static char *
merge_server_conf(ngx_conf_t *cf, void *parent, void *child);

static char *
alias_service_url_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy);

static char *
alias_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy);

static char *
dump_upstreams_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t
init_process(ngx_cycle_t *cycle);

static void
exit_process(ngx_cycle_t *cycle);

static void
connect_timeout_clean(ngx_event_t *ev);

static void
connect_to_alias_service(ngx_event_t *ev);

static void
send_request_to_alias_service(ngx_event_t *ev);

static void
recv_response_from_alias_service(ngx_event_t *ev);

static void
empty_handler(ngx_event_t *ev);

static ngx_command_t module_commands[] = {
    {
        ngx_string("alias"),
        NGX_HTTP_UPS_CONF | NGX_CONF_ANY,
        alias_directive,
        0,
        0,
        NULL
    },
    {
        ngx_string("alias_service_url"),
        NGX_HTTP_MAIN_CONF | NGX_CONF_1MORE,
        alias_service_url_directive,
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

ngx_module_t ngx_http_upstream_alias_module = {
    NGX_MODULE_V1,
    &module_ctx,                           /* module context */
    module_commands,                       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    init_process,                          /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    exit_process,                          /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
random_interval() {
    return 2000 + ngx_random() % 500;
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
    ngx_http_upstream_alias_main_conf_t *main_cf;

    main_cf = ngx_pcalloc(cf->pool, sizeof *main_cf);
    if (main_cf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&main_cf->aliases, cf->pool, 1,
                       sizeof(ngx_http_upstream_alias_t)) != NGX_OK) {
        return NULL;
    }

    ngx_memzero(&main_cf->alias_service_url, sizeof main_cf->alias_service_url);
    ngx_str_set(&main_cf->alias_service_url.url, "127.84.10.13/");

    return main_cf;
}

static char *
merge_server_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_upstream_alias_main_conf_t  *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_alias_module);
    ngx_int_t                             ret = -1;

    main_cf->alias_service_url.default_port = 80;
    main_cf->alias_service_url.uri_part = 1;
    ret = ngx_parse_url(cf->pool, &main_cf->alias_service_url);
    if (ret != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "upstream-alias: parse alias_service_url failed: %s",
                           main_cf->alias_service_url.err);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
alias_service_url_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {
    ngx_http_upstream_alias_main_conf_t *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_alias_module);
    ngx_str_t                           *url = (ngx_str_t *)cf->args->elts + 1;

    if (cf->args->nelts < 2) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "upstream-alias: alias_service_url need 1 arg",
                           main_cf->alias_service_url.err);
        return NGX_CONF_ERROR;
    }

    if (url->len <= 7) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "upstream-alias: alias_service_url error",
                           main_cf->alias_service_url.err);
        return NGX_CONF_ERROR;
    }

    if (ngx_strncasecmp(url->data, (u_char *)"http://", 7) != 0) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "upstream-alias: alias_service_url needs http prefix",
                           main_cf->alias_service_url.err);
        return NGX_CONF_ERROR;
    }

    main_cf->alias_service_url.url.data = url->data + 7;
    main_cf->alias_service_url.url.len = url->len - 7;

    return NGX_CONF_OK;
}

static char *
alias_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy) {
    ngx_http_upstream_srv_conf_t        *uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    ngx_http_upstream_alias_main_conf_t *main_cf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_alias_module);
    ngx_http_upstream_alias_t           *alias = NULL;

    if (cf->args->nelts > 2) {
        ngx_conf_log_error(NGX_LOG_ERR, cf, 0,
                           "upstream-alias: alias only need 0 or 1 args",
                           main_cf->alias_service_url.err);
        return NGX_CONF_ERROR;
    }

    alias = ngx_array_push(&main_cf->aliases);
    if (alias == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(alias, sizeof *alias);
    alias->upstream_conf = uscf;
    alias->name = cf->args->nelts <= 1 ? uscf->host :
                                         ((ngx_str_t *)cf->args->elts)[1];

    return NGX_CONF_OK;
}

static ngx_int_t
init_process(ngx_cycle_t *cycle) {
    ngx_http_upstream_alias_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_alias_module);
    ngx_http_upstream_alias_t           *aliases = main_cf->aliases.elts;
    ngx_uint_t i;
    ngx_event_t *connect_timer, *timeout_timer;
    ngx_uint_t refresh_in;

    for (i = 0; i < main_cf->aliases.nelts; i++) {
        connect_timer = &aliases[i].connect_timer;
        connect_timer->handler = connect_to_alias_service;
        connect_timer->log = cycle->log;
        connect_timer->data = &aliases[i];

        timeout_timer = &aliases[i].timeout_timer;
        timeout_timer->handler = connect_timeout_clean;
        timeout_timer->log = cycle->log;
        timeout_timer->data = &aliases[i];

        refresh_in = random_interval();
        ngx_log_debug(NGX_LOG_INFO, cycle->log, 0,
                      "upstream-alias: Initialize refresh alias '%V' in %ims",
                      &aliases[i].name, refresh_in);
        ngx_add_timer(connect_timer, refresh_in);
    }

    return NGX_OK;
}

static void
exit_process(ngx_cycle_t *cycle) {
    ngx_http_upstream_alias_main_conf_t *main_cf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_alias_module);
    ngx_http_upstream_alias_t           *aliases = main_cf->aliases.elts;
    ngx_uint_t i;

    for (i = 0; i < main_cf->aliases.nelts; i++) {
        if (aliases[i].pool) {
            ngx_destroy_pool(aliases[i].pool);
            aliases[i].pool = NULL;
        }
    }
}

static void
connect_timeout_clean(ngx_event_t *ev) {
    ngx_http_upstream_alias_t *alias = ev->data;

    ngx_log_error(NGX_LOG_ERR, ev->log, 0, "upstream-alias: alias %V timeout",
                  &alias->name);

    if (alias->peer_conn.connection != NULL) {
        ngx_close_connection(alias->peer_conn.connection);
        alias->peer_conn.connection = NULL;
    }

    if (alias->new_pool != NULL) {
        ngx_destroy_pool(alias->new_pool);
        alias->new_pool = NULL;
    }

    ngx_add_timer(&alias->connect_timer, random_interval());
}

static void
connect_to_alias_service(ngx_event_t *ev) {
    ngx_int_t                                 ret = -1;
    ngx_connection_t                         *c = NULL;
    ngx_http_upstream_alias_main_conf_t      *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_alias_module);
    ngx_http_upstream_alias_t                *alias = ev->data;

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_debug(NGX_LOG_INFO, ev->log, 0,
                  "upstream-alias: start refresh alias '%V'", &alias->name);

    if (alias->new_pool != NULL) {
        // the new pool may existed while previous refresh procedure failed.
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: new pool of alias %V is existing",
                      &alias->name);
        ngx_destroy_pool(alias->new_pool);
        alias->new_pool = NULL;
    }

    if (alias->peer_conn.connection != NULL) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: connection of alias %V is existing",
                      &alias->name);
        ngx_close_connection(alias->peer_conn.connection);
        alias->peer_conn.connection = NULL;
    }

    ngx_memzero(&alias->peer_conn, sizeof alias->peer_conn);
    ngx_memzero(&alias->send, sizeof alias->send);
    ngx_memzero(&alias->recv, sizeof alias->recv);
    ngx_memzero(&alias->body, sizeof alias->body);
    alias->peer_conn.get = ngx_event_get_peer;
    alias->peer_conn.log = ev->log;
    alias->peer_conn.log_error = NGX_ERROR_ERR;
    alias->peer_conn.cached = 0;
    alias->peer_conn.connection = NULL;
    alias->peer_conn.name = &main_cf->alias_service_url.host;
    alias->peer_conn.sockaddr = &main_cf->alias_service_url.sockaddr.sockaddr;
    alias->peer_conn.socklen = main_cf->alias_service_url.socklen;

    ngx_add_timer(&alias->timeout_timer, 10000);
    ret = ngx_event_connect_peer(&alias->peer_conn);
    if (ret == NGX_ERROR || ret == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: connect to alias_service_url failed: %V",
                      &alias->peer_conn.name);
        goto fail;
    }

    alias->new_pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ev->log);
    if (alias->new_pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: Could not create new pool");
        goto fail;
    }

    c = alias->peer_conn.connection;
    c->data = alias;
    c->log = alias->peer_conn.log;
    c->sendfile = 0;
    c->idle = 1; // for quick exit.
    c->read->log = c->log;
    c->write->log = c->log;
    c->write->handler = send_request_to_alias_service;
    c->read->handler = recv_response_from_alias_service;

    /* The kqueue's loop interface needs it. */
    if (ret == NGX_OK) {
        c->write->handler(c->write);
    }

    return;

fail:
    ngx_close_connection(alias->peer_conn.connection);
    alias->peer_conn.connection = NULL;
    ngx_del_timer(&alias->timeout_timer);
    ngx_add_timer(&alias->connect_timer, random_interval());
}

static void
send_request_to_alias_service(ngx_event_t *ev) {
    ssize_t                                   size;
    ngx_connection_t                         *c = ev->data;
    ngx_http_upstream_alias_t                *alias = c->data;
    ngx_http_upstream_alias_main_conf_t      *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_alias_module);

    if (whole_world_exiting()) {
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "upstream-alias: sending");

    u_char req[ngx_pagesize];
    ngx_memzero(req, sizeof req);
    ngx_sprintf(req, "GET %V?alias=%V HTTP/1.0\r\nHost: %V\r\n\r\n",
                &main_cf->alias_service_url.uri, &alias->name,
                &main_cf->alias_service_url.host);

    alias->send.pos = req;
    alias->send.last = alias->send.pos + ngx_strlen(req);
    while (alias->send.pos < alias->send.last) {
        size = c->send(c, alias->send.pos, alias->send.last - alias->send.pos);
        if (size > 0) {
            alias->send.pos += size;
        } else if (size == 0 || size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0, "upstream-alias: send error");
            goto fail;
        }
    }

    if (alias->send.pos == alias->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "upstream_alias_send: sent");
    }

    c->write->handler = empty_handler;
    return;

fail:
    ngx_close_connection(alias->peer_conn.connection);
    alias->peer_conn.connection = NULL;
    ngx_destroy_pool(alias->new_pool);
    alias->new_pool = NULL;
    ngx_del_timer(&alias->timeout_timer);
    ngx_add_timer(&alias->connect_timer, random_interval());
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
get_alias_servers(ngx_http_upstream_alias_t *alias) {
    ngx_int_t    ret = -1;
    ngx_array_t *servers = ngx_array_create(alias->new_pool, 2,
                                            sizeof(ngx_http_upstream_server_t));
    ngx_http_upstream_server_t *server = NULL;
    ngx_url_t u = {0};
    ngx_str_t curr_line = {0};
    ngx_str_t curr_arg = {0};

    u_char *body_pos = alias->body.data;
    u_char *body_end = alias->body.data + alias->body.len;
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
                    ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                  "upstream-alias: alias %V: expect 'server' prefix",
                                  &alias->name);
                    break;
                }

                first_arg_found = 1;
            } else if (!second_arg_found) {
                ngx_memzero(&u, sizeof u);
                u.url = curr_arg;
                u.no_resolve = 1;
                u.default_port = 80;
                ret = ngx_parse_url(alias->new_pool, &u);
                if (ret != NGX_OK) {
                    ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                  "upstream-alias: alias %V: parse addr failed",
                                  &alias->name);
                    break;
                }

                server = ngx_array_push(servers);
                ngx_memzero(server, sizeof *server);
                server->name = u.url;
                server->naddrs = u.naddrs;
                server->addrs = u.addrs;
                server->weight = 1;
                server->max_conns = 0;
                server->max_fails = 1;
                server->fail_timeout = 10;

                second_arg_found = 1;
            } else {
                if (ngx_strncmp(curr_arg.data, "weight=", 7) == 0) {
                    ret = ngx_atoi(curr_arg.data + 7, curr_arg.len - 7);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                      "upstream-alias: alias %V: weight invalid",
                                      &alias->name);
                        continue;
                    }

                    server->weight = ret;
                } else if (ngx_strncmp(curr_arg.data, "max_conns=", 10) == 0) {
                    ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                      "upstream-alias: alias %V: max_conns invalid",
                                      &alias->name);
                        continue;
                    }

                    server->max_conns = ret;
                } else if (ngx_strncmp(curr_arg.data, "max_fails=", 10) == 0) {
                    ret = ngx_atoi(curr_arg.data + 10, curr_arg.len - 10);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                      "upstream-alias: alias %V: max_fails invalid",
                                      &alias->name);
                        continue;
                    }

                    server->max_fails = ret;
                } else if (ngx_strncmp(curr_arg.data, "fail_timeout=", 13) == 0) {
                    ngx_str_t time_str = {.data = curr_arg.data + 13, .len = curr_arg.len - 13};
                    ret = (ngx_int_t)ngx_parse_time(&time_str, 1);
                    if (ret == NGX_ERROR || ret == 0) {
                        ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                      "upstream-alias: alias %V: fail_timeout invalid",
                                      &alias->name);
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
                    ngx_log_error(NGX_LOG_ERR, alias->connect_timer.log, 0,
                                  "upstream-alias: alias %V: unknown server option %V",
                                  &alias->name, &curr_arg);
                }
            }
        }
    } while (body_pos < body_end);

    return servers;
}

static void
refresh_upstream(ngx_http_upstream_alias_t *alias) {
    ngx_http_upstream_alias_main_conf_t  *main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_alias_module);
    ngx_http_upstream_srv_conf_t         *uscf = alias->upstream_conf;
    ngx_array_t                          *new_servers = NULL;
    ngx_conf_t                            cf = {0};

    if (whole_world_exiting()) {
        return;
    }

    new_servers = get_alias_servers(alias);
    if (new_servers == NULL || new_servers->nelts <= 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upstream-alias: Could not get servers of alias %V",
                      &alias->name);
        goto fail;
    }
    uscf->servers = new_servers;

    ngx_memzero(&cf, sizeof cf);
    cf.name = "alias_init_upstream";
    cf.cycle = (ngx_cycle_t *) ngx_cycle;
    cf.pool = alias->new_pool;
    cf.module_type = NGX_HTTP_MODULE;
    cf.cmd_type = NGX_HTTP_MAIN_CONF;
    cf.log = ngx_cycle->log;
    cf.ctx = main_cf->conf_ctx;

    ngx_http_upstream_init_pt init;
    init = alias->upstream_conf->peer.init_upstream ?
        alias->upstream_conf->peer.init_upstream :
        ngx_http_upstream_init_round_robin;

    if (init(&cf, uscf) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upstream-alias: Error re-initializing upstream");
    }

    if (alias->pool != NULL) {
        ngx_destroy_pool(alias->pool);
    }
    alias->pool = alias->new_pool;
    alias->new_pool = NULL;
    ngx_del_timer(&alias->timeout_timer);
    ngx_add_timer(&alias->connect_timer, random_interval());
    return;

fail:
    ngx_destroy_pool(alias->new_pool);
    alias->new_pool = NULL;
    ngx_del_timer(&alias->timeout_timer);
    ngx_add_timer(&alias->connect_timer, random_interval());
}

static void
recv_response_from_alias_service(ngx_event_t *ev) {
    ngx_int_t                              ret = -1;
    u_char                                *new_buf;
    ssize_t                                size, n;
    ngx_connection_t                      *c = ev->data;
    ngx_http_upstream_alias_t             *alias = c->data;

    int minor_version = 0, status = 0;
    struct phr_header headers[32] = {0};
    const char *msg = NULL;
    size_t msg_len = 0, num_headers = sizeof headers / sizeof headers[0];

    if (whole_world_exiting()) {
        return;
    }

    if (alias->recv.start == NULL) {
        /* 1 of the page_size, is it enough? */
        alias->recv.start = ngx_pcalloc(alias->new_pool, ngx_pagesize);
        if (alias->recv.start == NULL) {
            goto fail;
        }

        alias->recv.last = alias->recv.pos = alias->recv.start;
        alias->recv.end = alias->recv.start + ngx_pagesize;
    }

    while (1) {
        n = alias->recv.end - alias->recv.last;

        /* buffer not big enough? enlarge it by twice */
        if (n == 0) {
            size = alias->recv.end - alias->recv.start;
            new_buf = ngx_pcalloc(alias->new_pool, size * 2);
            if (new_buf == NULL) {
                ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                              "upstream-alias: allocate recv buf failed");
                goto fail;
            }
            ngx_memcpy(new_buf, alias->recv.start, size);

            alias->recv.pos = alias->recv.start = new_buf;
            alias->recv.last = new_buf + size;
            alias->recv.end = new_buf + size * 2;

            n = alias->recv.end - alias->recv.last;
        }

        size = c->recv(c, alias->recv.last, n);
        if (size > 0) {
            alias->recv.last += size;
            continue;
        } else if (size == 0) {
            break;
        } else if (size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            ngx_log_error(NGX_LOG_ERR, ev->log, 0, "upstream-alias: recv error");
            goto fail;
        }
    }

    ret = phr_parse_response((const char *)alias->recv.pos,
                             alias->recv.last - alias->recv.pos,
                             &minor_version, &status, &msg, &msg_len, headers,
                             &num_headers, 0);
    if (ret == -1) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: parse http headers of alias %V error",
                      &alias->name);
        goto fail;
    } else if (ret == -2) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: response of alias %V is incomplete",
                      &alias->name);
        goto fail;
    } else if (ret < 0) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: unknown picohttpparser error");
        goto fail;
    } else if (status != 200) {
        ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                      "upstream-alias: response of alias %V is not 200: %d",
                      status);
        goto fail;
    }
    alias->body.data = (u_char *)alias->recv.pos + ret;
    alias->body.len = alias->recv.last - alias->recv.pos - ret;

    c->read->handler = empty_handler;
    ngx_close_connection(alias->peer_conn.connection);
    alias->peer_conn.connection = NULL;
    refresh_upstream(alias);
    return;

fail:
    ngx_close_connection(alias->peer_conn.connection);
    alias->peer_conn.connection = NULL;
    ngx_destroy_pool(alias->new_pool);
    alias->new_pool = NULL;
    ngx_del_timer(&alias->timeout_timer);
    ngx_add_timer(&alias->connect_timer, random_interval());
}

static void
empty_handler(ngx_event_t *ev) {
    ngx_log_debug(NGX_LOG_DEBUG, ev->log, 0, "upstream-alias: empty handler");
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
    ngx_buf_t                             *b;
    ngx_int_t                              rc, ret;
    ngx_str_t                             *host;
    ngx_uint_t                             i;
    ngx_chain_t                            out;
    ngx_http_upstream_srv_conf_t         **uscfp = NULL;
    ngx_http_upstream_main_conf_t         *umcf;
    ngx_http_upstream_alias_main_conf_t   *main_cf;
    ngx_http_upstream_alias_t             *alias;
    size_t                                 buf_size = NGX_PAGE_SIZE * NGX_PAGE_NUMBER;

    umcf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                               ngx_http_upstream_module);

    main_cf = ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                                  ngx_http_upstream_alias_module);

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
                           "alias_service_url: http://%V;\n",
                           &main_cf->alias_service_url.url);

    for (i = 0; i < main_cf->aliases.nelts; i++) {
        alias = (ngx_http_upstream_alias_t *)main_cf->aliases.elts + i;
        b->last = ngx_snprintf(b->last, b->end - b->last,
                               "  upstream %V alias to %V;\n",
                               &alias->upstream_conf->host, &alias->name);
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
