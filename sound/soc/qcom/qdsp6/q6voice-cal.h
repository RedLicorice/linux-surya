/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_CAL_H
#define _Q6_VOICE_CAL_H

#include <linux/types.h>
#include "q6cvp.h"

struct device;
struct q6voice_cal;
struct q6voice_session;

/*
 * A call's calibration files, in the calibration node's firmware-name
 * directory: stream.bin, topologies.bin, and per pair of ACDB devices
 * <kind>-<tx>-<rx>.bin (devcfg, vocproc, volume; the ids as four hex digits).
 */
enum q6voice_cal_file {
	Q6VOICE_CAL_STREAM,	/* the stream's own */
	Q6VOICE_CAL_DEV_CFG,	/* what the endpoints are */
	Q6VOICE_CAL_VOCPROC,	/* the vocproc, other than by step */
	Q6VOICE_CAL_VOLUME,	/* the vocproc, per volume step */
	Q6VOICE_CAL_TOPOLOGIES,	/* the board's AVCS custom topologies */
	Q6VOICE_CAL_FILE_COUNT,
};

/*
 * The file's name in @buf, or NULL when the board has none: no directory, or
 * a per-device file without a device pair (@tx or @rx 0).
 */
const char *q6voice_cal_firmware(char *buf, size_t len,
				 enum q6voice_cal_file file, u32 tx, u32 rx);
int q6voice_cal_register_topologies(void);

/*
 * Returns NULL when no calibration is installed for the board, which callers
 * should treat as "carry on without it" rather than as a failure.
 */
struct q6voice_cal *q6voice_cal_load(struct device *dev, const char *name,
				     struct q6voice_session *mvm);
/*
 * @mvm is the session the memory was lent to, and may be NULL when it is
 * already gone -- the mapping goes with it, so there is nothing to return.
 */
void q6voice_cal_free(struct q6voice_cal *cal, struct q6voice_session *mvm);

/*
 * @instance says which era of registration command the vocproc expects; it
 * goes with the vocproc's version, the same way the create command does.
 */
int q6voice_cal_register_vol(struct q6voice_cal *cal,
			     struct q6voice_session *cvp, bool instance);
int q6voice_cal_register_cal(struct q6voice_cal *cal,
			     struct q6voice_session *cvp, bool instance);
int q6voice_cal_register_stream(struct q6voice_cal *cal,
				struct q6voice_session *cvs, bool instance);
int q6voice_cal_register_dev_cfg(struct q6voice_cal *cal,
				 struct q6voice_session *cvp);

#endif /*_Q6_VOICE_CAL_H */
