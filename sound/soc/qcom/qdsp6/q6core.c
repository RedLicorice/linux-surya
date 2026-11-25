// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/jiffies.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <linux/soc/qcom/apr.h>
#include "q6core.h"
#include "q6dsp-errno.h"

#define ADSP_STATE_READY_TIMEOUT_MS    3000
#define Q6_READY_TIMEOUT_MS 100
#define AVCS_CMD_ADSP_EVENT_GET_STATE		0x0001290C
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE	0x0001290D
#define AVCS_GET_VERSIONS       0x00012905
#define AVCS_GET_VERSIONS_RSP   0x00012906
#define AVCS_CMD_GET_FWK_VERSION	0x001292c
#define AVCS_CMDRSP_GET_FWK_VERSION	0x001292d
#define AVCS_CMD_REGISTER_TOPOLOGIES	0x00012923
#define AVCS_CMD_SHARED_MEM_MAP_REGIONS	0x00012924
#define AVCS_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00012925
#define AVCS_CMD_SHARED_MEM_UNMAP_REGIONS	0x00012926
#define AVCS_CMD_DEREGISTER_TOPOLOGIES	0x0001292a
#define AVCS_MODE_DEREGISTER_ALL_CUSTOM_TOPOLOGIES	2
#define AVCS_CMD_LOAD_TOPO_MODULES	0x0001296C
#define AVCS_CMD_UNLOAD_TOPO_MODULES	0x0001296D
#define AVCS_MEM_POOL_SHMEM8_4K		3
#define AVCS_TOPOLOGIES_TIMEOUT_MS	1000

#define ACDB_SECTION_GPROPLUT	0x54554c504f525047
#define ACDB_SECTION_DATAPOOL	0x4c4f4f5041544144
#define ACDB_TOPOLOGIES_BLOB	0x000131a7


/* AVCS_CMD_REGISTER_TOPOLOGIES and AVCS_CMD_DEREGISTER_TOPOLOGIES (with a mode) */
struct avcs_cmd_topologies {
	struct apr_hdr hdr;
	u32 addr_lsw;
	u32 addr_msw;
	u32 mem_map_handle;
	u32 size;
	u32 mode;
} __packed;

struct avcs_cmd_shared_mem_map_regions {
	struct apr_hdr hdr;
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
} __packed;

struct avcs_cmd_shared_mem_unmap_regions {
	struct apr_hdr hdr;
	u32 mem_map_handle;
} __packed;

struct avcs_svc_info {
	uint32_t service_id;
	uint32_t version;
} __packed;

struct avcs_cmdrsp_get_version {
	uint32_t build_id;
	uint32_t num_services;
	struct avcs_svc_info svc_api_info[];
} __packed;

/* for ADSP2.8 and above */
struct avcs_svc_api_info {
	uint32_t service_id;
	uint32_t api_version;
	uint32_t api_branch_version;
} __packed;

struct avcs_cmdrsp_get_fwk_version {
	uint32_t build_major_version;
	uint32_t build_minor_version;
	uint32_t build_branch_version;
	uint32_t build_subbranch_version;
	uint32_t num_services;
	struct avcs_svc_api_info svc_api_info[];
} __packed;

struct q6core {
	struct apr_device *adev;
	wait_queue_head_t wait;
	uint32_t avcs_state;
	struct mutex lock;
	bool resp_received;
	uint32_t num_services;
	struct avcs_cmdrsp_get_fwk_version *fwk_version;
	struct avcs_cmdrsp_get_version *svc_version;
	bool fwk_version_supported;
	bool get_state_supported;
	bool get_version_supported;
	bool is_version_requested;
	/* the command q6core_avcs_cmd() waits for: its opcode, then the DSP's status and a map's handle */
	u32 cmd_opcode;
	bool cmd_done;
	u32 cmd_status;
	u32 cmd_handle;
};

static struct q6core *g_core;

static int q6core_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	struct q6core *core = dev_get_drvdata(&adev->dev);
	const struct aprv2_ibasic_rsp_result_t *result;
	const struct apr_hdr *hdr = &data->hdr;

	result = data->payload;
	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT:{
		result = data->payload;
		switch (result->opcode) {
		case AVCS_GET_VERSIONS:
			if (result->status == ADSP_EUNSUPPORTED)
				core->get_version_supported = false;
			core->resp_received = true;
			break;
		case AVCS_CMD_GET_FWK_VERSION:
			if (result->status == ADSP_EUNSUPPORTED)
				core->fwk_version_supported = false;
			core->resp_received = true;
			break;
		case AVCS_CMD_ADSP_EVENT_GET_STATE:
			if (result->status == ADSP_EUNSUPPORTED)
				core->get_state_supported = false;
			core->resp_received = true;
			break;
		default:
			if (result->opcode == core->cmd_opcode) {
				core->cmd_status = result->status;
				core->cmd_done = true;
				wake_up(&core->wait);
			}
		}
		break;
	}
	case AVCS_CMDRSP_SHARED_MEM_MAP_REGIONS:
		if (core->cmd_opcode == AVCS_CMD_SHARED_MEM_MAP_REGIONS) {
			core->cmd_handle = *(const u32 *)data->payload;
			core->cmd_status = 0;
			core->cmd_done = true;
			wake_up(&core->wait);
		}
		break;
	case AVCS_CMDRSP_GET_FWK_VERSION: {
		struct avcs_cmdrsp_get_fwk_version *fwk;

		fwk = data->payload;

		core->fwk_version = kmemdup(data->payload,
					    struct_size(fwk, svc_api_info,
							fwk->num_services),
					    GFP_ATOMIC);
		if (!core->fwk_version)
			return -ENOMEM;

		core->fwk_version_supported = true;
		core->resp_received = true;

		break;
	}
	case AVCS_GET_VERSIONS_RSP: {
		struct avcs_cmdrsp_get_version *v;

		v = data->payload;

		core->svc_version = kmemdup(data->payload,
					    struct_size(v, svc_api_info,
							v->num_services),
					    GFP_ATOMIC);
		if (!core->svc_version)
			return -ENOMEM;

		core->get_version_supported = true;
		core->resp_received = true;

		break;
	}
	case AVCS_CMDRSP_ADSP_EVENT_GET_STATE:
		core->get_state_supported = true;
		core->avcs_state = result->opcode;

		core->resp_received = true;
		break;
	default:
		dev_err(&adev->dev, "Message id from adsp core svc: 0x%x\n",
			hdr->opcode);
		break;
	}

	if (core->resp_received)
		wake_up(&core->wait);

	return 0;
}

/* Send one AVCS command and wait for its answer: 0, -EIO (the DSP's status logged) or -ETIMEDOUT. core->lock held. */
static int q6core_avcs_cmd(struct q6core *core, struct apr_hdr *hdr)
{
	int rc;

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	core->cmd_opcode = hdr->opcode;
	core->cmd_done = false;
	core->cmd_status = 0;
	rc = apr_send_pkt(core->adev, (struct apr_pkt *)hdr);
	if (rc < 0)
		return rc;
	if (!wait_event_timeout(core->wait, core->cmd_done, msecs_to_jiffies(AVCS_TOPOLOGIES_TIMEOUT_MS)))
		return -ETIMEDOUT;
	if (core->cmd_status) {
		dev_err(&core->adev->dev, "DSP refused 0x%x: status 0x%x\n", hdr->opcode, core->cmd_status);
		return -EIO;
	}
	return 0;
}

/**
 * q6core_register_topologies() - give the DSP custom topologies (the vendor calibration's), for every service
 * @dsp_addr: the blob in memory the DSP reads (as the caller maps it for its own service: IOMMU stream in the upper bits)
 * @size: bytes of the blob
 *
 * Earlier custom topologies are dropped first (AVCS_CMD_DEREGISTER_TOPOLOGIES, all), as downstream does. The memory is
 * mapped to the core service for the command only; the caller frees it after.
 *
 * Return: 0, or a negative errno.
 */
static int q6core_register_topologies(u64 dsp_addr, u32 size)
{
	struct avcs_cmd_shared_mem_map_regions map = {};
	struct avcs_cmd_shared_mem_unmap_regions unmap = {};
	struct avcs_cmd_topologies cmd = {};
	struct q6core *core = g_core;
	int rc, rc2;

	if (!core || !size)
		return -ENODEV;
	mutex_lock(&core->lock);
	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = AVCS_CMD_DEREGISTER_TOPOLOGIES;
	cmd.mode = AVCS_MODE_DEREGISTER_ALL_CUSTOM_TOPOLOGIES;
	q6core_avcs_cmd(core, &cmd.hdr);	/* none registered yet is not an error */

	map.hdr.pkt_size = sizeof(map);
	map.hdr.opcode = AVCS_CMD_SHARED_MEM_MAP_REGIONS;
	map.mem_pool_id = AVCS_MEM_POOL_SHMEM8_4K;
	map.num_regions = 1;
	map.shm_addr_lsw = lower_32_bits(dsp_addr);
	map.shm_addr_msw = upper_32_bits(dsp_addr);
	map.mem_size_bytes = PAGE_ALIGN(size);
	rc = q6core_avcs_cmd(core, &map.hdr);
	if (rc)
		goto out;

	memset(&cmd, 0, sizeof(cmd));
	cmd.hdr.pkt_size = sizeof(cmd) - sizeof(cmd.mode);
	cmd.hdr.opcode = AVCS_CMD_REGISTER_TOPOLOGIES;
	cmd.addr_lsw = lower_32_bits(dsp_addr);
	cmd.addr_msw = upper_32_bits(dsp_addr);
	cmd.mem_map_handle = core->cmd_handle;
	cmd.size = size;
	rc = q6core_avcs_cmd(core, &cmd.hdr);

	unmap.hdr.pkt_size = sizeof(unmap);
	unmap.hdr.opcode = AVCS_CMD_SHARED_MEM_UNMAP_REGIONS;
	unmap.mem_map_handle = cmd.mem_map_handle;
	rc2 = q6core_avcs_cmd(core, &unmap.hdr);
	if (!rc)
		rc = rc2;
out:
	mutex_unlock(&core->lock);
	return rc;
}

/**
 * q6core_load_topology() - have the DSP load (or unload) the modules of one topology before a service opens it
 * @topology_id: the topology
 * @load: true to load, false to unload
 *
 * Return: 0, or a negative errno (-EIO with the DSP's status logged).
 */
int q6core_load_topology(u32 topology_id, bool load)
{
	struct {
		struct apr_hdr hdr;
		u32 topology_id;
	} __packed cmd = {};
	struct q6core *core = g_core;
	int rc;

	if (!core)
		return -ENODEV;
	mutex_lock(&core->lock);
	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = load ? AVCS_CMD_LOAD_TOPO_MODULES : AVCS_CMD_UNLOAD_TOPO_MODULES;
	cmd.topology_id = topology_id;
	rc = q6core_avcs_cmd(core, &cmd.hdr);
	mutex_unlock(&core->lock);
	return rc;
}
EXPORT_SYMBOL_GPL(q6core_load_topology);

static int q6core_get_fwk_versions(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_CMD_GET_FWK_VERSION;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return rc;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;

		if (!core->fwk_version_supported)
			return -ENOTSUPP;
		else
			return 0;
	}


	return rc;
}

static int q6core_get_svc_versions(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_GET_VERSIONS;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return rc;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;
		return 0;
	}

	return rc;
}

static bool __q6core_is_adsp_ready(struct q6core *core)
{
	struct apr_device *adev = core->adev;
	struct apr_pkt pkt;
	int rc;

	core->get_state_supported = false;

	pkt.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				      APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	pkt.hdr.pkt_size = APR_HDR_SIZE;
	pkt.hdr.opcode = AVCS_CMD_ADSP_EVENT_GET_STATE;

	rc = apr_send_pkt(adev, &pkt);
	if (rc < 0)
		return false;

	rc = wait_event_timeout(core->wait, (core->resp_received),
				msecs_to_jiffies(Q6_READY_TIMEOUT_MS));
	if (rc > 0 && core->resp_received) {
		core->resp_received = false;

		if (core->avcs_state)
			return true;
	}

	/* assume that the adsp is up if we not support this command */
	if (!core->get_state_supported)
		return true;

	return false;
}

/**
 * q6core_get_svc_api_info() - Get version number of a service.
 *
 * @svc_id: service id of the service.
 * @ainfo: Valid struct pointer to fill svc api information.
 *
 * Return: zero on success and error code on failure or unsupported
 */
int q6core_get_svc_api_info(int svc_id, struct q6core_svc_api_info *ainfo)
{
	int i;
	int ret = -ENOTSUPP;

	if (!g_core || !ainfo)
		return 0;

	mutex_lock(&g_core->lock);
	if (!g_core->is_version_requested) {
		if (q6core_get_fwk_versions(g_core) == -ENOTSUPP)
			q6core_get_svc_versions(g_core);
		g_core->is_version_requested = true;
	}

	if (g_core->fwk_version_supported) {
		for (i = 0; i < g_core->fwk_version->num_services; i++) {
			struct avcs_svc_api_info *info;

			info = &g_core->fwk_version->svc_api_info[i];
			if (svc_id != info->service_id)
				continue;

			ainfo->api_version = info->api_version;
			ainfo->api_branch_version = info->api_branch_version;
			ret = 0;
			break;
		}
	} else if (g_core->get_version_supported) {
		for (i = 0; i < g_core->svc_version->num_services; i++) {
			struct avcs_svc_info *info;

			info = &g_core->svc_version->svc_api_info[i];
			if (svc_id != info->service_id)
				continue;

			ainfo->api_version = info->version;
			ainfo->api_branch_version = 0;
			ret = 0;
			break;
		}
	}

	mutex_unlock(&g_core->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(q6core_get_svc_api_info);

/**
 * q6core_is_adsp_ready() - Get status of adsp
 *
 * Return: Will be an true if adsp is ready and false if not.
 */
bool q6core_is_adsp_ready(void)
{
	unsigned long  timeout;
	bool ret = false;

	if (!g_core)
		return false;

	mutex_lock(&g_core->lock);
	timeout = jiffies + msecs_to_jiffies(ADSP_STATE_READY_TIMEOUT_MS);
	for (;;) {
		if (__q6core_is_adsp_ready(g_core)) {
			ret = true;
			break;
		}

		if (!time_after(timeout, jiffies)) {
			ret = false;
			break;
		}
	}

	mutex_unlock(&g_core->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6core_is_adsp_ready);

static int acdb_section_offset(const struct firmware *fw,
			       u64 section, u32 *off_out)
{
	const u8 *data = fw->data + 32;
	u32 off = 32;
	u32 sect_size;
	u64 sect_id;

	while (off + 12 <= fw->size) {
		sect_id = *(const u64 *) data;
		sect_size = *(const u32 *) (data + 8);

		if (off + 12 + sect_size > fw->size)
			return -EINVAL;

		if (sect_id == section) {
			*off_out = off;
			return 0;
		}

		off += 12 + sect_size;
		data += 12 + sect_size;
	}

	return -ENOENT;
}

static int acdb_get_global_property(const struct firmware *fw,
				    u32 prop_id, u32 *offset)
{
	u32 gproplut_off, datapool_off;
	u32 lut_size, lut_nelts, i;
	const u32 *data;
	int ret;

	ret = acdb_section_offset(fw, ACDB_SECTION_GPROPLUT, &gproplut_off);
	if (ret)
		return ret;

	ret = acdb_section_offset(fw, ACDB_SECTION_DATAPOOL, &datapool_off);
	if (ret)
		return ret;

	lut_size = *(const u32 *) (fw->data + gproplut_off + 8);
	if (lut_size < 4)
		return -EINVAL;

	lut_nelts = *(const u32 *) (fw->data + gproplut_off + 12);
	if (lut_size < 4 + 8 * lut_nelts)
		return -EINVAL;

	data = (const u32 *) (fw->data + gproplut_off + 16);
	for (i = 0; i < lut_nelts; i++) {
		if (datapool_off + 12 + data[i * 2 + 1] + 4 > fw->size)
			return -EINVAL;

		if (data[i * 2] == prop_id) {
			*offset = datapool_off + 12 + data[i * 2 + 1];
			return 0;
		}
	}

	return -ENOENT;
}

/*
 * The board's AVCS custom topologies -- the topologies its vendor calibration
 * names for every service (ADM, voice, LSM) -- registered whenever the core
 * service comes up, so after every restart of the ADSP too: the DSP forgets
 * them with everything else. They live in the vendor's Global_cal.acdb
 * (global property 0x131a7), named by this node's firmware-name.
 *
 * The blob is larger than an APR packet takes in band, so it is lent to the
 * DSP: this node exists for the IOMMU stream it reads the memory through,
 * which the APR bus does not give its services. Registration replaces
 * whatever was registered before; it is the only one that does.
 */
struct q6core_topologies {
	struct device *dev;
	struct work_struct work;
	u64 sid;
};

static void q6core_topologies_work(struct work_struct *work)
{
	struct q6core_topologies *t = container_of(work, struct q6core_topologies, work);
	struct device *dev = t->dev;
	const struct firmware *fw;
	const char *fw_name;
	u32 off, size;
	dma_addr_t dma;
	void *cpu;
	int ret;

	if (of_property_read_string(dev->of_node, "firmware-name", &fw_name))
		return;

	if (!q6core_is_adsp_ready()) {
		dev_err(dev, "ADSP not ready: custom topologies not registered\n");
		return;
	}

	ret = request_firmware(&fw, fw_name, dev);
	if (ret) {
		dev_err(dev, "%s: %d\n", fw_name, ret);
		return;
	}

	ret = acdb_get_global_property(fw, ACDB_TOPOLOGIES_BLOB, &off);
	if (ret) {
		dev_err(dev, "%s: no custom topologies: %d\n", fw_name, ret);
		goto out;
	}
	size = get_unaligned_le32(fw->data + off);
	if (!size || size > fw->size - off - 4) {
		dev_err(dev, "%s: custom topologies of %u bytes do not fit\n", fw_name, size);
		goto out;
	}

	cpu = dma_alloc_coherent(dev, PAGE_ALIGN(size), &dma, GFP_KERNEL);
	if (!cpu)
		goto out;
	memcpy(cpu, fw->data + off + 4, size);
	ret = q6core_register_topologies(dma | (t->sid << 32), size);
	dma_free_coherent(dev, PAGE_ALIGN(size), cpu, dma);
	if (ret)
		dev_err(dev, "%s: custom topologies refused: %d\n", fw_name, ret);
	else
		dev_info(dev, "registered %u bytes of custom topologies from %s\n", size, fw_name);
out:
	release_firmware(fw);
}

static int q6core_topologies_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q6core_topologies *t;
	struct of_phandle_args args;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

	if (!of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", 0, &args)) {
		if (args.args_count)
			t->sid = args.args[0] & 0xf;
		of_node_put(args.np);
	}

	t->dev = dev;
	INIT_WORK(&t->work, q6core_topologies_work);
	platform_set_drvdata(pdev, t);
	/* the core service answers only once its probe is done: not from here */
	schedule_work(&t->work);
	return 0;
}

static void q6core_topologies_remove(struct platform_device *pdev)
{
	struct q6core_topologies *t = platform_get_drvdata(pdev);

	cancel_work_sync(&t->work);
}

static const struct of_device_id q6core_topologies_device_id[] = {
	{ .compatible = "qcom,q6core-topologies" },
	{},
};
MODULE_DEVICE_TABLE(of, q6core_topologies_device_id);

static struct platform_driver q6core_topologies_driver = {
	.driver = {
		.name = "qcom-q6core-topologies",
		.of_match_table = q6core_topologies_device_id,
	},
	.probe = q6core_topologies_probe,
	.remove = q6core_topologies_remove,
};

static int q6core_probe(struct apr_device *adev)
{
	g_core = kzalloc_obj(*g_core);
	if (!g_core)
		return -ENOMEM;

	dev_set_drvdata(&adev->dev, g_core);

	mutex_init(&g_core->lock);
	g_core->adev = adev;
	init_waitqueue_head(&g_core->wait);

	/* the topologies node: created with the service, so again after a restart */
	return of_platform_populate(adev->dev.of_node, NULL, NULL, &adev->dev);
}

static void q6core_exit(struct apr_device *adev)
{
	struct q6core *core = dev_get_drvdata(&adev->dev);

	of_platform_depopulate(&adev->dev);

	if (core->fwk_version_supported)
		kfree(core->fwk_version);
	if (core->get_version_supported)
		kfree(core->svc_version);

	g_core = NULL;
	kfree(core);
}

#ifdef CONFIG_OF
static const struct of_device_id q6core_device_id[]  = {
	{ .compatible = "qcom,q6core" },
	{},
};
MODULE_DEVICE_TABLE(of, q6core_device_id);
#endif

static struct apr_driver qcom_q6core_driver = {
	.probe = q6core_probe,
	.remove = q6core_exit,
	.callback = q6core_callback,
	.driver = {
		.name = "qcom-q6core",
		.of_match_table = of_match_ptr(q6core_device_id),
	},
};

static int __init q6core_init(void)
{
	int ret;

	ret = platform_driver_register(&q6core_topologies_driver);
	if (ret)
		return ret;

	ret = apr_driver_register(&qcom_q6core_driver);
	if (ret)
		platform_driver_unregister(&q6core_topologies_driver);
	return ret;
}
module_init(q6core_init);

static void __exit q6core_exit_module(void)
{
	apr_driver_unregister(&qcom_q6core_driver);
	platform_driver_unregister(&q6core_topologies_driver);
}
module_exit(q6core_exit_module);
MODULE_DESCRIPTION("q6 core");
MODULE_LICENSE("GPL v2");
