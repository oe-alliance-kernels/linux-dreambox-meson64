/*
 * sound/soc/amlogic/auge/dreamaudio.c
 *
 * Audio decoder device for a userspace decoding service. The SoC has no
 * audio decoder: the player drives this device through the DVB audio API
 * like any audio decoder, PES filters with decoder output for it deliver
 * their transport stream packets here, and a decoding service reads both
 * and plays through the S/PDIF driver, which paces the output by the PTS.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/amlogic/media/frame_sync/timestamp.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/dreamaudio.h>
#include <media/dmxdev_audio.h>

#include "avsync.h"

#define TS_SIZE		188
/* a few seconds of a high bitrate audio stream */
#define FIFO_SIZE	(2 * 1024 * 1024)
#define READ_CHUNK	(TS_SIZE * 256)
/* a player stays this far ahead of the audio playing, in 90 kHz; more than
 * PLAYER_JUMP is a jump */
#define PLAYER_LEAD	(2 * 90000)
#define PLAYER_JUMP	(10 * 90000)
#define PLAYER_POLL_MS	20
/* one device per audio decoder of the demux API, DMX_PES_AUDIO0..3 */
#define DEVICES		4

struct dreamaudio {
	struct miscdevice misc;
	char name[16];
	char nodename[32];
	spinlock_t lock;
	struct kfifo fifo;
	wait_queue_head_t wait;
	struct dreamaudio_state state;
	struct file *service;
	/* files that control playback; the last one closed stops it */
	unsigned int players;
	/* a player writing PES (AUDIO_SOURCE_MEMORY) waits here for room */
	wait_queue_head_t room;
	u8 mem_cc;
	/* PTS of the first and the newest PES of a player since a clear */
	bool mem_valid;
	u32 mem_first_pts;
	u32 mem_last_pts;
	/* wakes a player held back by its lead */
	struct timer_list room_timer;
};

struct dreamaudio_file {
	struct dreamaudio *dev;
	struct mutex read_lock;
	u8 *chunk;
	u32 seen_seq;
	bool player;
	/* the PES read now is skipped */
	bool stale;
};

static struct dreamaudio dreamaudio[DEVICES];

/* caller holds dev->lock */
static void dreamaudio_changed(struct dreamaudio *dev)
{
	dev->state.seq++;
	wake_up_interruptible(&dev->wait);
}

/* caller holds dev->lock */
static void dreamaudio_clear(struct dreamaudio *dev)
{
	kfifo_reset(&dev->fifo);
	dev->mem_valid = false;
	wake_up_interruptible(&dev->room);
	dev->state.clear_seq++;
	dreamaudio_changed(dev);
}

static void dreamaudio_sink_start(void *priv, int index)
{
	struct dreamaudio *dev = &dreamaudio[index];
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	dreamaudio_clear(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
}

/* the PTS of a PES starting in the packet */
static bool dreamaudio_pes_pts(const u8 *ts, u32 *pts)
{
	const u8 *pes = ts + 4;

	if (ts[0] != 0x47 || !(ts[1] & 0x40) || !(ts[3] & 0x10))
		return false;
	if (ts[3] & 0x20)
		pes += 1 + ts[4];
	if (pes + 14 > ts + TS_SIZE || pes[0] || pes[1] || pes[2] != 1 ||
	    !(pes[7] & 0x80))
		return false;
	*pts = ((u32)(pes[9] & 0x06) << 29) | (pes[10] << 22) |
	       ((pes[11] & 0xfe) << 14) | (pes[12] << 7) | (pes[13] >> 1);
	return true;
}

static void dreamaudio_sink_write(void *priv, int index, const u8 *buf1,
				  size_t len1, const u8 *buf2, size_t len2)
{
	struct dreamaudio *dev = &dreamaudio[index];
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	if (!dev->service) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}
	if (kfifo_avail(&dev->fifo) < len1 + len2) {
		dev->state.overflows += (len1 + len2) / TS_SIZE;
	} else {
		kfifo_in(&dev->fifo, buf1, len1);
		if (len2)
			kfifo_in(&dev->fifo, buf2, len2);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	wake_up_interruptible(&dev->wait);
}

static void dreamaudio_sink_stop(void *priv, int index)
{
}

static const struct dmxdev_audio_sink dreamaudio_sink = {
	.start = dreamaudio_sink_start,
	.write = dreamaudio_sink_write,
	.stop = dreamaudio_sink_stop,
};

static int dreamaudio_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct dreamaudio_file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	f->dev = container_of(misc, struct dreamaudio, misc);
	mutex_init(&f->read_lock);
	file->private_data = f;

	return nonseekable_open(inode, file);
}

static int dreamaudio_release(struct inode *inode, struct file *file)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->service == file) {
		dev->service = NULL;
		kfifo_reset(&dev->fifo);
		wake_up_interruptible(&dev->room);
	}
	if (f->player && !--dev->players &&
	    (dev->state.play_state != AUDIO_STOPPED ||
	     dev->state.source != AUDIO_SOURCE_DEMUX)) {
		dev->state.play_state = AUDIO_STOPPED;
		dev->state.source = AUDIO_SOURCE_DEMUX;
		kfifo_reset(&dev->fifo);
		wake_up_interruptible(&dev->room);
		dreamaudio_changed(dev);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
	kfree(f->chunk);
	kfree(f);

	return 0;
}

/*
 * A played recording pushes ahead after a jump, and the PCR runs ahead with
 * it; the audio from the start of the jump would play seconds behind the
 * picture. Skip it a whole PES at a time.
 */
static unsigned int dreamaudio_skip_stale(struct dreamaudio_file *f,
					  unsigned int n)
{
	unsigned int pos, out = 0, skipped = 0;
	u32 pts;

	for (pos = 0; pos + TS_SIZE <= n; pos += TS_SIZE) {
		if (dreamaudio_pes_pts(f->chunk + pos, &pts))
			f->stale = aml_avsync_stale(pts);
		if (f->stale) {
			skipped++;
			continue;
		}
		if (out != pos)
			memmove(f->chunk + out, f->chunk + pos, TS_SIZE);
		out += TS_SIZE;
	}
	if (skipped)
		pr_info_ratelimited("dreamaudio: skipped %u late packets\n",
				    skipped);

	return out;
}

static ssize_t dreamaudio_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	unsigned long flags;
	unsigned int n;
	bool sync;
	ssize_t ret;

	if (dev->service != file)
		return -EPERM;
	count = rounddown(min_t(size_t, count, READ_CHUNK), TS_SIZE);
	if (!count)
		return -EINVAL;

	mutex_lock(&f->read_lock);
	for (;;) {
		spin_lock_irqsave(&dev->lock, flags);
		n = kfifo_out(&dev->fifo, f->chunk, count);
		sync = dev->state.av_sync &&
		       dev->state.source == AUDIO_SOURCE_DEMUX;
		spin_unlock_irqrestore(&dev->lock, flags);
		if (n && sync)
			n = dreamaudio_skip_stale(f, n);
		else
			f->stale = false;
		if (n)
			break;
		if (sync && !kfifo_is_empty(&dev->fifo))
			continue;
		if (file->f_flags & O_NONBLOCK) {
			mutex_unlock(&f->read_lock);
			return -EAGAIN;
		}
		ret = wait_event_interruptible(dev->wait,
			!kfifo_is_empty(&dev->fifo) ||
			READ_ONCE(dev->state.seq) != f->seen_seq);
		if (ret) {
			mutex_unlock(&f->read_lock);
			return ret;
		}
		/* a state change ends a blocking read */
		if (READ_ONCE(dev->state.seq) != f->seen_seq &&
		    kfifo_is_empty(&dev->fifo)) {
			mutex_unlock(&f->read_lock);
			return -EAGAIN;
		}
	}
	ret = copy_to_user(buf, f->chunk, n) ? -EFAULT : n;
	mutex_unlock(&f->read_lock);
	wake_up_interruptible(&dev->room);

	return ret;
}

static void dreamaudio_room_timer(unsigned long data)
{
	struct dreamaudio *dev = (struct dreamaudio *)data;

	wake_up_interruptible(&dev->room);
}

/*
 * A player writes as fast as the buffer takes it; at a low bitrate that is
 * a minute of audio, which a flush throws away and the position of the sink
 * runs ahead by. Hold it PLAYER_LEAD ahead of the audio playing. Before the
 * audio starts, the system clock the video of a player runs stands in;
 * while either is still at the position before a flush, the first PES
 * since. Without both (audio only to Bluetooth) the buffer alone limits it.
 */
static bool dreamaudio_ahead(struct dreamaudio *dev)
{
	unsigned long flags;
	u32 first, last, ref;
	u64 pts;
	s32 lead;
	bool valid;

	spin_lock_irqsave(&dev->lock, flags);
	valid = dev->mem_valid && dev->service &&
		dev->state.source == AUDIO_SOURCE_MEMORY &&
		dev->state.play_state != AUDIO_STOPPED;
	first = dev->mem_first_pts;
	last = dev->mem_last_pts;
	spin_unlock_irqrestore(&dev->lock, flags);
	if (!valid)
		return false;
	if (!aml_spdif_get_pts(&pts))
		ref = (u32)pts;
	else if (timestamp_pcrscr_enable_state() && timestamp_pcrscr_get())
		ref = timestamp_pcrscr_get();
	else
		return false;
	/* still at the position before a flush */
	if (abs((s32)(last - ref)) >= PLAYER_JUMP)
		ref = first;
	lead = (s32)(last - ref);

	return lead > PLAYER_LEAD && lead < PLAYER_JUMP;
}

/* caller holds dev->lock: the PTS of a PES a player starts */
static void dreamaudio_player_pts(struct dreamaudio *dev, const u8 *pes)
{
	u32 pts;

	if (!(pes[7] & 0x80))
		return;
	pts = ((u32)(pes[9] & 0x06) << 29) | (pes[10] << 22) |
	      ((pes[11] & 0xfe) << 14) | (pes[12] << 7) | (pes[13] >> 1);
	if (!dev->mem_valid)
		dev->mem_first_pts = pts;
	dev->mem_last_pts = pts;
	dev->mem_valid = true;
}

/* PES of a player (AUDIO_SOURCE_MEMORY), in TS packets as the demux hands
 * them to the service; a write that starts with a start code starts a PES.
 * Without a service the data is dropped, a full buffer blocks the writer
 * unless it opened the device non-blocking.
 */
static ssize_t dreamaudio_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	u8 ts[TS_SIZE];
	unsigned long flags;
	size_t done = 0;
	bool start;
	u8 head[3];
	u8 pes[14];
	int ret;

	if (dev->service == file)
		return -EPERM;
	if (!count)
		return 0;
	start = count >= 3 && !copy_from_user(head, buf, 3) &&
		!head[0] && !head[1] && head[2] == 1;

	/* a PES goes in whole: hold it back before its first byte */
	while (dreamaudio_ahead(dev)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (msleep_interruptible(PLAYER_POLL_MS))
			return -ERESTARTSYS;
	}
	if (start && count >= 14 && !copy_from_user(pes, buf, 14)) {
		spin_lock_irqsave(&dev->lock, flags);
		dreamaudio_player_pts(dev, pes);
		spin_unlock_irqrestore(&dev->lock, flags);
	}

	while (done < count) {
		size_t len = min_t(size_t, count - done, TS_SIZE - 4);
		size_t stuff = TS_SIZE - 4 - len;

		if ((file->f_flags & O_NONBLOCK) && READ_ONCE(dev->service) &&
		    kfifo_avail(&dev->fifo) < TS_SIZE)
			return done ? done : -EAGAIN;
		ret = wait_event_interruptible(dev->room,
			!READ_ONCE(dev->service) ||
			kfifo_avail(&dev->fifo) >= TS_SIZE);
		if (ret)
			return done ? done : ret;
		if (!READ_ONCE(dev->service))
			return count;

		ts[0] = 0x47;
		ts[1] = (start && !done ? 0x40 : 0) | 0x01;
		ts[2] = 0x00;
		if (stuff) {
			/* the adaptation field fills a short packet */
			ts[4] = stuff - 1;
			if (stuff > 1) {
				ts[5] = 0;
				memset(ts + 6, 0xff, stuff - 2);
			}
		}
		if (copy_from_user(ts + 4 + stuff, buf + done, len))
			return done ? done : -EFAULT;

		spin_lock_irqsave(&dev->lock, flags);
		if (kfifo_avail(&dev->fifo) < TS_SIZE) {
			/* the demux filled it meanwhile */
			spin_unlock_irqrestore(&dev->lock, flags);
			continue;
		}
		ts[3] = (stuff ? 0x30 : 0x10) | (dev->mem_cc++ & 0x0f);
		kfifo_in(&dev->fifo, ts, TS_SIZE);
		spin_unlock_irqrestore(&dev->lock, flags);
		wake_up_interruptible(&dev->wait);
		done += len;
	}

	return done;
}

static unsigned int dreamaudio_poll(struct file *file, poll_table *wait)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	unsigned int mask = 0;

	if (dev->service != file) {
		/* a player waits for room, and for its lead to shrink */
		poll_wait(file, &dev->room, wait);
		if (dreamaudio_ahead(dev))
			mod_timer(&dev->room_timer, jiffies +
				  msecs_to_jiffies(PLAYER_POLL_MS));
		else if (!READ_ONCE(dev->service) ||
			 kfifo_avail(&dev->fifo) >= TS_SIZE)
			mask |= POLLOUT | POLLWRNORM;
		return mask;
	}
	poll_wait(file, &dev->wait, wait);
	if (!kfifo_is_empty(&dev->fifo))
		mask |= POLLIN | POLLRDNORM;
	if (READ_ONCE(dev->state.seq) != f->seen_seq)
		mask |= POLLPRI;

	return mask;
}

static const struct file_operations dreamaudio_delay_fops;

/* /proc/stb/audio belongs to the demux driver, a module loaded later */
static void dreamaudio_add_proc(void)
{
	static DEFINE_MUTEX(lock);
	static bool done;

	mutex_lock(&lock);
	if (!done &&
	    proc_create_data("stb/audio/audio_delay_pcm", 0644, NULL,
			     &dreamaudio_delay_fops, &aml_avsync_delay_pcm)) {
		proc_create_data("stb/audio/audio_delay_bitstream", 0644, NULL,
				 &dreamaudio_delay_fops, &aml_avsync_delay_raw);
		done = true;
	}
	mutex_unlock(&lock);
}

static int dreamaudio_service(struct file *file)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	unsigned long flags;
	int ret = 0;

	dreamaudio_add_proc();
	if (!f->chunk) {
		f->chunk = kmalloc(READ_CHUNK, GFP_KERNEL);
		if (!f->chunk)
			return -ENOMEM;
	}
	spin_lock_irqsave(&dev->lock, flags);
	if (dev->service && dev->service != file)
		ret = -EBUSY;
	else
		dev->service = file;
	spin_unlock_irqrestore(&dev->lock, flags);

	return ret;
}

static long dreamaudio_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct dreamaudio_file *f = file->private_data;
	struct dreamaudio *dev = f->dev;
	struct dreamaudio_state *st = &dev->state;
	void __user *uarg = (void __user *)arg;
	unsigned long flags;
	struct audio_status status;
	struct dreamaudio_state copy;
	u64 pts;
	int hold = -1;
	int ret = 0;

	switch (cmd) {
	case DREAMAUDIO_SERVICE:
		return dreamaudio_service(file);
	case DREAMAUDIO_GET_STATE:
		spin_lock_irqsave(&dev->lock, flags);
		copy = *st;
		spin_unlock_irqrestore(&dev->lock, flags);
		f->seen_seq = copy.seq;
		return copy_to_user(uarg, &copy, sizeof(copy)) ? -EFAULT : 0;
	case AUDIO_GET_PTS:
		ret = aml_spdif_get_pts(&pts);
		if (ret)
			return ret;
		return copy_to_user(uarg, &pts, sizeof(pts)) ? -EFAULT : 0;
	case AUDIO_GET_STATUS:
		memset(&status, 0, sizeof(status));
		spin_lock_irqsave(&dev->lock, flags);
		status.AV_sync_state = st->av_sync;
		status.mute_state = st->mute;
		status.play_state = st->play_state;
		status.stream_source = st->source;
		status.channel_select = st->channel;
		status.bypass_mode = st->bypass;
		spin_unlock_irqrestore(&dev->lock, flags);
		return copy_to_user(uarg, &status, sizeof(status)) ? -EFAULT : 0;
	case AUDIO_GET_CAPABILITIES:
		return put_user(AUDIO_CAP_LPCM | AUDIO_CAP_MP1 | AUDIO_CAP_MP2 |
				AUDIO_CAP_MP3 | AUDIO_CAP_AAC | AUDIO_CAP_AC3 |
				AUDIO_CAP_DTS, (unsigned int __user *)uarg);
	}

	spin_lock_irqsave(&dev->lock, flags);
	if (!f->player) {
		f->player = true;
		dev->players++;
	}
	switch (cmd) {
	case AUDIO_STOP:
		st->play_state = AUDIO_STOPPED;
		kfifo_reset(&dev->fifo);
		dev->mem_valid = false;
		wake_up_interruptible(&dev->room);
		break;
	case AUDIO_PLAY:
		st->play_state = AUDIO_PLAYING;
		break;
	case AUDIO_PAUSE:
		if (st->play_state == AUDIO_PLAYING) {
			st->play_state = AUDIO_PAUSED;
			hold = st->source == AUDIO_SOURCE_MEMORY;
		}
		break;
	case AUDIO_CONTINUE:
		if (st->play_state == AUDIO_PAUSED) {
			st->play_state = AUDIO_PLAYING;
			if (st->source == AUDIO_SOURCE_MEMORY)
				hold = 0;
		}
		break;
	case AUDIO_SELECT_SOURCE:
		st->source = arg;
		dev->mem_valid = false;
		break;
	case AUDIO_SET_MUTE:
		st->mute = !!arg;
		break;
	case AUDIO_SET_AV_SYNC:
		st->av_sync = !!arg;
		break;
	case AUDIO_SET_BYPASS_MODE:
		st->bypass = arg;
		break;
	case AUDIO_CHANNEL_SELECT:
		st->channel = arg;
		break;
	case AUDIO_CLEAR_BUFFER:
		dreamaudio_clear(dev);
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	case AUDIO_SET_ID:
	case AUDIO_SET_MIXER:
	case AUDIO_SET_STREAMTYPE:
	case AUDIO_SET_EXT_ID:
	case AUDIO_SET_ATTRIBUTES:
	case AUDIO_SET_KARAOKE:
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0;
	default:
		ret = -ENOTTY;
		break;
	}
	if (!ret)
		dreamaudio_changed(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
	/* a player's video decoder freezes without telling tsync; hold the
	 * system clock the audio follows with the pause
	 */
	if (hold >= 0)
		aml_avsync_hold_clock(hold);

	return ret;
}

/* /proc/stb/audio/audio_delay_pcm and audio_delay_bitstream, hex 90 kHz */
static int dreamaudio_delay_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%x\n", *(int *)m->private);
	return 0;
}

static int dreamaudio_delay_open(struct inode *inode, struct file *file)
{
	return single_open(file, dreamaudio_delay_show, PDE_DATA(inode));
}

static ssize_t dreamaudio_delay_write(struct file *file,
				      const char __user *buf, size_t count,
				      loff_t *ppos)
{
	int *delay = ((struct seq_file *)file->private_data)->private;
	char s[16];
	unsigned int v;

	if (count >= sizeof(s))
		return -EINVAL;
	if (copy_from_user(s, buf, count))
		return -EFAULT;
	s[count] = 0;
	if (kstrtouint(strim(s), 16, &v))
		return -EINVAL;
	WRITE_ONCE(*delay, (int)v);

	return count;
}

static const struct file_operations dreamaudio_delay_fops = {
	.owner = THIS_MODULE,
	.open = dreamaudio_delay_open,
	.read = seq_read,
	.write = dreamaudio_delay_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations dreamaudio_fops = {
	.owner = THIS_MODULE,
	.open = dreamaudio_open,
	.release = dreamaudio_release,
	.read = dreamaudio_read,
	.write = dreamaudio_write,
	.poll = dreamaudio_poll,
	.unlocked_ioctl = dreamaudio_ioctl,
	.compat_ioctl = dreamaudio_ioctl,
	.llseek = no_llseek,
};

static int __init dreamaudio_add(struct dreamaudio *dev, int index)
{
	void *buf;
	int ret;

	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->wait);
	init_waitqueue_head(&dev->room);
	setup_timer(&dev->room_timer, dreamaudio_room_timer, (unsigned long)dev);
	dev->state.play_state = AUDIO_STOPPED;
	dev->state.source = AUDIO_SOURCE_DEMUX;
	dev->state.av_sync = 1;

	buf = vmalloc(FIFO_SIZE);
	if (!buf)
		return -ENOMEM;
	ret = kfifo_init(&dev->fifo, buf, FIFO_SIZE);
	if (ret)
		goto err;

	snprintf(dev->name, sizeof(dev->name), "dreamaudio%d", index);
	snprintf(dev->nodename, sizeof(dev->nodename),
		 "dvb/adapter0/dreamaudio%d", index);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = dev->name;
	dev->misc.nodename = dev->nodename;
	dev->misc.mode = 0660;
	dev->misc.fops = &dreamaudio_fops;
	ret = misc_register(&dev->misc);
	if (ret)
		goto err;

	return 0;

err:
	vfree(buf);
	return ret;
}

static int __init dreamaudio_init(void)
{
	int i, ret;

	for (i = 0; i < DEVICES; i++) {
		ret = dreamaudio_add(&dreamaudio[i], i);
		if (ret)
			return ret;
	}

	return dmxdev_audio_sink_register(&dreamaudio_sink);
}
late_initcall(dreamaudio_init);
