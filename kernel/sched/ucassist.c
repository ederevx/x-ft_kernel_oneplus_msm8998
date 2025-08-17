// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/ucassist.c
 *
 * Copyright (C) 2024-2025, Edrick Vince Sinsuan
 *
 * This provides the kernel a way to configure and manage uclamp values at
 * init and during runtime for taskgroups and defined tasks.
 */
#define pr_fmt(fmt) "ucassist: %s: " fmt, __func__

#include <linux/sched.h>
#include <linux/sched-ucassist.h>
#ifdef CONFIG_FB
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/notifier.h>
#endif

#include "sched.h"

#define SCHED_CAPACITY_SCALE_PERC(perc) \
		((perc * SCHED_CAPACITY_SCALE) / 100)

#define DISPLAY_UCLAMP_MIN	SCHED_CAPACITY_SCALE_PERC(30)
#define GPU_UCLAMP_MIN		SCHED_CAPACITY_SCALE_PERC(20)

/* Disable UCLAMP restriction for 1 second after last input event */
#define INPUT_EVENT_TIMEOUT_MS 1000

#define ALL_UCLFLAGS	\
		(DISPLAY_UCLFLAG | GPU_UCLFLAG)

int cpu_uclamp_write_css(struct cgroup_subsys_state *css, char *buf,
					enum uclamp_id clamp_id);
int cpu_uclamp_ls_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 ls);
int cpu_uclamp_boosted_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 ls);

enum {
	ACTIVE_STATE = 0,
	INPUT_SLEEP_STATE,
#ifdef CONFIG_FB
	FB_SLEEP_STATE,
#endif
	NUM_STATES,
};

struct uclamp_data {
	char uclamp_max[3];
	char uclamp_min[3];
	bool latency_sensitive;
	bool boosted;
};

struct ucassist_css_struct {
	const char *name;
	struct cgroup_subsys_state *css;
	struct uclamp_data data;
};

struct ucassist_task_struct {
	const char target[TASK_COMM_LEN];
	unsigned int uclamp_max;
	unsigned int uclamp_min;
	unsigned int flags;
};

struct ucassist_task_list_struct {
	const struct ucassist_task_struct *uctdata;
	unsigned int uctdata_num;
	unsigned int flags;	
};

struct ucassist_sleep_struct {
	unsigned int uclamp_max;
	unsigned int uclamp_min;
	struct ucassist_css_struct *css_data;
	unsigned int css_num_data;
};

struct ucassist_struct {
	struct kthread_work update_work;
	struct kthread_worker worker;
	struct irq_work input_work;
	struct timer_list input_timer;
	unsigned long sleep_states;
	atomic_t input_pending;
};

bool ucassist_restrict_enabled __read_mostly = false;

static const struct ucassist_css_struct ucassist_css_data[] = {
	{
		.name = "top-app",
		.data = { "max", "20", 1, 0 },
	},
	{
		.name = "foreground",
		.data = { "max", "0", 1, 0 },
	},
	{
		.name = "background",
		.data = { "50", "0", 0, 0 },
	},
	{
		.name = "system-background",
		.data = { "50", "0", 0, 0 },
	},
	{
		.name = "dex2oat",
		.data = { "60", "0", 0, 0 },
	},
	{
		.name = "nnapi-hal",
		.data = { "max", "50", 0, 0 },
	},
	{
		.name = "camera-daemon",
		.data = { "max", "10", 1, 0 },
	},
};

static struct ucassist_css_struct ucassist_active_css_data[] = {
	{
		.name = "top-app",
		.data = { "max", "20", 1, 0 },
	},
	{
		.name = "foreground",
		.data = { "max", "0", 1, 0 },
	},
};

static struct ucassist_css_struct ucassist_input_sleep_css_data[] = {
	{
		.name = "top-app",
		.data = { "max", "0", 1, 0 },
	},
	{
		.name = "foreground",
		.data = { "max", "0", 0, 0 },
	},
};

#ifdef CONFIG_FB
static struct ucassist_css_struct ucassist_fb_sleep_css_data[] = {
	{
		.name = "background",
		.data = { "25", "0", 0, 0 },
	},
	{
		.name = "system-background",
		.data = { "25", "0", 0, 0 },
	},
};
#endif

static const struct ucassist_task_struct ucassist_display_task_data[] = {
	{
		.target = "composer",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
	},
};

static const struct ucassist_task_struct ucassist_gpu_task_data[] = {
	{
		.target = "Gralloc",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = GPU_UCLAMP_MIN,
	},
	{
		.target = "Render",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.flags = TRIGGER_UCFLAG,
	},
};

static const struct ucassist_task_struct ucassist_unfiltered_task_data[] = {
	{
		.target = "kgsl_worker",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = GPU_UCLAMP_MIN,
	},
	{
		.target = "mdss_fb",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
	},
	{
		.target = "surfaceflinger",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
	},
	{
		.target = "vsync_retire",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
	},
};

static const struct ucassist_task_list_struct ucassist_task_data_list[] = {
	{
		.uctdata = ucassist_display_task_data,
		.uctdata_num = ARRAY_SIZE(ucassist_display_task_data),
		.flags = DISPLAY_UCLFLAG,
	},
	{
		.uctdata = ucassist_gpu_task_data,
		.uctdata_num = ARRAY_SIZE(ucassist_gpu_task_data),
		.flags = GPU_UCLFLAG,		
	},
	{
		.uctdata = ucassist_unfiltered_task_data,
		.uctdata_num = ARRAY_SIZE(ucassist_unfiltered_task_data),
	},
};

static const struct ucassist_sleep_struct ucassist_sleep_data[] = {
	[ACTIVE_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = SCHED_CAPACITY_SCALE,
		.css_data = ucassist_active_css_data,
		.css_num_data = ARRAY_SIZE(ucassist_active_css_data),
	},
	[INPUT_SLEEP_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE_PERC(75),
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.css_data = ucassist_input_sleep_css_data,
		.css_num_data = ARRAY_SIZE(ucassist_input_sleep_css_data),
	},
#ifdef CONFIG_FB
	[FB_SLEEP_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE_PERC(50),
		.uclamp_min = 0,
		.css_data = ucassist_fb_sleep_css_data,
		.css_num_data = ARRAY_SIZE(ucassist_fb_sleep_css_data),
	},
#endif
};

static struct ucassist_struct ucassist = {
	.sleep_states = 0,
	.input_pending = ATOMIC_INIT(0),
};

static void ucassist_set_css_uclamp_data(struct cgroup_subsys_state *css,
				struct uclamp_data cdata)
{
	cpu_uclamp_write_css(css, cdata.uclamp_max, 
				UCLAMP_MAX);

	cpu_uclamp_write_css(css, cdata.uclamp_min, 
				UCLAMP_MIN);
	
	cpu_uclamp_ls_write_u64(css, NULL, cdata.latency_sensitive);

	cpu_uclamp_boosted_write_u64(css, NULL, cdata.boosted);
}

static void ucassist_sleep_set_css_data(struct cgroup_subsys_state *css);

int ucassist_init_cpu_values(struct cgroup_subsys_state *css)
{
	const struct ucassist_css_struct *ucs;
	int i;

	if (!css->cgroup->kn)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ucassist_css_data); i++) {
		ucs = &ucassist_css_data[i];

		if (strcmp(css->cgroup->kn->name, ucs->name))
			continue;

		pr_info("setting values for %s", ucs->name);
		ucassist_set_css_uclamp_data(css, ucs->data);
		break;
	}

	ucassist_sleep_set_css_data(css);
	return 0;
}

static void ucassist_input_trigger_timer(void);

static inline int 
ucassist_set_target_data(const struct ucassist_task_list_struct *ucl,
				const char *comm, unsigned int *min, 
				unsigned int *max, unsigned int flags)
{
	const struct ucassist_task_struct *uct;
	int i;

	for (i = 0; i < ucl->uctdata_num; i++) {
		uct = &ucl->uctdata[i];

		if (!strstr(comm, uct->target))
			continue;

		/* Trigger if the task and the caller has the trigger flag */
		if ((uct->flags & flags) & TRIGGER_UCFLAG)
			ucassist_input_trigger_timer();

		*min = uct->uclamp_min;
		*max = uct->uclamp_max;

		pr_warn_once("ucassist overrides task UCLAMPs\n");
		return 0;
	}

	return -EINVAL;
}

static inline bool ucassist_check_if_target(unsigned int target_flags, 
				unsigned int flags)
{
	/* Only consider UCLFLAGs */
	flags &= ALL_UCLFLAGS;
	target_flags &= ALL_UCLFLAGS;

	/* Return data to caller that matches a target list flag */
	if (flags && (target_flags & flags))
		return true;

	/* Return data if caller and target don't require a list flag */
	if (!flags && !target_flags)
		return true;

	return false;
}

int __ucassist_get_task_uclamp_data(const char *comm, 
				unsigned int *min, unsigned int *max,
				unsigned int flags)
{
	const struct ucassist_task_list_struct *ucl;
	int ret = -EINVAL, i;

	for (i = 0; i < ARRAY_SIZE(ucassist_task_data_list); i++) {
		ucl = &ucassist_task_data_list[i];

		if (!ucassist_check_if_target(ucl->flags, flags))
			continue;

		ret = ucassist_set_target_data(ucl, comm, min, max, flags);
		if (!ret)
			break;
	}

	return ret;
}

static void ucassist_sleep_set_css_data(struct cgroup_subsys_state *css)
{
	const struct ucassist_sleep_struct *us;
	struct ucassist_css_struct *ucs;
	int state, i;

	for (state = NUM_STATES - 1; state >= ACTIVE_STATE; state--) {
		us = &ucassist_sleep_data[state];
		for (i = 0; i < us->css_num_data; i++) {
			ucs = &us->css_data[i];

			if (strcmp(css->cgroup->kn->name, ucs->name))
				continue;

			ucs->css = css;
			break;
		}
	}
}

static void ucassist_update_fn(struct kthread_work *work)
{
	const struct ucassist_sleep_struct *us;
	struct ucassist_css_struct *ucs;
	unsigned long timeout;
	static int prev_state = ACTIVE_STATE;
	int state, i;

	del_timer(&ucassist.input_timer);

	/* Start from the deepest state towards the shallowest */
	for (state = NUM_STATES - 1; state > ACTIVE_STATE; state--) {
		if (test_bit(state, &ucassist.sleep_states))
			break;
	}

	if (state != prev_state) {
		prev_state = state;

		us = &ucassist_sleep_data[state];
		ucassist_sched_uclamp_set(us->uclamp_min, us->uclamp_max);

		for (i = 0; i < us->css_num_data; i++) {
			ucs = &us->css_data[i];
			if (ucs && ucs->css)
				ucassist_set_css_uclamp_data(ucs->css, ucs->data);
		}

		pr_info("sleep_state = %d\n", state);
	}

	if (state == ACTIVE_STATE) {
		timeout = jiffies + msecs_to_jiffies(INPUT_EVENT_TIMEOUT_MS);
		mod_timer(&ucassist.input_timer, timeout);
		atomic_set(&ucassist.input_pending, 0);
		pr_debug("input timer set\n");
	}
}

static void ucassist_set_sleep_state(unsigned int state, bool set)
{
	if (test_bit(state, &ucassist.sleep_states) != set) {
		if (set)
			set_bit(state, &ucassist.sleep_states);
		else
			clear_bit(state, &ucassist.sleep_states);
	}

	kthread_queue_work(&ucassist.worker, &ucassist.update_work);
}

static void ucassist_input_timer_fn(unsigned long data)
{
	pr_debug("input timer expired\n");
	ucassist_set_sleep_state(INPUT_SLEEP_STATE, true);
}

static void ucassist_input_fn(struct irq_work *irq_work)
{
	ucassist_set_sleep_state(INPUT_SLEEP_STATE, false);
}

static void ucassist_input_trigger_timer(void)
{
	if (unlikely(!ucassist_restrict_enabled))
		return;

	/* Ignore updates until we've updated the timer */
	if (atomic_cmpxchg(&ucassist.input_pending, 0, 1))
		return;

	irq_work_queue(&ucassist.input_work);
}

#ifdef CONFIG_FB
static int ucassist_fb_notifier_callback(struct notifier_block *self, 
				unsigned long event, 
				void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (event != FB_EARLY_EVENT_BLANK)
		return 0;

	if (!evdata || !evdata->data)
		return 0;

	blank = evdata->data;

	if (*blank == FB_BLANK_UNBLANK) {
		ucassist_set_sleep_state(FB_SLEEP_STATE, false);
		/* Trigger input as well to prevent capping wake performance */
		ucassist_set_sleep_state(INPUT_SLEEP_STATE, false);
	} else if (*blank == FB_BLANK_POWERDOWN) {
		ucassist_set_sleep_state(FB_SLEEP_STATE, true);
	}

	pr_debug("sleep_states = %lu\n", ucassist.sleep_states);

	return 0;
}

static struct notifier_block ucassist_fb_notif = {
	.notifier_call = ucassist_fb_notifier_callback,
};
#endif

static int __init ucassist_init(void)
{
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct task_struct *thread;
	int ret;

	kthread_init_work(&ucassist.update_work, ucassist_update_fn);
	kthread_init_worker(&ucassist.worker);
	thread = kthread_create(kthread_worker_fn, &ucassist.worker, 
			        "ucassist");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Cannot run kthread! ret = %d\n", ret);
		goto err;
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		pr_err("failed to set SCHED_FIFO\n");
		goto err_thread;
	}

	wake_up_process(thread);

	ucassist.input_timer.data = 0;
	ucassist.input_timer.expires = 0;
	ucassist.input_timer.function = ucassist_input_timer_fn;
	init_timer(&ucassist.input_timer);

	init_irq_work(&ucassist.input_work, ucassist_input_fn);

#ifdef CONFIG_FB
	ret = fb_register_client(&ucassist_fb_notif);
	if (ret) {
		pr_err("Failed to init fb_notifier\n");
		goto err_thread;
	}
#endif

	ucassist_restrict_enabled = true;
	pr_warn("ucassist restricts access to UCLAMP values");
	return 0;

err_thread:
	kthread_stop(thread);
err:
	return ret;
}
module_init(ucassist_init);