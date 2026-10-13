/*
 * vfio protocol over a UNIX socket.
 *
 * Copyright © 2018, 2021 Oracle and/or its affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>

#include "hw/vfio/vfio-device.h"
#include "hw/vfio-user/proxy.h"
#include "hw/vfio-user/trace.h"
#include "qapi/error.h"
#include "qobject/qbool.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/iothread.h"

static IOThread *vfio_user_iothread;

static void vfio_user_shutdown(VFIOUserProxy *proxy);
static VFIOUserMsg *vfio_user_getmsg(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                                     VFIOUserFDs *fds);
static void vfio_user_recycle(VFIOUserProxy *proxy, VFIOUserMsg *msg);

static void vfio_user_recv(void *opaque);
static void vfio_user_send(void *opaque);

static inline void vfio_user_set_error(VFIOUserHdr *hdr, uint32_t err)
{
    hdr->flags |= VFIO_USER_ERROR;
    hdr->error_reply = err;
}

/*
 * Functions called by main, CPU, or iothread threads
 */

static void vfio_user_shutdown(VFIOUserProxy *proxy)
{
    qio_channel_shutdown(proxy->ioc, QIO_CHANNEL_SHUTDOWN_READ, NULL);
    qio_channel_set_aio_fd_handler(proxy->ioc, proxy->ctx, NULL,
                                   proxy->ctx, NULL, NULL);
}

/*
 * Same return values as qio_channel_writev_full():
 *
 * QIO_CHANNEL_ERR_BLOCK: *errp not set
 * -1: *errp will be populated
 * otherwise: bytes written
 */
static ssize_t vfio_user_send_qio(VFIOUserProxy *proxy, VFIOUserMsg *msg,
                                  Error **errp)
{
    VFIOUserFDs *fds =  msg->fds;
    struct iovec iov = {
        .iov_base = msg->hdr,
        .iov_len = msg->hdr->size,
    };
    size_t numfds = 0;
    int *fdp = NULL;
    ssize_t ret;

    if (fds != NULL && fds->send_fds != 0) {
        numfds = fds->send_fds;
        fdp = fds->fds;
    }

    ret = qio_channel_writev_full(proxy->ioc, &iov, 1, fdp, numfds, 0, errp);

    if (ret == -1) {
        vfio_user_set_error(msg->hdr, EIO);
        vfio_user_shutdown(proxy);
    }
    trace_vfio_user_send_write(msg->hdr->id, ret);

    return ret;
}

static VFIOUserMsg *vfio_user_getmsg(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                                     VFIOUserFDs *fds)
{
    VFIOUserMsg *msg;

    msg = QTAILQ_FIRST(&proxy->free);
    if (msg != NULL) {
        QTAILQ_REMOVE(&proxy->free, msg, next);
    } else {
        msg = g_malloc0(sizeof(*msg));
        qemu_cond_init(&msg->cv);
    }

    msg->hdr = hdr;
    msg->fds = fds;
    return msg;
}

/*
 * Recycle a message list entry to the free list.
 */
static void vfio_user_recycle(VFIOUserProxy *proxy, VFIOUserMsg *msg)
{
    if (msg->type == VFIO_MSG_NONE) {
        error_printf("vfio_user_recycle - freeing free msg\n");
        return;
    }

    /* free msg buffer if no one is waiting to consume the reply */
    if (msg->type == VFIO_MSG_NOWAIT || msg->type == VFIO_MSG_ASYNC ||
        msg->type == VFIO_MSG_DISCARD) {
        g_free(msg->hdr);
        if (msg->fds != NULL) {
            g_free(msg->fds);
        }
    }

    msg->type = VFIO_MSG_NONE;
    msg->hdr = NULL;
    msg->fds = NULL;
    msg->complete = false;
    msg->pending = false;
    QTAILQ_INSERT_HEAD(&proxy->free, msg, next);
}

VFIOUserFDs *vfio_user_getfds(int numfds)
{
    VFIOUserFDs *fds = g_malloc0(sizeof(*fds) + (numfds * sizeof(int)));

    fds->fds = (int *)((char *)fds + sizeof(*fds));

    return fds;
}

/*
 * Stop waiting for the reply to @msg.  If that reply is being read right
 * now, finish reading it into a private buffer, as the waiter owns msg->hdr.
 */
static void vfio_user_abandon(VFIOUserProxy *proxy, VFIOUserMsg *msg)
{
    VFIOUserMsg *discard;

    if (proxy->part_recv == msg) {
        discard = vfio_user_getmsg(proxy,
                                   g_memdup2(msg->hdr, msg->hdr->size), NULL);
        discard->type = VFIO_MSG_DISCARD;
        proxy->part_recv = discard;
    } else if (msg->pending) {
        QTAILQ_REMOVE(&proxy->pending, msg, next);
    } else {
        QTAILQ_REMOVE(&proxy->outgoing, msg, next);
        proxy->num_outgoing--;
    }
}

/*
 * Socket is read unless a request is waiting to be handled in the main loop,
 * and written while there are outgoing messages.
 */
static void vfio_user_set_fd_handlers(VFIOUserProxy *proxy)
{
    bool reading = proxy->req_deferred == NULL;
    bool writing = !QTAILQ_EMPTY(&proxy->outgoing);

    qio_channel_set_aio_fd_handler(proxy->ioc,
                                   proxy->ctx, reading ? vfio_user_recv : NULL,
                                   proxy->ctx, writing ? vfio_user_send : NULL,
                                   proxy);
}

/*
 * Free a request that won't be handled.
 */
static void vfio_user_drop_request(VFIOUserMsg *msg)
{
    if (msg->fds != NULL) {
        vfio_user_putfds(msg);
    }
    g_free(msg->hdr);
    msg->hdr = NULL;
}

/*
 * Fail a request without handling it.
 */
static void vfio_user_reject(VFIOUserProxy *proxy, VFIOUserMsg *msg, int err)
{
    if (msg->hdr->flags & VFIO_USER_NO_REPLY) {
        vfio_user_drop_request(msg);
        return;
    }
    if (msg->fds != NULL) {
        vfio_user_putfds(msg);
    }
    vfio_user_send_error(proxy, msg->hdr, err);
}

/*
 * Release a request message entry once its buffer has been disposed of.
 */
static void vfio_user_put_request(VFIOUserProxy *proxy, VFIOUserMsg *msg)
{
    /* vfio_user_disconnect() may have freed the free list meanwhile */
    if (proxy->state == VFIO_PROXY_CLOSING) {
        qemu_cond_destroy(&msg->cv);
        g_free(msg);
    } else {
        vfio_user_recycle(proxy, msg);
    }
}

/*
 * Complete the deferred request, see vfio_user_dispatch(): handle it if
 * @handle, otherwise fail it, for contexts where device DMA can't be
 * handled safely.  Called with the BQL held.
 */
static void vfio_user_finish_deferred(VFIOUserProxy *proxy, bool handle)
{
    bool (*request)(void *opaque, VFIOUserMsg *msg) = proxy->request;
    VFIOUserMsg *msg = proxy->req_deferred;
    void *req_arg = proxy->req_arg;

    assert(bql_locked());

    proxy->req_running = true;
    qemu_mutex_unlock(&proxy->lock);

    if (!handle) {
        trace_vfio_user_fail_deferred(msg->hdr->command);
        vfio_user_reject(proxy, msg, EBUSY);
    } else if (request == NULL || !request(req_arg, msg)) {
        vfio_user_reject(proxy, msg, ENODEV);
    }

    qemu_mutex_lock(&proxy->lock);
    proxy->req_running = false;
    proxy->req_deferred = NULL;
    vfio_user_put_request(proxy, msg);
    if (proxy->state == VFIO_PROXY_CONNECTED) {
        vfio_user_set_fd_handlers(proxy);
    }
}

/*
 * Dispose of a message that will never be completely received.
 */
static void vfio_user_drop_part_recv(VFIOUserProxy *proxy, int err)
{
    VFIOUserMsg *msg = proxy->part_recv;

    if (msg == NULL) {
        return;
    }
    proxy->part_recv = NULL;
    proxy->recv_left = 0;

    switch (msg->type) {
    case VFIO_MSG_REQ:
        vfio_user_drop_request(msg);
        vfio_user_put_request(proxy, msg);
        break;
    case VFIO_MSG_DISCARD:
        /* received FDs are ours to close, the buffer is freed on recycle */
        if (msg->fds != NULL) {
            vfio_user_putfds(msg);
        }
        vfio_user_recycle(proxy, msg);
        break;
    case VFIO_MSG_WAIT:
        /* the waiter owns the buffer and recycles the entry */
        vfio_user_set_error(msg->hdr, err);
        msg->complete = true;
        qemu_cond_signal(&msg->cv);
        break;
    default:
        if (proxy->last_nowait == msg) {
            proxy->last_nowait = NULL;
        }
        vfio_user_recycle(proxy, msg);
    }
}

/*
 * Functions only called by iothread
 */

/*
 * Handle a request from the server.
 *
 * Requests are handled in the order received, before any later message is
 * read, so that e.g. a DMA write is visible by the time a subsequent reply
 * wakes up its waiter.
 *
 * The handler runs without the BQL: a vCPU holding it may be waiting for a
 * reply that the server only sends once this request completes.  The proxy
 * lock is dropped as well, since the handler sends its own reply.
 *
 * The bus-specific handler has the form:
 *    request(opaque, msg)
 * where 'opaque' was specified in vfio_user_set_handler
 * and 'msg' is the inbound message.
 *
 * The handler is responsible for disposing of the message buffer,
 * usually by re-using it when calling vfio_send_reply or vfio_send_error,
 * both of which free their message buffer when the reply is sent.
 *
 * If the handler uses a new buffer, it needs to free the old one.
 *
 * If the handler needs the BQL, e.g. for DMA to MMIO, it returns false
 * without disposing of the message.  No further messages are read until
 * that request completes, which happens either in the main loop, or, since
 * the main loop can't run then, in a thread that holds the BQL while waiting
 * for a reply from the server; see vfio_user_wait_reply().
 */
static void vfio_user_dispatch(VFIOUserProxy *proxy, VFIOUserMsg *msg)
{
    bool (*request)(void *opaque, VFIOUserMsg *msg) = proxy->request;
    void *req_arg = proxy->req_arg;
    bool handled = true;

    trace_vfio_user_recv_request(msg->hdr->command);

    proxy->req_active = true;
    qemu_mutex_unlock(&proxy->lock);

    if (request != NULL) {
        handled = request(req_arg, msg);
    } else {
        vfio_user_reject(proxy, msg, ENODEV);
    }

    qemu_mutex_lock(&proxy->lock);
    proxy->req_active = false;
    qemu_cond_broadcast(&proxy->req_cv);

    if (!handled) {
        if (proxy->state == VFIO_PROXY_CONNECTED) {
            trace_vfio_user_defer_request(msg->hdr->command);
            proxy->req_deferred = msg;
            vfio_user_set_fd_handlers(proxy);
            qemu_bh_schedule(proxy->req_bh);
            if (proxy->bql_waiter != NULL) {
                qemu_cond_signal(&proxy->bql_waiter->cv);
            }
            return;
        }
        vfio_user_drop_request(msg);
    }

    vfio_user_put_request(proxy, msg);
}

/*
 * Process a received message.
 *
 * Handling a request drops the proxy lock, so callers must re-check the
 * proxy state afterwards.
 */
static void vfio_user_process(VFIOUserProxy *proxy, VFIOUserMsg *msg)
{

    /*
     * Replies signal a waiter, if none just check for errors
     * and free the message buffer.
     *
     * Requests are handled right away.
     */
    if (msg->type == VFIO_MSG_DISCARD) {
        if (msg->fds != NULL) {
            vfio_user_putfds(msg);
        }
        vfio_user_recycle(proxy, msg);
    } else if ((msg->hdr->flags & VFIO_USER_TYPE) == VFIO_USER_REPLY) {
        msg->complete = true;
        if (msg->type == VFIO_MSG_WAIT) {
            qemu_cond_signal(&msg->cv);
        } else {
            if (msg->hdr->flags & VFIO_USER_ERROR) {
                error_printf("vfio_user_process: error reply on async ");
                error_printf("request command %x error %s\n",
                             msg->hdr->command,
                             strerror(msg->hdr->error_reply));
            }
            /* youngest nowait msg has been ack'd */
            if (proxy->last_nowait == msg) {
                proxy->last_nowait = NULL;
            }
            vfio_user_recycle(proxy, msg);
        }
    } else {
        vfio_user_dispatch(proxy, msg);
    }
}

/*
 * Complete a partial message read
 */
static int vfio_user_complete(VFIOUserProxy *proxy, Error **errp)
{
    VFIOUserMsg *msg = proxy->part_recv;
    size_t msgleft = proxy->recv_left;
    char *data;
    int ret;

    data = (char *)msg->hdr + (msg->hdr->size - msgleft);
    while (msgleft > 0) {
        ret = qio_channel_read(proxy->ioc, data, msgleft, errp);

        /* error or would block */
        if (ret <= 0) {
            /* try for rest on next iternation */
            if (ret == QIO_CHANNEL_ERR_BLOCK) {
                proxy->recv_left = msgleft;
            }
            return ret;
        }
        trace_vfio_user_recv_read(msg->hdr->id, ret);

        msgleft -= ret;
        data += ret;
    }

    /*
     * Read complete message, process it.
     */
    proxy->part_recv = NULL;
    proxy->recv_left = 0;
    vfio_user_process(proxy, msg);

    /* return positive value */
    return 1;
}

static int vfio_user_recv_hdr(VFIOUserProxy *proxy, Error **errp,
                              VFIOUserHdr *hdr, int **fdp, size_t *numfdp,
                              bool *isreply)
{
    struct iovec iov = {
        .iov_base = hdr,
        .iov_len = sizeof(*hdr),
    };
    int ret;

    /*
     * Read header
     */
    ret = qio_channel_readv_full(proxy->ioc, &iov, 1, fdp, numfdp, 0,
                                 errp);
    if (ret == QIO_CHANNEL_ERR_BLOCK) {
        return ret;
    }

    if (ret < 0) {
        error_setg_errno(errp, errno, "failed to read header");
        return -1;
    } else if (ret == 0) {
        error_setg(errp, "failed to read header: EOF");
        return -1;
    } else if (ret < sizeof(*hdr)) {
        error_setg(errp, "short read of header");
        return -1;
    }

    /*
     * Validate header
     */
    if (hdr->size < sizeof(*hdr)) {
        error_setg(errp, "bad header size");
        return -1;
    }

    switch (hdr->flags & VFIO_USER_TYPE) {
    case VFIO_USER_REQUEST:
        *isreply = false;
        break;
    case VFIO_USER_REPLY:
        *isreply = true;
        break;
    default:
        error_setg(errp, "unknown message type");
        return -1;
    }

    trace_vfio_user_recv_hdr(proxy->sockname, hdr->id, hdr->command, hdr->size,
                             hdr->flags);
    return 0;
}

/*
 * Receive and process one incoming message.
 *
 * For replies, find matching outgoing request and wake any waiters.
 * For requests, run the request handler.
 */
static int vfio_user_recv_one(VFIOUserProxy *proxy, Error **errp)
{
    g_autofree int *fdp = NULL;
    VFIOUserMsg *msg = NULL;
    bool isreply = false;
    size_t msgleft = 0;
    size_t numfds = 0;
    char *data = NULL;
    VFIOUserHdr hdr;
    int i, ret;

    /*
     * Complete any partial reads
     */
    if (proxy->part_recv != NULL) {
        ret = vfio_user_complete(proxy, errp);

        /* still not complete, try later */
        if (ret == QIO_CHANNEL_ERR_BLOCK) {
            return ret;
        }

        if (ret <= 0) {
            goto fatal;
        }
        return 0;
    }

    ret = vfio_user_recv_hdr(proxy, errp, &hdr, &fdp, &numfds, &isreply);
    if (ret < 0) {
        if (ret == QIO_CHANNEL_ERR_BLOCK) {
            return ret;
        }
        goto fatal;
    }

    /*
     * Find the matching request if this is a reply, or initialize a new
     * server->client request.
     */
    if (isreply) {
        QTAILQ_FOREACH(msg, &proxy->pending, next) {
            if (hdr.id == msg->id) {
                break;
            }
        }
        if (msg == NULL) {
            /*
             * Most likely a late reply to a request that timed out.  Read
             * and drop it to stay in sync with the stream.
             */
            if (hdr.size > proxy->max_xfer_size + sizeof(VFIOUserRegionRW)) {
                error_setg(errp, "unexpected reply too large");
                ret = -1;
                goto fatal;
            }
            trace_vfio_user_recv_discard(hdr.id, hdr.command);
            msg = vfio_user_getmsg(proxy, g_malloc0(hdr.size), NULL);
            msg->type = VFIO_MSG_DISCARD;
        } else {
            QTAILQ_REMOVE(&proxy->pending, msg, next);

            if (hdr.size > msg->rsize) {
                error_setg(errp, "reply larger than recv buffer");
                goto err;
            }
        }
    } else {
        void *buf;

        if (hdr.size > proxy->max_xfer_size + sizeof(VFIOUserDMARW)) {
            error_setg(errp, "vfio_user_recv request larger than max");
            goto err;
        }

        buf = g_malloc0(hdr.size);
        msg = vfio_user_getmsg(proxy, buf, NULL);
        msg->type = VFIO_MSG_REQ;
    }

    *msg->hdr = hdr;
    data = (char *)msg->hdr + sizeof(hdr);

    if (numfds != 0) {
        if (msg->type == VFIO_MSG_REQ || msg->type == VFIO_MSG_DISCARD) {
            msg->fds = vfio_user_getfds(numfds);
            msg->fds->recv_fds = numfds;
        } else {
            if (msg->fds == NULL || msg->fds->recv_fds < numfds) {
                error_setg(errp, "unexpected FDs in reply");
                goto err;
            }
            msg->fds->recv_fds = numfds;
        }

        memcpy(msg->fds->fds, fdp, numfds * sizeof(int));
    }

    /*
     * Read rest of message into the data buffer.
     */

    msgleft = hdr.size - sizeof(hdr);
    while (msgleft > 0) {
        ret = qio_channel_read(proxy->ioc, data, msgleft, errp);

        /*
         * We'll complete this read on the next go around; keep track of the
         * partial message until then.
         */
        if (ret == QIO_CHANNEL_ERR_BLOCK) {
            proxy->part_recv = msg;
            proxy->recv_left = msgleft;
            return ret;
        }

        if (ret <= 0) {
            goto fatal;
        }
        trace_vfio_user_recv_read(hdr.id, ret);

        msgleft -= ret;
        data += ret;
    }

    vfio_user_process(proxy, msg);
    return 0;

    /*
     * fatal means the other side closed or we don't trust the stream
     * err means this message is corrupt
     */
fatal:
    vfio_user_shutdown(proxy);
    proxy->state = VFIO_PROXY_ERROR;
    vfio_user_drop_part_recv(proxy, EIO);

    /* set error if server side closed */
    if (ret == 0) {
        error_setg(errp, "server closed socket");
    }

err:
    for (i = 0; i < numfds; i++) {
        close(fdp[i]);
    }
    if (msg != NULL) {
        if (msg->type == VFIO_MSG_REQ || msg->type == VFIO_MSG_DISCARD) {
            /*
             * Clean up the request message on failure. Change type back to
             * NOWAIT to free.
             */
            msg->type = VFIO_MSG_NOWAIT;
            vfio_user_recycle(proxy, msg);
        } else {
            /*
             * Report an error back to the sender. Sender will recycle msg.
             */
            vfio_user_set_error(msg->hdr, EINVAL);
            msg->complete = true;
            qemu_cond_signal(&msg->cv);
        }
    }
    return -1;
}

static void vfio_user_recv(void *opaque)
{
    VFIOUserProxy *proxy = opaque;
    Error *local_err = NULL;

    QEMU_LOCK_GUARD(&proxy->lock);

    while (proxy->state == VFIO_PROXY_CONNECTED &&
           proxy->req_deferred == NULL &&
           vfio_user_recv_one(proxy, &local_err) == 0) {
        ;
    }

    if (local_err != NULL) {
        error_report_err(local_err);
    }
}

/*
 * Send a single message, same return semantics as vfio_user_send_qio().
 *
 * Sent async messages are freed, others are moved to pending queue.
 */
static ssize_t vfio_user_send_one(VFIOUserProxy *proxy, Error **errp)
{
    VFIOUserMsg *msg;
    ssize_t ret;

    msg = QTAILQ_FIRST(&proxy->outgoing);
    ret = vfio_user_send_qio(proxy, msg, errp);
    if (ret < 0) {
        return ret;
    }

    QTAILQ_REMOVE(&proxy->outgoing, msg, next);
    proxy->num_outgoing--;
    if (msg->type == VFIO_MSG_ASYNC) {
        vfio_user_recycle(proxy, msg);
    } else {
        QTAILQ_INSERT_TAIL(&proxy->pending, msg, next);
        msg->pending = true;
    }

    return ret;
}

/*
 * Send messages from outgoing queue when the socket buffer has space.
 * If we deplete 'outgoing', remove ourselves from the poll list.
 */
static void vfio_user_send(void *opaque)
{
    VFIOUserProxy *proxy = opaque;

    QEMU_LOCK_GUARD(&proxy->lock);

    if (proxy->state == VFIO_PROXY_CONNECTED) {
        while (!QTAILQ_EMPTY(&proxy->outgoing)) {
            Error *local_err = NULL;
            int ret;

            ret = vfio_user_send_one(proxy, &local_err);

            if (ret == QIO_CHANNEL_ERR_BLOCK) {
                return;
            } else if (ret == -1) {
                error_report_err(local_err);
                return;
            }
        }
        vfio_user_set_fd_handlers(proxy);

        /* queue empty - send any pending multi write msgs */
        if (proxy->wr_multi != NULL) {
            vfio_user_flush_multi(proxy);
        }
    }
}

static void vfio_user_close_cb(void *opaque)
{
    VFIOUserProxy *proxy = opaque;

    QEMU_LOCK_GUARD(&proxy->lock);

    proxy->state = VFIO_PROXY_CLOSED;
    qemu_cond_signal(&proxy->close_cv);
}


/*
 * Functions called by main or CPU threads
 */

/*
 * Handle a request that needs the BQL, see vfio_user_dispatch().
 */
static void vfio_user_request_bh(void *opaque)
{
    VFIOUserProxy *proxy = opaque;

    QEMU_LOCK_GUARD(&proxy->lock);

    /* a BQL holder waiting for a reply may have handled it already */
    if (proxy->req_deferred != NULL && !proxy->req_running) {
        vfio_user_finish_deferred(proxy, true);
    }
}

/*
 * Messages are queued onto the proxy's outgoing list.
 *
 * It handles 3 types of messages:
 *
 * async messages - replies and posted writes
 *
 * There will be no reply from the server, so message
 * buffers are freed after they're sent.
 *
 * nowait messages - map/unmap during address space transactions
 *
 * These are also sent async, but a reply is expected so that
 * vfio_wait_reqs() can wait for the youngest nowait request.
 * They transition from the outgoing list to the pending list
 * when sent, and are freed when the reply is received.
 *
 * wait messages - all other requests
 *
 * The reply to these messages is waited for by their caller.
 * They also transition from outgoing to pending when sent, but
 * the message buffer is returned to the caller with the reply
 * contents.  The caller is responsible for freeing these messages.
 *
 * As an optimization, if the outgoing list and the socket send
 * buffer are empty, the message is sent inline instead of being
 * added to the outgoing list.  The rest of the transitions are
 * unchanged.
 */
static bool vfio_user_send_queued(VFIOUserProxy *proxy, VFIOUserMsg *msg,
                                  Error **errp)
{
    int ret;

    if (proxy->state != VFIO_PROXY_CONNECTED) {
        error_setg_errno(errp, ECONNRESET, "proxy is not connected");
        return false;
    }

    /* older coalesced writes go first */
    if (proxy->wr_multi != NULL &&
        ((msg->hdr->flags & VFIO_USER_TYPE) == VFIO_USER_REQUEST)) {
        vfio_user_flush_multi(proxy);
    }

    /*
     * Unsent outgoing msgs - add to tail
     */
    if (!QTAILQ_EMPTY(&proxy->outgoing)) {
        QTAILQ_INSERT_TAIL(&proxy->outgoing, msg, next);
        proxy->num_outgoing++;
        return true;
    }

    /*
     * Try inline - if blocked, queue it and kick send poller
     */
    if (proxy->flags & VFIO_PROXY_FORCE_QUEUED) {
        ret = QIO_CHANNEL_ERR_BLOCK;
    } else {
        ret = vfio_user_send_qio(proxy, msg, errp);
    }

    if (ret == QIO_CHANNEL_ERR_BLOCK) {
        QTAILQ_INSERT_HEAD(&proxy->outgoing, msg, next);
        proxy->num_outgoing = 1;
        vfio_user_set_fd_handlers(proxy);
        return true;
    }
    if (ret == -1) {
        return false;
    }

    /*
     * Sent - free async, add others to pending
     */
    if (msg->type == VFIO_MSG_ASYNC) {
        vfio_user_recycle(proxy, msg);
    } else {
        QTAILQ_INSERT_TAIL(&proxy->pending, msg, next);
        msg->pending = true;
    }

    return true;
}

/*
 * nowait send - vfio_wait_reqs() can wait for it later
 *
 * Returns false if we did not successfully receive a reply message, in which
 * case @errp will be populated.
 *
 * In either case, ownership of @hdr and @fds is taken, and the caller must
 * *not* free them itself.
 */
bool vfio_user_send_nowait(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                           VFIOUserFDs *fds, int rsize, Error **errp)
{
    VFIOUserMsg *msg;

    QEMU_LOCK_GUARD(&proxy->lock);

    msg = vfio_user_getmsg(proxy, hdr, fds);
    msg->id = hdr->id;
    msg->rsize = rsize ? rsize : hdr->size;
    msg->type = VFIO_MSG_NOWAIT;

    if (hdr->flags & VFIO_USER_NO_REPLY) {
        error_setg_errno(errp, EINVAL, "%s on NO_REPLY message", __func__);
        vfio_user_recycle(proxy, msg);
        return false;
    }

    if (!vfio_user_send_queued(proxy, msg, errp)) {
        vfio_user_recycle(proxy, msg);
        return false;
    }

    proxy->last_nowait = msg;

    return true;
}

/*
 * Wait for the reply to @msg, with the proxy lock held.  Returns false on
 * timeout.
 *
 * The server may send a request that needs the BQL before the reply, and
 * no message is read until that request completes, see vfio_user_dispatch().
 * The main loop can't handle it while we hold the BQL, so we complete it
 * here: handle it if @handle_deferred, or fail it where handling device DMA
 * isn't safe, e.g. during a memory transaction commit.
 */
static bool vfio_user_wait_reply(VFIOUserProxy *proxy, VFIOUserMsg *msg,
                                 bool handle_deferred)
{
    bool bql = bql_locked();
    bool ok = true;

    if (bql) {
        proxy->bql_waiter = msg;
    }
    while (!msg->complete) {
        if (bql && proxy->req_deferred != NULL) {
            vfio_user_finish_deferred(proxy, handle_deferred);
            continue;
        }
        if (!qemu_cond_timedwait(&msg->cv, &proxy->lock, proxy->wait_time)) {
            vfio_user_abandon(proxy, msg);
            ok = false;
            break;
        }
    }
    if (bql) {
        proxy->bql_waiter = NULL;
    }
    return ok;
}

static bool vfio_user_send_wait_common(VFIOUserProxy *proxy,
                                       VFIOUserHdr *hdr, VFIOUserFDs *fds,
                                       int rsize, bool handle_deferred,
                                       Error **errp)
{
    VFIOUserMsg *msg;
    bool ok = false;

    if (hdr->flags & VFIO_USER_NO_REPLY) {
        error_setg_errno(errp, EINVAL, "%s on NO_REPLY message", __func__);
        return false;
    }

    QEMU_LOCK_GUARD(&proxy->lock);

    /*
     * The server won't reply before it has our reply to the request being
     * handled, and we can't read anything else meanwhile.
     */
    if (proxy->req_running) {
        error_setg_errno(errp, EDEADLK,
                         "can't wait for a reply while handling a request");
        return false;
    }

    msg = vfio_user_getmsg(proxy, hdr, fds);
    msg->id = hdr->id;
    msg->rsize = rsize ? rsize : hdr->size;
    msg->type = VFIO_MSG_WAIT;

    ok = vfio_user_send_queued(proxy, msg, errp);

    if (ok && !vfio_user_wait_reply(proxy, msg, handle_deferred)) {
        error_setg_errno(errp, ETIMEDOUT, "timed out waiting for reply");
        ok = false;
    }

    vfio_user_recycle(proxy, msg);

    return ok;
}

/*
 * Returns false if we did not successfully receive a reply message, in which
 * case @errp will be populated.
 *
 * In either case, the caller must free @hdr and @fds if needed.
 *
 * Requests from the server that need the BQL are handled while waiting if the
 * caller holds the BQL, see vfio_user_wait_reply().
 */
bool vfio_user_send_wait(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                         VFIOUserFDs *fds, int rsize, Error **errp)
{
    return vfio_user_send_wait_common(proxy, hdr, fds, rsize, true, errp);
}

/*
 * As vfio_user_send_wait(), for DMA map and unmap requests, which may be sent
 * from memory listeners and IOMMU notifiers, where handling device DMA isn't
 * safe.  Requests from the server that need the BQL are failed instead.
 */
bool vfio_user_send_wait_dma(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                             VFIOUserFDs *fds, int rsize, Error **errp)
{
    return vfio_user_send_wait_common(proxy, hdr, fds, rsize, false, errp);
}

/*
 * async send - msg can be queued, but will be freed when sent
 *
 * Returns false on failure, in which case @errp will be populated.
 *
 * In either case, ownership of @hdr and @fds is taken, and the caller must
 * *not* free them itself.
 */
bool vfio_user_send_async(VFIOUserProxy *proxy, VFIOUserHdr *hdr,
                          VFIOUserFDs *fds, Error **errp)
{
    VFIOUserMsg *msg;

    QEMU_LOCK_GUARD(&proxy->lock);

    msg = vfio_user_getmsg(proxy, hdr, fds);
    msg->id = hdr->id;
    msg->rsize = 0;
    msg->type = VFIO_MSG_ASYNC;

    if (!(hdr->flags & (VFIO_USER_NO_REPLY | VFIO_USER_REPLY))) {
        error_setg_errno(errp, EINVAL, "%s on sync message", __func__);
        vfio_user_recycle(proxy, msg);
        return false;
    }

    if (!vfio_user_send_queued(proxy, msg, errp)) {
        vfio_user_recycle(proxy, msg);
        return false;
    }

    return true;
}

void vfio_user_wait_reqs(VFIOUserProxy *proxy)
{
    VFIOUserMsg *msg;

    /*
     * Any DMA map/unmap requests sent in the middle
     * of a memory region transaction were sent nowait.
     * Wait for them here.
     */
    qemu_mutex_lock(&proxy->lock);
    if (proxy->last_nowait != NULL) {
        msg = proxy->last_nowait;
        proxy->last_nowait = NULL;

        /* see vfio_user_send_wait_common(); the reply frees msg */
        if (proxy->req_running) {
            error_printf("vfio_user_wait_reqs - can't wait while handling "
                         "a request: command %x\n", msg->hdr->command);
            goto out;
        }

        /*
         * Change type to WAIT to wait for reply.  This is a memory
         * transaction commit, see vfio_user_wait_reply().
         */
        msg->type = VFIO_MSG_WAIT;
        if (!vfio_user_wait_reply(proxy, msg, false)) {
            error_printf("vfio_wait_reqs - timed out\n");
        }

        if (msg->hdr->flags & VFIO_USER_ERROR) {
            error_printf("vfio_user_wait_reqs - error reply on async ");
            error_printf("request: command %x error %s\n", msg->hdr->command,
                         strerror(msg->hdr->error_reply));
        }

        /*
         * Change type back to NOWAIT to free
         */
        msg->type = VFIO_MSG_NOWAIT;
        vfio_user_recycle(proxy, msg);
    }

out:
    qemu_mutex_unlock(&proxy->lock);
}

/*
 * Reply to an incoming request.
 */
void vfio_user_send_reply(VFIOUserProxy *proxy, VFIOUserHdr *hdr, int size)
{
    Error *local_err = NULL;

    if (size < sizeof(VFIOUserHdr)) {
        error_printf("%s: size too small", __func__);
        g_free(hdr);
        return;
    }

    /*
     * convert header to associated reply
     */
    hdr->flags = VFIO_USER_REPLY;
    hdr->size = size;

    if (!vfio_user_send_async(proxy, hdr, NULL, &local_err)) {
        error_report_err(local_err);
    }
}

/*
 * Send an error reply to an incoming request.
 */
void vfio_user_send_error(VFIOUserProxy *proxy, VFIOUserHdr *hdr, int error)
{
    Error *local_err = NULL;

    /*
     * convert header to associated reply
     */
    hdr->flags = VFIO_USER_REPLY;
    hdr->flags |= VFIO_USER_ERROR;
    hdr->error_reply = error;
    hdr->size = sizeof(*hdr);

    if (!vfio_user_send_async(proxy, hdr, NULL, &local_err)) {
        error_report_err(local_err);
    }
}

/*
 * Close FDs erroneously received in an incoming request.
 */
void vfio_user_putfds(VFIOUserMsg *msg)
{
    VFIOUserFDs *fds = msg->fds;
    int i;

    for (i = 0; i < fds->recv_fds; i++) {
        close(fds->fds[i]);
    }
    g_free(fds);
    msg->fds = NULL;
}

void
vfio_user_disable_posted_writes(VFIOUserProxy *proxy)
{
    WITH_QEMU_LOCK_GUARD(&proxy->lock) {
         proxy->flags |= VFIO_PROXY_NO_POST;
    }
}

static QLIST_HEAD(, VFIOUserProxy) vfio_user_sockets =
    QLIST_HEAD_INITIALIZER(vfio_user_sockets);

VFIOUserProxy *vfio_user_connect_dev(SocketAddress *addr, Error **errp)
{
    VFIOUserProxy *proxy;
    QIOChannelSocket *sioc;
    QIOChannel *ioc;
    char *sockname;

    if (addr->type != SOCKET_ADDRESS_TYPE_UNIX) {
        error_setg(errp, "vfio_user_connect - bad address family");
        return NULL;
    }
    sockname = addr->u.q_unix.path;

    sioc = qio_channel_socket_new();
    ioc = QIO_CHANNEL(sioc);
    if (qio_channel_socket_connect_sync(sioc, addr, errp) < 0) {
        goto fail;
    }
    if (!qio_channel_set_blocking(ioc, false, errp)) {
        goto fail;
    }

    proxy = g_malloc0(sizeof(VFIOUserProxy));
    proxy->sockname = g_strdup_printf("unix:%s", sockname);
    proxy->ioc = ioc;

    /* init defaults */
    proxy->max_xfer_size = VFIO_USER_DEF_MAX_XFER;
    proxy->max_send_fds = VFIO_USER_DEF_MAX_FDS;
    proxy->max_dma = VFIO_USER_DEF_MAP_MAX;
    proxy->dma_pgsizes = VFIO_USER_DEF_PGSIZE;
    proxy->max_bitmap = VFIO_USER_DEF_MAX_BITMAP;
    proxy->migr_pgsize = VFIO_USER_DEF_PGSIZE;

    proxy->flags = VFIO_PROXY_CLIENT;
    proxy->state = VFIO_PROXY_CONNECTED;

    qemu_mutex_init(&proxy->lock);
    qemu_cond_init(&proxy->close_cv);
    qemu_cond_init(&proxy->req_cv);

    if (vfio_user_iothread == NULL) {
        vfio_user_iothread = iothread_create("vfio-user", errp);
    }

    proxy->ctx = iothread_get_aio_context(vfio_user_iothread);
    proxy->req_bh = qemu_bh_new(vfio_user_request_bh, proxy);

    QTAILQ_INIT(&proxy->outgoing);
    QTAILQ_INIT(&proxy->free);
    QTAILQ_INIT(&proxy->pending);
    QLIST_INSERT_HEAD(&vfio_user_sockets, proxy, next);

    return proxy;

fail:
    object_unref(OBJECT(ioc));
    return NULL;
}

void vfio_user_set_handler(VFIODevice *vbasedev,
                           bool (*handler)(void *opaque, VFIOUserMsg *msg),
                           void *req_arg)
{
    VFIOUserProxy *proxy = vbasedev->proxy;

    QEMU_LOCK_GUARD(&proxy->lock);

    proxy->request = handler;
    proxy->req_arg = req_arg;
    vfio_user_set_fd_handlers(proxy);
}

/*
 * Stop handling requests from the server, and wait for the one currently
 * being handled, if any.  Requests received afterwards are rejected.
 *
 * Must not be called from the proxy's iothread.
 */
void vfio_user_clear_handler(VFIOUserProxy *proxy)
{
    QEMU_LOCK_GUARD(&proxy->lock);

    proxy->request = NULL;
    proxy->req_arg = NULL;
    while (proxy->req_active) {
        qemu_cond_wait(&proxy->req_cv, &proxy->lock);
    }
}

void vfio_user_disconnect(VFIOUserProxy *proxy)
{
    VFIOUserMsg *r1, *r2;

    qemu_mutex_lock(&proxy->lock);

    /* our side is quitting */
    if (proxy->state == VFIO_PROXY_CONNECTED) {
        vfio_user_shutdown(proxy);
        if (!QTAILQ_EMPTY(&proxy->pending)) {
            error_printf("vfio_user_disconnect: outstanding requests\n");
        }
    }
    object_unref(OBJECT(proxy->ioc));
    proxy->ioc = NULL;

    proxy->state = VFIO_PROXY_CLOSING;
    vfio_user_drop_part_recv(proxy, ECONNRESET);
    qemu_bh_delete(proxy->req_bh);
    proxy->req_bh = NULL;
    if (proxy->req_deferred != NULL) {
        vfio_user_drop_request(proxy->req_deferred);
        qemu_cond_destroy(&proxy->req_deferred->cv);
        g_free(proxy->req_deferred);
        proxy->req_deferred = NULL;
    }
    QTAILQ_FOREACH_SAFE(r1, &proxy->outgoing, next, r2) {
        qemu_cond_destroy(&r1->cv);
        QTAILQ_REMOVE(&proxy->outgoing, r1, next);
        g_free(r1);
    }
    QTAILQ_FOREACH_SAFE(r1, &proxy->pending, next, r2) {
        qemu_cond_destroy(&r1->cv);
        QTAILQ_REMOVE(&proxy->pending, r1, next);
        g_free(r1);
    }
    QTAILQ_FOREACH_SAFE(r1, &proxy->free, next, r2) {
        qemu_cond_destroy(&r1->cv);
        QTAILQ_REMOVE(&proxy->free, r1, next);
        g_free(r1);
    }

    /*
     * Make sure the iothread isn't blocking anywhere
     * with a ref to this proxy by waiting for a BH
     * handler to run after the proxy fd handlers were
     * deleted above.
     */
    aio_bh_schedule_oneshot(proxy->ctx, vfio_user_close_cb, proxy);

    while (proxy->state != VFIO_PROXY_CLOSED) {
        qemu_cond_wait(&proxy->close_cv, &proxy->lock);
    }

    /* we now hold the only ref to proxy */
    qemu_mutex_unlock(&proxy->lock);
    qemu_cond_destroy(&proxy->close_cv);
    qemu_cond_destroy(&proxy->req_cv);
    qemu_mutex_destroy(&proxy->lock);

    QLIST_REMOVE(proxy, next);
    if (QLIST_EMPTY(&vfio_user_sockets)) {
        iothread_destroy(vfio_user_iothread);
        vfio_user_iothread = NULL;
    }

    g_free(proxy->sockname);
    g_free(proxy);
}

void vfio_user_request_msg(VFIOUserHdr *hdr, uint16_t cmd,
                           uint32_t size, uint32_t flags)
{
    static uint16_t next_id;

    hdr->id = qatomic_fetch_inc(&next_id);
    hdr->command = cmd;
    hdr->size = size;
    hdr->flags = (flags & ~VFIO_USER_TYPE) | VFIO_USER_REQUEST;
    hdr->error_reply = 0;
}

struct cap_entry {
    const char *name;
    bool (*check)(VFIOUserProxy *proxy, QObject *qobj, Error **errp);
};

static bool caps_parse(VFIOUserProxy *proxy, QDict *qdict,
                       struct cap_entry caps[], Error **errp)
{
    QObject *qobj;
    struct cap_entry *p;

    for (p = caps; p->name != NULL; p++) {
        qobj = qdict_get(qdict, p->name);
        if (qobj != NULL) {
            if (!p->check(proxy, qobj, errp)) {
                return false;
            }
            qdict_del(qdict, p->name);
        }
    }

    /* warning, for now */
    if (qdict_size(qdict) != 0) {
        warn_report("spurious capabilities");
    }
    return true;
}

static bool check_migr_pgsize(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t pgsize;

    if (qn == NULL || !qnum_get_try_uint(qn, &pgsize)) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_PGSIZE);
        return false;
    }

    /* must not be zero or smaller than default */
    if (pgsize < VFIO_USER_DEF_PGSIZE ||
        (pgsize & (VFIO_USER_DEF_PGSIZE - 1))) {
        error_setg(errp, "%s 0x%"PRIx64" too small",
                   VFIO_USER_CAP_PGSIZE, pgsize);
        return false;
    }

    proxy->migr_pgsize = pgsize;
    return true;
}

static bool check_bitmap(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t bitmap_size;

    if (qn == NULL || !qnum_get_try_uint(qn, &bitmap_size)) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MAX_BITMAP);
        return false;
    }

    /* can only lower it */
    if (bitmap_size > VFIO_USER_DEF_MAX_BITMAP) {
        error_setg(errp, "%s too large", VFIO_USER_CAP_MAX_BITMAP);
        return false;
    }

    proxy->max_bitmap = bitmap_size;
    return true;
}

static struct cap_entry caps_migr[] = {
    { VFIO_USER_CAP_PGSIZE, check_migr_pgsize },
    { VFIO_USER_CAP_MAX_BITMAP, check_bitmap },
    { NULL }
};

static bool check_max_fds(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t max_send_fds;

    if (qn == NULL || !qnum_get_try_uint(qn, &max_send_fds) ||
        max_send_fds > VFIO_USER_MAX_MAX_FDS) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MAX_FDS);
        return false;
    }
    proxy->max_send_fds = max_send_fds;
    return true;
}

static bool check_max_xfer(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t max_xfer_size;

    if (qn == NULL || !qnum_get_try_uint(qn, &max_xfer_size) ||
        max_xfer_size > VFIO_USER_MAX_MAX_XFER) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MAX_XFER);
        return false;
    }
    proxy->max_xfer_size = max_xfer_size;
    return true;
}

static bool check_pgsizes(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t pgsizes;

    if (qn == NULL || !qnum_get_try_uint(qn, &pgsizes)) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_PGSIZES);
        return false;
    }

    /* must not be zero or smaller than default */
    if (pgsizes < VFIO_USER_DEF_PGSIZE ||
        (pgsizes & (VFIO_USER_DEF_PGSIZE - 1))) {
        error_setg(errp, "%s 0x%"PRIx64" too small",
                   VFIO_USER_CAP_PGSIZES, pgsizes);
        return false;
    }

    proxy->dma_pgsizes = pgsizes;
    return true;
}

static bool check_max_dma(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QNum *qn = qobject_to(QNum, qobj);
    uint64_t max_dma;

    if (qn == NULL || !qnum_get_try_uint(qn, &max_dma)) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MAP_MAX);
        return false;
    }

    /* can only lower it */
    if (max_dma > VFIO_USER_DEF_MAP_MAX) {
        error_setg(errp, "%s too large", VFIO_USER_CAP_MAP_MAX);
        return false;
    }

    proxy->max_dma = max_dma;
    return true;
}

static bool check_migr(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QDict *qdict = qobject_to(QDict, qobj);

    if (qdict == NULL) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MIGR);
        return false;
    }
    return caps_parse(proxy, qdict, caps_migr, errp);
}

static bool check_multi(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
    QBool *qb = qobject_to(QBool, qobj);

    if (qb == NULL) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP_MULTI);
        return false;
    }
    if (qbool_get_bool(qb)) {
        proxy->flags |= VFIO_PROXY_USE_MULTI;
    }
    return true;
}

static struct cap_entry caps_cap[] = {
    { VFIO_USER_CAP_MAX_FDS, check_max_fds },
    { VFIO_USER_CAP_MAX_XFER, check_max_xfer },
    { VFIO_USER_CAP_PGSIZES, check_pgsizes },
    { VFIO_USER_CAP_MAP_MAX, check_max_dma },
    { VFIO_USER_CAP_MIGR, check_migr },
    { VFIO_USER_CAP_MULTI, check_multi },
    { NULL }
};

static bool check_cap(VFIOUserProxy *proxy, QObject *qobj, Error **errp)
{
   QDict *qdict = qobject_to(QDict, qobj);

    if (qdict == NULL) {
        error_setg(errp, "malformed %s", VFIO_USER_CAP);
        return false;
    }
    return caps_parse(proxy, qdict, caps_cap, errp);
}

static struct cap_entry ver_0_0[] = {
    { VFIO_USER_CAP, check_cap },
    { NULL }
};

static bool caps_check(VFIOUserProxy *proxy, int minor, const char *caps,
                       Error **errp)
{
    QObject *qobj;
    QDict *qdict;
    bool ret;

    qobj = qobject_from_json(caps, NULL);
    if (qobj == NULL) {
        error_setg(errp, "malformed capabilities %s", caps);
        return false;
    }
    qdict = qobject_to(QDict, qobj);
    if (qdict == NULL) {
        error_setg(errp, "capabilities %s not an object", caps);
        qobject_unref(qobj);
        return false;
    }
    ret = caps_parse(proxy, qdict, ver_0_0, errp);

    qobject_unref(qobj);
    return ret;
}

static GString *caps_json(void)
{
    QDict *dict = qdict_new();
    QDict *capdict = qdict_new();
    QDict *migdict = qdict_new();
    GString *str;

    qdict_put_int(migdict, VFIO_USER_CAP_PGSIZE, VFIO_USER_DEF_PGSIZE);
    qdict_put_int(migdict, VFIO_USER_CAP_MAX_BITMAP, VFIO_USER_DEF_MAX_BITMAP);
    qdict_put_obj(capdict, VFIO_USER_CAP_MIGR, QOBJECT(migdict));

    qdict_put_int(capdict, VFIO_USER_CAP_MAX_FDS, VFIO_USER_MAX_MAX_FDS);
    qdict_put_int(capdict, VFIO_USER_CAP_MAX_XFER, VFIO_USER_DEF_MAX_XFER);
    qdict_put_int(capdict, VFIO_USER_CAP_PGSIZES, VFIO_USER_DEF_PGSIZE);
    qdict_put_int(capdict, VFIO_USER_CAP_MAP_MAX, VFIO_USER_DEF_MAP_MAX);
    qdict_put_bool(capdict, VFIO_USER_CAP_MULTI, true);

    qdict_put_obj(dict, VFIO_USER_CAP, QOBJECT(capdict));

    str = qobject_to_json(QOBJECT(dict));
    qobject_unref(dict);
    return str;
}

bool vfio_user_validate_version(VFIOUserProxy *proxy, Error **errp)
{
    g_autofree VFIOUserVersion *msgp = NULL;
    GString *caps;
    const char *reply = "";
    int size, caplen;

    caps = caps_json();
    caplen = caps->len + 1;
    size = sizeof(*msgp) + caplen;
    msgp = g_malloc0(size);

    vfio_user_request_msg(&msgp->hdr, VFIO_USER_VERSION, size, 0);
    msgp->major = VFIO_USER_MAJOR_VER;
    msgp->minor = VFIO_USER_MINOR_VER;
    memcpy(&msgp->capabilities, caps->str, caplen);
    g_string_free(caps, true);
    trace_vfio_user_version(msgp->major, msgp->minor, msgp->capabilities);

    if (!vfio_user_send_wait(proxy, &msgp->hdr, NULL, 0, errp)) {
        return false;
    }

    if (msgp->hdr.flags & VFIO_USER_ERROR) {
        error_setg_errno(errp, msgp->hdr.error_reply, "version reply");
        return false;
    }

    if (msgp->major != VFIO_USER_MAJOR_VER ||
        msgp->minor > VFIO_USER_MINOR_VER) {
        error_setg(errp, "incompatible server version");
        return false;
    }

    if (msgp->hdr.size < sizeof(*msgp)) {
        error_setg(errp, "short version reply");
        return false;
    }

    if (msgp->hdr.size > sizeof(*msgp)) {
        reply = msgp->capabilities;
        if (reply[msgp->hdr.size - sizeof(*msgp) - 1] != '\0') {
            error_setg(errp, "corrupt version reply");
            return false;
        }

        if (!caps_check(proxy, msgp->minor, reply, errp)) {
            return false;
        }
    }

    trace_vfio_user_version(msgp->major, msgp->minor, reply);
    return true;
}

void vfio_user_flush_multi(VFIOUserProxy *proxy)
{
    VFIOUserMsg *msg;
    VFIOUserWRMulti *wm = proxy->wr_multi;
    Error *local_err = NULL;

    proxy->wr_multi = NULL;

    /* adjust size for actual # of writes */
    wm->hdr.size -= (VFIO_USER_MULTI_MAX - wm->wr_cnt) * sizeof(VFIOUserWROne);

    msg = vfio_user_getmsg(proxy, &wm->hdr, NULL);
    msg->id = wm->hdr.id;
    msg->rsize = 0;
    msg->type = VFIO_MSG_ASYNC;
    trace_vfio_user_wrmulti("flush", wm->wr_cnt);

    if (!vfio_user_send_queued(proxy, msg, &local_err)) {
        error_report_err(local_err);
        vfio_user_recycle(proxy, msg);
    }
}

void vfio_user_create_multi(VFIOUserProxy *proxy)
{
    VFIOUserWRMulti *wm;

    wm = g_malloc0(sizeof(*wm));
    vfio_user_request_msg(&wm->hdr, VFIO_USER_REGION_WRITE_MULTI,
                          sizeof(*wm), VFIO_USER_NO_REPLY);
    proxy->wr_multi = wm;
}

void vfio_user_add_multi(VFIOUserProxy *proxy, uint8_t index,
                         off_t offset, uint32_t count, void *data)
{
    VFIOUserWRMulti *wm = proxy->wr_multi;
    VFIOUserWROne *w1 = &wm->wrs[wm->wr_cnt];

    w1->offset = offset;
    w1->region = index;
    w1->count = count;
    memcpy(&w1->data, data, count);

    wm->wr_cnt++;
    trace_vfio_user_wrmulti("add", wm->wr_cnt);
    if (wm->wr_cnt == VFIO_USER_MULTI_MAX ||
        proxy->num_outgoing < VFIO_USER_OUT_LOW) {
        vfio_user_flush_multi(proxy);
    }
}
