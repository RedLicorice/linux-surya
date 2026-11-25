// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "q6afe.h"
#include "q6core.h"
#include "q6cvp.h"
#include "q6cvs.h"
#include "q6mvm.h"
#include "q6voice-cal.h"
#include "q6voice-common.h"

struct q6voice_path_runtime {
	struct q6voice_session *sessions[Q6VOICE_SERVICE_COUNT];
	unsigned int started;

	/*
	 * The module parameters as they were when this call started. They are
	 * writable at runtime, and a call set up one way has to be taken down
	 * the same way.
	 */
	int setup_level;
	int cal_level;
	bool attach_stream;
	/* the path's call data (q6voice_param), likewise */
	struct q6cvp_config cvp_cfg;
	u32 tx_acdb, rx_acdb;

	/*
	 * Calibration lent through this call's MVM session. The handles
	 * belong to that session and die with it, so they are returned and
	 * freed with the session at the end of the call, and the next call
	 * lends its own.
	 */
	struct q6voice_cal *stream_cal;	/* the stream's own */
	struct q6voice_cal *dev_cfg;	/* what the endpoints are */
	struct q6voice_cal *cal;	/* the vocproc, other than by step */
	struct q6voice_cal *vol_cal;	/* the vocproc, per volume step */
};

struct q6voice_path {
	struct q6voice *v;

	enum q6voice_path_type type;
	int tx_port, rx_port;
	/* the board's data for the next call (q6voice_param), set by userspace */
	u32 params[Q6VOICE_PARAM_COUNT];
	/* Serialize access to voice path session */
	struct mutex lock;
	struct q6voice_path_runtime *runtime;
};

struct q6voice {
	struct device *dev;
	/* cached so the version is queried once, not per call */
	char cvd_version[32];
	/* Downlink volume step, applied at call start and whenever it changes */
	unsigned int rx_volume_step;
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];
};

/*
 * Calibration is per board, and the ADSP has to be lent it before it will
 * resolve a volume step. Loaded at the start of every call rather than at
 * probe: the memory is lent through the call's MVM session, which only exists
 * for the length of the call.
 */
/*
 * The three tables, in the order the vendor registers them: what the endpoints
 * are, then everything about the vocproc that does not vary with the volume
 * step, then what does. A volume table is an overlay on a vocproc that has
 * already been told the first two, which is why the order is not incidental.
 */
/* The files are the board's: the calibration node's firmware-name list (q6voice-cal). */

/*
 * Downlink volume for a call. Android's HAL maps the user-facing volume onto
 * steps 0..5 and pushes the result down a mixer control; with no such control
 * here, ask for the top of that range so calls are audible, and leave making
 * it adjustable to whoever adds the control.
 */
#define Q6VOICE_RX_VOLUME_STEP	5

/* Vendor defaults: a short ramp for volume, a long one for mute. */
#define Q6VOICE_VOLUME_RAMP_MS	20
#define Q6VOICE_MUTE_RAMP_MS	500

/*
 * Which of the two create commands the vocproc expects. They carry the same
 * payload and differ only in opcode, but the choice is not cosmetic: a CVD
 * from 2.2 on refuses VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT for a session that was
 * created with the older V2 command.
 */
static bool q6voice_cvp_create_v3(const char *cvd_version)
{
	unsigned int major, minor;

	if (sscanf(cvd_version, "%u.%u", &major, &minor) != 2)
		return false;

	return major > 2 || (major == 2 && minor >= 2);
}

/*
 * Registered by the bridge driver, which can go away (unbind, module unload)
 * while a call is being set up. The callbacks run under this mutex, and the
 * bridge unregisters before tearing anything down, so once
 * q6voice_set_modem_link(NULL) returns nothing is calling into it any more.
 */
static DEFINE_MUTEX(q6voice_modem_link_lock);
static const struct q6voice_modem_link *q6voice_modem_link;

void q6voice_set_modem_link(const struct q6voice_modem_link *link)
{
	mutex_lock(&q6voice_modem_link_lock);
	q6voice_modem_link = link;
	mutex_unlock(&q6voice_modem_link_lock);
}
EXPORT_SYMBOL_GPL(q6voice_set_modem_link);

static int q6voice_modem_link_start(void)
{
	int ret = 0;

	mutex_lock(&q6voice_modem_link_lock);
	if (q6voice_modem_link)
		ret = q6voice_modem_link->start();
	mutex_unlock(&q6voice_modem_link_lock);

	return ret;
}

static void q6voice_modem_link_end(void)
{
	mutex_lock(&q6voice_modem_link_lock);
	if (q6voice_modem_link)
		q6voice_modem_link->end();
	mutex_unlock(&q6voice_modem_link_lock);
}

/*
 * How much of the vocproc to configure before starting a call.
 *
 * The msm8916 driver this one grew from creates a vocproc, enables it, attaches
 * it and starts -- five commands, and calls are audible. Everything beyond that
 * here was added for a modem on another chip, at a time when the session name
 * and the create opcode were both still wrong, and has never been re-tested
 * since those were fixed. A configuration command the ADSP accepts but does not
 * want is indistinguishable from one it needs, so make it possible to take them
 * back out and hear the difference.
 *
 *   0  everything (default)
 *   1  no vocproc configuration: no channel info, media format, topology or
 *      volume, but still a stream session
 *   2  as msm8916 does it: no stream session either -- this ADSP refuses to
 *      enable a vocproc without one (VSS_IVOCPROC_CMD_ENABLE returns an
 *      error), so it is a comparison point rather than a usable setting
 *
 * Writable at runtime, so the three can be compared within one boot rather than
 * one reboot each.
 */
/*
 * How far to take the calibration handoff. Lending the memory and registering
 * it are separate conversations with the DSP and either can be the one that
 * kills it, so they can be reached one at a time: whichever level first takes
 * the board down is the one at fault. Off by default because a board that
 * resets on every call is worse than one that is merely quiet.
 */
enum {
	Q6VOICE_CAL_NONE = 0,	/* do not touch calibration at all */
	Q6VOICE_CAL_LEND,	/* load it and lend the DSP the memory */
	Q6VOICE_CAL_REGISTER,	/* also point the vocproc at it */
};

/*
 * Registering calibration is not optional on this board: without it the DSP
 * refuses the volume step (0x112c2) and the call is silent. It defaulted to
 * off while the table format was still being worked out.
 */
static int cal_level = Q6VOICE_CAL_REGISTER;
module_param(cal_level, int, 0644);
MODULE_PARM_DESC(cal_level,
		 "0 to leave calibration alone, 1 to lend the DSP the memory, 2 to also register it.");

static int setup_level;
module_param(setup_level, int, 0644);
MODULE_PARM_DESC(setup_level,
		 "0 = full setup, 1 = skip vocproc config, 2 = msm8916 minimal");

/*
 * Whether we attach the stream to the MVM ourselves.
 *
 * The ADSP merges control sessions by name, so the passive session we create
 * and the full-control one the modem creates for the same VSID are one object
 * reached through two handles -- our CVS and the modem's come back with the
 * same handle, 0x0100. Only one attach can succeed. Ours always gets there
 * first, because we build the session when the PCM opens while the modem only
 * starts once the ADSP has sent it APPS_START, which our create is what
 * triggers. The modem's attach then fails ADSP_EALREADY (status 9, visible in
 * its APR log) and it answers that by destroying the session and starting over,
 * indefinitely.
 *
 * The modem owns the call, so the attach belongs to it. Off by default; set to
 * 1 to restore the old behaviour.
 */
static bool attach_stream;
module_param(attach_stream, bool, 0644);
MODULE_PARM_DESC(attach_stream,
		 "Attach the stream to the MVM from here instead of leaving it to the modem.");

/* Return this call's calibration to the ADSP, while its MVM session lives. */
static void q6voice_path_free_cal(struct q6voice_path *p)
{
	struct q6voice_path_runtime *rt = p->runtime;
	struct q6voice_session *mvm = rt->sessions[Q6VOICE_SERVICE_MVM];

	q6voice_cal_free(rt->stream_cal, mvm);
	q6voice_cal_free(rt->dev_cfg, mvm);
	q6voice_cal_free(rt->cal, mvm);
	q6voice_cal_free(rt->vol_cal, mvm);
	rt->stream_cal = NULL;
	rt->dev_cfg = NULL;
	rt->cal = NULL;
	rt->vol_cal = NULL;
}

static int q6voice_path_start(struct q6voice_path *p)
{
	struct q6voice_path_runtime *rt = p->runtime;
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm, *cvp, *cvs = NULL;
	int ret;
	u32 tx_topology, rx_topology;

	rt->setup_level = READ_ONCE(setup_level);
	rt->cal_level = READ_ONCE(cal_level);
	rt->attach_stream = READ_ONCE(attach_stream);
	/* p->lock is held: the controls cannot change these halfway */
	rt->cvp_cfg.tx_topology = p->params[Q6VOICE_TX_TOPOLOGY];
	rt->cvp_cfg.rx_topology = p->params[Q6VOICE_RX_TOPOLOGY];
	rt->cvp_cfg.tx_channels = p->params[Q6VOICE_TX_CHANNELS];
	rt->tx_acdb = p->params[Q6VOICE_TX_ACDB_DEVICE];
	rt->rx_acdb = p->params[Q6VOICE_RX_ACDB_DEVICE];
	dev_info(dev, "call data: tx topology %#010x rx topology %#010x tx channels %u acdb devices %#x %#x\n",
		 rt->cvp_cfg.tx_topology, rt->cvp_cfg.rx_topology,
		 rt->cvp_cfg.tx_channels, rt->tx_acdb, rt->rx_acdb);

	dev_info(dev, "start path %d, setup level %d\n", p->type,
		 rt->setup_level);

	/*
	 * Where the modem is a separate chip it DMAs the call audio into system
	 * memory for as long as the call lasts, so the link to it must be held
	 * awake throughout -- an idle link suspends after a couple of seconds
	 * and the audio simply stops arriving. Vote first and release on the way
	 * out, as the vendor does.
	 */
	ret = q6voice_modem_link_start();
	if (ret) {
		dev_err(dev, "failed to wake the modem link: %d\n", ret);
		return ret;
	}

	mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm)) {
			ret = PTR_ERR(mvm);
			goto link_err;
		}
		p->runtime->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	/*
	 * The stream session carries the voice packets themselves. msm8916 does
	 * without one -- its modem is on the same die -- so it is the first
	 * thing to drop when reproducing that driver exactly.
	 */
	if (rt->setup_level < 2) {
		cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
		if (!cvs) {
			cvs = q6cvs_session_create(p->type);
			if (IS_ERR(cvs)) {
				ret = PTR_ERR(cvs);
				goto link_err;
			}
			p->runtime->sessions[Q6VOICE_SERVICE_CVS] = cvs;
		}

		/*
		 * Let the modem attach the stream, not us.
		 *
		 * The ADSP merges sessions by name, so the passive session we
		 * create and the full-control one the modem creates for the
		 * same VSID are one object with two handles -- our CVS and the
		 * modem's come back with the same handle. Only one attach can
		 * win. We always get there first, because our session is built
		 * when the PCM opens and the modem only starts once the ADSP
		 * has told it APPS_START, which needs our create to have
		 * happened. The modem's attach then fails with ADSP_EALREADY
		 * (status 9, seen in its APR log) and it responds by destroying
		 * the session and starting over, forever.
		 *
		 * The modem owns the call, so the attach is its to make. Ours
		 * is redundant on a merged session and is what starves it.
		 */
		if (rt->attach_stream) {
			ret = q6mvm_attach_stream(mvm, cvs, true);
			if (ret) {
				dev_err(dev, "failed to attach stream to mvm: %d\n",
					ret);
				goto link_err;
			}
		}
	}

	/*
	 * Which CVD the ADSP runs decides how the vocproc below is created, so
	 * ask before creating it. The vendor gates the topology commit on
	 * >= 2.2 and the per-path media format commands on >= 2.3 as well.
	 */
	if (!p->v->cvd_version[0] &&
	    !q6mvm_get_cvd_version(p->v->cvd_version, sizeof(p->v->cvd_version)))
		dev_info(dev, "ADSP CVD version: '%s'\n", p->v->cvd_version);

	/*
	 * Android loads the selected topology modules with call calibration,
	 * before it creates the CVP session. Keep that ordering here so the
	 * ADSP sees topology preload before any CVP state exists.
	 */
	if (rt->setup_level == 0) {
		tx_topology = rt->cvp_cfg.tx_topology;
		rx_topology = rt->cvp_cfg.rx_topology;
		if (tx_topology == VSS_IVOCPROC_TOPOLOGY_ID_NONE &&
		    rx_topology == VSS_IVOCPROC_TOPOLOGY_ID_NONE)
			goto topology_ready;

		/*
		 * Only the vendor's own topologies live in this blob. Failing to
		 * register it must not take the call down: the stock topology
		 * ids need it, but Qualcomm's generic ones do not, and refusing
		 * to start left no way to try them. Warn and carry on -- if the
		 * chosen topology really did need the blob, the vocproc create
		 * that follows says so.
		 */
		ret = q6voice_cal_register_topologies();
		if (ret)
			dev_warn(dev, "custom topology registration failed: %d\n",
				 ret);

		dev_info(dev, "preload CVD topology modules tx %#010x rx %#010x\n",
			 tx_topology, rx_topology);
		ret = q6core_load_topology(rx_topology, true);
		if (ret)
			dev_warn(dev, "rx topology preload failed: %d\n", ret);
		ret = q6core_load_topology(tx_topology, true);
		if (ret)
			dev_warn(dev, "tx topology preload failed: %d\n", ret);
		ret = 0;
	}
topology_ready:

	cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	if (!cvp) {
		cvp = q6cvp_session_create(p->type,
					   q6afe_get_port_id(p->tx_port),
					   q6afe_get_port_id(p->rx_port),
					   q6voice_cvp_create_v3(p->v->cvd_version),
					   &rt->cvp_cfg);
		if (IS_ERR(cvp)) {
			ret = PTR_ERR(cvp);
			goto stream_err;
		}
		p->runtime->sessions[Q6VOICE_SERVICE_CVP] = cvp;
	}

	if (rt->setup_level > 0)
		goto configured;

	/*
	 * Describe both directions before committing: the ADSP refuses the
	 * commit until it knows the channel layout of each side.
	 */
	ret = q6cvp_set_channel_info(cvp, &rt->cvp_cfg);
	if (ret)
		dev_warn(dev, "set channel info failed: %d\n", ret);

	/*
	 * CVD 2.3 and later also want the media format of each endpoint before
	 * the topology can be committed.
	 */
	ret = q6cvp_set_media_format(cvp, q6afe_get_port_id(p->tx_port),
				     q6afe_get_port_id(p->rx_port), &rt->cvp_cfg);
	if (ret)
		dev_warn(dev, "set media format failed: %d\n", ret);

	/*
	 * Commit the topology before enabling. Not fatal if the ADSP rejects
	 * it -- older CVD versions do not implement the command at all -- but
	 * newer ones need it or the vocproc never actually runs.
	 */
	ret = q6cvp_topology_commit(cvp);
	if (ret)
		dev_warn(dev, "topology commit failed: %d\n", ret);

	/*
	 * A render port with more than one channel needs the converter set up
	 * behind it, and only after the topology is committed.
	 */
	ret = q6cvp_set_mfc_config(cvp);
	if (ret)
		dev_warn(dev, "media format converter setup failed: %d\n", ret);

	/*
	 * Calibration goes in while the vocproc is still being described.
	 * Once it has been enabled and attached it is running, and a running
	 * vocproc will not take a new table: the command is well formed and
	 * simply fails. Absent calibration is not an error -- the call
	 * proceeds, and only the volume command notices.
	 */
	if (rt->cal_level >= Q6VOICE_CAL_LEND) {
		q6voice_path_free_cal(p);

		char name[128];

		rt->stream_cal = q6voice_cal_load(dev,
				q6voice_cal_firmware(name, sizeof(name),
						     Q6VOICE_CAL_STREAM, 0, 0), mvm);
		rt->dev_cfg = q6voice_cal_load(dev,
				q6voice_cal_firmware(name, sizeof(name),
						     Q6VOICE_CAL_DEV_CFG,
						     rt->tx_acdb, rt->rx_acdb), mvm);
		rt->cal = q6voice_cal_load(dev,
				q6voice_cal_firmware(name, sizeof(name),
						     Q6VOICE_CAL_VOCPROC,
						     rt->tx_acdb, rt->rx_acdb), mvm);
		rt->vol_cal = q6voice_cal_load(dev,
				q6voice_cal_firmware(name, sizeof(name),
						     Q6VOICE_CAL_VOLUME,
						     rt->tx_acdb, rt->rx_acdb), mvm);
	}

	if (rt->cal_level >= Q6VOICE_CAL_REGISTER) {
		bool instance = q6voice_cvp_create_v3(p->v->cvd_version);

		ret = q6voice_cal_register_stream(rt->stream_cal, cvs,
						  instance);
		if (ret)
			dev_warn(dev, "failed to register stream calibration: %d\n",
				 ret);

		if (!rt->dev_cfg)
			dev_warn(dev, "no device configuration to register\n");

		ret = q6voice_cal_register_dev_cfg(rt->dev_cfg, cvp);
		if (ret)
			dev_warn(dev, "failed to register device config: %d\n",
				 ret);

		ret = q6voice_cal_register_cal(rt->cal, cvp, instance);
		if (ret)
			dev_warn(dev, "failed to register calibration: %d\n",
				 ret);

		ret = q6voice_cal_register_vol(rt->vol_cal, cvp, instance);
		if (ret)
			dev_warn(dev, "failed to register volume calibration: %d\n",
				 ret);
	}

configured:
	ret = q6cvp_enable(cvp, true);
	if (ret) {
		dev_err(dev, "failed to enable cvp: %d\n", ret);
		goto cvp_err;
	}

	ret = q6mvm_attach(mvm, cvp, true);
	if (ret) {
		dev_err(dev, "failed to attach cvp to mvm: %d\n", ret);
		goto attach_err;
	}

	ret = q6mvm_set_tty_mode(mvm, 0);
	if (ret)
		dev_warn(dev, "failed to disable TTY mode: %d\n", ret);

	if (rt->setup_level > 0)
		goto started;


	/*
	 * Android sets receive volume and clears the CVS transmit stream mute
	 * before starting voice. Neither command is fatal: the call is still
	 * established without it, just inaudible, and saying so beats refusing
	 * to start.
	 */
	ret = q6cvp_set_rx_volume(cvp, p->v->rx_volume_step,
				  Q6VOICE_VOLUME_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to set rx volume: %d\n", ret);

	ret = q6cvs_set_mute(cvs, VSS_IVOLUME_DIRECTION_TX, false,
			     Q6VOICE_MUTE_RAMP_MS);
	if (ret)
		dev_warn(dev, "failed to unmute tx stream: %d\n", ret);

started:
	ret = q6mvm_start(mvm, true);
	if (ret) {
		dev_err(dev, "failed to start voice: %d\n", ret);
		goto start_err;
	}

	/*
	 * Match voc_enable_device() in the captured Android vendor driver: only
	 * the receive-side CVP device mute is restored here, and it is restored
	 * after START_VOICE has completed. Sending CVP RX and TX mute commands
	 * before START_VOICE differs from that state transition even though the
	 * DSP accepts the packets.
	 */
	if (rt->setup_level == 0) {
		ret = q6cvp_set_device_mute(cvp, VSS_IVOLUME_DIRECTION_RX, false,
					    Q6VOICE_MUTE_RAMP_MS);
		if (ret)
			dev_warn(dev, "failed to unmute rx device: %d\n", ret);
	}

	/*
	 * The call is running from here on: whatever failed above was only
	 * warned about, and returning it would leave a started call that
	 * q6voice_stop() never stops, with the modem link held awake.
	 */
	return 0;

start_err:
	q6mvm_start(mvm, false);
attach_err:
	q6mvm_attach(mvm, cvp, false);
cvp_err:
	q6cvp_enable(cvp, false);
stream_err:
	/* Nothing to detach if the setup level left it out, or if the attach
	 * was left to the modem -- detaching then would tear down its attach.
	 */
	if (cvs && rt->attach_stream)
		q6mvm_attach_stream(mvm, cvs, false);
link_err:
	q6voice_modem_link_end();
	return ret;
}

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime) {
		p->runtime = kzalloc(sizeof(*p->runtime), GFP_KERNEL);
		if (!p->runtime) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (p->runtime->started & BIT(capture)) {
		ret = -EALREADY;
		goto out;
	}

	p->runtime->started |= BIT(capture);

	/* FIXME: For now we only start if both RX/TX are active */
	if (p->runtime->started != 3)
		goto out;

	ret = q6voice_path_start(p);
	if (ret) {
		p->runtime->started &= ~BIT(capture);
		dev_err(v->dev, "failed to start path %d: %d\n", path, ret);
		goto out;
	}

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_start);

static void q6voice_path_stop(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	struct q6voice_session *cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
	int ret;

	dev_dbg(dev, "stop path %d\n", p->type);

	ret = q6mvm_start(mvm, false);
	if (ret)
		dev_err(dev, "failed to stop voice: %d\n", ret);

	ret = q6mvm_attach(mvm, cvp, false);
	if (ret)
		dev_err(dev, "failed to detach cvp from mvm: %d\n", ret);

	ret = q6cvp_enable(cvp, false);
	if (ret)
		dev_err(dev, "failed to disable cvp: %d\n", ret);

	/* Only undo an attach we made ourselves; see attach_stream above. */
	if (cvs && p->runtime->attach_stream) {
		ret = q6mvm_attach_stream(mvm, cvs, false);
		if (ret)
			dev_err(dev, "failed to detach stream from mvm: %d\n", ret);
	}

	q6voice_modem_link_end();
}

static void q6voice_path_destroy(struct q6voice_path *p)
{
	struct q6voice_path_runtime *runtime = p->runtime;
	enum q6voice_service_type svc;

	/* the handles are the MVM session's: return them before it goes */
	q6voice_path_free_cal(p);

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; ++svc) {
		if (runtime->sessions[svc])
			q6voice_session_release(runtime->sessions[svc]);
	}

	p->runtime = NULL;
	kfree(runtime);
}

int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime || !(p->runtime->started & BIT(capture)))
		goto out;

	if (p->runtime->started == 3)
		q6voice_path_stop(p);

	p->runtime->started &= ~BIT(capture);

	if (p->runtime->started == 0)
		q6voice_path_destroy(p);

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_stop);

static void q6voice_free(void *data)
{
	struct q6voice *v = data;
	enum q6voice_path_type path;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		mutex_lock(&p->lock);
		if (p->runtime) {
			dev_warn(v->dev,
				 "q6voice_remove() called while path %d is active\n",
				 path);

			if (p->runtime->started == 3)
				q6voice_path_stop(p);
			q6voice_path_destroy(p);
		}
		mutex_unlock(&p->lock);
		mutex_destroy(&p->lock);
	}
}

/*
 * Downlink volume, as a step rather than a gain.
 *
 * The ADSP resolves a step against the volume calibration table for the device
 * in use, so there is nothing meaningful to send it but an index; the vendor
 * HAL maps the user-facing volume onto the same 0..5 range. Applied
 * immediately when a call is up, and remembered for the next one either way.
 */
struct q6voice *q6voice_create(struct device *dev)
{
	struct q6voice *v;
	enum q6voice_path_type path;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);

	v->dev = dev;
	v->rx_volume_step = Q6VOICE_RX_VOLUME_STEP;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		p->v = v;
		p->type = path;
		/* no board data: the pass-through, one microphone */
		p->params[Q6VOICE_TX_TOPOLOGY] = VSS_IVOCPROC_TOPOLOGY_ID_NONE;
		p->params[Q6VOICE_RX_TOPOLOGY] = VSS_IVOCPROC_TOPOLOGY_ID_NONE;
		p->params[Q6VOICE_TX_CHANNELS] = 1;
		mutex_init(&p->lock);
	}

	ret = devm_add_action(dev, q6voice_free, v);
	if (ret)
		return ERR_PTR(ret);

	return v;
}
EXPORT_SYMBOL_GPL(q6voice_create);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_port;
	else
		return p->rx_port;
}
EXPORT_SYMBOL_GPL(q6voice_get_port);

void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		p->tx_port = index;
	else
		p->rx_port = index;
}
EXPORT_SYMBOL_GPL(q6voice_set_port);

u32 q6voice_get_param(struct q6voice *v, enum q6voice_path_type path,
		      enum q6voice_param param)
{
	struct q6voice_path *p = &v->paths[path];
	u32 val;

	mutex_lock(&p->lock);
	val = p->params[param];
	mutex_unlock(&p->lock);
	return val;
}
EXPORT_SYMBOL_GPL(q6voice_get_param);

/* Takes effect at the next call start: a running vocproc keeps its setup. */
void q6voice_set_param(struct q6voice *v, enum q6voice_path_type path,
		       enum q6voice_param param, u32 val)
{
	struct q6voice_path *p = &v->paths[path];

	mutex_lock(&p->lock);
	p->params[param] = val;
	mutex_unlock(&p->lock);
}
EXPORT_SYMBOL_GPL(q6voice_set_param);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice driver");
MODULE_LICENSE("GPL v2");
