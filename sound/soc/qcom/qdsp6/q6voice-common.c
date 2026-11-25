// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Stephan Gerhold

#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/qcom/apr.h>
#include "q6voice-common.h"

#define APRV2_IBASIC_CMD_DESTROY_SESSION	0x0001003C

/*
 * Long enough for this SoC. Qualcomm's own tree waits 300 ms everywhere except
 * on sm8250, where the vendor raises it to a second because the ADSP here is
 * slow to answer; a command that is merely late otherwise comes back as a
 * timeout and is indistinguishable from one the ADSP refused.
 */
#define TIMEOUT_MS	1000

struct q6voice_service {
	struct apr_device *adev;
	enum q6voice_service_type type;

	/*
	 * Sessions outlive the APR device when the ADSP restarts in the middle
	 * of a call: the device goes away, the session is only released when
	 * the call ends. Each session therefore holds a reference to the
	 * service (and the service one to the device), and @removed, set by
	 * q6voice_common_remove(), turns every later send into -ENODEV instead
	 * of a packet to a channel that no longer exists.
	 */
	struct kref refcount;
	bool removed;

	/* Protect sessions array, @removed and sending to @adev */
	spinlock_t lock;
	struct q6voice_session *sessions[Q6VOICE_PATH_COUNT];

	/*
	 * Slot for a command addressed to the service itself rather than to a
	 * session. Only one may be outstanding, which is enough: these are
	 * one-off configuration commands issued outside the call path.
	 */
	struct mutex svc_lock;
	wait_queue_head_t svc_wait;

	/*
	 * The fields below are shared with the APR callback and protected by
	 * @svc_rsp_lock. @svc_rsp points into the sender's stack, so the
	 * callback may only write to it while the sender is still waiting,
	 * and @svc_token, sent with each command and echoed by the ADSP, drops
	 * the late reply to a command that has already timed out.
	 */
	spinlock_t svc_rsp_lock;
	u32 svc_token;
	u32 svc_expected_opcode;
	u32 svc_result;

	/* for replies that carry a payload instead of a basic result */
	u32 svc_rsp_opcode;
	void *svc_rsp;
	u32 svc_rsp_size;
};

static void q6voice_service_free(struct kref *ref)
{
	struct q6voice_service *svc = container_of(ref, struct q6voice_service,
						   refcount);

	put_device(&svc->adev->dev);
	kfree(svc);
}

static void q6voice_service_put(struct q6voice_service *svc)
{
	kref_put(&svc->refcount, q6voice_service_free);
}

/* Send to the service unless its APR device is gone. */
static int q6voice_service_send_pkt(struct q6voice_service *svc,
				    struct apr_hdr *hdr)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->removed)
		ret = -ENODEV;
	else
		ret = apr_send_pkt(svc->adev, (struct apr_pkt *)hdr);
	spin_unlock_irqrestore(&svc->lock, flags);

	return ret;
}

/* Protect q6voice_services */
static DEFINE_SPINLOCK(q6voice_services_lock);
static struct q6voice_service *q6voice_services[Q6VOICE_SERVICE_COUNT] = {0};

/*
 * The ADSP can restart under us -- it has its own watchdog, and userspace can
 * ask for it. Its APR services disappear and come back when that happens, and
 * anything the ADSP was told before is gone with them: it comes back knowing
 * nothing about, say, where the voice mailbox lives. Drivers that configured
 * something once at probe need to hear about it so they can say it again.
 */
static void (*q6voice_svc_notifier)(enum q6voice_service_type type);
/* the notifier runs under it, so clearing it waits for a running one */
static DEFINE_MUTEX(q6voice_svc_notifier_lock);

void q6voice_common_set_svc_notifier(void (*notify)(enum q6voice_service_type))
{
	mutex_lock(&q6voice_svc_notifier_lock);
	q6voice_svc_notifier = notify;
	mutex_unlock(&q6voice_svc_notifier_lock);
}
EXPORT_SYMBOL_GPL(q6voice_common_set_svc_notifier);

/*
 * Addresses handed to the DSP are not bare addresses. The DSP maps them
 * through the SMMU itself, so it has to be told which stream to translate
 * against, and that is carried in the top half of the address: the low bits of
 * this device's stream id, shifted up out of the way of the address proper.
 *
 * Sending a bare address means the DSP translates against stream zero, maps
 * whatever that lands on, and reads it. On this SoC that resets the board.
 *
 * The mask is the vendor's, and is the same across every SoC in that tree; it
 * is the part of the stream id that selects the context bank.
 */
#define Q6VOICE_SMMU_SID_MASK		0xf
#define Q6VOICE_SMMU_SID_SHIFT		32

u64 q6voice_dsp_address(struct device *dev, dma_addr_t addr)
{
	struct of_phandle_args args;
	u64 sid = 0;

	if (!of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells",
					0, &args)) {
		if (args.args_count)
			sid = args.args[0] & Q6VOICE_SMMU_SID_MASK;
		of_node_put(args.np);
	}

	return addr | (sid << Q6VOICE_SMMU_SID_SHIFT);
}
EXPORT_SYMBOL_GPL(q6voice_dsp_address);

int q6voice_common_probe(struct apr_device *adev, enum q6voice_service_type type)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *svc, *current_svc;
	unsigned long flags;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return -EINVAL;

	svc = kzalloc(sizeof(*svc), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;

	get_device(dev);
	svc->adev = adev;
	svc->type = type;
	kref_init(&svc->refcount);
	spin_lock_init(&svc->lock);
	mutex_init(&svc->svc_lock);
	init_waitqueue_head(&svc->svc_wait);
	spin_lock_init(&svc->svc_rsp_lock);

	/* before it can be found: a reply may come back right away */
	dev_set_drvdata(dev, svc);

	spin_lock_irqsave(&q6voice_services_lock, flags);
	current_svc = q6voice_services[type];
	if (!current_svc)
		q6voice_services[type] = svc;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	if (current_svc) {
		dev_set_drvdata(dev, NULL);
		q6voice_service_put(svc);
		return -EEXIST;
	}

	/* Announce it only once it can actually be sent to. */
	mutex_lock(&q6voice_svc_notifier_lock);
	if (q6voice_svc_notifier)
		q6voice_svc_notifier(type);
	mutex_unlock(&q6voice_svc_notifier_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_probe);

void q6voice_common_remove(struct apr_device *adev)
{
	struct q6voice_service *svc = dev_get_drvdata(&adev->dev);
	enum q6voice_service_type type;
	unsigned long flags;
	int i;

	if (!svc)
		return;

	type = svc->type;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	if (q6voice_services[type] == svc)
		q6voice_services[type] = NULL;
	spin_unlock_irqrestore(&q6voice_services_lock, flags);

	/*
	 * The sessions stay with their owners until the call ends; from here
	 * on they can only fail. Wake whoever is waiting for a reply that will
	 * not come.
	 */
	spin_lock_irqsave(&svc->lock, flags);
	svc->removed = true;
	for (i = 0; i < Q6VOICE_PATH_COUNT; i++)
		if (svc->sessions[i])
			wake_up(&svc->sessions[i]->wait);
	spin_unlock_irqrestore(&svc->lock, flags);
	wake_up(&svc->svc_wait);

	dev_set_drvdata(&adev->dev, NULL);
	q6voice_service_put(svc);
}
EXPORT_SYMBOL_GPL(q6voice_common_remove);

static void q6voice_session_free(struct kref *ref)
{
	struct q6voice_session *s = container_of(ref, struct q6voice_session,
						 refcount);

	q6voice_service_put(s->svc);
	kfree(s);
}

static int q6voice_session_destroy(struct q6voice_session *s)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = APRV2_IBASIC_CMD_DESTROY_SESSION;

	return q6voice_common_send(s, &cmd.hdr);
}

void q6voice_session_release(struct q6voice_session *s)
{
	struct q6voice_service *svc = s->svc;
	unsigned long flags;

	if (s->handle)
		q6voice_session_destroy(s);

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->sessions[s->port] == s)
		svc->sessions[s->port] = NULL;
	spin_unlock_irqrestore(&svc->lock, flags);

	kref_put(&s->refcount, q6voice_session_free);
}
EXPORT_SYMBOL_GPL(q6voice_session_release);

struct q6voice_session *
q6voice_session_create(enum q6voice_service_type type,
		       enum q6voice_path_type path, struct apr_hdr *hdr)
{
	struct q6voice_service *svc;
	struct q6voice_session *s;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	svc = q6voice_services[type];
	if (svc)
		kref_get(&svc->refcount);
	spin_unlock_irqrestore(&q6voice_services_lock, flags);
	if (!svc)
		return ERR_PTR(-ENODEV);

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		q6voice_service_put(svc);
		return ERR_PTR(-ENOMEM);
	}

	s->dev = &svc->adev->dev;
	s->svc = svc;
	s->port = path;

	kref_init(&s->refcount);
	spin_lock_init(&s->lock);
	init_waitqueue_head(&s->wait);

	spin_lock_irqsave(&svc->lock, flags);
	if (svc->sessions[path] || svc->removed) {
		ret = svc->removed ? -ENODEV : -EBUSY;
		spin_unlock_irqrestore(&svc->lock, flags);
		kref_put(&s->refcount, q6voice_session_free);
		return ERR_PTR(ret);
	}
	svc->sessions[path] = s;
	spin_unlock_irqrestore(&svc->lock, flags);

	dev_dbg(s->dev, "create session\n");

	ret = q6voice_common_send(s, hdr);
	if (ret)
		goto err;

	if (!s->handle) {
		dev_warn(s->dev, "failed to receive handle\n");
		ret = -EIO;
		goto err;
	}

	dev_dbg(s->dev, "handle: %d\n", s->handle);

	return s;

err:
	q6voice_session_release(s);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6voice_session_create);

static void q6voice_session_callback(struct q6voice_session *s,
				     const struct apr_resp_pkt *data)
{
	struct aprv2_ibasic_rsp_result_t *result = data->payload;
	unsigned long flags;

	if (data->hdr.opcode != APR_BASIC_RSP_RESULT)
		return; /* Not handled here */

	if (data->payload_size < (int)sizeof(*result)) {
		dev_warn(s->dev, "short basic result (%d bytes)\n",
			 data->payload_size);
		return;
	}

	dev_dbg(s->dev, "basic result: opcode %#x, status: %#x\n",
		result->opcode, result->status);

	spin_lock_irqsave(&s->lock, flags);
	if (result->opcode != s->expected_opcode) {
		spin_unlock_irqrestore(&s->lock, flags);
		dev_warn(s->dev, "unexpected reply for opcode %#x (status: %#x)\n",
			 result->opcode, result->status);
		return;
	}

	if (!s->handle) {
		s->handle = data->hdr.src_port;
	} else if (s->handle != data->hdr.src_port) {
		spin_unlock_irqrestore(&s->lock, flags);
		dev_warn(s->dev, "unexpected reply for session %#x (!= %#x)\n",
			 data->hdr.src_port, s->handle);
		return;
	}

	s->result = result->status;
	s->expected_opcode = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	wake_up(&s->wait);
}

/*
 * Send a command whose reply comes back to the service rather than to a
 * session. @dest_port selects what inside the ADSP is being addressed: zero
 * for the service itself, as the mailbox configuration uses, or a session
 * handle for commands that belong to an existing session even though their
 * reply carries its own payload. @size is the full packet size including the
 * header.
 */
static int q6voice_send_svc(enum q6voice_service_type type, struct apr_hdr *hdr,
			    u32 size, u16 dest_port, u32 rsp_opcode, void *rsp,
			    u32 rsp_size)
{
	struct q6voice_service *svc;
	unsigned long flags;
	int ret;

	if (type >= Q6VOICE_SERVICE_COUNT)
		return -EINVAL;

	spin_lock_irqsave(&q6voice_services_lock, flags);
	svc = q6voice_services[type];
	if (svc)
		kref_get(&svc->refcount);
	spin_unlock_irqrestore(&q6voice_services_lock, flags);
	if (!svc)
		return -ENODEV;

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				       APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->pkt_size = size;
	hdr->src_port = Q6VOICE_SVC_PORT;
	hdr->dest_port = dest_port;

	mutex_lock(&svc->svc_lock);

	spin_lock_irqsave(&svc->svc_rsp_lock, flags);
	/* never zero, so a reply without a token is never taken for ours */
	if (!++svc->svc_token)
		++svc->svc_token;
	hdr->token = svc->svc_token;
	svc->svc_expected_opcode = hdr->opcode;
	svc->svc_result = 0;
	svc->svc_rsp_opcode = rsp_opcode;
	svc->svc_rsp = rsp;
	svc->svc_rsp_size = rsp_size;
	spin_unlock_irqrestore(&svc->svc_rsp_lock, flags);

	ret = q6voice_service_send_pkt(svc, hdr);
	if (ret < 0)
		goto out;

	wait_event_timeout(svc->svc_wait,
			   READ_ONCE(svc->svc_expected_opcode) == 0 ||
			   READ_ONCE(svc->removed),
			   msecs_to_jiffies(TIMEOUT_MS));

	/*
	 * Close the slot before looking at the outcome: once @svc_rsp is
	 * cleared under the lock the callback can no longer write into @rsp,
	 * which lives on the caller's stack.
	 */
	spin_lock_irqsave(&svc->svc_rsp_lock, flags);
	if (svc->svc_expected_opcode)
		ret = svc->removed ? -ENODEV : -ETIMEDOUT;
	else if (svc->svc_result > 0)
		ret = -EIO;
	else
		ret = 0;
	svc->svc_expected_opcode = 0;
	svc->svc_rsp_opcode = 0;
	svc->svc_rsp = NULL;
	spin_unlock_irqrestore(&svc->svc_rsp_lock, flags);

	if (ret == -EIO)
		dev_err(&svc->adev->dev, "command %#x failed with error %d\n",
			hdr->opcode, svc->svc_result);
out:
	mutex_unlock(&svc->svc_lock);
	q6voice_service_put(svc);
	return ret;
}

int q6voice_common_send_svc(enum q6voice_service_type type, struct apr_hdr *hdr,
			    u32 size)
{
	return q6voice_send_svc(type, hdr, size, 0, 0, NULL, 0);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc);

int q6voice_common_send_svc_rsp(enum q6voice_service_type type,
				struct apr_hdr *hdr, u32 size, u32 rsp_opcode,
				void *rsp, u32 rsp_size)
{
	return q6voice_send_svc(type, hdr, size, 0, rsp_opcode, rsp, rsp_size);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc_rsp);

/*
 * As above, but addressed to a session. Lending the ADSP memory is such a
 * command: it belongs to the MVM session, yet answers with a handle of its own
 * rather than a plain result. Addressing it to the service instead leaves the
 * ADSP with a command for nothing in particular, and it stops answering
 * altogether.
 */
int q6voice_common_send_svc_rsp_port(enum q6voice_service_type type,
				     struct apr_hdr *hdr, u32 size,
				     u16 dest_port, u32 rsp_opcode, void *rsp,
				     u32 rsp_size)
{
	return q6voice_send_svc(type, hdr, size, dest_port, rsp_opcode, rsp,
				rsp_size);
}
EXPORT_SYMBOL_GPL(q6voice_common_send_svc_rsp_port);

int q6voice_common_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	struct device *dev = &adev->dev;
	struct q6voice_service *v = dev_get_drvdata(dev);
	struct q6voice_session *s;
	unsigned long flags;

	if (!v)
		return 0;

	dev_dbg(dev, "callback: %#x\n", data->hdr.opcode);

	/* Reply to a service-level command rather than to a session */
	if (data->hdr.dest_port == Q6VOICE_SVC_PORT) {
		struct aprv2_ibasic_rsp_result_t *result = data->payload;
		u32 payload_size = data->payload_size > 0 ? data->payload_size : 0;
		bool done = false;

		spin_lock_irqsave(&v->svc_rsp_lock, flags);
		if (!v->svc_expected_opcode || data->hdr.token != v->svc_token) {
			/* nobody waiting, or the reply to a command given up on */
			spin_unlock_irqrestore(&v->svc_rsp_lock, flags);
			dev_dbg(dev, "stale svc reply %#x (token %u)\n",
				data->hdr.opcode, data->hdr.token);
			return 0;
		}

		if (v->svc_rsp_opcode && data->hdr.opcode == v->svc_rsp_opcode) {
			/* a reply carrying its own payload, e.g. a version string */
			if (payload_size < v->svc_rsp_size) {
				dev_err(dev, "short reply %#x: %u < %u bytes\n",
					data->hdr.opcode, payload_size,
					v->svc_rsp_size);
				v->svc_result = 1;
			} else {
				if (v->svc_rsp)
					memcpy(v->svc_rsp, data->payload,
					       v->svc_rsp_size);
				v->svc_result = 0;
			}
			done = true;
		} else if (data->hdr.opcode == APR_BASIC_RSP_RESULT &&
			   payload_size >= sizeof(*result)) {
			dev_dbg(dev, "svc result: opcode %#x, status: %#x\n",
				result->opcode, result->status);
			if (result->opcode == v->svc_expected_opcode) {
				v->svc_result = result->status;
				done = true;
			} else {
				dev_warn(dev, "unexpected svc reply for opcode %#x\n",
					 result->opcode);
			}
		}

		if (done)
			v->svc_expected_opcode = 0;
		spin_unlock_irqrestore(&v->svc_rsp_lock, flags);

		if (done)
			wake_up(&v->svc_wait);
		return 0;
	}

	if (data->hdr.dest_port >= Q6VOICE_PATH_COUNT) {
		dev_warn(dev, "callback() called for unhandled/invalid path: %d\n",
			 data->hdr.dest_port);
		return 0;
	}

	spin_lock_irqsave(&v->lock, flags);
	s = v->sessions[data->hdr.dest_port];
	if (s)
		kref_get(&s->refcount);
	spin_unlock_irqrestore(&v->lock, flags);

	if (s) {
		q6voice_session_callback(s, data);
		kref_put(&s->refcount, q6voice_session_free);
	} else {
		dev_warn(dev, "callback() called for inactive path: %d\n",
			 data->hdr.dest_port);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_callback);

/*
 * Every command to the DSP passes through here, so this is the one place a
 * whole call's conversation can be seen in order. Off by default: it is a line
 * per command at a point where a call is being set up, and only useful when
 * comparing the sequence against a working one.
 */
static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "Log every command sent to the DSP and its result.");

static const char *q6voice_service_name(enum q6voice_service_type type)
{
	switch (type) {
	case Q6VOICE_SERVICE_MVM:
		return "MVM";
	case Q6VOICE_SERVICE_CVP:
		return "CVP";
	case Q6VOICE_SERVICE_CVS:
		return "CVS";
	default:
		return "???";
	}
}

static void q6voice_trace(struct q6voice_session *s, struct apr_hdr *hdr,
			  int ret)
{
	if (!trace)
		return;

	dev_info(s->dev,
		 "%s opcode %#010x src %#06x dst %#06x size %u -> %s (%u)\n",
		 q6voice_service_name(s->svc->type), hdr->opcode, hdr->src_port,
		 hdr->dest_port, hdr->pkt_size,
		 ret == -ETIMEDOUT ? "timeout" : ret ? "failed" : "ok",
		 s->result);
}

int q6voice_common_send(struct q6voice_session *s, struct apr_hdr *hdr)
{
	unsigned long flags;
	int ret;

	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
				       APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->src_port = s->port;
	hdr->dest_port = s->handle;
	hdr->token = 0;

	spin_lock_irqsave(&s->lock, flags);
	s->expected_opcode = hdr->opcode;
	s->result = 0;
	spin_unlock_irqrestore(&s->lock, flags);

	ret = q6voice_service_send_pkt(s->svc, hdr);
	if (ret < 0)
		return ret;

	ret = wait_event_timeout(s->wait, READ_ONCE(s->expected_opcode) == 0 ||
				 READ_ONCE(s->svc->removed),
				 msecs_to_jiffies(TIMEOUT_MS));

	spin_lock_irqsave(&s->lock, flags);
	if (s->expected_opcode) {
		s->expected_opcode = 0;
		ret = READ_ONCE(s->svc->removed) ? -ENODEV : -ETIMEDOUT;
	} else {
		ret = 0;
	}
	spin_unlock_irqrestore(&s->lock, flags);

	if (ret) {
		q6voice_trace(s, hdr, ret);
		return ret;
	}

	q6voice_trace(s, hdr, s->result ? -EIO : 0);

	if (s->result > 0) {
		dev_err(s->dev, "command %#x failed with error %d\n",
			hdr->opcode, s->result);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6voice_common_send);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice common session management");
MODULE_LICENSE("GPL v2");
