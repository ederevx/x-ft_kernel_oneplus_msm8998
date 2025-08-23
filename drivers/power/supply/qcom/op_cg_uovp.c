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

#define CURRENT_CEIL_UA        1500000 /* DCP_CURRENT_UA (normal) = 1.5A */
#define CURRENT_FLOOR_UA       500000  /* SDP_CURRENT_UA (normal) = 500mA */
#define CURRENT_DELTA_UA       250000  /* At least 250mA */

#define CURRENT_SDP_CEIL_UA    500000  /* SDP_CURRENT_UA (normal) = 500mA */
#define CURRENT_SDP_FLOOR_UA   100000  /* SDP_CURRENT_UA (slow) = 100mA */

#define CHG_HYST_MV            100
#define CHG_SOFT_OVP_HYST_MV   (CHG_SOFT_OVP_MV - CHG_HYST_MV)
#define CHG_SOFT_UVP_HYST_MV   (CHG_SOFT_UVP_MV + CHG_HYST_MV)

#define DETECT_CNT             3

struct op_cg_uovp_data {
	struct smb_charger *chg;

	int counter;

	int vchg_mv;

	bool last_uovp_state;
	bool is_overvolt;

	bool initialized;
};

static struct op_cg_uovp_data op_uovp_data;
static DEFINE_MUTEX(op_uovp_data_lock);

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
	int ret;

	ret = vote(chg->usb_icl_votable, UOVP_VOTER, true, icl_ua);
	if (ret < 0) {
		pr_err("can't vote for USB ICL, ret=%d", ret);
		return ret;
	}

	/* Ensure we get the latest vote result */
	rerun_election(chg->usb_icl_votable);

	curr_icl_ua = get_effective_result(chg->usb_icl_votable);
	if (curr_icl_ua != icl_ua) {
		pr_err("current icl ua does not match vote");
		return -EINVAL;
	}

	power_supply_changed(chg->usb_psy);

	/* Let the ICL vote settle */
	msleep(500);
	return 0;
}

/* Evaluate whether chg is SDP according to smblib_set_icl_current */
static inline bool op_cg_check_sdp_icl(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	if (chg->typec_mode != POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		return false;

	if (chg->real_charger_type != POWER_SUPPLY_TYPE_USB)
		return false;

	if (chg->non_std_chg_present)
		return false;

	return true;
}

static int op_cg_current_inc_dec(struct op_cg_uovp_data *opdata,
				bool increase)
{
	struct smb_charger *chg = opdata->chg;
	int icl_ua, target_icl_ua, ret;

	icl_ua = get_effective_result(chg->usb_icl_votable);
	pr_info("icl_ua=%d", icl_ua);

	/* We cannot control the current if !icl_ua */
	if (!icl_ua)
		return -EPERM;

	if (!op_cg_check_sdp_icl(opdata)) {
		/* Calculate target ICL as a multiple of CURRENT_DELTA_UA */
		target_icl_ua = CURRENT_DELTA_UA * DIV_ROUND_UP(icl_ua, CURRENT_DELTA_UA);
		target_icl_ua += CURRENT_DELTA_UA * (increase ? 1 : -1);
		target_icl_ua = clamp(target_icl_ua, CURRENT_FLOOR_UA, CURRENT_CEIL_UA);
	} else {
		/* We only support 500mA and 100mA for SDP */
		target_icl_ua = increase ? CURRENT_SDP_CEIL_UA : CURRENT_SDP_FLOOR_UA;
	}

	if (icl_ua != target_icl_ua) {
		pr_info("set target_icl_ua=%d", target_icl_ua);
		ret = op_cg_current_set(opdata, target_icl_ua);
	} else {
		pr_debug("icl_ua already at %d mA", (target_icl_ua / 1000));
		ret = -EALREADY;
	}
	return ret;
}

static bool op_cg_evaluate_uovp(struct op_cg_uovp_data *opdata, bool hyst)
{
	int target_vchg_mv;
	bool is_uovp;

	if (!opdata->chg->vbus_present)
		return false;

	target_vchg_mv = hyst ? CHG_SOFT_OVP_HYST_MV : CHG_SOFT_OVP_MV;
	is_uovp = opdata->is_overvolt = (opdata->vchg_mv >= target_vchg_mv);

	if (!is_uovp) {
		target_vchg_mv = hyst ? CHG_SOFT_UVP_HYST_MV : CHG_SOFT_UVP_MV;
		is_uovp = (opdata->vchg_mv <= target_vchg_mv);
	}

	if (is_uovp && !hyst)
		pr_info("charger is %svoltage", opdata->is_overvolt ? 
				"over" : "under", opdata->vchg_mv);

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

static bool op_cg_evaluate_state_counter(struct op_cg_uovp_data *opdata, bool is_uovp)
{
	if (opdata->last_uovp_state == is_uovp)
		opdata->counter++;
	else
		opdata->counter = 0;

	opdata->last_uovp_state = is_uovp;
	return (opdata->counter > DETECT_CNT);
}

static void op_cg_detect_uovp(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	int ret;

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

	if (!op_cg_evaluate_state_counter(opdata, true)) {
		pr_info("uovp counter=%d", opdata->counter);
		return;
	}

	/* Only call cutoff if current control fails */
	if (!chg->chg_ovp)
		op_cg_uovp_cutoff(opdata);
}

static void op_cg_detect_normal(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	if (!op_cg_evaluate_state_counter(opdata, false)) {
		pr_info("normal counter=%d", opdata->counter);
		return;
	}

	/* Restore charging first if it has been disabled */
	if (chg->chg_ovp) {
		op_cg_uovp_restore(opdata);
		return;
	}

	/* Reset the counter if we will increase the current */
	opdata->counter = 0;

	/* Increase the current if not undervolt for @DETECT_CNT 
	   iterations */
	if (op_cg_current_inc_dec(opdata, true))
		return;

	/* Revert if we are under/overvoltage or can't evaluate */
	if (op_cg_reevaluate_uovp(opdata, false))
		op_cg_detect_uovp(opdata);
}

void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	if (!chg->vbus_present) {
		pr_info("no vbus present, skip uovp");
		return;
	}

	mutex_lock(&op_uovp_data_lock);

	if (!opdata->initialized) {
		mutex_unlock(&op_uovp_data_lock);
		return;
	}

	pr_info("vchg_mv=%d", vchg_mv);
	opdata->vchg_mv = vchg_mv;

	if (op_cg_evaluate_uovp(opdata, false))
		op_cg_detect_uovp(opdata);
	else if (!op_cg_evaluate_uovp(opdata, true))
		op_cg_detect_normal(opdata);

	mutex_unlock(&op_uovp_data_lock);
}

void op_cg_uovp_enable(struct smb_charger *chg, bool chg_present)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	if (opdata->initialized == chg_present)
		return;

	mutex_lock(&op_uovp_data_lock);

	/* Clear data whenever changing states */
	memset(opdata, 0, sizeof(*opdata));

	if (chg_present) {
		opdata->chg = chg;
		opdata->initialized = true;
	} else {
		chg->chg_ovp = false;
		vote(chg->usb_icl_votable, UOVP_VOTER, false, 0);
	}

	mutex_unlock(&op_uovp_data_lock);

	pr_info("UOVP is %s", chg_present ? "enabled" : "disabled");
}
