/*
 * dreamaudio.h - audio decoder device run by a userspace decoding service
 *
 * The device takes the DVB audio API from the player and the transport
 * stream packets of PES filters with decoder output for its audio decoder.
 * A decoding service reads both and plays through the ALSA driver, which
 * paces the output by the PTS.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI_DVBDREAMAUDIO_H_
#define _UAPI_DVBDREAMAUDIO_H_

#include <linux/types.h>

struct dreamaudio_state {
	__u32 seq;		/* changes with every command */
	__u32 clear_seq;	/* AUDIO_CLEAR_BUFFER and stream restarts */
	__u32 play_state;	/* audio_play_state_t */
	__u32 source;		/* audio_stream_source_t */
	__s32 bypass;		/* AUDIO_SET_BYPASS_MODE value */
	__u32 channel;		/* audio_channel_select_t */
	__u32 mute;
	__u32 av_sync;
	__u32 overflows;	/* transport stream packets lost */
};

/* the caller becomes the decoding service; read() then returns the
 * transport stream packets, poll() signals POLLPRI on a state change
 */
#define DREAMAUDIO_SERVICE	_IO('o', 0xd0)
#define DREAMAUDIO_GET_STATE	_IOR('o', 0xd1, struct dreamaudio_state)

#endif
