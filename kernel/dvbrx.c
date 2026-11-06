// SPDX-License-Identifier: GPL-2.0
/*
 * dvbrx: DVB adapters backed by a userspace receiver (dvbrx).
 *
 * Each instance registers a DVB-S/S2 adapter with a frontend, a software
 * demux, dvr and net devices, and a control device /dev/dvbrxN through
 * which the daemon receives tuning and LNB control requests, feeds the
 * transport stream, reports the lock status, and names the frontend after
 * the SDR with the symbol rates it can receive (see dvbrx_ioctl.h).
 */
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <media/dmxdev.h>
#include <media/dvb_demux.h>
#include <media/dvb_frontend.h>
#include <media/dvb_net.h>
#include <media/dvbdev.h>

#include "dvbrx_ioctl.h"

/* the tuning parameters must fit in the event record */
static_assert(sizeof(struct dvbrx_tune) <= sizeof(((struct dvbrx_event *)0)->pad));

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static unsigned int adapters = 1;
module_param(adapters, uint, 0444);
MODULE_PARM_DESC(adapters, "number of adapters (1-8)");

static unsigned int lnb_timeout_ms = 2000;
module_param(lnb_timeout_ms, uint, 0644);
MODULE_PARM_DESC(lnb_timeout_ms, "time allowed to the daemon for an LNB command");

#define MAX_ADAPTERS 8
#define EVENT_FIFO 64
#define WRITE_CHUNK (188 * 348) /* TS bytes copied from userspace at once */

struct dvbrx {
	int index;
	char name[16];
	struct miscdevice misc;

	struct dvb_adapter adapter;
	struct dvb_frontend fe;
	struct dvb_demux demux;
	struct dmxdev dmxdev;
	struct dvb_net net;
	bool adapter_ok, fe_ok, demux_ok, dmxdev_ok, net_ok;

	/* events to the daemon */
	spinlock_t lock;
	DECLARE_KFIFO(events, struct dvbrx_event, EVENT_FIFO);
	wait_queue_head_t event_wq;
	u32 next_seq;

	/* replies from the daemon */
	wait_queue_head_t reply_wq;
	u32 reply_seq;
	s32 reply_result;
	bool replied;

	struct mutex daemon_lock; /* one daemon at a time */
	bool daemon;

	enum fe_status status;
	struct mutex write_lock; /* wbuf and the demux feed */
	u8 *wbuf;
};

static struct dvbrx *instances[MAX_ADAPTERS];

/* ---- events ----------------------------------------------------------------- */

static u32 push_event(struct dvbrx *s, struct dvbrx_event *ev)
{
	unsigned long flags;
	u32 seq;

	spin_lock_irqsave(&s->lock, flags);
	seq = ev->seq = ++s->next_seq;
	if (kfifo_is_full(&s->events)) {
		struct dvbrx_event drop;

		/* the daemon is not reading: keep the newest requests */
		if (!kfifo_get(&s->events, &drop))
			pr_warn_ratelimited("%s: event queue inconsistent\n", s->name);
	}
	kfifo_put(&s->events, *ev);
	spin_unlock_irqrestore(&s->lock, flags);
	wake_up_interruptible(&s->event_wq);
	return seq;
}

/* Sends an event and waits for the daemon to carry it out. */
static int sync_event(struct dvbrx *s, struct dvbrx_event *ev)
{
	long left;
	u32 seq;

	if (!READ_ONCE(s->daemon)) {
		pr_info_ratelimited("%s: no daemon, LNB command ignored\n", s->name);
		return 0;
	}
	WRITE_ONCE(s->replied, false);
	seq = push_event(s, ev);
	left = wait_event_interruptible_timeout(
		s->reply_wq, READ_ONCE(s->replied) && READ_ONCE(s->reply_seq) == seq,
		msecs_to_jiffies(lnb_timeout_ms));
	if (left < 0)
		return left;
	if (left == 0) {
		pr_warn_ratelimited("%s: daemon did not answer event %u\n", s->name,
				    ev->type);
		return -ETIMEDOUT;
	}
	return READ_ONCE(s->reply_result);
}

/* ---- frontend --------------------------------------------------------------- */

static struct dvbrx *fe_to_dvbrx(struct dvb_frontend *fe)
{
	return container_of(fe, struct dvbrx, fe);
}

static enum dvbfe_algo dvbrx_get_algo(struct dvb_frontend *fe)
{
	return DVBFE_ALGO_HW;
}

static int dvbrx_tune(struct dvb_frontend *fe, bool re_tune, unsigned int mode_flags,
		      unsigned int *delay, enum fe_status *status)
{
	struct dvbrx *s = fe_to_dvbrx(fe);
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	if (re_tune) {
		struct dvbrx_event ev = { .type = DVBRX_EV_TUNE };

		ev.tune.delivery_system = c->delivery_system;
		/* the DVB API gives satellite frequencies in kHz, others in Hz */
		ev.tune.frequency_khz = c->delivery_system == SYS_DVBS ||
						c->delivery_system == SYS_DVBS2 ?
					       c->frequency :
					       c->frequency / 1000;
		ev.tune.symbol_rate = c->symbol_rate;
		ev.tune.modulation = c->modulation;
		ev.tune.fec_inner = c->delivery_system == SYS_DVBT ? c->code_rate_HP :
								      c->fec_inner;
		ev.tune.rolloff = c->rolloff;
		ev.tune.pilot = c->pilot;
		ev.tune.inversion = c->inversion;
		ev.tune.stream_id = c->stream_id == NO_STREAM_ID_FILTER ? -1 : (s32)c->stream_id;
		ev.tune.scrambling_index = c->scrambling_sequence_index;
		ev.tune.bandwidth_hz = c->bandwidth_hz;
		ev.tune.transmission_mode = c->transmission_mode;
		ev.tune.guard_interval = c->guard_interval;
		ev.tune.hierarchy = c->hierarchy;
		ev.tune.code_rate_lp = c->code_rate_LP;
		WRITE_ONCE(s->status, 0);
		push_event(s, &ev);
	}
	*delay = HZ / 10;
	*status = READ_ONCE(s->status);
	return 0;
}

static int dvbrx_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	*status = READ_ONCE(fe_to_dvbrx(fe)->status);
	return 0;
}

static int dvbrx_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	/* legacy API: 0.1 dB */
	*snr = c->cnr.stat[0].scale == FE_SCALE_DECIBEL ?
		       clamp_val(c->cnr.stat[0].svalue / 100, 0, 0xffff) :
		       0;
	return 0;
}

static int dvbrx_set_voltage(struct dvb_frontend *fe, enum fe_sec_voltage voltage)
{
	struct dvbrx_event ev = { .type = DVBRX_EV_VOLTAGE, .voltage = voltage };

	return sync_event(fe_to_dvbrx(fe), &ev);
}

static int dvbrx_set_tone(struct dvb_frontend *fe, enum fe_sec_tone_mode tone)
{
	struct dvbrx_event ev = { .type = DVBRX_EV_TONE, .tone = tone };

	return sync_event(fe_to_dvbrx(fe), &ev);
}

static int dvbrx_send_burst(struct dvb_frontend *fe, enum fe_sec_mini_cmd burst)
{
	struct dvbrx_event ev = { .type = DVBRX_EV_BURST, .burst = burst };

	return sync_event(fe_to_dvbrx(fe), &ev);
}

static int dvbrx_send_diseqc(struct dvb_frontend *fe, struct dvb_diseqc_master_cmd *cmd)
{
	struct dvbrx_event ev = { .type = DVBRX_EV_DISEQC };

	if (cmd->msg_len < 3 || cmd->msg_len > 6)
		return -EINVAL;
	ev.diseqc.len = cmd->msg_len;
	memcpy(ev.diseqc.msg, cmd->msg, cmd->msg_len);
	return sync_event(fe_to_dvbrx(fe), &ev);
}

static int dvbrx_sleep(struct dvb_frontend *fe)
{
	struct dvbrx_event ev = { .type = DVBRX_EV_SLEEP };

	push_event(fe_to_dvbrx(fe), &ev);
	return 0;
}

static const struct dvb_frontend_ops dvbrx_fe_ops = {
	.delsys = { SYS_DVBS, SYS_DVBS2, SYS_DVBC_ANNEX_A, SYS_DVBT, SYS_DVBT2 },
	.info = {
		.name = "dvbrx SDR DVB-S/S2/S2X/C/T/T2",
		/* satellite IF, cable and terrestrial bands, within the SDR's
		 * range */
		.frequency_min_hz = 47 * MHz,
		.frequency_max_hz = 3000 * MHz,
		.frequency_stepsize_hz = 0,
		.symbol_rate_min = 100000,
		.symbol_rate_max = 60000000,
		.caps = FE_CAN_INVERSION_AUTO | FE_CAN_FEC_AUTO | FE_CAN_QPSK |
			FE_CAN_QAM_16 | FE_CAN_QAM_32 | FE_CAN_QAM_64 | FE_CAN_QAM_128 |
			FE_CAN_QAM_256 | FE_CAN_QAM_AUTO | FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO | FE_CAN_HIERARCHY_AUTO |
			FE_CAN_2G_MODULATION | FE_CAN_MULTISTREAM | FE_CAN_RECOVER,
	},
	/*
	 * No .release: the frontend is embedded in struct dvbrx, and
	 * dvb_frontend_detach() would drop a module reference for it as if it
	 * had been obtained through dvb_attach().
	 */
	.sleep = dvbrx_sleep,
	.get_frontend_algo = dvbrx_get_algo,
	.tune = dvbrx_tune,
	.read_status = dvbrx_read_status,
	.read_snr = dvbrx_read_snr,
	.set_voltage = dvbrx_set_voltage,
	.set_tone = dvbrx_set_tone,
	.diseqc_send_burst = dvbrx_send_burst,
	.diseqc_send_master_cmd = dvbrx_send_diseqc,
};

/* ---- demux ------------------------------------------------------------------ */

static int dvbrx_start_feed(struct dvb_demux_feed *feed)
{
	return 0; /* the daemon always sends the whole transport stream */
}

static int dvbrx_stop_feed(struct dvb_demux_feed *feed)
{
	return 0;
}

/* ---- control device ------------------------------------------------------- */

static struct dvbrx *file_to_dvbrx(struct file *f)
{
	return container_of(f->private_data, struct dvbrx, misc);
}

static int ctl_open(struct inode *inode, struct file *f)
{
	struct dvbrx *s = file_to_dvbrx(f);

	mutex_lock(&s->daemon_lock);
	if (s->daemon) {
		mutex_unlock(&s->daemon_lock);
		return -EBUSY;
	}
	WRITE_ONCE(s->daemon, true);
	mutex_unlock(&s->daemon_lock);
	return 0;
}

static int ctl_release(struct inode *inode, struct file *f)
{
	struct dvbrx *s = file_to_dvbrx(f);

	mutex_lock(&s->daemon_lock);
	WRITE_ONCE(s->daemon, false);
	WRITE_ONCE(s->status, 0);
	mutex_unlock(&s->daemon_lock);
	/* release any application waiting on an LNB command */
	WRITE_ONCE(s->reply_result, -EIO);
	WRITE_ONCE(s->reply_seq, s->next_seq);
	WRITE_ONCE(s->replied, true);
	wake_up_interruptible(&s->reply_wq);
	return 0;
}

static ssize_t ctl_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct dvbrx *s = file_to_dvbrx(f);
	struct dvbrx_event ev;
	unsigned long flags;
	int got;

	if (len < sizeof(ev))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&s->lock, flags);
		got = kfifo_get(&s->events, &ev);
		spin_unlock_irqrestore(&s->lock, flags);
		if (got)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(s->event_wq, !kfifo_is_empty(&s->events)))
			return -ERESTARTSYS;
	}
	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static ssize_t ctl_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	struct dvbrx *s = file_to_dvbrx(f);
	size_t done = 0;

	mutex_lock(&s->write_lock);
	while (done < len) {
		size_t n = min_t(size_t, len - done, WRITE_CHUNK);

		if (copy_from_user(s->wbuf, buf + done, n)) {
			mutex_unlock(&s->write_lock);
			return done ? done : -EFAULT;
		}
		dvb_dmx_swfilter(&s->demux, s->wbuf, n);
		done += n;
	}
	mutex_unlock(&s->write_lock);
	return done;
}

static __poll_t ctl_poll(struct file *f, poll_table *wait)
{
	struct dvbrx *s = file_to_dvbrx(f);

	poll_wait(f, &s->event_wq, wait);
	return (kfifo_is_empty(&s->events) ? 0 : EPOLLIN | EPOLLRDNORM) | EPOLLOUT |
	       EPOLLWRNORM;
}

static long ctl_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct dvbrx *s = file_to_dvbrx(f);

	switch (cmd) {
	case DVBRX_IOC_STATUS: {
		struct dtv_frontend_properties *c = &s->fe.dtv_property_cache;
		struct dvbrx_status st;

		if (copy_from_user(&st, (void __user *)arg, sizeof(st)))
			return -EFAULT;
		WRITE_ONCE(s->status, st.status);
		c->cnr.len = 1;
		c->cnr.stat[0].scale = st.status & FE_HAS_CARRIER ? FE_SCALE_DECIBEL :
								    FE_SCALE_NOT_AVAILABLE;
		c->cnr.stat[0].svalue = st.cnr;
		c->strength.len = 1;
		c->strength.stat[0].scale =
			st.strength == INT_MIN ? FE_SCALE_NOT_AVAILABLE : FE_SCALE_DECIBEL;
		c->strength.stat[0].svalue = st.strength;
		c->post_bit_error.len = c->post_bit_count.len = 1;
		c->post_bit_error.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_error.stat[0].uvalue = st.bit_errors;
		c->post_bit_count.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_count.stat[0].uvalue = st.bit_count;
		c->block_error.len = c->block_count.len = 1;
		c->block_error.stat[0].scale = FE_SCALE_COUNTER;
		c->block_error.stat[0].uvalue = st.block_errors;
		c->block_count.stat[0].scale = FE_SCALE_COUNTER;
		c->block_count.stat[0].uvalue = st.block_count;
		return 0;
	}
	case DVBRX_IOC_REPLY: {
		struct dvbrx_reply r;

		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;
		WRITE_ONCE(s->reply_result, r.result);
		WRITE_ONCE(s->reply_seq, r.seq);
		WRITE_ONCE(s->replied, true);
		wake_up_interruptible(&s->reply_wq);
		return 0;
	}
	case DVBRX_IOC_NAME: {
		struct dvbrx_name n;

		if (copy_from_user(&n, (void __user *)arg, sizeof(n)))
			return -EFAULT;
		static_assert(sizeof(n.name) == sizeof(s->fe.ops.info.name));
		n.name[sizeof(n.name) - 1] = 0;
		if (!n.name[0])
			return -EINVAL;
		strscpy(s->fe.ops.info.name, n.name, sizeof(s->fe.ops.info.name));
		return 0;
	}
	case DVBRX_IOC_LIMITS: {
		struct dvbrx_limits l;
		struct dvb_frontend_internal_info *info = &s->fe.ops.info;

		if (copy_from_user(&l, (void __user *)arg, sizeof(l)))
			return -EFAULT;
		if (l.symbol_rate_min && l.symbol_rate_max && l.symbol_rate_min > l.symbol_rate_max)
			return -EINVAL;
		if (l.symbol_rate_min)
			WRITE_ONCE(info->symbol_rate_min, l.symbol_rate_min);
		if (l.symbol_rate_max)
			WRITE_ONCE(info->symbol_rate_max, l.symbol_rate_max);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ctl_fops = {
	.owner = THIS_MODULE,
	.open = ctl_open,
	.release = ctl_release,
	.read = ctl_read,
	.write = ctl_write,
	.poll = ctl_poll,
	.unlocked_ioctl = ctl_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

/* ---- setup ------------------------------------------------------------------ */

static void dvbrx_destroy(struct dvbrx *s)
{
	if (s->net_ok)
		dvb_net_release(&s->net);
	if (s->fe_ok) {
		dvb_unregister_frontend(&s->fe);
		dvb_frontend_detach(&s->fe);
	}
	if (s->dmxdev_ok)
		dvb_dmxdev_release(&s->dmxdev);
	if (s->demux_ok)
		dvb_dmx_release(&s->demux);
	if (s->adapter_ok)
		dvb_unregister_adapter(&s->adapter);
	if (s->misc.this_device)
		misc_deregister(&s->misc);
	kfree(s->wbuf);
	kfree(s);
}

static int dvbrx_create(int index, struct dvbrx **out)
{
	struct dvbrx *s = kzalloc(sizeof(*s), GFP_KERNEL);
	int ret;

	if (!s)
		return -ENOMEM;
	s->index = index;
	snprintf(s->name, sizeof(s->name), "dvbrx%d", index);
	spin_lock_init(&s->lock);
	INIT_KFIFO(s->events);
	init_waitqueue_head(&s->event_wq);
	init_waitqueue_head(&s->reply_wq);
	mutex_init(&s->daemon_lock);
	mutex_init(&s->write_lock);
	s->wbuf = kmalloc(WRITE_CHUNK, GFP_KERNEL);
	if (!s->wbuf) {
		ret = -ENOMEM;
		goto fail;
	}

	s->misc.minor = MISC_DYNAMIC_MINOR;
	s->misc.name = s->name;
	s->misc.fops = &ctl_fops;
	s->misc.mode = 0660;
	ret = misc_register(&s->misc);
	if (ret) {
		s->misc.this_device = NULL;
		goto fail;
	}

	ret = dvb_register_adapter(&s->adapter, s->name, THIS_MODULE, s->misc.this_device,
				   adapter_nr);
	if (ret < 0)
		goto fail;
	s->adapter_ok = true;

	s->demux.dmx.capabilities = DMX_TS_FILTERING | DMX_SECTION_FILTERING;
	s->demux.priv = s;
	s->demux.filternum = 256;
	s->demux.feednum = 256;
	s->demux.start_feed = dvbrx_start_feed;
	s->demux.stop_feed = dvbrx_stop_feed;
	ret = dvb_dmx_init(&s->demux);
	if (ret)
		goto fail;
	s->demux_ok = true;

	s->dmxdev.filternum = 256;
	s->dmxdev.demux = &s->demux.dmx;
	s->dmxdev.capabilities = 0;
	ret = dvb_dmxdev_init(&s->dmxdev, &s->adapter);
	if (ret)
		goto fail;
	s->dmxdev_ok = true;

	memcpy(&s->fe.ops, &dvbrx_fe_ops, sizeof(dvbrx_fe_ops));
	s->fe.demodulator_priv = s;
	ret = dvb_register_frontend(&s->adapter, &s->fe);
	if (ret)
		goto fail;
	s->fe_ok = true;

	ret = dvb_net_init(&s->adapter, &s->net, &s->demux.dmx);
	if (ret)
		goto fail;
	s->net_ok = true;

	pr_info("%s: DVB adapter %d, control device /dev/%s\n", s->name, s->adapter.num,
		s->name);
	*out = s;
	return 0;
fail:
	dvbrx_destroy(s);
	return ret;
}

static int __init dvbrx_init(void)
{
	unsigned int i;
	int ret;

	if (adapters < 1 || adapters > MAX_ADAPTERS)
		return -EINVAL;
	for (i = 0; i < adapters; i++) {
		ret = dvbrx_create(i, &instances[i]);
		if (ret) {
			while (i--)
				dvbrx_destroy(instances[i]);
			return ret;
		}
	}
	return 0;
}

static void __exit dvbrx_exit(void)
{
	unsigned int i;

	for (i = 0; i < adapters; i++)
		if (instances[i])
			dvbrx_destroy(instances[i]);
}

module_init(dvbrx_init);
module_exit(dvbrx_exit);

MODULE_DESCRIPTION("DVB adapters backed by a userspace SDR receiver");
MODULE_LICENSE("GPL");
