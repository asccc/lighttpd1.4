#include "first.h"

#include "base.h"
#include "buffer.h"
#include "burl.h"       /* HTTP_PARSEOPT_HEADER_STRICT */
#include "chunk.h"
#include "log.h"
#include "connections.h"
#include "fdevent.h"
#include "http_header.h"

#include "reqpool.h"
#include "request.h"
#include "response.h"
#include "network.h"
#include "stat_cache.h"

#include "plugin.h"
#include "plugins.h"

#include "sock_addr_cache.h"

#include <sys/stat.h>

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "sys-socket.h"

#define HTTP_LINGER_TIMEOUT 5

#define connection_set_state(r,state)       request_set_state((r),(state))
#define connection_set_state_error(r,state) request_set_state_error((r),(state))


__attribute_cold__
static connection *connection_init(server *srv);

static void connection_reset(connection *con);

static connection *connections_get_new_connection(server *srv) {
    connection *con;
    --srv->lim_conns;
    if (srv->conns_pool) {
        con = srv->conns_pool;
        srv->conns_pool = con->next;
    }
    else {
        con = connection_init(srv);
        connection_reset(con);
    }
    /*con->prev = NULL;*//*(already set)*/
    if ((con->next = srv->conns))
        con->next->prev = con;
    return (srv->conns = con);
}

static void connection_del(server *srv, connection *con) {
    if (con->next)
        con->next->prev = con->prev;
    if (con->prev)
        con->prev->next = con->next;
    else
        srv->conns = con->next;
    con->prev = NULL;
    con->next = srv->conns_pool;
    srv->conns_pool = con;
    ++srv->lim_conns;
}

static void connection_close(connection *con) {
	if (con->fd < 0) con->fd = -con->fd;

	plugins_call_handle_connection_close(con);

	server * const srv = con->srv;
	request_st * const r = &con->request;
	request_reset_ex(r); /*(r->conf.* is still valid below)*/
	connection_set_state(r, CON_STATE_CONNECT);

	chunkqueue_reset(con->read_queue);
	con->request_count = 0;
	con->is_ssl_sock = 0;
	con->revents_err = 0;

	fdevent_fdnode_event_del(srv->ev, con->fdn);
	fdevent_unregister(srv->ev, con->fdn);
	con->fdn = NULL;
#ifdef __WIN32
	if (0 == closesocket(con->fd))
#else
	if (0 == close(con->fd))
#endif
		--srv->cur_fds;
	else
		log_perror(r->conf.errh, __FILE__, __LINE__,
		  "(warning) close: %d", con->fd);

	if (r->conf.log_state_handling) {
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "connection closed for fd %d", con->fd);
	}
	con->fd = -1;

	connection_del(srv, con);
}

static void connection_read_for_eos_plain(connection * const con) {
	/* we have to do the linger_on_close stuff regardless
	 * of r->keep_alive; even non-keepalive sockets
	 * may still have unread data, and closing before reading
	 * it will make the client not see all our output.
	 */
	ssize_t len;
	const int type = sock_addr_get_family(&con->dst_addr);
	char buf[16384];
	do {
		len = fdevent_socket_read_discard(con->fd, buf, sizeof(buf),
						  type, SOCK_STREAM);
	} while (len > 0 || (len < 0 && errno == EINTR));

	if (len < 0 && errno == EAGAIN) return;
      #if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
	if (len < 0 && errno == EWOULDBLOCK) return;
      #endif

	/* 0 == len || (len < 0 && (errno is a non-recoverable error)) */
		con->close_timeout_ts = log_monotonic_secs - (HTTP_LINGER_TIMEOUT+1);
}

static void connection_read_for_eos_ssl(connection * const con) {
	if (con->network_read(con, con->read_queue, MAX_READ_LIMIT) < 0)
		con->close_timeout_ts = log_monotonic_secs - (HTTP_LINGER_TIMEOUT+1);
	chunkqueue_reset(con->read_queue);
}

static void connection_read_for_eos(connection * const con) {
	!con->is_ssl_sock
	  ? connection_read_for_eos_plain(con)
	  : connection_read_for_eos_ssl(con);
}

static void connection_handle_close_state(connection *con) {
	connection_read_for_eos(con);

	if (log_monotonic_secs - con->close_timeout_ts > HTTP_LINGER_TIMEOUT) {
		connection_close(con);
	}
}

static void connection_handle_shutdown(connection *con) {
	plugins_call_handle_connection_shut_wr(con);

	connection_reset(con);

	/* close the connection */
	if (con->fd >= 0
	    && (con->is_ssl_sock || 0 == shutdown(con->fd, SHUT_WR))) {
		con->close_timeout_ts = log_monotonic_secs;

		request_st * const r = &con->request;
		connection_set_state(r, CON_STATE_CLOSE);
		if (r->conf.log_state_handling) {
			log_error(r->conf.errh, __FILE__, __LINE__,
			  "shutdown for fd %d", con->fd);
		}
	} else {
		connection_close(con);
	}
}


static void
connection_handle_request_start_state (request_st * const r, connection * const con)
{
    /*connection_set_state(r, CON_STATE_REQUEST_START);*/
    ++con->request_count;
    con->read_idle_ts = log_monotonic_secs;
    r->start_hp.tv_sec = log_epoch_secs;
    r->loops_per_request = 0;
    if (r->conf.high_precision_timestamps)
        log_clock_gettime_realtime(&r->start_hp);
}


static void connection_handle_response_end_state(request_st * const r, connection * const con) {
	if (r->http_version > HTTP_VERSION_1_1) {
		r->keep_alive = 0;
		/* set a status so that mod_accesslog, mod_rrdtool hooks are called
		 * in plugins_call_handle_request_done() (XXX: or set to 0 to omit) */
		r->http_status = 100; /* XXX: what if con->state == CON_STATE_ERROR? */
	}

	/* call request_done hook if http_status set (e.g. to log request) */
	/* (even if error, connection dropped, as long as http_status is set) */
	if (r->http_status) plugins_call_handle_request_done(r);

	if (r->reqbody_length != r->reqbody_queue.bytes_in
	    || r->state == CON_STATE_ERROR) {
		/* request body may not have been read completely */
		r->keep_alive = 0;
		/* clean up failed partial write of 1xx intermediate responses*/
		if (&r->write_queue != con->write_queue) { /*(for HTTP/1.1)*/
			chunkqueue_free(con->write_queue);
			con->write_queue = &r->write_queue;
		}
	}

        if (r->keep_alive > 0) {
		request_reset(r);
		con->is_readable = 1; /* potentially trigger optimistic read */
		/*(accounting used by mod_accesslog for HTTP/1.0 and HTTP/1.1)*/
		/*(overloaded to detect next bytes recv'd on keep-alive con)*/
		r->x.h1.bytes_read_ckpt = r->read_queue.bytes_in;
		r->x.h1.bytes_written_ckpt = r->write_queue.bytes_out;
#if 0
		r->start_hp.tv_sec = log_epoch_secs;
		con->read_idle_ts = log_monotonic_secs;
#endif
		connection_set_state(r, CON_STATE_REQUEST_START);
	} else {
		connection_handle_shutdown(con);
	}
}


__attribute_pure__
static off_t
connection_write_throttled (const connection * const con, off_t max_bytes)
{
    const request_config * const restrict rconf = &con->request.conf;
    if (0 == rconf->global_bytes_per_second && 0 == rconf->bytes_per_second)
        return max_bytes;

    if (rconf->global_bytes_per_second) {
        off_t limit = (off_t)rconf->global_bytes_per_second
                    - *(rconf->global_bytes_per_second_cnt_ptr);
        if (max_bytes > limit)
            max_bytes = limit;
    }

    if (rconf->bytes_per_second) {
        off_t limit = (off_t)rconf->bytes_per_second
                    - con->bytes_written_cur_second;
        if (max_bytes > limit)
            max_bytes = limit;
    }

    return max_bytes > 0 ? max_bytes : 0; /*(0 == reached traffic limit)*/
}


static off_t
connection_write_throttle (connection * const con, off_t max_bytes)
{
    /*assert(max_bytes > 0);*/
    max_bytes = connection_write_throttled(con, max_bytes);
    if (0 == max_bytes) con->traffic_limit_reached = 1;
    return max_bytes;
}


static int
connection_write_chunkqueue (connection * const con, chunkqueue * const restrict cq, off_t max_bytes)
{
    /*assert(!chunkqueue_is_empty(cq));*//* checked by callers */

    con->write_request_ts = log_monotonic_secs;

    max_bytes = connection_write_throttle(con, max_bytes);
    if (0 == max_bytes) return 1;

    off_t written = cq->bytes_out;
    int ret;

  #ifdef TCP_CORK
    int corked = 0;
  #endif

    /* walk chunkqueue up to first FILE_CHUNK (if present)
     * This may incur memory load misses for pointer chasing, but effectively
     * preloads part of the chunkqueue, something which used to be a side effect
     * of a previous (less efficient) version of chunkqueue_length() which
     * walked the entire chunkqueue (on each and every call).  The loads here
     * make a measurable difference in performance in underlying call to
     * con->network_write() */
    if (cq->first->next && cq->first->type == MEM_CHUNK) {
        const chunk *c = cq->first;
        do { c = c->next; } while (c && c->type == MEM_CHUNK);
      #ifdef TCP_CORK
        /* Linux: put a cork into socket as we want to combine write() calls
         * but only if we really have multiple chunks including non-MEM_CHUNK
         * (or if multiple chunks and TLS), and only if TCP socket */
        /* (max_bytes may have been reduced by connection_write_throttle(),
         *  but not bothering to check; might result in some extra corking) */
        if (NULL != c || (con->is_ssl_sock && chunkqueue_length(cq) > 16384)) {
            const int sa_family = sock_addr_get_family(&con->srv_socket->addr);
            if (sa_family == AF_INET || sa_family == AF_INET6) {
                corked = 1;
                (void)setsockopt(con->fd, IPPROTO_TCP, TCP_CORK,
                                 &corked, sizeof(corked));
            }
        }
      #endif
    }

    ret = con->network_write(con, cq, max_bytes);
    if (ret >= 0) {
        ret = chunkqueue_is_empty(cq) ? 0 : 1;
    }

  #ifdef TCP_CORK
    if (corked) {
        corked = 0;
        (void)setsockopt(con->fd, IPPROTO_TCP, TCP_CORK,
                         &corked, sizeof(corked));
    }
  #endif

    written = cq->bytes_out - written;
    con->bytes_written_cur_second += written;
    request_st * const r = &con->request;
    if (r->conf.global_bytes_per_second_cnt_ptr)
        *(r->conf.global_bytes_per_second_cnt_ptr) += written;

    return ret;
}


static int
connection_write_1xx_info (request_st * const r, connection * const con)
{
    /* (Note: prior 1xx intermediate responses may be present in cq) */
    /* (Note: also choosing not to update con->write_request_ts
     *  which differs from connection_write_chunkqueue()) */
    chunkqueue * const cq = con->write_queue;
    off_t written = cq->bytes_out;

    int rc = con->network_write(con, cq, MAX_WRITE_LIMIT);

    written = cq->bytes_out - written;
    con->bytes_written_cur_second += written;
    if (r->conf.global_bytes_per_second_cnt_ptr)
        *(r->conf.global_bytes_per_second_cnt_ptr) += written;

    if (rc < 0) {
        connection_set_state_error(r, CON_STATE_ERROR);
        return 0; /* error */
    }

    if (!chunkqueue_is_empty(cq)) { /* partial write (unlikely) */
        con->is_writable = 0;
        if (cq == &r->write_queue) {
            /* save partial write of 1xx in separate chunkqueue
             * Note: sending of remainder of 1xx might be delayed
             * until next set of response headers are sent */
            con->write_queue = chunkqueue_init(NULL);
            /* (copy bytes for accounting purposes in event of failure) */
            con->write_queue->bytes_in = cq->bytes_out; /*(yes, bytes_out)*/
            con->write_queue->bytes_out = cq->bytes_out;
            chunkqueue_append_chunkqueue(con->write_queue, cq);
        }
    }

  #if 0
    /* XXX: accounting inconsistency
     * 1xx is not currently included in r->resp_header_len,
     * so mod_accesslog reporting of %b or %B (FORMAT_BYTES_OUT_NO_HEADER)
     * reports all bytes out minus len of final response headers,
     * but including 1xx intermediate responses.  If 1xx intermediate
     * responses were included in r->resp_header_len, then there are a
     * few places in the code which must be adjusted to use r->resp_header_done
     * instead of (0 == r->resp_header_len) as flag that final response was set
     * (Doing the following would "discard" the 1xx len from bytes_out)
     */
    r->write_queue.bytes_in = r->write_queue.bytes_out = 0;
  #endif

    return 1; /* success */
}


int
connection_send_1xx (request_st * const r, connection * const con)
{
    /* Make best effort to send HTTP/1.1 1xx intermediate */
    /* (Note: if other modules set response headers *before* the
     *  handle_response_start hook, and the backends subsequently sends 1xx,
     *  then the response headers are sent here with 1xx and might be cleared
     *  by caller (http_response_parse_headers() and http_response_check_1xx()),
     *  instead of being sent with the final response.
     *  (e.g. mod_magnet setting response headers, then backend sending 103)) */

    chunkqueue * const cq = con->write_queue; /*(bypass r->write_queue)*/

    buffer * const b = chunkqueue_append_buffer_open(cq);
    buffer_copy_string_len(b, CONST_STR_LEN("HTTP/1.1 "));
    http_status_append(b, r->http_status);
    for (uint32_t i = 0; i < r->resp_headers.used; ++i) {
        const data_string * const ds = (data_string *)r->resp_headers.data[i];
        const uint32_t klen = buffer_clen(&ds->key);
        const uint32_t vlen = buffer_clen(&ds->value);
        if (0 == klen || 0 == vlen) continue;
        buffer_append_str2(b, CONST_STR_LEN("\r\n"), ds->key.ptr, klen);
        buffer_append_str2(b, CONST_STR_LEN(": "), ds->value.ptr, vlen);
    }
    buffer_append_string_len(b, CONST_STR_LEN("\r\n\r\n"));
    chunkqueue_append_buffer_commit(cq);

    if (con->traffic_limit_reached)
        return 1; /* success; send later if throttled */

    return connection_write_1xx_info(r, con);
}


static int
connection_write_100_continue (request_st * const r, connection * const con)
{
    /* Make best effort to send "HTTP/1.1 100 Continue" */
    static const char http_100_continue[] = "HTTP/1.1 100 Continue\r\n\r\n";

    if (con->traffic_limit_reached)
        return 1; /* success; skip sending if throttled */

    chunkqueue * const cq = con->write_queue; /*(bypass r->write_queue)*/
    chunkqueue_append_mem(cq, http_100_continue, sizeof(http_100_continue)-1);
    return connection_write_1xx_info(r, con);
}


static int connection_handle_write(request_st * const r, connection * const con) {
	/*assert(!chunkqueue_is_empty(cq));*//* checked by callers */

	if (con->is_writable <= 0) return CON_STATE_WRITE;
	int rc = connection_write_chunkqueue(con, con->write_queue, MAX_WRITE_LIMIT);
	switch (rc) {
	case 0:
		if (r->resp_body_finished) {
			connection_set_state(r, CON_STATE_RESPONSE_END);
			return CON_STATE_RESPONSE_END;
		}
		break;
	case -1: /* error on our side */
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "connection closed: write failed on fd %d", con->fd);
		__attribute_fallthrough__
	case -2: /* remote close */
		connection_set_state_error(r, CON_STATE_ERROR);
		return CON_STATE_ERROR;
	case 1:
		/* do not spin trying to send HTTP/2 server Connection Preface
		 * while waiting for TLS negotiation to complete */
		if (con->write_queue->bytes_out)
			con->is_writable = 0;

		/* not finished yet -> WRITE */
		break;
	}

	return CON_STATE_WRITE; /*(state did not change)*/
}

static int connection_handle_write_state(request_st * const r, connection * const con) {
    do {
        /* only try to write if we have something in the queue */
        if (!chunkqueue_is_empty(&r->write_queue)) {
            int rc = connection_handle_write(r, con);
            if (rc != CON_STATE_WRITE) return rc;
        }
        else if (r->resp_body_finished) {
            connection_set_state(r, CON_STATE_RESPONSE_END);
            return CON_STATE_RESPONSE_END;
        }

        if (r->handler_module && !r->resp_body_finished) {
            const plugin * const p = r->handler_module;
            int rc = p->handle_subrequest(r, p->data);
            switch(rc) {
            case HANDLER_WAIT_FOR_EVENT:
            case HANDLER_FINISHED:
            case HANDLER_GO_ON:
                break;
            /*case HANDLER_COMEBACK:*/
            /*case HANDLER_ERROR:*/
            default:
                connection_set_state_error(r, CON_STATE_ERROR);
                return CON_STATE_ERROR;
            }
        }
    } while (!chunkqueue_is_empty(&r->write_queue)
             ? con->is_writable > 0 && 0 == con->traffic_limit_reached
             : r->resp_body_finished);

    return CON_STATE_WRITE;
}


__attribute_cold__
static connection *connection_init(server *srv) {
	connection * const con = ck_calloc(1, sizeof(*con));

	con->srv = srv;
	con->plugin_slots = srv->plugin_slots;
	con->config_data_base = srv->config_data_base;

	request_st * const r = &con->request;
	request_init_data(r, con, srv);
	con->write_queue = &r->write_queue;
	con->read_queue = &r->read_queue;

	/* init plugin-specific per-connection structures */
	con->plugin_ctx = ck_calloc(srv->plugins.used + 1, sizeof(void *));

	return con;
}


static void connection_free(connection * const con) {
    request_st * const r = &con->request;

    connection_reset(con);
    if (con->write_queue != &r->write_queue)
        chunkqueue_free(con->write_queue);
    if (con->read_queue != &r->read_queue)
        chunkqueue_free(con->read_queue);
    request_free_data(r);

    free(con->plugin_ctx);
    free(con->dst_addr_buf.ptr);
    free(con);
}

void connections_pool_clear(server * const srv) {
    connection *con;
    while ((con = srv->conns_pool)) {
        srv->conns_pool = con->next;
        connection_free(con);
    }
}

void connections_free(server *srv) {
    connections_pool_clear(srv);

    connection *con;
    while ((con = srv->conns)) {
        srv->conns = con->next;
        connection_free(con);
    }
}


static void connection_reset(connection *con) {
	request_st * const r = &con->request;
	request_reset(r);
	con->is_readable = 1;
	con->bytes_written_cur_second = 0;
	con->fn = NULL;
}


__attribute_cold__
static chunk *
connection_discard_blank_line (chunkqueue * const cq, uint32_t header_len)
{
    /*(separate func only to be able to mark with compiler hint as cold)*/
    chunkqueue_mark_written(cq, header_len);
    return cq->first; /* refresh c after chunkqueue_mark_written() */
}


static chunk * connection_read_header_more(connection *con, chunkqueue *cq, chunk *c, const size_t olen) {
    /*(should not be reached by HTTP/2 streams)*/
    /*if (r->http_version == HTTP_VERSION_2) return NULL;*/
    /*(However, new connections over TLS may become HTTP/2 connections via ALPN
     * and return from this routine with r->http_version == HTTP_VERSION_2) */

    if ((NULL == c || NULL == c->next) && con->is_readable > 0) {
        con->read_idle_ts = log_monotonic_secs;
        if (0 != con->network_read(con, cq, MAX_READ_LIMIT)) {
            request_st * const r = &con->request;
            connection_set_state_error(r, CON_STATE_ERROR);
        }
        /* check if switched to HTTP/2 (ALPN "h2" during TLS negotiation) */
        request_st * const r = &con->request;
        if (r->http_version == HTTP_VERSION_2) return NULL;
    }

    if (cq->first != cq->last && 0 != olen) {
        const size_t clen = chunkqueue_length(cq);
        size_t block = (olen + (16384-1)) & ~(16384-1);
        block += (block - olen > 1024 ? 0 : 16384);
        chunkqueue_compact_mem(cq, block > clen ? clen : block);
    }

    /* detect if data is added to chunk */
    c = cq->first;
    return (c && (size_t)c->offset + olen < buffer_clen(c->mem))
      ? c
      : NULL;
}


__attribute_cold__
static void
connection_transition_h2 (request_st * const h2r, connection * const con)
{
    buffer_copy_string_len(&h2r->target,      CONST_STR_LEN("*"));
    buffer_copy_string_len(&h2r->target_orig, CONST_STR_LEN("*"));
    buffer_copy_string_len(&h2r->uri.path,    CONST_STR_LEN("*"));
    h2r->http_method = HTTP_METHOD_PRI;
    h2r->reqbody_length = -1; /*(unnecessary for h2r?)*/
    h2r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_POLLIN;

    /* (h2r->state == CON_STATE_READ) for transition by ALPN
     *   or starting cleartext HTTP/2 with Prior Knowledge
     *   (e.g. via HTTP Alternative Services)
     * (h2r->state == CON_STATE_WRITE) for Upgrade: h2c */

    if (h2r->state == CON_STATE_ERROR) /*(CON_STATE_ERROR unexpected here)*/
        return;

    connection_set_state(h2r, CON_STATE_WRITE);

  #if 0 /* ... if it turns out we need a separate fdevent handler for HTTP/2 */
    con->fdn->handler = connection_handle_fdevent_h2;
  #endif

    /* r->conf.h2proto must be checked prior to setting r->http_version to
     * HTTP_VERSION_2, so if we get here, http_dispatch[HTTP_VERSION_2] inited*/
    if (NULL == con->h2) /*(not yet transitioned to HTTP/2; not Upgrade: h2c)*/
        http_dispatch[HTTP_VERSION_2].upgrade_h2(h2r, con);
}


__attribute_cold__
static int
connection_check_upgrade (request_st * const r, connection * const con)
{
    buffer *upgrade = http_header_request_get(r, HTTP_HEADER_UPGRADE,
                                              CONST_STR_LEN("Upgrade"));
    /*if (NULL == upgrade) return 0;*//*(checked by caller)*/

    buffer * const http_connection =
      http_header_request_get(r, HTTP_HEADER_CONNECTION,
                              CONST_STR_LEN("Connection"));
    if (NULL == http_connection) {
        http_header_request_unset(r, HTTP_HEADER_UPGRADE,
                                  CONST_STR_LEN("Upgrade"));
        return 0;
    }

    if (r->http_version == HTTP_VERSION_1_1) {
        /* Upgrade: websocket (not handled here)
         * (potentially handled by modules elsewhere) */

        if (!http_header_str_contains_token(BUF_PTR_LEN(upgrade),
                                            CONST_STR_LEN("h2c")))
            return 0; /*(preserve Connection and Upgrade as-is)*/

        /* Upgrade: h2c
         * RFC7540 3.2 Starting HTTP/2 for "http" URIs */

        if (http_header_str_contains_token(BUF_PTR_LEN(http_connection),
                                           CONST_STR_LEN("HTTP2-Settings"))) {
            if (http_dispatch[HTTP_VERSION_2].upgrade_h2c)
                http_dispatch[HTTP_VERSION_2].upgrade_h2c(r, con);
        } /*else ignore Upgrade: h2c; HTTP2-Settings required for Upgrade: h2c*/
        /*(remove "HTTP2-Settings", even if not listed in "Connection")*/
        http_header_request_unset(r, HTTP_HEADER_HTTP2_SETTINGS,
                                  CONST_STR_LEN("HTTP2-Settings"));
        http_header_remove_token(http_connection,
                                 CONST_STR_LEN("HTTP2-Settings"));
    } /*(else invalid with HTTP/1.0; remove Connection and Upgrade)*/

    http_header_request_unset(r, HTTP_HEADER_UPGRADE,
                              CONST_STR_LEN("Upgrade"));
    http_header_remove_token(http_connection, CONST_STR_LEN("Upgrade"));

    if (r->http_version != HTTP_VERSION_2)
        return 0;

    /*(Upgrade: h2c over cleartext does not have SNI; no COMP_HTTP_HOST)*/
    r->conditional_is_valid = (1 << COMP_SERVER_SOCKET)
                            | (1 << COMP_HTTP_REMOTE_IP);
    /*connection_handle_write(r, con);*//* defer write to network */
    return 1;
}


/**
 * handle request header read
 *
 * we get called by the state-engine and by the fdevent-handler
 */
__attribute_noinline__
static int connection_handle_read_state(connection * const con)  {
    /*(should not be reached by HTTP/2 streams)*/
    chunkqueue * const cq = con->read_queue;
    chunk *c = cq->first;
    uint32_t clen = 0;
    uint32_t header_len = 0;
    request_st * const r = &con->request;
    uint8_t keepalive_request_start = 0;
    uint8_t pipelined_request_start = 0;
    uint8_t discard_blank = 0;
    unsigned short hoff[8192]; /* max num header lines + 3; 16k on stack */

    if (con->request_count > 1) {
        discard_blank = 1;
        if (cq->bytes_in == r->x.h1.bytes_read_ckpt) {
            keepalive_request_start = 1;
            if (NULL != c) { /* !chunkqueue_is_empty(cq)) */
                pipelined_request_start = 1;
                /* partial header of next request has already been read,
                 * so optimistically check for more data received on
                 * socket while processing the previous request */
                con->is_readable = 1;
                /*(if partially read next request and unable to read any bytes,
                 * then will unnecessarily scan again before subsequent read)*/
            }
        }
    }

    do {
        if (NULL == c) continue;
        clen = buffer_clen(c->mem) - c->offset;
        if (0 == clen) continue;
        if (__builtin_expect( (c->offset > USHRT_MAX), 0)) /*(highly unlikely)*/
            chunkqueue_compact_mem_offset(cq);

        hoff[0] = 1;                         /* number of lines */
        hoff[1] = (unsigned short)c->offset; /* base offset for all lines */
        /*hoff[2] = ...;*/                   /* offset from base for 2nd line */

        header_len = http_header_parse_hoff(c->mem->ptr + c->offset,clen,hoff);

        /* casting to (unsigned short) might truncate, and the hoff[]
         * addition might overflow, but max_request_field_size is USHRT_MAX,
         * so failure will be detected below */
        const uint32_t max_request_field_size = r->conf.max_request_field_size;
        if ((header_len ? header_len : clen) > max_request_field_size
            || hoff[0] >= sizeof(hoff)/sizeof(hoff[0])-1) {
            log_error(r->conf.errh, __FILE__, __LINE__, "%s",
                      "oversized request-header -> sending Status 431");
            r->http_status = 431; /* Request Header Fields Too Large */
            r->keep_alive = 0;
            return 1;
        }

        if (__builtin_expect( (0 != header_len), 1)) {
            if (__builtin_expect( (hoff[0] > 1), 1))
                break; /* common case; request headers complete */

            if (discard_blank) { /* skip one blank line e.g. following POST */
                if (header_len == clen) continue;
                const int ch = c->mem->ptr[c->offset+header_len];
                if (ch != '\r' && ch != '\n') {
                    /* discard prior blank line if next line is not blank */
                    discard_blank = 0;
                    clen = 0;/*(for connection_read_header_more() to return c)*/
                    c = connection_discard_blank_line(cq, header_len);/*cold*/
                    continue;
                } /*(else fall through to error out in next block)*/
            }
        }

        if (((unsigned char *)c->mem->ptr)[c->offset] < 32) {
            /* expecting ASCII method beginning with alpha char
             * or HTTP/2 pseudo-header beginning with ':' */
            /*(TLS handshake begins with SYN 0x16 (decimal 22))*/
            log_error(r->conf.errh, __FILE__, __LINE__, "%s (%s)",
                      c->mem->ptr[c->offset] == 0x16
                      ? "unexpected TLS ClientHello on clear port"
                      : "invalid request-line -> sending Status 400",
                      con->dst_addr_buf.ptr);
            r->http_status = 400; /* Bad Request */
            r->keep_alive = 0;
            return 1;
        }
    } while ((c = connection_read_header_more(con, cq, c, clen)));

    if (keepalive_request_start) {
        if (cq->bytes_in > r->x.h1.bytes_read_ckpt) {
            /* update r->start_hp.tv_sec timestamp when first byte of
             * next request is received on a keep-alive connection */
            r->start_hp.tv_sec = log_epoch_secs;
            if (r->conf.high_precision_timestamps)
                log_clock_gettime_realtime(&r->start_hp);
        }
        if (pipelined_request_start && c)
            con->read_idle_ts = log_monotonic_secs;
    }

    if (NULL == c) return 0; /* incomplete request headers */

  #ifdef __COVERITY__
    if (buffer_clen(c->mem) < hoff[1]) {
        return 1;
    }
  #endif

    char * const hdrs = c->mem->ptr + hoff[1];

    if (con->request_count > 1) {
        /* adjust r->x.h1.bytes_read_ckpt for http_request_stats_bytes_in()
         * (headers_len is still in cq; marked written, bytes_out incr below) */
        r->x.h1.bytes_read_ckpt = cq->bytes_out;
        /* clear buffers which may have been kept for reporting on keep-alive,
         * (e.g. mod_status) */
        request_reset_ex(r);
    }
    /* RFC7540 3.5 HTTP/2 Connection Preface
     * "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
     * (Connection Preface MUST be exact match)
     * If ALT-SVC used to advertise HTTP/2, then client might start
     * http connection (not TLS) sending HTTP/2 connection preface.
     * (note: intentionally checking only on initial request) */
    else if (!con->is_ssl_sock && r->conf.h2proto
             && hoff[0] == 2 && hoff[2] == 16
             && hdrs[0]=='P' && hdrs[1]=='R' && hdrs[2]=='I' && hdrs[3]==' ') {
        r->http_version = HTTP_VERSION_2;
        return 0;
    }

    r->rqst_header_len = header_len;
    if (r->conf.log_request_header)
        log_error_multiline(r->conf.errh, __FILE__, __LINE__,
                            hdrs, header_len, "fd:%d rqst: ", con->fd);
    http_request_headers_process(r, hdrs, hoff, con->proto_default_port);
    chunkqueue_mark_written(cq, r->rqst_header_len);

    if (light_btst(r->rqst_htags, HTTP_HEADER_UPGRADE)
        && 0 == r->http_status
        && connection_check_upgrade(r, con))
        return 0;

    return 1;
}


static handler_t connection_handle_fdevent(void * const context, const int revents) {
    connection * restrict con = context;
    const int is_ssl_sock = con->is_ssl_sock;

    joblist_append(con);

    if (revents & ~(FDEVENT_IN | FDEVENT_OUT))
        con->revents_err |= (revents & ~(FDEVENT_IN | FDEVENT_OUT));

    if (revents & (FDEVENT_IN | FDEVENT_OUT)) {
        if (is_ssl_sock) /*(ssl may read and write for both reads and writes)*/
            con->is_readable = con->is_writable = 1;
        else {
            if (revents & FDEVENT_IN)
                con->is_readable = 1;
            if (revents & FDEVENT_OUT)
                con->is_writable = 1;
        }
    }

    return HANDLER_FINISHED;
}


__attribute_cold__
static int connection_read_cq_err(connection *con) {
    request_st * const r = &con->request;
  #if defined(__WIN32)
    int lastError = WSAGetLastError();
    switch (lastError) {
    case EAGAIN:
        return 0;
    case EINTR:
        /* we have been interrupted before we could read */
        con->is_readable = 1;
        return 0;
    case ECONNRESET:
        /* suppress logging for this error, expected for keep-alive */
        break;
    default:
        log_error(r->conf.errh, __FILE__, __LINE__,
          "connection closed - recv failed: %d", lastError);
        break;
    }
  #else /* __WIN32 */
    switch (errno) {
    case EAGAIN:
        return 0;
    case EINTR:
        /* we have been interrupted before we could read */
        con->is_readable = 1;
        return 0;
    case ECONNRESET:
        /* suppress logging for this error, expected for keep-alive */
        break;
    default:
        log_perror(r->conf.errh, __FILE__, __LINE__,
          "connection closed - read failed");
        break;
    }
  #endif /* __WIN32 */

    connection_set_state_error(r, CON_STATE_ERROR);
    return -1;
}


/* 0: everything ok, -1: error, -2: con closed */
static int connection_read_cq(connection *con, chunkqueue *cq, off_t max_bytes) {
    ssize_t len;
    size_t mem_len = 0;

    do {
        /* obtain chunk memory into which to read
         * fill previous chunk if it has a reasonable amount of space available
         * (use mem_len=0 to obtain large buffer at least half of chunk_buf_sz)
         */
        chunk *ckpt = cq->last;
        char * const mem = chunkqueue_get_memory(cq, &mem_len);
        if (mem_len > (size_t)max_bytes) mem_len = (size_t)max_bytes;

      #if defined(__WIN32)
        len = recv(con->fd, mem, mem_len, 0);
      #else
        len = read(con->fd, mem, mem_len);
      #endif

        chunkqueue_use_memory(cq, ckpt, len > 0 ? len : 0);

        if (len != (ssize_t)mem_len) {
            /* we got less than expected, wait for the next fd-event */
            con->is_readable = 0;
            return len > 0 ? 0 : 0 == len ? -2 : connection_read_cq_err(con);
        }

        max_bytes -= len;

        int frd;
        mem_len = (0 == fdevent_ioctl_fionread(con->fd, S_IFSOCK, &frd))
          ? (frd < max_bytes) ? (size_t)frd : (size_t)max_bytes
          : 0;
    } while (max_bytes);
    return 0;
}


static int connection_write_cq(connection *con, chunkqueue *cq, off_t max_bytes) {
    request_st * const r = &con->request;
    return con->srv->network_backend_write(con->fd,cq,max_bytes,r->conf.errh);
}


static handler_t connection_handle_read_post_state(request_st * const r);

connection *connection_accepted(server *srv, const server_socket *srv_socket, sock_addr *cnt_addr, int cnt) {
		connection *con;

		srv->cur_fds++;

		/* ok, we have the connection, register it */
#if 0
		log_error(srv->errh, __FILE__, __LINE__, "accepted() %d", cnt);
#endif

		con = connections_get_new_connection(srv);

		con->fd = cnt;
		con->fdn = fdevent_register(srv->ev, con->fd, connection_handle_fdevent, con);
		con->network_read = connection_read_cq;
		con->network_write = connection_write_cq;
		con->reqbody_read = connection_handle_read_post_state;

		request_st * const r = &con->request;
		connection_set_state(r, CON_STATE_REQUEST_START);

		con->connection_start = log_monotonic_secs;
		con->dst_addr = *cnt_addr;
		sock_addr_cache_inet_ntop_copy_buffer(&con->dst_addr_buf,
		                                      &con->dst_addr);
		con->srv_socket = srv_socket;
		con->is_ssl_sock = srv_socket->is_ssl;
		con->proto_default_port = 80; /* "http" */

		config_cond_cache_reset(r);
		r->conditional_is_valid = (1 << COMP_SERVER_SOCKET)
		                        | (1 << COMP_HTTP_REMOTE_IP);

		if (HANDLER_GO_ON != plugins_call_handle_connection_accept(con)) {
			connection_reset(con);
			connection_close(con);
			return NULL;
		}
		if (r->http_status < 0) connection_set_state(r, CON_STATE_WRITE);
		return con;
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
connection_log_state (const request_st * const r, const char * const tag)
{
    buffer * const tb = r->tmp_buf;
    buffer_clear(tb);
    http_request_state_append(tb, r->state);
    log_error(r->conf.errh, __FILE__, __LINE__,
      "fd:%d id:%d state:%s%s", r->con->fd, r->x.h2.id, tb->ptr, tag);
}


static void
connection_state_machine_loop (request_st * const r, connection * const con)
{
	request_state_t ostate;
	do {
		if (r->conf.log_state_handling)
			connection_log_state(r, "");

		switch ((ostate = r->state)) {
		case CON_STATE_REQUEST_START: /* transient */
			/*(should not be reached by HTTP/2 streams)*/
			connection_handle_request_start_state(r, con);
			connection_set_state(r, CON_STATE_READ);
			__attribute_fallthrough__
		case CON_STATE_READ:
			/*(should not be reached by HTTP/2 streams)*/
			if (!connection_handle_read_state(con)) {
				if (r->http_version == HTTP_VERSION_2) {
					connection_transition_h2(r, con);
					connection_state_machine(con);
					ostate = CON_STATE_WRITE;
					continue; /*(end loop if CON_STATE_WRITE)*/
				}
				break;
			}
			/*connection_set_state(r, CON_STATE_REQUEST_END);*/
			/*__attribute_fallthrough__*/
		/*case CON_STATE_REQUEST_END:*//* transient */
			connection_set_state(r,
			  (0 == r->reqbody_length)
			  ? CON_STATE_HANDLE_REQUEST
			  : CON_STATE_READ_POST);
			__attribute_fallthrough__
		case CON_STATE_READ_POST:
		case CON_STATE_HANDLE_REQUEST:
			switch (http_response_handler(r)) {
			  case HANDLER_GO_ON:/*CON_STATE_RESPONSE_START occurred;transient*/
			  case HANDLER_FINISHED:
				break;
			  case HANDLER_WAIT_FOR_EVENT:
				ostate = r->state;
				continue; /*(end outer loop)*/
			  /*case HANDLER_COMEBACK:*//*(not expected)*/
			  /*case HANDLER_ERROR:*/
			  default:
				connection_set_state_error(r, CON_STATE_ERROR);
				continue;
			}
			/*__attribute_fallthrough__*/
		/*case CON_STATE_RESPONSE_START:*//*occurred;transient*/
			http_response_write_header(r);
			connection_set_state(r, CON_STATE_WRITE);
			__attribute_fallthrough__
		case CON_STATE_WRITE:
			if (connection_handle_write_state(r, con) == CON_STATE_WRITE)
				continue; /*(end outer loop)*/
			__attribute_fallthrough__
		case CON_STATE_RESPONSE_END: /* transient */
		case CON_STATE_ERROR:        /* transient */
			connection_handle_response_end_state(r, con);
			/*(make sure ostate will not match r->state)*/
			ostate = CON_STATE_RESPONSE_END;/* != r->state */
			break;
		case CON_STATE_CLOSE:
			/*(should not be reached by HTTP/2 streams)*/
			connection_handle_close_state(con);
			break;
		case CON_STATE_CONNECT:
			break;
		default:/*(should not happen)*/
			/*connection_log_state(r, "");*/ /*(unknown state)*/
			break;
		}
	} while (ostate != r->state);

        if (r->conf.log_state_handling)
            connection_log_state(r, " at loop exit");
}


__attribute_cold__
static void
connection_revents_err (request_st * const r, connection * const con)
{
    /* defer handling FDEVENT_HUP and FDEVENT_ERR to here in order to
     * first attempt (in callers) to read data in kernel socket buffers */
    /*assert(con->revents_err & ~(FDEVENT_IN | FDEVENT_OUT));*/
    const int revents = (int)con->revents_err;
    con->revents_err = 0;

    if (r->state == CON_STATE_CLOSE)
        con->close_timeout_ts = log_monotonic_secs - (HTTP_LINGER_TIMEOUT+1);
    else if (revents & FDEVENT_HUP)
        connection_set_state_error(r, CON_STATE_ERROR);
    else if (revents & FDEVENT_RDHUP) {
        int events = fdevent_fdnode_interest(con->fdn);
        events &= ~(FDEVENT_IN|FDEVENT_RDHUP);
        r->conf.stream_request_body &=
          ~(FDEVENT_STREAM_REQUEST_BUFMIN|FDEVENT_STREAM_REQUEST_POLLIN);
        r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_POLLRDHUP;
        con->is_readable = 1; /*(can read 0 for end-of-stream)*/
        if (chunkqueue_is_empty(con->read_queue)) r->keep_alive = 0;
        if (r->reqbody_length < -1)/*(transparent proxy mode; no more rd data)*/
            r->reqbody_length = r->reqbody_queue.bytes_in;
        if (sock_addr_get_family(&con->dst_addr) == AF_UNIX) {
            /* future: will getpeername() on AF_UNIX check if still connected?*/
            fdevent_fdnode_event_set(con->srv->ev, con->fdn, events);
        }
        else if (fdevent_is_tcp_half_closed(con->fd)) {
            /* Success of fdevent_is_tcp_half_closed() after FDEVENT_RDHUP
             * indicates TCP FIN received, but does not distinguish between
             * client shutdown(fd, SHUT_WR) and client close(fd).  Remove
             * FDEVENT_RDHUP so that we do not spin on ready event.  However,
             * a later TCP RST will not be detected until next write to socket.
             * future: might getpeername() to check for TCP RST on half-closed
             * sockets (without FDEVENT_RDHUP interest) when checking for write
             * timeouts once a second in server.c, though getpeername() on
             * Windows might not indicate this */
            r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_TCP_FIN;
            fdevent_fdnode_event_set(con->srv->ev, con->fdn, events);
        }
        else {
            /* Failure of fdevent_is_tcp_half_closed() indicates TCP RST
             * (or unable to tell (unsupported OS), though should not
             * be setting FDEVENT_RDHUP in that case) */
            connection_set_state_error(r, CON_STATE_ERROR);
        }
    }
    else if (revents & FDEVENT_ERR)  /* error, connection reset */
        connection_set_state_error(r, CON_STATE_ERROR);
    else
        log_error(r->conf.errh, __FILE__, __LINE__,
          "connection closed: poll() -> ??? %d", revents);
}


static void
connection_set_fdevent_interest (request_st * const r, connection * const con)
{
    if (con->fd < 0) return;

    if (con->revents_err && r->state != CON_STATE_ERROR) {
        connection_revents_err(r, con); /* resets con->revents_err = 0 */
        connection_state_machine(con);
        return;
        /* connection_state_machine() will end up calling back into
         * connection_set_fdevent_interest(), but with 0 == con->revents_err */
    }

    int n = 0;
    switch(r->state) {
      case CON_STATE_READ:
        n = FDEVENT_IN;
        if (!(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_POLLRDHUP))
            n |= FDEVENT_RDHUP;
        break;
      case CON_STATE_WRITE:
        if (!chunkqueue_is_empty(con->write_queue)
            && 0 == con->is_writable && 0 == con->traffic_limit_reached)
            n |= FDEVENT_OUT;
        __attribute_fallthrough__
      case CON_STATE_READ_POST:
        if (r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_POLLIN)
            n |= FDEVENT_IN;
        if (!(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_POLLRDHUP))
            n |= FDEVENT_RDHUP;
        break;
      case CON_STATE_CLOSE:
        n = FDEVENT_IN;
        break;
      case CON_STATE_CONNECT:
        return;
      default:
        break;
    }

    const int events = fdevent_fdnode_interest(con->fdn);
    if (con->is_readable < 0) {
        con->is_readable = 0;
        n |= FDEVENT_IN;
    }
    if (con->is_writable < 0) {
        con->is_writable = 0;
        n |= FDEVENT_OUT;
    }
    if (events & FDEVENT_RDHUP)
        n |= FDEVENT_RDHUP;

    if (n == events) return;

    /* update timestamps when enabling interest in events */
    if ((n & FDEVENT_IN) && !(events & FDEVENT_IN))
        con->read_idle_ts = log_monotonic_secs;
    if ((n & FDEVENT_OUT) && !(events & FDEVENT_OUT))
        con->write_request_ts = log_monotonic_secs;
    fdevent_fdnode_event_set(con->srv->ev, con->fdn, n);
}


void
connection_state_machine (connection * const con)
{
    int rc = !con->fn || con->fn->process_streams(con, http_response_handler,
                                                       connection_handle_write);
    request_st * const r = &con->request;
    if (rc)
        connection_state_machine_loop(r, con);
    connection_set_fdevent_interest(r, con);
}


static void connection_check_timeout (connection * const con, const unix_time64_t cur_ts) {
    const int waitevents = fdevent_fdnode_interest(con->fdn);
    int changed = 0;
    int t_diff;

    request_st * const r = &con->request;
    if (r->state == CON_STATE_CLOSE) {
        if (cur_ts - con->close_timeout_ts > HTTP_LINGER_TIMEOUT) {
            changed = 1;
        }
    }
    else if (con->fn && r->state == CON_STATE_WRITE) {
        changed = con->fn->check_timeout(con, cur_ts);
    }
    else if (waitevents & FDEVENT_IN) {
        if (con->request_count == 1 || r->state != CON_STATE_READ) {
            /* e.g. CON_STATE_READ_POST || CON_STATE_WRITE */
            if (cur_ts - con->read_idle_ts > r->conf.max_read_idle) {
                /* time - out */
                if (r->conf.log_request_handling) {
                    log_error(r->conf.errh, __FILE__, __LINE__,
                              "connection closed - read timeout: %d", con->fd);
                }

                connection_set_state_error(r, CON_STATE_ERROR);
                changed = 1;
            }
        } else {
            if (cur_ts - con->read_idle_ts > con->keep_alive_idle) {
                /* time - out */
                if (r->conf.log_request_handling) {
                    log_error(r->conf.errh, __FILE__, __LINE__,
                              "connection closed - keep-alive timeout: %d",
                              con->fd);
                }

                connection_set_state_error(r, CON_STATE_ERROR);
                changed = 1;
            }
        }
    }

    /* max_write_idle timeout currently functions as backend timeout,
     * too, after response has been started.
     * Although backend timeouts now exist, there is no default for timeouts
     * to backends, so were this client timeout now to be changed to check
     * for write interest to the client, then timeout would not occur if the
     * backend hung and there was no backend read timeout set.  Therefore,
     * max_write_idle timeout remains timeout for both reading from backend
     * and writing to client, though this check here is only for HTTP/1.1.
     * In the future, if there were a quick way to detect that a backend
     * read timeout was in effect, then this timeout could check for write
     * interest to client.  (not a priority) */
    /*if (waitevents & FDEVENT_OUT)*/
    if (r->http_version <= HTTP_VERSION_1_1
        && r->state == CON_STATE_WRITE && con->write_request_ts != 0) {
      #if 0
        if (cur_ts - con->write_request_ts > 60) {
            log_error(r->conf.errh, __FILE__, __LINE__,
                      "connection closed - pre-write-request-timeout: %d %d",
                      con->fd, cur_ts - con->write_request_ts);
        }
      #endif

        if (cur_ts - con->write_request_ts > r->conf.max_write_idle) {
            /* time - out */
            if (r->conf.log_timeouts) {
                log_error(r->conf.errh, __FILE__, __LINE__,
                  "NOTE: a request from %s for %.*s timed out after writing "
                  "%lld bytes. We waited %d seconds. If this is a problem, "
                  "increase server.max-write-idle",
                  r->dst_addr_buf->ptr,
                  BUFFER_INTLEN_PTR(&r->target),
                  (long long)con->write_queue->bytes_out,
                  (int)r->conf.max_write_idle);
            }
            connection_set_state_error(r, CON_STATE_ERROR);
            changed = 1;
        }
    }

    /* lighttpd HTTP/2 limitation: rate limit config r->conf.bytes_per_second
     * (currently) taken only from top-level config (socket), with host if SNI
     * used, but not any other config conditions, e.g. not per-file-type */

    if (0 == (t_diff = cur_ts - con->connection_start)) t_diff = 1;

    if (con->traffic_limit_reached &&
        (r->conf.bytes_per_second == 0
         || con->write_queue->bytes_out
              < (off_t)r->conf.bytes_per_second * t_diff)) {
        /* enable connection again */
        con->traffic_limit_reached = 0;

        changed = 1;
    }

    con->bytes_written_cur_second = 0;

    if (changed) {
        connection_state_machine(con);
    }
}

void connection_periodic_maint (server * const srv, const unix_time64_t cur_ts) {
    /* check all connections for timeouts */
    for (connection *con = srv->conns, *tc; con; con = tc) {
        tc = con->next;
        connection_check_timeout(con, cur_ts);
    }
}

void connection_graceful_shutdown_maint (server *srv) {
    const int graceful_expire =
      (srv->graceful_expire_ts && srv->graceful_expire_ts < log_monotonic_secs);
    for (connection *con = srv->conns, *tc; con; con = tc) {
        tc = con->next;
        int changed = 0;

        request_st * const r = &con->request;
        if (r->state == CON_STATE_CLOSE) {
            /* reduce remaining linger timeout to be
             * (from zero) *up to* one more second, but no more */
            if (HTTP_LINGER_TIMEOUT > 1)
                con->close_timeout_ts -= (HTTP_LINGER_TIMEOUT - 1);
            if (log_monotonic_secs - con->close_timeout_ts > HTTP_LINGER_TIMEOUT)
                changed = 1;
        }
        else if (con->fn) {
            if (con->fn->goaway_graceful(con))
                changed = 1;
        }
        else if (r->state == CON_STATE_READ && con->request_count > 1
                 && chunkqueue_is_empty(con->read_queue)) {
            /* close connections in keep-alive waiting for next request */
            connection_set_state_error(r, CON_STATE_ERROR);
            changed = 1;
        }

        if (graceful_expire) {
            connection_set_state_error(r, CON_STATE_ERROR);
            changed = 1;
        }

        r->keep_alive = 0;            /* disable keep-alive */

        r->conf.bytes_per_second = 0;         /* disable rate limit */
        r->conf.global_bytes_per_second = 0;  /* disable rate limit */
        if (con->traffic_limit_reached) {
            con->traffic_limit_reached = 0;
            changed = 1;
        }

        if (changed) {
            connection_state_machine(con);
        }
    }
}


static int
connection_handle_read_post_cq_compact (chunkqueue * const cq)
{
    /* combine first mem chunk with next non-empty mem chunk
     * (loop if next chunk is empty) */
    chunk *c = cq->first;
    if (NULL == c) return 0;
    const uint32_t mlen = buffer_clen(c->mem) - (size_t)c->offset;
    while ((c = c->next)) {
        const uint32_t blen = buffer_clen(c->mem) - (size_t)c->offset;
        if (0 == blen) continue;
        chunkqueue_compact_mem(cq, mlen + blen);
        return 1;
    }
    return 0;
}


__attribute_pure__
static int
connection_handle_read_post_chunked_crlf (chunkqueue * const cq)
{
    /* caller might check chunkqueue_length(cq) >= 2 before calling here
     * to limit return value to either 1 for good or -1 for error */
    chunk *c;
    buffer *b;
    char *p;
    size_t len;

    /* caller must have called chunkqueue_remove_finished_chunks(cq), so if
     * chunkqueue is not empty, it contains chunk with at least one char */
    if (chunkqueue_is_empty(cq)) return 0;

    c = cq->first;
    b = c->mem;
    p = b->ptr+c->offset;
    if (p[0] != '\r') return -1; /* error */
    if (p[1] == '\n') return 1;
    len = buffer_clen(b) - (size_t)c->offset;
    if (1 != len) return -1; /* error */

    while (NULL != (c = c->next)) {
        b = c->mem;
        len = buffer_clen(b) - (size_t)c->offset;
        if (0 == len) continue;
        p = b->ptr+c->offset;
        return (p[0] == '\n') ? 1 : -1; /* error if not '\n' */
    }
    return 0;
}


static handler_t
connection_handle_read_post_chunked (request_st * const r, chunkqueue * const cq, chunkqueue * const dst_cq)
{
    /* r->conf.max_request_size is in kBytes */
    const off_t max_request_size = (off_t)r->conf.max_request_size << 10;
    off_t te_chunked = r->x.h1.te_chunked;
    do {
        off_t len = chunkqueue_length(cq);

        while (0 == te_chunked) {
            char *p;
            chunk *c = cq->first;
            if (NULL == c) break;
            force_assert(c->type == MEM_CHUNK);
            p = strchr(c->mem->ptr+c->offset, '\n');
            if (NULL != p) { /* found HTTP chunked header line */
                off_t hsz = p + 1 - (c->mem->ptr+c->offset);
                unsigned char *s = (unsigned char *)c->mem->ptr+c->offset;
                for (unsigned char u;(u=(unsigned char)hex2int(*s))!=0xFF;++s) {
                    if (te_chunked > (off_t)(1uLL<<(8*sizeof(off_t)-5))-1-2) {
                        log_error(r->conf.errh, __FILE__, __LINE__,
                          "chunked data size too large -> 400");
                        /* 400 Bad Request */
                        return http_response_reqbody_read_error(r, 400);
                    }
                    te_chunked <<= 4;
                    te_chunked |= u;
                }
                if (s == (unsigned char *)c->mem->ptr+c->offset) { /*(no hex)*/
                    log_error(r->conf.errh, __FILE__, __LINE__,
                      "chunked header invalid chars -> 400");
                    /* 400 Bad Request */
                    return http_response_reqbody_read_error(r, 400);
                }
                while (*s == ' ' || *s == '\t') ++s;
                if (*s != '\r' && *s != ';') {
                    log_error(r->conf.errh, __FILE__, __LINE__,
                      "chunked header invalid chars -> 400");
                    /* 400 Bad Request */
                    return http_response_reqbody_read_error(r, 400);
                }

                if (hsz >= 1024) {
                    /* prevent theoretical integer overflow
                     * casting to (size_t) and adding 2 (for "\r\n") */
                    log_error(r->conf.errh, __FILE__, __LINE__,
                      "chunked header line too long -> 400");
                    /* 400 Bad Request */
                    return http_response_reqbody_read_error(r, 400);
                }

                if (0 == te_chunked) {
                    /* do not consume final chunked header until
                     * (optional) trailers received along with
                     * request-ending blank line "\r\n" */
                    if (p[0] == '\r' && p[1] == '\n') {
                        /*(common case with no trailers; final \r\n received)*/
                        hsz += 2;
                    }
                    else {
                        /* trailers or final CRLF crosses into next cq chunk */
                        hsz -= 2;
                        do {
                            c = cq->first;
                            p = strstr(c->mem->ptr+c->offset+hsz, "\r\n\r\n");
                        } while (NULL == p
                                 && connection_handle_read_post_cq_compact(cq));
                        if (NULL == p) {
                            /*(effectively doubles max request field size
                             * potentially received by backend, if in the future
                             * these trailers are added to request headers)*/
                            if ((off_t)buffer_clen(c->mem) - c->offset
                                < (off_t)r->conf.max_request_field_size) {
                                break;
                            }
                            else {
                                /* ignore excessively long trailers;
                                 * disable keep-alive on connection */
                                r->keep_alive = 0;
                                p = c->mem->ptr + buffer_clen(c->mem)
                                  - 4;
                            }
                        }
                        hsz = p + 4 - (c->mem->ptr+c->offset);
                        /* trailers currently ignored, but could be processed
                         * here if 0 == (r->conf.stream_request_body &
                         *               & (FDEVENT_STREAM_REQUEST
                         *                 |FDEVENT_STREAM_REQUEST_BUFMIN))
                         * taking care to reject fields forbidden in trailers,
                         * making trailers available to CGI and other backends*/
                    }
                    chunkqueue_mark_written(cq, (size_t)hsz);
                    r->reqbody_length = dst_cq->bytes_in;
                    break; /* done reading HTTP chunked request body */
                }

                /* consume HTTP chunked header */
                chunkqueue_mark_written(cq, (size_t)hsz);
                len = chunkqueue_length(cq);

                if (0 !=max_request_size
                    && (max_request_size < te_chunked
                     || max_request_size - te_chunked < dst_cq->bytes_in)) {
                    log_error(r->conf.errh, __FILE__, __LINE__,
                      "request-size too long: %lld -> 413",
                      (long long)(dst_cq->bytes_in + te_chunked));
                    /* 413 Payload Too Large */
                    return http_response_reqbody_read_error(r, 413);
                }

                te_chunked += 2; /*(for trailing "\r\n" after chunked data)*/

                break; /* read HTTP chunked header */
            }

            /*(likely better ways to handle chunked header crossing chunkqueue
             * chunks, but this situation is not expected to occur frequently)*/
            if ((off_t)buffer_clen(c->mem) - c->offset >= 1024) {
                log_error(r->conf.errh, __FILE__, __LINE__,
                  "chunked header line too long -> 400");
                /* 400 Bad Request */
                return http_response_reqbody_read_error(r, 400);
            }
            else if (!connection_handle_read_post_cq_compact(cq)) {
                break;
            }
        }
        if (0 == te_chunked) break;

        if (te_chunked > 2) {
            if (len > te_chunked-2) len = te_chunked-2;
            if (dst_cq->bytes_in + te_chunked <= 64*1024) {
                /* avoid buffering request bodies <= 64k on disk */
                chunkqueue_steal(dst_cq, cq, len);
            }
            else if (0 != chunkqueue_steal_with_tempfiles(dst_cq, cq, len,
                                                          r->conf.errh)) {
                /* 500 Internal Server Error */
                return http_response_reqbody_read_error(r, 500);
            }
            te_chunked -= len;
            len = chunkqueue_length(cq);
        }

        if (len < te_chunked) break;

        if (2 == te_chunked) {
            if (-1 == connection_handle_read_post_chunked_crlf(cq)) {
                log_error(r->conf.errh, __FILE__, __LINE__,
                  "chunked data missing end CRLF -> 400");
                /* 400 Bad Request */
                return http_response_reqbody_read_error(r, 400);
            }
            chunkqueue_mark_written(cq, 2);/*consume \r\n at end of chunk data*/
            te_chunked -= 2;
        }

    } while (!chunkqueue_is_empty(cq));

    r->x.h1.te_chunked = te_chunked;
    return HANDLER_GO_ON;
}


static handler_t
connection_handle_read_body_unknown (request_st * const r, chunkqueue * const cq, chunkqueue * const dst_cq)
{
    /* r->conf.max_request_size is in kBytes */
    const off_t max_request_size = (off_t)r->conf.max_request_size << 10;
    chunkqueue_append_chunkqueue(dst_cq, cq);
    if (0 != max_request_size && dst_cq->bytes_in > max_request_size) {
        log_error(r->conf.errh, __FILE__, __LINE__,
          "request-size too long: %lld -> 413", (long long)dst_cq->bytes_in);
        /* 413 Payload Too Large */
        return http_response_reqbody_read_error(r, 413);
    }
    return HANDLER_GO_ON;
}


__attribute_cold__
static int
connection_check_expect_100 (request_st * const r, connection * const con)
{
    if (con->is_writable <= 0)
        return 1;

    const buffer * const vb =
      http_header_request_get(r, HTTP_HEADER_EXPECT,
                              CONST_STR_LEN("Expect"));
    if (NULL == vb)
        return 1;

    /* (always unset Expect header so that check is not repeated for request */
    int rc = buffer_eq_icase_slen(vb, CONST_STR_LEN("100-continue"));
    http_header_request_unset(r, HTTP_HEADER_EXPECT,
                              CONST_STR_LEN("Expect"));
    if (!rc
        || 0 != r->reqbody_queue.bytes_in
        || !chunkqueue_is_empty(&r->read_queue)
        || !chunkqueue_is_empty(&r->write_queue)
        || r->http_version == HTTP_VERSION_1_0)
        return 1;

    /* send 100 Continue only if no request body data received yet
     * and response has not yet started (checked above) */
    return connection_write_100_continue(r, con);
}


static handler_t
connection_handle_read_post_state (request_st * const r)
{
    connection * const con = r->con;
    chunkqueue * const cq = &r->read_queue;
    chunkqueue * const dst_cq = &r->reqbody_queue;

    int is_closed = 0;

    if (con->is_readable > 0) {
        con->read_idle_ts = log_monotonic_secs;
        const off_t max_per_read =
          !(r->conf.stream_request_body /*(if not streaming request body)*/
            & (FDEVENT_STREAM_REQUEST|FDEVENT_STREAM_REQUEST_BUFMIN))
            ? MAX_READ_LIMIT
            : (r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_BUFMIN)
              ? 16384  /* FDEVENT_STREAM_REQUEST_BUFMIN */
              : 65536; /* FDEVENT_STREAM_REQUEST */
        switch(con->network_read(con, cq, max_per_read)) {
        case -1:
            connection_set_state_error(r, CON_STATE_ERROR);
            return HANDLER_ERROR;
        case -2:
            is_closed = 1;
            break;
        default:
            break;
        }

        chunkqueue_remove_finished_chunks(cq);
    }

    /* Check for Expect: 100-continue in request headers */
    if (light_btst(r->rqst_htags, HTTP_HEADER_EXPECT)
        && !connection_check_expect_100(r, con))
        return HANDLER_ERROR;

    if (r->reqbody_length < 0) {
        /*(-1: Transfer-Encoding: chunked, -2: unspecified length)*/
        handler_t rc = (-1 == r->reqbody_length)
                     ? connection_handle_read_post_chunked(r, cq, dst_cq)
                     : connection_handle_read_body_unknown(r, cq, dst_cq);
        if (HANDLER_GO_ON != rc) return rc;
        chunkqueue_remove_finished_chunks(cq);
    }
    else {
        off_t len = (off_t)r->reqbody_length - dst_cq->bytes_in;
        if (r->reqbody_length <= 64*1024) {
            /* don't buffer request bodies <= 64k on disk */
            chunkqueue_steal(dst_cq, cq, len);
        }
        else if (chunkqueue_length(dst_cq) + len <= 64*1024
                 && (!dst_cq->first || dst_cq->first->type == MEM_CHUNK)) {
            /* avoid tempfiles when streaming request body to fast backend */
            chunkqueue_steal(dst_cq, cq, len);
        }
        else if (0 !=
                 chunkqueue_steal_with_tempfiles(dst_cq,cq,len,r->conf.errh)) {
            /* writing to temp file failed */ /* Internal Server Error */
            return http_response_reqbody_read_error(r, 500);
        }
        chunkqueue_remove_finished_chunks(cq);
    }

    if (dst_cq->bytes_in == (off_t)r->reqbody_length) {
        /* Content is ready */
        r->conf.stream_request_body &= ~FDEVENT_STREAM_REQUEST_POLLIN;
        if (r->state == CON_STATE_READ_POST) {
            connection_set_state(r, CON_STATE_HANDLE_REQUEST);
        }
        return HANDLER_GO_ON;
    }
    else if (is_closed) {
      #if 0
        return http_response_reqbody_read_error(r, 400); /* Bad Request */
      #endif
        return HANDLER_ERROR;
    }
    else {
        r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_POLLIN;
        return (r->conf.stream_request_body & FDEVENT_STREAM_REQUEST)
          ? HANDLER_GO_ON
          : HANDLER_WAIT_FOR_EVENT;
    }
}
