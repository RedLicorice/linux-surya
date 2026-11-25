/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_VOICE_H
#define _Q6_VOICE_H

enum q6voice_path_type {
	/*
	 * Android maps VSID 0x11c05000 (VoiceMMode1) to passive-session
	 * index 6 and uses that index as the APR source port. The session name
	 * alone is not sufficient: using source port 0 creates the legacy Voice
	 * passive session, which the modem's VoiceMMode1 state machine never
	 * joins even though every ADSP command succeeds.
	 */
	Q6VOICE_PATH_VOICEMMODE1 = 6,
	Q6VOICE_PATH_VOICE	= Q6VOICE_PATH_VOICEMMODE1,
	/* TODO: Q6VOICE_PATH_VOIP	= 1, */
	/* TODO: Q6VOICE_PATH_VOLTE	= 2, */
	/* TODO: Q6VOICE_PATH_VOICE2	= 3, */
	/* TODO: Q6VOICE_PATH_QCHAT	= 4, */
	/* TODO: Q6VOICE_PATH_VOWLAN	= 5, */
	/* TODO: Q6VOICE_PATH_VOICEMMODE2	= 7, */
	Q6VOICE_PATH_COUNT	= 8,
};

struct q6voice;

/*
 * What a call is set up with, per path: the board's data, set by userspace
 * (the UCM's call verb) and read when the call starts.
 */
enum q6voice_param {
	Q6VOICE_TX_TOPOLOGY,	/* vocproc capture topology id */
	Q6VOICE_RX_TOPOLOGY,	/* vocproc render topology id */
	Q6VOICE_TX_CHANNELS,	/* microphone channels the vocproc takes */
	Q6VOICE_TX_ACDB_DEVICE,	/* the vendor ACDB's capture device: calibration */
	Q6VOICE_RX_ACDB_DEVICE,	/* the vendor ACDB's render device: calibration */
	Q6VOICE_PARAM_COUNT,
};

struct q6voice *q6voice_create(struct device *dev);
int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture);
int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture);


int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture);
void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index);
u32 q6voice_get_param(struct q6voice *v, enum q6voice_path_type path,
		      enum q6voice_param param);
void q6voice_set_param(struct q6voice *v, enum q6voice_path_type path,
		       enum q6voice_param param, u32 val);

#endif /*_Q6_VOICE_H */
