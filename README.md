This is simple project.

#Compile and install:
sudo apxs -c -i mod_websocket.c
sudo apxs -c -i mod_websocket_connect.c

#Simple websocket.conf in apache conf dir:
 # Websocket Connect Module #
LoadModule websocket_module            modules/mod_websocket.so
LoadModule websocket_connect_module    modules/mod_websocket_connect.so
<IfModule mod_websocket.c>
  <Location /echo>
    SetHandler websocket-handler
    WebSocketHandler modules/mod_websocket_connect.so websocket_connect_init
    # Connection settings
    ConnConnectionType 0            # default CONN_TCP = 0, CONN_UNIX = 1
    ConnUnixPath /tmp/test.sock     # set if CONN_UNIX
    ConnTcpHost localhost           # set if CONN_TCP
    ConnTcpPort 80                  # set if CONN_TCP
    ConnSocketTimeout .050000       # > 0 default 50ms
    ConnReceiveTimeout .050000      # > 0 default 50ms from RedisTimeout
    ConnTcpNoDelay 0
    ConnKeepAlive 0
    ConnReuseAddress 0
    ConnNonBlocking 0
    ConnMaxBufferSize 16777216
  </Location>
  <Location /redis_sock_blocking>
    SetHandler websocket-handler
    WebSocketHandler modules/mod_websocket_connect.so websocket_connect_init
    # Connection settings
    ConnConnectionType 1            # CONN_UNIX = 1
    ConnUnixPath /tmp/redis.sock
    ConnReceiveTimeout 15.0         # timeout 15 sec
  </Location>
  <Location /redis_sock_non_blocking>
    SetHandler websocket-handler
    WebSocketHandler modules/mod_websocket_connect.so websocket_connect_init
    # Connection settings
    ConnConnectionType 1            # CONN_UNIX = 1
    ConnUnixPath /tmp/redis.sock    # non blocking
    ConnNonBlocking 1
  </Location>
  <Location /redis_tcp_blocking>
    SetHandler websocket-handler
    WebSocketHandler modules/mod_websocket_connect.so websocket_connect_init
    # Connection settings
    ConnConnectionType 0
    ConnTcpHost localhost
    ConnTcpPort 6379
    ConnTcpNoDelay 1
    ConnKeepAlive 1
  </Location>
  <Location /redis_tcp_non_blocking>
    SetHandler websocket-handler
    WebSocketHandler modules/mod_websocket_connect.so websocket_connect_init
    # Connection settings
    ConnConnectionType 0
    ConnTcpHost localhost
    ConnTcpPort 6379
    ConnNonBlocking 1
  </Location>
</IfModule>

<IfModule reqtimeout_module>
 RequestReadTimeout body=300,minrate=1
</IfModule>