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

#include "sched.h"

#define SCHED_CAPACITY_SCALE_PERC(perc) \
		((perc * SCHED_CAPACITY_SCALE) / 100)

#define DISPLAY_UCLAMP_MIN_PERC 30
#define DISPLAY_UCLAMP_MIN SCHED_CAPACITY_SCALE_PERC(DISPLAY_UCLAMP_MIN_PERC)

#define GPU_UCLAMP_MIN_PERC 20
#define GPU_UCLAMP_MIN SCHED_CAPACITY_SCALE_PERC(GPU_UCLAMP_MIN_PERC)

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
};

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
	},
	{
		.target = "Gralloc",
		.uclamp_max = SCHED_CAPACITY_SCALE,
		.uclamp_min = GPU_UCLAMP_MIN,
	},
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
		.target = "Render",
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

int cpu_ucassist_init_values(struct cgroup_subsys_state *css)
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

int task_ucassist_get_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max)
{
	const struct ucassist_task_struct *uc;
	int i;

	for (i = 0; i < ARRAY_SIZE(ucassist_task_data); i++) {
		uc = &ucassist_task_data[i];

		if (likely(!strstr(p->comm, uc->target)))
			continue;

		*min = uc->uclamp_min;
		*max = uc->uclamp_max;
		return 0;
	}

	return -EINVAL;
}
