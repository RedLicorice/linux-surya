// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>
#include <sound/asound.h>
#include "q6adm.h"
#include "q6afe.h"
#include "q6core.h"
#include "q6dsp-common.h"
#include "q6dsp-errno.h"

#define ADM_CMD_DEVICE_OPEN_V5		0x00010326
#define ADM_CMDRSP_DEVICE_OPEN_V5	0x00010329
#define ADM_CMD_DEVICE_CLOSE_V5		0x00010327
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5	0x00010325
#define ADM_CMD_SET_PP_PARAMS_V6	0x0001035D
#define ADM_CMD_DEVICE_OPEN_V6		0x00010356
#define ADM_CMDRSP_DEVICE_OPEN_V6	0x00010357
#define ADM_ENDPOINT_NONE		0xFFFF
/* In-band SET_PP_PARAMS packets stay below what the DSP's APR transport takes in one message */
#define ADM_PP_PARAMS_MAX_INBAND	3584

#define TIMEOUT_MS 1000
#define RESET_COPP_ID 99
#define INVALID_COPP_ID 0xFF
/* Definition for a legacy device session. */
#define ADM_LEGACY_DEVICE_SESSION	0
#define ADM_MATRIX_ID_AUDIO_RX		0
#define ADM_MATRIX_ID_AUDIO_TX		1

struct q6copp {
	int afe_port;
	int copp_idx;
	int id;
	int topology;
	int mode;
	int rate;
	int bit_width;
	int channels;
	int app_type;
	int acdb_id;
	struct q6adm_ec_ref ec;

	struct aprv2_ibasic_rsp_result_t result;
	struct kref refcount;
	wait_queue_head_t wait;
	struct list_head node;
	struct q6adm *adm;
};

struct q6adm {
	struct apr_device *apr;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	unsigned long copp_bitmap[AFE_MAX_PORTS];
	struct list_head copps_list;
	spinlock_t copps_list_lock;
	struct aprv2_ibasic_rsp_result_t result;
	struct mutex lock;
	wait_queue_head_t matrix_map_wait;
	const char *cal_dir;
};

struct q6adm_cmd_device_open_v5 {
	u16 flags;
	u16 mode_of_operation;
	u16 endpoint_id_1;
	u16 endpoint_id_2;
	u32 topology_id;
	u16 dev_num_channel;
	u16 bit_width;
	u32 sample_rate;
	u8 dev_channel_mapping[8];
} __packed;

struct q6adm_cmd_device_open_v6 {
	struct q6adm_cmd_device_open_v5 v5;
	u16 dev_num_channel_eid2;
	u16 bit_width_eid2;
	u32 sample_rate_eid2;
	u8 dev_channel_mapping_eid2[8];
} __packed;

struct q6adm_cmd_set_pp_params {
	u32 data_payload_addr_lsw;
	u32 data_payload_addr_msw;
	u32 mem_map_handle;
	u32 payload_size;
} __packed;

/*
 * Parameter header of ADM_CMD_SET_PP_PARAMS_V6 (instance aware), which is also
 * the layout of the calibration files: a run of these, each followed by
 * param_size bytes of data padded to four bytes.
 */
struct q6adm_param_hdr_v3 {
	__le32 module_id;
	__le16 instance_id;
	__le16 reserved;
	__le32 param_id;
	__le32 param_size;
} __packed;

struct q6adm_cmd_matrix_map_routings_v5 {
	u32 matrix_id;
	u32 num_sessions;
} __packed;

struct q6adm_session_map_node_v5 {
	u16 session_id;
	u16 num_copps;
} __packed;

static struct q6copp *q6adm_find_copp(struct q6adm *adm, int port_idx,
				  int copp_idx)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_for_each_entry(c, &adm->copps_list, node) {
		if ((port_idx == c->afe_port) && (copp_idx == c->copp_idx)) {
			ret = c;
			kref_get(&c->refcount);
			break;
		}
	}

	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;

}

static int q6adm_apr_send_copp_pkt(struct q6adm *adm, struct q6copp *copp,
				   struct apr_pkt *pkt, uint32_t rsp_opcode)
{
	struct device *dev = adm->dev;
	uint32_t opcode = pkt->hdr.opcode;
	int ret;

	mutex_lock(&adm->lock);
	copp->result.opcode = 0;
	copp->result.status = 0;
	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "Failed to send APR packet\n");
		ret = -EINVAL;
		goto err;
	}

	/* Wait for the callback with copp id */
	if (rsp_opcode)
		ret = wait_event_timeout(copp->wait,
					 (copp->result.opcode == opcode) ||
					 (copp->result.opcode == rsp_opcode),
					 msecs_to_jiffies(TIMEOUT_MS));
	else
		ret = wait_event_timeout(copp->wait,
					 (copp->result.opcode == opcode),
					 msecs_to_jiffies(TIMEOUT_MS));

	if (!ret) {
		dev_err(dev, "ADM copp cmd timedout\n");
		ret = -ETIMEDOUT;
	} else if (copp->result.status > 0) {
		dev_err(dev, "DSP returned error[%d]\n",
			copp->result.status);
		ret = -EINVAL;
	}

err:
	mutex_unlock(&adm->lock);
	return ret;
}

static int q6adm_device_close(struct q6adm *adm, struct q6copp *copp,
			      int port_id, int copp_idx)
{
	struct apr_pkt close;

	close.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					APR_HDR_LEN(APR_HDR_SIZE),
					APR_PKT_VER);
	close.hdr.pkt_size = sizeof(close);
	close.hdr.src_port = port_id;
	close.hdr.dest_port = copp->id;
	close.hdr.token = port_id << 16 | copp_idx;
	close.hdr.opcode = ADM_CMD_DEVICE_CLOSE_V5;

	return q6adm_apr_send_copp_pkt(adm, copp, &close, 0);
}

static void q6adm_free_copp(struct kref *ref)
{
	struct q6copp *c = container_of(ref, struct q6copp, refcount);
	struct q6adm *adm = c->adm;
	unsigned long flags;
	int ret;

	ret = q6adm_device_close(adm, c, c->afe_port, c->copp_idx);
	if (ret < 0)
		dev_err(adm->dev, "Failed to close copp %d\n", ret);

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	clear_bit(c->copp_idx, &adm->copp_bitmap[c->afe_port]);
	list_del(&c->node);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	kfree(c);
}

static int q6adm_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	const struct aprv2_ibasic_rsp_result_t *result = data->payload;
	int port_idx, copp_idx;
	const struct apr_hdr *hdr = &data->hdr;
	struct q6copp *copp;
	struct q6adm *adm = dev_get_drvdata(&adev->dev);

	if (!data->payload_size)
		return 0;

	copp_idx = (hdr->token) & 0XFF;
	port_idx = ((hdr->token) >> 16) & 0xFF;
	if (port_idx < 0 || port_idx >= AFE_MAX_PORTS) {
		dev_err(&adev->dev, "Invalid port idx %d token %d\n",
		       port_idx, hdr->token);
		return 0;
	}
	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
		dev_err(&adev->dev, "Invalid copp idx %d token %d\n",
			copp_idx, hdr->token);
		return 0;
	}

	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT: {
		if (result->status != 0) {
			dev_err(&adev->dev, "cmd = 0x%x return error = 0x%x\n",
				result->opcode, result->status);
		}
		switch (result->opcode) {
		case ADM_CMD_DEVICE_OPEN_V5:
		case ADM_CMD_DEVICE_OPEN_V6:
		case ADM_CMD_DEVICE_CLOSE_V5:
		case ADM_CMD_SET_PP_PARAMS_V6:
			list_for_each_entry(copp, &adm->copps_list, node) {
				if ((port_idx == copp->afe_port) && (copp_idx == copp->copp_idx)) {
					copp->result = *result;
					wake_up(&copp->wait);
					break;
				}
			}
			break;
		case ADM_CMD_MATRIX_MAP_ROUTINGS_V5:
			adm->result = *result;
			wake_up(&adm->matrix_map_wait);
			break;

		default:
			dev_err(&adev->dev, "Unknown Cmd: 0x%x\n",
				result->opcode);
			break;
		}
		return 0;
	}
	case ADM_CMDRSP_DEVICE_OPEN_V5:
	case ADM_CMDRSP_DEVICE_OPEN_V6: {
		struct adm_cmd_rsp_device_open_v5 {
			u32 status;
			u16 copp_id;
			u16 reserved;
		} __packed *open = data->payload;

		copp = q6adm_find_copp(adm, port_idx, copp_idx);
		if (!copp)
			return 0;

		if (open->copp_id == INVALID_COPP_ID) {
			dev_err(&adev->dev, "Invalid coppid rxed %d\n",
				open->copp_id);
			copp->result.status = ADSP_EBADPARAM;
			wake_up(&copp->wait);
			kref_put(&copp->refcount, q6adm_free_copp);
			break;
		}
		copp->result.opcode = hdr->opcode;
		copp->id = open->copp_id;
		wake_up(&copp->wait);
		kref_put(&copp->refcount, q6adm_free_copp);
	}
	break;
	default:
		dev_err(&adev->dev, "Unknown cmd:0x%x\n",
		       hdr->opcode);
		break;
	}

	return 0;
}

static struct q6copp *q6adm_alloc_copp(struct q6adm *adm, int port_idx)
{
	struct q6copp *c;
	int idx;

	idx = find_first_zero_bit(&adm->copp_bitmap[port_idx],
				  MAX_COPPS_PER_PORT);

	if (idx >= MAX_COPPS_PER_PORT)
		return ERR_PTR(-EBUSY);

	c = kzalloc_obj(*c, GFP_ATOMIC);
	if (!c)
		return ERR_PTR(-ENOMEM);

	set_bit(idx, &adm->copp_bitmap[port_idx]);
	c->copp_idx = idx;
	c->afe_port = port_idx;
	c->adm = adm;

	init_waitqueue_head(&c->wait);

	return c;
}

static struct q6copp *q6adm_find_matching_copp(struct q6adm *adm,
					       int port_id, int topology,
					       int mode, int rate,
					       int channel_mode, int bit_width,
					       int app_type, int ec_port)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);

	list_for_each_entry(c, &adm->copps_list, node) {
		if ((port_id == c->afe_port) && (topology == c->topology) &&
		    (mode == c->mode) && (rate == c->rate) &&
		    (bit_width == c->bit_width) && (app_type == c->app_type) &&
		    (ec_port == c->ec.port_id)) {
			ret = c;
			kref_get(&c->refcount);
		}
	}
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;
}

static int q6adm_device_open(struct q6adm *adm, struct q6copp *copp,
			     int port_id, int path, int topology,
			     int channel_mode, int bit_width, int rate)
{
	struct q6adm_cmd_device_open_v6 *open6;
	struct q6adm_cmd_device_open_v5 *open;
	int afe_port = q6afe_get_port_id(port_id);
	bool ec = copp->ec.port_id >= 0;
	struct apr_pkt *pkt;
	int ret, pkt_size = APR_HDR_SIZE + (ec ? sizeof(*open6) : sizeof(*open));

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	open6 = p + APR_HDR_SIZE;
	open = &open6->v5;
	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = afe_port;
	pkt->hdr.dest_port = afe_port;
	pkt->hdr.token = port_id << 16 | copp->copp_idx;
	pkt->hdr.opcode = ec ? ADM_CMD_DEVICE_OPEN_V6 : ADM_CMD_DEVICE_OPEN_V5;
	open->flags = ADM_LEGACY_DEVICE_SESSION;
	open->mode_of_operation = path;
	open->endpoint_id_1 = afe_port;
	open->endpoint_id_2 = ADM_ENDPOINT_NONE;
	open->topology_id = topology;
	open->dev_num_channel = channel_mode & 0x00FF;
	open->bit_width = bit_width;
	open->sample_rate = rate;

	ret = q6dsp_map_channels(&open->dev_channel_mapping[0],
				 channel_mode);
	if (ret)
		return ret;

	if (ec) {
		open->endpoint_id_2 = q6afe_get_port_id(copp->ec.port_id);
		open6->dev_num_channel_eid2 = copp->ec.channels;
		open6->bit_width_eid2 = copp->ec.bit_width;
		open6->sample_rate_eid2 = copp->ec.rate;
		ret = q6dsp_map_channels(&open6->dev_channel_mapping_eid2[0],
					 copp->ec.channels);
		if (ret)
			return ret;
	}

	return q6adm_apr_send_copp_pkt(adm, copp, pkt, ec ? ADM_CMDRSP_DEVICE_OPEN_V6 :
				       ADM_CMDRSP_DEVICE_OPEN_V5);
}

static int q6adm_send_pp_params(struct q6adm *adm, struct q6copp *copp,
				const void *params, size_t size)
{
	struct q6adm_cmd_set_pp_params *cmd;
	int afe_port = q6afe_get_port_id(copp->afe_port);
	int pkt_size = APR_HDR_SIZE + sizeof(*cmd) + size;
	struct apr_pkt *pkt;

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	cmd = p + APR_HDR_SIZE;
	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = afe_port;
	pkt->hdr.dest_port = copp->id;
	pkt->hdr.token = copp->afe_port << 16 | copp->copp_idx;
	pkt->hdr.opcode = ADM_CMD_SET_PP_PARAMS_V6;
	cmd->payload_size = size;
	memcpy(cmd + 1, params, size);

	return q6adm_apr_send_copp_pkt(adm, copp, pkt, 0);
}

/*
 * The COPP's calibration, if the board ships one for this topology, rate and
 * channel count: <firmware-name>/adm-<topology>-<rate>-<channels>.bin, sent
 * in-band right after the open, several parameters per packet. A missing file
 * leaves the topology on the DSP's defaults.
 */
static void q6adm_send_cal(struct q6adm *adm, struct q6copp *copp)
{
	const struct firmware *fw;
	size_t off = 0, used = 0, sent = 0, n = 0;
	char name[128];
	int ret;

	if (!adm->cal_dir || copp->topology == NULL_COPP_TOPOLOGY)
		return;

	snprintf(name, sizeof(name), "%s/adm-%08x-%d-%d.bin", adm->cal_dir,
		 copp->topology, copp->rate, copp->channels);
	if (firmware_request_nowarn(&fw, name, adm->dev))
		return;

	void *buf __free(kfree) = kzalloc(ADM_PP_PARAMS_MAX_INBAND, GFP_KERNEL);
	if (!buf)
		goto out;

	while (off + sizeof(struct q6adm_param_hdr_v3) <= fw->size) {
		const struct q6adm_param_hdr_v3 *in = (const void *)fw->data + off;
		u32 size = le32_to_cpu(in->param_size);
		size_t need = sizeof(*in) + ALIGN(size, 4);

		if (size > fw->size - off - sizeof(*in)) {
			dev_err(adm->dev, "%s: truncated at %zu\n", name, off);
			break;
		}
		off += need;

		if (need > ADM_PP_PARAMS_MAX_INBAND) {
			dev_warn(adm->dev, "%s: module %#x param %#x (%u bytes) too large for in-band, skipped\n",
				 name, le32_to_cpu(in->module_id), le32_to_cpu(in->param_id), size);
			continue;
		}
		if (used + need > ADM_PP_PARAMS_MAX_INBAND) {
			ret = q6adm_send_pp_params(adm, copp, buf, used);
			if (ret < 0)
				dev_warn(adm->dev, "%s: SET_PP_PARAMS failed: %d\n", name, ret);
			sent += used;
			used = 0;
		}
		/* the file is already in the DSP's layout: header, data, padding */
		memcpy(buf + used, in, sizeof(*in) + size);
		memset(buf + used + sizeof(*in) + size, 0, need - sizeof(*in) - size);
		used += need;
		n++;
	}
	if (used) {
		ret = q6adm_send_pp_params(adm, copp, buf, used);
		if (ret < 0)
			dev_warn(adm->dev, "%s: SET_PP_PARAMS failed: %d\n", name, ret);
		sent += used;
	}
	dev_info(adm->dev, "copp %#x: %zu calibration parameters (%zu bytes) from %s\n",
		 copp->id, n, sent, name);
out:
	release_firmware(fw);
}

/**
 * q6adm_open() - open adm and grab a free copp
 *
 * @dev: Pointer to adm child device.
 * @port_id: port id
 * @path: playback or capture path.
 * @rate: rate at which copp is required.
 * @channel_mode: channel mode
 * @topology: adm topology id
 * @perf_mode: performace mode.
 * @bit_width: audio sample bit width
 * @app_type: Application type.
 * @acdb_id: ACDB id
 *
 * Return: Will be an negative on error or a valid copp pointer on success.
 */
struct q6copp *q6adm_open_ec(struct device *dev, int port_id, int path, int rate,
			      int channel_mode, int topology, int perf_mode,
			      uint16_t bit_width, int app_type, int acdb_id,
			      const struct q6adm_ec_ref *ec)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6copp *copp;
	unsigned long flags;
	int ret = 0;

	if (port_id < 0) {
		dev_err(dev, "Invalid port_id %d\n", port_id);
		return ERR_PTR(-EINVAL);
	}

	copp = q6adm_find_matching_copp(adm, port_id, topology, perf_mode,
				      rate, channel_mode, bit_width, app_type,
				      ec ? ec->port_id : -1);
	if (copp) {
		dev_err(dev, "Found Matching Copp 0x%x\n", copp->copp_idx);
		return copp;
	}

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	copp = q6adm_alloc_copp(adm, port_id);
	if (IS_ERR(copp)) {
		spin_unlock_irqrestore(&adm->copps_list_lock, flags);
		return ERR_CAST(copp);
	}

	list_add_tail(&copp->node, &adm->copps_list);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	kref_init(&copp->refcount);
	copp->topology = topology;
	copp->mode = perf_mode;
	copp->rate = rate;
	copp->channels = channel_mode;
	copp->bit_width = bit_width;
	copp->app_type = app_type;
	copp->acdb_id = acdb_id;
	copp->ec.port_id = -1;
	if (ec && ec->port_id >= 0)
		copp->ec = *ec;

	ret = q6adm_device_open(adm, copp, port_id, path, topology,
				channel_mode, bit_width, rate);
	if (ret < 0) {
		kref_put(&copp->refcount, q6adm_free_copp);
		return ERR_PTR(ret);
	}

	q6adm_send_cal(adm, copp);

	return copp;
}
EXPORT_SYMBOL_GPL(q6adm_open_ec);

struct q6copp *q6adm_open(struct device *dev, int port_id, int path, int rate,
	       int channel_mode, int topology, int perf_mode,
	       uint16_t bit_width, int app_type, int acdb_id)
{
	return q6adm_open_ec(dev, port_id, path, rate, channel_mode, topology,
			     perf_mode, bit_width, app_type, acdb_id, NULL);
}
EXPORT_SYMBOL_GPL(q6adm_open);

/**
 * q6adm_get_copp_id() - get copp index
 *
 * @copp: Pointer to valid copp
 *
 * Return: Will be an negative on error or a valid copp index on success.
 **/
int q6adm_get_copp_id(struct q6copp *copp)
{
	if (!copp)
		return -EINVAL;

	return copp->copp_idx;
}
EXPORT_SYMBOL_GPL(q6adm_get_copp_id);

/**
 * q6adm_matrix_map() - Map asm streams and afe ports using payload
 *
 * @dev: Pointer to adm child device.
 * @path: playback or capture path.
 * @payload_map: map between session id and afe ports.
 * @perf_mode: Performace mode.
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_matrix_map(struct device *dev, int path,
		     struct route_payload payload_map, int perf_mode)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6adm_cmd_matrix_map_routings_v5 *route;
	struct q6adm_session_map_node_v5 *node;
	struct apr_pkt *pkt;
	uint16_t *copps_list;
	int ret, i, copp_idx;
	/* Assumes port_ids have already been validated during adm_open */
	struct q6copp *copp;
	int pkt_size = (APR_HDR_SIZE + sizeof(*route) +  sizeof(*node) +
		    (sizeof(uint32_t) * payload_map.num_copps));

	void *matrix_map __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!matrix_map)
		return -ENOMEM;

	pkt = matrix_map;
	route = matrix_map + APR_HDR_SIZE;
	node = matrix_map + APR_HDR_SIZE + sizeof(*route);
	copps_list = matrix_map + APR_HDR_SIZE + sizeof(*route) + sizeof(*node);

	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.token = 0;
	pkt->hdr.opcode = ADM_CMD_MATRIX_MAP_ROUTINGS_V5;
	route->num_sessions = 1;

	switch (path) {
	case ADM_PATH_PLAYBACK:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_RX;
		break;
	case ADM_PATH_LIVE_REC:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_TX;
		break;
	default:
		dev_err(dev, "Wrong path set[%d]\n", path);
		break;
	}

	node->session_id = payload_map.session_id;
	node->num_copps = payload_map.num_copps;

	for (i = 0; i < payload_map.num_copps; i++) {
		int port_idx = payload_map.port_id[i];

		if (port_idx < 0) {
			dev_err(dev, "Invalid port_id %d\n",
				payload_map.port_id[i]);
			return -EINVAL;
		}
		copp_idx = payload_map.copp_idx[i];

		copp = q6adm_find_copp(adm, port_idx, copp_idx);
		if (!copp)
			return -EINVAL;

		copps_list[i] = copp->id;
		kref_put(&copp->refcount, q6adm_free_copp);
	}

	mutex_lock(&adm->lock);
	adm->result.status = 0;
	adm->result.opcode = 0;

	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "routing for stream %d failed ret %d\n",
		       payload_map.session_id, ret);
		goto fail_cmd;
	}
	ret = wait_event_timeout(adm->matrix_map_wait,
				 adm->result.opcode == pkt->hdr.opcode,
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		dev_err(dev, "routing for stream %d failed\n",
		       payload_map.session_id);
		ret = -ETIMEDOUT;
		goto fail_cmd;
	} else if (adm->result.status > 0) {
		dev_err(dev, "DSP returned error[%d]\n",
			adm->result.status);
		ret = -EINVAL;
		goto fail_cmd;
	}

fail_cmd:
	mutex_unlock(&adm->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6adm_matrix_map);

/**
 * q6adm_close() - Close adm copp
 *
 * @dev: Pointer to adm child device.
 * @copp: pointer to previously opened copp
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_close(struct device *dev, struct q6copp *copp)
{
	kref_put(&copp->refcount, q6adm_free_copp);

	return 0;
}
EXPORT_SYMBOL_GPL(q6adm_close);

static int q6adm_probe(struct apr_device *adev)
{
	struct device *dev = &adev->dev;
	struct q6adm *adm;

	adm = devm_kzalloc(dev, sizeof(*adm), GFP_KERNEL);
	if (!adm)
		return -ENOMEM;

	adm->apr = adev;
	dev_set_drvdata(dev, adm);
	adm->dev = dev;
	q6core_get_svc_api_info(adev->svc_id, &adm->ainfo);
	/* optional: where the board's COPP calibration files live */
	of_property_read_string(dev->of_node, "firmware-name", &adm->cal_dir);
	mutex_init(&adm->lock);
	init_waitqueue_head(&adm->matrix_map_wait);

	INIT_LIST_HEAD(&adm->copps_list);
	spin_lock_init(&adm->copps_list_lock);

	return devm_of_platform_populate(dev);
}

#ifdef CONFIG_OF
static const struct of_device_id q6adm_device_id[]  = {
	{ .compatible = "qcom,q6adm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6adm_device_id);
#endif

static struct apr_driver qcom_q6adm_driver = {
	.probe = q6adm_probe,
	.callback = q6adm_callback,
	.driver = {
		.name = "qcom-q6adm",
		.of_match_table = of_match_ptr(q6adm_device_id),
	},
};

module_apr_driver(qcom_q6adm_driver);
MODULE_DESCRIPTION("Q6 Audio Device Manager");
MODULE_LICENSE("GPL v2");
