/* verbs.c */

/*
 * Userspace Software iWARP library for DPDK
 *
 * Authors: Patrick MacArthur <pam@zurich.ibm.com>
 *
 * Copyright (c) 2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <infiniband/driver.h>

#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_ip.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_ring.h>

#include "config_file.h"
#include "interface.h"
#include "kni.h"
#include "usiw_kabi.h"
#include "util.h"
#include "verbs.h"

enum { SIZE_POW2_MAX = (INT_MAX >> 1) + 1 };

static_assert(USIW_VENDOR_ID == USIW_DEVICE_VENDOR_ID,
		"Vendor ID in verbs.h does not match kABI");
static_assert(USIW_VENDOR_PART_ID == USIW_DEVICE_VENDOR_PART_ID,
		"Vendor part ID in verbs.h does not match kABI");

/** Returns the least power of 2 greater than in.  If in is greater than the
 * highest power of 2 representable as a size_t, then the behavior is
 * undefined. */
static int
next_pow2(int in)
{
	int out;
	assert(in < SIZE_POW2_MAX);
	for (out = 1; out < 2 * in; out <<= 1)
		;
	return out;
} /* next_pow2 */


static int
usiw_hash_mr(uintptr_t addr, size_t len)
{
	return rte_jhash_3words(addr >> 32, addr & UINT32_MAX,
			len & UINT32_MAX, 0) & 0xffffff;
} /* usiw_hash_mr */


__attribute__((__visibility__("default")))
struct ibv_mr *
usiw_reg_mr_with_rkey(struct ibv_pd *pd, void *addr, size_t len, int access,
		uint32_t rkey)
{
	struct usiw_mr_table *tbl = container_of(pd, struct usiw_mr_table, pd);
	uint32_t hash = rkey % tbl->capacity;
	struct usiw_mr *mr;

	mr = malloc(sizeof(*mr));
	if (!mr) {
		return NULL;
	}

	mr->mr.addr = addr;
	mr->mr.length = len;
	mr->mr.handle = 0;
	mr->mr.lkey = rkey;
	mr->mr.rkey = rkey;
	mr->next = tbl->entries[hash];
	mr->access = access;
	tbl->entries[hash] = mr;
	return &mr->mr;
} /* usiw_reg_mr_with_rkey */


__attribute__((__visibility__("default")))
int
usiw_accl_post_recvv(struct ibv_qp *ib_qp, const struct iovec *iov, size_t iov_size,
		void *context)
{
	struct usiw_recv_wqe *wqe;
	struct usiw_qp *qp;
	unsigned int y;
	int x;

	if (iov_size > DPDK_VERBS_IOV_LEN_MAX) {
		return -EINVAL;
	}

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	x = qp_get_next_recv_wqe(qp, &wqe);
	if (x < 0)
		return x;

	wqe->wr_context = context;
	memcpy(wqe->iov, iov, iov_size * sizeof(*iov));
	wqe->iov_count = iov_size;
	wqe->total_request_size = 0;
	for (y = 0; y < iov_size; ++y) {
		wqe->total_request_size += iov[y].iov_len;
	}
	wqe->msn = 0;
	wqe->recv_size = 0;
	wqe->input_size = 0;
	x = rte_ring_enqueue(qp->rq0.ring, wqe);
	assert(x == 0);

	return 0;
} /* usiw_accl_post_recvv */


__attribute__((__visibility__("default")))
int
usiw_accl_post_recv(struct ibv_qp *ib_qp, void *addr, size_t length, void *context)
{
	struct iovec iov;
	iov.iov_base = addr;
	iov.iov_len = length;
	return usiw_accl_post_recvv(ib_qp, &iov, 1, context);
} /* usiw_accl_post_recv */


__attribute__((__visibility__("default")))
int
usiw_accl_post_send(struct ibv_qp *ib_qp, void *addr, size_t length,
		struct usiw_ah *ah, void *context)
{
	struct iovec iov;
	iov.iov_base = addr;
	iov.iov_len = length;
	return usiw_accl_post_sendv(ib_qp, &iov, 1, ah, context);
} /* usiw_accl_post_send */


static bool
qp_connected(struct usiw_qp *qp)
{
	return rte_atomic16_read(&qp->conn_state) == usiw_qp_connected;
} /* qp_connected */


__attribute__((__visibility__("default")))
int
usiw_accl_post_sendv(struct ibv_qp *ib_qp, struct iovec *iov, size_t iov_size,
		struct usiw_ah *ah, void *context)
{
	struct usiw_qp *qp;
	struct ee_state *ee;
	struct usiw_send_wqe *wqe;
	unsigned int y;
	int x;

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	if (!ah && !qp_connected(qp)) {
		return -EINVAL;
	}

	if (iov_size > DPDK_VERBS_IOV_LEN_MAX) {
		return -EINVAL;
	}

	ee = usiw_get_ee_context(qp, ah);
	if (!ee) {
		return -EINVAL;
	}

	x = qp_get_next_send_wqe(qp, &wqe);
	if (x < 0)
		return x;

	wqe->opcode = usiw_wr_send;
	wqe->wr_context = context;
	memcpy(wqe->iov, iov, iov_size * sizeof(*iov));
	wqe->iov_count = iov_size;
	wqe->remote_ep = ee;
	wqe->state = SEND_WQE_INIT;
	wqe->msn = 0; /* will be assigned at send time */
	wqe->total_length = 0;
	for (y = 0; y < iov_size; ++y) {
		wqe->total_length += wqe->iov[y].iov_len;
	}
	wqe->bytes_sent = 0;
	wqe->bytes_acked = 0;
	x = rte_ring_enqueue(qp->sq.ring, wqe);
	assert(x == 0);

	return 0;
} /* usiw_accl_post_sendv */


__attribute__((__visibility__("default")))
int
usiw_accl_post_write(struct ibv_qp *ib_qp, void *addr, size_t length,
		struct usiw_ah *ah, uint64_t remote_addr, uint32_t rkey,
		void *context)
{
	struct usiw_qp *qp;
	struct ee_state *ee;
	struct usiw_send_wqe *wqe;
	int x;

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	if (!ah && !qp_connected(qp)) {
		return -EINVAL;
	}

	ee = usiw_get_ee_context(qp, ah);
	if (!ee) {
		return -EINVAL;
	}

	x = qp_get_next_send_wqe(qp, &wqe);
	if (x < 0)
		return x;

	wqe->opcode = usiw_wr_write;
	wqe->wr_context = context;
	wqe->iov[0].iov_base = addr;
	wqe->iov[0].iov_len = length;
	wqe->iov_count = 1;
	wqe->remote_ep = ee;
	wqe->remote_addr = remote_addr;
	wqe->rkey = rkey;
	wqe->state = SEND_WQE_INIT;
	wqe->msn = 0; /* will be assigned at send time */
	wqe->bytes_sent = 0;
	wqe->bytes_acked = 0;
	wqe->total_length = length;
	x = rte_ring_enqueue(qp->sq.ring, wqe);
	assert(x == 0);

	return 0;
} /* usiw_accl_post_write */


__attribute__((__visibility__("default")))
int
usiw_accl_post_read(struct ibv_qp *ib_qp, void *addr, size_t length,
		struct usiw_ah *ah, uint64_t remote_addr, uint32_t rkey,
		void *context)
{
	struct usiw_send_wqe *wqe;
	struct ee_state *ee;
	struct usiw_qp *qp;
	int x;

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	if (!ah && !qp_connected(qp)) {
		return -EINVAL;
	}

	ee = usiw_get_ee_context(qp, ah);
	if (!ee) {
		return -EINVAL;
	}

	x = qp_get_next_send_wqe(qp, &wqe);
	if (x < 0)
		return x;

	wqe->opcode = usiw_wr_read;
	wqe->wr_context = context;
	wqe->iov[0].iov_base = addr;
	wqe->iov[0].iov_len = length;
	wqe->iov_count = 1;
	wqe->remote_addr = remote_addr;
	wqe->rkey = rkey;
	wqe->remote_ep = ee;
	wqe->state = SEND_WQE_INIT;
	wqe->msn = 0; /* will be assigned at send time */
	wqe->local_stag = 0;
	wqe->total_length = length;
	wqe->bytes_sent = 0;
	x = rte_ring_enqueue(qp->sq.ring, wqe);
	assert(x == 0);

	return 0;
} /* usiw_accl_post_read */


static int
usiw_query_device(struct ibv_context *context,
		struct ibv_device_attr *device_attr)
{
	if (!context || !device_attr) {
		return -EINVAL;
	}

	snprintf(device_attr->fw_ver, 64, "0");
	device_attr->node_guid = 0;
	device_attr->sys_image_guid = 0;
	device_attr->max_mr_size = MAX_MR_SIZE;
	device_attr->page_size_cap = 4096;
	device_attr->vendor_id = USIW_VENDOR_ID;
	device_attr->vendor_part_id = USIW_VENDOR_PART_ID;
	device_attr->hw_ver = 0;
	device_attr->max_qp = DPDKV_MAX_QP;
	device_attr->max_qp_wr = RTE_MIN(MAX_SEND_WR, MAX_RECV_WR);
	device_attr->device_cap_flags = 0;
	device_attr->max_sge = DPDK_VERBS_IOV_LEN_MAX;
	device_attr->max_sge_rd = DPDK_VERBS_RDMA_READ_IOV_LEN_MAX;
	device_attr->max_cq = INT_MAX;
	device_attr->max_cqe = RTE_MIN(INT_MAX, SIZE_POW2_MAX);
	device_attr->max_mr = INT_MAX;
	device_attr->max_pd = INT_MAX;
	device_attr->max_qp_rd_atom = 1;
	device_attr->max_ee_rd_atom = 1;
	device_attr->max_res_rd_atom = 1;
	device_attr->max_qp_init_rd_atom = 1;
	device_attr->max_ee_init_rd_atom = 1;
	device_attr->atomic_cap = IBV_ATOMIC_NONE;
	device_attr->max_ee = 0;
	device_attr->max_rdd = 0;
	device_attr->max_mw = 0;
	device_attr->max_raw_ipv6_qp = 0;
	device_attr->max_raw_ethy_qp = 0;
	device_attr->max_mcast_grp = 0;
	device_attr->max_mcast_qp_attach = 0;
	device_attr->max_total_mcast_qp_attach = 0;
	device_attr->max_ah = MAX_ARP_ENTRIES;
	device_attr->max_fmr = 0;
	device_attr->max_srq = 0;
	device_attr->max_pkeys = 0;
	device_attr->local_ca_ack_delay = 0;
	device_attr->phys_port_cnt = 1;

	return 0;
} /* usiw_query_device */


static int
usiw_query_port(struct ibv_context *context, uint8_t port_num,
		struct ibv_port_attr *port_attr)
{
	struct usiw_context *ctx = usiw_get_context(context);

	if (!context || !port_attr || port_num != 1) {
		return -EINVAL;
	}

	memset(port_attr, 0, sizeof(*port_attr));
	port_attr->state = ctx->port->ipv4_addr
		? IBV_PORT_ACTIVE : IBV_PORT_DOWN;
	port_attr->max_mtu = IBV_MTU_1024;
	port_attr->active_mtu = IBV_MTU_1024;
	port_attr->max_msg_sz = UINT32_MAX;
	port_attr->link_layer = IBV_LINK_LAYER_ETHERNET;

	return 0;
} /* usiw_query_port */


static struct ibv_pd *
usiw_alloc_pd(struct ibv_context *context)
{
	static const int default_capacity = 1023;
	struct ibv_alloc_pd cmd;
	struct ibv_alloc_pd_resp resp;
	struct usiw_mr_table *tbl;
	int ret;

	tbl = calloc(1, sizeof(*tbl)
			+ default_capacity * sizeof(struct usiw_mr));
	if (!tbl) {
		return NULL;
	}

	ret = ibv_cmd_alloc_pd(context, &tbl->pd, &cmd, sizeof(cmd), &resp,
			sizeof(resp));
	if (ret) {
		errno = ret;
		free(tbl);
		return NULL;
	}

	tbl->capacity = default_capacity;
	return &tbl->pd;
} /* usiw_alloc_pd */


static int
usiw_dealloc_pd(struct ibv_pd *pd)
{
	struct usiw_mr_table *tbl = container_of(pd, struct usiw_mr_table, pd);
	int ret;

	if (tbl->mr_count) {
		return -EBUSY;
	}
	ret = ibv_cmd_dealloc_pd(pd);

	free(tbl);
	return ret;
} /* usiw_dealloc_pd */

static struct ibv_mr *
usiw_reg_mr(struct ibv_pd *pd, void *addr, size_t len, int access)
{
	uint32_t rkey;
	if (len > MAX_MR_SIZE) {
		errno = EINVAL;
		return NULL;
	}
	rkey = usiw_hash_mr((uintptr_t)addr, len);
	return usiw_reg_mr_with_rkey(pd, addr, len, access, rkey);
} /* usiw_reg_mr */

static struct ibv_mr *
usiw_rereg_mr(__attribute__((unused)) struct ibv_mr *mr,
		__attribute__((unused)) int flags,
		__attribute__((unused)) struct ibv_pd *pd,
		__attribute__((unused)) void *addr,
		__attribute__((unused)) size_t length,
		__attribute__((unused)) int access)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_rereg_mr */

static int
usiw_dereg_mr(struct ibv_mr *mr)
{
	struct usiw_mr_table *tbl = container_of(mr->pd,
			struct usiw_mr_table, pd);
	struct usiw_mr **candidate = usiw_mr_lookup(tbl, mr->rkey);
	struct usiw_mr *ourmr = container_of(mr, struct usiw_mr, mr);
	/* TODO: deal with hash collisions and lookup failure */
	if (ourmr != *candidate) {
		return -EINVAL;
	}

	usiw_dereg_mr_real(tbl, candidate);
	return 0;
} /* usiw_dereg_mr */


static struct ibv_mw *
usiw_alloc_mw(__attribute__((unused)) struct ibv_pd *pd,
		__attribute__((unused)) enum ibv_mw_type type)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_alloc_mw */


static int
usiw_bind_mw(__attribute__((unused)) struct ibv_qp *ib_qp,
		__attribute__((unused)) struct ibv_mw *mw,
		__attribute__((unused)) struct ibv_mw_bind *mw_bind)
{
	return ENOSYS;
} /* usiw_bind_mw */


static int
usiw_dealloc_mw(__attribute__((unused)) struct ibv_mw *mw)
{
	return ENOSYS;
} /* usiw_dealloc_mw */


static struct ibv_cq *
usiw_create_cq(struct ibv_context *context, int size,
		struct ibv_comp_channel *channel, int socket_id)
{
	struct ibv_create_cq cmd;
	struct {
		struct ibv_create_cq_resp ibv;
		struct usiw_uresp_create_cq priv;
	} resp;
	struct usiw_cq *cq;
	unsigned int x;
	char name[RTE_RING_NAMESIZE];
	int ret;

	if (size + 1 > SIZE_POW2_MAX) {
		errno = -EINVAL;
		return NULL;
	}
	size = next_pow2(size + 1) - 1;
	cq = malloc(sizeof(*cq) + size * sizeof(*cq->storage));
	if (!cq)
		return NULL;

	/* Do not pass comp_vector to kernel space, since the kernel space
	 * implementation is just a dummy to support connection management and
	 * does not know or care about the userspace handling of
	 * comp_vectors. */
	ret = ibv_cmd_create_cq(context, size, channel, 0, &cq->ib_cq,
			&cmd, sizeof(cmd), &resp.ibv, sizeof(resp));
	if (ret) {
		errno = ret;
		free(cq);
		return NULL;
	}

	cq->cq_id = resp.priv.cq_id;
	snprintf(name, RTE_RING_NAMESIZE, "cq%" PRIu32 "_ready_ring", cq->cq_id);
	cq->cqe_ring = rte_malloc(NULL, rte_ring_get_memsize(size + 1),
			socket_id);
	if (!cq->cqe_ring) {
		errno = rte_errno;
		ibv_cmd_destroy_cq(&cq->ib_cq);
		free(cq);
		return NULL;
	}
	ret = rte_ring_init(cq->cqe_ring, name, size + 1,
			RING_F_SP_ENQ|RING_F_SC_DEQ);
	if (ret) {
		errno = -ret;
		rte_free(cq->cqe_ring);
		ibv_cmd_destroy_cq(&cq->ib_cq);
		free(cq);
		return NULL;
	}
	snprintf(name, RTE_RING_NAMESIZE, "cq%" PRIu32 "_empty_ring", cq->cq_id);
	cq->free_ring = rte_malloc(NULL, rte_ring_get_memsize(size + 1),
			socket_id);
	if (!cq->cqe_ring) {
		errno = rte_errno;
		rte_free(cq->cqe_ring);
		ibv_cmd_destroy_cq(&cq->ib_cq);
		free(cq);
		return NULL;
	}
	ret = rte_ring_init(cq->free_ring, name, size + 1,
			RING_F_SP_ENQ|RING_F_SC_DEQ);
	if (!cq->free_ring) {
		errno = ret;
		rte_free(cq->free_ring);
		rte_free(cq->cqe_ring);
		ibv_cmd_destroy_cq(&cq->ib_cq);
		free(cq);
		return NULL;
	}

	cq->capacity = size;
	cq->storage = (struct usiw_wc *)(cq + 1);
	for (x = 0; x < cq->capacity; ++x) {
		rte_ring_enqueue(cq->free_ring, &cq->storage[x]);
	}
	cq->qp_count = 0;
	rte_atomic32_init(&cq->notify_count);
	rte_spinlock_init(&cq->lock);
	return &cq->ib_cq;
} /* usiw_create_cq */


static int
do_poll_cq(struct usiw_cq *cq, int num_entries, struct usiw_wc *wc)
{
	void *cqe[num_entries];
	int count, x, ret;

	rte_spinlock_lock(&cq->lock);
	count = rte_ring_dequeue_burst(cq->cqe_ring, cqe, num_entries);
	rte_spinlock_unlock(&cq->lock);
	if (count >= 0) {
		ret = rte_ring_enqueue_burst(cq->free_ring, cqe, count);
		assert(ret == count);
		if (ret < 0) {
			count = ret;
		}
		for (x = 0; x < count; ++x) {
			memcpy(&wc[x], cqe[x], sizeof(struct usiw_wc));
		}
	}

	return count;
} /* do_poll_cq */


static void
convert_cqes(struct usiw_wc *cqe, int num_entries, struct ibv_wc *wc)
{
	int x;

	for (x = 0; x < num_entries; ++x) {
		wc[x].wr_id = (uintptr_t)cqe[x].wr_context;
		wc[x].status = cqe[x].status;
		wc[x].opcode = cqe[x].opcode;
		wc[x].byte_len = cqe[x].byte_len;
		wc[x].qp_num = cqe[x].qp_num;
	}
} /* convert_cqes */


static int
usiw_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
	struct usiw_wc cqe[num_entries];
	struct usiw_cq *ourcq;
	int count;

	ourcq = container_of(cq, struct usiw_cq, ib_cq);
	count = do_poll_cq(ourcq, num_entries, cqe);
	convert_cqes(cqe, count, wc);
	return count;
} /* usiw_poll_cq */


static int
usiw_destroy_cq(struct ibv_cq *cq)
{
	struct usiw_cq *ourcq = container_of(cq, struct usiw_cq, ib_cq);
	int ret;

	if (ourcq->qp_count) {
		errno = EBUSY;
		return -1;
	}
	ret = ibv_cmd_destroy_cq(cq);
	rte_free(ourcq->cqe_ring);
	rte_free(ourcq->free_ring);
	free(ourcq);
	return ret;
} /* usiw_destroy_cq */

static int
usiw_req_notify_cq(struct ibv_cq *ib_cq, int solicited_only)
{
	struct usiw_cq *cq = container_of(ib_cq, struct usiw_cq, ib_cq);
	if (!ib_cq->channel)
		return EINVAL;
	if (solicited_only)
		return 0;

	rte_atomic32_inc(&cq->notify_count);

	return 0;
} /* usiw_req_notify_cq */

static int
usiw_resize_cq(__attribute__((unused)) struct ibv_cq *cq,
		__attribute__((unused)) int cqe)
{
	return ENOSYS;
} /* usiw_resize_cq */


static struct ibv_srq *
usiw_create_srq_ex(__attribute__((unused)) struct ibv_context *context,
		__attribute__((unused))
		struct ibv_srq_init_attr_ex *srq_init_attr_ex)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_create_srq_ex */


static struct ibv_srq *
usiw_create_srq(struct ibv_pd *pd, struct ibv_srq_init_attr *init_attr)
{
	struct ibv_srq_init_attr_ex init_attr_ex;
	init_attr_ex.srq_context = init_attr->srq_context;
	memcpy(&init_attr_ex.attr, &init_attr->attr, sizeof(init_attr_ex.attr));
	init_attr_ex.comp_mask = IBV_SRQ_INIT_ATTR_TYPE|IBV_SRQ_INIT_ATTR_PD;
	init_attr_ex.srq_type = IBV_SRQT_BASIC;
	init_attr_ex.pd = pd;
	return usiw_create_srq_ex(pd->context, &init_attr_ex);
} /* usiw_create_srq */


static int
usiw_modify_srq(__attribute__((unused)) struct ibv_srq *srq,
		__attribute__((unused)) struct ibv_srq_attr *srq_attr,
		__attribute__((unused)) int srq_attr_mask)
{
	return ENOSYS;
} /* usiw_modify_srq */


static int
usiw_get_srq_num(__attribute__((unused)) struct ibv_srq *srq,
		__attribute__((unused)) uint32_t *srq_num)
{
	return ENOSYS;
} /* usiw_get_srq_num */


static int
usiw_query_srq(__attribute__((unused)) struct ibv_srq *srq,
		__attribute__((unused)) struct ibv_srq_attr *srq_attr)
{
	return ENOSYS;
} /* usiw_query_srq */


static int
usiw_destroy_srq(__attribute__((unused)) struct ibv_srq *srq)
{
	return ENOSYS;
} /* usiw_destroy_srq */


static int
usiw_post_srq_recv(__attribute__((unused)) struct ibv_srq *srq,
		__attribute__((unused)) struct ibv_recv_wr *recv_wr,
		__attribute__((unused)) struct ibv_recv_wr **bad_recv_wr)
{
	return ENOSYS;
} /* usiw_post_srq_recv */


static uint16_t
port_get_next_qp(struct usiw_port *port)
{
	uint32_t x;

	if (!port->qp_bitmask) {
		return 0;
	}
	/* Skip special QPN 0 */
	for (x = 1; x < 64; ++x) {
		if (port->qp_bitmask & (UINT64_C(1) << x)) {
			port->qp_bitmask &= ~(UINT64_C(1) << x);
			return x;
		}
	}

	/* Should never get here */
	assert(0);
	return 0;
} /* port_get_next_qpn */


static struct ibv_qp *
usiw_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr)
{
	struct {
		struct ibv_create_qp ibv;
		struct usiw_udata_create_qp priv;
	} cmd;
	struct {
		struct ibv_create_qp_resp ibv;
		struct usiw_uresp_create_qp priv;
	} resp;
	struct usiw_context *ctx;
	struct usiw_qp *qp;
	int retval;

	if ((qp_init_attr->qp_type != IBV_QPT_UD
				&& qp_init_attr->qp_type != IBV_QPT_RC)
			|| qp_init_attr->cap.max_send_wr > MAX_SEND_WR
			|| qp_init_attr->cap.max_recv_wr > MAX_RECV_WR
			|| qp_init_attr->cap.max_send_sge
						> DPDK_VERBS_IOV_LEN_MAX
			|| qp_init_attr->cap.max_recv_sge
						> DPDK_VERBS_IOV_LEN_MAX
			|| qp_init_attr->cap.max_inline_data
			> sizeof(struct iovec) * DPDK_VERBS_IOV_LEN_MAX) {
		errno = EINVAL;
		goto errout;
	}
	qp_init_attr->cap.max_send_wr
		= RTE_MAX(next_pow2(qp_init_attr->cap.max_send_wr) - 1, 63);
	qp_init_attr->cap.max_recv_wr
		= RTE_MAX(next_pow2(qp_init_attr->cap.max_recv_wr) - 1, 63);

	/* By default provide one cache line of scatter-gather elements (the
	 * cache line includes the count at the start) */
	if (!qp_init_attr->cap.max_send_sge) {
		qp_init_attr->cap.max_send_sge = 3;
	}
	if (!qp_init_attr->cap.max_recv_sge) {
		qp_init_attr->cap.max_recv_sge = 3;
	}
	if (qp_init_attr->cap.max_inline_data > sizeof(struct iovec)
					* qp_init_attr->cap.max_send_sge) {
		qp_init_attr->cap.max_send_sge
			= (qp_init_attr->cap.max_inline_data - 1)
			/ sizeof(struct iovec) + 1;
	}

	ctx = usiw_get_context(pd->context);

	qp = malloc(sizeof(struct usiw_qp));
	if (!qp) {
		errno = ENOMEM;
		goto errout;
	}

	/* Create kernel QP for connection manager */
	cmd.priv.ird_max = qp->ird_max = USIW_IRD_MAX;
	cmd.priv.ord_max = qp->ord_max = USIW_ORD_MAX;
	retval = ibv_cmd_create_qp(pd, &qp->ib_qp, qp_init_attr,
			&cmd.ibv, sizeof(cmd), &resp.ibv, sizeof(resp));
	if (retval != 0) {
		errno = retval;
		goto free_user_qp;
	}

	qp->qp_flags = qp_init_attr->sq_sig_all
		? usiw_qp_sig_all : 0;
	rte_atomic16_init(&qp->conn_state);
	rte_atomic16_set(&qp->conn_state, usiw_qp_unbound);
	qp->qp_state = IBV_QPS_INIT;
	rte_spinlock_init(&qp->conn_event_lock);
	qp->ctx = ctx;
	qp->port = ctx->port;
	qp->stats.recv_max_burst_size = RX_BURST_SIZE;
	qp->stats.recv_count_histo = calloc(qp->stats.recv_max_burst_size + 1,
			sizeof(*qp->stats.recv_count_histo));
	if (!qp->stats.recv_count_histo) {
		goto free_kernel_qp;
	}

	qp->send_cq = container_of(qp_init_attr->send_cq,
			struct usiw_cq, ib_cq);
	qp->send_cq->qp_count++;
	qp->recv_cq = container_of(qp_init_attr->recv_cq,
			struct usiw_cq, ib_cq);
	if (qp->send_cq != qp->recv_cq) {
		qp->recv_cq->qp_count++;
	}
	qp->txq_end = qp->txq;
	qp->timer_last = 0;
	qp->pd = container_of(pd, struct usiw_mr_table, pd);

	qp->rx_queue = qp->tx_queue
		= port_get_next_qp(ctx->port);
	if (qp->rx_queue == 0) {
		errno = -retval;
		goto free_kernel_qp;
	}

	retval = usiw_send_wqe_queue_init(qp->ib_qp.qp_num,
			&qp->sq, qp_init_attr->cap.max_send_wr,
			qp_init_attr->cap.max_send_sge);
	if (retval != 0) {
		errno = -retval;
		goto free_kernel_qp;
	}
	qp->sq.max_inline = qp_init_attr->cap.max_inline_data;

	retval = usiw_recv_wqe_queue_init(qp->ib_qp.qp_num,
			&qp->rq0, qp_init_attr->cap.max_recv_wr,
			qp_init_attr->cap.max_recv_sge);
	if (retval != 0) {
		errno = -retval;
		goto free_kernel_qp;
	}

	qp->readresp_store = NULL;
	TAILQ_INIT(&qp->readresp_active);
	TAILQ_INIT(&qp->readresp_empty);
	qp->ird_active = 0;

	if (sem_init(&qp->conn_event_sem, 0, 0) < 0) {
		goto free_kernel_qp;
	}

	rte_atomic32_init(&qp->refcnt);
	rte_atomic32_set(&qp->refcnt, 1);

	rte_spinlock_lock(&ctx->qp_lock);
	rte_atomic32_inc(&ctx->qp_init_count);
	HASH_ADD(hh, ctx->qp, ib_qp.qp_num,
			sizeof(qp->ib_qp.qp_num), qp);
	rte_spinlock_unlock(&ctx->qp_lock);

	return &qp->ib_qp;

free_kernel_qp:
	ibv_cmd_destroy_qp(&qp->ib_qp);
free_user_qp:
	free(qp);
errout:
	return NULL;
} /* usiw_create_qp */

static int
usiw_query_qp(struct ibv_qp *ib_qp, struct ibv_qp_attr *attr, int attr_mask,
					    struct ibv_qp_init_attr *init_attr)
{
	struct usiw_qp *qp;
	if (!attr) {
		return -EINVAL;
	}

	if (attr_mask & (IBV_QP_CUR_STATE|IBV_QP_EN_SQD_ASYNC_NOTIFY
				|IBV_QP_PKEY_INDEX|IBV_QP_QKEY|IBV_QP_AV
				|IBV_QP_RQ_PSN|IBV_QP_ALT_PATH|IBV_QP_SQ_PSN
				|IBV_QP_PATH_MIG_STATE|IBV_QP_DEST_QPN)) {
		return -EOPNOTSUPP;
	}

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	switch (rte_atomic16_read(&qp->conn_state)) {
	case usiw_qp_unbound:
		attr->qp_state = IBV_QPS_INIT;
		break;
	case usiw_qp_connected:
		attr->qp_state = IBV_QPS_RTS;
		break;
	case usiw_qp_shutdown:
		attr->qp_state = IBV_QPS_SQD;
		break;
	case usiw_qp_error:
		attr->qp_state = IBV_QPS_ERR;
		break;
	default:
		assert(0);
		return -EINVAL;
	}

	attr->path_mtu = IBV_MTU_1024;
	attr->qp_access_flags = IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE
			|IBV_ACCESS_REMOTE_READ;
	attr->cap.max_send_wr = qp->sq.max_wr;
	attr->cap.max_recv_wr = qp->rq0.max_wr;
	attr->cap.max_send_sge = qp->sq.max_sge;
	attr->cap.max_recv_sge = qp->rq0.max_sge;
	attr->cap.max_inline_data = qp->sq.max_inline;
	attr->sq_draining = 0;
	attr->max_rd_atomic = 1;
	attr->max_dest_rd_atomic = 1;
	attr->rnr_retry = 0;
	attr->retry_cnt = 0;
	attr->timeout = 0;

	if (init_attr) {
		init_attr->qp_context = ib_qp->qp_context;
		init_attr->send_cq = ib_qp->send_cq;
		init_attr->recv_cq = ib_qp->recv_cq;
		init_attr->srq = ib_qp->srq;
		init_attr->cap = attr->cap;
		init_attr->qp_type = ib_qp->qp_type;
		init_attr->sq_sig_all
			= !!(qp->qp_flags & usiw_qp_sig_all);
	}

	return 0;
} /* usiw_query_qp */


static int
usiw_modify_qp(struct ibv_qp *ib_qp, struct ibv_qp_attr *attr, int attr_mask)
{
	struct usiw_qp *qp;
	struct ibv_modify_qp cmd;
	int ret;

	ret = ibv_cmd_modify_qp(ib_qp, attr, attr_mask, &cmd, sizeof(cmd));
	if (ret != 0) {
		return ret;
	}

	if (!(attr_mask & IBV_QP_STATE)) {
		return 0;
	}

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	switch (attr->qp_state) {
	case IBV_QPS_SQD:
	case IBV_QPS_ERR:
		rte_atomic16_set(&qp->conn_state, usiw_qp_shutdown);
		break;
	default:
		break;
	}

	return 0;
} /* usiw_modify_qp */

static int
usiw_destroy_qp(struct ibv_qp *ib_qp)
{
	struct usiw_qp *qp;
	struct usiw_context *ctx;
	int ret;

	ret = ibv_cmd_destroy_qp(ib_qp);

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	ctx = qp->ctx;
	if (rte_atomic16_read(&qp->conn_state) == usiw_qp_unbound) {
		assert(rte_atomic32_read(&ctx->qp_init_count) != 0);
		rte_atomic32_dec(&ctx->qp_init_count);
	}
	qp->recv_cq->qp_count--;
	if (qp->send_cq != qp->recv_cq) {
		qp->send_cq->qp_count--;
	}
	rte_atomic16_set(&qp->conn_state, usiw_qp_error);

	rte_spinlock_lock(&ctx->qp_lock);
	HASH_DEL(ctx->qp, qp);
	rte_spinlock_unlock(&ctx->qp_lock);

	if (rte_atomic32_sub_return(&qp->refcnt, 1) == 0) {
		usiw_do_destroy_qp(qp);
	}

	return ret;
} /* usiw_destroy_qp */

static int
do_inline(struct usiw_qp *qp, struct usiw_send_wqe *wqe,
		struct ibv_send_wr *wr)
{
	unsigned int offset;
	struct ibv_sge *sge;
	char *dest;
	int index;

	wqe->flags |= usiw_send_inline;
	dest = (char *)wqe->iov;
	for (index = 0, offset = 0; index < wr->num_sge; ++index) {
		sge = &wr->sg_list[index];
		if (offset + sge->length > qp->sq.max_inline) {
			return EINVAL;
		}
		memcpy(dest + offset, (char *)(uintptr_t)sge->addr,
				sge->length);
		offset += sge->length;
	}
	wqe->total_length = offset;
	return 0;
} /* do_inline */

static int
usiw_post_send(struct ibv_qp *ib_qp, struct ibv_send_wr *wr,
		struct ibv_send_wr **bad_wr)
{
	struct usiw_qp *qp;
	struct usiw_send_wqe *wqe;
	struct usiw_mr **mr;
	int sge_limit, x, ret;

	if (!wr) {
		ret = EINVAL;
		goto errout;
	}

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	switch (rte_atomic16_read(&qp->conn_state)) {
	case usiw_qp_connected:
		break;
	case usiw_qp_shutdown:
	case usiw_qp_error:
		ret = EINVAL;
		goto errout;
	default:
		/* This is an ugly hack to get around the fact that
		 * ibv_modify_qp doesn't get called from userspace to let us
		 * know about state changes.  We call ibv_modify_qp() ourselves
		 * with attr_mask==0 to get the udata response that contains
		 * the addressing information that we need about the
		 * connection. */
		sem_wait(&qp->conn_event_sem);
		if (!qp_connected(qp)) {
			ret = EINVAL;
			goto errout;
		}
		break;
	}
	for (; wr != NULL; wr = wr->next) {
		sge_limit = (wr->opcode == IBV_WR_RDMA_READ)
					? 1 : qp->sq.max_sge;
		if (wr->num_sge > sge_limit) {
			ret = EINVAL;
			goto errout;
		}

		ret = qp_get_next_send_wqe(qp, &wqe);
		if (ret < 0) {
			goto errout;
		}

		wqe->flags = ((wr->send_flags & IBV_SEND_SIGNALED)
				|| (qp->qp_flags & usiw_qp_sig_all))
			? usiw_send_signaled : 0;

		switch (wr->opcode) {
		case IBV_WR_SEND:
			wqe->opcode = usiw_wr_send;
			if ((wr->send_flags & IBV_SEND_INLINE)
					&& (ret = do_inline(qp, wqe, wr))!=0) {
				goto errout;
			}
			break;
		case IBV_WR_RDMA_WRITE:
			wqe->opcode = usiw_wr_write;
			wqe->remote_addr = wr->wr.rdma.remote_addr;
			wqe->rkey = wr->wr.rdma.rkey;
			if ((wr->send_flags & IBV_SEND_INLINE)
					&& (ret = do_inline(qp, wqe, wr))!=0) {
				goto errout;
			}
			break;
		case IBV_WR_RDMA_READ:
			if (wr->send_flags & IBV_SEND_INLINE) {
				ret = EINVAL;
				goto errout;
			}
			wqe->opcode = usiw_wr_read;
			wqe->remote_addr = wr->wr.rdma.remote_addr;
			wqe->rkey = wr->wr.rdma.rkey;
			mr = usiw_mr_lookup(qp->pd, wr->sg_list[0].lkey);
			if (!mr || !((*mr)->access & IBV_ACCESS_REMOTE_WRITE)) {
				ret = EINVAL;
				goto free_wqe;
			}
			wqe->local_stag = (*mr)->mr.rkey;
			break;
		default:
			ret = EOPNOTSUPP;
			goto free_wqe;
		}
		wqe->wr_context = (void *)(uintptr_t)wr->wr_id;
		wqe->iov_count = wr->num_sge;
		wqe->remote_ep = qp->ep_default;
		wqe->state = SEND_WQE_INIT;
		wqe->msn = 0; /* will be assigned at send time */
		if (!(wqe->flags & usiw_send_inline)) {
			wqe->total_length = 0;
			for (x = 0; x < wr->num_sge; ++x) {
				wqe->iov[x].iov_base
					=(void *)(uintptr_t)wr->sg_list[x].addr;
				wqe->iov[x].iov_len = wr->sg_list[x].length;
				wqe->total_length += wqe->iov[x].iov_len;
			}
		}
		wqe->bytes_sent = 0;
		wqe->bytes_acked = 0;
		x = rte_ring_enqueue(qp->sq.ring, wqe);
		assert(x == 0);
	}

	return 0;

free_wqe:
	qp_free_send_wqe(qp, wqe, 0);
errout:
	*bad_wr = wr;
	return ret;
} /* usiw_post_send */

static int
usiw_post_recv(struct ibv_qp *ib_qp, struct ibv_recv_wr *wr,
		struct ibv_recv_wr **bad_wr)
{
	struct usiw_recv_wqe *wqe;
	struct usiw_qp *qp;
	int x, ret;

	qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	if (rte_atomic16_read(&qp->conn_state) == usiw_qp_error) {
		*bad_wr = wr;
		return EINVAL;
	}

	for (; wr != NULL; wr = wr->next) {
		if (wr->num_sge > qp->rq0.max_sge) {
			ret = EINVAL;
			goto errout;
		}

		ret = qp_get_next_recv_wqe(qp, &wqe);
		if (ret < 0) {
			ret = -ret;
			goto errout;
		}

		wqe->wr_context = (void *)(uintptr_t)wr->wr_id;
		wqe->total_request_size = 0;
		wqe->iov_count = wr->num_sge;
		for (x = 0; x < wr->num_sge; ++x) {
			wqe->iov[x].iov_base
				= (void *)(uintptr_t)wr->sg_list[x].addr;
			wqe->iov[x].iov_len = wr->sg_list[x].length;
			wqe->total_request_size += wqe->iov[x].iov_len;
		}
		wqe->remote_ep = qp->ep_default;
		wqe->msn = 0;
		wqe->recv_size = 0;
		wqe->input_size = 0;
		x = rte_ring_enqueue(qp->rq0.ring, wqe);
		assert(x == 0);
	}

	return 0;

errout:
	*bad_wr = wr;
	return ret;
} /* usiw_post_recv */

static struct ibv_ah *
usiw_create_ah(__attribute__((unused)) struct ibv_pd *pd,
		__attribute__((unused)) struct ibv_ah_attr *attr)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_create_ah */


static int
usiw_destroy_ah(__attribute__((unused)) struct ibv_ah *ah)
{
	return ENOSYS;
} /* usiw_destroy_ah */


static int
usiw_attach_mcast(__attribute__((unused)) struct ibv_qp *ib_qp,
		__attribute__((unused)) const union ibv_gid *gid,
		__attribute__((unused)) uint16_t lid)
{
	return ENOSYS;
} /* usiw_attach_mcast */


static int
usiw_detach_mcast(__attribute__((unused)) struct ibv_qp *ib_qp,
		__attribute__((unused)) const union ibv_gid *gid,
		__attribute__((unused)) uint16_t lid)
{
	return ENOSYS;
} /* usiw_detach_mcast */


static struct ibv_qp *
usiw_open_qp(__attribute__((unused)) struct ibv_context *context,
		__attribute__((unused)) struct ibv_qp_open_attr *attr)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_open_qp */


static struct ibv_qp *
usiw_create_qp_ex(struct ibv_context *context,
		struct ibv_qp_init_attr_ex *qp_init_attr_ex)
{
	if (!(qp_init_attr_ex->comp_mask & IBV_QP_INIT_ATTR_PD)
			|| qp_init_attr_ex->pd->context != context
			|| ((qp_init_attr_ex->comp_mask & IBV_QP_INIT_ATTR_XRCD)
				&& qp_init_attr_ex->xrcd != NULL)) {
		errno = EINVAL;
		return NULL;
	}

	return usiw_create_qp(qp_init_attr_ex->pd,
			(struct ibv_qp_init_attr *)qp_init_attr_ex);
} /* usiw_create_qp_ex */


static struct ibv_xrcd *
usiw_open_xrcd(__attribute__((unused)) struct ibv_context *context,
		__attribute__((unused))
		struct ibv_xrcd_init_attr *xrcd_init_attr)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_open_xrcd */


static int
usiw_close_xrcd(__attribute__((unused)) struct ibv_xrcd *xrcd)
{
	return -ENOSYS;
} /* usiw_close_xrcd */


static struct ibv_flow *
usiw_create_flow(__attribute__((unused)) struct ibv_qp *ib_qp,
		__attribute__((unused)) struct ibv_flow_attr *flow_attr)
{
	errno = ENOSYS;
	return NULL;
} /* usiw_create_flow */

static int
usiw_destroy_flow(__attribute__((unused)) struct ibv_flow *flow)
{
	return ENOSYS;
} /* usiw_destroy_flow */


static int
usiw_query_device_ex(struct ibv_context *context,
		__attribute__((unused))
		const struct ibv_query_device_ex_input *input,
		struct ibv_device_attr_ex *attr,
		__attribute__((unused)) size_t attr_size)
{
	int ret;

	/* ignore input and attr_size for now --- the only way that we can get
	 * here is if the caller supports extended verbs, and the odp_caps was
	 * added at the same time, so no comp_mask bits are defined yet. */
	assert(attr_size >= offsetof(struct ibv_device_attr_ex, odp_caps)
			+ sizeof(attr->odp_caps));
	ret = usiw_query_device(context, &attr->orig_attr);
	if (ret < 0) {
		return ret;
	}
	attr->comp_mask = 0;
	memset(&attr->odp_caps, 0, sizeof(attr->odp_caps));
	return 0;
} /* usiw_query_device_ex */


static struct ibv_context_ops usiw_ops = {
	.query_device = usiw_query_device,
	.query_port = usiw_query_port,
	.alloc_pd = usiw_alloc_pd,
	.dealloc_pd = usiw_dealloc_pd,
	.reg_mr = usiw_reg_mr,
	.rereg_mr = usiw_rereg_mr,
	.dereg_mr = usiw_dereg_mr,
	.alloc_mw = usiw_alloc_mw,
	.bind_mw = usiw_bind_mw,
	.dealloc_mw = usiw_dealloc_mw,
	.create_cq = usiw_create_cq,
	.poll_cq = usiw_poll_cq,
	.req_notify_cq = usiw_req_notify_cq,
	.cq_event = NULL,
	.resize_cq = usiw_resize_cq,
	.destroy_cq = usiw_destroy_cq,
	.create_srq = usiw_create_srq,
	.modify_srq = usiw_modify_srq,
	.query_srq = usiw_query_srq,
	.destroy_srq = usiw_destroy_srq,
	.post_srq_recv = usiw_post_srq_recv,
	.create_qp = usiw_create_qp,
	.query_qp = usiw_query_qp,
	.modify_qp = usiw_modify_qp,
	.destroy_qp = usiw_destroy_qp,
	.post_send = usiw_post_send,
	.post_recv = usiw_post_recv,
	.create_ah = usiw_create_ah,
	.destroy_ah = usiw_destroy_ah,
	.attach_mcast = usiw_attach_mcast,
	.detach_mcast = usiw_detach_mcast,
	.async_event = NULL,
};

static unsigned int
usiw_num_completion_vectors(void)
{
	unsigned max_socket_id;
	unsigned socket_id;
	unsigned lcore_id;

	max_socket_id = 0;
	RTE_LCORE_FOREACH(lcore_id) {
		socket_id = rte_lcore_to_socket_id(lcore_id);
		if (socket_id > max_socket_id) {
			max_socket_id = socket_id;
		}
	}

	return max_socket_id + 1;
} /* usiw_num_completion_vectors */

void
usiw_query_qp_stats(const struct ibv_qp *restrict ib_qp,
		struct usiw_qp_stats *restrict stats)
{
	struct usiw_qp *qp = container_of(ib_qp, struct usiw_qp, ib_qp);
	memcpy(stats, &qp->stats, sizeof(*stats));
} /* usiw_port_get_stats */


int
usiw_init_context(struct verbs_device *device, struct ibv_context *context,
		int cmd_fd)
{
	struct ibv_get_context cmd;
	struct {
		struct ibv_get_context_resp ibv;
		struct usiw_uresp_alloc_ctx priv;
	} resp;
	struct usiw_context *ctx;
	struct usiw_device *dev;
	int ret;

	context->cmd_fd = cmd_fd;
	if ((ret = ibv_cmd_get_context(context, &cmd, sizeof(cmd),
					&resp.ibv, sizeof(resp)) != 0)) {
		RTE_LOG(DEBUG, USER1, "ibv_cmd_get_context failed: %s\n",
				strerror(ret));
		errno = ret;
		return -1;
	}

	ctx = usiw_get_context(context);
	ctx->event_fd = resp.priv.event_fd;
	memcpy(&ctx->vcontext.context.ops, &usiw_ops,
			sizeof(ctx->vcontext.context.ops));
	ctx->vcontext.context.num_comp_vectors = usiw_num_completion_vectors();
	verbs_set_ctx_op(&ctx->vcontext, query_device_ex,
			&usiw_query_device_ex);
	verbs_set_ctx_op(&ctx->vcontext, ibv_destroy_flow, &usiw_destroy_flow);
	verbs_set_ctx_op(&ctx->vcontext, ibv_create_flow, &usiw_create_flow);
	verbs_set_ctx_op(&ctx->vcontext, open_qp, &usiw_open_qp);
	verbs_set_ctx_op(&ctx->vcontext, create_qp_ex, &usiw_create_qp_ex);
	verbs_set_ctx_op(&ctx->vcontext, get_srq_num, &usiw_get_srq_num);
	verbs_set_ctx_op(&ctx->vcontext, create_srq_ex, &usiw_create_srq_ex);
	verbs_set_ctx_op(&ctx->vcontext, open_xrcd, &usiw_open_xrcd);
	verbs_set_ctx_op(&ctx->vcontext, close_xrcd, &usiw_close_xrcd);

	dev = container_of(device, struct usiw_device, vdev);
	ctx->port = dev->port;
	ctx->port->ctx = ctx;

	rte_atomic32_init(&ctx->qp_init_count);
	LIST_INIT(&ctx->qp_active);

	ctx->qp = NULL;
	rte_spinlock_init(&ctx->qp_lock);

	return 0;
} /* usiw_init_context */


void
usiw_uninit_context(__attribute__((unused)) struct verbs_device *device,
		__attribute__((unused)) struct ibv_context *ib_ctx)
{
} /* usiw_uninit_context */
