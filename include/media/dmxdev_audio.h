/*
 * include/media/dmxdev_audio.h
 *
 * Audio decoder outside the demux driver: PES filters with decoder output
 * for an audio decoder deliver their transport stream packets here.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _DMXDEV_AUDIO_H_
#define _DMXDEV_AUDIO_H_

#include <linux/types.h>

struct dmxdev_audio_sink {
	/* index: audio decoder 0..3, as DMX_PES_AUDIO0..3; atomic context */
	void (*start)(void *priv, int index);
	void (*write)(void *priv, int index, const u8 *buf1, size_t len1,
		      const u8 *buf2, size_t len2);
	void (*stop)(void *priv, int index);
	void *priv;
};

int dmxdev_audio_sink_register(const struct dmxdev_audio_sink *sink);

#endif
