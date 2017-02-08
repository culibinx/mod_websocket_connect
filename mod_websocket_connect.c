/*
 * Copyright 2017 culibinx@gmail.com
 *
 * Plugin based on apache-websocket, written by self.disconnect
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND!
 *
*/

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "apr_thread_proc.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_portable.h"
#include "apr_version.h"
#include "apr.h"
#include "websocket_plugin.h"

#if APR_HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if APR_HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif


#if APR_HAVE_UNISTD_H
#include <unistd.h>         /* for getpid() */
#endif

#if (APR_MAJOR_VERSION < 1)
#undef apr_socket_create
#define apr_socket_create apr_socket_create_ex
#endif

#if APR_HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#if (APR_MAJOR_VERSION < 2)
#include "apr_support.h"        /* for apr_wait_for_io_or_timeout() */
#endif

#define DEFAULT_SOCKET_TIMEOUT 50000 /* 50ms */
#define DEFAULT_TCP_HOST "127.0.0.1"
#define DEFAULT_TCP_PORT 80
#define DEFAULT_SOCK "/tmp/socket.sock"

#define ARRAY_INIT_SZ               32
#define DEFAULT_RECEIVE_BUFFER_SIZE         4096
#define DEFAULT_RECEIVE_BUFFER_MAX_SIZE     16*1024*1024

#define MILLION_I 1000000
#define MILLION_F 1000000.0

#if !defined(APR_MSG_PEEK) && defined(MSG_PEEK)
#define APR_MSG_PEEK MSG_PEEK
#endif

#if !defined(APACHE_LOG)
#define APACHE_LOG(severity, handle, message...)\
ap_log_error(APLOG_MARK, APLOG_NOERRNO | severity, 0, handle, message)
#endif

module AP_MODULE_DECLARE_DATA websocket_connect_module;

enum connection_type {
    CONN_TCP,
    CONN_UNIX
    //CONN_FD // not used
};

typedef struct _config_rec {
    enum connection_type type;
    const char *tcp_host;
    int tcp_port;
    const char *unix_path;
    struct timeval socket_timeout;
    struct timeval receive_timeout;
    int non_blocking;
    int tcp_no_delay;
    int keep_alive;
    int reuse_address;
    int receive_buffer_max_size;
} config_rec;

typedef struct _ConnectContext {
    const WebSocketServer *server;
    apr_socket_t *socket;
    apr_pool_t *pool;
    apr_thread_t *thread;
    apr_thread_mutex_t *mutex;
    apr_array_header_t *arr;
    config_rec *config;
} ConnectContext;

//#pragma mark - timeval misc

static int64_t misc_timeval_to_int64(struct timeval *tv) {
    int64_t clock = tv->tv_sec;
    clock *= MILLION_I;
    clock += tv->tv_usec;
    return clock;
}

static void misc_int64_to_timeval(struct timeval *tv, int64_t index) {
    tv->tv_usec = index % MILLION_I;
    tv->tv_sec = index / MILLION_I;
}

static void misc_str_to_tv(char* time_str, struct timeval *tv) {
    int i;
    int64_t time;
    double time2;
    char *decimal, intVal[30];
    
    if (time_str) {
        time = 0;
        time2 = 0;
        decimal = strpbrk(time_str, ".");
        
        /* Just get integer */
        for (i = 0; i < strcspn(time_str, ".") + 1; i++)
        intVal[i] = 0;
        
        strncpy(intVal, time_str, strcspn(time_str, "."));
        
        if (intVal[0] != 0) {
            time2 = atof(intVal);
            
            /* convert to micro-seconds */
            time = time2 * MILLION_F;
        }
        
        if (decimal) {
            
            /* conv decimal to usec */
            time2 = atof(decimal) * MILLION_F;
            time += time2;
        }
        
        /* Check for negative num, and if translated correctly */
        if (atof(time_str) < 0)
        time = time * (time < 0 ? 1 : -1);
    }
    
    else
    time = 0;
    
    misc_int64_to_timeval(tv, time);
}

//#pragma mark - connect misc

static int is_socket_connected(apr_socket_t *socket) {
    apr_pollfd_t pfds[1];
    apr_status_t status;
    apr_int32_t  nfds;
    
    pfds[0].reqevents = APR_POLLIN;
    pfds[0].desc_type = APR_POLL_SOCKET;
    pfds[0].desc.s = socket;
    
    do {
        status = apr_poll(&pfds[0], 1, &nfds, 0);
    } while (APR_STATUS_IS_EINTR(status));
    
    if (status == APR_SUCCESS && nfds == 1 &&
        pfds[0].rtnevents == APR_POLLIN) {
        apr_sockaddr_t unused;
        apr_size_t len = 1;
        char buf[1];
        /* The socket might be closed in which case
         * the poll will return POLLIN.
         * If there is no data available the socket
         * is closed.
         */
        status = apr_socket_recvfrom(&unused, socket, APR_MSG_PEEK,
                                     &buf[0], &len);
        if (status == APR_SUCCESS && len)
        return 1;
        else
        return 0;
    }
    else if (APR_STATUS_IS_EAGAIN(status) || APR_STATUS_IS_TIMEUP(status)) {
        return 1;
    }
    return 0;
    
}

#if APR_HAVE_SYS_UN_H
static apr_status_t connect_unix(apr_socket_t *sock, const char *uds_path, apr_pool_t *p) {
    apr_status_t rv;
    apr_os_sock_t rawsock;
    apr_interval_time_t t;
    struct sockaddr_un *sa;
    apr_socklen_t addrlen, pathlen;
    
    rv = apr_os_sock_get(&rawsock, sock);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    
    rv = apr_socket_timeout_get(sock, &t);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    
    pathlen = strlen(uds_path);
    /* copy the UDS path (including NUL) to the sockaddr_un */
    addrlen = APR_OFFSETOF(struct sockaddr_un, sun_path) + pathlen;
    sa = (struct sockaddr_un *)apr_palloc(p, addrlen + 1);
    memcpy(sa->sun_path, uds_path, pathlen + 1);
    sa->sun_family = AF_UNIX;
    
    do {
        rv = connect(rawsock, (struct sockaddr*)sa, addrlen);
    } while (rv == -1 && (rv = errno) == EINTR);
    
    if (rv && rv != EISCONN) {
        if ((rv == EINPROGRESS || rv == EALREADY) && (t > 0))  {
#if APR_MAJOR_VERSION < 2
            rv = apr_wait_for_io_or_timeout(NULL, sock, 0);
#else
            rv = apr_socket_wait(sock, APR_WAIT_WRITE);
#endif
        }
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }
    
    return APR_SUCCESS;
}
#endif

static int socket_set_options(apr_socket_t *sock, config_rec *config) {
    
    apr_status_t rv;
    
    if (sock == NULL) {
        return APR_ENOTIMPL;
    }
    
    if (!config->type && config->tcp_no_delay) {
        rv = apr_socket_opt_set(sock, APR_TCP_NODELAY, 1);
        if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_opt_set(APR_TCP_NODELAY): Failed to set");
            return rv;
        }
    }
    
    if (!config->type && config->keep_alive) {
        rv = apr_socket_opt_set(sock, APR_SO_KEEPALIVE, 1);
        if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_opt_set(APR_SO_KEEPALIVE): Failed to set");
            return rv;
        }
    }
    
    if (config->non_blocking) {
        rv = apr_socket_opt_set(sock, APR_SO_NONBLOCK, 1);
        if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_opt_set(APR_SO_NONBLOCK): Failed to set");
            return rv;
        }
    }
    
    if (config->socket_timeout.tv_sec || config->socket_timeout.tv_usec) {
        rv = apr_socket_timeout_set(sock,misc_timeval_to_int64(&config->socket_timeout));
        if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_timeout_set: Failed to set");
            return rv;
        }
    }
    
    if (config->receive_timeout.tv_sec || config->receive_timeout.tv_usec) {
        apr_os_sock_t rawsock;
        rv = apr_os_sock_get(&rawsock, sock);
        if (rv != APR_SUCCESS) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_receive_timeout_set: Failed to set");
            return rv;
        }
        setsockopt(rawsock, SOL_SOCKET, SO_RCVTIMEO,
                   (char *)&config->receive_timeout,sizeof(struct timeval));
    }
    
    if (!config->type && config->reuse_address) {
        rv = apr_socket_opt_set(sock, APR_SO_REUSEADDR, 1);
        if (rv != APR_SUCCESS && rv != APR_ENOTIMPL) {
            APACHE_LOG(APLOG_ERR, NULL, "apr_socket_opt_set(APR_SO_REUSEADDR): Failed to set");
            return rv;
        }
    }
    
    return APR_SUCCESS;
}

static void close_socket(apr_socket_t *socket, int immediate) {
    if (immediate) {
        apr_socket_opt_set(socket, APR_SO_LINGER, 1);
    } else {
        apr_interval_time_t saved_timeout = 0;
        apr_socket_timeout_get(socket, &saved_timeout);
        if (saved_timeout) {
            apr_socket_timeout_set(socket, 0);
        }
    }
    apr_socket_close(socket);
}

static int connection_shutdown(ConnectContext *cr, int immediate) {
    if (cr && cr->socket != NULL) {
        close_socket(cr->socket, immediate);
        cr->socket = NULL;
    }
    return 1;
}

static apr_socket_t * connect_context(apr_pool_t *pool, config_rec *config) {
    
    apr_status_t rv;
    apr_socket_t *newsock;

    if (config->type == CONN_TCP)
    {
        apr_sockaddr_t *sa;
        
        rv = apr_sockaddr_info_get(&sa, config->tcp_host, APR_INET, config->tcp_port, 0, pool);
        if (rv != APR_SUCCESS) {
            APACHE_LOG(APLOG_ERR, NULL,
                       "error geting TCP addrinfo socket for "
                       "%s%i", config->tcp_host, config->tcp_port);
            newsock = NULL;
            return NULL;
        }
        
        if (!config->tcp_port) {
            rv = apr_getservbyname(sa, config->tcp_host);
            if (rv != APR_SUCCESS) {
                APACHE_LOG(APLOG_ERR, NULL,
                           "error geting port TCP socket for "
                           "%s", config->tcp_host);
                newsock = NULL;
                return NULL;
            }
        }
        
        rv = apr_socket_create(&newsock, sa->family, SOCK_STREAM, APR_PROTO_TCP, pool);
        if (rv != APR_SUCCESS) {
            APACHE_LOG(APLOG_ERR, NULL,
                       "error creating TCP domain socket for "
                       "target %s", config->unix_path);
            newsock = NULL;
            return NULL;
        }
        
        if (socket_set_options(newsock, config) != APR_SUCCESS) {
            newsock = NULL;
            return NULL;
        }
        
        rv = apr_socket_connect(newsock, sa);
        if (rv != APR_SUCCESS) {
            apr_socket_close(newsock);
            APACHE_LOG(APLOG_ERR, NULL,
                       "attempt to connect to TCP domain socket "
                       "%s:%i failed", config->tcp_host, config->tcp_port);
            newsock = NULL;
            return NULL;
        }
        
        if (socket_set_options(newsock, config) != APR_SUCCESS) {
            apr_socket_close(newsock);
            newsock = NULL;
            return NULL;
        }
        
    }
    else if (config->type == CONN_UNIX)
    {
#if APR_HAVE_SYS_UN_H
        rv = apr_socket_create(&newsock, AF_UNIX, SOCK_STREAM, 0, pool);
        if (rv != APR_SUCCESS) {
            APACHE_LOG(APLOG_ERR, NULL,
                       "error creating Unix domain socket for "
                       "target %s", config->unix_path);
            newsock = NULL;
            return NULL;
        }
        
        if (socket_set_options(newsock, config) != APR_SUCCESS) {
            newsock = NULL;
            return NULL;
        }
        
        rv = connect_unix(newsock, config->unix_path, pool);
        if (rv != APR_SUCCESS) {
            apr_socket_close(newsock);
            APACHE_LOG(APLOG_ERR, NULL,
                       "attempt to connect to Unix domain socket "
                       "%s failed", config->unix_path);
            newsock = NULL;
            return NULL;
        }
        
        if (socket_set_options(newsock, config) != APR_SUCCESS) {
            apr_socket_close(newsock);
            newsock = NULL;
            return NULL;
        }
        
#else
        APACHE_LOG(APLOG_ERR, NULL,
                   "connection with Unix domain socket not supported");
#endif
    }
    
    return newsock;
}

//#pragma mark - callback functions

void * APR_THREAD_FUNC websocket_connect_run(apr_thread_t *thread, void *data) {
    return NULL;
}

void * CALLBACK websocket_connect_on_connect(const WebSocketServer *server) {
    ConnectContext *cr = NULL;
    
    if ((server != NULL) && (server->version == WEBSOCKET_SERVER_VERSION_1)) {
        
        request_rec *r = server->request(server);
        if (r != NULL) {
            
            apr_pool_t *pool = NULL;
            
            /* Create new pool */
            if ((apr_pool_create(&pool, r->pool) == APR_SUCCESS)) {
                
                /* Allocate data */
                if ((cr = (ConnectContext *) apr_palloc(pool, sizeof(ConnectContext))) != NULL) {
                    
                    apr_thread_t *thread = NULL;
                    apr_threadattr_t *thread_attr = NULL;
                    apr_thread_mutex_t *mutex = NULL;
                    apr_array_header_t *arr = NULL;
                    
                    cr->server = server;
                    cr->pool = pool;
                    cr->thread = NULL;
                    cr->config = (config_rec *) ap_get_module_config(r->per_dir_config,
                                                                     &websocket_connect_module);
                    if (cr->config 
                        && ((arr = apr_array_make(pool, ARRAY_INIT_SZ, sizeof(char*))) != NULL)
                        && (apr_thread_mutex_create(&mutex, APR_THREAD_MUTEX_DEFAULT, pool) == APR_SUCCESS)
                        && (apr_threadattr_create(&thread_attr, pool) == APR_SUCCESS)
                        && (apr_threadattr_detach_set(thread_attr, 0) == APR_SUCCESS)
                        && (apr_thread_create(&thread, thread_attr, websocket_connect_run, cr, pool) == APR_SUCCESS)
                        ) {
                        /* Success */
                        cr->thread = thread;
                        cr->mutex = mutex;
                        cr->arr = arr;
                        pool = NULL;
                    } else {
                        /* Error */
                        /* Quit context */
                        cr = NULL;
                        APACHE_LOG(APLOG_ERR, r->server, "module is not started");
                    }
                } else {
                    APACHE_LOG(APLOG_ERR, r->server, "module is not started");
                }
                if (pool != NULL) {
                    apr_pool_destroy(pool);
                }
            }
        }
    }
    return cr;
}

void CALLBACK websocket_connect_on_disconnect(void *plugin_private, const WebSocketServer *server) {
    ConnectContext *cr = (ConnectContext *) plugin_private;
    if (cr != 0) {
        if (cr->thread) {
            apr_status_t status;
            connection_shutdown(cr, 0);
            status = apr_thread_join(&status, cr->thread);
        }
        /* Quit context */
        cr = NULL;
    }
}

//#pragma mark - on message callback

static apr_size_t CALLBACK websocket_connect_on_message(void *plugin_private, const WebSocketServer *server,
                                                   const int type, unsigned char *buffer, const apr_size_t buffer_size)
{
    ConnectContext *cr = (ConnectContext *) plugin_private;
    apr_thread_mutex_lock(cr->mutex);

    /* check context */
    if (!cr || !cr->config) {

        /* invalid config - send close to client */
        server->send(server, MESSAGE_TYPE_TEXT, (unsigned char *)"ERRCONFIG", 10);
        server->send(server, MESSAGE_TYPE_CLOSE, NULL, 0);

        APACHE_LOG(APLOG_WARNING, NULL, "ERRCONFIG: problem with config");
        
        apr_thread_mutex_unlock(cr->mutex);
        return 0;
    }

    /* check destination socket */
    if ((cr->socket != NULL && is_socket_connected(cr->socket)) ||
        (connection_shutdown(cr, 1) && (cr->socket = connect_context(cr->pool, cr->config)) != NULL)) {
        
        apr_socket_t *socket = cr->socket;
        /* execution block */
        {
            
            int receive_buffer_max_size = cr->config->receive_buffer_max_size;
            apr_size_t readed = 0;
            apr_size_t writen = 0;
            apr_size_t length = 0;
            apr_status_t e = APR_SUCCESS;

            /* sending */
            writen = buffer_size;
            e = apr_socket_send(socket, (char*)buffer, &writen);
            if (e != APR_SUCCESS || writen < buffer_size) {
                if (APR_STATUS_IS_EAGAIN(e)) {
                    server->send(server, MESSAGE_TYPE_TEXT, (unsigned char*)"ERREAGAIN", 10);
                    APACHE_LOG(APLOG_WARNING, NULL, "ERREAGAIN: Try later");
                } else {
                    if (writen < buffer_size) {
                        server->send(server, MESSAGE_TYPE_TEXT, (unsigned char*)"ERRSENDSIZE", 12);
                        APACHE_LOG(APLOG_WARNING, NULL, "ERRSENDSIZE: Sending size error");
                    }
                    else {
                        server->send(server, MESSAGE_TYPE_TEXT, (unsigned char*)"ERRSEND", 8);
                        APACHE_LOG(APLOG_WARNING, NULL, "ERRSEND: Sending error");
                    }
                }
            }
            else {
                
                /* receiving */
                char recbuf[DEFAULT_RECEIVE_BUFFER_SIZE];
                length = sizeof(recbuf);
                while (readed <= receive_buffer_max_size) {
                    memset(recbuf, 0, DEFAULT_RECEIVE_BUFFER_SIZE);
                    e = apr_socket_recv(socket, recbuf, &length);
                    if (e != APR_EOF && length) {
                        *(char**)apr_array_push(cr->arr) = apr_pstrdup(cr->pool, recbuf);
                        readed += length;
                    } else {
                        break;
                    }
                }
                
                /* check */
                if (!readed) {
                    server->send(server, MESSAGE_TYPE_TEXT, (unsigned char*)"ERRNOREPLY", 11);
                    APACHE_LOG(APLOG_WARNING, NULL, "ERRNOREPLY: Receive empty reply");
                }
                else if (readed > receive_buffer_max_size) {
                    server->send(server, MESSAGE_TYPE_TEXT, (unsigned char*)"ERRMAXREPLY", 12);
                    APACHE_LOG(APLOG_WARNING, NULL, "ERRMAXREPLY: Receive max buffer reply");
                }
                else {
                    // on success
                    server->send(server, MESSAGE_TYPE_BINARY,
                                 (unsigned char *)apr_array_pstrcat(cr->pool, cr->arr, '\0'), readed);
                }
            }
            
            /* clean receiving buffer */
            apr_array_clear(cr->arr);
        }
        
    } else {
        
        /* invalid destination socket - send close to client */
        server->send(server, MESSAGE_TYPE_TEXT, (unsigned char *)"ERRCONNECTION", 14);
        server->send(server, MESSAGE_TYPE_CLOSE, NULL, 0);

        APACHE_LOG(APLOG_WARNING, NULL, "ERRCONNECTION: problem with connection");
    }

    apr_thread_mutex_unlock(cr->mutex);
    return 0;
}

//#pragma mark - settings

static WebSocketPlugin s_plugin = {
    sizeof(WebSocketPlugin),
    WEBSOCKET_PLUGIN_VERSION_0,
    NULL, /* destroy */
    websocket_connect_on_connect,
    websocket_connect_on_message,
    websocket_connect_on_disconnect
};

extern EXPORT WebSocketPlugin * CALLBACK websocket_connect_init()
{
    return &s_plugin;
}

static int websocket_connect_handler(request_rec * r)
{
    return DECLINED;
}

static void websocket_connect_register_hooks(apr_pool_t *p)
{
    ap_hook_handler(websocket_connect_handler, NULL, NULL, APR_HOOK_LAST);
}

//#pragma mark - set configuration parameters

static const char *set_connection_type(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->type = atoi(arg) ? CONN_UNIX : CONN_TCP;
    return NULL;
}

static const char *set_unix_path(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->unix_path = arg != NULL ? arg : "";
    return NULL;
}

static const char *set_tcp_host(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->tcp_host = arg != NULL ? arg : "";
    return NULL;
}

static const char *set_tcp_port(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    int port = atoi(arg);
    if (port > 0 && port <= 65535) {
        config->tcp_port = port;
    }
    return NULL;
}

static const char *set_socket_timeout(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    struct timeval timeout;
    misc_str_to_tv((char*)arg, &timeout);
    if (misc_timeval_to_int64(&timeout)) {
        config->socket_timeout = timeout;
    }
    return NULL;
}

static const char *set_receive_timeout(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    struct timeval timeout;
    misc_str_to_tv((char*)arg, &timeout);
    config->receive_timeout = timeout;
    return NULL;
}

static const char *set_tcp_no_delay(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->tcp_no_delay = atoi(arg) ? 1 : 0;
    return NULL;
}

static const char *set_keep_alive(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->keep_alive = atoi(arg) ? 1 : 0;
    return NULL;
}

static const char *set_non_blocking(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->non_blocking = atoi(arg) ? 1 : 0;
    return NULL;
}

static const char *set_reuse_address(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    config->reuse_address = atoi(arg) ? 1 : 0;
    return NULL;
}

static const char *set_receive_buffer_max_size(cmd_parms *cmd, void *cfg, const char *arg)
{
    config_rec *config = (config_rec *)cfg;
    int receive_buffer_max_size = atoi(arg);
    config->receive_buffer_max_size = receive_buffer_max_size ? receive_buffer_max_size : DEFAULT_RECEIVE_BUFFER_MAX_SIZE;
    return NULL;
}

static const command_rec websocket_connect_cmds[] =
{
    AP_INIT_TAKE1("ConnConnectionType", set_connection_type, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnUnixPath", set_unix_path, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnTcpHost", set_tcp_host, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnTcpPort", set_tcp_port, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnSocketTimeout", set_socket_timeout, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnReceiveTimeout", set_receive_timeout, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnTcpNoDelay", set_tcp_no_delay, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnKeepAlive", set_keep_alive, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnNonBlocking", set_non_blocking, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnReuseAddress", set_reuse_address, NULL, OR_AUTHCFG,
                  ""),
    AP_INIT_TAKE1("ConnMaxBufferSize", set_receive_buffer_max_size, NULL, OR_AUTHCFG,
                  ""),
    { NULL }
    //ACCESS_CONF | OR_INDEXES
};

//#pragma mark - default configuration parameters

static void *websocket_connect_create_dir_config(apr_pool_t * p, char *path)
{
    config_rec *conf = NULL;
    
    if (path != NULL) {
        conf = apr_pcalloc(p, sizeof(config_rec));
        if (conf != NULL) {
            conf->type = CONN_TCP;
            conf->tcp_host = DEFAULT_TCP_HOST;
            conf->tcp_port = DEFAULT_TCP_PORT;
            conf->unix_path = DEFAULT_SOCK;
            conf->receive_buffer_max_size = DEFAULT_RECEIVE_BUFFER_MAX_SIZE;
            
            conf->tcp_no_delay = 0;
            conf->keep_alive = 0;
            conf->non_blocking = 0;
            conf->reuse_address = 0;
            
            struct timeval socket_timeout;
            socket_timeout.tv_sec = 0;
            socket_timeout.tv_usec = DEFAULT_SOCKET_TIMEOUT;
            conf->socket_timeout = socket_timeout;
            
            struct timeval receive_timeout;
            receive_timeout.tv_sec = 0;
            receive_timeout.tv_usec = 0;
            conf->receive_timeout = receive_timeout;
            
        }
    }
    return (void *) conf;
}


/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA websocket_connect_module = {
    STANDARD20_MODULE_STUFF,
    websocket_connect_create_dir_config,  /* create per-dir    config structures */
    NULL,                                 /* merge  per-dir    config structures */
    NULL,                                 /* create per-server config structures */
    NULL,                                 /* merge  per-server config structures */
    websocket_connect_cmds,               /* table of config file commands       */
    websocket_connect_register_hooks      /* register hooks                      */
};

