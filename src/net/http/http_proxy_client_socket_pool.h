// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_HTTP_HTTP_PROXY_CLIENT_SOCKET_POOL_H_
#define NET_HTTP_HTTP_PROXY_CLIENT_SOCKET_POOL_H_

#include <string>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/http/http_auth.h"
#include "net/http/http_response_info.h"
#include "net/http/proxy_client_socket.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/client_socket_pool_base.h"
#include "net/socket/client_socket_pool_histograms.h"
#include "net/socket/ssl_client_socket.h"
#include "net/spdy/spdy_session.h"

namespace net {

class HttpAuthCache;
class HttpAuthHandlerFactory;
class ProxyDelegate;
class SSLClientSocketPool;
class SSLSocketParams;
class SpdySessionPool;
class SpdyStream;
class TransportClientSocketPool;
class TransportSocketParams;

// HttpProxySocketParams only needs the socket params for one of the proxy
// types.  The other param must be NULL.  When using an HTTP Proxy,
// |transport_params| must be set.  When using an HTTPS Proxy, |ssl_params|
// must be set.
class NET_EXPORT_PRIVATE HttpProxySocketParams
    : public base::RefCounted<HttpProxySocketParams> {
 public:
  HttpProxySocketParams(
      const scoped_refptr<TransportSocketParams>& transport_params,
      const scoped_refptr<SSLSocketParams>& ssl_params,
      const GURL& request_url,
      const std::string& user_agent,
      const HostPortPair& endpoint,
      HttpAuthCache* http_auth_cache,
      HttpAuthHandlerFactory* http_auth_handler_factory,
      SpdySessionPool* spdy_session_pool,
      bool tunnel,
      ProxyDelegate* proxy_delegate);

  const scoped_refptr<TransportSocketParams>& transport_params() const {
    return transport_params_;
  }
  const scoped_refptr<SSLSocketParams>& ssl_params() const {
    return ssl_params_;
  }
  const GURL& request_url() const { return request_url_; }
  const std::string& user_agent() const { return user_agent_; }
  const HostPortPair& endpoint() const { return endpoint_; }
  HttpAuthCache* http_auth_cache() const { return http_auth_cache_; }
  HttpAuthHandlerFactory* http_auth_handler_factory() const {
    return http_auth_handler_factory_;
  }
  SpdySessionPool* spdy_session_pool() {
    return spdy_session_pool_;
  }
  const HostResolver::RequestInfo& destination() const;
  bool tunnel() const { return tunnel_; }
  bool ignore_limits() const { return ignore_limits_; }

  ProxyDelegate* proxy_delegate() const {
    return proxy_delegate_;
  }

 private:
  friend class base::RefCounted<HttpProxySocketParams>;
  ~HttpProxySocketParams();

  const scoped_refptr<TransportSocketParams> transport_params_;
  const scoped_refptr<SSLSocketParams> ssl_params_;
  SpdySessionPool* spdy_session_pool_;
  const GURL request_url_;
  const std::string user_agent_;
  const HostPortPair endpoint_;
  HttpAuthCache* const http_auth_cache_;
  HttpAuthHandlerFactory* const http_auth_handler_factory_;
  const bool tunnel_;
  bool ignore_limits_;
  ProxyDelegate* proxy_delegate_;

  DISALLOW_COPY_AND_ASSIGN(HttpProxySocketParams);
};

// HttpProxyConnectJob optionally establishes a tunnel through the proxy
// server after connecting the underlying transport socket.
class HttpProxyConnectJob : public ConnectJob {
 public:
  HttpProxyConnectJob(const std::string& group_name,
                      RequestPriority priority,
                      const scoped_refptr<HttpProxySocketParams>& params,
                      const base::TimeDelta& timeout_duration,
                      TransportClientSocketPool* transport_pool,
                      SSLClientSocketPool* ssl_pool,
                      Delegate* delegate,
                      NetLog* net_log);
  ~HttpProxyConnectJob() override;

  // ConnectJob methods.
  LoadState GetLoadState() const override;

  void GetAdditionalErrorState(ClientSocketHandle* handle) override;

 private:
  enum State {
    STATE_TCP_CONNECT,
    STATE_TCP_CONNECT_COMPLETE,
    STATE_SSL_CONNECT,
    STATE_SSL_CONNECT_COMPLETE,
    STATE_HTTP_PROXY_CONNECT,
    STATE_HTTP_PROXY_CONNECT_COMPLETE,
    STATE_SPDY_PROXY_CREATE_STREAM,
    STATE_SPDY_PROXY_CREATE_STREAM_COMPLETE,
    STATE_SPDY_PROXY_CONNECT_COMPLETE,
    STATE_NONE,
  };

  void OnIOComplete(int result);

  // Runs the state transition loop.
  int DoLoop(int result);

  // Connecting to HTTP Proxy
  int DoTransportConnect();
  int DoTransportConnectComplete(int result);
  // Connecting to HTTPS Proxy
  int DoSSLConnect();
  int DoSSLConnectComplete(int result);

  int DoHttpProxyConnect();
  int DoHttpProxyConnectComplete(int result);

  int DoSpdyProxyCreateStream();
  int DoSpdyProxyCreateStreamComplete(int result);

  void NotifyProxyDelegateOfCompletion(int result);

  // Begins the tcp connection and the optional Http proxy tunnel.  If the
  // request is not immediately servicable (likely), the request will return
  // ERR_IO_PENDING. An OK return from this function or the callback means
  // that the connection is established; ERR_PROXY_AUTH_REQUESTED means
  // that the tunnel needs authentication credentials, the socket will be
  // returned in this case, and must be release back to the pool; or
  // a standard net error code will be returned.
  int ConnectInternal() override;

  scoped_refptr<HttpProxySocketParams> params_;
  TransportClientSocketPool* const transport_pool_;
  SSLClientSocketPool* const ssl_pool_;

  State next_state_;
  CompletionCallback callback_;
  scoped_ptr<ClientSocketHandle> transport_socket_handle_;
  scoped_ptr<ProxyClientSocket> transport_socket_;
  bool using_spdy_;
  // Protocol negotiated with the server.
  NextProto protocol_negotiated_;

  HttpResponseInfo error_response_info_;

  SpdyStreamRequest spdy_stream_request_;

  base::WeakPtrFactory<HttpProxyConnectJob> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(HttpProxyConnectJob);
};

class NET_EXPORT_PRIVATE HttpProxyClientSocketPool
    : public ClientSocketPool,
      public HigherLayeredPool {
 public:
  typedef HttpProxySocketParams SocketParams;

  HttpProxyClientSocketPool(int max_sockets,
                            int max_sockets_per_group,
                            ClientSocketPoolHistograms* histograms,
                            TransportClientSocketPool* transport_pool,
                            SSLClientSocketPool* ssl_pool,
                            NetLog* net_log);

  ~HttpProxyClientSocketPool() override;

  // ClientSocketPool implementation.
  int RequestSocket(const std::string& group_name,
                    const void* connect_params,
                    RequestPriority priority,
                    ClientSocketHandle* handle,
                    const CompletionCallback& callback,
                    const BoundNetLog& net_log) override;

  void RequestSockets(const std::string& group_name,
                      const void* params,
                      int num_sockets,
                      const BoundNetLog& net_log) override;

  void CancelRequest(const std::string& group_name,
                     ClientSocketHandle* handle) override;

  void ReleaseSocket(const std::string& group_name,
                     scoped_ptr<StreamSocket> socket,
                     int id) override;

  void FlushWithError(int error) override;

  void CloseIdleSockets() override;

  int IdleSocketCount() const override;

  int IdleSocketCountInGroup(const std::string& group_name) const override;

  LoadState GetLoadState(const std::string& group_name,
                         const ClientSocketHandle* handle) const override;

  base::DictionaryValue* GetInfoAsValue(
      const std::string& name,
      const std::string& type,
      bool include_nested_pools) const override;

  base::TimeDelta ConnectionTimeout() const override;

  ClientSocketPoolHistograms* histograms() const override;

  // LowerLayeredPool implementation.
  bool IsStalled() const override;

  void AddHigherLayeredPool(HigherLayeredPool* higher_pool) override;

  void RemoveHigherLayeredPool(HigherLayeredPool* higher_pool) override;

  // HigherLayeredPool implementation.
  bool CloseOneIdleConnection() override;

 private:
  typedef ClientSocketPoolBase<HttpProxySocketParams> PoolBase;

  class HttpProxyConnectJobFactory : public PoolBase::ConnectJobFactory {
   public:
    HttpProxyConnectJobFactory(TransportClientSocketPool* transport_pool,
                               SSLClientSocketPool* ssl_pool,
                               NetLog* net_log);

    // ClientSocketPoolBase::ConnectJobFactory methods.
    scoped_ptr<ConnectJob> NewConnectJob(
        const std::string& group_name,
        const PoolBase::Request& request,
        ConnectJob::Delegate* delegate) const override;

    base::TimeDelta ConnectionTimeout() const override;

   private:
    TransportClientSocketPool* const transport_pool_;
    SSLClientSocketPool* const ssl_pool_;
    NetLog* net_log_;
    base::TimeDelta timeout_;

    DISALLOW_COPY_AND_ASSIGN(HttpProxyConnectJobFactory);
  };

  TransportClientSocketPool* const transport_pool_;
  SSLClientSocketPool* const ssl_pool_;
  PoolBase base_;

  DISALLOW_COPY_AND_ASSIGN(HttpProxyClientSocketPool);
};

}  // namespace net

#endif  // NET_HTTP_HTTP_PROXY_CLIENT_SOCKET_POOL_H_
