// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/ucassist.c
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 *
 * This provides the kernel a way to configure uclamp
 * values during certain kernel events.
 */
#define pr_fmt(fmt) "ucassist: %s: " fmt, __func__

#include <linux/input.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "sched.h"

int cpu_uclamp_write_css(struct cgroup_subsys_state *css, char *buf,
					enum uclamp_id clamp_id);
int cpu_uclamp_ls_write_u64(struct cgroup_subsys_state *css,
					struct cftype *cftype, u64 ls);
int cpu_uclamp_boost_write_u64(struct cgroup_subsys_state *css,
					struct cftype *cftype, u64 boost);

struct uclamp_data {
	char uclamp_max[3];
	char uclamp_min[3];
	u64 boosted;
	u64 latency_sensitive;
};

struct ucassist_struct {
	const char *name;
	struct cgroup_subsys_state *css;

	struct uclamp_data init;

	struct {
		const bool enabled;
		const unsigned long duration_ms;

		struct work_struct ework;
		struct delayed_work dwork;

		struct uclamp_data enable;
		struct uclamp_data disable;
	} input;
};

static struct ucassist_struct ucassist_data[] = {
	[0] = {
		.name = "top-app",
		.init = { "max", "10", 1, 0 },
		.input = {
			.enabled = true,
			.duration_ms = 5000,
			.enable = { "max", "78", 1, 0 },
			.disable = { "max", "10", 1, 0 },
		},
	},
	[1] = {
		.name = "foreground",
		.init = { "50", "0", 0, 0 },
	},
	[2] = {
		.name = "background",
		.init = { "max", "20", 0, 0 },
	},
	[3] = {
		.name = "system-background",
		.init = { "40", "0", 0, 0 },
	},
};

static struct workqueue_struct *ucassist_wq;

static void ucassist_set_uclamp_data(struct cgroup_subsys_state *css,
		struct uclamp_data cdata)
{
	cpu_uclamp_write_css(css, cdata.uclamp_max, 
				UCLAMP_MAX);

	cpu_uclamp_write_css(css, cdata.uclamp_min, 
				UCLAMP_MIN);

	cpu_uclamp_boost_write_u64(css, NULL, 
				cdata.boosted);

	cpu_uclamp_ls_write_u64(css, NULL, 
				cdata.latency_sensitive);
}

static DEFINE_SPINLOCK(ucassist_data_lock);

static void ucassist_disable_input_data(struct work_struct *work)
{
	struct ucassist_struct *uc = container_of(work, 
			struct ucassist_struct,
			input.dwork.work);

	if (spin_trylock(&ucassist_data_lock)) {
		ucassist_set_uclamp_data(uc->css, uc->input.disable);
		spin_unlock(&ucassist_data_lock);
	}
}

static void ucassist_enable_input_data(struct work_struct *work)
{
	struct ucassist_struct *uc = container_of(work, 
			struct ucassist_struct,
			input.ework);

	spin_lock(&ucassist_data_lock);
	ucassist_set_uclamp_data(uc->css, uc->input.enable);
	spin_unlock(&ucassist_data_lock);
}

static void ucassist_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	static DEFINE_SPINLOCK(ucassist_event_lock);
	struct ucassist_struct *uc;
	int i;

	if (!spin_trylock(&ucassist_event_lock))
		return;

	for (i = 0; i < ARRAY_SIZE(ucassist_data); i++) {
		uc = &ucassist_data[i];

		if (!uc->css)
			continue;

		if (!uc->input.enabled)
			continue;

		if (mod_delayed_work(ucassist_wq, &uc->input.dwork,
				msecs_to_jiffies(uc->input.duration_ms)))
			continue;

		queue_work(ucassist_wq, &uc->input.ework);
	}

	spin_unlock(&ucassist_event_lock);
}

static int ucassist_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "ucassist";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void ucassist_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id ucassist_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler ucassist_handler = {
	.event		   = ucassist_event,
	.connect	   = ucassist_connect,
	.disconnect	   = ucassist_disconnect,
	.name		   = "ucassist_h",
	.id_table	   = ucassist_ids,
};

int cpu_ucassist_init_values(struct cgroup_subsys_state *css)
{
	struct ucassist_struct *uc;
	int i;

	if (!css->cgroup->kn)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ucassist_data); i++) {
		uc = &ucassist_data[i];

		if (strcmp(css->cgroup->kn->name, uc->name))
			continue;

		pr_info("setting values for %s", uc->name);

		if (uc->input.enabled) {
			INIT_WORK(&uc->input.ework, 
					ucassist_enable_input_data);
			INIT_DELAYED_WORK(&uc->input.dwork, 
					ucassist_disable_input_data);
		}

		uc->css = css;
		ucassist_set_uclamp_data(css, uc->init);
	}

	return 0;
}

static int ucassist_init(void) 
{
	int ret = 0;

	ucassist_wq = alloc_workqueue("ucassist", WQ_HIGHPRI, 0);
	if (!ucassist_wq)
		ucassist_wq = system_highpri_wq;

	ret = input_register_handler(&ucassist_handler);
	if (ret)
		pr_err("Failed to register ucassist handler\n");

	return ret;
}
module_init(ucassist_init);