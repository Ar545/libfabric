#include "fi_dpdk.h"

static ssize_t dpdk_eq_read(struct fid_eq *eq_fid, uint32_t *event, void *buf, size_t len,
                            uint64_t flags) {
    struct util_eq *eq;

    eq = container_of(eq_fid, struct util_eq, eq_fid);

    // TODO: This function should be implemented!!
    printf("[dpdk_eq_read] UNIMPLEMENTED\n");
    // dpdk_conn_mgr_run(eq);

    return ofi_eq_read(eq_fid, event, buf, len, flags);
}

static int dpdk_eq_close(struct fid *fid) {
    struct dpdk_eq *eq;
    int             ret;

    ret = ofi_eq_cleanup(fid);
    if (ret)
        return ret;

    eq = container_of(fid, struct dpdk_eq, util_eq.eq_fid.fid);

    ofi_mutex_destroy(&eq->close_lock);
    free(eq);
    return 0;
}

static ssize_t dummy_sread(struct fid_eq *eq_fid, uint32_t *event, void *buf, size_t len,
                           int timeout, uint64_t flags) {
    // TODO: This is the correct implementation, but to test the current implementation, we are just
    // returning an expected value. The correct implementatino is ofi_eq_sread. But at that point,
    // the EQ mechanism must be implemented!
    printf("[DUMMY SREAD] UNIMPLEMENTED!\n");

    *event = FI_CONNECTED;

    return 16;
}

static struct fi_ops_eq dpdk_eq_ops = {
    .size     = sizeof(struct fi_ops_eq),
    .read     = dpdk_eq_read,
    .readerr  = ofi_eq_readerr,
    .sread    = dummy_sread, // ofi_eq_sread, TODO: replace with ofi_eq_sread!!!
    .write    = ofi_eq_write,
    .strerror = ofi_eq_strerror,
};

static struct fi_ops dpdk_eq_fi_ops = {
    .size     = sizeof(struct fi_ops),
    .close    = dpdk_eq_close,
    .bind     = fi_no_bind,
    .control  = ofi_eq_control,
    .ops_open = fi_no_ops_open,
};

static int dpdk_eq_wait_try_func(void *arg) {
    OFI_UNUSED(arg);
    return FI_SUCCESS;
}

int dpdk_eq_add_progress(struct dpdk_eq *eq, struct dpdk_progress *progress, void *context) {
    return ofi_wait_add_fd(eq->util_eq.wait, ofi_dynpoll_get_fd(&progress->epoll_fd), POLLIN,
                           dpdk_eq_wait_try_func, NULL, context);
}

int dpdk_eq_del_progress(struct dpdk_eq *eq, struct dpdk_progress *progress) {
    return ofi_wait_del_fd(eq->util_eq.wait, ofi_dynpoll_get_fd(&progress->epoll_fd));
}

int dpdk_eq_create(struct fid_fabric *fabric_fid, struct fi_eq_attr *attr, struct fid_eq **eq_fid,
                   void *context) {
    struct dpdk_eq     *eq;
    struct fi_wait_attr wait_attr;
    struct fid_wait    *wait;
    int                 ret;

    eq = calloc(1, sizeof(*eq));
    if (!eq)
        return -FI_ENOMEM;

    ret = ofi_eq_init(fabric_fid, attr, &eq->util_eq.eq_fid, context);
    if (ret) {
        FI_WARN(&dpdk_prov, FI_LOG_EQ, "EQ creation failed\n");
        goto err1;
    }

    ret = ofi_mutex_init(&eq->close_lock);
    if (ret)
        goto err2;

    eq->util_eq.eq_fid.ops     = &dpdk_eq_ops;
    eq->util_eq.eq_fid.fid.ops = &dpdk_eq_fi_ops;

    if (!eq->util_eq.wait) {
        memset(&wait_attr, 0, sizeof wait_attr);
        wait_attr.wait_obj = FI_WAIT_POLLFD;
        ret                = fi_wait_open(fabric_fid, &wait_attr, &wait);
        if (ret) {
            FI_WARN(&dpdk_prov, FI_LOG_EQ, "opening wait failed\n");
            goto err3;
        }
        eq->util_eq.internal_wait = 1;
        eq->util_eq.wait          = container_of(wait, struct util_wait, wait_fid);
    }
    *eq_fid = &eq->util_eq.eq_fid;
    return 0;
err3:
    ofi_mutex_destroy(&eq->close_lock);
err2:
    ofi_eq_cleanup(&eq->util_eq.eq_fid.fid);
err1:
    free(eq);
    return ret;
}
