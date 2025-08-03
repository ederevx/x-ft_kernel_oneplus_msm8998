// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/ucassist.c
 *
 * Copyright (C) 2024-2025, Edrick Vince Sinsuan
 *
 * This provides the kernel a way to configure uclamp values at init
 * for taskgroups and during runtime for defined tasks.
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

#define DISPLAY_UCLAMP_MIN_PERC 30
#define DISPLAY_UCLAMP_MIN SCHED_CAPACITY_SCALE_PERC(DISPLAY_UCLAMP_MIN_PERC)

#define GPU_UCLAMP_MIN_PERC 20
#define GPU_UCLAMP_MIN SCHED_CAPACITY_SCALE_PERC(GPU_UCLAMP_MIN_PERC)

/* Disable UCLAMP restriction for 1 second after last input event */
#define INPUT_EVENT_TIMEOUT_MS 1000

int cpu_uclamp_write_css(struct cgroup_subsys_state *css, char *buf,
					enum uclamp_id clamp_id);
int cpu_uclamp_ls_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 ls);
int cpu_uclamp_boosted_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 ls);

struct uclamp_data {
	char uclamp_max[3];
	char uclamp_min[3];
	bool latency_sensitive;
	bool boosted;
};

struct ucassist_css_struct {
	const char *name;
	struct uclamp_data data;
	bool initialized;
};

struct ucassist_task_struct {
	const char target[TASK_COMM_LEN];
	unsigned int uclamp_max;
	unsigned int uclamp_min;
	bool trigger_input;
};

struct ucassist_sleep_struct {
	unsigned int uclamp_max;
	unsigned int uclamp_min;
};

enum {
	ZERO_SLEEP_STATE = 0,
	INPUT_SLEEP_STATE,
#ifdef CONFIG_FB
	FB_SLEEP_STATE,
#endif
	MAX_STATES,
};

static unsigned long ucassist_sleep_states = 0;

bool ucassist_restrict_enabled __read_mostly = false;

static const struct ucassist_css_struct ucassist_css_data[] = {
	{
		.name = "top-app",
		.data = { "max", "10", 1, 1 },
	},
	{
		.name = "foreground",
		.data = { "max", "0", 0, 0 },
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

static const struct ucassist_task_struct ucassist_task_data[] = {
	{
		.target = "composer",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.trigger_input = false,
	},
	{
		.target = "Gralloc",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = GPU_UCLAMP_MIN,
		.trigger_input = false,
	},
	{
		.target = "kgsl_worker",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = GPU_UCLAMP_MIN,
		.trigger_input = false,
	},
	{
		.target = "mdss_fb",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.trigger_input = false,
	},
	{
		.target = "Render",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.trigger_input = true,
	},
	{
		.target = "surfaceflinger",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.trigger_input = true,
	},
	{
		.target = "vsync_retire",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = DISPLAY_UCLAMP_MIN,
		.trigger_input = false,
	},
};

static const struct ucassist_sleep_struct ucassist_sleep_data[] = {
	[ZERO_SLEEP_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = SCHED_CAPACITY_SCALE,
	},
	[INPUT_SLEEP_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE_PERC(75),
		.uclamp_min = DISPLAY_UCLAMP_MIN,
	},
#ifdef CONFIG_FB
	[FB_SLEEP_STATE] = {
		.uclamp_max = SCHED_CAPACITY_SCALE_PERC(50),
		.uclamp_min = 0,
	},
#endif
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

int ucassist_init_cpu_values(struct cgroup_subsys_state *css)
{
	const struct ucassist_css_struct *uc;
	int i;

	if (!css->cgroup->kn)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ucassist_css_data); i++) {
		uc = &ucassist_css_data[i];

		if (strcmp(css->cgroup->kn->name, uc->name))
			continue;

		pr_info("setting values for %s", uc->name);
		ucassist_set_css_uclamp_data(css, uc->data);
		break;
	}

	return 0;
}

static void ucassist_input_trigger_timer(void);

int ucassist_get_task_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max)
{
	const struct ucassist_task_struct *uc;
	int i;

	for (i = 0; i < ARRAY_SIZE(ucassist_task_data); i++) {
		uc = &ucassist_task_data[i];

		if (likely(!strstr(p->comm, uc->target)))
			continue;

		if (uc->trigger_input)
			ucassist_input_trigger_timer();

		*min = uc->uclamp_min;
		*max = uc->uclamp_max;

		pr_warn_once("ucassist overrides task UCLAMPs\n");
		return 0;
	}

	return -EINVAL;
}

static void ucassist_state_update_fn(struct work_struct *work)
{
	const struct ucassist_sleep_struct *us;
	int state;

	/* Start from the deepest state towards the shallowest */
	for (state = MAX_STATES - 1; state > ZERO_SLEEP_STATE; state--) {
		if (test_bit(state, &ucassist_sleep_states))
			break;
	}

	us = &ucassist_sleep_data[state];
	ucassist_sched_uclamp_set(us->uclamp_min, us->uclamp_max);
}
static DECLARE_WORK(ucassist_state_update_work, ucassist_state_update_fn);

static void ucassist_set_sleep_state(unsigned int state, bool set)
{
	if (set)
		set_bit(state, &ucassist_sleep_states);
	else
		clear_bit(state, &ucassist_sleep_states);

	schedule_work(&ucassist_state_update_work);
}

static void ucassist_input_timer_func(unsigned long data)
{
	pr_debug("input timer expired\n");
	ucassist_set_sleep_state(INPUT_SLEEP_STATE, true);
}
static DEFINE_TIMER(ucassist_input_timer, ucassist_input_timer_func, 0, 0);

static void ucassist_trigger_input_fn(struct irq_work *irq_work)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(INPUT_EVENT_TIMEOUT_MS);

	if (!mod_timer(&ucassist_input_timer, timeout)) {
		pr_debug("input timer set\n");
		ucassist_set_sleep_state(INPUT_SLEEP_STATE, false);
	}
}
static DEFINE_IRQ_WORK(ucassist_trigger_input_work, ucassist_trigger_input_fn);

static void ucassist_input_trigger_timer(void)
{
	static DEFINE_RAW_SPINLOCK(trigger_lock);

	if (unlikely(!ucassist_restrict_enabled))
		return;

	/* Prevent multiple concurrent access */
	if (raw_spin_trylock(&trigger_lock)) {
		irq_work_queue(&ucassist_trigger_input_work);
		raw_spin_unlock(&trigger_lock);
	}
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
		ucassist_input_trigger_timer();
	} else if (*blank == FB_BLANK_POWERDOWN) {
		ucassist_set_sleep_state(FB_SLEEP_STATE, true);
	}

	pr_debug("sleep_states = %lu\n", ucassist_sleep_states);

	return 0;
}

static struct notifier_block ucassist_fb_notif = {
	.notifier_call = ucassist_fb_notifier_callback,
};
#endif

static int __init ucassist_init(void)
{
	const struct ucassist_sleep_struct *us;
	int i, ret;

	/* Check values in the scale data for invalid values */
	for (i = MAX_STATES - 1; i >= 0; i--) {
		us = &ucassist_sleep_data[i];
		if (us->uclamp_min > us->uclamp_max || 
		    us->uclamp_max > SCHED_CAPACITY_SCALE) {
			pr_err("Invalid values! idx = %d\n", i);
			return -EINVAL;
		}
	}

#ifdef CONFIG_FB
	ret = fb_register_client(&ucassist_fb_notif);
	if (ret) {
		pr_err("Failed to init fb_notifier\n");
		return ret;
	}
#endif

	ucassist_restrict_enabled = true;
	pr_warn("ucassist restricts access to UCLAMP sysctl");
	return 0;
}
module_init(ucassist_init);