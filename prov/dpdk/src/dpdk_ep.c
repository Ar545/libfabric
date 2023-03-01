#include "fi_dpdk.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/types.h>

#include <ofi_util.h>

static int dpdk_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags) {
    struct dpdk_ep  *ep;
    struct dpdk_srx *srx;
    int              ret;

    ep = container_of(fid, struct dpdk_ep, util_ep.ep_fid.fid);

    if (bfid->fclass == FI_CLASS_SRX_CTX) {
        srx     = container_of(bfid, struct dpdk_srx, rx_fid.fid);
        ep->srx = srx;
        return FI_SUCCESS;
    }

    ret = ofi_ep_bind(&ep->util_ep, bfid, flags);
    return ret;
}

static int dpdk_ep_close(struct fid *fid) {
    struct dpdk_progress *progress;
    struct dpdk_ep       *ep;

    printf("[dpdk_ep_close] UNIMPLEMENTED\n");
    // ep = container_of(fid, struct dpdk_ep, util_ep.ep_fid.fid);

    // progress = dpdk_ep2_progress(ep);
    // ofi_genlock_lock(&progress->lock);
    // dlist_remove_init(&ep->unexp_entry);
    // dpdk_halt_sock(progress, ep->bsock.sock);
    // dpdk_ep_flush_all_queues(ep);
    // ofi_genlock_unlock(&progress->lock);

    // free(ep->cm_msg);
    // ofi_close_socket(ep->bsock.sock);

    // ofi_endpoint_close(&ep->util_ep);
    free(ep);
    return 0;
}

static int dpdk_ep_ctrl(struct fid *fid, int command, void *arg) {
    struct dpdk_ep *ep;

    ep = container_of(fid, struct dpdk_ep, util_ep.ep_fid.fid);
    switch (command) {
    case FI_ENABLE:
        if ((ofi_needs_rx(ep->util_ep.caps) && !ep->util_ep.rx_cq) ||
            (ofi_needs_tx(ep->util_ep.caps) && !ep->util_ep.tx_cq))
        {
            FI_WARN(&dpdk_prov, FI_LOG_EP_CTRL, "missing needed CQ binding\n");
            return -FI_ENOCQ;
        }
        break;
    default:
        FI_WARN(&dpdk_prov, FI_LOG_EP_CTRL, "unsupported command\n");
        return -FI_ENOSYS;
    }
    return FI_SUCCESS;
}

static struct fi_ops dpdk_ep_fi_ops = {
    .size     = sizeof(struct fi_ops),
    .close    = dpdk_ep_close,
    .bind     = dpdk_ep_bind,
    .control  = dpdk_ep_ctrl,
    .ops_open = fi_no_ops_open,
};

static int dpdk_ep_getname(fid_t fid, void *addr, size_t *addrlen) {
    struct dpdk_ep *ep;
    size_t          addrlen_in = *addrlen;
    int             ret;

    ep = container_of(fid, struct dpdk_ep, util_ep.ep_fid);

    // TODO: Complete the implementation in a DPDK-specific way
    printf("[dpdk_ep_getname] UNIMPLEMENTED\n");
    // ret = ofi_getsockname(ep->bsock.sock, addr, (socklen_t *)addrlen);
    ret = 0;
    strncpy(addr, "dummy_address", 14);

    if (ret)
        return -ofi_sockerr();

    return (addrlen_in < *addrlen) ? -FI_ETOOSMALL : FI_SUCCESS;
}

static int dpdk_ep_connect(struct fid_ep *ep_fid, const void *addr, const void *param,
                           size_t paramlen) {

    // TODO: IMPLEMENT THIS FUNCTION
    printf("[dpdk_ep_connect] UNIMPLEMENTED\n");

    return 0;
}

static struct fi_ops_cm dpdk_cm_ops = {
    .size     = sizeof(struct fi_ops_cm),
    .setname  = fi_no_setname,
    .getname  = dpdk_ep_getname,
    .getpeer  = fi_no_getpeer, // TODO: Provide an implementation!
    .connect  = dpdk_ep_connect,
    .listen   = fi_no_listen,
    .accept   = fi_no_accept, // TODO: Provide accept!
    .reject   = fi_no_reject,
    .shutdown = fi_no_shutdown, // TODO: Provide shutdown!
    .join     = fi_no_join,
};

int dpdk_endpoint(struct fid_domain *domain, struct fi_info *info, struct fid_ep **ep_fid,
                  void *context) {
    struct dpdk_ep          *ep;
    struct dpdk_pep         *pep;
    struct dpdk_conn_handle *handle;
    int                      ret = 0;

    ep = calloc(1, sizeof(*ep));
    if (!ep) {
        return -FI_ENOMEM;
    }

    ret = ofi_endpoint_init(domain, &dpdk_util_prov, info, &ep->util_ep, context, NULL);
    if (ret) {
        goto err1;
    }

    // TODO: Complete the implementation in a DPDK-specific way
    printf("[dpdk_endpoint] DPDK Endpoint only partially implemented!\n");

    // ofi_bsock_init(&ep->bsock, &dpdk_ep2_progress(ep)->sockapi,
    // 	       dpdk_staging_sbuf_size, dpdk_prefetch_rbuf_size);
    // if (info->handle) {
    // 	if (((fid_t) info->handle)->fclass == FI_CLASS_PEP) {
    // 		pep = container_of(info->handle, struct dpdk_pep,
    // 				   util_pep.pep_fid.fid);

    // 		ep->bsock.sock = pep->sock;
    // 		pep->sock = INVALID_SOCKET;
    // 	} else {
    // 		ep->state = dpdk_ACCEPTING;
    // 		conn = container_of(info->handle,
    // 				    struct dpdk_conn_handle, fid);
    // 		/* EP now owns socket */
    // 		ep->bsock.sock = conn->sock;
    // 		conn->sock = INVALID_SOCKET;
    // 		if (dpdk_trace_msg) {
    // 			ep->hdr_bswap = conn->endian_match ?
    // 					dpdk_hdr_trace : dpdk_hdr_bswap_trace;
    // 		} else {
    // 			ep->hdr_bswap = conn->endian_match ?
    // 					dpdk_hdr_none : dpdk_hdr_bswap;
    // 		}
    // 		/* Save handle, but we only free if user calls accept.
    // 		 * Otherwise, user will call reject, which will free it.
    // 		 */
    // 		ep->conn = conn;

    // 		ret = dpdk_setup_socket(ep->bsock.sock, info);
    // 		if (ret)
    // 			goto err3;
    // 	}
    // } else {
    // 	ep->bsock.sock = ofi_socket(ofi_get_sa_family(info), SOCK_STREAM, 0);
    // 	if (ep->bsock.sock == INVALID_SOCKET) {
    // 		ret = -ofi_sockerr();
    // 		goto err2;
    // 	}

    // 	ret = dpdk_setup_socket(ep->bsock.sock, info);
    // 	if (ret)
    // 		goto err3;

    // 	if (!dpdk_io_uring)
    // 		dpdk_set_zerocopy(ep->bsock.sock);

    // 	if (info->src_addr && (!ofi_is_any_addr(info->src_addr) ||
    // 				ofi_addr_get_port(info->src_addr))) {

    // 		if (!ofi_addr_get_port(info->src_addr)) {
    // 			dpdk_set_no_port(ep->bsock.sock);
    // 		}

    // 		ret = bind(ep->bsock.sock, info->src_addr,
    // 			(socklen_t) info->src_addrlen);
    // 		if (ret) {
    // 			FI_WARN(&dpdk_prov, FI_LOG_EP_CTRL, "bind failed\n");
    // 			ret = -ofi_sockerr();
    // 			goto err3;
    // 		}
    // 	}
    // }

    // ep->cm_msg = calloc(1, sizeof(*ep->cm_msg));
    // if (!ep->cm_msg) {
    //     ret = -FI_ENOMEM;
    //     goto err3;
    // }

    // dlist_init(&ep->unexp_entry);
    // slist_init(&ep->rx_queue);
    // slist_init(&ep->tx_queue);
    // slist_init(&ep->priority_queue);
    // slist_init(&ep->rma_read_queue);
    // slist_init(&ep->need_ack_queue);
    // slist_init(&ep->async_queue);

    // if (info->ep_attr->rx_ctx_cnt != FI_SHARED_CONTEXT)
    // 	ep->rx_avail = (int) info->rx_attr->size;

    // ep->cur_rx.hdr_done = 0;
    // ep->cur_rx.hdr_len = sizeof(ep->cur_rx.hdr.base_hdr);

    *ep_fid            = &ep->util_ep.ep_fid;
    (*ep_fid)->fid.ops = &dpdk_ep_fi_ops;
    // (*ep_fid)->ops     = &dpdk_ep_ops;
    (*ep_fid)->cm = &dpdk_cm_ops;
    //     (*ep_fid)->msg     = &dpdk_msg_ops;
    //     (*ep_fid)->rma     = &dpdk_rma_ops;
    //     (*ep_fid)->tagged  = &dpdk_tagged_ops;

    return 0;
// err3:
//     ofi_close_socket(ep->bsock.sock);
err2:
    ofi_endpoint_close(&ep->util_ep);
err1:
    free(ep);
    return ret;
}

static int dpdk_pep_close(struct fid *fid) {

    printf("[dpdk_pep_close] UNIMPLEMENTED\n");
    return 0;
}

static int dpdk_pep_bind(struct fid *fid) {

    printf("[dpdk_pep_bind] UNIMPLEMENTED\n");
    return 0;
}

static int dpdk_pep_setname(fid_t fid, void *addr, size_t addrlen) {

    printf("[dpdk_pep_setname] UNIMPLEMENTED\n");
    strncpy(addr, "dummy_address", 14);
    return 0;
}

static int dpdk_pep_getname(fid_t fid, void *addr, size_t addrlen) {

    printf("[dpdk_pep_getname] UNIMPLEMENTED\n");
    strncpy(addr, "dummy_address", 14);
    return 0;
}

static int dpdk_pep_listen(struct fid_pep *pep_fid) {

    printf("[dpdk_pep_listen] UNIMPLEMENTED\n");
    return 0;
}

static int dpdk_pep_reject(struct fid_pep *pep, fid_t fid_handle, const void *param,
                           size_t paramlen) {
    printf("[dpdk_pep_reject] UNIMPLEMENTED\n");
    return 0;
}
static struct fi_ops dpdk_pep_fi_ops = {
    .size     = sizeof(struct fi_ops),
    .close    = dpdk_pep_close,
    .bind     = dpdk_pep_bind,
    .control  = fi_no_control,
    .ops_open = fi_no_ops_open,
};

static struct fi_ops_cm dpdk_pep_cm_ops = {
    .size     = sizeof(struct fi_ops_cm),
    .setname  = dpdk_pep_setname,
    .getname  = dpdk_pep_getname,
    .getpeer  = fi_no_getpeer,
    .connect  = fi_no_connect,
    .listen   = dpdk_pep_listen,
    .accept   = fi_no_accept,
    .reject   = dpdk_pep_reject,
    .shutdown = fi_no_shutdown,
    .join     = fi_no_join,
};

static struct fi_ops_ep dpdk_pep_ops = {
    .size         = sizeof(struct fi_ops_ep),
    .getopt       = fi_no_getopt, // TODO: implement: dpdk_pep_getopt,
    .setopt       = fi_no_setopt,
    .tx_ctx       = fi_no_tx_ctx,
    .rx_ctx       = fi_no_rx_ctx,
    .rx_size_left = fi_no_rx_size_left,
    .tx_size_left = fi_no_tx_size_left,
};

int dpdk_passive_ep(struct fid_fabric *fabric, struct fi_info *info, struct fid_pep **pep_fid,
                    void *context) {
    struct dpdk_pep *pep;
    int              ret;

    if (!info) {
        FI_WARN(&dpdk_prov, FI_LOG_EP_CTRL, "invalid info\n");
        return -FI_EINVAL;
    }

    ret = ofi_prov_check_info(&dpdk_util_prov, fabric->api_version, info);
    if (ret) {
        return ret;
    }

    pep = calloc(1, sizeof(*pep));
    if (!pep) {
        return -FI_ENOMEM;
    }

    ret = ofi_pep_init(fabric, info, &pep->util_pep, context);
    if (ret) {
        goto err1;
    }

    // TODO: finish to implement!
    printf("[dpdk_passive_ep ] PARTIALLY UNIMPLEMENTED\n");

    pep->util_pep.pep_fid.fid.ops = &dpdk_pep_fi_ops;
    pep->util_pep.pep_fid.cm      = &dpdk_pep_cm_ops;
    pep->util_pep.pep_fid.ops     = &dpdk_pep_ops;

    pep->info = fi_dupinfo(info);
    if (!pep->info) {
        ret = -FI_ENOMEM;
        goto err2;
    }

    //     pep->cm_ctx.fid.fclass = DPDK_CLASS_CM;
    //     pep->cm_ctx.hfid       = &pep->util_pep.pep_fid.fid;
    //     pep->cm_ctx.state      = DPDK_CM_LISTENING;
    //     pep->cm_ctx.cm_data_sz = 0;
    //     pep->sock              = INVALID_SOCKET;

    //     if (info->src_addr) {
    //         ret = dpdk_pep_sock_create(pep);
    //         if (ret)
    //             goto err3;
    //     }

    // TODO: Here we first set the ops to pep->util_pep, then we pass the pointer to the caller.
    // Instead, in the dpdk_endpoint(), we first pass the pointer to the caller, then we set the ops
    // to the caller. We should be consistent and choose one of the two approaches!
    *pep_fid = &pep->util_pep.pep_fid;
    return FI_SUCCESS;
err3:
    fi_freeinfo(pep->info);
err2:
    ofi_pep_close(&pep->util_pep);
err1:
    free(pep);
    return ret;
}
