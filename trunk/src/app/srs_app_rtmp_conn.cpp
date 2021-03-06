/*
The MIT License (MIT)

Copyright (c) 2013-2017 SRS(ossrs)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <srs_app_rtmp_conn.hpp>

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

using namespace std;

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_core_autofree.hpp>
#include <srs_app_source.hpp>
#include <srs_app_server.hpp>
#include <srs_app_pithy_print.hpp>
#include <srs_app_config.hpp>
#include <srs_app_refer.hpp>
#include <srs_app_hls.hpp>
#include <srs_app_bandwidth.hpp>
#include <srs_app_st.hpp>
#include <srs_app_http_hooks.hpp>
#include <srs_app_edge.hpp>
#include <srs_app_utility.hpp>
#include <srs_rtmp_msg_array.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_app_recv_thread.hpp>
#include <srs_core_performance.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_app_security.hpp>
#include <srs_app_statistic.hpp>
#include <srs_protocol_utility.hpp>
#include <srs_protocol_json.hpp>
#include <srs_app_kafka.hpp>

// when stream is busy, for example, streaming is already
// publishing, when a new client to request to publish,
// sleep a while and close the connection.
#define SRS_STREAM_BUSY_SLEEP_US (int64_t)(3*1000*1000LL)

// the timeout to wait encoder to republish
// if timeout, close the connection.
#define SRS_REPUBLISH_SEND_TIMEOUT_US (int64_t)(3*60*1000*1000LL)
// if timeout, close the connection.
#define SRS_REPUBLISH_RECV_TIMEOUT_US (int64_t)(3*60*1000*1000LL)

// the timeout to wait client data, when client paused
// if timeout, close the connection.
#define SRS_PAUSED_SEND_TIMEOUT_US (int64_t)(30*60*1000*1000LL)
// if timeout, close the connection.
#define SRS_PAUSED_RECV_TIMEOUT_US (int64_t)(30*60*1000*1000LL)

// when edge timeout, retry next.
#define SRS_EDGE_TOKEN_TRAVERSE_TIMEOUT_US (int64_t)(3*1000*1000LL)

SrsSimpleRtmpClient::SrsSimpleRtmpClient()
{
    req = NULL;
    client = NULL;
    kbps = new SrsKbps();
    
    transport = new SrsTcpClient();
    stream_id = 0;
}

SrsSimpleRtmpClient::~SrsSimpleRtmpClient()
{
    close();
    
    srs_freep(kbps);
    srs_freep(transport);
    srs_freep(client);
}

int SrsSimpleRtmpClient::connect(string url, int64_t connect_timeout, int64_t stream_timeout)
{
    int ret = ERROR_SUCCESS;
    
    // when ok, ignore.
    // TODO: FIXME: should reconnect when disconnected.
    if (transport->connected()) {
        return ret;
    }
    
    // parse uri
    srs_freep(req);
    req = new SrsRequest();
    srs_parse_rtmp_url(url, req->tcUrl, req->stream);
    srs_discovery_tc_url(req->tcUrl, req->schema, req->host, req->vhost, req->app, req->port, req->param);
    
    // connect host.
    if ((ret = transport->connect(req->host, req->port, connect_timeout)) != ERROR_SUCCESS) {
        return ret;
    }
    
    srs_freep(client);
    client = new SrsRtmpClient(transport);
    
    kbps->set_io(transport, transport);
    
    client->set_recv_timeout(stream_timeout);
    client->set_send_timeout(stream_timeout);
    
    // connect to vhost/app
    if ((ret = client->handshake()) != ERROR_SUCCESS) {
        srs_error("sdk: handshake with server failed. ret=%d", ret);
        return ret;
    }
    if ((ret = connect_app()) != ERROR_SUCCESS) {
        srs_error("sdk: connect with server failed. ret=%d", ret);
        return ret;
    }
    if ((ret = client->create_stream(stream_id)) != ERROR_SUCCESS) {
        srs_error("sdk: connect with server failed, stream_id=%d. ret=%d", stream_id, ret);
        return ret;
    }
    
    return ret;
}

int SrsSimpleRtmpClient::connect_app()
{
    int ret = ERROR_SUCCESS;
    
    // args of request takes the srs info.
    if (req->args == NULL) {
        req->args = SrsAmf0Any::object();
    }
    
    // notify server the edge identity,
    // @see https://github.com/ossrs/srs/issues/147
    SrsAmf0Object* data = req->args;
    data->set("srs_sig", SrsAmf0Any::str(RTMP_SIG_SRS_KEY));
    data->set("srs_server", SrsAmf0Any::str(RTMP_SIG_SRS_SERVER));
    data->set("srs_license", SrsAmf0Any::str(RTMP_SIG_SRS_LICENSE));
    data->set("srs_role", SrsAmf0Any::str(RTMP_SIG_SRS_ROLE));
    data->set("srs_url", SrsAmf0Any::str(RTMP_SIG_SRS_URL));
    data->set("srs_version", SrsAmf0Any::str(RTMP_SIG_SRS_VERSION));
    data->set("srs_site", SrsAmf0Any::str(RTMP_SIG_SRS_WEB));
    data->set("srs_email", SrsAmf0Any::str(RTMP_SIG_SRS_EMAIL));
    data->set("srs_copyright", SrsAmf0Any::str(RTMP_SIG_SRS_COPYRIGHT));
    data->set("srs_primary", SrsAmf0Any::str(RTMP_SIG_SRS_PRIMARY));
    data->set("srs_authors", SrsAmf0Any::str(RTMP_SIG_SRS_AUTHROS));
    // for edge to directly get the id of client.
    data->set("srs_pid", SrsAmf0Any::number(getpid()));
    data->set("srs_id", SrsAmf0Any::number(_srs_context->get_id()));
    
    // local ip of edge
    std::vector<std::string> ips = srs_get_local_ipv4_ips();
    assert(_srs_config->get_stats_network() < (int)ips.size());
    std::string local_ip = ips[_srs_config->get_stats_network()];
    data->set("srs_server_ip", SrsAmf0Any::str(local_ip.c_str()));
    
    // generate the tcUrl
    std::string param = "";
    std::string target_vhost = req->vhost;
    std::string tc_url = srs_generate_tc_url(req->host, req->vhost, req->app, req->port, param);
    
    // replace the tcUrl in request,
    // which will replace the tc_url in client.connect_app().
    req->tcUrl = tc_url;
    
    // upnode server identity will show in the connect_app of client.
    // @see https://github.com/ossrs/srs/issues/160
    // the debug_srs_upnode is config in vhost and default to true.
    bool debug_srs_upnode = _srs_config->get_debug_srs_upnode(req->vhost);
    if ((ret = client->connect_app(req->app, tc_url, req, debug_srs_upnode)) != ERROR_SUCCESS) {
        srs_error("sdk: connect with server failed, tcUrl=%s, dsu=%d. ret=%d",
                  tc_url.c_str(), debug_srs_upnode, ret);
        return ret;
    }
    
    return ret;
}

bool SrsSimpleRtmpClient::connected()
{
    return transport->connected();
}

void SrsSimpleRtmpClient::close()
{
    transport->close();
    
    srs_freep(client);
    srs_freep(req);
}

int SrsSimpleRtmpClient::publish()
{
    int ret = ERROR_SUCCESS;
    
    // publish.
    if ((ret = client->publish(req->stream, stream_id)) != ERROR_SUCCESS) {
        srs_error("sdk: publish failed, stream=%s, stream_id=%d. ret=%d",
                  req->stream.c_str(), stream_id, ret);
        return ret;
    }
    
    return ret;
}

int SrsSimpleRtmpClient::play()
{
    int ret = ERROR_SUCCESS;
    
    if ((ret = client->play(req->stream, stream_id)) != ERROR_SUCCESS) {
        srs_error("connect with server failed, stream=%s, stream_id=%d. ret=%d",
                  req->stream.c_str(), stream_id, ret);
        return ret;
    }
    
    return ret;
}

void SrsSimpleRtmpClient::kbps_sample(const char* label, int64_t age)
{
    kbps->sample();
    
    int sr = kbps->get_send_kbps();
    int sr30s = kbps->get_send_kbps_30s();
    int sr5m = kbps->get_send_kbps_5m();
    int rr = kbps->get_recv_kbps();
    int rr30s = kbps->get_recv_kbps_30s();
    int rr5m = kbps->get_recv_kbps_5m();
    
    srs_trace("<- %s time=%"PRId64", okbps=%d,%d,%d, ikbps=%d,%d,%d", age, sr, sr30s, sr5m, rr, rr30s, rr5m);
}

void SrsSimpleRtmpClient::kbps_sample(const char* label, int64_t age, int msgs)
{
    kbps->sample();
    
    int sr = kbps->get_send_kbps();
    int sr30s = kbps->get_send_kbps_30s();
    int sr5m = kbps->get_send_kbps_5m();
    int rr = kbps->get_recv_kbps();
    int rr30s = kbps->get_recv_kbps_30s();
    int rr5m = kbps->get_recv_kbps_5m();
    
    srs_trace("<- %s time=%"PRId64", msgs=%d, okbps=%d,%d,%d, ikbps=%d,%d,%d", age, msgs, sr, sr30s, sr5m, rr, rr30s, rr5m);
}

int SrsSimpleRtmpClient::sid()
{
    return stream_id;
}

int SrsSimpleRtmpClient::rtmp_create_msg(char type, u_int32_t timestamp, char* data, int size, SrsSharedPtrMessage** pmsg)
{
    *pmsg = NULL;
    
    int ret = ERROR_SUCCESS;
    
    if ((ret = srs_rtmp_create_msg(type, timestamp, data, size, stream_id, pmsg)) != ERROR_SUCCESS) {
        srs_error("sdk: create shared ptr msg failed. ret=%d", ret);
        return ret;
    }
    
    return ret;
}

int SrsSimpleRtmpClient::recv_message(SrsCommonMessage** pmsg)
{
    return client->recv_message(pmsg);
}

int SrsSimpleRtmpClient::decode_message(SrsCommonMessage* msg, SrsPacket** ppacket)
{
    return client->decode_message(msg, ppacket);
}

int SrsSimpleRtmpClient::send_and_free_messages(SrsSharedPtrMessage** msgs, int nb_msgs)
{
    return client->send_and_free_messages(msgs, nb_msgs, stream_id);
}

int SrsSimpleRtmpClient::send_and_free_message(SrsSharedPtrMessage* msg)
{
    return client->send_and_free_message(msg, stream_id);
}

void SrsSimpleRtmpClient::set_recv_timeout(int64_t timeout)
{
    transport->set_recv_timeout(timeout);
}

SrsRtmpConn::SrsRtmpConn(SrsServer* svr, st_netfd_t c, string cip)
    : SrsConnection(svr, c, cip)
{
    server = svr;
    
    req = new SrsRequest();
    res = new SrsResponse();
    skt = new SrsStSocket(c);
    rtmp = new SrsRtmpServer(skt);
    refer = new SrsRefer();
    bandwidth = new SrsBandwidth();
    security = new SrsSecurity();
    duration = 0;
    kbps = new SrsKbps();
    kbps->set_io(skt, skt);
    wakable = NULL;

    mw_sleep = SRS_PERF_MW_SLEEP;
    mw_enabled = false;
    realtime = SRS_PERF_MIN_LATENCY_ENABLED;
    send_min_interval = 0;
    tcp_nodelay = false;

    _srs_config->subscribe(this);
}

SrsRtmpConn::~SrsRtmpConn()
{
    _srs_config->unsubscribe(this);
    
    srs_freep(req);
    srs_freep(res);
    srs_freep(rtmp);
    srs_freep(skt);
    srs_freep(refer);
    srs_freep(bandwidth);
    srs_freep(security);
    srs_freep(kbps);
}

void SrsRtmpConn::dispose()
{
    SrsConnection::dispose();
    
    // wakeup the handler which need to notice.
    if (wakable) {
        wakable->wakeup();
    }
}

// TODO: return detail message when error for client.
int SrsRtmpConn::do_cycle()
{
    int ret = ERROR_SUCCESS;
    
    srs_trace("RTMP client ip=%s, fd=%d", ip.c_str(), st_netfd_fileno(stfd));
    
    // notify kafka cluster.
#ifdef SRS_AUTO_KAFKA
    if ((ret = _srs_kafka->on_client(srs_id(), SrsListenerRtmpStream, ip)) != ERROR_SUCCESS) {
        srs_error("kafka handler on_client failed. ret=%d", ret);
        return ret;
    }
#endif

    rtmp->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    rtmp->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    
    if ((ret = rtmp->handshake()) != ERROR_SUCCESS) {
        srs_error("rtmp handshake failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("rtmp handshake success");
    
    if ((ret = rtmp->connect_app(req)) != ERROR_SUCCESS) {
        srs_error("rtmp connect vhost/app failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("rtmp connect app success");
    
    // set client ip to request.
    req->ip = ip;
    
    // discovery vhost, resolve the vhost from config
    SrsConfDirective* parsed_vhost = _srs_config->get_vhost(req->vhost);
    if (parsed_vhost) {
        req->vhost = parsed_vhost->arg0();
    }
    
    srs_info("discovery app success. schema=%s, vhost=%s, port=%s, app=%s",
        req->schema.c_str(), req->vhost.c_str(), req->port.c_str(), req->app.c_str());
    
    if (req->schema.empty() || req->vhost.empty() || req->port == 0 || req->app.empty()) {
        ret = ERROR_RTMP_REQ_TCURL;
        srs_error("discovery tcUrl failed. "
            "tcUrl=%s, schema=%s, vhost=%s, port=%d, app=%s, ret=%d",
            req->tcUrl.c_str(), req->schema.c_str(), req->vhost.c_str(), req->port, req->app.c_str(), ret);
        return ret;
    }
    
    // check vhost, allow default vhost.
    if ((ret = check_vhost(true)) != ERROR_SUCCESS) {
        srs_error("check vhost failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("check vhost success.");
    
    srs_trace("connect app, "
        "tcUrl=%s, pageUrl=%s, swfUrl=%s, schema=%s, vhost=%s, port=%d, app=%s, args=%s", 
        req->tcUrl.c_str(), req->pageUrl.c_str(), req->swfUrl.c_str(), 
        req->schema.c_str(), req->vhost.c_str(), req->port,
        req->app.c_str(), (req->args? "(obj)":"null"));
    
    // show client identity
    if(req->args) {
        std::string srs_version;
        std::string srs_server_ip;
        int srs_pid = 0;
        int srs_id = 0;
        
        SrsAmf0Any* prop = NULL;
        if ((prop = req->args->ensure_property_string("srs_version")) != NULL) {
            srs_version = prop->to_str();
        }
        if ((prop = req->args->ensure_property_string("srs_server_ip")) != NULL) {
            srs_server_ip = prop->to_str();
        }
        if ((prop = req->args->ensure_property_number("srs_pid")) != NULL) {
            srs_pid = (int)prop->to_number();
        }
        if ((prop = req->args->ensure_property_number("srs_id")) != NULL) {
            srs_id = (int)prop->to_number();
        }
        
        srs_info("edge-srs ip=%s, version=%s, pid=%d, id=%d", 
            srs_server_ip.c_str(), srs_version.c_str(), srs_pid, srs_id);
        if (srs_pid > 0) {
            srs_trace("edge-srs ip=%s, version=%s, pid=%d, id=%d", 
                srs_server_ip.c_str(), srs_version.c_str(), srs_pid, srs_id);
        }
    }
    
    ret = service_cycle();

    int disc_ret = ERROR_SUCCESS;
    if ((disc_ret = on_disconnect()) != ERROR_SUCCESS) {
        srs_warn("connection on disconnect peer failed, but ignore this error. disc_ret=%d, ret=%d", disc_ret, ret);
    }

    return ret;
}

int SrsRtmpConn::on_reload_vhost_removed(string vhost)
{
    int ret = ERROR_SUCCESS;
    
    if (req->vhost != vhost) {
        return ret;
    }
    
    // if the vhost connected is removed, disconnect the client.
    srs_trace("vhost %s removed/disabled, close client url=%s", 
        vhost.c_str(), req->get_stream_url().c_str());
    
    // should never close the fd in another thread,
    // one fd should managed by one thread, we should use interrupt instead.
    // so we just ignore the vhost enabled event.
    //srs_close_stfd(stfd);
    
    return ret;
}

int SrsRtmpConn::on_reload_vhost_play(string vhost)
{
    int ret = ERROR_SUCCESS;
    
    if (req->vhost != vhost) {
        return ret;
    }
    
    // send_min_interval
    if (true) {
        double v = _srs_config->get_send_min_interval(vhost);
        if (v != send_min_interval) {
            srs_trace("apply smi %.2f=>%.2f", send_min_interval, v);
            send_min_interval = v;
        }
    }

    return ret;
}

int SrsRtmpConn::on_reload_vhost_tcp_nodelay(string vhost)
{
    int ret = ERROR_SUCCESS;
    
    if (req->vhost != vhost) {
        return ret;
    }
    
    set_sock_options();
    
    return ret;
}

int SrsRtmpConn::on_reload_vhost_realtime(string vhost)
{
    int ret = ERROR_SUCCESS;
    
    if (req->vhost != vhost) {
        return ret;
    }
    
    bool realtime_enabled = _srs_config->get_realtime_enabled(req->vhost);
    if (realtime_enabled != realtime) {
        srs_trace("realtime changed %d=>%d", realtime, realtime_enabled);
        realtime = realtime_enabled;
    }

    return ret;
}

int SrsRtmpConn::on_reload_vhost_publish(string vhost)
{
    int ret = ERROR_SUCCESS;
    
    if (req->vhost != vhost) {
        return ret;
    }
    
    int p1stpt = _srs_config->get_publish_1stpkt_timeout(req->vhost);
    if (p1stpt != publish_1stpkt_timeout) {
        srs_trace("p1stpt changed %d=>%d", publish_1stpkt_timeout, p1stpt);
        publish_1stpkt_timeout = p1stpt;
    }
    
    int pnt = _srs_config->get_publish_normal_timeout(req->vhost);
    if (pnt != publish_normal_timeout) {
        srs_trace("pnt changed %d=>%d", publish_normal_timeout, pnt);
        publish_normal_timeout = pnt;
    }
    
    return ret;
}

void SrsRtmpConn::resample()
{
    kbps->resample();
}

int64_t SrsRtmpConn::get_send_bytes_delta()
{
    return kbps->get_send_bytes_delta();
}

int64_t SrsRtmpConn::get_recv_bytes_delta()
{
    return kbps->get_recv_bytes_delta();
}

void SrsRtmpConn::cleanup()
{
    kbps->cleanup();
}
    
int SrsRtmpConn::service_cycle()
{    
    int ret = ERROR_SUCCESS;
    
    if ((ret = rtmp->set_window_ack_size((int)(2.5 * 1000 * 1000))) != ERROR_SUCCESS) {
        srs_error("set window acknowledgement size failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("set window acknowledgement size success");
        
    if ((ret = rtmp->set_peer_bandwidth((int)(2.5 * 1000 * 1000), 2)) != ERROR_SUCCESS) {
        srs_error("set peer bandwidth failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("set peer bandwidth success");

    // get the ip which client connected.
    std::string local_ip = srs_get_local_ip(st_netfd_fileno(stfd));
    
    // do bandwidth test if connect to the vhost which is for bandwidth check.
    if (_srs_config->get_bw_check_enabled(req->vhost)) {
        return bandwidth->bandwidth_check(rtmp, skt, req, local_ip);
    }
    
    // do token traverse before serve it.
    // @see https://github.com/ossrs/srs/pull/239
    if (true) {
        bool vhost_is_edge = _srs_config->get_vhost_is_edge(req->vhost);
        bool edge_traverse = _srs_config->get_vhost_edge_token_traverse(req->vhost);
        if (vhost_is_edge && edge_traverse) {
            if ((ret = check_edge_token_traverse_auth()) != ERROR_SUCCESS) {
                srs_warn("token auth failed, ret=%d", ret);
                return ret;
            }
        }
    }
    
    // set chunk size to larger.
    // set the chunk size before any larger response greater than 128,
    // to make OBS happy, @see https://github.com/ossrs/srs/issues/454
    int chunk_size = _srs_config->get_chunk_size(req->vhost);
    if ((ret = rtmp->set_chunk_size(chunk_size)) != ERROR_SUCCESS) {
        srs_error("set chunk_size=%d failed. ret=%d", chunk_size, ret);
        return ret;
    }
    srs_info("set chunk_size=%d success", chunk_size);
    
    // response the client connect ok.
    if ((ret = rtmp->response_connect_app(req, local_ip.c_str())) != ERROR_SUCCESS) {
        srs_error("response connect app failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("response connect app success");
        
    if ((ret = rtmp->on_bw_done()) != ERROR_SUCCESS) {
        srs_error("on_bw_done failed. ret=%d", ret);
        return ret;
    }
    srs_verbose("on_bw_done success");
    
    while (!disposed) {
        ret = stream_service_cycle();
        
        // stream service must terminated with error, never success.
        // when terminated with success, it's user required to stop.
        if (ret == ERROR_SUCCESS) {
            continue;
        }
        
        // when not system control error, fatal error, return.
        if (!srs_is_system_control_error(ret)) {
            if (ret != ERROR_SOCKET_TIMEOUT && !srs_is_client_gracefully_close(ret)) {
                srs_error("stream service cycle failed. ret=%d", ret);
            }
            return ret;
        }
        
        // for republish, continue service
        if (ret == ERROR_CONTROL_REPUBLISH) {
            // set timeout to a larger value, wait for encoder to republish.
            rtmp->set_send_timeout(SRS_REPUBLISH_RECV_TIMEOUT_US);
            rtmp->set_recv_timeout(SRS_REPUBLISH_SEND_TIMEOUT_US);
            
            srs_trace("control message(unpublish) accept, retry stream service.");
            continue;
        }
        
        // for "some" system control error, 
        // logical accept and retry stream service.
        if (ret == ERROR_CONTROL_RTMP_CLOSE) {
            // TODO: FIXME: use ping message to anti-death of socket.
            // @see: https://github.com/ossrs/srs/issues/39
            // set timeout to a larger value, for user paused.
            rtmp->set_recv_timeout(SRS_PAUSED_RECV_TIMEOUT_US);
            rtmp->set_send_timeout(SRS_PAUSED_SEND_TIMEOUT_US);
            
            srs_trace("control message(close) accept, retry stream service.");
            continue;
        }
        
        // for other system control message, fatal error.
        srs_error("control message(%d) reject as error. ret=%d", ret, ret);
        return ret;
    }
    
    return ret;
}

int SrsRtmpConn::stream_service_cycle()
{
    int ret = ERROR_SUCCESS;
        
    SrsRtmpConnType type;
    if ((ret = rtmp->identify_client(res->stream_id, type, req->stream, req->duration)) != ERROR_SUCCESS) {
        if (!srs_is_client_gracefully_close(ret)) {
            srs_error("identify client failed. ret=%d", ret);
        }
        return ret;
    }
    req->strip();
    srs_trace("client identified, type=%s, stream_name=%s, duration=%.2f", 
        srs_client_type_string(type).c_str(), req->stream.c_str(), req->duration);
    
    // security check
    if ((ret = security->check(type, ip, req)) != ERROR_SUCCESS) {
        srs_error("security check failed. ret=%d", ret);
        return ret;
    }
    srs_info("security check ok");

    // client is identified, set the timeout to service timeout.
    rtmp->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    rtmp->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    
    // find a source to serve.
    SrsSource* source = NULL;
    if ((ret = SrsSource::fetch_or_create(req, server, &source)) != ERROR_SUCCESS) {
        return ret;
    }
    srs_assert(source != NULL);
    
    // update the statistic when source disconveried.
    SrsStatistic* stat = SrsStatistic::instance();
    if ((ret = stat->on_client(_srs_context->get_id(), req, this, type)) != ERROR_SUCCESS) {
        srs_error("stat client failed. ret=%d", ret);
        return ret;
    }

    bool vhost_is_edge = _srs_config->get_vhost_is_edge(req->vhost);
    bool enabled_cache = _srs_config->get_gop_cache(req->vhost);
    srs_trace("source url=%s, ip=%s, cache=%d, is_edge=%d, source_id=%d[%d]",
        req->get_stream_url().c_str(), ip.c_str(), enabled_cache, vhost_is_edge, 
        source->source_id(), source->source_id());
    source->set_cache(enabled_cache);
    
    switch (type) {
        case SrsRtmpConnPlay: {
            srs_verbose("start to play stream %s.", req->stream.c_str());
            
            // response connection start play
            if ((ret = rtmp->start_play(res->stream_id)) != ERROR_SUCCESS) {
                srs_error("start to play stream failed. ret=%d", ret);
                return ret;
            }
            if ((ret = http_hooks_on_play()) != ERROR_SUCCESS) {
                srs_error("http hook on_play failed. ret=%d", ret);
                return ret;
            }
            
            srs_info("start to play stream %s success", req->stream.c_str());
            ret = playing(source);
            http_hooks_on_stop();
            
            return ret;
        }
        case SrsRtmpConnFMLEPublish: {
            srs_verbose("FMLE start to publish stream %s.", req->stream.c_str());
            
            if ((ret = rtmp->start_fmle_publish(res->stream_id)) != ERROR_SUCCESS) {
                srs_error("start to publish stream failed. ret=%d", ret);
                return ret;
            }
            
            return publishing(source);
        }
        case SrsRtmpConnFlashPublish: {
            srs_verbose("flash start to publish stream %s.", req->stream.c_str());
            
            if ((ret = rtmp->start_flash_publish(res->stream_id)) != ERROR_SUCCESS) {
                srs_error("flash start to publish stream failed. ret=%d", ret);
                return ret;
            }
            
            return publishing(source);
        }
        default: {
            ret = ERROR_SYSTEM_CLIENT_INVALID;
            srs_info("invalid client type=%d. ret=%d", type, ret);
            return ret;
        }
    }

    return ret;
}

int SrsRtmpConn::check_vhost(bool try_default_vhost)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(req != NULL);
    
    SrsConfDirective* vhost = _srs_config->get_vhost(req->vhost, try_default_vhost);
    if (vhost == NULL) {
        ret = ERROR_RTMP_VHOST_NOT_FOUND;
        srs_error("vhost %s not found. ret=%d", req->vhost.c_str(), ret);
        return ret;
    }
    
    if (!_srs_config->get_vhost_enabled(req->vhost)) {
        ret = ERROR_RTMP_VHOST_NOT_FOUND;
        srs_error("vhost %s disabled. ret=%d", req->vhost.c_str(), ret);
        return ret;
    }
    
    if (req->vhost != vhost->arg0()) {
        srs_trace("vhost change from %s to %s", req->vhost.c_str(), vhost->arg0().c_str());
        req->vhost = vhost->arg0();
    }
    
    if (_srs_config->get_refer_enabled(req->vhost)) {
        if ((ret = refer->check(req->pageUrl, _srs_config->get_refer_all(req->vhost))) != ERROR_SUCCESS) {
            srs_error("check refer failed. ret=%d", ret);
            return ret;
        }
        srs_verbose("check refer success.");
    }
    
    if ((ret = http_hooks_on_connect()) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int SrsRtmpConn::playing(SrsSource* source)
{
    int ret = ERROR_SUCCESS;
    
    // create consumer of souce.
    SrsConsumer* consumer = NULL;
    if ((ret = source->create_consumer(this, consumer)) != ERROR_SUCCESS) {
        srs_error("create consumer failed. ret=%d", ret);
        return ret;
    }
    SrsAutoFree(SrsConsumer, consumer);
    srs_verbose("consumer created success.");

    // use isolate thread to recv, 
    // @see: https://github.com/ossrs/srs/issues/217
    SrsQueueRecvThread trd(consumer, rtmp, SRS_PERF_MW_SLEEP);
    
    // start isolate recv thread.
    if ((ret = trd.start()) != ERROR_SUCCESS) {
        srs_error("start isolate recv thread failed. ret=%d", ret);
        return ret;
    }
    
    // delivery messages for clients playing stream.
    wakable = consumer;
    ret = do_playing(source, consumer, &trd);
    wakable = NULL;
    
    // stop isolate recv thread
    trd.stop();
    
    // warn for the message is dropped.
    if (!trd.empty()) {
        srs_warn("drop the received %d messages", trd.size());
    }
    
    return ret;
}

int SrsRtmpConn::do_playing(SrsSource* source, SrsConsumer* consumer, SrsQueueRecvThread* trd)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(consumer != NULL);
    
    if (_srs_config->get_refer_enabled(req->vhost)) {
        if ((ret = refer->check(req->pageUrl, _srs_config->get_refer_play(req->vhost))) != ERROR_SUCCESS) {
            srs_error("check play_refer failed. ret=%d", ret);
            return ret;
        }
        srs_verbose("check play_refer success.");
    }
    
    // initialize other components
    SrsPithyPrint* pprint = SrsPithyPrint::create_rtmp_play();
    SrsAutoFree(SrsPithyPrint, pprint);

    SrsMessageArray msgs(SRS_PERF_MW_MSGS);
    bool user_specified_duration_to_stop = (req->duration > 0);
    int64_t starttime = -1;
    
    // setup the realtime.
    realtime = _srs_config->get_realtime_enabled(req->vhost);
    // setup the mw config.
    // when mw_sleep changed, resize the socket send buffer.
    mw_enabled = true;
    change_mw_sleep(_srs_config->get_mw_sleep_ms(req->vhost));
    // initialize the send_min_interval
    send_min_interval = _srs_config->get_send_min_interval(req->vhost);
    
    // set the sock options.
    set_sock_options();
    
    srs_trace("start play smi=%.2f, mw_sleep=%d, mw_enabled=%d, realtime=%d, tcp_nodelay=%d",
        send_min_interval, mw_sleep, mw_enabled, realtime, tcp_nodelay);
    
    while (!disposed) {
        // collect elapse for pithy print.
        pprint->elapse();
        
        // when source is set to expired, disconnect it.
        if (expired) {
            ret = ERROR_USER_DISCONNECT;
            srs_error("connection expired. ret=%d", ret);
            return ret;
        }

        // to use isolate thread to recv, can improve about 33% performance.
        // @see: https://github.com/ossrs/srs/issues/196
        // @see: https://github.com/ossrs/srs/issues/217
        while (!trd->empty()) {
            SrsCommonMessage* msg = trd->pump();
            srs_verbose("pump client message to process.");
            
            if ((ret = process_play_control_msg(consumer, msg)) != ERROR_SUCCESS) {
                if (!srs_is_system_control_error(ret) && !srs_is_client_gracefully_close(ret)) {
                    srs_error("process play control message failed. ret=%d", ret);
                }
                return ret;
            }
        }
        
        // quit when recv thread error.
        if ((ret = trd->error_code()) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret) && !srs_is_system_control_error(ret)) {
                srs_error("recv thread failed. ret=%d", ret);
            }
            return ret;
        }
        
#ifdef SRS_PERF_QUEUE_COND_WAIT
        // for send wait time debug
        srs_verbose("send thread now=%"PRId64"us, wait %dms", srs_update_system_time_ms(), mw_sleep);
        
        // wait for message to incoming.
        // @see https://github.com/ossrs/srs/issues/251
        // @see https://github.com/ossrs/srs/issues/257
        if (realtime) {
            // for realtime, min required msgs is 0, send when got one+ msgs.
            consumer->wait(0, mw_sleep);
        } else {
            // for no-realtime, got some msgs then send.
            consumer->wait(SRS_PERF_MW_MIN_MSGS, mw_sleep);
        }
        
        // for send wait time debug
        srs_verbose("send thread now=%"PRId64"us wakeup", srs_update_system_time_ms());
#endif
        
        // get messages from consumer.
        // each msg in msgs.msgs must be free, for the SrsMessageArray never free them.
        // @remark when enable send_min_interval, only fetch one message a time.
        int count = (send_min_interval > 0)? 1 : 0;
        if ((ret = consumer->dump_packets(&msgs, count)) != ERROR_SUCCESS) {
            srs_error("get messages from consumer failed. ret=%d", ret);
            return ret;
        }

        // reportable
        if (pprint->can_print()) {
            kbps->sample();
            srs_trace("-> "SRS_CONSTS_LOG_PLAY
                " time=%"PRId64", msgs=%d, okbps=%d,%d,%d, ikbps=%d,%d,%d, mw=%d",
                pprint->age(), count,
                kbps->get_send_kbps(), kbps->get_send_kbps_30s(), kbps->get_send_kbps_5m(),
                kbps->get_recv_kbps(), kbps->get_recv_kbps_30s(), kbps->get_recv_kbps_5m(),
                mw_sleep
            );
        }
        
        // we use wait timeout to get messages,
        // for min latency event no message incoming,
        // so the count maybe zero.
        if (count > 0) {
            srs_verbose("mw wait %dms and got %d msgs %d(%"PRId64"-%"PRId64")ms", 
                mw_sleep, count, 
                (count > 0? msgs.msgs[count - 1]->timestamp - msgs.msgs[0]->timestamp : 0),
                (count > 0? msgs.msgs[0]->timestamp : 0), 
                (count > 0? msgs.msgs[count - 1]->timestamp : 0));
        }
        
        if (count <= 0) {
#ifndef SRS_PERF_QUEUE_COND_WAIT
            srs_info("mw sleep %dms for no msg", mw_sleep);
            st_usleep(mw_sleep * 1000);
#else
            srs_verbose("mw wait %dms and got nothing.", mw_sleep);
#endif
            // ignore when nothing got.
            continue;
        }
        srs_info("got %d msgs, min=%d, mw=%d", count, SRS_PERF_MW_MIN_MSGS, mw_sleep);
        
        // only when user specifies the duration, 
        // we start to collect the durations for each message.
        if (user_specified_duration_to_stop) {
            for (int i = 0; i < count; i++) {
                SrsSharedPtrMessage* msg = msgs.msgs[i];
                
                // foreach msg, collect the duration.
                // @remark: never use msg when sent it, for the protocol sdk will free it.
                if (starttime < 0 || starttime > msg->timestamp) {
                    starttime = msg->timestamp;
                }
                duration += msg->timestamp - starttime;
                starttime = msg->timestamp;
            }
        }
        
        // sendout messages, all messages are freed by send_and_free_messages().
        // no need to assert msg, for the rtmp will assert it.
        if (count > 0 && (ret = rtmp->send_and_free_messages(msgs.msgs, count, res->stream_id)) != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_error("send messages to client failed. ret=%d", ret);
            }
            return ret;
        }
        
        // if duration specified, and exceed it, stop play live.
        // @see: https://github.com/ossrs/srs/issues/45
        if (user_specified_duration_to_stop) {
            if (duration >= (int64_t)req->duration) {
                ret = ERROR_RTMP_DURATION_EXCEED;
                srs_trace("stop live for duration exceed. ret=%d", ret);
                return ret;
            }
        }
        
        // apply the minimal interval for delivery stream in ms.
        if (send_min_interval > 0) {
            st_usleep((int64_t)(send_min_interval * 1000));
        }
    }
    
    return ret;
}

int SrsRtmpConn::publishing(SrsSource* source)
{
    int ret = ERROR_SUCCESS;
    
    if (_srs_config->get_refer_enabled(req->vhost)) {
        if ((ret = refer->check(req->pageUrl, _srs_config->get_refer_publish(req->vhost))) != ERROR_SUCCESS) {
            srs_error("check publish_refer failed. ret=%d", ret);
            return ret;
        }
        srs_verbose("check publish_refer success.");
    }

    if ((ret = http_hooks_on_publish()) != ERROR_SUCCESS) {
        srs_error("http hook on_publish failed. ret=%d", ret);
        return ret;
    }

    bool vhost_is_edge = _srs_config->get_vhost_is_edge(req->vhost);
    if ((ret = acquire_publish(source, vhost_is_edge)) == ERROR_SUCCESS) {
        // use isolate thread to recv,
        // @see: https://github.com/ossrs/srs/issues/237
        SrsPublishRecvThread trd(rtmp, req, 
            st_netfd_fileno(stfd), 0, this, source, true, vhost_is_edge);

        srs_info("start to publish stream %s success", req->stream.c_str());
        ret = do_publishing(source, &trd);

        // stop isolate recv thread
        trd.stop();
    }
    
    // whatever the acquire publish, always release publish.
    // when the acquire error in the midlle-way, the publish state changed,
    // but failed, so we must cleanup it.
    // @see https://github.com/ossrs/srs/issues/474
    // @remark when stream is busy, should never release it.
    if (ret != ERROR_SYSTEM_STREAM_BUSY) {
        release_publish(source, vhost_is_edge);
    }

    http_hooks_on_unpublish();

    return ret;
}

int SrsRtmpConn::do_publishing(SrsSource* source, SrsPublishRecvThread* trd)
{
    int ret = ERROR_SUCCESS;
    
    SrsPithyPrint* pprint = SrsPithyPrint::create_rtmp_publish();
    SrsAutoFree(SrsPithyPrint, pprint);

    // start isolate recv thread.
    if ((ret = trd->start()) != ERROR_SUCCESS) {
        srs_error("start isolate recv thread failed. ret=%d", ret);
        return ret;
    }
    
    // change the isolate recv thread context id,
    // merge its log to current thread.
    int receive_thread_cid = trd->get_cid();
    trd->set_cid(_srs_context->get_id());
    
    // initialize the publish timeout.
    publish_1stpkt_timeout = _srs_config->get_publish_1stpkt_timeout(req->vhost);
    publish_normal_timeout = _srs_config->get_publish_1stpkt_timeout(req->vhost);
    
    // set the sock options.
    set_sock_options();
    
    if (true) {
        bool mr = _srs_config->get_mr_enabled(req->vhost);
        int mr_sleep = _srs_config->get_mr_sleep_ms(req->vhost);
        srs_trace("start publish mr=%d/%d, p1stpt=%d, pnt=%d, tcp_nodelay=%d, rtcid=%d",
                  mr, mr_sleep, publish_1stpkt_timeout, publish_normal_timeout, tcp_nodelay, receive_thread_cid);
    }

    int64_t nb_msgs = 0;
    while (!disposed) {
        pprint->elapse();
        
        // when source is set to expired, disconnect it.
        if (expired) {
            ret = ERROR_USER_DISCONNECT;
            srs_error("connection expired. ret=%d", ret);
            return ret;
        }

        // cond wait for timeout.
        if (nb_msgs == 0) {
            // when not got msgs, wait for a larger timeout.
            // @see https://github.com/ossrs/srs/issues/441
            trd->wait(publish_1stpkt_timeout);
        } else {
            trd->wait(publish_normal_timeout);
        }

        // check the thread error code.
        if ((ret = trd->error_code()) != ERROR_SUCCESS) {
            if (!srs_is_system_control_error(ret) && !srs_is_client_gracefully_close(ret)) {
                srs_error("recv thread failed. ret=%d", ret);
            }
            return ret;
        }

        // when not got any messages, timeout.
        if (trd->nb_msgs() <= nb_msgs) {
            ret = ERROR_SOCKET_TIMEOUT;
            srs_warn("publish timeout %dms, nb_msgs=%"PRId64", ret=%d",
                nb_msgs? publish_normal_timeout : publish_1stpkt_timeout, nb_msgs, ret);
            break;
        }
        nb_msgs = trd->nb_msgs();

        // reportable
        if (pprint->can_print()) {
            kbps->sample();
            bool mr = _srs_config->get_mr_enabled(req->vhost);
            int mr_sleep = _srs_config->get_mr_sleep_ms(req->vhost);
            srs_trace("<- "SRS_CONSTS_LOG_CLIENT_PUBLISH
                " time=%"PRId64", okbps=%d,%d,%d, ikbps=%d,%d,%d, mr=%d/%d, p1stpt=%d, pnt=%d", pprint->age(),
                kbps->get_send_kbps(), kbps->get_send_kbps_30s(), kbps->get_send_kbps_5m(),
                kbps->get_recv_kbps(), kbps->get_recv_kbps_30s(), kbps->get_recv_kbps_5m(),
                mr, mr_sleep, publish_1stpkt_timeout, publish_normal_timeout
            );
        }
    }

    return ret;
}

int SrsRtmpConn::acquire_publish(SrsSource* source, bool is_edge)
{
    int ret = ERROR_SUCCESS;

    if (!source->can_publish(is_edge)) {
        ret = ERROR_SYSTEM_STREAM_BUSY;
        srs_warn("stream %s is already publishing. ret=%d", 
            req->get_stream_url().c_str(), ret);
        return ret;
    }
    
    // when edge, ignore the publish event, directly proxy it.
    if (is_edge) {
        if ((ret = source->on_edge_start_publish()) != ERROR_SUCCESS) {
            srs_error("notice edge start publish stream failed. ret=%d", ret);
            return ret;
        }        
    } else {
        if ((ret = source->on_publish()) != ERROR_SUCCESS) {
            srs_error("notify publish failed. ret=%d", ret);
            return ret;
        }
    }

    return ret;
}
    
void SrsRtmpConn::release_publish(SrsSource* source, bool is_edge)
{
    // when edge, notice edge to change state.
    // when origin, notice all service to unpublish.
    if (is_edge) {
        source->on_edge_proxy_unpublish();
    } else {
        source->on_unpublish();
    }
}

int SrsRtmpConn::handle_publish_message(SrsSource* source, SrsCommonMessage* msg, bool is_fmle, bool vhost_is_edge)
{
    int ret = ERROR_SUCCESS;
    
    // process publish event.
    if (msg->header.is_amf0_command() || msg->header.is_amf3_command()) {
        SrsPacket* pkt = NULL;
        if ((ret = rtmp->decode_message(msg, &pkt)) != ERROR_SUCCESS) {
            srs_error("fmle decode unpublish message failed. ret=%d", ret);
            return ret;
        }
        SrsAutoFree(SrsPacket, pkt);

        // for flash, any packet is republish.
        if (!is_fmle) {
            // flash unpublish.
            // TODO: maybe need to support republish.
            srs_trace("flash flash publish finished.");
            return ERROR_CONTROL_REPUBLISH;
        }

        // for fmle, drop others except the fmle start packet.
        if (dynamic_cast<SrsFMLEStartPacket*>(pkt)) {
            SrsFMLEStartPacket* unpublish = dynamic_cast<SrsFMLEStartPacket*>(pkt);
            if ((ret = rtmp->fmle_unpublish(res->stream_id, unpublish->transaction_id)) != ERROR_SUCCESS) {
                return ret;
            }
            return ERROR_CONTROL_REPUBLISH;
        }

        srs_trace("fmle ignore AMF0/AMF3 command message.");
        return ret;
    }

    // video, audio, data message
    if ((ret = process_publish_message(source, msg, vhost_is_edge)) != ERROR_SUCCESS) {
        srs_error("fmle process publish message failed. ret=%d", ret);
        return ret;
    }
    
    return ret;
}

int SrsRtmpConn::process_publish_message(SrsSource* source, SrsCommonMessage* msg, bool vhost_is_edge)
{
    int ret = ERROR_SUCCESS;
    
    // for edge, directly proxy message to origin.
    if (vhost_is_edge) {
        if ((ret = source->on_edge_proxy_publish(msg)) != ERROR_SUCCESS) {
            srs_error("edge publish proxy msg failed. ret=%d", ret);
            return ret;
        }
        return ret;
    }
    
    // process audio packet
    if (msg->header.is_audio()) {
        if ((ret = source->on_audio(msg)) != ERROR_SUCCESS) {
            srs_error("source process audio message failed. ret=%d", ret);
            return ret;
        }
        return ret;
    }
    // process video packet
    if (msg->header.is_video()) {
        if ((ret = source->on_video(msg)) != ERROR_SUCCESS) {
            srs_error("source process video message failed. ret=%d", ret);
            return ret;
        }
        return ret;
    }
    
    // process aggregate packet
    if (msg->header.is_aggregate()) {
        if ((ret = source->on_aggregate(msg)) != ERROR_SUCCESS) {
            srs_error("source process aggregate message failed. ret=%d", ret);
            return ret;
        }
        return ret;
    }
    
    // process onMetaData
    if (msg->header.is_amf0_data() || msg->header.is_amf3_data()) {
        SrsPacket* pkt = NULL;
        if ((ret = rtmp->decode_message(msg, &pkt)) != ERROR_SUCCESS) {
            srs_error("decode onMetaData message failed. ret=%d", ret);
            return ret;
        }
        SrsAutoFree(SrsPacket, pkt);
    
        if (dynamic_cast<SrsOnMetaDataPacket*>(pkt)) {
            SrsOnMetaDataPacket* metadata = dynamic_cast<SrsOnMetaDataPacket*>(pkt);
            if ((ret = source->on_meta_data(msg, metadata)) != ERROR_SUCCESS) {
                srs_error("source process onMetaData message failed. ret=%d", ret);
                return ret;
            }
            srs_info("process onMetaData message success.");
            return ret;
        }
        
        srs_info("ignore AMF0/AMF3 data message.");
        return ret;
    }
    
    return ret;
}

int SrsRtmpConn::process_play_control_msg(SrsConsumer* consumer, SrsCommonMessage* msg)
{
    int ret = ERROR_SUCCESS;
    
    if (!msg) {
        srs_verbose("ignore all empty message.");
        return ret;
    }
    SrsAutoFree(SrsCommonMessage, msg);
    
    if (!msg->header.is_amf0_command() && !msg->header.is_amf3_command()) {
        srs_info("ignore all message except amf0/amf3 command.");
        return ret;
    }
    
    SrsPacket* pkt = NULL;
    if ((ret = rtmp->decode_message(msg, &pkt)) != ERROR_SUCCESS) {
        srs_error("decode the amf0/amf3 command packet failed. ret=%d", ret);
        return ret;
    }
    srs_info("decode the amf0/amf3 command packet success.");
    
    SrsAutoFree(SrsPacket, pkt);
    
    // for jwplayer/flowplayer, which send close as pause message.
    // @see https://github.com/ossrs/srs/issues/6
    SrsCloseStreamPacket* close = dynamic_cast<SrsCloseStreamPacket*>(pkt);
    if (close) {
        ret = ERROR_CONTROL_RTMP_CLOSE;
        srs_trace("system control message: rtmp close stream. ret=%d", ret);
        return ret;
    }
    
    // call msg,
    // support response null first,
    // @see https://github.com/ossrs/srs/issues/106
    // TODO: FIXME: response in right way, or forward in edge mode.
    SrsCallPacket* call = dynamic_cast<SrsCallPacket*>(pkt);
    if (call) {
        // only response it when transaction id not zero,
        // for the zero means donot need response.
        if (call->transaction_id > 0) {
            SrsCallResPacket* res = new SrsCallResPacket(call->transaction_id);
            res->command_object = SrsAmf0Any::null();
            res->response = SrsAmf0Any::null();
            if ((ret = rtmp->send_and_free_packet(res, 0)) != ERROR_SUCCESS) {
                if (!srs_is_system_control_error(ret) && !srs_is_client_gracefully_close(ret)) {
                    srs_warn("response call failed. ret=%d", ret);
                }
                return ret;
            }
        }
        return ret;
    }
    
    // pause
    SrsPausePacket* pause = dynamic_cast<SrsPausePacket*>(pkt);
    if (pause) {
        if ((ret = rtmp->on_play_client_pause(res->stream_id, pause->is_pause)) != ERROR_SUCCESS) {
            srs_error("rtmp process play client pause failed. ret=%d", ret);
            return ret;
        }

        if ((ret = consumer->on_play_client_pause(pause->is_pause)) != ERROR_SUCCESS) {
            srs_error("consumer process play client pause failed. ret=%d", ret);
            return ret;
        }
        srs_info("process pause success, is_pause=%d, time=%d.", pause->is_pause, pause->time_ms);
        return ret;
    }
    
    // other msg.
    srs_info("ignore all amf0/amf3 command except pause and video control.");
    return ret;
}

void SrsRtmpConn::change_mw_sleep(int sleep_ms)
{
    if (!mw_enabled) {
        return;
    }
    
    // get the sock buffer size.
    int fd = st_netfd_fileno(stfd);
    int onb_sbuf = 0;
    socklen_t sock_buf_size = sizeof(int);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &onb_sbuf, &sock_buf_size);
    
#ifdef SRS_PERF_MW_SO_SNDBUF
    // the bytes:
    //      4KB=4096, 8KB=8192, 16KB=16384, 32KB=32768, 64KB=65536,
    //      128KB=131072, 256KB=262144, 512KB=524288
    // the buffer should set to sleep*kbps/8,
    // for example, your system delivery stream in 1000kbps,
    // sleep 800ms for small bytes, the buffer should set to:
    //      800*1000/8=100000B(about 128KB).
    // other examples:
    //      2000*3000/8=750000B(about 732KB).
    //      2000*5000/8=1250000B(about 1220KB).
    int kbps = 5000;
    int socket_buffer_size = sleep_ms * kbps / 8;

    // socket send buffer, system will double it.
    int nb_sbuf = socket_buffer_size / 2;
    
    // override the send buffer by macro.
    #ifdef SRS_PERF_SO_SNDBUF_SIZE
    nb_sbuf = SRS_PERF_SO_SNDBUF_SIZE / 2;
    #endif
    
    // set the socket send buffer when required larger buffer
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &nb_sbuf, sock_buf_size) < 0) {
        srs_warn("set sock SO_SENDBUF=%d failed.", nb_sbuf);
    }
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &nb_sbuf, &sock_buf_size);
    
    srs_trace("mw changed sleep %d=>%d, max_msgs=%d, esbuf=%d, sbuf %d=>%d, realtime=%d", 
        mw_sleep, sleep_ms, SRS_PERF_MW_MSGS, socket_buffer_size,
        onb_sbuf, nb_sbuf, realtime);
#else
    srs_trace("mw changed sleep %d=>%d, max_msgs=%d, sbuf %d, realtime=%d", 
        mw_sleep, sleep_ms, SRS_PERF_MW_MSGS, onb_sbuf, realtime);
#endif
        
    mw_sleep = sleep_ms;
}

void SrsRtmpConn::set_sock_options()
{
    bool nvalue = _srs_config->get_tcp_nodelay(req->vhost);
    if (nvalue != tcp_nodelay) {
        tcp_nodelay = nvalue;
#ifdef SRS_PERF_TCP_NODELAY
        int fd = st_netfd_fileno(stfd);

        socklen_t nb_v = sizeof(int);

        int ov = 0;
        getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ov, &nb_v);

        int v = tcp_nodelay;
        // set the socket send buffer when required larger buffer
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, nb_v) < 0) {
            srs_warn("set sock TCP_NODELAY=%d failed.", v);
        }
        getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &nb_v);

        srs_trace("set TCP_NODELAY %d=>%d", ov, v);
#else
        srs_warn("SRS_PERF_TCP_NODELAY is disabled but tcp_nodelay configed.");
#endif
    }
}

int SrsRtmpConn::check_edge_token_traverse_auth()
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(req);
    
    SrsTcpClient* transport = new SrsTcpClient();
    SrsAutoFree(SrsTcpClient, transport);
    
    vector<string> args = _srs_config->get_vhost_edge_origin(req->vhost)->args;
    for (int i = 0; i < (int)args.size(); i++) {
        string hostport = args.at(i);
        if ((ret = connect_server(hostport, transport)) == ERROR_SUCCESS) {
            break;
        }
    }
    if (ret != ERROR_SUCCESS) {
        srs_warn("token traverse connect failed. ret=%d", ret);
        return ret;
    }
    
    SrsRtmpClient* client = new SrsRtmpClient(transport);
    SrsAutoFree(SrsRtmpClient, client);
    
    return do_token_traverse_auth(client);
}

int SrsRtmpConn::connect_server(string hostport, SrsTcpClient* transport)
{
    int ret = ERROR_SUCCESS;
    
    SrsConfDirective* conf = _srs_config->get_vhost_edge_origin(req->vhost);
    srs_assert(conf);
    
    // select the origin.
    string server;
    int port = SRS_CONSTS_RTMP_DEFAULT_PORT;
    srs_parse_hostport(hostport, server, port);
    
    // open socket.
    int64_t timeout = SRS_EDGE_TOKEN_TRAVERSE_TIMEOUT_US;
    if ((ret = transport->connect(server, port, timeout)) != ERROR_SUCCESS) {
        srs_warn("edge token traverse failed, tcUrl=%s to server=%s, port=%d, timeout=%"PRId64", ret=%d",
            req->tcUrl.c_str(), server.c_str(), port, timeout, ret);
        return ret;
    }
    srs_info("edge token auth connected, url=%s/%s, server=%s:%d", req->tcUrl.c_str(), req->stream.c_str(), server.c_str(), port);
    
    return ret;
}

int SrsRtmpConn::do_token_traverse_auth(SrsRtmpClient* client)
{
    int ret = ERROR_SUCCESS;
    
    srs_assert(client);

    client->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    client->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT_US);
    
    if ((ret = client->handshake()) != ERROR_SUCCESS) {
        srs_error("handshake with server failed. ret=%d", ret);
        return ret;
    }
    
    // for token tranverse, always take the debug info(which carries token).
    if ((ret = client->connect_app(req->app, req->tcUrl, req, true)) != ERROR_SUCCESS) {
        srs_error("connect with server failed, tcUrl=%s. ret=%d", req->tcUrl.c_str(), ret);
        return ret;
    }
    
    srs_trace("edge token auth ok, tcUrl=%s", req->tcUrl.c_str());
    
    return ret;
}

int SrsRtmpConn::on_disconnect()
{
    int ret = ERROR_SUCCESS;

    http_hooks_on_close();
    
#ifdef SRS_AUTO_KAFKA
    if ((ret = _srs_kafka->on_close(srs_id())) != ERROR_SUCCESS) {
        srs_error("notify kafka failed. ret=%d", ret);
        return ret;
    }
#endif

    // TODO: implements it.

    return ret;
}

int SrsRtmpConn::http_hooks_on_connect()
{
    int ret = ERROR_SUCCESS;
    
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return ret;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_connect(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_connect");
            return ret;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((ret = SrsHttpHooks::on_connect(url, req)) != ERROR_SUCCESS) {
            srs_error("hook client on_connect failed. url=%s, ret=%d", url.c_str(), ret);
            return ret;
        }
    }
#endif

    return ret;
}

void SrsRtmpConn::http_hooks_on_close()
{
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_close(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_close");
            return;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        SrsHttpHooks::on_close(url, req, kbps->get_send_bytes(), kbps->get_recv_bytes());
    }
#endif
}

int SrsRtmpConn::http_hooks_on_publish()
{
    int ret = ERROR_SUCCESS;
    
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return ret;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_publish(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_publish");
            return ret;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((ret = SrsHttpHooks::on_publish(url, req)) != ERROR_SUCCESS) {
            srs_error("hook client on_publish failed. url=%s, ret=%d", url.c_str(), ret);
            return ret;
        }
    }
#endif

    return ret;
}

void SrsRtmpConn::http_hooks_on_unpublish()
{
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_unpublish(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_unpublish");
            return;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        SrsHttpHooks::on_unpublish(url, req);
    }
#endif
}

int SrsRtmpConn::http_hooks_on_play()
{
    int ret = ERROR_SUCCESS;
    
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return ret;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_play(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_play");
            return ret;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        if ((ret = SrsHttpHooks::on_play(url, req)) != ERROR_SUCCESS) {
            srs_error("hook client on_play failed. url=%s, ret=%d", url.c_str(), ret);
            return ret;
        }
    }
#endif

    return ret;
}

void SrsRtmpConn::http_hooks_on_stop()
{
#ifdef SRS_AUTO_HTTP_CALLBACK
    if (!_srs_config->get_vhost_http_hooks_enabled(req->vhost)) {
        return;
    }
    
    // the http hooks will cause context switch,
    // so we must copy all hooks for the on_connect may freed.
    // @see https://github.com/ossrs/srs/issues/475
    vector<string> hooks;
    
    if (true) {
        SrsConfDirective* conf = _srs_config->get_vhost_on_stop(req->vhost);
        
        if (!conf) {
            srs_info("ignore the empty http callback: on_stop");
            return;
        }
        
        hooks = conf->args;
    }
    
    for (int i = 0; i < (int)hooks.size(); i++) {
        std::string url = hooks.at(i);
        SrsHttpHooks::on_stop(url, req);
    }
#endif

    return;
}

