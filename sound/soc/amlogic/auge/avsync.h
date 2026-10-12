/*
 * sound/soc/amlogic/auge/avsync.h
 *
 * Audio output paced by the tsync system clock (pcrscr).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __AML_AUDIO_AVSYNC_H__
#define __AML_AUDIO_AVSYNC_H__

#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <sound/pcm.h>

struct aml_avsync_ops {
	/* start the output DMA, atomic context */
	void (*start)(void *priv);
	/* trim the output clock, process context */
	void (*set_ppm)(void *priv, int ppm);
};

struct aml_avsync {
	spinlock_t lock;
	const struct aml_avsync_ops *ops;
	void *priv;
	struct snd_pcm_substream *substream;
	struct hrtimer gate;

	/* timing */
	bool gating;
	bool started;
	ktime_t start_at;
	ktime_t gate_until;
	ktime_t restart_at;

	/* PTS of the next written frame, -1 = none */
	s64 next_pts;
	s64 last_pts;
	bool mapped;
	snd_pcm_uframes_t map_frame;
	u32 map_pts;
	unsigned int rate;
	/* drop granularity in frames, a whole IEC 61937 burst for raw data;
	 * the frames between two mapped writes, seen twice, give the burst
	 */
	unsigned int quantum;
	/* a drop or insert waits for the next PTS to confirm the error */
	int pending;
	u64 pending_pts;
	bool raw;
	snd_pcm_uframes_t burst;
	/* written data lands this far back: dropped minus inserted frames */
	snd_pcm_sframes_t shift;
	/* part of the shift the reported position carries, and the last DMA
	 * position; dropped frames count as played, inserted ones hold it
	 */
	snd_pcm_sframes_t ptr_shift;
	snd_pcm_uframes_t last_phys;

	int user_offset_ms;
	/* the player runs the system clock itself (media playback, no PCR) */
	bool sysclock;

	/* clock trim */
	int ppm;
	int integ;
	s64 err_sum;
	int err_cnt;
	ktime_t trim_at;

	/* status */
	bool reported;
	int last_err_us;
	unsigned int drops;
	unsigned int restarts;
	unsigned int inserts;
};

void aml_avsync_init(struct aml_avsync *s, const struct aml_avsync_ops *ops,
		     void *priv);
/* rate: frames per second the output plays */
void aml_avsync_prepare(struct aml_avsync *s,
			struct snd_pcm_substream *substream, bool raw,
			unsigned int rate);
bool aml_avsync_trigger_start(struct aml_avsync *s);
void aml_avsync_trigger_stop(struct aml_avsync *s);
bool aml_avsync_gating(struct aml_avsync *s);
int aml_avsync_copy(struct aml_avsync *s, struct snd_pcm_substream *substream,
		    snd_pcm_uframes_t pos, void __user *buf,
		    snd_pcm_uframes_t count);
void aml_avsync_set_pts(struct aml_avsync *s, s64 pts);
void aml_avsync_set_sysclock(struct aml_avsync *s, bool on);
void aml_avsync_get_status(struct aml_avsync *s, long *val, int n);
snd_pcm_uframes_t aml_avsync_pointer(struct aml_avsync *s,
				     struct snd_pcm_runtime *runtime,
				     snd_pcm_uframes_t phys);

#endif
