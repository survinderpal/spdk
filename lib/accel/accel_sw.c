/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/accel_engine.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"
#include "spdk/util.h"

#ifdef SPDK_CONFIG_PMDK
#include "libpmem.h"
#endif

#ifdef SPDK_CONFIG_ISAL
#include "../isa-l/include/igzip_lib.h"
#endif

struct sw_accel_io_channel {
	/* for ISAL */
#ifdef SPDK_CONFIG_ISAL
	struct isal_zstream		stream;
	struct inflate_state		state;
#endif
	struct spdk_poller		*completion_poller;
	TAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
};

/* Post SW completions to a list and complete in a poller as we don't want to
 * complete them on the caller's stack as they'll likely submit another. */
inline static void
_add_to_comp_list(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task, int status)
{
	accel_task->status = status;
	TAILQ_INSERT_TAIL(&sw_ch->tasks_to_complete, accel_task, link);
}

/* Used when the SW engine is selected and the durable flag is set. */
inline static int
_check_flags(int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
#ifndef SPDK_CONFIG_PMDK
		/* PMDK is required to use this flag. */
		SPDK_ERRLOG("ACCEL_FLAG_PERSISTENT set but PMDK not configured. Configure PMDK or do not use this flag.\n");
		return -EINVAL;
#endif
	}
	return 0;
}

static bool
sw_accel_supports_opcode(enum accel_opcode opc)
{
	switch (opc) {
	case ACCEL_OPC_COPY:
	case ACCEL_OPC_FILL:
	case ACCEL_OPC_DUALCAST:
	case ACCEL_OPC_COMPARE:
	case ACCEL_OPC_CRC32C:
	case ACCEL_OPC_COPY_CRC32C:
	case ACCEL_OPC_COMPRESS:
	case ACCEL_OPC_DECOMPRESS:
		return true;
	default:
		return false;
	}
}

static inline void
_pmem_memcpy(void *dst, const void *src, size_t len)
{
#ifdef SPDK_CONFIG_PMDK
	int is_pmem = pmem_is_pmem(dst, len);

	if (is_pmem) {
		pmem_memcpy_persist(dst, src, len);
	} else {
		memcpy(dst, src, len);
		pmem_msync(dst, len);
	}
#else
	SPDK_ERRLOG("Function not defined without SPDK_CONFIG_PMDK enabled.\n");
	assert(0);
#endif
}

static void
_sw_accel_dualcast(void *dst1, void *dst2, void *src, size_t nbytes, int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
		_pmem_memcpy(dst1, src, nbytes);
		_pmem_memcpy(dst2, src, nbytes);
	} else {
		memcpy(dst1, src, nbytes);
		memcpy(dst2, src, nbytes);
	}
}

static void
_sw_accel_copy(void *dst, void *src, size_t nbytes, int flags)
{

	if (flags & ACCEL_FLAG_PERSISTENT) {
		_pmem_memcpy(dst, src, nbytes);
	} else {
		memcpy(dst, src, nbytes);
	}
}

static void
_sw_accel_copyv(void *dst, struct iovec *iov, uint32_t iovcnt, int flags)
{
	uint32_t i;

	for (i = 0; i < iovcnt; i++) {
		assert(iov[i].iov_base != NULL);
		if (flags & ACCEL_FLAG_PERSISTENT) {
			_pmem_memcpy(dst, iov[i].iov_base, (size_t)iov[i].iov_len);
		} else {
			memcpy(dst, iov[i].iov_base, (size_t)iov[i].iov_len);
		}
		dst += iov[i].iov_len;
	}
}

static int
_sw_accel_compare(void *src1, void *src2, size_t nbytes)
{
	return memcmp(src1, src2, nbytes);
}

static void
_sw_accel_fill(void *dst, uint8_t fill, size_t nbytes, int flags)
{
	if (flags & ACCEL_FLAG_PERSISTENT) {
#ifdef SPDK_CONFIG_PMDK
		int is_pmem = pmem_is_pmem(dst, nbytes);

		if (is_pmem) {
			pmem_memset_persist(dst, fill, nbytes);
		} else {
			memset(dst, fill, nbytes);
			pmem_msync(dst, nbytes);
		}
#else
		SPDK_ERRLOG("Function not defined without SPDK_CONFIG_PMDK enabled.\n");
		assert(0);
#endif
	} else {
		memset(dst, fill, nbytes);
	}
}

static void
_sw_accel_crc32c(uint32_t *crc_dst, void *src, uint32_t seed, uint64_t nbytes)
{
	*crc_dst = spdk_crc32c_update(src, nbytes, ~seed);
}

static void
_sw_accel_crc32cv(uint32_t *crc_dst, struct iovec *iov, uint32_t iovcnt, uint32_t seed)
{
	*crc_dst = spdk_crc32c_iov_update(iov, iovcnt, ~seed);
}

static int
_sw_accel_compress(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	sw_ch->stream.next_in = accel_task->src;
	sw_ch->stream.next_out = accel_task->dst;
	sw_ch->stream.avail_in = accel_task->nbytes;
	sw_ch->stream.avail_out = accel_task->nbytes_dst;

	isal_deflate_stateless(&sw_ch->stream);
	if (accel_task->output_size != NULL) {
		assert(accel_task->nbytes_dst > sw_ch->stream.avail_out);
		*accel_task->output_size = accel_task->nbytes_dst - sw_ch->stream.avail_out;
	}

	return 0;
#else
	SPDK_ERRLOG("ISAL option is required to use software compression.\n");
	return -EINVAL;
#endif
}

static int
_sw_accel_decompress(struct sw_accel_io_channel *sw_ch, struct spdk_accel_task *accel_task)
{
#ifdef SPDK_CONFIG_ISAL
	int rc;

	sw_ch->state.next_in = accel_task->src;
	sw_ch->state.avail_in = accel_task->nbytes;
	sw_ch->state.next_out = accel_task->dst;
	sw_ch->state.avail_out = accel_task->nbytes_dst;

	rc = isal_inflate_stateless(&sw_ch->state);
	if (rc) {
		SPDK_ERRLOG("isal_inflate_stateless retunred error %d.\n", rc);
	}
	return rc;
#else
	SPDK_ERRLOG("ISAL option is required to use software decompression.\n");
	return -EINVAL;
#endif
}

static int
sw_accel_submit_tasks(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task)
{
	struct sw_accel_io_channel *sw_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *tmp;
	int rc = 0;

	do {
		switch (accel_task->op_code) {
		case ACCEL_OPC_COPY:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_copy(accel_task->dst, accel_task->src, accel_task->nbytes, accel_task->flags);
			}
			break;
		case ACCEL_OPC_FILL:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_fill(accel_task->dst, accel_task->fill_pattern, accel_task->nbytes, accel_task->flags);
			}
			break;
		case ACCEL_OPC_DUALCAST:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				_sw_accel_dualcast(accel_task->dst, accel_task->dst2, accel_task->src, accel_task->nbytes,
						   accel_task->flags);
			}
			break;
		case ACCEL_OPC_COMPARE:
			rc = _sw_accel_compare(accel_task->src, accel_task->src2, accel_task->nbytes);
			break;
		case ACCEL_OPC_CRC32C:
			if (accel_task->v.iovcnt == 0) {
				_sw_accel_crc32c(accel_task->crc_dst, accel_task->src, accel_task->seed, accel_task->nbytes);
			} else {
				_sw_accel_crc32cv(accel_task->crc_dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->seed);
			}
			break;
		case ACCEL_OPC_COPY_CRC32C:
			rc = _check_flags(accel_task->flags);
			if (rc == 0) {
				if (accel_task->v.iovcnt == 0) {
					_sw_accel_copy(accel_task->dst, accel_task->src, accel_task->nbytes, accel_task->flags);
					_sw_accel_crc32c(accel_task->crc_dst, accel_task->src, accel_task->seed, accel_task->nbytes);
				} else {
					_sw_accel_copyv(accel_task->dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->flags);
					_sw_accel_crc32cv(accel_task->crc_dst, accel_task->v.iovs, accel_task->v.iovcnt, accel_task->seed);
				}
			}
			break;
		case ACCEL_OPC_COMPRESS:
			rc = _sw_accel_compress(sw_ch, accel_task);
			break;
		case ACCEL_OPC_DECOMPRESS:
			rc = _sw_accel_decompress(sw_ch, accel_task);
			break;
		default:
			assert(false);
			break;
		}

		tmp = TAILQ_NEXT(accel_task, link);

		_add_to_comp_list(sw_ch, accel_task, rc);

		accel_task = tmp;
	} while (accel_task);

	return 0;
}

static struct spdk_io_channel *sw_accel_get_io_channel(void);
static int sw_accel_engine_init(void);
static void sw_accel_engine_fini(void *ctxt);
static size_t sw_accel_engine_get_ctx_size(void);

static struct spdk_accel_module_if g_sw_module = {
	.module_init = sw_accel_engine_init,
	.module_fini = sw_accel_engine_fini,
	.write_config_json = NULL,
	.get_ctx_size = sw_accel_engine_get_ctx_size,
	.name			= "software",
	.supports_opcode	= sw_accel_supports_opcode,
	.get_io_channel		= sw_accel_get_io_channel,
	.submit_tasks		= sw_accel_submit_tasks
};

static int
accel_comp_poll(void *arg)
{
	struct sw_accel_io_channel	*sw_ch = arg;
	TAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
	struct spdk_accel_task		*accel_task;

	if (TAILQ_EMPTY(&sw_ch->tasks_to_complete)) {
		return SPDK_POLLER_IDLE;
	}

	TAILQ_INIT(&tasks_to_complete);
	TAILQ_SWAP(&tasks_to_complete, &sw_ch->tasks_to_complete, spdk_accel_task, link);

	while ((accel_task = TAILQ_FIRST(&tasks_to_complete))) {
		TAILQ_REMOVE(&tasks_to_complete, accel_task, link);
		spdk_accel_task_complete(accel_task, accel_task->status);
	}

	return SPDK_POLLER_BUSY;
}

static int
sw_accel_create_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;

	TAILQ_INIT(&sw_ch->tasks_to_complete);
	sw_ch->completion_poller = SPDK_POLLER_REGISTER(accel_comp_poll, sw_ch, 0);

#ifdef SPDK_CONFIG_ISAL
	isal_deflate_stateless_init(&sw_ch->stream);
	sw_ch->stream.level = 1;
	sw_ch->stream.level_buf = calloc(1, ISAL_DEF_LVL1_DEFAULT);
	if (sw_ch->stream.level_buf == NULL) {
		SPDK_ERRLOG("Could not allocate isal internal buffer\n");
		return -ENOMEM;
	}
	sw_ch->stream.level_buf_size = ISAL_DEF_LVL1_DEFAULT;
	isal_inflate_init(&sw_ch->state);
#endif

	return 0;
}

static void
sw_accel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct sw_accel_io_channel *sw_ch = ctx_buf;

#ifdef SPDK_CONFIG_ISAL
	free(sw_ch->stream.level_buf);
#endif

	spdk_poller_unregister(&sw_ch->completion_poller);
}

static struct spdk_io_channel *
sw_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&g_sw_module);
}

static size_t
sw_accel_engine_get_ctx_size(void)
{
	return sizeof(struct spdk_accel_task);
}

static int
sw_accel_engine_init(void)
{
	SPDK_NOTICELOG("Accel framework software engine initialized.\n");
	spdk_io_device_register(&g_sw_module, sw_accel_create_cb, sw_accel_destroy_cb,
				sizeof(struct sw_accel_io_channel), "sw_accel_engine");

	return 0;
}

static void
sw_accel_engine_fini(void *ctxt)
{
	spdk_io_device_unregister(&g_sw_module, NULL);
	spdk_accel_engine_module_finish();
}

SPDK_ACCEL_MODULE_REGISTER(sw, &g_sw_module)
