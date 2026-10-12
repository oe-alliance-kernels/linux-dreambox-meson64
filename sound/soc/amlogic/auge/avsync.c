/*
 * sound/soc/amlogic/auge/avsync.c
 *
 * Audio output paced by the tsync system clock (pcrscr), the clock the
 * video layer presents frames by. Userspace passes the PTS of the data
 * it writes; the start of the output is held until the first frame is
 * due, late data is dropped and the output clock is trimmed to follow
 * the system clock.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/amlogic/media/frame_sync/timestamp.h>
#include <linux/amlogic/media/frame_sync/tsync.h>
#include <linux/amlogic/media/frame_sync/tsync_pcr.h>

#include "avsync.h"

/* audio plays this much ahead of the pcrscr */
static int avsync_offset_ms = -8;
module_param(avsync_offset_ms, int, 0644);
MODULE_PARM_DESC(avsync_offset_ms, "audio lead over the system clock in ms");

static int avsync_debug;

int aml_avsync_delay_pcm;
int aml_avsync_delay_raw;
module_param(avsync_debug, int, 0644);
MODULE_PARM_DESC(avsync_debug, "1: log the clock trim, 2: also the timing of each write");

#define LATE_DROP_US		20000
#define EARLY_FIX_US		20000
#define EARLY_RESTART_US	40000
#define TIMING_LIMIT_US		2000000
#define DROP_MARGIN_US		5000
#define TRIM_INTERVAL_MS	500
#define TRIM_KP			50
#define TRIM_KI			5
#define TRIM_INTEG_MAX		300
#define TRIM_PPM_MAX		500
#define RESTART_HOLDOFF_MS	2000
#define GATE_MAX_MS		1000
/* audio this far ahead holds the output until it is due */
#define GATE_EARLY_US		4000000
#define GATE_LEAD_US		3000
#define GATE_POLL_MS		10
/* demux audio this far behind the system clock is skipped, in 90 kHz */
#define STALE_PTS		(90 * 200)

static bool avsync_gate_wait(struct aml_avsync *s, s64 *wait_us);

/*
 * The held output starts when the first written frame is due, GATE_EARLY_US
 * ahead at most. A full buffer gets no more writes to work that out on: the
 * timer looks again until the system clock is usable, GATE_MAX_MS at most.
 */
static enum hrtimer_restart avsync_gate_fn(struct hrtimer *t)
{
	struct aml_avsync *s = container_of(t, struct aml_avsync, gate);
	unsigned long flags;
	ktime_t now, next;
	s64 wait_us;

	spin_lock_irqsave(&s->lock, flags);
	if (s->gating) {
		now = ktime_get();
		if (ktime_before(now, s->gate_until)) {
			next = ktime_set(0, 0);
			if (!avsync_gate_wait(s, &wait_us)) {
				next = ktime_add_ms(now, GATE_POLL_MS);
				if (ktime_after(next, s->gate_until))
					next = s->gate_until;
			} else if (wait_us > 0) {
				/* a known start time holds the output past GATE_MAX_MS */
				next = ktime_add_us(now, wait_us);
				if (ktime_after(next, s->gate_until))
					s->gate_until = next;
			}
			if (ktime_to_ns(next)) {
				s->start_at = next;
				hrtimer_set_expires(t, next);
				spin_unlock_irqrestore(&s->lock, flags);
				return HRTIMER_RESTART;
			}
		}
		s->gating = false;
		s->started = true;
		s->ops->start(s->priv);
	}
	spin_unlock_irqrestore(&s->lock, flags);

	return HRTIMER_NORESTART;
}

void aml_avsync_init(struct aml_avsync *s, const struct aml_avsync_ops *ops,
		     void *priv)
{
	memset(s, 0, sizeof(*s));
	spin_lock_init(&s->lock);
	s->ops = ops;
	s->priv = priv;
	s->next_pts = -1;
	hrtimer_init(&s->gate, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	s->gate.function = avsync_gate_fn;
}

void aml_avsync_prepare(struct aml_avsync *s,
			struct snd_pcm_substream *substream, bool raw,
			unsigned int rate)
{
	unsigned long flags;

	hrtimer_cancel(&s->gate);

	spin_lock_irqsave(&s->lock, flags);
	s->substream = substream;
	s->gating = false;
	s->started = false;
	s->start_at = ktime_set(0, 0);
	s->next_pts = -1;
	s->last_pts = -1;
	s->reported = false;
	s->mapped = false;
	s->shift = 0;
	s->ptr_shift = 0;
	s->last_phys = 0;
	s->rate = rate;
	s->quantum = raw ? max(1536U * rate / 48000, 1U) : 1;
	s->raw = raw;
	s->burst = 0;
	s->pending = 0;
	s->err_sum = 0;
	s->err_cnt = 0;
	s->integ = 0;
	s->trim_at = ktime_get();
	spin_unlock_irqrestore(&s->lock, flags);

	/* hw_params may have reset the output clock */
	if (s->ppm)
		s->ops->set_ppm(s->priv, s->ppm);
}

/* true: start the output now, false: the gate timer starts it */
bool aml_avsync_trigger_start(struct aml_avsync *s)
{
	unsigned long flags;
	ktime_t now = ktime_get();
	ktime_t at;
	bool start = true;

	spin_lock_irqsave(&s->lock, flags);
	if (s->mapped) {
		/* without a start time yet the system clock is not usable, look
		 * again shortly */
		at = ktime_to_ns(s->start_at) ? s->start_at :
			ktime_add_ms(now, GATE_POLL_MS);
		if (ktime_after(at, now)) {
			s->gating = true;
			s->gate_until = ktime_add_ms(now, GATE_MAX_MS);
			hrtimer_start(&s->gate, at, HRTIMER_MODE_ABS);
			start = false;
		}
	}
	if (start)
		s->started = true;
	spin_unlock_irqrestore(&s->lock, flags);

	return start;
}

void aml_avsync_trigger_stop(struct aml_avsync *s)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->gating = false;
	s->started = false;
	spin_unlock_irqrestore(&s->lock, flags);
	hrtimer_try_to_cancel(&s->gate);
}

bool aml_avsync_gating(struct aml_avsync *s)
{
	return READ_ONCE(s->gating);
}

void aml_avsync_set_pts(struct aml_avsync *s, s64 pts)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->next_pts = pts;
	spin_unlock_irqrestore(&s->lock, flags);
}

void aml_avsync_set_sysclock(struct aml_avsync *s, bool on)
{
	if (READ_ONCE(s->sysclock) == on)
		return;
	WRITE_ONCE(s->sysclock, on);
	tsync_pcr_set_sysclock(on);
}

/* a player in pause: hold the system clock */
void aml_avsync_hold_clock(bool hold)
{
	tsync_pcr_hold(hold);
}

/* audio of a demux PES this far behind the system clock comes too late to
 * play with the picture
 */
bool aml_avsync_stale(u32 pts)
{
	u32 scr;

	if (tsync_get_mode() != TSYNC_MODE_PCRMASTER ||
	    !tsync_pcr_demux_pcr_used())
		return false;
	scr = timestamp_pcrscr_get();

	return scr && (s32)(pts - scr) < -STALE_PTS;
}

void aml_avsync_get_status(struct aml_avsync *s, long *val, int n)
{
	long v[7];

	v[0] = s->last_err_us;
	v[1] = s->ppm;
	v[2] = s->shift;
	v[3] = s->drops;
	v[4] = s->restarts;
	v[5] = (s->gating ? 1 : 0) | (s->started ? 2 : 0) | (s->mapped ? 4 : 0);
	v[6] = s->inserts;
	memcpy(val, v, sizeof(long) * min_t(int, n, ARRAY_SIZE(v)));
}

/* ALSA position of the output: DMA position plus the frames dropped */
snd_pcm_uframes_t aml_avsync_pointer(struct aml_avsync *s,
				     struct snd_pcm_runtime *runtime,
				     snd_pcm_uframes_t phys)
{
	snd_pcm_sframes_t size = runtime->buffer_size;
	snd_pcm_sframes_t shift = READ_ONCE(s->shift);
	snd_pcm_sframes_t played = ((snd_pcm_sframes_t)phys -
		(snd_pcm_sframes_t)s->last_phys + size) % size;

	if (shift > s->ptr_shift)
		s->ptr_shift = shift;
	else if (shift < s->ptr_shift)
		s->ptr_shift = max(shift, s->ptr_shift - played);
	s->last_phys = phys;

	return ((snd_pcm_sframes_t)phys + s->ptr_shift % size + size) % size;
}

int aml_avsync_get_pts(struct aml_avsync *s, u64 *pts)
{
	struct snd_pcm_substream *substream;
	unsigned long flags;
	s64 frames;
	int ret = -ENODATA;

	spin_lock_irqsave(&s->lock, flags);
	substream = s->substream;
	if (s->started && s->mapped && s->rate && substream &&
	    substream->runtime) {
		frames = (s64)(substream->runtime->status->hw_ptr -
			       s->map_frame);
		*pts = (s->map_pts + div_s64(frames * 90000, s->rate)) &
			0x1ffffffffULL;
		ret = 0;
	}
	spin_unlock_irqrestore(&s->lock, flags);

	return ret;
}

static snd_pcm_uframes_t us_to_frames(struct aml_avsync *s, s64 us)
{
	return div_u64((u64)us * s->rate, 1000000);
}

static s64 frames_to_us(struct aml_avsync *s, s64 frames)
{
	return div_s64(frames * 1000000, s->rate);
}

static void avsync_trim(struct aml_avsync *s, ktime_t now, s64 err_us)
{
	s64 avg_us;
	int ppm;

	s->err_sum += err_us;
	s->err_cnt++;
	if (ktime_ms_delta(now, s->trim_at) < TRIM_INTERVAL_MS)
		return;

	/* late: run faster; the integral follows a stream whose audio does
	 * not run at the rate its PTS say
	 */
	avg_us = div_s64(s->err_sum, s->err_cnt);
	s->integ = clamp_t(int, s->integ + div_s64(avg_us * TRIM_KI, 1000),
			   -TRIM_INTEG_MAX, TRIM_INTEG_MAX);
	ppm = clamp_t(int, div_s64(avg_us * TRIM_KP, 1000) + s->integ,
		      -TRIM_PPM_MAX, TRIM_PPM_MAX);
	if (abs(ppm - s->ppm) >= 2) {
		if (avsync_debug)
			pr_info("avsync: err %lldus ppm %d integ %d\n",
				avg_us, ppm, s->integ);
		s->ppm = ppm;
		s->ops->set_ppm(s->priv, ppm);
	}
	s->err_sum = 0;
	s->err_cnt = 0;
	s->trim_at = now;
}

static void avsync_trim_reset(struct aml_avsync *s, ktime_t now)
{
	s->err_sum = 0;
	s->err_cnt = 0;
	s->trim_at = now;
}

/* the pcrscr follows the demux PCR, or the video of a media player */
static bool avsync_scr(struct aml_avsync *s, u32 *scr)
{
	u32 pcr;

	if (READ_ONCE(s->sysclock)) {
		/* stopped without video (audio only) and in pause: play free */
		if (!timestamp_pcrscr_enable_state())
			return false;
		*scr = timestamp_pcrscr_get();
		return *scr != 0;
	}
	if (tsync_get_mode() != TSYNC_MODE_PCRMASTER)
		return false;
	if (!tsync_pcr_demux_pcr_used())
		return false;
	if (!tsync_get_demux_pcr(&pcr) || !pcr || pcr == 0xffffffff)
		return false;
	*scr = timestamp_pcrscr_get();
	/* The demux of a played recording often reports its PCR invalid
	 * after a pause or a jump while tsync follows it all the same; a
	 * system clock running with the PCR is the clock the picture plays
	 * by. One far off it is from before a zap or a flush.
	 */
	if (abs((s32)(*scr - pcr)) > 90 * 1000)
		return false;

	return *scr != 0;
}

/* s->lock held: until the first written frame is due; false while the
 * system clock gives no usable time for it */
static bool avsync_gate_wait(struct aml_avsync *s, s64 *wait_us)
{
	struct snd_pcm_substream *substream = s->substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_uframes_t appl;
	snd_pcm_sframes_t queued;
	s64 due_us;
	u32 pts, scr;

	if (!s->mapped || !s->rate || !substream || !substream->runtime ||
	    !avsync_scr(s, &scr))
		return false;
	runtime = substream->runtime;
	appl = runtime->control->appl_ptr;
	pts = (u32)s->map_pts +
		(u32)div_u64((u64)(appl - s->map_frame) * 90000, s->rate);
	due_us = div_s64((s64)(s32)(pts - scr) * 100, 9) -
		(s64)(avsync_offset_ms - s->user_offset_ms) * 1000 +
		div_s64((s64)(s->quantum > 1 ? READ_ONCE(aml_avsync_delay_raw) :
			      READ_ONCE(aml_avsync_delay_pcm)) * 100, 9);
	if (due_us < -TIMING_LIMIT_US || due_us > GATE_EARLY_US)
		return false;
	queued = (snd_pcm_sframes_t)(appl - runtime->status->hw_ptr) -
		(s->shift - READ_ONCE(s->ptr_shift));
	*wait_us = due_us - frames_to_us(s, queued) + GATE_LEAD_US;
	return true;
}

/* start the held output at the given time, the gate timer is the fallback */
static void avsync_set_start(struct aml_avsync *s, ktime_t at)
{
	unsigned long flags;

	spin_lock_irqsave(&s->lock, flags);
	s->start_at = at;
	if (s->gating) {
		/* the time comes from a due frame, GATE_EARLY_US ahead at most */
		if (ktime_after(at, s->gate_until))
			s->gate_until = at;
		hrtimer_start(&s->gate, at, HRTIMER_MODE_ABS);
	}
	spin_unlock_irqrestore(&s->lock, flags);
}

static snd_pcm_uframes_t avsync_ring_pos(struct snd_pcm_runtime *runtime,
					 snd_pcm_sframes_t frame)
{
	snd_pcm_sframes_t size = runtime->buffer_size;
	snd_pcm_sframes_t pos = frame % size;

	return pos < 0 ? pos + size : pos;
}

static void avsync_zero(struct snd_pcm_runtime *runtime,
			snd_pcm_uframes_t pos, snd_pcm_uframes_t count)
{
	snd_pcm_uframes_t first = min(count, runtime->buffer_size - pos);

	memset(runtime->dma_area + frames_to_bytes(runtime, pos), 0,
	       frames_to_bytes(runtime, first));
	if (count > first)
		memset(runtime->dma_area, 0,
		       frames_to_bytes(runtime, count - first));
}

static void avsync_update(struct aml_avsync *s,
			  struct snd_pcm_substream *substream,
			  snd_pcm_uframes_t appl, snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_sframes_t queued, room;
	snd_pcm_uframes_t d;
	unsigned long flags;
	s64 pts_in, due_us, play_us, err_us;
	ktime_t now;
	u32 pts, scr;

	spin_lock_irqsave(&s->lock, flags);
	pts_in = s->next_pts;
	s->next_pts = -1;
	spin_unlock_irqrestore(&s->lock, flags);

	/* frames decoded from one PES packet repeat its PTS */
	if (pts_in >= 0 && pts_in != s->last_pts) {
		/* one burst per write: AC-4 and DTS bursts are no 1536 frames */
		if (s->raw && s->mapped) {
			d = appl - s->map_frame;
			if (d == s->burst && d >= 64 && d <= runtime->buffer_size / 4 &&
			    d != s->quantum) {
				s->quantum = d;
				pr_info("avsync: bursts of %lu frames\n", d);
			}
			s->burst = d;
		}
		s->last_pts = pts_in;
		s->map_frame = appl;
		s->map_pts = pts_in & 0x1ffffffffULL;
		s->mapped = true;
	}
	if (!s->mapped || !s->rate || !avsync_scr(s, &scr))
		return;

	now = ktime_get();
	pts = (u32)s->map_pts +
		(u32)div_u64((u64)(appl - s->map_frame) * 90000, s->rate);
	/* when the first frame of this write is due, from now */
	due_us = div_s64((s64)(s32)(pts - scr) * 100, 9) -
		(s64)(avsync_offset_ms - s->user_offset_ms) * 1000 +
		div_s64((s64)(s->quantum > 1 ? READ_ONCE(aml_avsync_delay_raw) :
			      READ_ONCE(aml_avsync_delay_pcm)) * 100, 9);
	if (avsync_debug > 1)
		pr_info_ratelimited("avsync: appl %lu map %lu/%llx pts %x scr %x due %lldus rate %u\n",
				    appl, s->map_frame, s->map_pts, pts, scr,
				    due_us, s->rate);
	if (due_us < -TIMING_LIMIT_US ||
	    due_us > (READ_ONCE(s->started) ? TIMING_LIMIT_US : GATE_EARLY_US))
		return;

	/* frames ahead of this write in output order */
	queued = (snd_pcm_sframes_t)(appl - runtime->status->hw_ptr) -
		(s->shift - READ_ONCE(s->ptr_shift));
	play_us = frames_to_us(s, queued);

	if (!READ_ONCE(s->started)) {
		/* the output fetches ahead once started */
		s64 wait_us = due_us - play_us + GATE_LEAD_US;

		if (wait_us >= 0) {
			avsync_set_start(s, ktime_add_us(now, wait_us));
			return;
		}
		/* late: drop what is written and start, the running output
		 * drops the rest; dropped frames still fill the buffer
		 */
		d = roundup(us_to_frames(s, -wait_us), s->quantum);
		d = min_t(snd_pcm_uframes_t, d, max_t(snd_pcm_sframes_t,
			  queued + (snd_pcm_sframes_t)count, 0));
		d = rounddown(d, s->quantum);
		WRITE_ONCE(s->shift, s->shift + d);
		if (d)
			s->drops++;
		avsync_set_start(s, now);
		return;
	}

	err_us = play_us - due_us;
	s->last_err_us = err_us;
	if (!s->reported) {
		s->reported = true;
		pr_info("avsync: started, %lldus off\n", err_us);
	}

	if (err_us > LATE_DROP_US || err_us < -EARLY_FIX_US) {
		/* one stray PTS is no reason to drop or insert */
		int dir = err_us > 0 ? 1 : -1;

		if (s->pending != dir) {
			s->pending = dir;
			s->pending_pts = s->map_pts;
			return;
		}
		if (s->pending_pts == s->map_pts)
			return;
	}
	s->pending = 0;

	if (err_us > LATE_DROP_US) {
		/* data written earlier is overwritten */
		room = queued - (snd_pcm_sframes_t)us_to_frames(s, DROP_MARGIN_US);
		d = room > 0 ? min_t(snd_pcm_uframes_t,
				     us_to_frames(s, err_us), room) : 0;
		/* the reported position jumps by the drop; a jump near the
		 * buffer size is no progress to ALSA
		 */
		d = min_t(snd_pcm_uframes_t, d, runtime->buffer_size / 4);
		d = rounddown(d, s->quantum);
		if (!d) {
			/* less than a burst: the clock trim catches up */
			avsync_trim(s, now, err_us);
			return;
		}
		WRITE_ONCE(s->shift, s->shift + d);
		s->drops++;
		pr_info("avsync: %lldms late, dropped %lu frames\n",
			div_s64(err_us, 1000), d);
		avsync_trim_reset(s, now);
	} else if (err_us < -EARLY_FIX_US) {
		/* silence before this write, as far as the buffer holds it */
		room = (snd_pcm_sframes_t)runtime->buffer_size -
			(snd_pcm_sframes_t)us_to_frames(s, DROP_MARGIN_US) -
			(queued + (snd_pcm_sframes_t)count);
		d = room > 0 ? min_t(snd_pcm_uframes_t,
				     us_to_frames(s, -err_us), room) : 0;
		d = rounddown(d, s->quantum);
		if (!d && (us_to_frames(s, -err_us) < s->quantum ||
			   err_us > -EARLY_RESTART_US)) {
			/* less than a burst, or no room for silence: the clock
			 * trim catches up
			 */
			avsync_trim(s, now, err_us);
			return;
		}
		if (d) {
			avsync_zero(runtime,
				    avsync_ring_pos(runtime, appl - s->shift), d);
			WRITE_ONCE(s->shift, s->shift - d);
			s->inserts++;
			pr_info("avsync: %lldms early, inserted %lu frames\n",
				div_s64(-err_us, 1000), d);
		} else if (ktime_after(now, s->restart_at)) {
			s->restarts++;
			s->restart_at = ktime_add_ms(now, RESTART_HOLDOFF_MS);
			pr_info("avsync: %lldms early, restarting the output\n",
				div_s64(-err_us, 1000));
			snd_pcm_stop_xrun(substream);
		}
		avsync_trim_reset(s, now);
	} else {
		avsync_trim(s, now, err_us);
	}
}

int aml_avsync_copy(struct aml_avsync *s, struct snd_pcm_substream *substream,
		    snd_pcm_uframes_t pos, void __user *buf,
		    snd_pcm_uframes_t count)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t size = runtime->buffer_size;
	snd_pcm_uframes_t dst, first;
	char *area = runtime->dma_area;

	avsync_update(s, substream, runtime->control->appl_ptr, count);

	dst = avsync_ring_pos(runtime, (snd_pcm_sframes_t)pos - s->shift);
	first = min(count, size - dst);
	if (copy_from_user(area + frames_to_bytes(runtime, dst), buf,
			   frames_to_bytes(runtime, first)))
		return -EFAULT;
	if (count > first &&
	    copy_from_user(area, buf + frames_to_bytes(runtime, first),
			   frames_to_bytes(runtime, count - first)))
		return -EFAULT;

	/* after a drop the DMA may run past the data until the next period
	 * interrupt tells ALSA; only the free part of the ring is silenced,
	 * a full ring holds the next data to play there
	 */
	if (s->shift > READ_ONCE(s->ptr_shift)) {
		snd_pcm_sframes_t queued = (snd_pcm_sframes_t)
			(runtime->control->appl_ptr + count -
			 runtime->status->hw_ptr) -
			(s->shift - READ_ONCE(s->ptr_shift));
		snd_pcm_sframes_t room = (snd_pcm_sframes_t)size - queued;

		if (room > 0)
			avsync_zero(runtime, (dst + count) % size,
				    min_t(snd_pcm_uframes_t, room,
					  min_t(snd_pcm_uframes_t, s->shift,
						runtime->period_size)));
	}

	return 0;
}
