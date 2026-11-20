// SPDX-License-Identifier: GPL-2.0-only
/*
 * QDSP6 Listen (LSM) service: keyword detection on the aDSP, one session, through /dev/q6lsm.
 *
 * Ported from the downstream techpack/audio dsp/q6lsm.c (Copyright (c) 2013-2019, The Linux Foundation): the
 * APR opcodes and packet layouts are theirs. Left out on purpose: several sessions, ACDB calibration and custom
 * topologies, SSR callbacks, the ALSA msm-lsm-client front end, look-ahead buffering (LAB) and debugfs. Policy (which
 * module, which parameters, which AFE port) stays in userspace, as with the downstream sound-trigger HAL; the driver
 * only carries the commands, maps the memory the DSP reads out of band and queues the DSP's events for read().
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/sound/q6lsm.h>
#include "q6core.h"

/* Opcodes (downstream include/dsp/apr_audio-v2.h) */
#define LSM_SESSION_CMD_SHARED_MEM_MAP_REGIONS		0x00012A7F
#define LSM_SESSION_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00012A80
#define LSM_SESSION_CMD_SHARED_MEM_UNMAP_REGIONS	0x00012A81
#define LSM_SESSION_CMD_OPEN_TX				0x00012A82
#define LSM_SESSION_CMD_SET_PARAMS			0x00012A83
#define LSM_SESSION_CMD_REGISTER_SOUND_MODEL		0x00012A84
#define LSM_SESSION_CMD_DEREGISTER_SOUND_MODEL		0x00012A85
#define LSM_SESSION_CMD_START				0x00012A86
#define LSM_SESSION_CMD_STOP				0x00012A87
#define LSM_SESSION_CMD_CLOSE_TX			0x00012A88
#define LSM_SESSION_CMD_OPEN_TX_V2			0x00012A8B
#define LSM_CMD_ADD_TOPOLOGIES				0x00012A8C
#define LSM_SESSION_CMD_SET_PARAMS_V2			0x00012A8F
#define LSM_SESSION_CMD_OPEN_TX_V3			0x00012A95
#define LSM_SESSION_CMD_SET_PARAMS_V3			0x00012A92

#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL			3
#define LSM_SESSION_ID					1

/* Tunables: data, not code (module parameters, also settable at load time). */
static unsigned int cmd_timeout_ms = 1000;
module_param(cmd_timeout_ms, uint, 0644);
MODULE_PARM_DESC(cmd_timeout_ms, "how long to wait for the DSP's answer to a command");
static unsigned int sample_rate = 16000;
module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate, "LSM_SESSION_CMD_OPEN_TX sampling rate");
static unsigned int event_queue_len = 16;
module_param(event_queue_len, uint, 0444);
MODULE_PARM_DESC(event_queue_len, "DSP events kept for read(); the oldest goes when it is full");
static unsigned int max_oob_bytes = 4 << 20;
module_param(max_oob_bytes, uint, 0644);
MODULE_PARM_DESC(max_oob_bytes, "largest out-of-band payload (a sound model) one ioctl may map");
static unsigned int max_inband_bytes = 512;
module_param(max_inband_bytes, uint, 0644);
MODULE_PARM_DESC(max_inband_bytes, "largest parameter sent inside the APR packet");

/* Packet layouts (downstream include/dsp/q6lsm.h, apr_audio-v2.h) */
struct mem_mapping_hdr {
	u32 data_payload_addr_lsw;
	u32 data_payload_addr_msw;
	u32 mem_map_handle;
} __packed;

struct param_hdr_v1 {
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
} __packed;

struct param_hdr_v2 {
	u32 module_id;
	u32 param_id;
	u32 param_size;
} __packed;

struct param_hdr_v3 {
	u32 module_id;
	u16 instance_id;
	u16 reserved;
	u32 param_id;
	u32 param_size;
} __packed;

struct lsm_session_cmd_set_params_v2 {	/* also LSM_SESSION_CMD_SET_PARAMS, with a v1 param header */
	struct apr_hdr hdr;
	u32 payload_size;
	struct mem_mapping_hdr mem_hdr;
} __packed;

struct lsm_session_cmd_set_params_v3 {
	struct apr_hdr hdr;
	struct mem_mapping_hdr mem_hdr;
	u32 payload_size;
} __packed;

struct lsm_stream_cmd_open_tx {
	struct apr_hdr hdr;
	u16 app_id;
	u16 reserved;
	u32 sampling_rate;
} __packed;

struct lsm_stream_cmd_open_tx_v2 {
	struct apr_hdr hdr;
	u32 topology_id;
} __packed;

struct lsm_stream_cmd_open_tx_v3 {
	struct apr_hdr hdr;
	u32 num_stages;
	u32 topology_id;
	u32 island_enable;
} __packed;

struct lsm_cmd_reg_snd_model {
	struct apr_hdr hdr;
	u32 model_size;
	u32 model_addr_lsw;
	u32 model_addr_msw;
	u32 mem_map_handle;
} __packed;

struct lsm_cmd_add_topologies {
	struct apr_hdr hdr;
	u32 addr_lsw;
	u32 addr_msw;
	u32 mem_map_handle;
	u32 size;
} __packed;

struct avs_cmd_shared_mem_map_regions {
	struct apr_hdr hdr;
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
} __packed;

struct avs_cmd_shared_mem_unmap_regions {
	struct apr_hdr hdr;
	u32 mem_map_handle;
} __packed;

/* Memory the DSP reads (a sound model, a large parameter), mapped for the session's lifetime. */
struct q6lsm_buf {
	struct list_head node;
	void *cpu;
	dma_addr_t dma;
	size_t size;
	u64 dsp_addr;
	u32 handle;
};

struct q6lsm {
	struct apr_device *adev;
	struct device *dev;
	struct device *mem_dev;		/* the child with the DSP's IOMMU stream (lsm-mem) */
	s64 sid;			/* that stream's id, the upper bits of a DSP address; -1 without an IOMMU */
	struct miscdevice misc;

	struct mutex lock;		/* one user, one command at a time */
	bool in_use, opened, started;
	struct list_head bufs;

	/* the command waiting for its answer */
	wait_queue_head_t cmd_wait;
	spinlock_t cmd_lock;
	u32 cmd_opcode;
	bool cmd_done;
	u32 cmd_status;
	u32 cmd_handle;

	/* DSP events for read() */
	wait_queue_head_t ev_wait;
	spinlock_t ev_lock;
	struct q6lsm_event *events;
	unsigned int ev_head, ev_count, ev_dropped;
};

static struct device *lsm_mem_dev;
static s64 lsm_mem_sid = -1;
static DEFINE_MUTEX(lsm_mem_lock);

static void q6lsm_hdr(struct apr_hdr *hdr, u32 size, u32 opcode)
{
	hdr->hdr_field = APR_SEQ_CMD_HDR_FIELD;
	hdr->pkt_size = size;
	hdr->src_port = (LSM_SESSION_ID << 8) | LSM_SESSION_ID;
	hdr->dest_port = (LSM_SESSION_ID << 8) | LSM_SESSION_ID;
	hdr->token = LSM_SESSION_ID;
	hdr->opcode = opcode;
}

static void q6lsm_mmap_hdr(struct apr_hdr *hdr, u32 size, u32 opcode)
{
	hdr->hdr_field = APR_SEQ_CMD_HDR_FIELD;
	hdr->pkt_size = size;
	hdr->src_port = 0;
	hdr->dest_port = LSM_SESSION_ID;
	hdr->token = LSM_SESSION_ID << 8;
	hdr->opcode = opcode;
}

/* Send a packet and wait for the DSP's answer to it: 0, -EIO with the DSP's status logged, or -ETIMEDOUT. */
static int q6lsm_send(struct q6lsm *lsm, struct apr_hdr *hdr)
{
	unsigned long flags;
	long left;
	int ret;

	spin_lock_irqsave(&lsm->cmd_lock, flags);
	lsm->cmd_opcode = hdr->opcode;
	lsm->cmd_done = false;
	lsm->cmd_status = 0;
	spin_unlock_irqrestore(&lsm->cmd_lock, flags);

	ret = apr_send_pkt(lsm->adev, (struct apr_pkt *)hdr);
	if (ret < 0) {
		dev_err(lsm->dev, "send 0x%x: %d\n", hdr->opcode, ret);
		return ret;
	}
	left = wait_event_timeout(lsm->cmd_wait, READ_ONCE(lsm->cmd_done), msecs_to_jiffies(cmd_timeout_ms));
	if (!left) {
		dev_err(lsm->dev, "no answer to 0x%x in %u ms\n", hdr->opcode, cmd_timeout_ms);
		return -ETIMEDOUT;
	}
	if (lsm->cmd_status) {
		dev_err(lsm->dev, "DSP refused 0x%x: status 0x%x\n", hdr->opcode, lsm->cmd_status);
		return -EIO;
	}
	return 0;
}

static void q6lsm_queue_event(struct q6lsm *lsm, u32 opcode, const void *payload, size_t size)
{
	struct q6lsm_event *ev;
	unsigned long flags;

	spin_lock_irqsave(&lsm->ev_lock, flags);
	if (lsm->ev_count == event_queue_len) {	/* full: the oldest goes */
		lsm->ev_head = (lsm->ev_head + 1) % event_queue_len;
		lsm->ev_count--;
		lsm->ev_dropped++;
	}
	ev = &lsm->events[(lsm->ev_head + lsm->ev_count) % event_queue_len];
	ev->opcode = opcode;
	ev->size = min_t(size_t, size, Q6LSM_EVENT_MAX_PAYLOAD);
	ev->time_ns = ktime_get_boottime_ns();
	memcpy(ev->payload, payload, ev->size);
	lsm->ev_count++;
	spin_unlock_irqrestore(&lsm->ev_lock, flags);
	wake_up_interruptible(&lsm->ev_wait);
}

static int q6lsm_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	struct q6lsm *lsm = dev_get_drvdata(&adev->dev);
	const u32 *payload = data->payload;
	u32 opcode = data->hdr.opcode;
	unsigned long flags;

	if (!lsm)
		return 0;

	if (opcode == APR_BASIC_RSP_RESULT || opcode == LSM_SESSION_CMDRSP_SHARED_MEM_MAP_REGIONS) {
		if (data->payload_size < sizeof(u32))
			return 0;
		spin_lock_irqsave(&lsm->cmd_lock, flags);
		if (opcode == LSM_SESSION_CMDRSP_SHARED_MEM_MAP_REGIONS &&
		    lsm->cmd_opcode == LSM_SESSION_CMD_SHARED_MEM_MAP_REGIONS) {
			lsm->cmd_handle = payload[0];
			lsm->cmd_done = true;
		} else if (opcode == APR_BASIC_RSP_RESULT && data->payload_size >= 2 * sizeof(u32) &&
			   payload[0] == lsm->cmd_opcode) {
			lsm->cmd_status = payload[1];
			lsm->cmd_done = true;
		}
		spin_unlock_irqrestore(&lsm->cmd_lock, flags);
		wake_up(&lsm->cmd_wait);
		return 0;
	}

	q6lsm_queue_event(lsm, opcode, data->payload, data->payload_size);
	return 0;
}

static int q6lsm_map(struct q6lsm *lsm, const void __user *src, size_t size, struct q6lsm_buf **out)
{
	struct avs_cmd_shared_mem_map_regions cmd = {};
	struct q6lsm_buf *buf;
	int ret;

	if (!lsm->mem_dev)
		return -ENODEV;
	if (!size || size > max_oob_bytes)
		return -EINVAL;
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf->size = PAGE_ALIGN(size);
	buf->cpu = dma_alloc_coherent(lsm->mem_dev, buf->size, &buf->dma, GFP_KERNEL);
	if (!buf->cpu) {
		kfree(buf);
		return -ENOMEM;
	}
	buf->dsp_addr = lsm->sid < 0 ? buf->dma : buf->dma | ((u64)lsm->sid << 32);
	if (src && copy_from_user(buf->cpu, src, size)) {
		ret = -EFAULT;
		goto free;
	}

	q6lsm_mmap_hdr(&cmd.hdr, sizeof(cmd), LSM_SESSION_CMD_SHARED_MEM_MAP_REGIONS);
	cmd.mem_pool_id = ADSP_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd.num_regions = 1;
	cmd.shm_addr_lsw = lower_32_bits(buf->dsp_addr);
	cmd.shm_addr_msw = upper_32_bits(buf->dsp_addr);
	cmd.mem_size_bytes = buf->size;
	ret = q6lsm_send(lsm, &cmd.hdr);
	if (ret)
		goto free;
	buf->handle = lsm->cmd_handle;
	list_add_tail(&buf->node, &lsm->bufs);
	*out = buf;
	return 0;
free:
	dma_free_coherent(lsm->mem_dev, buf->size, buf->cpu, buf->dma);
	kfree(buf);
	return ret;
}

static void q6lsm_unmap_all(struct q6lsm *lsm)
{
	struct avs_cmd_shared_mem_unmap_regions cmd = {};
	struct q6lsm_buf *buf, *next;

	list_for_each_entry_safe(buf, next, &lsm->bufs, node) {
		q6lsm_mmap_hdr(&cmd.hdr, sizeof(cmd), LSM_SESSION_CMD_SHARED_MEM_UNMAP_REGIONS);
		cmd.mem_map_handle = buf->handle;
		q6lsm_send(lsm, &cmd.hdr);
		dma_free_coherent(lsm->mem_dev, buf->size, buf->cpu, buf->dma);
		list_del(&buf->node);
		kfree(buf);
	}
}

static int q6lsm_simple(struct q6lsm *lsm, u32 opcode)
{
	struct apr_hdr hdr = {};

	q6lsm_hdr(&hdr, sizeof(hdr), opcode);
	return q6lsm_send(lsm, &hdr);
}

static int q6lsm_open_session(struct q6lsm *lsm, const struct q6lsm_open *o)
{
	union {
		struct lsm_stream_cmd_open_tx v1;
		struct lsm_stream_cmd_open_tx_v2 v2;
		struct lsm_stream_cmd_open_tx_v3 v3;
	} cmd = {};
	int ret;

	if (lsm->opened)
		return -EBUSY;
	if ((o->flags & Q6LSM_OPEN_LOAD_MODULES) && o->kind != Q6LSM_OPEN_APP_ID) {
		ret = q6core_load_topology(o->value, true);
		if (ret)
			return ret;
	}
	switch (o->kind) {
	case Q6LSM_OPEN_APP_ID:
		q6lsm_hdr(&cmd.v1.hdr, sizeof(cmd.v1), LSM_SESSION_CMD_OPEN_TX);
		cmd.v1.app_id = o->value;
		cmd.v1.sampling_rate = sample_rate;
		break;
	case Q6LSM_OPEN_TOPOLOGY:
		q6lsm_hdr(&cmd.v2.hdr, sizeof(cmd.v2), LSM_SESSION_CMD_OPEN_TX_V2);
		cmd.v2.topology_id = o->value;
		break;
	case Q6LSM_OPEN_STAGES:
		q6lsm_hdr(&cmd.v3.hdr, sizeof(cmd.v3), LSM_SESSION_CMD_OPEN_TX_V3);
		cmd.v3.num_stages = 1;
		cmd.v3.topology_id = o->value;
		cmd.v3.island_enable = !!o->island;
		break;
	default:
		return -EINVAL;
	}
	ret = q6lsm_send(lsm, &cmd.v1.hdr);
	if (!ret)
		lsm->opened = true;
	return ret;
}

/* The parameter header (v1, v2 or v3, as the flags say) followed by the payload, written at dst. */
static size_t q6lsm_param_hdr(void *dst, const struct q6lsm_param *p)
{
	if (p->flags & Q6LSM_PARAM_V3) {
		struct param_hdr_v3 h = { p->module_id, p->instance_id, 0, p->param_id, p->size };

		memcpy(dst, &h, sizeof(h));
		return sizeof(h);
	}
	if (p->flags & Q6LSM_PARAM_LEGACY) {
		struct param_hdr_v1 h = { p->module_id, p->param_id, p->size, 0 };

		memcpy(dst, &h, sizeof(h));
		return sizeof(h);
	}
	{
		struct param_hdr_v2 h = { p->module_id, p->param_id, p->size };

		memcpy(dst, &h, sizeof(h));
		return sizeof(h);
	}
}

static int q6lsm_set_param(struct q6lsm *lsm, const struct q6lsm_param *p)
{
	const void __user *data = u64_to_user_ptr(p->data);
	bool v3 = p->flags & Q6LSM_PARAM_V3;
	size_t head = v3 ? sizeof(struct lsm_session_cmd_set_params_v3) : sizeof(struct lsm_session_cmd_set_params_v2);
	struct mem_mapping_hdr *mem;
	u32 *payload_size, opcode;
	struct q6lsm_buf *buf;
	size_t hlen;
	u8 *pkt;
	int ret;

	if (!lsm->opened)
		return -ENOTCONN;
	if ((p->flags & Q6LSM_PARAM_LEGACY) && (v3 || p->size > U16_MAX))
		return -EINVAL;
	opcode = v3 ? LSM_SESSION_CMD_SET_PARAMS_V3 :
		 (p->flags & Q6LSM_PARAM_LEGACY) ? LSM_SESSION_CMD_SET_PARAMS : LSM_SESSION_CMD_SET_PARAMS_V2;

	if (!(p->flags & Q6LSM_PARAM_OUT_OF_BAND) && p->size > max_inband_bytes)
		return -E2BIG;
	pkt = kzalloc(head + sizeof(struct param_hdr_v3) +
		      ((p->flags & Q6LSM_PARAM_OUT_OF_BAND) ? 0 : p->size), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;
	if (v3) {
		struct lsm_session_cmd_set_params_v3 *c = (void *)pkt;

		mem = &c->mem_hdr;
		payload_size = &c->payload_size;
	} else {
		struct lsm_session_cmd_set_params_v2 *c = (void *)pkt;

		mem = &c->mem_hdr;
		payload_size = &c->payload_size;
	}

	if (p->flags & Q6LSM_PARAM_OUT_OF_BAND) {
		u8 header[sizeof(struct param_hdr_v3)];

		hlen = q6lsm_param_hdr(header, p);
		ret = q6lsm_map(lsm, NULL, hlen + p->size, &buf);
		if (ret)
			goto out;
		memcpy(buf->cpu, header, hlen);
		if (copy_from_user(buf->cpu + hlen, data, p->size)) {
			ret = -EFAULT;
			goto out;
		}
		mem->data_payload_addr_lsw = lower_32_bits(buf->dsp_addr);
		mem->data_payload_addr_msw = upper_32_bits(buf->dsp_addr);
		mem->mem_map_handle = buf->handle;
		*payload_size = hlen + p->size;
		q6lsm_hdr((struct apr_hdr *)pkt, head, opcode);
	} else {
		hlen = q6lsm_param_hdr(pkt + head, p);
		if (copy_from_user(pkt + head + hlen, data, p->size)) {
			ret = -EFAULT;
			goto out;
		}
		*payload_size = hlen + p->size;
		q6lsm_hdr((struct apr_hdr *)pkt, head + hlen + p->size, opcode);
	}
	ret = q6lsm_send(lsm, (struct apr_hdr *)pkt);
out:
	kfree(pkt);
	return ret;
}

static int q6lsm_register_model(struct q6lsm *lsm, const struct q6lsm_model *m)
{
	struct lsm_cmd_reg_snd_model cmd = {};
	struct q6lsm_buf *buf;
	int ret;

	if (!lsm->opened)
		return -ENOTCONN;
	ret = q6lsm_map(lsm, u64_to_user_ptr(m->data), m->size, &buf);
	if (ret)
		return ret;
	q6lsm_hdr(&cmd.hdr, sizeof(cmd), LSM_SESSION_CMD_REGISTER_SOUND_MODEL);
	cmd.model_size = m->size;
	cmd.model_addr_lsw = lower_32_bits(buf->dsp_addr);
	cmd.model_addr_msw = upper_32_bits(buf->dsp_addr);
	cmd.mem_map_handle = buf->handle;
	return q6lsm_send(lsm, &cmd.hdr);
}

/*
 * The custom topologies of the vendor's calibration (ACDB), which the DSP needs before an OPEN_TX_V2/V3 names one of
 * them: the blob goes as it is (the DSP parses it) to the core service, which registers them for every service.
 */
static int q6lsm_add_topologies(struct q6lsm *lsm, const struct q6lsm_model *m)
{
	dma_addr_t dma;
	size_t size;
	void *cpu;
	int ret;

	if (!lsm->mem_dev)
		return -ENODEV;
	if (!m->size || m->size > max_oob_bytes)
		return -EINVAL;
	if (m->reserved & Q6LSM_TOPOLOGIES_LSM) {	/* the LSM service's own table, mapped for the session */
		struct lsm_cmd_add_topologies cmd = {};
		struct q6lsm_buf *buf;

		ret = q6lsm_map(lsm, u64_to_user_ptr(m->data), m->size, &buf);
		if (ret)
			return ret;
		q6lsm_hdr(&cmd.hdr, sizeof(cmd), LSM_CMD_ADD_TOPOLOGIES);
		cmd.hdr.dest_port = 0;
		cmd.addr_lsw = lower_32_bits(buf->dsp_addr);
		cmd.addr_msw = upper_32_bits(buf->dsp_addr);
		cmd.mem_map_handle = buf->handle;
		cmd.size = m->size;
		return q6lsm_send(lsm, &cmd.hdr);
	}
	size = PAGE_ALIGN(m->size);
	cpu = dma_alloc_coherent(lsm->mem_dev, size, &dma, GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;
	if (copy_from_user(cpu, u64_to_user_ptr(m->data), m->size))
		ret = -EFAULT;
	else
		ret = q6core_register_topologies(lsm->sid < 0 ? dma : dma | ((u64)lsm->sid << 32), m->size);
	dma_free_coherent(lsm->mem_dev, size, cpu, dma);
	return ret;
}

/* Back to nothing: stopped, closed, every mapping gone. Errors are logged; the DSP's state is reset anyway. */
static void q6lsm_teardown(struct q6lsm *lsm)
{
	if (lsm->started)
		q6lsm_simple(lsm, LSM_SESSION_CMD_STOP);
	lsm->started = false;
	if (lsm->opened)
		q6lsm_simple(lsm, LSM_SESSION_CMD_CLOSE_TX);
	lsm->opened = false;
	q6lsm_unmap_all(lsm);
}

static long q6lsm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct q6lsm *lsm = file->private_data;
	void __user *uarg = (void __user *)arg;
	union {
		struct q6lsm_open open;
		struct q6lsm_param param;
		struct q6lsm_model model;
	} a;
	long ret;

	if (_IOC_TYPE(cmd) != Q6LSM_IOC_MAGIC)
		return -ENOTTY;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) &&
	    (_IOC_SIZE(cmd) > sizeof(a) || copy_from_user(&a, uarg, _IOC_SIZE(cmd))))
		return -EFAULT;

	mutex_lock(&lsm->lock);
	switch (cmd) {
	case Q6LSM_IOC_OPEN:
		ret = q6lsm_open_session(lsm, &a.open);
		break;
	case Q6LSM_IOC_SET_PARAM:
		ret = q6lsm_set_param(lsm, &a.param);
		break;
	case Q6LSM_IOC_REGISTER_MODEL:
		ret = q6lsm_register_model(lsm, &a.model);
		break;
	case Q6LSM_IOC_ADD_TOPOLOGIES:
		ret = q6lsm_add_topologies(lsm, &a.model);
		break;
	case Q6LSM_IOC_START:
		ret = lsm->opened ? q6lsm_simple(lsm, LSM_SESSION_CMD_START) : -ENOTCONN;
		if (!ret)
			lsm->started = true;
		break;
	case Q6LSM_IOC_STOP:
		ret = lsm->started ? q6lsm_simple(lsm, LSM_SESSION_CMD_STOP) : 0;
		if (!ret)
			lsm->started = false;
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&lsm->lock);
	return ret;
}

static ssize_t q6lsm_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct q6lsm *lsm = file->private_data;
	struct q6lsm_event ev;
	unsigned long flags;
	int ret;

	if (len < sizeof(ev))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&lsm->ev_lock, flags);
		if (lsm->ev_count) {
			ev = lsm->events[lsm->ev_head];
			lsm->ev_head = (lsm->ev_head + 1) % event_queue_len;
			lsm->ev_count--;
			spin_unlock_irqrestore(&lsm->ev_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&lsm->ev_lock, flags);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(lsm->ev_wait, READ_ONCE(lsm->ev_count));
		if (ret)
			return ret;
	}
	if (copy_to_user(ubuf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static __poll_t q6lsm_poll(struct file *file, poll_table *wait)
{
	struct q6lsm *lsm = file->private_data;

	poll_wait(file, &lsm->ev_wait, wait);
	return READ_ONCE(lsm->ev_count) ? EPOLLIN | EPOLLRDNORM : 0;
}

static int q6lsm_fopen(struct inode *inode, struct file *file)
{
	struct q6lsm *lsm = container_of(file->private_data, struct q6lsm, misc);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&lsm->lock);
	if (lsm->in_use) {
		ret = -EBUSY;
	} else {
		lsm->in_use = true;
		mutex_lock(&lsm_mem_lock);
		lsm->mem_dev = lsm_mem_dev;
		lsm->sid = lsm_mem_sid;
		mutex_unlock(&lsm_mem_lock);
		spin_lock_irqsave(&lsm->ev_lock, flags);
		lsm->ev_head = lsm->ev_count = lsm->ev_dropped = 0;
		spin_unlock_irqrestore(&lsm->ev_lock, flags);
		file->private_data = lsm;
	}
	mutex_unlock(&lsm->lock);
	return ret;
}

static int q6lsm_release(struct inode *inode, struct file *file)
{
	struct q6lsm *lsm = file->private_data;

	mutex_lock(&lsm->lock);
	q6lsm_teardown(lsm);
	lsm->in_use = false;
	mutex_unlock(&lsm->lock);
	return 0;
}

static const struct file_operations q6lsm_fops = {
	.owner = THIS_MODULE,
	.open = q6lsm_fopen,
	.release = q6lsm_release,
	.unlocked_ioctl = q6lsm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.read = q6lsm_read,
	.poll = q6lsm_poll,
	.llseek = noop_llseek,
};

static int q6lsm_probe(struct apr_device *adev)
{
	struct device *dev = &adev->dev;
	struct q6lsm *lsm;
	int ret;

	lsm = devm_kzalloc(dev, sizeof(*lsm), GFP_KERNEL);
	if (!lsm)
		return -ENOMEM;
	lsm->events = devm_kcalloc(dev, event_queue_len, sizeof(*lsm->events), GFP_KERNEL);
	if (!lsm->events || !event_queue_len)
		return -ENOMEM;
	lsm->adev = adev;
	lsm->dev = dev;
	lsm->sid = -1;
	mutex_init(&lsm->lock);
	INIT_LIST_HEAD(&lsm->bufs);
	init_waitqueue_head(&lsm->cmd_wait);
	init_waitqueue_head(&lsm->ev_wait);
	spin_lock_init(&lsm->cmd_lock);
	spin_lock_init(&lsm->ev_lock);
	dev_set_drvdata(dev, lsm);

	lsm->misc.minor = MISC_DYNAMIC_MINOR;
	lsm->misc.name = "q6lsm";
	lsm->misc.fops = &q6lsm_fops;
	lsm->misc.parent = dev;
	ret = misc_register(&lsm->misc);
	if (ret)
		return ret;
	return devm_of_platform_populate(dev);	/* lsm-mem: the DMA device */
}

static void q6lsm_remove(struct apr_device *adev)
{
	struct q6lsm *lsm = dev_get_drvdata(&adev->dev);

	misc_deregister(&lsm->misc);
}

static const struct of_device_id q6lsm_of_match[] = {
	{ .compatible = "qcom,q6lsm" },
	{}
};
MODULE_DEVICE_TABLE(of, q6lsm_of_match);

static struct apr_driver q6lsm_driver = {
	.probe = q6lsm_probe,
	.remove = q6lsm_remove,
	.callback = q6lsm_callback,
	.driver = {
		.name = "qcom-q6lsm",
		.of_match_table = q6lsm_of_match,
	},
};

/* lsm-mem: a child node with the IOMMU stream the DSP reads our memory through (as q6asm-dais). */
static int q6lsm_mem_probe(struct platform_device *pdev)
{
	struct of_phandle_args args;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	mutex_lock(&lsm_mem_lock);
	lsm_mem_sid = of_parse_phandle_with_fixed_args(pdev->dev.of_node, "iommus", 1, 0, &args) < 0 ?
		      -1 : (args.args[0] & 0xf);
	lsm_mem_dev = &pdev->dev;
	mutex_unlock(&lsm_mem_lock);
	return 0;
}

static void q6lsm_mem_remove(struct platform_device *pdev)
{
	mutex_lock(&lsm_mem_lock);
	lsm_mem_dev = NULL;
	mutex_unlock(&lsm_mem_lock);
}

static const struct of_device_id q6lsm_mem_of_match[] = {
	{ .compatible = "qcom,q6lsm-mem" },
	{}
};
MODULE_DEVICE_TABLE(of, q6lsm_mem_of_match);

static struct platform_driver q6lsm_mem_driver = {
	.probe = q6lsm_mem_probe,
	.remove = q6lsm_mem_remove,
	.driver = {
		.name = "qcom-q6lsm-mem",
		.of_match_table = q6lsm_mem_of_match,
	},
};

static int __init q6lsm_init(void)
{
	int ret = platform_driver_register(&q6lsm_mem_driver);

	if (ret)
		return ret;
	ret = apr_driver_register(&q6lsm_driver);
	if (ret)
		platform_driver_unregister(&q6lsm_mem_driver);
	return ret;
}
module_init(q6lsm_init);

static void __exit q6lsm_exit(void)
{
	apr_driver_unregister(&q6lsm_driver);
	platform_driver_unregister(&q6lsm_mem_driver);
}
module_exit(q6lsm_exit);

MODULE_DESCRIPTION("QDSP6 Listen (LSM) keyword detection service, /dev/q6lsm");
MODULE_LICENSE("GPL");
