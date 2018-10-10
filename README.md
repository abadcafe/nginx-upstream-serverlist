# nginx-upstream-serverlist

Yet another nginx module for dynamically update upstream server list without
reload or restart, and with simple and fully featured HTTP interface.

Nginx's upstream can not dynamically modify the server directive's arguments. If
someone wants add new servers, change server weight, or deal with other
arguments like max_conns etc, he/she must reload or restart nginx, which may
cause PV loss.

This module add two directives: a) `serverlist`, b) `serverlist_service`, to resolve the problem.

## Installation
The module must compile with nginx >= 1.11.0 .
```sh
cd [nginx source directory]
./configure --add-module=[/path/to/nginx-upstream-serverlist]
make
make install
```

## Usage
One can use the two directives in `http block` and `upstream block` like below:

<pre>
http {
  serverlist_service url=http://127.0.0.1/serverlists/  # or unix socket path.
  upstream test {
    serverlist test;
    server 127.255.255.255 down; # just a trick, explain later.
  }
}
</pre>

Now the module will periodically request to http://127.0.0.1/serverlists/test,
and the url should response nginx upstream `server` directives in plain text,
like below:

<pre>
server 127.0.0.1:80;                 # a plain server
server 127.0.0.2:81 weight=1 backup; # or with arguments
</pre>

Supported `server` directive arguments include:
* weight
* max_conns (only in nginx >= 1.11.5)
* max_fails
* fail_timeout
* down
* backup

NOTE: One can use "Last-Modified" or "Etag" HTTP header in response to prevent
wasted upstream refresh actions, Especially when thousands serverlists and
upstreams configured.

## Directives
### serverlist_service
* Syntax: `serverlist_service url=http://xxx/ [conf_dump_dir=dumped_dir/] [interval=5s] [concurrency=1];`
* Context: `http`

One `http block` can contain only one `serverlist_service` directive.

The `url` argument specified where to request. It must be a HTTP URL, other
protocol is not supported yet.

The `conf_dump_dir` argument specified where responsed valid serverlists dump
to. If `conf_dump_dir` is a relative path, it relative to nginx config
directory. One can include dumped serverlist file in `upstream block` to ensure
upstream has most recent `server` directives even after nginx restarted, to
recover from serverlist service unavailable. Like blow:

<pre>
http {
  serverlist_service url=http://127.0.0.1/serverlists/ conf_dump_dir=dumped_serverlists/
  upstream test {
    serverlist test;

    # nginx needs at least one server in the upstream, otherwise nginx will
    # report error and exit.
    server 127.255.255.255 down;

    # nginx will report error and exit if include a non-existed file. So use
    # wildcard pattern instead plain path.
    include dumped_serverlists/test.conf*;
  }
}
</pre>

The `concurrency` argument specified how many connections per worker process
will use to communicate to serverlist service. Default is 1.

### serverlist
* Syntax: `serverlist [name];`
* Context: `upstream`

One `upstream block` can contain only one `serverlist` directive.

The directive can optionally specify a `name` argument. If the argument absent,
it means use upstream's name as serverlist's name. The URL to fetch `server`
directives for the upstream will be
`http://[serverlist_service's url]/[serverlist's name]`

## Inspired By
### [nginx-upstream-dynamic-servers](https://github.com/GUI/nginx-upstream-dynamic-servers/)
A free dynamic upstream implement depends on DNS, added a `resolve` argument to
`server` directive, just like commercial Nginx Plus does.

### [nginx-upsync-module](https://github.com/weibocom/nginx-upsync-module)
A dynamic upstream implement depends on Etcd or Consul, use Etcd or Consul as
upstream's control plane.
