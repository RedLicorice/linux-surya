/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace interface of the QDSP6 Listen (LSM) service: one keyword detection session on the aDSP.
 *
 * The flow, as the Android sound-trigger HAL drives it downstream:
 *   open(/dev/q6lsm)
 *   Q6LSM_IOC_OPEN            the session (legacy app id, or a topology)
 *   Q6LSM_IOC_SET_PARAM       e.g. connect to the AFE port, operation mode, confidence levels, the sound model
 *                             (large payloads go out of band, through memory the driver maps for the DSP)
 *   Q6LSM_IOC_REGISTER_MODEL  the legacy sound model command (sessions opened without a topology)
 *   Q6LSM_IOC_START
 *   read()/poll()             one struct q6lsm_event per DSP event (a detection, among others)
 *   Q6LSM_IOC_STOP
 *   close()                   stops and closes the session, unmaps and frees its memory
 */
#ifndef _UAPI__SOUND_Q6LSM_H
#define _UAPI__SOUND_Q6LSM_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* q6lsm_open.kind */
#define Q6LSM_OPEN_APP_ID	1	/* LSM_SESSION_CMD_OPEN_TX: value = app id (2: voice wakeup v2) */
#define Q6LSM_OPEN_TOPOLOGY	2	/* LSM_SESSION_CMD_OPEN_TX_V2: value = topology id */
#define Q6LSM_OPEN_STAGES	3	/* LSM_SESSION_CMD_OPEN_TX_V3: value = topology id of the one stage */

struct q6lsm_open {
	__u32 kind;
	__u32 value;
	__u32 island;		/* Q6LSM_OPEN_STAGES only: run the stage in the low-power island */
	__u32 reserved;
};

/* q6lsm_param.flags */
#define Q6LSM_PARAM_V3		(1 << 0)	/* SET_PARAMS_V3 with an instance id, else SET_PARAMS_V2 */
#define Q6LSM_PARAM_OUT_OF_BAND	(1 << 1)	/* the payload goes through mapped memory (a sound model) */
#define Q6LSM_PARAM_LEGACY	(1 << 2)	/* SET_PARAMS (v1 header) for sessions opened by app id */

struct q6lsm_param {
	__u32 module_id;
	__u16 instance_id;
	__u16 reserved;
	__u32 param_id;
	__u32 size;		/* bytes at data */
	__u64 data;		/* user pointer */
	__u32 flags;
	__u32 reserved2;
};

struct q6lsm_model {
	__u64 data;		/* user pointer */
	__u32 size;
	__u32 reserved;
};

#define Q6LSM_EVENT_MAX_PAYLOAD	256

/* One DSP event, as read() returns it (a read returns whole events only). */
struct q6lsm_event {
	__u32 opcode;		/* e.g. 0x00012B00..0x00012B06: LSM_SESSION_EVENT_DETECTION_STATUS(_V2/_V3), ... */
	__u32 size;		/* bytes of payload used */
	__u64 time_ns;		/* CLOCK_BOOTTIME when the driver got it */
	__u8 payload[Q6LSM_EVENT_MAX_PAYLOAD];
};

#define Q6LSM_IOC_MAGIC			'L'
#define Q6LSM_IOC_OPEN			_IOW(Q6LSM_IOC_MAGIC, 0x01, struct q6lsm_open)
#define Q6LSM_IOC_SET_PARAM		_IOW(Q6LSM_IOC_MAGIC, 0x02, struct q6lsm_param)
#define Q6LSM_IOC_REGISTER_MODEL	_IOW(Q6LSM_IOC_MAGIC, 0x03, struct q6lsm_model)
#define Q6LSM_IOC_START			_IO(Q6LSM_IOC_MAGIC, 0x04)
#define Q6LSM_IOC_STOP			_IO(Q6LSM_IOC_MAGIC, 0x05)

#endif /* _UAPI__SOUND_Q6LSM_H */
