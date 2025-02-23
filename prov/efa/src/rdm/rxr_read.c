/*
 * Copyright (c) 2019-2022 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "efa.h"
#include "efa_av.h"
#include "rxr.h"
#include "rxr_rma.h"
#include "rxr_cntr.h"
#include "rxr_read.h"

int rxr_locate_iov_pos(struct iovec *iov, int iov_count, size_t offset,
		       int *iov_idx, size_t *iov_offset)
{
	int i;
	size_t curoffset;

	curoffset = 0;
	for (i = 0; i < iov_count; ++i) {
		if (offset >= curoffset &&
		    offset < curoffset + iov[i].iov_len) {
			*iov_idx = i;
			*iov_offset = offset - curoffset;
			return 0;
		}

		curoffset += iov[i].iov_len;
	}

	return -1;
}

int rxr_locate_rma_iov_pos(struct fi_rma_iov *rma_iov, int rma_iov_count, size_t offset,
			   int *rma_iov_idx, size_t *rma_iov_offset)
{
	int i;
	size_t curoffset;

	curoffset = 0;
	for (i = 0; i < rma_iov_count; ++i) {
		if (offset >= curoffset &&
		    offset < curoffset + rma_iov[i].len) {
			*rma_iov_idx = i;
			*rma_iov_offset = offset - curoffset;
			return 0;
		}

		curoffset += rma_iov[i].len;
	}

	return -1;
}

/*
 * rxr_read_prepare_pkt_entry_mr() ensure pkt_entry's memory is registered.
 *
 * For a packet entry whose memory is not registered, it will reserve a pkt entry
 * from rx_readcopy_pkt_pool and copy data their.
 *
 * Return value:
 *
 *     On success, return 0
 *     On pack entry reservation failure, return -FI_EAGAIN
 */
static
ssize_t rxr_read_prepare_pkt_entry_mr(struct rxr_ep *ep, struct rxr_read_entry *read_entry)
{
	size_t pkt_offset;
	struct rxr_pkt_entry *pkt_entry;
	struct rxr_pkt_entry *pkt_entry_copy;

	assert(read_entry->context_type == RXR_READ_CONTEXT_PKT_ENTRY);
	/*
	 * In this case, target buffer is data in a pkt_entry, so rma_iov_count must be 1.
	 */
	assert(read_entry->rma_iov_count == 1);

	pkt_entry = read_entry->context;
	if (pkt_entry->mr) {
		assert(read_entry->rma_iov[0].key == fi_mr_key(pkt_entry->mr));
		return 0;
	}

	assert(pkt_entry->alloc_type == RXR_PKT_FROM_OOO_POOL ||
	       pkt_entry->alloc_type == RXR_PKT_FROM_UNEXP_POOL ||
	       pkt_entry->alloc_type == RXR_PKT_FROM_SHM_RX_POOL);

	pkt_offset = (char *)read_entry->rma_iov[0].addr - pkt_entry->wiredata;
	assert(pkt_offset > sizeof(struct rxr_base_hdr));

	pkt_entry_copy = rxr_pkt_entry_clone(ep, ep->rx_readcopy_pkt_pool,
					     RXR_PKT_FROM_READ_COPY_POOL,
					     pkt_entry);
	if (!pkt_entry_copy) {
		EFA_WARN(FI_LOG_CQ,
			"readcopy pkt pool exhausted! Set FI_EFA_READCOPY_POOL_SIZE to a higher value!");
		return -FI_EAGAIN;
	}

	rxr_pkt_entry_release_rx(ep, pkt_entry);

	assert(pkt_entry_copy->mr);
	read_entry->context = pkt_entry_copy;
	read_entry->rma_iov[0].addr = (uint64_t)pkt_entry_copy->wiredata + pkt_offset;
	read_entry->rma_iov[0].key = fi_mr_key(pkt_entry_copy->mr);

	return 0;
}

/*
 * rxr_read_mr_reg register the memory of local buffer if application did not
 * provide descriptor.
 * It is called by rxr_read_post().
 * On success, it return 0.
 * If memory registration failed with -FI_ENOMEM, it will return -FI_EAGAIN.
 * If memory registration failed with other error, it will return the error code.
 */
ssize_t rxr_read_mr_reg(struct rxr_ep *ep, struct rxr_read_entry *read_entry)
{
	size_t i;
	int err;

	for (i = 0; i < read_entry->iov_count; ++i) {
		if (read_entry->mr_desc[i] || read_entry->mr[i]) {
			continue;
		}

		err = fi_mr_reg(&rxr_ep_domain(ep)->util_domain.domain_fid,
				read_entry->iov[i].iov_base, read_entry->iov[i].iov_len,
				FI_RECV, 0, 0, 0, &read_entry->mr[i], NULL);

		if (err) {
			/* If registration failed with -FI_ENOMEM, we return -FI_EAGAIN.
			 * This read entry will be put into a queue.
			 *
			 * The progress engine will progress other message transfers, which
			 * will release registrations. Thus, when the progress engine call this
			 * function again later, there will be registrations available.
			 *
			 * All registration opened here will be closed during release of
			 * the read_entry.
			 */
			EFA_WARN(FI_LOG_MR, "Unable to register MR buf for read!\n");
			if (err == -FI_ENOMEM)
				err = -FI_EAGAIN;
			return err;
		}

		read_entry->mr_desc[i] = fi_mr_desc(read_entry->mr[i]);
	}

	return 0;
}

/**
 * @brief convert descriptor from application for lower provider to use
 *
 * Each provider define its descriptors format. The descriptor provided
 * by application is in EFA provider format.
 * This function convert it to descriptors for lower provider according
 * to lower provider type. It also handle the case application does not
 * provider descriptors.
 *
 * @param lower_ep_type[in] lower efa type, can be EFA_EP or SHM_EP.
 * @param numdesc[in]       number of descriptors in the array
 * @param desc_in[in]       descriptors provided by application
 * @param desc_out[out]     descriptors for lower provider.
 */
static inline
void rxr_read_copy_desc(enum rxr_lower_ep_type lower_ep_type,
			int numdesc, void **desc_in, void **desc_out)
{
	if (!desc_in) {
		memset(desc_out, 0, numdesc * sizeof(void *));
		return;
	}

	memcpy(desc_out, desc_in, numdesc * sizeof(void *));
	if (lower_ep_type == SHM_EP) {
		rxr_convert_desc_for_shm(numdesc, desc_out);
	}
}

/* rxr_read_alloc_entry allocates a read entry.
 * It is called by rxr_read_post_or_queue().
 * Input:
 *   x_entry: can be a tx_entry or an rx_entry.
 *            If x_entry is tx_entry, application called fi_read().
 *            If x_entry is rx_entry, read message protocol is being used.
 *   lower_ep_type: EFA_EP or SHM_EP
 * Return:
 *   On success, return the pointer of allocated read_entry
 *   Otherwise, return NULL
 */
struct rxr_read_entry *rxr_read_alloc_entry(struct rxr_ep *ep, struct rxr_op_entry *op_entry,
					    enum rxr_lower_ep_type lower_ep_type)
{
	struct rxr_read_entry *read_entry;
	int i;
	size_t total_iov_len, total_rma_iov_len;

	read_entry = ofi_buf_alloc(ep->read_entry_pool);
	if (OFI_UNLIKELY(!read_entry)) {
		EFA_WARN(FI_LOG_EP_CTRL, "RDMA entries exhausted\n");
		return NULL;
	}

	read_entry->type = RXR_READ_ENTRY;
	read_entry->read_id = ofi_buf_index(read_entry);
	read_entry->state = RXR_RDMA_ENTRY_CREATED;


	read_entry->iov_count = op_entry->iov_count;
	memcpy(read_entry->iov, op_entry->iov,
	       op_entry->iov_count * sizeof(struct iovec));

	read_entry->rma_iov_count = op_entry->rma_iov_count;
	memcpy(read_entry->rma_iov, op_entry->rma_iov,
	       op_entry->rma_iov_count * sizeof(struct fi_rma_iov));

	total_iov_len = ofi_total_iov_len(op_entry->iov, op_entry->iov_count);
	total_rma_iov_len = ofi_total_rma_iov_len(op_entry->rma_iov, op_entry->rma_iov_count);
	read_entry->total_len = MIN(total_iov_len, total_rma_iov_len);

	rxr_read_copy_desc(lower_ep_type, read_entry->iov_count, op_entry->desc, read_entry->mr_desc);

	read_entry->context = op_entry;
	read_entry->addr = op_entry->addr;
	read_entry->lower_ep_type = lower_ep_type;

	if (op_entry->type == RXR_TX_ENTRY) {
		assert(op_entry->op == ofi_op_read_req);
		read_entry->context_type = RXR_READ_CONTEXT_TX_ENTRY;
		read_entry->bytes_submitted = 0;
		read_entry->bytes_finished = 0;
	} else {
		assert(op_entry->type == RXR_RX_ENTRY);
		assert(op_entry->op == ofi_op_write || op_entry->op == ofi_op_msg ||
		       op_entry->op == ofi_op_tagged);
		read_entry->context_type = RXR_READ_CONTEXT_RX_ENTRY;
		read_entry->bytes_submitted = op_entry->bytes_runt;
		read_entry->bytes_finished = op_entry->bytes_runt;
	}

	memset(read_entry->mr, 0, read_entry->iov_count * sizeof(struct fid_mr *));

	if (lower_ep_type == SHM_EP) {
		assert(lower_ep_type == SHM_EP);
		/* FI_MR_VIRT_ADDR is not being set, use 0-based offset instead. */
		if (!(g_shm_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR)) {
			for (i = 0; i < read_entry->rma_iov_count; ++i)
				read_entry->rma_iov[i].addr = 0;
		}
	}

	return read_entry;
}

void rxr_read_release_entry(struct rxr_ep *ep, struct rxr_read_entry *read_entry)
{
	int i, err;

	for (i = 0; i < read_entry->iov_count; ++i) {
		if (read_entry->mr[i]) {
			err = fi_close((struct fid *)read_entry->mr[i]);
			if (err) {
				EFA_WARN(FI_LOG_MR, "Unable to close mr\n");
				rxr_read_write_error(ep, read_entry, -err, FI_EFA_ERR_MR_DEREG);
			}
		}
	}

#ifdef ENABLE_EFA_POISONING
	rxr_poison_mem_region(read_entry, sizeof(struct rxr_read_entry));
#endif
	read_entry->state = RXR_RDMA_ENTRY_FREE;
	ofi_buf_free(read_entry);
}

static inline
int rxr_read_post_or_queue(struct rxr_ep *ep, struct rxr_read_entry *read_entry)
{
	int err;

	err = rxr_read_post(ep, read_entry);
	if (err == -FI_EAGAIN) {
		dlist_insert_tail(&read_entry->pending_entry, &ep->read_pending_list);
		read_entry->state = RXR_RDMA_ENTRY_PENDING;
		err = 0;
	} else if(err) {
		rxr_read_release_entry(ep, read_entry);
		EFA_WARN(FI_LOG_CQ,
			"RDMA post read failed. errno=%d.\n", err);
	}

	return err;
}

int rxr_read_post_local_read_or_queue(struct rxr_ep *ep,
				      struct rxr_op_entry *rx_entry,
				      size_t data_offset,
				      struct rxr_pkt_entry *pkt_entry,
				      char *data, size_t data_size)
{
	int err;
	struct rxr_read_entry *read_entry;

	read_entry = ofi_buf_alloc(ep->read_entry_pool);
	if (!read_entry) {
		EFA_WARN(FI_LOG_EP_CTRL, "RDMA entries exhausted\n");
		return -FI_ENOBUFS;
	}

	read_entry->type = RXR_READ_ENTRY;
	read_entry->read_id = ofi_buf_index(read_entry);
	read_entry->lower_ep_type = EFA_EP;
	read_entry->context_type = RXR_READ_CONTEXT_PKT_ENTRY;
	read_entry->context = pkt_entry;
	read_entry->state = RXR_RDMA_ENTRY_CREATED;
	read_entry->addr = FI_ADDR_NOTAVAIL;
	read_entry->total_len = data_size;
	read_entry->bytes_submitted = 0;
	read_entry->bytes_finished = 0;

	/* setup rma_iov */
	read_entry->rma_iov_count = 1;
	read_entry->rma_iov[0].addr = (uint64_t)data;
	read_entry->rma_iov[0].len = data_size;
	read_entry->rma_iov[0].key = (pkt_entry->mr) ? fi_mr_key(pkt_entry->mr) : 0;

	/* setup iov */
	assert(pkt_entry->x_entry == rx_entry);
	assert(rx_entry->desc && efa_mr_is_hmem(rx_entry->desc[0]));
	read_entry->iov_count = rx_entry->iov_count;
	memset(read_entry->mr, 0, sizeof(*read_entry->mr) * read_entry->iov_count);
	memcpy(read_entry->iov, rx_entry->iov, rx_entry->iov_count * sizeof(struct iovec));
	rxr_read_copy_desc(EFA_EP, rx_entry->iov_count, rx_entry->desc, read_entry->mr_desc);
	ofi_consume_iov_desc(read_entry->iov, read_entry->mr_desc, &read_entry->iov_count, data_offset);
	if (read_entry->iov_count == 0) {
		EFA_WARN(FI_LOG_CQ,
			"data_offset %ld out of range\n",
			data_offset);
		ofi_buf_free(read_entry);
		return -FI_ETRUNC;
	}

	assert(efa_mr_is_hmem(read_entry->mr_desc[0]));
	err = ofi_truncate_iov(read_entry->iov, &read_entry->iov_count, data_size + ep->msg_prefix_size);

	if (err) {
		EFA_WARN(FI_LOG_CQ,
			"data_offset %ld data_size %ld out of range\n",
			data_offset, data_size);
		ofi_buf_free(read_entry);
		return -FI_ETRUNC;
	}

	return rxr_read_post_or_queue(ep, read_entry);
}

/**
 * @brief post read request(s) and update read_entry
 *
 * This function posts read request(s) according to information in read_entry.
 * Depend on read_entry->total_len and max read size of device. This function
 * might issue multiple read requdsts.
 *
 * @param[in]		ep		endpoint
 * @param[in,out]	read_entry	read_entry that has information of the read request.
 * 					If read request is successfully submitted,
 * 					read_entry->bytes_submitted will be updated.
 * @return	On success, return 0
 * 		On failure, return a negative error code.
 */
int rxr_read_post(struct rxr_ep *ep, struct rxr_read_entry *read_entry)
{
	int ret;
	int iov_idx = 0, rma_iov_idx = 0;
	size_t iov_offset = 0, rma_iov_offset = 0;
	size_t total_iov_len, total_rma_iov_len;
	size_t read_once_len, max_read_once_len;
	struct rxr_pkt_entry *pkt_entry;

	assert(read_entry->iov_count > 0);
	assert(read_entry->rma_iov_count > 0);


	if (read_entry->total_len == 0) {
		/* because fi_send uses a pkt_entry as context
		 * we had to use a pkt_entry as context too
		 */
		if (read_entry->lower_ep_type == SHM_EP)
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->shm_tx_pkt_pool, RXR_PKT_FROM_SHM_TX_POOL);
		else
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->efa_tx_pkt_pool, RXR_PKT_FROM_EFA_TX_POOL);

		if (OFI_UNLIKELY(!pkt_entry))
			return -FI_EAGAIN;

		rxr_pkt_init_read_context(ep, read_entry, read_entry->addr, read_entry->read_id, 0, pkt_entry);
		ret = rxr_pkt_entry_read(ep, pkt_entry,
					 read_entry->iov[0].iov_base,
					 0,
					 read_entry->mr_desc[0],
					 read_entry->rma_iov[0].addr,
					 read_entry->rma_iov[0].key);
		if (ret)
			rxr_pkt_entry_release_tx(ep, pkt_entry);
		return ret;
	}

	assert(read_entry->bytes_submitted < read_entry->total_len);
	if (read_entry->context_type == RXR_READ_CONTEXT_PKT_ENTRY) {
		assert(read_entry->lower_ep_type == EFA_EP);
		ret = rxr_read_prepare_pkt_entry_mr(ep, read_entry);
		if (ret)
			return ret;
	}

	if (read_entry->lower_ep_type == EFA_EP) {
		ret = rxr_read_mr_reg(ep, read_entry);
		if (ret)
			return ret;
	}

	max_read_once_len = (read_entry->lower_ep_type == EFA_EP)
				? MIN(rxr_env.efa_read_segment_size, rxr_ep_domain(ep)->device->max_rdma_size)
				: SIZE_MAX;
	assert(max_read_once_len > 0);

	ret = rxr_locate_iov_pos(read_entry->iov, read_entry->iov_count,
				 read_entry->bytes_submitted + ep->msg_prefix_size,
				 &iov_idx, &iov_offset);
	assert(ret == 0);
	if (ret) {
		return ret;
	}

	ret = rxr_locate_rma_iov_pos(read_entry->rma_iov, read_entry->rma_iov_count,
				     read_entry->bytes_submitted,
				     &rma_iov_idx, &rma_iov_offset);
	assert(ret == 0);
	if (ret) {
		return ret; 
	}
	total_iov_len = ofi_total_iov_len(read_entry->iov, read_entry->iov_count);
	total_rma_iov_len = ofi_total_rma_iov_len(read_entry->rma_iov, read_entry->rma_iov_count);
	assert(read_entry->total_len == MIN(total_iov_len, total_rma_iov_len));

	while (read_entry->bytes_submitted < read_entry->total_len) {

		if (read_entry->lower_ep_type == EFA_EP && ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
			return -FI_EAGAIN;

		assert(iov_idx < read_entry->iov_count);
		assert(iov_offset < read_entry->iov[iov_idx].iov_len);
		assert(rma_iov_idx < read_entry->rma_iov_count);
		assert(rma_iov_offset < read_entry->rma_iov[rma_iov_idx].len);

		read_once_len = MIN(read_entry->iov[iov_idx].iov_len - iov_offset,
				    read_entry->rma_iov[rma_iov_idx].len - rma_iov_offset);
		read_once_len = MIN(read_once_len, max_read_once_len);

		if (read_entry->lower_ep_type == SHM_EP)
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->shm_tx_pkt_pool, RXR_PKT_FROM_SHM_TX_POOL);
		else
			pkt_entry = rxr_pkt_entry_alloc(ep, ep->efa_tx_pkt_pool, RXR_PKT_FROM_EFA_TX_POOL);

		if (OFI_UNLIKELY(!pkt_entry))
			return -FI_EAGAIN;

		rxr_pkt_init_read_context(ep, read_entry, read_entry->addr, read_entry->read_id, read_once_len, pkt_entry);
		ret = rxr_pkt_entry_read(ep, pkt_entry,
					 (char *)read_entry->iov[iov_idx].iov_base + iov_offset,
					 read_once_len,
					 read_entry->mr_desc[iov_idx],
					 read_entry->rma_iov[rma_iov_idx].addr + rma_iov_offset,
					 read_entry->rma_iov[rma_iov_idx].key);
		if (ret) {
			rxr_pkt_entry_release_tx(ep, pkt_entry);
			return ret;
		}

		read_entry->bytes_submitted += read_once_len;

		iov_offset += read_once_len;
		assert(iov_offset <= read_entry->iov[iov_idx].iov_len);
		if (iov_offset == read_entry->iov[iov_idx].iov_len) {
			iov_idx += 1;
			iov_offset = 0;
		}

		rma_iov_offset += read_once_len;
		assert(rma_iov_offset <= read_entry->rma_iov[rma_iov_idx].len);
		if (rma_iov_offset == read_entry->rma_iov[rma_iov_idx].len) {
			rma_iov_idx += 1;
			rma_iov_offset = 0;
		}

	}

	if (read_entry->total_len == total_iov_len) {
		assert(iov_idx == read_entry->iov_count);
		assert(iov_offset == 0);
	}

	if (read_entry->total_len == total_rma_iov_len) {
		assert(rma_iov_idx == read_entry->rma_iov_count);
		assert(rma_iov_offset == 0);
	}

	return 0;
}

void rxr_read_write_error(struct rxr_ep *ep, struct rxr_read_entry *read_entry,
			  int err, int prov_errno)
{
	struct rxr_op_entry *tx_entry;
	struct rxr_op_entry *rx_entry;

	if (read_entry->context_type == RXR_READ_CONTEXT_TX_ENTRY) {
		tx_entry = read_entry->context;
		rxr_tx_entry_handle_error(tx_entry, err, prov_errno);
	} else {
		assert(read_entry->context_type == RXR_READ_CONTEXT_RX_ENTRY);
		rx_entry = read_entry->context;
		rxr_rx_entry_handle_error(rx_entry, err, prov_errno);
	}

	if (read_entry->state == RXR_RDMA_ENTRY_PENDING)
		dlist_remove(&read_entry->pending_entry);
}

