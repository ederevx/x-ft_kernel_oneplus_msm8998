// SPDX-License-Identifier: GPL-2.0
/*
 * power/supply/qcom/op_cg_uovp.c
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 *
 * This provides USB charger under/overvoltage protection through 
 * current limiting for the OnePlus 5/T.
 */
#define pr_fmt(fmt) "SMBLIB: %s: " fmt, __func__

#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include "smb-reg.h"
#include "smb-lib.h"
#include "op_cg_uovp.h"

#define UOVP_VOTER			"UOVP_VOTER"

#define CURRENT_CEIL_DEFAULT   1500000 /* DCP_CURRENT_UA (normal) = 1.5A */
#define CURRENT_FLOOR_UA       500000  /* SDP_CURRENT_UA (normal) = 500mA */
#define CURRENT_DIFF_UA        250000  /* At least 250mA */

#define CHG_HYST_MV            100
#define CHG_SOFT_OVP_HYST_MV   (CHG_SOFT_OVP_MV - CHG_HYST_MV)
#define CHG_SOFT_UVP_HYST_MV   (CHG_SOFT_UVP_MV + CHG_HYST_MV)

#define DETECT_CNT             3
#define VOTE_RETRIES           3

#define TIMEOUT_CNT            5

#define DCP_CHARGER_BITS \
	(DCP_CHARGER_BIT | FLOAT_CHARGER_BIT | OCP_CHARGER_BIT \
		| CDP_CHARGER_BIT)

struct op_cg_current_table {
	int max_icl_ua;
	int apsd_bit;
};

struct op_cg_uovp_data {
	struct smb_charger *chg;

	int uovp_cnt;
	int not_uovp_cnt;

	int not_uovp_timeout;
	int vchg_mv;

	bool last_uovp_state;
	bool uovp_state;
	bool is_overvolt;
	bool is_sdp;

	bool initialized;
	bool enable;
};

/* Table of max currents uA with their supported apsd bit */
static const struct op_cg_current_table op_cg_current_data[] = {
	{ 900000,           SDP_CHARGER_BIT   },
	{ 1500000,          DCP_CHARGER_BITS  },
};

static struct op_cg_uovp_data op_uovp_data;

static void op_cg_uovp_cutoff(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	pr_info("charger is over voltage, stop charging");
	op_charging_en(chg, false);
	chg->chg_ovp = true;
}

static void op_cg_uovp_restore(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	pr_info("charger voltage is back to normal");
	op_charging_en(chg, true);
	op_check_battery_temp(chg);
	smblib_rerun_aicl(chg);
	chg->chg_ovp = false;
}

static int op_cg_current_set(struct op_cg_uovp_data *opdata,
				int icl_ua)
{
	struct smb_charger *chg = opdata->chg;
	int curr_icl_ua;
	int ret = 0;
	int retries = VOTE_RETRIES;

	while (retries-- > 0) {
		ret = vote(chg->usb_icl_votable, UOVP_VOTER,
						true, icl_ua);
		if (ret < 0) {
			pr_err("can't set charger max current, ret=%d", ret);
			break;
		}

		/* Ensure we get the latest vote result */
		rerun_election(chg->usb_icl_votable);

		curr_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (curr_icl_ua != icl_ua) {
			pr_err("current icl ua does not match vote, rerun AICL");
			ret = -EINVAL;

			/* Rerun AICL if we're not able to change effective 
			   vote then try again */
			vote(chg->usb_icl_votable, UOVP_VOTER, false, 0);
			smblib_rerun_aicl(chg);
			msleep(500);
			continue;
		}

		power_supply_changed(chg->usb_psy);

		/* Let the ICL vote settle */
		msleep(500);
		break;
	}

	return ret;
}

static int op_cg_get_ceil_icl_ua(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	int ceil_icl_ua = CURRENT_CEIL_DEFAULT;
	int apsd_bit, i;

	/* Make sure we have the latest APSD bit in case it has been rerun */
	apsd_bit = op_get_apsd_bit(chg);

	for (i = ARRAY_SIZE(op_cg_current_data) - 1; i >= 0; i--) {
		const struct op_cg_current_table *d = &op_cg_current_data[i];

		if (apsd_bit & d->apsd_bit) {
			ceil_icl_ua = d->max_icl_ua;
			break;
		}
	}

	opdata->is_sdp = !!(apsd_bit & SDP_CHARGER_BIT);

	return ceil_icl_ua;
}

static int op_cg_current_inc_dec(struct op_cg_uovp_data *opdata,
				bool increase)
{
	struct smb_charger *chg = opdata->chg;
	int ceil_icl_ua, icl_ua, target_icl_ua, ret;

	ceil_icl_ua = op_cg_get_ceil_icl_ua(opdata);
	icl_ua = get_effective_result(chg->usb_icl_votable);
	pr_info("ceil_icl_ua=%d icl_ua=%d", ceil_icl_ua, icl_ua);

	/* We cannot control the current if !icl_ua */
	if (!icl_ua)
		return -EPERM;

	if (increase) {
		target_icl_ua = CURRENT_FLOOR_UA;

		while (1) {
			if (target_icl_ua >= ceil_icl_ua || opdata->is_sdp) {
				target_icl_ua = ceil_icl_ua;
				break;
			}

			if (target_icl_ua >= icl_ua + CURRENT_DIFF_UA)
				break;

			target_icl_ua += CURRENT_DIFF_UA;
		}
	} else {
		target_icl_ua = ceil_icl_ua;

		while (1) {
			if (target_icl_ua <= CURRENT_FLOOR_UA || opdata->is_sdp) {
				target_icl_ua = CURRENT_FLOOR_UA;
				break;
			}

			if (target_icl_ua <= icl_ua - CURRENT_DIFF_UA)
				break;

			target_icl_ua -= CURRENT_DIFF_UA;
		}
	}

	if (icl_ua != target_icl_ua) {
		pr_info("target_icl_ua=%d", target_icl_ua);
		ret = op_cg_current_set(opdata, target_icl_ua);
	} else {
		pr_err("icl_ua already at %d mA", (target_icl_ua / 1000));
		ret = -EINVAL;
	}
	return ret;
}

static bool op_cg_evaluate_uovp(struct op_cg_uovp_data *opdata, bool hyst)
{
	bool is_uovp;

	if (hyst)
		opdata->is_overvolt = !(opdata->vchg_mv < CHG_SOFT_OVP_HYST_MV);
	else
		opdata->is_overvolt = (opdata->vchg_mv > CHG_SOFT_OVP_MV);

	is_uovp = opdata->is_overvolt;
	if (!is_uovp) {
		if (hyst)
			is_uovp = !(opdata->vchg_mv > CHG_SOFT_UVP_HYST_MV);
		else
			is_uovp = (opdata->vchg_mv < CHG_SOFT_UVP_MV);
	}

	if (is_uovp && !hyst)
		pr_info("charger is %svoltage voltage=%d", 
			opdata->is_overvolt ? "over" : "under", opdata->vchg_mv);

	return is_uovp;
}

static int op_cg_reevaluate_uovp(struct op_cg_uovp_data *opdata, bool hyst)
{
	struct smb_charger *chg = opdata->chg;
	union power_supply_propval vbus_val;
	int ret;

	/* Re-evaluate the voltage and increase/decrease the 
	   voltage again if needed */
	ret = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (ret < 0)
		return ret;
	opdata->vchg_mv = vbus_val.intval;

	return op_cg_evaluate_uovp(opdata, hyst);
}

static void op_cg_detect_uovp(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	int ret;

	if (!op_cg_evaluate_uovp(opdata, false))
		return;

	opdata->uovp_state = true;
	opdata->not_uovp_cnt = 0;
	opdata->not_uovp_timeout = 0;

	if (opdata->last_uovp_state)
		opdata->uovp_cnt++;

	while (1) {
		/* Increase the current if over, decrease if under */
		ret = op_cg_current_inc_dec(opdata, opdata->is_overvolt);
		if (ret < 0)
			break;

		ret = op_cg_reevaluate_uovp(opdata, false);
		if (!ret || ret < 0)
			break;
	}

	/* We have successfully resolved the UOV */
	if (!ret)
		return;

	if (opdata->uovp_cnt <= DETECT_CNT) {
		pr_info("uovp_cnt=%d", opdata->uovp_cnt);
		return;
	}

	/* Only call cutoff if current control fails */
	if (!chg->chg_ovp)
		op_cg_uovp_cutoff(opdata);
}

static void op_cg_detect_normal(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	int ret;

	if (op_cg_evaluate_uovp(opdata, true))
		return;

	opdata->uovp_state = false;
	opdata->uovp_cnt = 0;

	if (!opdata->last_uovp_state)
		opdata->not_uovp_cnt++;

	if (opdata->not_uovp_cnt <= DETECT_CNT) {
		pr_info("not_uovp_cnt=%d", opdata->not_uovp_cnt);
		return;
	}

	/* Restore charging first if it has been disabled */
	if (chg->chg_ovp) {
		op_cg_uovp_restore(opdata);
		return;
	}

	opdata->not_uovp_cnt = 0;

	/* Wait for timeout to be cleared before trying again */
	if (opdata->not_uovp_timeout > 0) {
		opdata->not_uovp_timeout--;
		return;
	}

	/* Increase the current if not undervolt for @DETECT_CNT 
	   iterations and we're not in timeout */
	ret = op_cg_current_inc_dec(opdata, true);
	if (!ret) {
		/* Timeout if we are under/overvoltage or can't evaluate */
		if (op_cg_reevaluate_uovp(opdata, false))
			ret = -ETIMEDOUT;
	}

	/* Timeout if we failed */
	if (ret < 0)
		opdata->not_uovp_timeout = TIMEOUT_CNT;
}

static void op_cg_handle_uovp(struct op_cg_uovp_data *opdata)
{
	op_cg_detect_uovp(opdata);

	/* Check normal if it did not transition from !uovp -> uovp */
	if (!(opdata->uovp_state && !opdata->last_uovp_state))
		op_cg_detect_normal(opdata);

	opdata->last_uovp_state = opdata->uovp_state;
}

void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	if (!opdata->initialized)
		return;

	if (!chg->vbus_present) {
		pr_info("no vbus present, skip uovp");
		return;
	}

	/* Wait for the charger to settle */
	if (!opdata->enable) {
		opdata->enable = true;
		return;
	}

	pr_info("vchg_mv=%d", vchg_mv);

	opdata->vchg_mv = vchg_mv;
	op_cg_handle_uovp(opdata);
}

void op_cg_uovp_enable(struct smb_charger *chg, bool chg_present)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	if (opdata->initialized == chg_present)
		return;

	/* Clear data whenever changing states */
	memset(opdata, 0, sizeof(*opdata));

	if (chg_present) {
		opdata->chg = chg;
		opdata->initialized = true;
		pr_info("UOVP is enabled, apsd_bit=0x%d", op_get_apsd_bit(chg));
	} else {
		chg->chg_ovp = false;
		vote(chg->usb_icl_votable, UOVP_VOTER, false, 0);
		pr_info("UOVP is disabled");
	}
}
