// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

/*
 *
 * Filename:
 * ---------
 *    mtk_charger.c
 *
 * Project:
 * --------
 *   Android_Software
 *
 * Description:
 * ------------
 *   This Module defines functions of Battery charging
 *
 * Author:
 * -------
 * Wy Chuang
 *
 */
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reboot.h>

#include <asm/setup.h>

#include "mtk_charger.h"
#include "mtk_battery.h"
#include "moto_chg_tcmd.h"
#include <linux/of_gpio.h>

#if defined(CONFIG_MOTO_CHG_WT6670F_SUPPORT) && defined(CONFIG_MOTO_CHARGER_SGM415XX)
#include "wt6670f.h"
#include "sgm415xx.h"
#endif

struct tag_bootmode {
	u32 size;
	u32 tag;
	u32 bootmode;
	u32 boottype;
};

#ifdef MTK_BASE
#ifdef MODULE
static char __chg_cmdline[COMMAND_LINE_SIZE];
static char *chg_cmdline = __chg_cmdline;

const char *chg_get_cmd(void)
{
	struct device_node * of_chosen = NULL;
	char *bootargs = NULL;

	if (__chg_cmdline[0] != 0)
		return chg_cmdline;

	of_chosen = of_find_node_by_path("/chosen");
	if (of_chosen) {
		bootargs = (char *)of_get_property(
					of_chosen, "bootargs", NULL);
		if (!bootargs)
			chr_err("%s: failed to get bootargs\n", __func__);
		else {
			strncpy(__chg_cmdline, bootargs, 100);
			chr_err("%s: bootargs: %s\n", __func__, bootargs);
		}
	} else
		chr_err("%s: failed to get /chosen \n", __func__);

	return chg_cmdline;
}

#else
const char *chg_get_cmd(void)
{
	return saved_command_line;
}
#endif
#endif

int chr_get_debug_level(void)
{
	struct power_supply *psy;
	static struct mtk_charger *info;
	int ret;

	if (info == NULL) {
		psy = power_supply_get_by_name("mtk-master-charger");
		if (psy == NULL)
			ret = CHRLOG_DEBUG_LEVEL;
		else {
			info =
			(struct mtk_charger *)power_supply_get_drvdata(psy);
			if (info == NULL)
				ret = CHRLOG_DEBUG_LEVEL;
			else
				ret = info->log_level;
		}
	} else
		ret = info->log_level;

	return ret;
}
EXPORT_SYMBOL(chr_get_debug_level);

void _wake_up_charger(struct mtk_charger *info)
{
	unsigned long flags;

	if (info == NULL)
		return;
	info->timer_cb_duration[2] = ktime_get_boottime();
	spin_lock_irqsave(&info->slock, flags);
	info->timer_cb_duration[3] = ktime_get_boottime();
	if (!info->charger_wakelock->active)
		__pm_stay_awake(info->charger_wakelock);
	info->timer_cb_duration[4] = ktime_get_boottime();
	spin_unlock_irqrestore(&info->slock, flags);
	info->charger_thread_timeout = true;
	info->timer_cb_duration[5] = ktime_get_boottime();
	wake_up_interruptible(&info->wait_que);
}

bool is_disable_charger(struct mtk_charger *info)
{
	if (info == NULL)
		return true;

	if (info->disable_charger == true || IS_ENABLED(CONFIG_POWER_EXT))
		return true;
	else
		return false;
}

int _mtk_enable_charging(struct mtk_charger *info,
	bool en)
{
	chr_debug("%s en:%d\n", __func__, en);
	if (info->algo.enable_charging != NULL)
		return info->algo.enable_charging(info, en);
	return false;
}

int mtk_charger_notifier(struct mtk_charger *info, int event)
{
	return srcu_notifier_call_chain(&info->evt_nh, event, NULL);
}

static void mtk_charger_parse_dt(struct mtk_charger *info,
				struct device *dev)
{
	struct device_node *np = dev->of_node;
	u32 val = 0;
	struct device_node *boot_node = NULL;
	struct tag_bootmode *tag = NULL;

	boot_node = of_parse_phandle(dev->of_node, "bootmode", 0);
	if (!boot_node)
		chr_err("%s: failed to get boot mode phandle\n", __func__);
	else {
		tag = (struct tag_bootmode *)of_get_property(boot_node,
							"atag,boot", NULL);
		if (!tag)
			chr_err("%s: failed to get atag,boot\n", __func__);
		else {
			chr_err("%s: size:0x%x tag:0x%x bootmode:0x%x boottype:0x%x\n",
				__func__, tag->size, tag->tag,
				tag->bootmode, tag->boottype);
			info->bootmode = tag->bootmode;
			info->boottype = tag->boottype;
		}
	}

	if (of_property_read_string(np, "algorithm_name",
		&info->algorithm_name) < 0) {
		chr_err("%s: no algorithm_name name\n", __func__);
		info->algorithm_name = "Basic";
	}

	if (strcmp(info->algorithm_name, "Basic") == 0) {
		chr_err("found Basic\n");
		mtk_basic_charger_init(info);
	} else if (strcmp(info->algorithm_name, "Pulse") == 0) {
		chr_err("found Pulse\n");
		mtk_pulse_charger_init(info);
	}

	info->disable_charger = of_property_read_bool(np, "disable_charger");
	info->charger_unlimited = of_property_read_bool(np, "charger_unlimited");
	info->atm_enabled = of_property_read_bool(np, "atm_is_enabled");
	info->enable_sw_safety_timer =
			of_property_read_bool(np, "enable_sw_safety_timer");
	info->sw_safety_timer_setting = info->enable_sw_safety_timer;
	info->disable_aicl = of_property_read_bool(np, "disable_aicl");

	/* common */

	if (of_property_read_u32(np, "charger_configuration", &val) >= 0)
		info->config = val;
	else {
		chr_err("use default charger_configuration:%d\n",
			SINGLE_CHARGER);
		info->config = SINGLE_CHARGER;
	}

	if (of_property_read_u32(np, "battery_cv", &val) >= 0)
		info->data.battery_cv = val;
	else {
		chr_err("use default BATTERY_CV:%d\n", BATTERY_CV);
		info->data.battery_cv = BATTERY_CV;
	}

	if (of_property_read_u32(np, "max_charger_voltage", &val) >= 0)
		info->data.max_charger_voltage = val;
	else {
		chr_err("use default V_CHARGER_MAX:%d\n", V_CHARGER_MAX);
		info->data.max_charger_voltage = V_CHARGER_MAX;
	}
	info->data.max_charger_voltage_setting = info->data.max_charger_voltage;

	if (of_property_read_u32(np, "min_charger_voltage", &val) >= 0)
		info->data.min_charger_voltage = val;
	else {
		chr_err("use default V_CHARGER_MIN:%d\n", V_CHARGER_MIN);
		info->data.min_charger_voltage = V_CHARGER_MIN;
	}
	info->enable_vbat_mon = of_property_read_bool(np, "enable_vbat_mon");
	if (info->enable_vbat_mon == true)
		info->setting.vbat_mon_en = true;
	chr_err("use 6pin bat, enable_vbat_mon:%d\n", info->enable_vbat_mon);
	info->enable_vbat_mon_bak = of_property_read_bool(np, "enable_vbat_mon");

	/* sw jeita */
	info->enable_sw_jeita = of_property_read_bool(np, "enable_sw_jeita");
	if (of_property_read_u32(np, "jeita_temp_above_t4_cv", &val) >= 0)
		info->data.jeita_temp_above_t4_cv = val;
	else {
		chr_err("use default JEITA_TEMP_ABOVE_T4_CV:%d\n",
			JEITA_TEMP_ABOVE_T4_CV);
		info->data.jeita_temp_above_t4_cv = JEITA_TEMP_ABOVE_T4_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t3_to_t4_cv", &val) >= 0)
		info->data.jeita_temp_t3_to_t4_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T3_TO_T4_CV:%d\n",
			JEITA_TEMP_T3_TO_T4_CV);
		info->data.jeita_temp_t3_to_t4_cv = JEITA_TEMP_T3_TO_T4_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t2_to_t3_cv", &val) >= 0)
		info->data.jeita_temp_t2_to_t3_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T2_TO_T3_CV:%d\n",
			JEITA_TEMP_T2_TO_T3_CV);
		info->data.jeita_temp_t2_to_t3_cv = JEITA_TEMP_T2_TO_T3_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t1_to_t2_cv", &val) >= 0)
		info->data.jeita_temp_t1_to_t2_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T1_TO_T2_CV:%d\n",
			JEITA_TEMP_T1_TO_T2_CV);
		info->data.jeita_temp_t1_to_t2_cv = JEITA_TEMP_T1_TO_T2_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_t0_to_t1_cv", &val) >= 0)
		info->data.jeita_temp_t0_to_t1_cv = val;
	else {
		chr_err("use default JEITA_TEMP_T0_TO_T1_CV:%d\n",
			JEITA_TEMP_T0_TO_T1_CV);
		info->data.jeita_temp_t0_to_t1_cv = JEITA_TEMP_T0_TO_T1_CV;
	}

	if (of_property_read_u32(np, "jeita_temp_below_t0_cv", &val) >= 0)
		info->data.jeita_temp_below_t0_cv = val;
	else {
		chr_err("use default JEITA_TEMP_BELOW_T0_CV:%d\n",
			JEITA_TEMP_BELOW_T0_CV);
		info->data.jeita_temp_below_t0_cv = JEITA_TEMP_BELOW_T0_CV;
	}

	if (of_property_read_u32(np, "temp_t4_thres", &val) >= 0)
		info->data.temp_t4_thres = val;
	else {
		chr_err("use default TEMP_T4_THRES:%d\n",
			TEMP_T4_THRES);
		info->data.temp_t4_thres = TEMP_T4_THRES;
	}

	if (of_property_read_u32(np, "temp_t4_thres_minus_x_degree", &val) >= 0)
		info->data.temp_t4_thres_minus_x_degree = val;
	else {
		chr_err("use default TEMP_T4_THRES_MINUS_X_DEGREE:%d\n",
			TEMP_T4_THRES_MINUS_X_DEGREE);
		info->data.temp_t4_thres_minus_x_degree =
					TEMP_T4_THRES_MINUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t3_thres", &val) >= 0)
		info->data.temp_t3_thres = val;
	else {
		chr_err("use default TEMP_T3_THRES:%d\n",
			TEMP_T3_THRES);
		info->data.temp_t3_thres = TEMP_T3_THRES;
	}

	if (of_property_read_u32(np, "temp_t3_thres_minus_x_degree", &val) >= 0)
		info->data.temp_t3_thres_minus_x_degree = val;
	else {
		chr_err("use default TEMP_T3_THRES_MINUS_X_DEGREE:%d\n",
			TEMP_T3_THRES_MINUS_X_DEGREE);
		info->data.temp_t3_thres_minus_x_degree =
					TEMP_T3_THRES_MINUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t2_thres", &val) >= 0)
		info->data.temp_t2_thres = val;
	else {
		chr_err("use default TEMP_T2_THRES:%d\n",
			TEMP_T2_THRES);
		info->data.temp_t2_thres = TEMP_T2_THRES;
	}

	if (of_property_read_u32(np, "temp_t2_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t2_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T2_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T2_THRES_PLUS_X_DEGREE);
		info->data.temp_t2_thres_plus_x_degree =
					TEMP_T2_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t1_thres", &val) >= 0)
		info->data.temp_t1_thres = val;
	else {
		chr_err("use default TEMP_T1_THRES:%d\n",
			TEMP_T1_THRES);
		info->data.temp_t1_thres = TEMP_T1_THRES;
	}

	if (of_property_read_u32(np, "temp_t1_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t1_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T1_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T1_THRES_PLUS_X_DEGREE);
		info->data.temp_t1_thres_plus_x_degree =
					TEMP_T1_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_t0_thres", &val) >= 0)
		info->data.temp_t0_thres = val;
	else {
		chr_err("use default TEMP_T0_THRES:%d\n",
			TEMP_T0_THRES);
		info->data.temp_t0_thres = TEMP_T0_THRES;
	}

	if (of_property_read_u32(np, "temp_t0_thres_plus_x_degree", &val) >= 0)
		info->data.temp_t0_thres_plus_x_degree = val;
	else {
		chr_err("use default TEMP_T0_THRES_PLUS_X_DEGREE:%d\n",
			TEMP_T0_THRES_PLUS_X_DEGREE);
		info->data.temp_t0_thres_plus_x_degree =
					TEMP_T0_THRES_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "temp_neg_10_thres", &val) >= 0)
		info->data.temp_neg_10_thres = val;
	else {
		chr_err("use default TEMP_NEG_10_THRES:%d\n",
			TEMP_NEG_10_THRES);
		info->data.temp_neg_10_thres = TEMP_NEG_10_THRES;
	}

	/* battery temperature protection */
	info->thermal.sm = BAT_TEMP_NORMAL;
	info->thermal.enable_min_charge_temp =
		of_property_read_bool(np, "enable_min_charge_temp");

	if (of_property_read_u32(np, "min_charge_temp", &val) >= 0)
		info->thermal.min_charge_temp = val;
	else {
		chr_err("use default MIN_CHARGE_TEMP:%d\n",
			MIN_CHARGE_TEMP);
		info->thermal.min_charge_temp = MIN_CHARGE_TEMP;
	}

	if (of_property_read_u32(np, "min_charge_temp_plus_x_degree", &val)
		>= 0) {
		info->thermal.min_charge_temp_plus_x_degree = val;
	} else {
		chr_err("use default MIN_CHARGE_TEMP_PLUS_X_DEGREE:%d\n",
			MIN_CHARGE_TEMP_PLUS_X_DEGREE);
		info->thermal.min_charge_temp_plus_x_degree =
					MIN_CHARGE_TEMP_PLUS_X_DEGREE;
	}

	if (of_property_read_u32(np, "max_charge_temp", &val) >= 0)
		info->thermal.max_charge_temp = val;
	else {
		chr_err("use default MAX_CHARGE_TEMP:%d\n",
			MAX_CHARGE_TEMP);
		info->thermal.max_charge_temp = MAX_CHARGE_TEMP;
	}

	if (of_property_read_u32(np, "max_charge_temp_minus_x_degree", &val)
		>= 0) {
		info->thermal.max_charge_temp_minus_x_degree = val;
	} else {
		chr_err("use default MAX_CHARGE_TEMP_MINUS_X_DEGREE:%d\n",
			MAX_CHARGE_TEMP_MINUS_X_DEGREE);
		info->thermal.max_charge_temp_minus_x_degree =
					MAX_CHARGE_TEMP_MINUS_X_DEGREE;
	}

	/* charging current */
	if (of_property_read_u32(np, "usb_charger_current", &val) >= 0) {
		info->data.usb_charger_current = val;
	} else {
		chr_err("use default USB_CHARGER_CURRENT:%d\n",
			USB_CHARGER_CURRENT);
		info->data.usb_charger_current = USB_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "ac_charger_current", &val) >= 0) {
		info->data.ac_charger_current = val;
	} else {
		chr_err("use default AC_CHARGER_CURRENT:%d\n",
			AC_CHARGER_CURRENT);
		info->data.ac_charger_current = AC_CHARGER_CURRENT;
	}

	if (of_property_read_u32(np, "ac_charger_input_current", &val) >= 0)
		info->data.ac_charger_input_current = val;
	else {
		chr_err("use default AC_CHARGER_INPUT_CURRENT:%d\n",
			AC_CHARGER_INPUT_CURRENT);
		info->data.ac_charger_input_current = AC_CHARGER_INPUT_CURRENT;
	}

	if (of_property_read_u32(np, "charging_host_charger_current", &val)
		>= 0) {
		info->data.charging_host_charger_current = val;
	} else {
		chr_err("use default CHARGING_HOST_CHARGER_CURRENT:%d\n",
			CHARGING_HOST_CHARGER_CURRENT);
		info->data.charging_host_charger_current =
					CHARGING_HOST_CHARGER_CURRENT;
	}

	/* dynamic mivr */
	info->enable_dynamic_mivr =
			of_property_read_bool(np, "enable_dynamic_mivr");

	if (of_property_read_u32(np, "min_charger_voltage_1", &val) >= 0)
		info->data.min_charger_voltage_1 = val;
	else {
		chr_err("use default V_CHARGER_MIN_1: %d\n", V_CHARGER_MIN_1);
		info->data.min_charger_voltage_1 = V_CHARGER_MIN_1;
	}

	if (of_property_read_u32(np, "min_charger_voltage_2", &val) >= 0)
		info->data.min_charger_voltage_2 = val;
	else {
		chr_err("use default V_CHARGER_MIN_2: %d\n", V_CHARGER_MIN_2);
		info->data.min_charger_voltage_2 = V_CHARGER_MIN_2;
	}

	if (of_property_read_u32(np, "max_dmivr_charger_current", &val) >= 0)
		info->data.max_dmivr_charger_current = val;
	else {
		chr_err("use default MAX_DMIVR_CHARGER_CURRENT: %d\n",
			MAX_DMIVR_CHARGER_CURRENT);
		info->data.max_dmivr_charger_current =
					MAX_DMIVR_CHARGER_CURRENT;
	}
	/* fast charging algo support indicator */
	info->enable_fast_charging_indicator =
			of_property_read_bool(np, "enable_fast_charging_indicator");

	if (of_property_read_u32(np, "fast_charging_indicator", &val) >= 0)
		info->fast_charging_indicator = val;
	else {
		chr_err("use default fast_charging_indicator:%d\n",
			DEFAULT_ALG);
		info->fast_charging_indicator = DEFAULT_ALG;
	}

	if (of_property_read_u32(np, "wireless_factory_max_current", &val) >= 0) {
		info->data.wireless_factory_max_current = val;
	} else {
		chr_err("use default WIRELESS_FACTORY_MAX_CURRENT:%d\n",
			WIRELESS_FACTORY_MAX_CURRENT);
		info->data.wireless_factory_max_current = WIRELESS_FACTORY_MAX_CURRENT;
	}

	if (of_property_read_u32(np, "wireless_factory_max_input_current", &val) >= 0) {
		info->data.wireless_factory_max_input_current = val;
	} else {
		chr_err("use default WIRELESS_FACTORY_MAX_INPUT_CURRENT:%d\n",
			WIRELESS_FACTORY_MAX_INPUT_CURRENT);
		info->data.wireless_factory_max_input_current = WIRELESS_FACTORY_MAX_INPUT_CURRENT;
	}
	/* meta mode*/
	if ((info->bootmode == 1) ||(info->bootmode == 5)) {
		info->config = SINGLE_CHARGER;
		info->fast_charging_indicator = DEFAULT_ALG;
	}
}

static void mtk_charger_start_timer(struct mtk_charger *info)
{
	struct timespec64 end_time, time_now;
	ktime_t ktime, ktime_now;
	int ret = 0;

	/* If the timer was already set, cancel it */
	ret = alarm_try_to_cancel(&info->charger_timer);
	if (ret < 0) {
		chr_err("%s: callback was running, skip timer\n", __func__);
		return;
	}

	ktime_now = ktime_get_boottime();
	time_now = ktime_to_timespec64(ktime_now);
	end_time.tv_sec = time_now.tv_sec + info->polling_interval;
	end_time.tv_nsec = time_now.tv_nsec + 0;
	info->endtime = end_time;
	ktime = ktime_set(info->endtime.tv_sec, info->endtime.tv_nsec);

	chr_err("%s: alarm timer start:%d, %ld %ld\n", __func__, ret,
		info->endtime.tv_sec, info->endtime.tv_nsec);
	alarm_start(&info->charger_timer, ktime);
}

static void check_battery_exist(struct mtk_charger *info)
{
	unsigned int i = 0;
	int count = 0;
	//int boot_mode = get_boot_mode();

	if (is_disable_charger(info))
		return;

	for (i = 0; i < 3; i++) {
		if (is_battery_exist(info) == false)
			count++;
	}

#ifdef FIXME
	if (count >= 3) {
		if (boot_mode == META_BOOT || boot_mode == ADVMETA_BOOT ||
		    boot_mode == ATE_FACTORY_BOOT)
			chr_info("boot_mode = %d, bypass battery check\n",
				boot_mode);
		else {
			chr_err("battery doesn't exist, shutdown\n");
			orderly_poweroff(true);
		}
	}
#endif
}

static void check_dynamic_mivr(struct mtk_charger *info)
{
	int i = 0, ret = 0;
	int vbat = 0;
	bool is_fast_charge = false;
	struct chg_alg_device *alg = NULL;

	if (!info->enable_dynamic_mivr)
		return;

	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		if (alg == NULL)
			continue;

		ret = chg_alg_is_algo_ready(alg);
		if (ret == ALG_RUNNING) {
			is_fast_charge = true;
			break;
		}
	}

	if (!is_fast_charge) {
		vbat = get_battery_voltage(info);
		if (vbat < info->data.min_charger_voltage_2 / 1000 - 200)
			charger_dev_set_mivr(info->chg1_dev,
				info->data.min_charger_voltage_2);
		else if (vbat < info->data.min_charger_voltage_1 / 1000 - 200)
			charger_dev_set_mivr(info->chg1_dev,
				info->data.min_charger_voltage_1);
		else
			charger_dev_set_mivr(info->chg1_dev,
				info->data.min_charger_voltage);
	}
}

/* sw jeita */
void do_sw_jeita_state_machine(struct mtk_charger *info)
{
	struct sw_jeita_data *sw_jeita;

	sw_jeita = &info->sw_jeita;
	sw_jeita->pre_sm = sw_jeita->sm;
	sw_jeita->charging = true;

	/* JEITA battery temp Standard */
	if (info->battery_temp >= info->data.temp_t4_thres) {
		chr_err("[SW_JEITA] Battery Over high Temperature(%d) !!\n",
			info->data.temp_t4_thres);

		sw_jeita->sm = TEMP_ABOVE_T4;
		sw_jeita->charging = false;
	} else if (info->battery_temp > info->data.temp_t3_thres) {
		/* control 45 degree to normal behavior */
		if ((sw_jeita->sm == TEMP_ABOVE_T4)
		    && (info->battery_temp
			>= info->data.temp_t4_thres_minus_x_degree)) {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
				info->data.temp_t4_thres_minus_x_degree,
				info->data.temp_t4_thres);

			sw_jeita->charging = false;
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t3_thres,
				info->data.temp_t4_thres);

			sw_jeita->sm = TEMP_T3_TO_T4;
		}
	} else if (info->battery_temp >= info->data.temp_t2_thres) {
		if (((sw_jeita->sm == TEMP_T3_TO_T4)
		     && (info->battery_temp
			 >= info->data.temp_t3_thres_minus_x_degree))
		    || ((sw_jeita->sm == TEMP_T1_TO_T2)
			&& (info->battery_temp
			    <= info->data.temp_t2_thres_plus_x_degree))) {
			chr_err("[SW_JEITA] Battery Temperature not recovery to normal temperature charging mode yet!!\n");
		} else {
			chr_err("[SW_JEITA] Battery Normal Temperature between %d and %d !!\n",
				info->data.temp_t2_thres,
				info->data.temp_t3_thres);
			sw_jeita->sm = TEMP_T2_TO_T3;
		}
	} else if (info->battery_temp >= info->data.temp_t1_thres) {
		if ((sw_jeita->sm == TEMP_T0_TO_T1
		     || sw_jeita->sm == TEMP_BELOW_T0)
		    && (info->battery_temp
			<= info->data.temp_t1_thres_plus_x_degree)) {
			if (sw_jeita->sm == TEMP_T0_TO_T1) {
				chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
					info->data.temp_t1_thres_plus_x_degree,
					info->data.temp_t2_thres);
			}
			if (sw_jeita->sm == TEMP_BELOW_T0) {
				chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
					info->data.temp_t1_thres,
					info->data.temp_t1_thres_plus_x_degree);
				sw_jeita->charging = false;
			}
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t1_thres,
				info->data.temp_t2_thres);

			sw_jeita->sm = TEMP_T1_TO_T2;
		}
	} else if (info->battery_temp >= info->data.temp_t0_thres) {
		if ((sw_jeita->sm == TEMP_BELOW_T0)
		    && (info->battery_temp
			<= info->data.temp_t0_thres_plus_x_degree)) {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d,not allow charging yet!!\n",
				info->data.temp_t0_thres,
				info->data.temp_t0_thres_plus_x_degree);

			sw_jeita->charging = false;
		} else {
			chr_err("[SW_JEITA] Battery Temperature between %d and %d !!\n",
				info->data.temp_t0_thres,
				info->data.temp_t1_thres);

			sw_jeita->sm = TEMP_T0_TO_T1;
		}
	} else {
		chr_err("[SW_JEITA] Battery below low Temperature(%d) !!\n",
			info->data.temp_t0_thres);
		sw_jeita->sm = TEMP_BELOW_T0;
		sw_jeita->charging = false;
	}

	/* set CV after temperature changed */
	/* In normal range, we adjust CV dynamically */
	if (sw_jeita->sm != TEMP_T2_TO_T3) {
		if (sw_jeita->sm == TEMP_ABOVE_T4)
			sw_jeita->cv = info->data.jeita_temp_above_t4_cv;
		else if (sw_jeita->sm == TEMP_T3_TO_T4)
			sw_jeita->cv = info->data.jeita_temp_t3_to_t4_cv;
		else if (sw_jeita->sm == TEMP_T2_TO_T3)
			sw_jeita->cv = 0;
		else if (sw_jeita->sm == TEMP_T1_TO_T2)
			sw_jeita->cv = info->data.jeita_temp_t1_to_t2_cv;
		else if (sw_jeita->sm == TEMP_T0_TO_T1)
			sw_jeita->cv = info->data.jeita_temp_t0_to_t1_cv;
		else if (sw_jeita->sm == TEMP_BELOW_T0)
			sw_jeita->cv = info->data.jeita_temp_below_t0_cv;
		else
			sw_jeita->cv = info->data.battery_cv;
	} else {
		sw_jeita->cv = 0;
	}

	chr_err("[SW_JEITA]preState:%d newState:%d tmp:%d cv:%d\n",
		sw_jeita->pre_sm, sw_jeita->sm, info->battery_temp,
		sw_jeita->cv);
}

static int mtk_chgstat_notify(struct mtk_charger *info)
{
	int ret = 0;
	char *env[2] = { "CHGSTAT=1", NULL };

	chr_err("%s: 0x%x\n", __func__, info->notify_code);
	ret = kobject_uevent_env(&info->pdev->dev.kobj, KOBJ_CHANGE, env);
	if (ret)
		chr_err("%s: kobject_uevent_fail, ret=%d", __func__, ret);

	return ret;
}

static void mtk_charger_set_algo_log_level(struct mtk_charger *info, int level)
{
	struct chg_alg_device *alg;
	int i = 0, ret = 0;

	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		if (alg == NULL)
			continue;

		ret = chg_alg_set_prop(alg, ALG_LOG_LEVEL, level);
		if (ret < 0)
			chr_err("%s: set ALG_LOG_LEVEL fail, ret =%d", __func__, ret);
	}
}

static ssize_t sw_jeita_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->enable_sw_jeita);
	return sprintf(buf, "%d\n", pinfo->enable_sw_jeita);
}

static ssize_t sw_jeita_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	signed int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp == 0)
			pinfo->enable_sw_jeita = false;
		else
			pinfo->enable_sw_jeita = true;

	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR_RW(sw_jeita);
/* sw jeita end*/

static ssize_t chr_type_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->chr_type);
	return sprintf(buf, "%d\n", pinfo->chr_type);
}

static ssize_t chr_type_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	signed int temp;

	if (kstrtoint(buf, 10, &temp) == 0)
		pinfo->chr_type = temp;
	else
		chr_err("%s: format error!\n", __func__);

	return size;
}

static DEVICE_ATTR_RW(chr_type);

static ssize_t Pump_Express_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int ret = 0, i = 0;
	bool is_ta_detected = false;
	struct mtk_charger *pinfo = dev->driver_data;
	struct chg_alg_device *alg = NULL;

	if (!pinfo) {
		chr_err("%s: pinfo is null\n", __func__);
		return sprintf(buf, "%d\n", is_ta_detected);
	}

	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = pinfo->alg[i];
		if (alg == NULL)
			continue;
		ret = chg_alg_is_algo_ready(alg);
		if (ret == ALG_RUNNING) {
			is_ta_detected = true;
			break;
		}
	}
	chr_err("%s: idx = %d, detect = %d\n", __func__, i, is_ta_detected);
	return sprintf(buf, "%d\n", is_ta_detected);
}

static DEVICE_ATTR_RO(Pump_Express);

static ssize_t fast_chg_indicator_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_debug("%s: %d\n", __func__, pinfo->fast_charging_indicator);
	return sprintf(buf, "%d\n", pinfo->fast_charging_indicator);
}

static ssize_t fast_chg_indicator_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	unsigned int temp;

	if (kstrtouint(buf, 10, &temp) == 0)
		pinfo->fast_charging_indicator = temp;
	else
		chr_err("%s: format error!\n", __func__);

	if (pinfo->fast_charging_indicator > 0) {
		pinfo->log_level = CHRLOG_DEBUG_LEVEL;
		mtk_charger_set_algo_log_level(pinfo, pinfo->log_level);
	}

	_wake_up_charger(pinfo);
	return size;
}

static DEVICE_ATTR_RW(fast_chg_indicator);

static ssize_t enable_meta_current_limit_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_debug("%s: %d\n", __func__, pinfo->enable_meta_current_limit);
	return sprintf(buf, "%d\n", pinfo->enable_meta_current_limit);
}

static ssize_t enable_meta_current_limit_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	unsigned int temp;

	if (kstrtouint(buf, 10, &temp) == 0)
		pinfo->enable_meta_current_limit = temp;
	else
		chr_err("%s: format error!\n", __func__);

	if (pinfo->enable_meta_current_limit > 0) {
		pinfo->log_level = CHRLOG_DEBUG_LEVEL;
		mtk_charger_set_algo_log_level(pinfo, pinfo->log_level);
	}

	_wake_up_charger(pinfo);
	return size;
}

static DEVICE_ATTR_RW(enable_meta_current_limit);

static ssize_t vbat_mon_show(struct device *dev, struct device_attribute *attr,
					       char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_debug("%s: %d\n", __func__, pinfo->enable_vbat_mon);
	return sprintf(buf, "%d\n", pinfo->enable_vbat_mon);
}

static ssize_t vbat_mon_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	unsigned int temp;

	if (kstrtouint(buf, 10, &temp) == 0) {
		if (temp == 0)
			pinfo->enable_vbat_mon = false;
		else
			pinfo->enable_vbat_mon = true;
	} else {
		chr_err("%s: format error!\n", __func__);
	}

	_wake_up_charger(pinfo);
	return size;
}

static DEVICE_ATTR_RW(vbat_mon);

static ssize_t ADC_Charger_Voltage_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;
	int vbus = get_vbus(pinfo); /* mV */

	chr_err("%s: %d\n", __func__, vbus);
	return sprintf(buf, "%d\n", vbus);
}

static DEVICE_ATTR_RO(ADC_Charger_Voltage);

static ssize_t mmi_vbats_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mtk_charger *info = dev->driver_data;
	int vbats;

	charger_dev_get_adc(info->chg1_dev,
			ADC_CHANNEL_VBAT, &vbats, &vbats);
	chr_err("%s: %d\n", __func__, vbats);

	return sprintf(buf, "%d\n", vbats);
}

static DEVICE_ATTR_RO(mmi_vbats);

static ssize_t mmi_ibus_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mtk_charger *info = dev->driver_data;
	int ibus;

	ibus = get_ibus(info);

	return sprintf(buf, "%d\n", ibus);
}

static DEVICE_ATTR_RO(mmi_ibus);

static ssize_t ADC_Charging_Current_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;
	int ibat = get_battery_current(pinfo); /* mA */

	chr_err("%s: %d\n", __func__, ibat);
	return sprintf(buf, "%d\n", ibat);
}

static DEVICE_ATTR_RO(ADC_Charging_Current);

static ssize_t input_current_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;
	int aicr = 0;

	aicr = pinfo->chg_data[CHG1_SETTING].thermal_input_current_limit;
	chr_err("%s: %d\n", __func__, aicr);
	return sprintf(buf, "%d\n", aicr);
}

static ssize_t input_current_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	struct charger_data *chg_data;
	signed int temp;

	chg_data = &pinfo->chg_data[CHG1_SETTING];
	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp < 0)
			chg_data->thermal_input_current_limit = 0;
		else
			chg_data->thermal_input_current_limit = temp;
	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR_RW(input_current);

static ssize_t charger_log_level_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_err("%s: %d\n", __func__, pinfo->log_level);
	return sprintf(buf, "%d\n", pinfo->log_level);
}

static ssize_t charger_log_level_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	int temp;

	if (kstrtoint(buf, 10, &temp) == 0) {
		if (temp < 0) {
			chr_err("%s: val is invalid: %d\n", __func__, temp);
			temp = 0;
		}
		pinfo->log_level = temp;
		chr_err("%s: log_level=%d\n", __func__, pinfo->log_level);

	} else {
		chr_err("%s: format error!\n", __func__);
	}
	return size;
}

static DEVICE_ATTR_RW(charger_log_level);

static ssize_t BatteryNotify_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtk_charger *pinfo = dev->driver_data;

	chr_info("%s: 0x%x\n", __func__, pinfo->notify_code);

	return sprintf(buf, "%u\n", pinfo->notify_code);
}

static ssize_t BatteryNotify_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct mtk_charger *pinfo = dev->driver_data;
	unsigned int reg = 0;
	int ret = 0;

	if (buf != NULL && size != 0) {
		ret = kstrtouint(buf, 16, &reg);
		if (ret < 0) {
			chr_err("%s: failed, ret = %d\n", __func__, ret);
			return ret;
		}
		pinfo->notify_code = reg;
		chr_info("%s: store code=0x%x\n", __func__, pinfo->notify_code);
		mtk_chgstat_notify(pinfo);
	}
	return size;
}

static DEVICE_ATTR_RW(BatteryNotify);

/* procfs */
static int mtk_chg_set_cv_show(struct seq_file *m, void *data)
{
	struct mtk_charger *pinfo = m->private;

	seq_printf(m, "%d\n", pinfo->data.battery_cv);
	return 0;
}

static int mtk_chg_set_cv_open(struct inode *node, struct file *file)
{
	return single_open(file, mtk_chg_set_cv_show, PDE_DATA(node));
}

static ssize_t mtk_chg_set_cv_write(struct file *file,
		const char *buffer, size_t count, loff_t *data)
{
	int len = 0, ret = 0;
	char desc[32] = {0};
	unsigned int cv = 0;
	struct mtk_charger *info = PDE_DATA(file_inode(file));
	struct power_supply *psy = NULL;
	union  power_supply_propval dynamic_cv;

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	ret = kstrtou32(desc, 10, &cv);
	if (ret == 0) {
		if (cv >= BATTERY_CV) {
			info->data.battery_cv = BATTERY_CV;
			chr_info("%s: adjust charge voltage %dV too high, use default cv\n",
				  __func__, cv);
		} else {
			info->data.battery_cv = cv;
			chr_info("%s: adjust charge voltage = %dV\n", __func__, cv);
		}
		psy = power_supply_get_by_name("battery");
		if (!IS_ERR_OR_NULL(psy)) {
			dynamic_cv.intval = info->data.battery_cv;
			ret = power_supply_set_property(psy,
				POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, &dynamic_cv);
			if (ret < 0)
				chr_err("set gauge cv fail\n");
		}
		return count;
	}

	chr_err("%s: bad argument\n", __func__);
	return count;
}

static const struct proc_ops mtk_chg_set_cv_fops = {
	.proc_open = mtk_chg_set_cv_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = mtk_chg_set_cv_write,
};

static int mtk_chg_current_cmd_show(struct seq_file *m, void *data)
{
	struct mtk_charger *pinfo = m->private;

	seq_printf(m, "%d %d\n", pinfo->usb_unlimited, pinfo->cmd_discharging);
	return 0;
}

static int mtk_chg_current_cmd_open(struct inode *node, struct file *file)
{
	return single_open(file, mtk_chg_current_cmd_show, PDE_DATA(node));
}

static ssize_t mtk_chg_current_cmd_write(struct file *file,
		const char *buffer, size_t count, loff_t *data)
{
	int len = 0;
	char desc[32] = {0};
	int current_unlimited = 0;
	int cmd_discharging = 0;
	struct mtk_charger *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	if (sscanf(desc, "%d %d", &current_unlimited, &cmd_discharging) == 2) {
		info->usb_unlimited = current_unlimited;
		if (cmd_discharging == 1) {
			info->cmd_discharging = true;
			charger_dev_enable(info->chg1_dev, false);
			charger_dev_do_event(info->chg1_dev,
					EVENT_DISCHARGE, 0);
		} else if (cmd_discharging == 0) {
			info->cmd_discharging = false;
			charger_dev_enable(info->chg1_dev, true);
			charger_dev_do_event(info->chg1_dev,
					EVENT_RECHARGE, 0);
		}

		chr_info("%s: current_unlimited=%d, cmd_discharging=%d\n",
			__func__, current_unlimited, cmd_discharging);
		return count;
	}

	chr_err("bad argument, echo [usb_unlimited] [disable] > current_cmd\n");
	return count;
}

static const struct proc_ops mtk_chg_current_cmd_fops = {
	.proc_open = mtk_chg_current_cmd_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = mtk_chg_current_cmd_write,
};

static int mtk_chg_en_power_path_show(struct seq_file *m, void *data)
{
	struct mtk_charger *pinfo = m->private;
	bool power_path_en = true;

	charger_dev_is_powerpath_enabled(pinfo->chg1_dev, &power_path_en);
	seq_printf(m, "%d\n", power_path_en);

	return 0;
}

static int mtk_chg_en_power_path_open(struct inode *node, struct file *file)
{
	return single_open(file, mtk_chg_en_power_path_show, PDE_DATA(node));
}

static ssize_t mtk_chg_en_power_path_write(struct file *file,
		const char *buffer, size_t count, loff_t *data)
{
	int len = 0, ret = 0;
	char desc[32] = {0};
	unsigned int enable = 0;
	struct mtk_charger *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	ret = kstrtou32(desc, 10, &enable);
	if (ret == 0) {
		charger_dev_enable_powerpath(info->chg1_dev, enable);
		chr_info("%s: enable power path = %d\n", __func__, enable);
		return count;
	}

	chr_err("bad argument, echo [enable] > en_power_path\n");
	return count;
}

static const struct proc_ops mtk_chg_en_power_path_fops = {
	.proc_open = mtk_chg_en_power_path_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = mtk_chg_en_power_path_write,
};

static int mtk_chg_en_safety_timer_show(struct seq_file *m, void *data)
{
	struct mtk_charger *pinfo = m->private;
	bool safety_timer_en = false;

	charger_dev_is_safety_timer_enabled(pinfo->chg1_dev, &safety_timer_en);
	seq_printf(m, "%d\n", safety_timer_en);

	return 0;
}

static int mtk_chg_en_safety_timer_open(struct inode *node, struct file *file)
{
	return single_open(file, mtk_chg_en_safety_timer_show, PDE_DATA(node));
}

static ssize_t mtk_chg_en_safety_timer_write(struct file *file,
	const char *buffer, size_t count, loff_t *data)
{
	int len = 0, ret = 0;
	char desc[32] = {0};
	unsigned int enable = 0;
	struct mtk_charger *info = PDE_DATA(file_inode(file));

	if (!info)
		return -EINVAL;
	if (count <= 0)
		return -EINVAL;

	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
		return -EFAULT;

	desc[len] = '\0';

	ret = kstrtou32(desc, 10, &enable);
	if (ret == 0) {
		charger_dev_enable_safety_timer(info->chg1_dev, enable);
		info->safety_timer_cmd = (int)enable;
		chr_info("%s: enable safety timer = %d\n", __func__, enable);

		/* SW safety timer */
		if (info->sw_safety_timer_setting == true) {
			if (enable)
				info->enable_sw_safety_timer = true;
			else
				info->enable_sw_safety_timer = false;
		}

		return count;
	}

	chr_err("bad argument, echo [enable] > en_safety_timer\n");
	return count;
}

static const struct proc_ops mtk_chg_en_safety_timer_fops = {
	.proc_open = mtk_chg_en_safety_timer_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = mtk_chg_en_safety_timer_write,
};

void scd_ctrl_cmd_from_user(void *nl_data, struct sc_nl_msg_t *ret_msg)
{
	struct sc_nl_msg_t *msg;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return;

	msg = nl_data;
	ret_msg->sc_cmd = msg->sc_cmd;

	switch (msg->sc_cmd) {

	case SC_DAEMON_CMD_PRINT_LOG:
		{
			chr_err("[%s] %s", SC_TAG, &msg->sc_data[0]);
		}
	break;

	case SC_DAEMON_CMD_SET_DAEMON_PID:
		{
			memcpy(&info->sc.g_scd_pid, &msg->sc_data[0],
				sizeof(info->sc.g_scd_pid));
			chr_err("[%s][fr] SC_DAEMON_CMD_SET_DAEMON_PID = %d(first launch)\n",
				SC_TAG, info->sc.g_scd_pid);
		}
	break;

	case SC_DAEMON_CMD_SETTING:
		{
			struct scd_cmd_param_t_1 data;

			memcpy(&data, &msg->sc_data[0],
				sizeof(struct scd_cmd_param_t_1));

			chr_debug("[%s] rcv data:%d %d %d %d %d %d %d %d %d %d %d %d %d %d Ans:%d\n",
				SC_TAG,
				data.data[0],
				data.data[1],
				data.data[2],
				data.data[3],
				data.data[4],
				data.data[5],
				data.data[6],
				data.data[7],
				data.data[8],
				data.data[9],
				data.data[10],
				data.data[11],
				data.data[12],
				data.data[13],
				data.data[14]);

			info->sc.solution = data.data[SC_SOLUTION];
			if (data.data[SC_SOLUTION] == SC_DISABLE)
				info->sc.disable_charger = true;
			else if (data.data[SC_SOLUTION] == SC_REDUCE)
				info->sc.disable_charger = false;
			else
				info->sc.disable_charger = false;
		}
	break;
	default:
		chr_err("[%s] bad sc_DAEMON_CTRL_CMD_FROM_USER 0x%x\n", SC_TAG, msg->sc_cmd);
		break;
	}

}

static void sc_nl_send_to_user(u32 pid, int seq, struct sc_nl_msg_t *reply_msg)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	/* int size=sizeof(struct fgd_nl_msg_t); */
	int size = reply_msg->sc_data_len + SCD_NL_MSG_T_HDR_LEN;
	int len = NLMSG_SPACE(size);
	void *data;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return;

	reply_msg->identity = SCD_NL_MAGIC;

	if (in_interrupt())
		skb = alloc_skb(len, GFP_ATOMIC);
	else
		skb = alloc_skb(len, GFP_KERNEL);

	if (!skb)
		return;

	nlh = nlmsg_put(skb, pid, seq, 0, size, 0);
	data = NLMSG_DATA(nlh);
	memcpy(data, reply_msg, size);
	NETLINK_CB(skb).portid = 0;	/* from kernel */
	NETLINK_CB(skb).dst_group = 0;	/* unicast */

	ret = netlink_unicast(info->sc.daemo_nl_sk, skb, pid, MSG_DONTWAIT);
	if (ret < 0) {
		chr_err("[Netlink] sc send failed %d\n", ret);
		return;
	}

}

static void chg_nl_data_handler(struct sk_buff *skb)
{
	u32 pid;
	kuid_t uid;
	int seq;
	void *data;
	struct nlmsghdr *nlh;
	struct sc_nl_msg_t *sc_msg, *sc_ret_msg;
	int size = 0;

	nlh = (struct nlmsghdr *)skb->data;
	pid = NETLINK_CREDS(skb)->pid;
	uid = NETLINK_CREDS(skb)->uid;
	seq = nlh->nlmsg_seq;

	data = NLMSG_DATA(nlh);

	sc_msg = (struct sc_nl_msg_t *)data;

	size = sc_msg->sc_ret_data_len + SCD_NL_MSG_T_HDR_LEN;

	if (size > (PAGE_SIZE << 1))
		sc_ret_msg = vmalloc(size);
	else {
		if (in_interrupt())
			sc_ret_msg = kmalloc(size, GFP_ATOMIC);
	else
		sc_ret_msg = kmalloc(size, GFP_KERNEL);
	}

	if (sc_ret_msg == NULL) {
		if (size > PAGE_SIZE)
			sc_ret_msg = vmalloc(size);

		if (sc_ret_msg == NULL)
			return;
	}

	memset(sc_ret_msg, 0, size);

	scd_ctrl_cmd_from_user(data, sc_ret_msg);
	sc_nl_send_to_user(pid, seq, sc_ret_msg);

	kvfree(sc_ret_msg);
}

void sc_select_charging_current(struct mtk_charger *info, struct charger_data *pdata)
{
	if (info->sc.g_scd_pid == 0)
		info->sc.sc_ibat = -1;	/* kpoc and factory mode */

	if (info->sc.g_scd_pid != 0 && info->sc.disable_in_this_plug == false) {
		chr_debug("sck: %d %d %d %d %d\n",
			info->sc.pre_ibat,
			info->sc.sc_ibat,
			pdata->charging_current_limit,
			pdata->thermal_charging_current_limit,
			info->sc.solution);
		if (info->sc.pre_ibat == -1 || info->sc.solution == SC_IGNORE
			|| info->sc.solution == SC_DISABLE) {
			info->sc.sc_ibat = -1;
		} else {
			if (info->sc.pre_ibat == pdata->charging_current_limit
				&& info->sc.solution == SC_REDUCE
				&& ((pdata->charging_current_limit - 100000) >= 500000)) {
				if (info->sc.sc_ibat == -1)
					info->sc.sc_ibat = pdata->charging_current_limit - 100000;
				else if (info->sc.sc_ibat - 100000 >= 500000)
					info->sc.sc_ibat = info->sc.sc_ibat - 100000;
			}
		}
	}
	info->sc.pre_ibat =  pdata->charging_current_limit;

	if (pdata->thermal_charging_current_limit != -1) {
	#ifdef MTK_BASE
		if (pdata->thermal_charging_current_limit <
		    pdata->charging_current_limit)
			pdata->charging_current_limit =
					pdata->thermal_charging_current_limit;
	#endif
		info->sc.disable_in_this_plug = true;
	} else if ((info->sc.solution == SC_REDUCE || info->sc.solution == SC_KEEP)
		&& info->sc.sc_ibat <
		pdata->charging_current_limit && info->sc.g_scd_pid != 0 &&
		info->sc.disable_in_this_plug == false) {
		pdata->charging_current_limit = info->sc.sc_ibat;
	}

}

void sc_init(struct smartcharging *sc)
{
	sc->enable = false;
	sc->battery_size = 3000;
	sc->start_time = 0;
	sc->end_time = 80000;
	sc->current_limit = 2000;
	sc->target_percentage = 80;
	sc->left_time_for_cv = 3600;
	sc->pre_ibat = -1;
}

void sc_update(struct mtk_charger *info)
{
	memset(&info->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	info->sc.data.data[SC_VBAT] = get_battery_voltage(info);
	info->sc.data.data[SC_BAT_TMP] = get_battery_temperature(info);
	info->sc.data.data[SC_UISOC] = get_uisoc(info);
	//info->sc.data.data[SC_SOC] = battery_get_soc();

	info->sc.data.data[SC_ENABLE] = info->sc.enable;
	info->sc.data.data[SC_BAT_SIZE] = info->sc.battery_size;
	info->sc.data.data[SC_START_TIME] = info->sc.start_time;
	info->sc.data.data[SC_END_TIME] = info->sc.end_time;
	info->sc.data.data[SC_IBAT_LIMIT] = info->sc.current_limit;
	info->sc.data.data[SC_TARGET_PERCENTAGE] = info->sc.target_percentage;
	info->sc.data.data[SC_LEFT_TIME_FOR_CV] = info->sc.left_time_for_cv;

	charger_dev_get_charging_current(info->chg1_dev, &info->sc.data.data[SC_IBAT_SETTING]);
	info->sc.data.data[SC_IBAT_SETTING] = info->sc.data.data[SC_IBAT_SETTING] / 1000;
	info->sc.data.data[SC_IBAT] = get_battery_current(info);


}

int wakeup_sc_algo_cmd(struct scd_cmd_param_t_1 *data, int subcmd, int para1)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (info->sc.g_scd_pid != 0) {
		struct sc_nl_msg_t *sc_msg;
		int size = SCD_NL_MSG_T_HDR_LEN + sizeof(struct scd_cmd_param_t_1);

		if (size > (PAGE_SIZE << 1))
			sc_msg = vmalloc(size);
		else {
			if (in_interrupt())
				sc_msg = kmalloc(size, GFP_ATOMIC);
		else
			sc_msg = kmalloc(size, GFP_KERNEL);

		}

		if (sc_msg == NULL) {
			if (size > PAGE_SIZE)
				sc_msg = vmalloc(size);

			if (sc_msg == NULL)
				return -1;
		}

		chr_debug(
			"[wakeup_fg_algo] malloc size=%d pid=%d\n",
			size, info->sc.g_scd_pid);
		memset(sc_msg, 0, size);
		sc_msg->sc_cmd = SC_DAEMON_CMD_NOTIFY_DAEMON;
		sc_msg->sc_subcmd = subcmd;
		sc_msg->sc_subcmd_para1 = para1;
		memcpy(sc_msg->sc_data, data, sizeof(struct scd_cmd_param_t_1));
		sc_msg->sc_data_len += sizeof(struct scd_cmd_param_t_1);
		sc_nl_send_to_user(info->sc.g_scd_pid, 0, sc_msg);

		kvfree(sc_msg);

		return 0;
	}
	chr_debug("pid is NULL\n");
	return -1;
}

static ssize_t enable_sc_show(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	chr_err(
	"[enable smartcharging] : %d\n",
	info->sc.enable);

	return sprintf(buf, "%d\n", info->sc.enable);
}

static ssize_t enable_sc_store(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val = 0;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (buf != NULL && size != 0) {
		chr_err("[enable smartcharging] buf is %s\n", buf);
		ret = kstrtoul(buf, 10, &val);
		if (ret == -ERANGE || ret == -EINVAL)
			return -EINVAL;
		if (val == 0)
			info->sc.enable = false;
		else
			info->sc.enable = true;

		chr_err(
			"[enable smartcharging]enable smartcharging=%d\n",
			info->sc.enable);
	}
	return size;
}
static DEVICE_ATTR_RW(enable_sc);

static ssize_t sc_stime_show(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	chr_err(
	"[smartcharging stime] : %d\n",
	info->sc.start_time);

	return sprintf(buf, "%d\n", info->sc.start_time);
}

static ssize_t sc_stime_store(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	long val = 0;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging stime] buf is %s\n", buf);
		ret = kstrtol(buf, 10, &val);
		if (ret == -ERANGE || ret == -EINVAL)
			return -EINVAL;
		if (val < 0) {
			chr_err(
				"[smartcharging stime] val is %ld ??\n",
				val);
			val = 0;
		}

		if (val >= 0)
			info->sc.start_time = (int)val;

		chr_err(
			"[smartcharging stime]enable smartcharging=%d\n",
			info->sc.start_time);
	}
	return size;
}
static DEVICE_ATTR_RW(sc_stime);

static ssize_t sc_etime_show(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	chr_err(
	"[smartcharging etime] : %d\n",
	info->sc.end_time);

	return sprintf(buf, "%d\n", info->sc.end_time);
}

static ssize_t sc_etime_store(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	long val = 0;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging etime] buf is %s\n", buf);
		ret = kstrtol(buf, 10, &val);
		if (ret == -ERANGE || ret == -EINVAL)
			return -EINVAL;
		if (val < 0) {
			chr_err(
				"[smartcharging etime] val is %ld ??\n",
				val);
			val = 0;
		}

		if (val >= 0)
			info->sc.end_time = (int)val;

		chr_err(
			"[smartcharging stime]enable smartcharging=%d\n",
			info->sc.end_time);
	}
	return size;
}
static DEVICE_ATTR_RW(sc_etime);

static ssize_t sc_tuisoc_show(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	chr_err(
	"[smartcharging target uisoc] : %d\n",
	info->sc.target_percentage);

	return sprintf(buf, "%d\n", info->sc.target_percentage);
}

static ssize_t sc_tuisoc_store(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	long val = 0;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging tuisoc] buf is %s\n", buf);
		ret = kstrtol(buf, 10, &val);
		if (ret == -ERANGE || ret == -EINVAL)
			return -EINVAL;
		if (val < 0) {
			chr_err(
				"[smartcharging tuisoc] val is %ld ??\n",
				val);
			val = 0;
		}

		if (val >= 0)
			info->sc.target_percentage = (int)val;

		chr_err(
			"[smartcharging stime]tuisoc=%d\n",
			info->sc.target_percentage);
	}
	return size;
}
static DEVICE_ATTR_RW(sc_tuisoc);

static ssize_t sc_ibat_limit_show(
	struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	chr_err(
	"[smartcharging ibat limit] : %d\n",
	info->sc.current_limit);

	return sprintf(buf, "%d\n", info->sc.current_limit);
}

static ssize_t sc_ibat_limit_store(
	struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t size)
{
	long val = 0;
	int ret;
	struct power_supply *chg_psy = NULL;
	struct mtk_charger *info = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}
	info = (struct mtk_charger *)power_supply_get_drvdata(chg_psy);
	if (info == NULL)
		return -EINVAL;

	if (buf != NULL && size != 0) {
		chr_err("[smartcharging ibat limit] buf is %s\n", buf);
		ret = kstrtol(buf, 10, &val);
		if (ret == -ERANGE || ret == -EINVAL)
			return -EINVAL;
		if (val < 0) {
			chr_err(
				"[smartcharging ibat limit] val is %ld ??\n",
				val);
			val = 0;
		}

		if (val >= 0)
			info->sc.current_limit = (int)val;

		chr_err(
			"[smartcharging ibat limit]=%d\n",
			info->sc.current_limit);
	}
	return size;
}
static DEVICE_ATTR_RW(sc_ibat_limit);

int mtk_chg_enable_vbus_ovp(bool enable)
{
	static struct mtk_charger *pinfo;
	int ret = 0;
	u32 sw_ovp = 0;
	struct power_supply *psy;

	if (pinfo == NULL) {
		psy = power_supply_get_by_name("mtk-master-charger");
		if (psy == NULL) {
			chr_err("[%s]psy is not rdy\n", __func__);
			return -1;
		}

		pinfo = (struct mtk_charger *)power_supply_get_drvdata(psy);
		if (pinfo == NULL) {
			chr_err("[%s]mtk_gauge is not rdy\n", __func__);
			return -1;
		}
	}

	if (enable)
		sw_ovp = pinfo->data.max_charger_voltage_setting;
	else
		sw_ovp = 15000000;

	/* Enable/Disable SW OVP status */
	pinfo->data.max_charger_voltage = sw_ovp;

	disable_hw_ovp(pinfo, enable);

	chr_err("[%s] en:%d ovp:%d\n",
			    __func__, enable, sw_ovp);
	return ret;
}
EXPORT_SYMBOL(mtk_chg_enable_vbus_ovp);

/* return false if vbus is over max_charger_voltage */
static bool mtk_chg_check_vbus(struct mtk_charger *info)
{
	int vchr = 0;

	vchr = get_vbus(info) * 1000; /* uV */
	if (vchr > info->data.max_charger_voltage) {
		chr_err("%s: vbus(%d mV) > %d mV\n", __func__, vchr / 1000,
			info->data.max_charger_voltage / 1000);
		return false;
	}
	return true;
}

static void mtk_battery_notify_VCharger_check(struct mtk_charger *info)
{
#if defined(BATTERY_NOTIFY_CASE_0001_VCHARGER)
	int vchr = 0;

	vchr = get_vbus(info) * 1000; /* uV */
	if (vchr < info->data.max_charger_voltage)
		info->notify_code &= ~CHG_VBUS_OV_STATUS;
	else {
		info->notify_code |= CHG_VBUS_OV_STATUS;
		chr_err("[BATTERY] charger_vol(%d mV) > %d mV\n",
			vchr / 1000, info->data.max_charger_voltage / 1000);
		mtk_chgstat_notify(info);
	}
#endif
}

static void mtk_battery_notify_VBatTemp_check(struct mtk_charger *info)
{
#if defined(BATTERY_NOTIFY_CASE_0002_VBATTEMP)
	if (info->battery_temp >= info->thermal.max_charge_temp) {
		info->notify_code |= CHG_BAT_OT_STATUS;
		chr_err("[BATTERY] bat_temp(%d) out of range(too high)\n",
			info->battery_temp);
		mtk_chgstat_notify(info);
	} else {
		info->notify_code &= ~CHG_BAT_OT_STATUS;
	}

	if (info->enable_sw_jeita == true) {
		if (info->battery_temp < info->data.temp_neg_10_thres) {
			info->notify_code |= CHG_BAT_LT_STATUS;
			chr_err("bat_temp(%d) out of range(too low)\n",
				info->battery_temp);
			mtk_chgstat_notify(info);
		} else {
			info->notify_code &= ~CHG_BAT_LT_STATUS;
		}
	} else {
#ifdef BAT_LOW_TEMP_PROTECT_ENABLE
		if (info->battery_temp < info->thermal.min_charge_temp) {
			info->notify_code |= CHG_BAT_LT_STATUS;
			chr_err("bat_temp(%d) out of range(too low)\n",
				info->battery_temp);
			mtk_chgstat_notify(info);
		} else {
			info->notify_code &= ~CHG_BAT_LT_STATUS;
		}
#endif
	}
#endif
}

static void mtk_battery_notify_UI_test(struct mtk_charger *info)
{
	switch (info->notify_test_mode) {
	case 1:
		info->notify_code = CHG_VBUS_OV_STATUS;
		chr_debug("[%s] CASE_0001_VCHARGER\n", __func__);
		break;
	case 2:
		info->notify_code = CHG_BAT_OT_STATUS;
		chr_debug("[%s] CASE_0002_VBATTEMP\n", __func__);
		break;
	case 3:
		info->notify_code = CHG_OC_STATUS;
		chr_debug("[%s] CASE_0003_ICHARGING\n", __func__);
		break;
	case 4:
		info->notify_code = CHG_BAT_OV_STATUS;
		chr_debug("[%s] CASE_0004_VBAT\n", __func__);
		break;
	case 5:
		info->notify_code = CHG_ST_TMO_STATUS;
		chr_debug("[%s] CASE_0005_TOTAL_CHARGINGTIME\n", __func__);
		break;
	case 6:
		info->notify_code = CHG_BAT_LT_STATUS;
		chr_debug("[%s] CASE6: VBATTEMP_LOW\n", __func__);
		break;
	case 7:
		info->notify_code = CHG_TYPEC_WD_STATUS;
		chr_debug("[%s] CASE7: Moisture Detection\n", __func__);
		break;
	default:
		chr_debug("[%s] Unknown BN_TestMode Code: %x\n",
			__func__, info->notify_test_mode);
	}
	mtk_chgstat_notify(info);
}

static void mtk_battery_notify_check(struct mtk_charger *info)
{
	if (info->notify_test_mode == 0x0000) {
		mtk_battery_notify_VCharger_check(info);
		mtk_battery_notify_VBatTemp_check(info);
	} else {
		mtk_battery_notify_UI_test(info);
	}
}

static void mtk_chg_get_tchg(struct mtk_charger *info)
{
	int ret;
	int tchg_min = -127, tchg_max = -127;
	struct charger_data *pdata;
	bool en = false;

	pdata = &info->chg_data[CHG1_SETTING];
	ret = charger_dev_get_temperature(info->chg1_dev, &tchg_min, &tchg_max);
	if (ret < 0) {
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
	} else {
		pdata->junction_temp_min = tchg_min;
		pdata->junction_temp_max = tchg_max;
	}

	if (info->chg2_dev) {
		pdata = &info->chg_data[CHG2_SETTING];
		ret = charger_dev_get_temperature(info->chg2_dev,
			&tchg_min, &tchg_max);

		if (ret < 0) {
			pdata->junction_temp_min = -127;
			pdata->junction_temp_max = -127;
		} else {
			pdata->junction_temp_min = tchg_min;
			pdata->junction_temp_max = tchg_max;
		}
	}

	if (info->dvchg1_dev) {
		pdata = &info->chg_data[DVCHG1_SETTING];
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
		ret = charger_dev_is_enabled(info->dvchg1_dev, &en);
		if (ret >= 0 && en) {
			ret = charger_dev_get_adc(info->dvchg1_dev,
						  ADC_CHANNEL_TEMP_JC,
						  &tchg_min, &tchg_max);
			if (ret >= 0) {
				pdata->junction_temp_min = tchg_min;
				pdata->junction_temp_max = tchg_max;
			}
		}
	}

	if (info->dvchg2_dev) {
		pdata = &info->chg_data[DVCHG2_SETTING];
		pdata->junction_temp_min = -127;
		pdata->junction_temp_max = -127;
		ret = charger_dev_is_enabled(info->dvchg2_dev, &en);
		if (ret >= 0 && en) {
			ret = charger_dev_get_adc(info->dvchg2_dev,
						  ADC_CHANNEL_TEMP_JC,
						  &tchg_min, &tchg_max);
			if (ret >= 0) {
				pdata->junction_temp_min = tchg_min;
				pdata->junction_temp_max = tchg_max;
			}
		}
	}
}

static bool mtk_is_charger_on(struct mtk_charger *info);
static int mtk_charger_force_disable_power_path(struct mtk_charger *info,
	int idx, bool disable);
bool factory_charging_enable = true;
static void charger_check_status(struct mtk_charger *info)
{
	bool charging = true;
	bool chg_dev_chgen = true;
	int temperature;
	struct battery_thermal_protection_data *thermal;
	int uisoc = 0;

	if (get_charger_type(info) == POWER_SUPPLY_TYPE_UNKNOWN)
		return;

	temperature = info->battery_temp;
	thermal = &info->thermal;
	uisoc = get_uisoc(info);

	info->setting.vbat_mon_en = true;
	if (info->enable_sw_jeita == true || info->enable_vbat_mon != true ||
	    info->batpro_done == true)
		info->setting.vbat_mon_en = false;
#ifdef MTK_BASE
	if (info->enable_sw_jeita == true) {
		do_sw_jeita_state_machine(info);
		if (info->sw_jeita.charging == false) {
			charging = false;
			goto stop_charging;
		}
	} else {

		if (thermal->enable_min_charge_temp) {
			if (temperature < thermal->min_charge_temp) {
				chr_err("Battery Under Temperature or NTC fail %d %d\n",
					temperature, thermal->min_charge_temp);
				thermal->sm = BAT_TEMP_LOW;
				charging = false;
				goto stop_charging;
			} else if (thermal->sm == BAT_TEMP_LOW) {
				if (temperature >=
				    thermal->min_charge_temp_plus_x_degree) {
					chr_err("Battery Temperature raise from %d to %d(%d), allow charging!!\n",
					thermal->min_charge_temp,
					temperature,
					thermal->min_charge_temp_plus_x_degree);
					thermal->sm = BAT_TEMP_NORMAL;
				} else {
					charging = false;
					goto stop_charging;
				}
			}
		}

		if (temperature >= thermal->max_charge_temp) {
			chr_err("Battery over Temperature or NTC fail %d %d\n",
				temperature, thermal->max_charge_temp);
			thermal->sm = BAT_TEMP_HIGH;
			charging = false;
			goto stop_charging;
		} else if (thermal->sm == BAT_TEMP_HIGH) {
			if (temperature
			    < thermal->max_charge_temp_minus_x_degree) {
				chr_err("Battery Temperature raise from %d to %d(%d), allow charging!!\n",
				thermal->max_charge_temp,
				temperature,
				thermal->max_charge_temp_minus_x_degree);
				thermal->sm = BAT_TEMP_NORMAL;
			} else {
				charging = false;
				goto stop_charging;
			}
		}
	}
#endif
	mtk_chg_get_tchg(info);

	if (!mtk_chg_check_vbus(info)) {
		charging = false;
		goto stop_charging;
	}
	if (info->cmd_discharging)
		charging = false;
	if (info->safety_timeout)
		charging = false;
	if (info->vbusov_stat)
		charging = false;
	if (info->sc.disable_charger == true)
		charging = false;

	if (info->mmi.pres_chrg_step == STEP_STOP)
		charging = false;
	if (info->mmi.demo_discharging)
		charging = false;
	if (info->mmi.adaptive_charging_disable_ibat)
		charging = false;
	if (info->atm_enabled) {
		if (factory_charging_enable)
			charging = true;
		else
			charging = false;
	}

stop_charging:
	mtk_battery_notify_check(info);

	if (charging && uisoc < 80 && info->batpro_done == true) {
		info->setting.vbat_mon_en = true;
		info->batpro_done = false;
		info->stop_6pin_re_en = false;
	}

	chr_err("tmp:%d (jeita:%d sm:%d cv:%d en:%d) (sm:%d) en:%d c:%d s:%d ov:%d sc:%d %d %d saf_cmd:%d bat_mon:%d %d\n",
		temperature, info->enable_sw_jeita, info->sw_jeita.sm,
		info->sw_jeita.cv, info->sw_jeita.charging, thermal->sm,
		charging, info->cmd_discharging, info->safety_timeout,
		info->vbusov_stat, info->sc.disable_charger,
		info->can_charging, charging, info->safety_timer_cmd,
		info->enable_vbat_mon, info->batpro_done);

	charger_dev_is_enabled(info->chg1_dev, &chg_dev_chgen);

	if (charging != info->can_charging)
		_mtk_enable_charging(info, charging);
	else if (charging == false && chg_dev_chgen == true)
		_mtk_enable_charging(info, charging);

	info->can_charging = charging;

	if (info->mmi.adaptive_charging_disable_ichg || info->mmi.demo_discharging) {
		mtk_charger_force_disable_power_path(info, CHG1_SETTING, true);
		pr_info("[%s] force disable power path true\n", __func__);
	} else if (mtk_is_charger_on(info)) {
		mtk_charger_force_disable_power_path(info, CHG1_SETTING, false);
		pr_info("[%s] force disable power path false\n", __func__);
	}
}

static int  mtk_charger_tcmd_set_usb_current(void *input, int  val);
static bool charger_init_algo(struct mtk_charger *info)
{
	struct chg_alg_device *alg;
	int idx = 0;

	info->chg1_dev = get_charger_by_name("primary_chg");
	if (info->chg1_dev)
		chr_err("%s, Found primary charger\n", __func__);
	else {
		chr_err("%s, *** Error : can't find primary charger ***\n"
			, __func__);
		return false;
	}

	alg = get_chg_alg_by_name("pe5");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get pe5 fail\n");
	else {
		chr_err("get pe5 success\n");
		alg->config = info->config;
		alg->alg_id = PE5_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}
	idx++;

	alg = get_chg_alg_by_name("pe4");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get pe4 fail\n");
	else {
		chr_err("get pe4 success\n");
		alg->config = info->config;
		alg->alg_id = PE4_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}
	idx++;

	alg = get_chg_alg_by_name("pd");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get pd fail\n");
	else {
		chr_err("get pd success\n");
		alg->config = info->config;
		alg->alg_id = PDC_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}
	idx++;

	alg = get_chg_alg_by_name("pe2");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get pe2 fail\n");
	else {
		chr_err("get pe2 success\n");
		alg->config = info->config;
		alg->alg_id = PE2_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}
	idx++;

	alg = get_chg_alg_by_name("wlc");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get wlc fail\n");
	else {
		chr_err("get wlc success\n");
		alg->config = info->config;
		alg->alg_id = WLC_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}
	idx++;

	alg = get_chg_alg_by_name("pe");
	info->alg[idx] = alg;
	if (alg == NULL)
		chr_err("get pe fail\n");
	else {
		chr_err("get pe success\n");
		alg->config = info->config;
		alg->alg_id = PE_ID;
		chg_alg_init_algo(alg);
		register_chg_alg_notifier(alg, &info->chg_alg_nb);
	}

	chr_err("config is %d\n", info->config);
	if (info->config == DUAL_CHARGERS_IN_SERIES) {
		info->chg2_dev = get_charger_by_name("secondary_chg");
		if (info->chg2_dev)
			chr_err("Found secondary charger\n");
		else {
			chr_err("*** Error : can't find secondary charger ***\n");
			return false;
		}
	} else if (info->config == DIVIDER_CHARGER ||
		   info->config == DUAL_DIVIDER_CHARGERS) {
		info->dvchg1_dev = get_charger_by_name("primary_dvchg");
		if (info->dvchg1_dev)
			chr_err("Found primary divider charger\n");
		else {
			chr_err("*** Error : can't find primary divider charger ***\n");
			return false;
		}
		if (info->config == DUAL_DIVIDER_CHARGERS) {
			info->dvchg2_dev =
				get_charger_by_name("secondary_dvchg");
			if (info->dvchg2_dev)
				chr_err("Found secondary divider charger\n");
			else {
				chr_err("*** Error : can't find secondary divider charger ***\n");
				return false;
			}
		}
	}

	chr_err("register chg1 notifier %d %d\n",
		info->chg1_dev != NULL, info->algo.do_event != NULL);
	if (info->chg1_dev != NULL && info->algo.do_event != NULL) {
		chr_err("register chg1 notifier done\n");
		info->chg1_nb.notifier_call = info->algo.do_event;
		register_charger_device_notifier(info->chg1_dev,
						&info->chg1_nb);
		charger_dev_set_drvdata(info->chg1_dev, info);
	}

	chr_err("register dvchg chg1 notifier %d %d\n",
		info->dvchg1_dev != NULL, info->algo.do_dvchg1_event != NULL);
	if (info->dvchg1_dev != NULL && info->algo.do_dvchg1_event != NULL) {
		chr_err("register dvchg chg1 notifier done\n");
		info->dvchg1_nb.notifier_call = info->algo.do_dvchg1_event;
		register_charger_device_notifier(info->dvchg1_dev,
						&info->dvchg1_nb);
		charger_dev_set_drvdata(info->dvchg1_dev, info);
	}

	chr_err("register dvchg chg2 notifier %d %d\n",
		info->dvchg2_dev != NULL, info->algo.do_dvchg2_event != NULL);
	if (info->dvchg2_dev != NULL && info->algo.do_dvchg2_event != NULL) {
		chr_err("register dvchg chg2 notifier done\n");
		info->dvchg2_nb.notifier_call = info->algo.do_dvchg2_event;
		register_charger_device_notifier(info->dvchg2_dev,
						 &info->dvchg2_nb);
		charger_dev_set_drvdata(info->dvchg2_dev, info);
	}

	charger_dev_set_mivr(info->chg1_dev, info->data.min_charger_voltage);
	if (info->mmi.factory_mode) {
		/* Disable charging when enter ATM mode(factory mode) */
		mtk_charger_tcmd_set_usb_current((void *)info, 2000);
		charger_dev_enable(info->chg1_dev, false);
	}
	return true;
}

static int mtk_charger_enable_power_path(struct mtk_charger *info,
	int idx, bool en);
static int mtk_charger_plug_out(struct mtk_charger *info)
{
	struct charger_data *pdata1 = &info->chg_data[CHG1_SETTING];
	struct charger_data *pdata2 = &info->chg_data[CHG2_SETTING];
	struct chg_alg_device *alg;
	struct chg_alg_notify notify;
	int i;

	chr_err("%s\n", __func__);
	info->chr_type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->charger_thread_polling = false;
	info->pd_reset = false;
	info->mmi.active_fast_alg = 0;

	pdata1->disable_charging_count = 0;
	pdata1->input_current_limit_by_aicl = -1;
	pdata2->disable_charging_count = 0;

	notify.evt = EVT_PLUG_OUT;
	notify.value = 0;
	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		chg_alg_notifier_call(alg, &notify);
	}
	memset(&info->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	wakeup_sc_algo_cmd(&info->sc.data, SC_EVENT_PLUG_OUT, 0);
	charger_dev_set_input_current(info->chg1_dev, 100000);
	charger_dev_set_mivr(info->chg1_dev, info->data.min_charger_voltage);
	charger_dev_plug_out(info->chg1_dev);
	mtk_charger_force_disable_power_path(info, CHG1_SETTING, true);

	if (info->enable_vbat_mon)
		charger_dev_enable_6pin_battery_charging(info->chg1_dev, false);

	power_supply_changed(info->psy1);
	return 0;
}

static int mtk_charger_plug_in(struct mtk_charger *info,
				int chr_type)
{
	struct chg_alg_device *alg;
	struct chg_alg_notify notify;
	int i, vbat;

	chr_debug("%s\n",
		__func__);

	info->chr_type = chr_type;
	info->usb_type = get_usb_type(info);
	info->charger_thread_polling = true;

	info->can_charging = true;
	//info->enable_dynamic_cv = true;
	info->safety_timeout = false;
	info->vbusov_stat = false;
	info->old_cv = 0;
	info->stop_6pin_re_en = false;
	info->batpro_done = false;

	chr_err("mtk_is_charger_on plug in, type:%d\n", chr_type);

	vbat = get_battery_voltage(info);

	notify.evt = EVT_PLUG_IN;
	notify.value = 0;
	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		chg_alg_notifier_call(alg, &notify);
		chg_alg_set_prop(alg, ALG_REF_VBAT, vbat);
	}

	memset(&info->sc.data, 0, sizeof(struct scd_cmd_param_t_1));
	info->sc.disable_in_this_plug = false;
	wakeup_sc_algo_cmd(&info->sc.data, SC_EVENT_PLUG_IN, 0);
	charger_dev_plug_in(info->chg1_dev);
	if(POWER_SUPPLY_TYPE_WIRELESS == chr_type)
		mtk_charger_enable_power_path(info, CHG1_SETTING, true);
	mtk_charger_force_disable_power_path(info, CHG1_SETTING, false);

	power_supply_changed(info->psy1);
	return 0;
}


static bool mtk_is_charger_on(struct mtk_charger *info)
{
	int chr_type;

	chr_type = get_charger_type(info);
	if (chr_type == POWER_SUPPLY_TYPE_UNKNOWN) {
		if (info->chr_type != POWER_SUPPLY_TYPE_UNKNOWN) {
			mtk_charger_plug_out(info);
			mutex_lock(&info->cable_out_lock);
			info->cable_out_cnt = 0;
			mutex_unlock(&info->cable_out_lock);
		}
	} else {
		if (info->chr_type == POWER_SUPPLY_TYPE_UNKNOWN)
			mtk_charger_plug_in(info, chr_type);
		else {
			info->chr_type = chr_type;
			info->usb_type = get_usb_type(info);
		}

		if (info->cable_out_cnt > 0) {
			mtk_charger_plug_out(info);
			mtk_charger_plug_in(info, chr_type);
			mutex_lock(&info->cable_out_lock);
			info->cable_out_cnt = 0;
			mutex_unlock(&info->cable_out_lock);
		}
	}

	if (chr_type == POWER_SUPPLY_TYPE_UNKNOWN)
		return false;

	return true;
}

#ifdef MTK_BASE
static void charger_send_kpoc_uevent(struct mtk_charger *info)
{
	static bool first_time = true;
	ktime_t ktime_now;

	if (first_time) {
		info->uevent_time_check = ktime_get();
		first_time = false;
	} else {
		ktime_now = ktime_get();
		if ((ktime_ms_delta(ktime_now, info->uevent_time_check) / 1000) >= 60) {
			mtk_chgstat_notify(info);
			info->uevent_time_check = ktime_now;
		}
	}
}
#endif

/*********************
 * MMI Functionality *
 *********************/
#define CHG_SHOW_MAX_SIZE 50
static struct mtk_charger *mmi_info;

enum {
	POWER_SUPPLY_CHARGE_RATE_NONE = 0,
	POWER_SUPPLY_CHARGE_RATE_NORMAL,
	POWER_SUPPLY_CHARGE_RATE_WEAK,
	POWER_SUPPLY_CHARGE_RATE_TURBO,
};

static char *charge_rate[] = {
	"None", "Normal", "Weak", "Turbo"
};

static char *stepchg_str[] = {
	[STEP_MAX]		= "MAX",
	[STEP_NORM]		= "NORMAL",
	[STEP_FULL]		= "FULL",
	[STEP_FLOAT]	= "FLOAT",
	[STEP_DEMO]		= "DEMO",
	[STEP_STOP]		= "STOP",
	[STEP_NONE]		= "NONE",
};

int mmi_get_prop_from_battery(struct mtk_charger *info,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!info->bat_psy) {
		info->bat_psy = power_supply_get_by_name("battery");

		if (!info->bat_psy) {
			pr_err("[%s]Error getting battery power sypply\n", __func__);
			return -EINVAL;
		}
	}

	rc = power_supply_get_property(info->bat_psy, psp, val);

	return rc;
}

int mmi_get_prop_from_charger(struct mtk_charger *info,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;
	struct power_supply *chg_psy = NULL;

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (chg_psy == NULL || IS_ERR(chg_psy)) {
		pr_err("%s Couldn't get chg_psy\n", __func__);
		return -EINVAL;
	}

	rc = power_supply_get_property(chg_psy, psp, val);

	return rc;
}

void update_charging_limit_modes(struct mtk_charger *info, int batt_soc)
{
	enum charging_limit_modes charging_limit_modes;

	charging_limit_modes = info->mmi.charging_limit_modes;
	if ((charging_limit_modes != CHARGING_LIMIT_RUN)
	    && (batt_soc >= info->mmi.upper_limit_capacity))
		charging_limit_modes = CHARGING_LIMIT_RUN;
	else if ((charging_limit_modes != CHARGING_LIMIT_OFF)
		   && (batt_soc <= info->mmi.lower_limit_capacity))
		charging_limit_modes = CHARGING_LIMIT_OFF;

	if (charging_limit_modes != info->mmi.charging_limit_modes)
		info->mmi.charging_limit_modes = charging_limit_modes;
}

static int mmi_get_ffc_fv(struct mtk_charger *info, int temp_c)
{
	int ffc_max_fv;
	int i = 0;
	int temp = temp_c;
	int num_zones;
	struct mmi_ffc_zone *zone;

	//temp = charger->batt_info.batt_temp;
	num_zones = info->mmi.num_ffc_zones;
	zone = info->mmi.ffc_zones;
	while (i < num_zones && temp > zone[i++].temp);
	zone = i > 0? &zone[i - 1] : NULL;

	info->mmi.chrg_iterm = zone->ffc_chg_iterm;
	ffc_max_fv = zone->ffc_max_mv;
	pr_info("FFC temp zone %d, fv %d mV, chg iterm %d mA\n",
		  zone, ffc_max_fv, info->mmi.chrg_iterm);

	return ffc_max_fv;
}

#define MIN_TEMP_C -20
#define MAX_TEMP_C 60
#define MIN_MAX_TEMP_C 47
#define HYSTERISIS_DEGC 2
static int mmi_find_colder_temp_zone(int pres_zone, int vbat,
				struct mmi_temp_zone *zones,
				int num_zones)
{
	int i;
	int colder_zone;
	int target_zone;

	if (pres_zone == ZONE_HOT)
		colder_zone = num_zones - 1;
	else if (pres_zone == ZONE_COLD ||
	  zones[pres_zone].temp_c == zones[ZONE_FIRST].temp_c)
		return ZONE_COLD;
	else {
		for (i = pres_zone - 1; i >= ZONE_FIRST; i--) {
			if (zones[pres_zone].temp_c > zones[i].temp_c) {
				colder_zone = i;
				break;
			}
		}
		if (i < 0)
			return ZONE_COLD;
	}

	target_zone = colder_zone;
	for (i = ZONE_FIRST; i < colder_zone; i++) {
		if (zones[colder_zone].temp_c == zones[i].temp_c) {
			target_zone = i;
			if (vbat < zones[i].norm_mv)
				break;
		}
	}

	return target_zone;
}

static int mmi_find_hotter_temp_zone(int pres_zone, int vbat,
				struct mmi_temp_zone *zones,
				int num_zones)
{
	int i;
	int hotter_zone;
	int target_zone;

	if (pres_zone == ZONE_COLD)
		hotter_zone = ZONE_FIRST;
	else if (pres_zone == ZONE_HOT ||
	    zones[pres_zone].temp_c == zones[num_zones - 1].temp_c)
		return ZONE_HOT;
	else {
		for (i = pres_zone + 1; i < num_zones; i++) {
			if (zones[pres_zone].temp_c < zones[i].temp_c) {
				hotter_zone = i;
				break;
			}
		}
		if (i >= num_zones)
			return ZONE_HOT;
	}

	target_zone = hotter_zone;
	for (i = hotter_zone; i < num_zones; i++) {
		if (zones[hotter_zone].temp_c == zones[i].temp_c) {
			target_zone = i;
			if (vbat < zones[i].norm_mv)
				break;
		}
	}
	return target_zone;
}

static int mmi_refresh_temp_zone(int pres_zone, int vbat,
				struct mmi_temp_zone *zones,
				int num_zones)
{
	int i;
	int target_zone;

	if (pres_zone == ZONE_COLD || pres_zone == ZONE_HOT)
		return pres_zone;

	target_zone = pres_zone;
	for (i = ZONE_FIRST; i < num_zones; i++) {
		if (zones[pres_zone].temp_c == zones[i].temp_c) {
			target_zone = i;
			if (vbat < zones[i].norm_mv)
				break;
		}
	}
	return target_zone;
}

static void mmi_find_temp_zone(struct mtk_charger *info, int temp_c, int vbat_mv)
{
	int i;
	//int temp_c;
	//int vbat_mv;
	int max_temp;
	int prev_zone, num_zones;
	int hotter_zone, colder_zone;
	struct mmi_temp_zone *zones;
	int hotter_t, hotter_fcc;
	int colder_t, colder_fcc;

	if (!info) {
		pr_err("called before chg valid!\n");
		return;
	}

	//temp_c = charger->batt_info.batt_temp;
	//vbat_mv = charger->batt_info.batt_mv;
	num_zones = info->mmi.num_temp_zones;
	prev_zone = info->mmi.pres_temp_zone;
	if (!info->mmi.temp_zones) {
		zones = NULL;
		num_zones = 0;
		max_temp = MAX_TEMP_C;
	} else {
		zones = info->mmi.temp_zones;
		if (info->mmi.max_chrg_temp >= MIN_MAX_TEMP_C)
			max_temp = info->mmi.max_chrg_temp;
		else
			max_temp = zones[num_zones - 1].temp_c;
	}

	if (prev_zone == ZONE_NONE && zones) {
		for (i = num_zones - 1; i >= 0; i--) {
			if (temp_c >= zones[i].temp_c) {
				info->mmi.pres_temp_zone =
					mmi_find_hotter_temp_zone(i,
							vbat_mv,
							zones,
							num_zones);
				return;
			}
		}
		if (temp_c < MIN_TEMP_C)
			info->mmi.pres_temp_zone = ZONE_COLD;
		else
			info->mmi.pres_temp_zone =
					mmi_find_hotter_temp_zone(ZONE_COLD,
							vbat_mv,
							zones,
							num_zones);
		return;
	}

	if (prev_zone == ZONE_COLD) {
		if (temp_c >= MIN_TEMP_C + HYSTERISIS_DEGC) {
			if (!num_zones)
				info->mmi.pres_temp_zone = ZONE_FIRST;
			else
				info->mmi.pres_temp_zone =
					mmi_find_hotter_temp_zone(prev_zone,
							vbat_mv,
							zones,
							num_zones);
		}
	} else if (prev_zone == ZONE_HOT) {
		if (temp_c <=  max_temp - HYSTERISIS_DEGC) {
			if (!num_zones)
				info->mmi.pres_temp_zone = ZONE_FIRST;
			else
				info->mmi.pres_temp_zone =
					mmi_find_colder_temp_zone(prev_zone,
							vbat_mv,
							zones,
							num_zones);
		}
	} else if (zones) {
		hotter_zone = mmi_find_hotter_temp_zone(prev_zone,
						vbat_mv,
						zones,
						num_zones);
		colder_zone = mmi_find_colder_temp_zone(prev_zone,
						vbat_mv,
						zones,
						num_zones);
		if (hotter_zone == ZONE_HOT) {
			hotter_fcc = 0;
			hotter_t = zones[prev_zone].temp_c;
		} else {
			hotter_fcc = zones[hotter_zone].fcc_max_ma;
			hotter_t = zones[prev_zone].temp_c;
		}

		if (colder_zone == ZONE_COLD) {
			colder_fcc = 0;
			colder_t = MIN_TEMP_C;
		} else {
			colder_fcc = zones[colder_zone].fcc_max_ma;
			colder_t = zones[colder_zone].temp_c;
		}

		if (zones[prev_zone].fcc_max_ma < hotter_fcc)
			hotter_t += HYSTERISIS_DEGC;

		if (zones[prev_zone].fcc_max_ma < colder_fcc)
			colder_t -= HYSTERISIS_DEGC;

		if (temp_c < MIN_TEMP_C)
			info->mmi.pres_temp_zone = ZONE_COLD;
		else if (temp_c >= max_temp)
			info->mmi.pres_temp_zone = ZONE_HOT;
		else if (temp_c >= hotter_t)
			info->mmi.pres_temp_zone = hotter_zone;
		else if (temp_c < colder_t)
			info->mmi.pres_temp_zone = colder_zone;
		else
			info->mmi.pres_temp_zone =
					mmi_refresh_temp_zone(prev_zone,
							vbat_mv,
							zones,
							num_zones);
	} else {
		if (temp_c < MIN_TEMP_C)
			info->mmi.pres_temp_zone = ZONE_COLD;
		else if (temp_c >= max_temp)
			info->mmi.pres_temp_zone = ZONE_HOT;
		else
			info->mmi.pres_temp_zone = ZONE_FIRST;
	}

	if (prev_zone != info->mmi.pres_temp_zone) {
		pr_info("[C:%s]: temp zone switch %x -> %x\n",
			__func__,
			prev_zone,
			info->mmi.pres_temp_zone);
	}
}

#define TAPER_COUNT 2
#define TAPER_DROP_MA 100
static bool mmi_has_current_tapered(struct mtk_charger *info,
				    int batt_ma, int taper_ma)
{
	bool change_state = false;
	int allowed_fcc, target_ma, rc;
	bool devchg1_en = false;

	if (!info) {
		pr_err("[%s]called before info valid!\n", __func__);
		return false;
	}

	if (info->dvchg1_dev) {
		rc = charger_dev_is_enabled(info->dvchg1_dev, &devchg1_en);
		if (rc < 0)
			devchg1_en = false;
	}

	if (devchg1_en)
		target_ma = taper_ma;
	else {
		rc = charger_dev_get_charging_current(info->chg1_dev, &allowed_fcc);
		if (rc < 0) {
			pr_err("[%s]can't get charging current!\n", __func__);
		} else
			allowed_fcc = allowed_fcc /1000;

		if (allowed_fcc >= taper_ma)
			target_ma = taper_ma;
		else
			target_ma = allowed_fcc - TAPER_DROP_MA;
	}

	if (batt_ma > 0) {
		if (batt_ma <= target_ma)
			if (info->mmi.chrg_taper_cnt >= TAPER_COUNT) {
				change_state = true;
				info->mmi.chrg_taper_cnt = 0;
			} else
				info->mmi.chrg_taper_cnt++;
		else
			info->mmi.chrg_taper_cnt = 0;
	} else {
		if (info->mmi.chrg_taper_cnt >= TAPER_COUNT) {
			change_state = true;
			info->mmi.chrg_taper_cnt = 0;
		} else
			info->mmi.chrg_taper_cnt++;
	}

	return change_state;
}

#define WEAK_CHRG_THRSH 450
#define TURBO_CHRG_THRSH 2500
void mmi_charge_rate_check(struct mtk_charger *info)
{
	int icl, rc;
	int rp_level = 0;
	union power_supply_propval val;
	struct charger_data *pdata;

	if (info == NULL)
		return;

	pdata = &info->chg_data[CHG1_SETTING];
	icl = pdata->input_current_limit / 1000;
	rc = mmi_get_prop_from_charger(info, POWER_SUPPLY_PROP_ONLINE, &val);
	if (rc < 0) {
		pr_err("[%s]Error get chg online rc = %d\n", __func__, rc);
		info->mmi.charge_rate= POWER_SUPPLY_CHARGE_RATE_NONE;
		goto end_rate_check;
	} else if (!val.intval) {
		pr_info("[%s]usb off line\n", __func__);
		info->mmi.charge_rate = POWER_SUPPLY_CHARGE_RATE_NONE;
		goto end_rate_check;
	}

	rp_level = adapter_dev_get_property(info->pd_adapter, TYPEC_RP_LEVEL);
	if (rp_level == 3000
		|| info->pd_type == MTK_PD_CONNECT_PE_READY_SNK
		|| info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_PD30
		|| info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO) {
		info->mmi.charge_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
		goto end_rate_check;
 	}

	if (icl >= TURBO_CHRG_THRSH)
		info->mmi.charge_rate = POWER_SUPPLY_CHARGE_RATE_TURBO;
	else if (icl < WEAK_CHRG_THRSH)
		info->mmi.charge_rate = POWER_SUPPLY_CHARGE_RATE_WEAK;
	else
		info->mmi.charge_rate =  POWER_SUPPLY_CHARGE_RATE_NORMAL;

end_rate_check:
	pr_info("%s ICL:%d, Rp:%d, PD:%d, Charger Detected: %s\n",
		__func__, icl, rp_level, info->pd_type, charge_rate[info->mmi.charge_rate]);
}

static ssize_t charge_rate_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	if (!mmi_info) {
		pr_err("SMBMMI: mmi_info is not initialized\n");
		return 0;
	}

	mmi_charge_rate_check(mmi_info);
	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%s\n",
			 charge_rate[mmi_info->mmi.charge_rate]);
}

static mmi_get_battery_age(void)
{
	struct mtk_gauge *gauge;
	struct power_supply *psy;

	psy = power_supply_get_by_name("mtk-gauge");
	if (psy == NULL) {
		pr_err("[%s]psy is not rdy\n", __func__);
		return 100;
	}

	gauge = (struct mtk_gauge *)power_supply_get_drvdata(psy);
	if (gauge == NULL) {
		pr_err("[%s]mtk_gauge is not rdy\n", __func__);
		return 100;
	}

	gauge->gm->aging_factor =
		gauge->gm->aging_factor > 10000? 10000: gauge->gm->aging_factor;
	pr_info("[%s]battery age is %d\n", __func__, gauge->gm->aging_factor);

	return gauge->gm->aging_factor /100;
}
static DEVICE_ATTR(charge_rate, S_IRUGO, charge_rate_show, NULL);

static ssize_t age_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", mmi_get_battery_age());
}
static DEVICE_ATTR(age, S_IRUGO, age_show, NULL);

static struct attribute * mmi_g[] = {
	&dev_attr_charge_rate.attr,
	&dev_attr_age.attr,
	NULL,
};

static const struct attribute_group power_supply_mmi_attr_group = {
	.attrs = mmi_g,
};


static void mmi_updata_batt_status(struct mtk_charger *info)
{
	static struct power_supply	*batt_psy;
	char *chrg_rate_string = NULL;
	char *envp[2];
	int rc;

	if (!batt_psy) {
		batt_psy = power_supply_get_by_name("battery");
		if (!batt_psy) {
			pr_err( "%s  No battery supply found\n", __func__);
			return;
		}

		rc = sysfs_create_group(&batt_psy->dev.kobj,
				&power_supply_mmi_attr_group);
		if (rc)
			pr_err("%s  failed: attr create\n", __func__);
	}

	mmi_charge_rate_check(info);
	chrg_rate_string = kmalloc(CHG_SHOW_MAX_SIZE, GFP_KERNEL);
	if (!chrg_rate_string) {
		pr_err("%s  Failed to Get Uevent Mem\n", __func__);
		envp[0] = NULL;
	} else {
		scnprintf(chrg_rate_string, CHG_SHOW_MAX_SIZE,
			  "POWER_SUPPLY_CHARGE_RATE=%s",
			  charge_rate[info->mmi.charge_rate]);
		envp[0] = chrg_rate_string;
		envp[1] = NULL;
	}

	kobject_uevent_env(&batt_psy->dev.kobj, KOBJ_CHANGE, envp);
	kfree(chrg_rate_string);
}

int mmi_batt_health_check(void)
{
	if (mmi_info == NULL) {
		pr_err("[%s]called before charger_manager valid!\n", __func__);
		return POWER_SUPPLY_HEALTH_GOOD;
	}
	return mmi_info->mmi.batt_health;
}
EXPORT_SYMBOL(mmi_batt_health_check);

int mmi_get_batt_cv_delata_by_cycle(int batt_cycle)
{
	int batt_cv_delata = 0;
	int i ;
	struct mmi_cycle_cv_steps *zones;
	int num_zones;

	if (mmi_info == NULL) {
		pr_err("[%s]called before charger_manager valid!\n", __func__);
		return -1;
	}
	zones = mmi_info->mmi.cycle_cv_steps;
	num_zones = mmi_info->mmi.num_cycle_cv_steps;

	if(zones != NULL && num_zones > 0) {
		for(i = 0; i < num_zones; i++) {
			if(batt_cycle > zones[i].cycle) {
				batt_cv_delata = zones[i].delta_cv_mv;
			}
		}
	}

	return batt_cv_delata;
}

int mmi_set_batt_cv_to_fg(int battery_cv)
{
	int rc = -1;
	struct power_supply *psy = NULL;
	union  power_supply_propval dynamic_cv;

	psy = power_supply_get_by_name("battery");
	if (!IS_ERR_OR_NULL(psy)) {
		dynamic_cv.intval = battery_cv;
		rc = power_supply_set_property(psy,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, &dynamic_cv);
		if (rc < 0)
			chr_err("[%s]Error setting battery_cv\n", __func__);
		else {
			mmi_info->data.battery_cv = battery_cv;
			pr_err("[%s]setting battery_cv=%duV\n", __func__, mmi_info->data.battery_cv);
		}
	} else {
		pr_err("[%s]Error getting battery psy\n", __func__);
	}
	return rc;
}


#define WARM_TEMP 45
#define COOL_TEMP 0
#define HYST_STEP_MV 50
#define DEMO_MODE_HYS_SOC 5
#define DEMO_MODE_VOLTAGE 4000
static void mmi_charger_check_status(struct mtk_charger *info)
{
	int rc;
	int batt_mv;
	int batt_ma;
	int batt_soc;
	int batt_temp;
	int usb_mv;
	int charger_present = 0;
	int stop_recharge_hyst;
	int prev_step;
	int batt_cv_delata;

	union power_supply_propval val;
	struct mmi_params *mmi = &info->mmi;
	struct mmi_temp_zone *zone;
	int max_fv_mv = -EINVAL;
	int target_fcc = -EINVAL;
	int target_fv = -EINVAL;

	/* Collect Current Information */

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Voltage rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_mv = val.intval / 1000;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Current rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_ma = val.intval / 1000;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_CAPACITY, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Capacity rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_soc = val.intval;

	rc = mmi_get_prop_from_battery(info,
				POWER_SUPPLY_PROP_TEMP, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting Batt Temperature rc = %d\n", __func__, rc);
		goto end_check;
	} else
		batt_temp = val.intval / 10;

	rc = mmi_get_prop_from_charger(info,
				POWER_SUPPLY_PROP_ONLINE, &val);
	if (rc < 0) {
		pr_err("[%s]Error getting charger online rc = %d\n", __func__, rc);
		goto end_check;
	} else
		charger_present = val.intval;

	usb_mv = get_vbus(info);


	pr_info("[%s]batt=%d mV, %d mA, %d C, USB= %d mV\n", __func__,
		batt_mv, batt_ma, batt_temp, usb_mv);

	if (!mmi->temp_zones) {
		pr_err("[%s]temp_zones is NULL\n", __func__);
		pr_info("[%s]EFFECTIVE: FV = %d, CDIS = %d, FCC = %d, "
		"USBICL = %d, DEMO_DISCHARG = %d\n", __func__,
		mmi->target_fv,
		mmi->chg_disable,
		mmi->target_fcc,
		mmi->target_usb,
		mmi->demo_discharging);

		goto end_check;
	}

	if (mmi->enable_charging_limit && mmi->is_factory_image)
		update_charging_limit_modes(info, batt_soc);

	mmi_find_temp_zone(info, batt_temp, batt_mv);
	if (mmi->pres_temp_zone >= info->mmi.num_temp_zones)
		zone = &mmi->temp_zones[0];
	else
		zone = &mmi->temp_zones[mmi->pres_temp_zone];

	if (mmi->base_fv_mv == 0) {
		mmi->base_fv_mv = info->data.battery_cv / 1000;
	}

#if defined(CONFIG_MOTO_CHG_WT6670F_SUPPORT) && defined(CONFIG_MOTO_CHARGER_SGM415XX)
	mmi->vfloat_comp_mv = FV_COMP_0_MV;
	if ( (info->dvchg1_dev != NULL && info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO) ||
                (m_chg_type == POWER_SUPPLY_TYPE_USB_HVDCP_3P5)){
		max_fv_mv = mmi_get_ffc_fv(info, batt_temp);
		if (info->mmi.chrg_iterm > FFC_ITERM_500MA) {
			mmi->vfloat_comp_mv = (batt_soc == 100)?FV_COMP_24_MV:FV_COMP_32_MV; //Only for ffc charging
		}
#else
	if (info->dvchg1_dev != NULL
		&& info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO) {
		max_fv_mv = mmi_get_ffc_fv(info, batt_temp);
#endif /* CONFIG_MOTO_CHG_WT6670F_SUPPORT */
		if (max_fv_mv == 0)
			max_fv_mv = mmi->base_fv_mv;
	} else {
		max_fv_mv = mmi->base_fv_mv;
		info->mmi.chrg_iterm =  info->mmi.back_chrg_iterm;
	}

	if(info->mmi.cycle_cv_steps != NULL) {
		rc = mmi_get_prop_from_battery(info,
					POWER_SUPPLY_PROP_CYCLE_COUNT, &val);
		if (rc < 0) {
			pr_err("[%s]Error getting battery cycle count rc = %d\n", __func__, rc);
		} else {
			batt_cv_delata = mmi_get_batt_cv_delata_by_cycle(val.intval);
			if( batt_cv_delata > 0)
				max_fv_mv = max_fv_mv - batt_cv_delata;

			if((max_fv_mv * 1000) != info->data.battery_cv)
				mmi_set_batt_cv_to_fg(max_fv_mv * 1000);
		}
	}

	/* Determine Next State */
	prev_step = info->mmi.pres_chrg_step;

	if (mmi->charging_limit_modes == CHARGING_LIMIT_RUN)
		pr_warn("Factory Mode/Image so Limiting Charging!!!\n");

	if (!charger_present) {
		mmi->pres_chrg_step = STEP_NONE;
	} else if ((mmi->pres_temp_zone == ZONE_HOT) ||
		   (mmi->pres_temp_zone == ZONE_COLD) ||
		   (mmi->charging_limit_modes == CHARGING_LIMIT_RUN)) {
		info->mmi.pres_chrg_step = STEP_STOP;
	} else if (mmi->demo_mode) {
		bool voltage_full;
		static int demo_full_soc = 100;
		static int usb_suspend = 0;

		mmi->pres_chrg_step = STEP_DEMO;
		pr_info("[%s]Battery in Demo Mode charging Limited %dper\n",
				__func__, mmi->demo_mode);

		voltage_full = ((usb_suspend == 0) &&
			((batt_mv + HYST_STEP_MV) >= DEMO_MODE_VOLTAGE) &&
			mmi_has_current_tapered(info, batt_ma,
						mmi->chrg_iterm));

		if ((usb_suspend == 0) &&
		    ((batt_soc >= mmi->demo_mode) ||
		     voltage_full)) {
			demo_full_soc = batt_soc;
			mmi->demo_discharging = true;
			usb_suspend = 1;
		} else if (usb_suspend &&
			   (batt_soc <=
				(demo_full_soc - DEMO_MODE_HYS_SOC))) {
			mmi->demo_discharging = false;
			usb_suspend = 0;
			mmi->chrg_taper_cnt = 0;
		}
		if (usb_suspend)
			charger_dev_set_input_current(info->chg1_dev, 0);

		pr_info("Charge Demo Mode:us = %d, vf = %d, dfs = %d,bs = %d\n",
				usb_suspend, voltage_full, demo_full_soc, batt_soc);
	} else if (mmi->pres_chrg_step == STEP_NONE) {
		if (zone->norm_mv && ((batt_mv + HYST_STEP_MV) >= zone->norm_mv)) {
			if (zone->fcc_norm_ma)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_STOP;
		} else
			mmi->pres_chrg_step = STEP_MAX;
	} else if (mmi->pres_chrg_step == STEP_STOP) {
		if (batt_temp > COOL_TEMP)
			stop_recharge_hyst = 2 * HYST_STEP_MV;
		else
			stop_recharge_hyst = 5 * HYST_STEP_MV;
		if (zone->norm_mv && ((batt_mv + stop_recharge_hyst) >= zone->norm_mv)) {
			if (zone->fcc_norm_ma)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_STOP;
		} else
			mmi->pres_chrg_step = STEP_MAX;
	}else if (mmi->pres_chrg_step == STEP_MAX) {
		if (!zone->norm_mv) {
			/* No Step in this Zone */
			mmi->chrg_taper_cnt = 0;
			if ((batt_mv + HYST_STEP_MV) >= max_fv_mv)
				mmi->pres_chrg_step = STEP_NORM;
			else
				mmi->pres_chrg_step = STEP_MAX;
		} else if ((batt_mv + HYST_STEP_MV) < zone->norm_mv) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_MAX;
		} else if (!zone->fcc_norm_ma)
			mmi->pres_chrg_step = STEP_FLOAT;
		else if (mmi_has_current_tapered(info, batt_ma,
						 zone->fcc_norm_ma)) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		}
	} else if (mmi->pres_chrg_step == STEP_NORM) {
		if (!zone->fcc_norm_ma)
			mmi->pres_chrg_step = STEP_FLOAT;
		else if ((batt_mv + HYST_STEP_MV) < zone->norm_mv) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_MAX;
		}
		else if ((batt_mv + HYST_STEP_MV/2) < max_fv_mv) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		} else if (mmi_has_current_tapered(info, batt_ma,
						   mmi->chrg_iterm)) {
			mmi->pres_chrg_step = STEP_FULL;
		}
	} else if (mmi->pres_chrg_step == STEP_FULL) {
		if (batt_mv < (max_fv_mv - HYST_STEP_MV * 2)) {
			mmi->chrg_taper_cnt = 0;
			mmi->pres_chrg_step = STEP_NORM;
		}
	} else if (mmi->pres_chrg_step == STEP_FLOAT) {
		if ((zone->fcc_norm_ma) ||
		    ((batt_mv + HYST_STEP_MV) < zone->norm_mv))
			mmi->pres_chrg_step = STEP_MAX;
		else if (mmi_has_current_tapered(info, batt_ma,
				   mmi->chrg_iterm))
			mmi->pres_chrg_step = STEP_STOP;

	}

	/* Take State actions */
	switch (mmi->pres_chrg_step) {
	case STEP_FLOAT:
	case STEP_MAX:
		if (!zone->norm_mv)
			target_fv = max_fv_mv + mmi->vfloat_comp_mv;
		else
			target_fv = zone->norm_mv + mmi->vfloat_comp_mv;
		target_fcc = zone->fcc_max_ma;
		break;
	case STEP_FULL:
		target_fv = max_fv_mv;
		target_fcc = -EINVAL;
		break;
	case STEP_NORM:
		target_fv = max_fv_mv + mmi->vfloat_comp_mv;
		target_fcc = zone->fcc_norm_ma;
		break;
	case STEP_NONE:
		target_fv = max_fv_mv;
		target_fcc = zone->fcc_norm_ma;
		break;
	case STEP_STOP:
		target_fv = max_fv_mv;
		target_fcc = -EINVAL;
		break;
	case STEP_DEMO:
		target_fv = DEMO_MODE_VOLTAGE;
		target_fcc = zone->fcc_max_ma;
		break;
	default:
		break;
	}

	mmi->target_fv = target_fv * 1000;

	mmi->chg_disable = (target_fcc < 0);

	mmi->target_fcc = ((target_fcc >= 0) ? (target_fcc * 1000) : 0);

	if (info->mmi.pres_temp_zone == ZONE_HOT) {
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else if (info->mmi.pres_temp_zone == ZONE_COLD) {
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_COLD;
	} else if (batt_temp >= WARM_TEMP) {
		if (info->mmi.pres_chrg_step == STEP_STOP)
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_OVERHEAT;
		else
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (batt_temp <= COOL_TEMP) {
		if (info->mmi.pres_chrg_step == STEP_STOP)
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_COLD;
		else
			info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	} else
		info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;

	pr_info("[%s]FV %d mV, FCC %d mA\n",
		 __func__, target_fv, target_fcc);
	pr_info("[%s]Step State = %s\n", __func__,
		stepchg_str[(int)mmi->pres_chrg_step]);
	pr_info("[%s]EFFECTIVE: FV = %d, CDIS = %d, FCC = %d, "
		"USBICL = %d, DEMO_DISCHARG = %d\n",
		__func__,
		mmi->target_fv,
		mmi->chg_disable,
		mmi->target_fcc,
		mmi->target_usb,
		mmi->demo_discharging);
	pr_info("[%s]adaptive charging:disable_ibat = %d, "
		"disable_ichg = %d, enable HZ = %d, "
		"charging disable = %d\n",
		__func__,
		mmi->adaptive_charging_disable_ibat,
		mmi->adaptive_charging_disable_ichg,
		mmi->charging_enable_hz,
		mmi->battery_charging_disable);
end_check:

	return;
}

static int parse_mmi_dt(struct mtk_charger *info, struct device *dev)
{
	struct device_node *node = dev->of_node;
	int rc = 0;
	int byte_len;
	int i;

	if (!node) {
		pr_info("[%s]mmi dtree info. missing\n",__func__);
		return -ENODEV;
	}

	if (of_find_property(node, "mmi,mmi-cycle-cv-steps", &byte_len)) {
		if ((byte_len / sizeof(u32)) % 2) {
			pr_err("[%s]DT error wrong mmi cycle batt_cv zones, byte_len = %d\n",
				__func__, byte_len);
			return -ENODEV;
		}

		info->mmi.cycle_cv_steps = (struct mmi_cycle_cv_steps *)
			devm_kzalloc(dev, byte_len, GFP_KERNEL);

		if (info->mmi.cycle_cv_steps == NULL)
			return -ENOMEM;

		info->mmi.num_cycle_cv_steps =
			byte_len / sizeof(struct mmi_cycle_cv_steps);

		rc = of_property_read_u32_array(node,
				"mmi,mmi-cycle-cv-steps",
				(u32 *)info->mmi.cycle_cv_steps,
				byte_len / sizeof(u32));
		if (rc < 0) {
			pr_err("[%s]Couldn't read mmi cycle cv steps rc = %d\n", __func__, rc);
			return rc;
		}
		pr_info("[%s]mmi cycle cv steps: Num: %d\n",
				__func__,
				info->mmi.num_cycle_cv_steps);
		for (i = 0; i < info->mmi.num_cycle_cv_steps; i++) {
			pr_info("[%s]mmi cycle cv steps: cycle > %d, delta_cv_mv = %d mV\n",
				__func__,
				info->mmi.cycle_cv_steps[i].cycle,
				info->mmi.cycle_cv_steps[i].delta_cv_mv);
		}
	} else {
		info->mmi.cycle_cv_steps = NULL;
		info->mmi.num_cycle_cv_steps = 0;
		pr_err("[%s]mmi cycle cv steps is not set\n", __func__);
	}

	if (of_find_property(node, "mmi,mmi-temp-zones", &byte_len)) {
		if ((byte_len / sizeof(u32)) % 4) {
			pr_err("[%s]DT error wrong mmi temp zones\n",__func__);
			return -ENODEV;
		}

		info->mmi.temp_zones = (struct mmi_temp_zone *)
			devm_kzalloc(dev, byte_len, GFP_KERNEL);

		if (info->mmi.temp_zones == NULL)
			return -ENOMEM;

		info->mmi.num_temp_zones =
			byte_len / sizeof(struct mmi_temp_zone);

		rc = of_property_read_u32_array(node,
				"mmi,mmi-temp-zones",
				(u32 *)info->mmi.temp_zones,
				byte_len / sizeof(u32));
		if (rc < 0) {
			pr_err("[%s]Couldn't read mmi temp zones rc = %d\n", __func__, rc);
			return rc;
		}
		pr_info("[%s]"
			"mmi temp zones: Num: %d\n", __func__, info->mmi.num_temp_zones);
		for (i = 0; i < info->mmi.num_temp_zones; i++) {
			pr_info("[%s]"
				"mmi temp zones: Zone %d, Temp %d C, "
				"Step Volt %d mV, Full Rate %d mA, "
				"Taper Rate %d mA\n", __func__, i,
				info->mmi.temp_zones[i].temp_c,
				info->mmi.temp_zones[i].norm_mv,
				info->mmi.temp_zones[i].fcc_max_ma,
				info->mmi.temp_zones[i].fcc_norm_ma);
		}
		info->mmi.pres_temp_zone = ZONE_NONE;
	} else {
		info->mmi.temp_zones = NULL;
		info->mmi.num_temp_zones = 0;
		pr_err("[%s]mmi temp zones is not set\n", __func__);
	}

	if (of_find_property(node, "mmi,mmi-ffc-zones", &byte_len)) {
		if ((byte_len / sizeof(u32)) % 3) {
			pr_err("DT error wrong mmi ffc zones\n");
			return -ENODEV;
		}

		info->mmi.ffc_zones = (struct mmi_ffc_zone *)
			devm_kzalloc(dev, byte_len, GFP_KERNEL);

		info->mmi.num_ffc_zones =
			byte_len / sizeof(struct mmi_ffc_zone);

		if (info->mmi.ffc_zones == NULL)
			return -ENOMEM;

		rc = of_property_read_u32_array(node,
				"mmi,mmi-ffc-zones",
				(u32 *)info->mmi.ffc_zones,
				byte_len / sizeof(u32));
		if (rc < 0) {
			pr_err("Couldn't read mmi ffc zones rc = %d\n", rc);
			return rc;
		}

		for (i = 0; i < info->mmi.num_ffc_zones; i++) {
			pr_err("FFC:Zone %d,Temp %d,Volt %d,Ich %d", i,
				 info->mmi.ffc_zones[i].temp,
				 info->mmi.ffc_zones[i].ffc_max_mv,
				 info->mmi.ffc_zones[i].ffc_chg_iterm);
		}
	} else
		info->mmi.ffc_zones = NULL;

	rc = of_property_read_u32(node, "mmi,iterm-ma",
				  &info->mmi.chrg_iterm);
	if (rc)
		info->mmi.chrg_iterm = 150;
	 info->mmi.back_chrg_iterm = info->mmi.chrg_iterm;

	info->mmi.enable_mux =
		of_property_read_bool(node, "mmi,enable-mux");

	info->mmi.wls_switch_en = of_get_named_gpio(node, "mmi,mux_wls_switch_en", 0);
	if(!gpio_is_valid(info->mmi.wls_switch_en))
		pr_err("mmi wls_switch_en is %d invalid\n", info->mmi.wls_switch_en );

	info->mmi.wls_boost_en = of_get_named_gpio(node, "mmi,mux_wls_boost_en", 0);
	if(!gpio_is_valid(info->mmi.wls_boost_en))
		pr_err("mmi wls_boost_en is %d invalid\n", info->mmi.wls_boost_en);

	info->mmi.enable_charging_limit =
		of_property_read_bool(node, "mmi,enable-charging-limit");

	rc = of_property_read_u32(node, "mmi,upper-limit-capacity",
				  &info->mmi.upper_limit_capacity);
	if (rc)
		info->mmi.upper_limit_capacity = 100;

	rc = of_property_read_u32(node, "mmi,lower-limit-capacity",
				  &info->mmi.lower_limit_capacity);
	if (rc)
		info->mmi.lower_limit_capacity = 0;

	rc = of_property_read_u32(node, "mmi,vfloat-comp-uv",
				  &info->mmi.vfloat_comp_mv);
	if (rc)
		info->mmi.vfloat_comp_mv = 0;
	info->mmi.vfloat_comp_mv /= 1000;

	rc = of_property_read_u32(node, "mmi,min-cp-therm-current-ua",
				  &info->mmi.min_therm_current_limit);
	if (rc)
		info->mmi.min_therm_current_limit = 2000000;

	return rc;
}


static int chg_reboot(struct notifier_block *nb,
			 unsigned long event, void *unused)
{
	struct mtk_charger *info = container_of(nb, struct mtk_charger,
						mmi.chg_reboot);
	union power_supply_propval val;
	int rc;

	pr_info("chg Reboot\n");
	if (!info) {
		pr_info("called before chip valid!\n");
		return NOTIFY_DONE;
	}

	if (info->mmi.factory_mode) {
		switch (event) {
		case SYS_POWER_OFF:
			aee_kernel_RT_Monitor_api_factory();
			info->is_suspend = true;
			/* Disable Factory Kill */
			info->disable_charger = true;
			/* Disable Charging */
			charger_dev_enable(info->chg1_dev, false);
			/* Suspend USB */
			charger_dev_enable_hz(info->chg1_dev, true);

			rc = mmi_get_prop_from_charger(info,
				POWER_SUPPLY_PROP_ONLINE, &val);
			while (rc >= 0 && val.intval) {
				msleep(100);
				rc = mmi_get_prop_from_charger(info,
					POWER_SUPPLY_PROP_ONLINE, &val);
				pr_info("Wait for VBUS to decay\n");
			}

			pr_info("VBUS UV wait 1 sec!\n");
			/* Delay 1 sec to allow more VBUS decay */
			msleep(1000);
			break;
		default:
			break;
		}
	}

	return NOTIFY_DONE;
}


static ssize_t factory_image_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid factory image mode value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	mmi_info->mmi.is_factory_image = (mode) ? true : false;

	return r ? r : count;
}

static ssize_t factory_image_mode_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = (mmi_info->mmi.is_factory_image) ? 1 : 0;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(factory_image_mode, 0644,
		factory_image_mode_show,
		factory_image_mode_store);

static ssize_t factory_charge_upper_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.upper_limit_capacity;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(factory_charge_upper, 0444,
		factory_charge_upper_show,
		NULL);

static ssize_t force_demo_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid demo  mode value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}
	mmi_info->mmi.chrg_taper_cnt = 0;

	if ((mode >= 35) && (mode <= 80))
		mmi_info->mmi.demo_mode = mode;
	else
		mmi_info->mmi.demo_mode = 35;

	return r ? r : count;
}

static ssize_t force_demo_mode_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.demo_mode;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(force_demo_mode, 0644,
		force_demo_mode_show,
		force_demo_mode_store);

static ssize_t force_max_chrg_temp_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("[%s]Invalid max temp value = %lu\n", __func__, mode);
		return -EINVAL;
	}

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	if ((mode >= MIN_MAX_TEMP_C) && (mode <= MAX_TEMP_C))
		mmi_info->mmi.max_chrg_temp = mode;
	else
		mmi_info->mmi.max_chrg_temp = MAX_TEMP_C;

	return r ? r : count;
}

static ssize_t force_max_chrg_temp_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int state;

	if (!mmi_info) {
		pr_err("[%s]mmi_info not valid\n", __func__);
		return -ENODEV;
	}

	state = mmi_info->mmi.max_chrg_temp;

	return scnprintf(buf, CHG_SHOW_MAX_SIZE, "%d\n", state);
}

static DEVICE_ATTR(force_max_chrg_temp, 0644,
		force_max_chrg_temp_show,
		force_max_chrg_temp_store);

void mmi_init(struct mtk_charger *info)
{
	int rc;

	if (!info)
		return;

	info->mmi.factory_mode = info->atm_enabled;

	info->mmi.is_factory_image = false;
	info->mmi.charging_limit_modes = CHARGING_LIMIT_UNKNOWN;
	info->mmi.adaptive_charging_disable_ibat = false;
	info->mmi.adaptive_charging_disable_ichg = false;
	info->mmi.charging_enable_hz = false;
	info->mmi.battery_charging_disable = false;

	rc = parse_mmi_dt(info, &info->pdev->dev);
	if (rc < 0)
		pr_info("[%s]Error getting mmi dt items rc = %d\n",__func__, rc);

	if(gpio_is_valid(info->mmi.wls_switch_en)) {
		rc  = devm_gpio_request_one(&info->pdev->dev, info->mmi.wls_switch_en,
				  GPIOF_OUT_INIT_LOW, "mux_wls_switch_en");
		if (rc  < 0)
			pr_err(" [%s] Failed to request wls_switch_en gpio, ret:%d", __func__, rc);
	}
	if(gpio_is_valid(info->mmi.wls_boost_en)) {
		rc  = devm_gpio_request_one(&info->pdev->dev, info->mmi.wls_boost_en,
				  GPIOF_OUT_INIT_LOW, "mux_wls_boost_en");
		if (rc  < 0)
			pr_err(" [%s] Failed to request wls_boost_en gpio, ret:%d", __func__, rc);
	}
	info->mmi.batt_health = POWER_SUPPLY_HEALTH_GOOD;

	info->mmi.chg_reboot.notifier_call = chg_reboot;
	info->mmi.chg_reboot.next = NULL;
	info->mmi.chg_reboot.priority = 1;
	rc = register_reboot_notifier(&info->mmi.chg_reboot);
	if (rc)
		pr_err("SMB register for reboot failed\n");

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_force_demo_mode);
	if (rc) {
		pr_err("[%s]couldn't create force_demo_mode\n", __func__);
	}

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_force_max_chrg_temp);
	if (rc) {
		pr_err("[%s]couldn't create force_max_chrg_temp\n", __func__);
	}

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_factory_image_mode);
	if (rc)
		pr_err("[%s]couldn't create factory_image_mode\n", __func__);

	rc = device_create_file(&info->pdev->dev,
				&dev_attr_factory_charge_upper);
	if (rc)
		pr_err("[%s]couldn't create factory_charge_upper\n", __func__);

	info->mmi.init_done = true;
}

#ifdef MTK_BASE
static void kpoc_power_off_check(struct mtk_charger *info)
{
	unsigned int boot_mode = info->bootmode;
	int vbus = 0;
	int counter = 0;
	/* 8 = KERNEL_POWER_OFF_CHARGING_BOOT */
	/* 9 = LOW_POWER_OFF_CHARGING_BOOT */
	if (boot_mode == 8 || boot_mode == 9) {
		vbus = get_vbus(info);
		if (vbus >= 0 && vbus < 2500 && !mtk_is_charger_on(info) && !info->pd_reset) {
			chr_err("Unplug Charger/USB in KPOC mode, vbus=%d, shutdown\n", vbus);
			while (1) {
				if (counter >= 20000) {
					chr_err("%s, wait too long\n", __func__);
					kernel_power_off();
					break;
				}
				if (info->is_suspend == false) {
					chr_err("%s, not in suspend, shutdown\n", __func__);
					kernel_power_off();
				} else {
					chr_err("%s, suspend! cannot shutdown\n", __func__);
					msleep(20);
				}
				counter++;
			}
		}
		charger_send_kpoc_uevent(info);
	}
}
#endif

#ifndef CONFIG_MOTO_CHARGER_SGM415XX
static void charger_status_check(struct mtk_charger *info)
{
	union power_supply_propval online, status;
	struct power_supply *chg_psy = NULL;
	int ret;
	bool charging = true;

	chg_psy = devm_power_supply_get_by_phandle(&info->pdev->dev,
						       "charger");
	if (IS_ERR_OR_NULL(chg_psy)) {
		chr_err("%s Couldn't get chg_psy\n", __func__);
	} else {
		ret = power_supply_get_property(chg_psy,
			POWER_SUPPLY_PROP_ONLINE, &online);
		online.intval = online.intval || info->wireless_online;

		ret = power_supply_get_property(chg_psy,
			POWER_SUPPLY_PROP_STATUS, &status);

		if (!online.intval)
			charging = false;
		else {
			if (status.intval == POWER_SUPPLY_STATUS_NOT_CHARGING)
				charging = false;
		}
	}
	if (charging != info->is_charging)
		power_supply_changed(info->psy1);
	info->is_charging = charging;
}
#endif

static char *dump_charger_type(int chg_type, int usb_type)
{
	switch (chg_type) {
	case POWER_SUPPLY_TYPE_UNKNOWN:
		return "none";
	case POWER_SUPPLY_TYPE_USB:
		if (usb_type == POWER_SUPPLY_USB_TYPE_SDP)
			return "usb";
		else
			return "nonstd";
	case POWER_SUPPLY_TYPE_USB_CDP:
		return "usb-h";
	case POWER_SUPPLY_TYPE_USB_DCP:
		return "std";
	//case POWER_SUPPLY_TYPE_USB_FLOAT:
	//	return "nonstd";
	default:
		return "unknown";
	}
}

static int charger_routine_thread(void *arg)
{
	struct mtk_charger *info = arg;
	unsigned long flags;
	unsigned int init_times = 3;
	static bool is_module_init_done;
	bool is_charger_on;
	int ret;
	int vbat_min, vbat_max;
	u32 chg_cv = 0;

	while (1) {
		ret = wait_event_interruptible(info->wait_que,
			(info->charger_thread_timeout == true));
		if (ret < 0) {
			chr_err("%s: wait event been interrupted(%d)\n", __func__, ret);
			continue;
		}

		while (is_module_init_done == false) {
			if (charger_init_algo(info) == true) {
				is_module_init_done = true;
				if (info->charger_unlimited) {
					info->enable_sw_safety_timer = false;
					charger_dev_enable_safety_timer(info->chg1_dev, false);
				}
			}
			else {
				if (init_times > 0) {
					chr_err("retry to init charger\n");
					init_times = init_times - 1;
					msleep(10000);
				} else {
					chr_err("holding to init charger\n");
					msleep(60000);
				}
			}
		}

		mutex_lock(&info->charger_lock);
		spin_lock_irqsave(&info->slock, flags);
		if (!info->charger_wakelock->active)
			__pm_stay_awake(info->charger_wakelock);
		spin_unlock_irqrestore(&info->slock, flags);
		info->charger_thread_timeout = false;

		info->battery_temp = get_battery_temperature(info);
		ret = charger_dev_get_adc(info->chg1_dev,
			ADC_CHANNEL_VBAT, &vbat_min, &vbat_max);
		ret = charger_dev_get_constant_voltage(info->chg1_dev, &chg_cv);

		if (vbat_min != 0)
			vbat_min = vbat_min / 1000;

		chr_err("Vbat=%d vbats=%d vbus:%d ibus:%d I=%d T=%d uisoc:%d type:%s>%s pd:%d swchg_ibat:%d cv:%d\n",
			get_battery_voltage(info),
			vbat_min,
			get_vbus(info),
			get_ibus(info),
			get_battery_current(info),
			info->battery_temp,
			get_uisoc(info),
			dump_charger_type(info->chr_type, info->usb_type),
			dump_charger_type(get_charger_type(info), get_usb_type(info)),
			info->pd_type, get_ibat(info), chg_cv);

		is_charger_on = mtk_is_charger_on(info);

		if (info->charger_thread_polling == true)
			mtk_charger_start_timer(info);

		check_battery_exist(info);
		check_dynamic_mivr(info);
		mmi_charger_check_status(info);
		charger_check_status(info);
#ifdef MTK_BASE
		kpoc_power_off_check(info);
#endif

		if (is_disable_charger(info) == false &&
			is_charger_on == true &&
			info->can_charging == true) {
			if (info->algo.do_algorithm)
				info->algo.do_algorithm(info);
#ifndef CONFIG_MOTO_CHARGER_SGM415XX
			sc_update(info);
			wakeup_sc_algo_cmd(&info->sc.data, SC_EVENT_CHARGING, 0);
			charger_status_check(info);
#endif
		} else {
			chr_debug("disable charging %d %d %d\n",
			    is_disable_charger(info), is_charger_on, info->can_charging);
#ifndef CONFIG_MOTO_CHARGER_SGM415XX
			sc_update(info);
			wakeup_sc_algo_cmd(&info->sc.data, SC_EVENT_STOP_CHARGING, 0);
#endif
		}
		mmi_updata_batt_status(info);
		spin_lock_irqsave(&info->slock, flags);
		__pm_relax(info->charger_wakelock);
		spin_unlock_irqrestore(&info->slock, flags);
		chr_debug("%s end , %d\n",
			__func__, info->charger_thread_timeout);
		mutex_unlock(&info->charger_lock);
	}

	return 0;
}


#ifdef CONFIG_PM
static int charger_pm_event(struct notifier_block *notifier,
			unsigned long pm_event, void *unused)
{
	ktime_t ktime_now;
	struct timespec64 now;
	struct mtk_charger *info;

	info = container_of(notifier,
		struct mtk_charger, pm_notifier);

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		info->is_suspend = true;
		chr_debug("%s: enter PM_SUSPEND_PREPARE\n", __func__);
		break;
	case PM_POST_SUSPEND:
		info->is_suspend = false;
		chr_debug("%s: enter PM_POST_SUSPEND\n", __func__);
		ktime_now = ktime_get_boottime();
		now = ktime_to_timespec64(ktime_now);

		if (timespec64_compare(&now, &info->endtime) >= 0 &&
			info->endtime.tv_sec != 0 &&
			info->endtime.tv_nsec != 0) {
			chr_err("%s: alarm timeout, wake up charger\n",
				__func__);
			__pm_relax(info->charger_wakelock);
			info->endtime.tv_sec = 0;
			info->endtime.tv_nsec = 0;
			_wake_up_charger(info);
		}
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}
#endif /* CONFIG_PM */

static enum alarmtimer_restart
	mtk_charger_alarm_timer_func(struct alarm *alarm, ktime_t now)
{
	struct mtk_charger *info =
	container_of(alarm, struct mtk_charger, charger_timer);
	ktime_t *time_p = info->timer_cb_duration;

	info->timer_cb_duration[0] = ktime_get_boottime();
	if (info->is_suspend == false) {
		chr_debug("%s: not suspend, wake up charger\n", __func__);
		info->timer_cb_duration[1] = ktime_get_boottime();
		_wake_up_charger(info);
		info->timer_cb_duration[6] = ktime_get_boottime();
	} else {
		chr_debug("%s: alarm timer timeout\n", __func__);
		__pm_stay_awake(info->charger_wakelock);
	}

	info->timer_cb_duration[7] = ktime_get_boottime();

	if (ktime_us_delta(time_p[7], time_p[0]) > 5000)
		chr_err("%s: delta_t: %ld %ld %ld %ld %ld %ld %ld (%ld)\n",
			__func__,
			ktime_us_delta(time_p[1], time_p[0]),
			ktime_us_delta(time_p[2], time_p[1]),
			ktime_us_delta(time_p[3], time_p[2]),
			ktime_us_delta(time_p[4], time_p[3]),
			ktime_us_delta(time_p[5], time_p[4]),
			ktime_us_delta(time_p[6], time_p[5]),
			ktime_us_delta(time_p[7], time_p[6]),
			ktime_us_delta(time_p[7], time_p[0]));

	return ALARMTIMER_NORESTART;
}

static void mtk_charger_init_timer(struct mtk_charger *info)
{
	alarm_init(&info->charger_timer, ALARM_BOOTTIME,
			mtk_charger_alarm_timer_func);
	mtk_charger_start_timer(info);

#ifdef CONFIG_PM
	if (register_pm_notifier(&info->pm_notifier))
		chr_err("%s: register pm failed\n", __func__);
#endif /* CONFIG_PM */
}

static int mtk_charger_setup_files(struct platform_device *pdev)
{
	int ret = 0;
	struct proc_dir_entry *battery_dir = NULL, *entry = NULL;
	struct mtk_charger *info = platform_get_drvdata(pdev);

	ret = device_create_file(&(pdev->dev), &dev_attr_sw_jeita);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_chr_type);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_enable_meta_current_limit);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_fast_chg_indicator);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_vbat_mon);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_Pump_Express);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_ADC_Charger_Voltage);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_ADC_Charging_Current);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_input_current);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_charger_log_level);
	if (ret)
		goto _out;

	/* Battery warning */
	ret = device_create_file(&(pdev->dev), &dev_attr_BatteryNotify);
	if (ret)
		goto _out;

	/* sysfs node */
	ret = device_create_file(&(pdev->dev), &dev_attr_enable_sc);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_sc_stime);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_sc_etime);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_sc_tuisoc);
	if (ret)
		goto _out;

	ret = device_create_file(&(pdev->dev), &dev_attr_sc_ibat_limit);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_mmi_vbats);
	if (ret)
		goto _out;
	ret = device_create_file(&(pdev->dev), &dev_attr_mmi_ibus);
	if (ret)
		goto _out;


	battery_dir = proc_mkdir("mtk_battery_cmd", NULL);
	if (!battery_dir) {
		chr_err("%s: mkdir /proc/mtk_battery_cmd failed\n", __func__);
		return -ENOMEM;
	}

	entry = proc_create_data("current_cmd", 0644, battery_dir,
			&mtk_chg_current_cmd_fops, info);
	if (!entry) {
		ret = -ENODEV;
		goto fail_procfs;
	}
	entry = proc_create_data("en_power_path", 0644, battery_dir,
			&mtk_chg_en_power_path_fops, info);
	if (!entry) {
		ret = -ENODEV;
		goto fail_procfs;
	}
	entry = proc_create_data("en_safety_timer", 0644, battery_dir,
			&mtk_chg_en_safety_timer_fops, info);
	if (!entry) {
		ret = -ENODEV;
		goto fail_procfs;
	}
	entry = proc_create_data("set_cv", 0644, battery_dir,
			&mtk_chg_set_cv_fops, info);
	if (!entry) {
		ret = -ENODEV;
		goto fail_procfs;
	}

	return 0;

fail_procfs:
	remove_proc_subtree("mtk_battery_cmd", NULL);
_out:
	return ret;
}


static void mot_chg_get_atm_mode(struct mtk_charger *info)
{
	const char *bootargs_ptr = NULL;
	char *bootargs_str = NULL;
	char *idx = NULL;
	char *kvpair = NULL;
	struct device_node *n = of_find_node_by_path("/chosen");
	size_t bootargs_ptr_len = 0;
	char *value = NULL;

	if (n == NULL)
		return;

	bootargs_ptr = (char *)of_get_property(n, "mmi,bootconfig", NULL);

	if (!bootargs_ptr) {
		chr_err("%s: failed to get mmi,bootconfig\n", __func__);
		goto err_putnode;
	}

	bootargs_ptr_len = strlen(bootargs_ptr);
	if (!bootargs_str) {
		/* Following operations need a non-const version of bootargs */
		bootargs_str = kzalloc(bootargs_ptr_len + 1, GFP_KERNEL);
		if (!bootargs_str)
			goto err_putnode;
	}
	strlcpy(bootargs_str, bootargs_ptr, bootargs_ptr_len + 1);

	idx = strnstr(bootargs_str, "androidboot.atm=", strlen(bootargs_str));
	if (idx) {
		kvpair = strsep(&idx, " ");
		if (kvpair)
			if (strsep(&kvpair, "=")) {
				value = strsep(&kvpair, "\n");
			}
	}
	if (value) {
		if (!strncmp(value, "enable", strlen("enable"))) {
			info->atm_enabled = true;
			factory_charging_enable = false;
		}
		chr_err("%s: value = %s  enable %d\n", __func__, value,info->atm_enabled);
	}
	kfree(bootargs_str);

err_putnode:
	of_node_put(n);
}

#ifdef MTK_BASE
void mtk_charger_get_atm_mode(struct mtk_charger *info)
{
	char atm_str[64] = {0};
	char *ptr = NULL, *ptr_e = NULL;
	char keyword[] = "androidboot.atm=";
	int size = 0;

	ptr = strstr(chg_get_cmd(), keyword);
	if (ptr != 0) {
		ptr_e = strstr(ptr, " ");
		if (ptr_e == 0)
			goto end;

		size = ptr_e - (ptr + strlen(keyword));
		if (size <= 0)
			goto end;
		strncpy(atm_str, ptr + strlen(keyword), size);
		atm_str[size] = '\0';
		chr_err("%s: atm_str: %s\n", __func__, atm_str);

		if (!strncmp(atm_str, "enable", strlen("enable"))) {
			info->atm_enabled = true;
			factory_charging_enable = false;
		}
	}
end:
	chr_err("%s: atm_enabled = %d\n", __func__, info->atm_enabled);
}
#endif

static int psy_charger_property_is_writeable(struct power_supply *psy,
					       enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		return 1;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return 1;
	case POWER_SUPPLY_PROP_INPUT_POWER_LIMIT:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property charger_psy_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_INPUT_POWER_LIMIT,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int psy_charger_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val)
{
	struct mtk_charger *info;
	struct charger_device *chg;
	struct charger_data *pdata;
	int ret = 0;
	struct chg_alg_device *alg = NULL;

	info = (struct mtk_charger *)power_supply_get_drvdata(psy);
	if (info == NULL) {
		chr_err("%s: get info failed\n", __func__);
		return -EINVAL;
	}
	chr_debug("%s psp:%d\n", __func__, psp);

	if (info->psy1 != NULL &&
		info->psy1 == psy)
		chg = info->chg1_dev;
	else if (info->psy2 != NULL &&
		info->psy2 == psy)
		chg = info->chg2_dev;
	else if (info->psy_dvchg1 != NULL && info->psy_dvchg1 == psy)
		chg = info->dvchg1_dev;
	else if (info->psy_dvchg2 != NULL && info->psy_dvchg2 == psy)
		chg = info->dvchg2_dev;
	else {
		chr_err("%s fail\n", __func__);
		return 0;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (chg == info->dvchg1_dev) {
			val->intval = false;
			alg = get_chg_alg_by_name("pe5");
			if (alg == NULL)
				chr_err("get pe5 fail\n");
			else {
				ret = chg_alg_is_algo_ready(alg);
				if (ret == ALG_RUNNING)
					val->intval = true;
			}
			break;
		}

		val->intval = is_charger_exist(info);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		if (chg != NULL)
			val->intval = true;
		else
			val->intval = false;
		break;
#ifdef CONFIG_MOTO_CHARGER_SGM415XX
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = get_charger_type(info);
		break;
#endif
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = info->enable_hv_charging;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = get_vbus(info);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		if (chg == info->chg1_dev)
			val->intval =
				info->chg_data[CHG1_SETTING].junction_temp_max * 10;
		else if (chg == info->chg2_dev)
			val->intval =
				info->chg_data[CHG2_SETTING].junction_temp_max * 10;
		else if (chg == info->dvchg1_dev) {
			pdata = &info->chg_data[DVCHG1_SETTING];
			val->intval = pdata->junction_temp_max;
		} else if (chg == info->dvchg2_dev) {
			pdata = &info->chg_data[DVCHG2_SETTING];
			val->intval = pdata->junction_temp_max;
		} else
			val->intval = -127;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = get_charger_charging_current(info, chg);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = get_charger_input_current(info, chg);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = info->chr_type;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_BOOT:
		val->intval = get_charger_zcv(info, chg);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mtk_charger_enable_power_path(struct mtk_charger *info,
	int idx, bool en)
{
	int ret = 0;
	bool is_en = true;
	struct charger_device *chg_dev = NULL;

	if (!info)
		return -EINVAL;

	switch (idx) {
	case CHG1_SETTING:
		chg_dev = get_charger_by_name("primary_chg");
		break;
	case CHG2_SETTING:
		chg_dev = get_charger_by_name("secondary_chg");
		break;
	default:
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(chg_dev)) {
		chr_err("%s: chg_dev not found\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&info->pp_lock[idx]);
	info->enable_pp[idx] = en;

	if (info->force_disable_pp[idx])
		goto out;

	ret = charger_dev_is_powerpath_enabled(chg_dev, &is_en);
	if (ret < 0) {
		chr_err("%s: get is power path enabled failed\n", __func__);
		goto out;
	}
	if (is_en == en) {
		chr_err("%s: power path is already en = %d\n", __func__, is_en);
		goto out;
	}

	pr_info("%s: enable power path = %d\n", __func__, en);
	ret = charger_dev_enable_powerpath(chg_dev, en);
out:
	mutex_unlock(&info->pp_lock[idx]);
	return ret;
}

static int mtk_charger_force_disable_power_path(struct mtk_charger *info,
	int idx, bool disable)
{
	int ret = 0;
	struct charger_device *chg_dev = NULL;

	if (!info)
		return -EINVAL;

	switch (idx) {
	case CHG1_SETTING:
		chg_dev = get_charger_by_name("primary_chg");
		break;
	case CHG2_SETTING:
		chg_dev = get_charger_by_name("secondary_chg");
		break;
	default:
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(chg_dev)) {
		chr_err("%s: chg_dev not found\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&info->pp_lock[idx]);

	if (disable == info->force_disable_pp[idx])
		goto out;

	info->force_disable_pp[idx] = disable;
	ret = charger_dev_enable_powerpath(chg_dev,
		info->force_disable_pp[idx] ? false : info->enable_pp[idx]);
out:
	mutex_unlock(&info->pp_lock[idx]);
	return ret;
}

int psy_charger_set_property(struct power_supply *psy,
			enum power_supply_property psp,
			const union power_supply_propval *val)
{
	struct mtk_charger *info;
	int idx;

	chr_err("%s: prop:%d %d\n", __func__, psp, val->intval);

	info = (struct mtk_charger *)power_supply_get_drvdata(psy);

	if (info == NULL) {
		chr_err("%s: failed to get info\n", __func__);
		return -EINVAL;
	}

	if (info->psy1 != NULL &&
		info->psy1 == psy)
		idx = CHG1_SETTING;
	else if (info->psy2 != NULL &&
		info->psy2 == psy)
		idx = CHG2_SETTING;
	else if (info->psy_dvchg1 != NULL && info->psy_dvchg1 == psy)
		idx = DVCHG1_SETTING;
	else if (info->psy_dvchg2 != NULL && info->psy_dvchg2 == psy)
		idx = DVCHG2_SETTING;
	else {
		chr_err("%s fail\n", __func__);
		return 0;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (val->intval > 0)
			info->enable_hv_charging = true;
		else
			info->enable_hv_charging = false;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		info->chg_data[idx].thermal_charging_current_limit =
			val->intval;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		info->chg_data[idx].thermal_input_current_limit =
			val->intval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		if (val->intval > 0 && !info->wireless_online)
			mtk_charger_enable_power_path(info, idx, false);
		else
			mtk_charger_enable_power_path(info, idx, true);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		if (val->intval > 0)
			mtk_charger_force_disable_power_path(info, idx, true);
		else
			mtk_charger_force_disable_power_path(info, idx, false);
		break;
	case POWER_SUPPLY_PROP_INPUT_POWER_LIMIT:
		info->mmi.adaptive_charging_disable_ichg = !!val->intval;
		pr_info("%s: adaptive charging disable ichg %d\n", __func__,
			info->mmi.adaptive_charging_disable_ichg);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		info->mmi.adaptive_charging_disable_ibat =  !!val->intval;
		pr_info("%s: adaptive charging disable ibat %d\n", __func__,
			info->mmi.adaptive_charging_disable_ibat);
		break;
#ifdef CONFIG_MOTO_CHARGER_SGM415XX
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		info->chg_data[idx].cp_ichg_limit = val->intval;
		break;
#endif
	default:
		return -EINVAL;
	}
	_wake_up_charger(info);

	return 0;
}

static void mtk_charger_external_power_changed(struct power_supply *psy)
{
	struct mtk_charger *info;
	union power_supply_propval prop = {0};
	union power_supply_propval prop2 = {0};
	union power_supply_propval vbat0 = {0};
	struct power_supply *wl_psy = NULL;
	struct power_supply *chg_psy = NULL;
	int ret;

	info = (struct mtk_charger *)power_supply_get_drvdata(psy);
        wl_psy = power_supply_get_by_name("wireless");
        if (wl_psy == NULL || IS_ERR(wl_psy)) {
                chr_err("%s Couldn't get wl_psy\n", __func__);
                prop.intval = 0;
        } else {
                ret = power_supply_get_property(wl_psy,
                        POWER_SUPPLY_PROP_ONLINE, &prop);
                info->wireless_online = prop.intval;
                if (1 == prop.intval) {
                        pr_notice("%s event, name:%s online:%d\n", __func__,
                                psy->desc->name, prop.intval);
                }
        }

	if (info == NULL) {
		pr_notice("%s: failed to get info\n", __func__);
		return;
	}
	chg_psy = info->chg_psy;

	if (chg_psy == NULL) {
		pr_notice("%s Couldn't get chg_psy\n", __func__);
		chg_psy = devm_power_supply_get_by_phandle(&info->pdev->dev,
						       "charger");
		if (IS_ERR_OR_NULL(chg_psy)) {
			chg_psy = devm_power_supply_get_by_phandle(&info->pdev->dev,
						       "charger_2nd");
		}
		chr_err("%s charger psy name: %s\n", __func__, chg_psy->desc->name);
		info->chg_psy = chg_psy;
	} else {
		ret = power_supply_get_property(chg_psy,
			POWER_SUPPLY_PROP_ONLINE, &prop);
		ret = power_supply_get_property(chg_psy,
			POWER_SUPPLY_PROP_USB_TYPE, &prop2);
		ret = power_supply_get_property(chg_psy,
			POWER_SUPPLY_PROP_ENERGY_EMPTY, &vbat0);
	}

	if (info->vbat0_flag != vbat0.intval) {
		if (vbat0.intval) {
			info->enable_vbat_mon = false;
			charger_dev_enable_6pin_battery_charging(info->chg1_dev, false);
		} else
			info->enable_vbat_mon = info->enable_vbat_mon_bak;

		info->vbat0_flag = vbat0.intval;
	}

	pr_notice("%s event, name:%s online:%d type:%d vbus:%d\n", __func__,
		psy->desc->name, prop.intval, prop2.intval,
		get_vbus(info));
	/*icl been set to 2A when factory mode boot,for wireless,2A more than OCP(1.5A),
		 will report OCP before wireless power transfer interrupt.here set to max
		 current wireless chip could support when wireless cable insert.
	*/
	if(info->wireless_online && info->mmi.factory_mode)
		mtk_charger_tcmd_set_usb_current((void *)info, info->data.wireless_factory_max_input_current);

	_wake_up_charger(info);
}

int notify_adapter_event(struct notifier_block *notifier,
			unsigned long evt, void *val)
{
	struct mtk_charger *pinfo = NULL;

	chr_err("%s %lu\n", __func__, evt);

	pinfo = container_of(notifier,
		struct mtk_charger, pd_nb);

	switch (evt) {
	case  MTK_PD_CONNECT_NONE:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify Detach\n");
		pinfo->pd_type = MTK_PD_CONNECT_NONE;
		pinfo->pd_reset = false;
		mutex_unlock(&pinfo->pd_lock);
		mtk_chg_alg_notify_call(pinfo, EVT_DETACH, 0);
		/* reset PE40 */
		break;

	case MTK_PD_CONNECT_HARD_RESET:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify HardReset\n");
		pinfo->pd_type = MTK_PD_CONNECT_NONE;
		pinfo->pd_reset = true;
		mutex_unlock(&pinfo->pd_lock);
		mtk_chg_alg_notify_call(pinfo, EVT_HARDRESET, 0);
		_wake_up_charger(pinfo);
		/* reset PE40 */
		break;

	case MTK_PD_CONNECT_PE_READY_SNK:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify fixe voltage ready\n");
		pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK;
		pinfo->pd_reset = false;
		mutex_unlock(&pinfo->pd_lock);
		/* PD is ready */
		break;

	case MTK_PD_CONNECT_PE_READY_SNK_PD30:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify PD30 ready\r\n");
		pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK_PD30;
		pinfo->pd_reset = false;
		mutex_unlock(&pinfo->pd_lock);
		/* PD30 is ready */
		break;

	case MTK_PD_CONNECT_PE_READY_SNK_APDO:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify APDO Ready\n");
		pinfo->pd_type = MTK_PD_CONNECT_PE_READY_SNK_APDO;
		pinfo->pd_reset = false;
		mutex_unlock(&pinfo->pd_lock);
		/* PE40 is ready */
		_wake_up_charger(pinfo);
		break;

	case MTK_PD_CONNECT_TYPEC_ONLY_SNK:
		mutex_lock(&pinfo->pd_lock);
		chr_err("PD Notify Type-C Ready\n");
		pinfo->pd_type = MTK_PD_CONNECT_TYPEC_ONLY_SNK;
		pinfo->pd_reset = false;
		mutex_unlock(&pinfo->pd_lock);
		/* type C is ready */
		_wake_up_charger(pinfo);
		break;
	case MTK_TYPEC_WD_STATUS:
		chr_err("wd status = %d\n", *(bool *)val);
		pinfo->water_detected = *(bool *)val;
		if (pinfo->water_detected == true)
			pinfo->notify_code |= CHG_TYPEC_WD_STATUS;
		else
			pinfo->notify_code &= ~CHG_TYPEC_WD_STATUS;
		mtk_chgstat_notify(pinfo);
		break;
	}
	return NOTIFY_DONE;
}

int chg_alg_event(struct notifier_block *notifier,
			unsigned long event, void *data)
{
	chr_err("%s: evt:%lu\n", __func__, event);

	return NOTIFY_DONE;
}

static char *mtk_charger_supplied_to[] = {
	"battery"
};

/*==================moto chg tcmd interface======================*/
static int  mtk_charger_tcmd_set_chg_enable(void *input, int  val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	val = !!val;
	charger_dev_enable(cm->chg1_dev, val);

	factory_charging_enable = val;
	val = val ? EVENT_RECHARGE : EVENT_DISCHARGE;
	_wake_up_charger(cm);
	//ret = mtk_charger_notifier(cm, val);
	//charger_dev_do_event(info->chg1_dev,
	//				val, 0);
	return ret;
}

static int  mtk_charger_tcmd_set_usb_enable(void *input, int  val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret;

	val = !!val;
	ret = charger_dev_enable_powerpath(cm->chg1_dev, val);

	return ret;
}

static int  mtk_charger_tcmd_set_chg_current(void *input, int  val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ibat = val * 1000;
	ret = charger_dev_set_charging_current(cm->chg1_dev, cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ibat);

	return ret;
}

static int  mtk_charger_tcmd_get_chg_current(void *input, int* val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	*val = cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ibat / 1000;

	return ret;
}

static int  mtk_charger_tcmd_set_usb_current(void *input, int  val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ichg = val * 1000;
	ret = charger_dev_set_input_current(cm->chg1_dev, cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ichg);

	return ret;
}

static int  mtk_charger_tcmd_get_usb_current(void *input, int* val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	*val = cm->chg_data[CHG1_SETTING].moto_chg_tcmd_ichg / 1000;

	return ret;
}

static int  mtk_charger_tcmd_get_usb_voltage(void *input, int* val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	*val = get_vbus(cm); /* mV */
	*val *= 1000; /*convert to uV*/

	return ret;
}

static int  mtk_charger_tcmd_get_charger_type(void *input, int* val)
{
	struct mtk_charger *cm = (struct mtk_charger *)input;
	int ret = 0;

	*val = cm->chr_type;

	return ret;
}

static int  mtk_charger_tcmd_register(struct mtk_charger *cm)
{
	int ret;

	cm->chg_tcmd_client.data = cm;
	cm->chg_tcmd_client.client_id = MOTO_CHG_TCMD_CLIENT_CHG;

	cm->chg_tcmd_client.set_chg_enable = mtk_charger_tcmd_set_chg_enable;
	cm->chg_tcmd_client.set_usb_enable = mtk_charger_tcmd_set_usb_enable;
	cm->chg_tcmd_client.get_chg_current = mtk_charger_tcmd_get_chg_current;
	cm->chg_tcmd_client.set_chg_current = mtk_charger_tcmd_set_chg_current;
	cm->chg_tcmd_client.get_usb_current = mtk_charger_tcmd_get_usb_current;
	cm->chg_tcmd_client.set_usb_current = mtk_charger_tcmd_set_usb_current;

	cm->chg_tcmd_client.get_usb_voltage = mtk_charger_tcmd_get_usb_voltage;

	cm->chg_tcmd_client.get_charger_type = mtk_charger_tcmd_get_charger_type;

	ret = moto_chg_tcmd_register(&cm->chg_tcmd_client);

	return ret;
}
/*==================================================*/
static int mtk_charger_probe(struct platform_device *pdev)
{
	struct mtk_charger *info = NULL;
	int i;
	char *name = NULL;
	struct netlink_kernel_cfg cfg = {
		.input = chg_nl_data_handler,
	};

	chr_err("%s: starts\n", __func__);

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	platform_set_drvdata(pdev, info);
	info->pdev = pdev;

	info->log_level = CHRLOG_ERROR_LEVEL;
	mtk_charger_parse_dt(info, &pdev->dev);

	mutex_init(&info->cable_out_lock);
	mutex_init(&info->charger_lock);
	mutex_init(&info->pd_lock);
	mutex_init(&info->mmi_mux_lock);
	for (i = 0; i < CHG2_SETTING + 1; i++) {
		mutex_init(&info->pp_lock[i]);
		info->force_disable_pp[i] = false;
		info->enable_pp[i] = true;
	}
	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s",
		"charger suspend wakelock");
	info->charger_wakelock =
		wakeup_source_register(NULL, name);
	spin_lock_init(&info->slock);

	init_waitqueue_head(&info->wait_que);
	info->polling_interval = CHARGING_INTERVAL;
	mtk_charger_init_timer(info);
#ifdef CONFIG_PM
	info->pm_notifier.notifier_call = charger_pm_event;
#endif /* CONFIG_PM */
	srcu_init_notifier_head(&info->evt_nh);
	mtk_charger_setup_files(pdev);
#ifdef MTK_BASE
	mtk_charger_get_atm_mode(info);
#else
	mot_chg_get_atm_mode(info);
#endif
	for (i = 0; i < CHGS_SETTING_MAX; i++) {
		info->chg_data[i].thermal_charging_current_limit = -1;
		info->chg_data[i].thermal_input_current_limit = -1;
		info->chg_data[i].input_current_limit_by_aicl = -1;
		info->chg_data[i].moto_chg_tcmd_ichg = -1;
		info->chg_data[i].moto_chg_tcmd_ibat = -1;
#ifdef CONFIG_MOTO_CHARGER_SGM415XX
		info->chg_data[i].cp_ichg_limit= -1;
#endif
	}
	info->enable_hv_charging = true;

	info->psy_desc1.name = "mtk-master-charger";
	info->psy_desc1.type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->psy_desc1.properties = charger_psy_properties;
	info->psy_desc1.num_properties = ARRAY_SIZE(charger_psy_properties);
	info->psy_desc1.get_property = psy_charger_get_property;
	info->psy_desc1.set_property = psy_charger_set_property;
	info->psy_desc1.property_is_writeable =
			psy_charger_property_is_writeable;
	info->psy_desc1.external_power_changed =
		mtk_charger_external_power_changed;
	info->psy_cfg1.drv_data = info;
	info->psy_cfg1.supplied_to = mtk_charger_supplied_to;
	info->psy_cfg1.num_supplicants = ARRAY_SIZE(mtk_charger_supplied_to);
	info->psy1 = power_supply_register(&pdev->dev, &info->psy_desc1,
			&info->psy_cfg1);

	info->chg_psy = devm_power_supply_get_by_phandle(&pdev->dev,
		"charger");
	if (IS_ERR_OR_NULL(info->chg_psy)) {
		info->chg_psy = devm_power_supply_get_by_phandle(&pdev->dev,
					       "charger_2nd");
		if (IS_ERR_OR_NULL(info->chg_psy)) {
			chr_err("%s: devm power fail to get chg_psy\n", __func__);
		}
	}
	chr_err("%s charger psy name: %s\n", __func__, info->chg_psy->desc->name);
	
	info->bat_psy = devm_power_supply_get_by_phandle(&pdev->dev,
		"gauge");
	if (IS_ERR_OR_NULL(info->bat_psy))
		chr_err("%s: devm power fail to get bat_psy\n", __func__);

	if (IS_ERR(info->psy1))
		chr_err("register psy1 fail:%ld\n",
			PTR_ERR(info->psy1));

	info->psy_desc2.name = "mtk-slave-charger";
	info->psy_desc2.type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->psy_desc2.properties = charger_psy_properties;
	info->psy_desc2.num_properties = ARRAY_SIZE(charger_psy_properties);
	info->psy_desc2.get_property = psy_charger_get_property;
	info->psy_desc2.set_property = psy_charger_set_property;
	info->psy_desc2.property_is_writeable =
			psy_charger_property_is_writeable;
	info->psy_cfg2.drv_data = info;
	info->psy2 = power_supply_register(&pdev->dev, &info->psy_desc2,
			&info->psy_cfg2);

	if (IS_ERR(info->psy2))
		chr_err("register psy2 fail:%ld\n",
			PTR_ERR(info->psy2));

	info->psy_dvchg_desc1.name = "mtk-mst-div-chg";
	info->psy_dvchg_desc1.type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->psy_dvchg_desc1.properties = charger_psy_properties;
	info->psy_dvchg_desc1.num_properties =
		ARRAY_SIZE(charger_psy_properties);
	info->psy_dvchg_desc1.get_property = psy_charger_get_property;
	info->psy_dvchg_desc1.set_property = psy_charger_set_property;
	info->psy_dvchg_desc1.property_is_writeable =
		psy_charger_property_is_writeable;
	info->psy_dvchg_cfg1.drv_data = info;
	info->psy_dvchg1 = power_supply_register(&pdev->dev,
						 &info->psy_dvchg_desc1,
						 &info->psy_dvchg_cfg1);
	if (IS_ERR(info->psy_dvchg1))
		chr_err("register psy dvchg1 fail:%ld\n",
			PTR_ERR(info->psy_dvchg1));

	info->psy_dvchg_desc2.name = "mtk-slv-div-chg";
	info->psy_dvchg_desc2.type = POWER_SUPPLY_TYPE_UNKNOWN;
	info->psy_dvchg_desc2.properties = charger_psy_properties;
	info->psy_dvchg_desc2.num_properties =
		ARRAY_SIZE(charger_psy_properties);
	info->psy_dvchg_desc2.get_property = psy_charger_get_property;
	info->psy_dvchg_desc2.set_property = psy_charger_set_property;
	info->psy_dvchg_desc2.property_is_writeable =
		psy_charger_property_is_writeable;
	info->psy_dvchg_cfg2.drv_data = info;
	info->psy_dvchg2 = power_supply_register(&pdev->dev,
						 &info->psy_dvchg_desc2,
						 &info->psy_dvchg_cfg2);
	if (IS_ERR(info->psy_dvchg2))
		chr_err("register psy dvchg2 fail:%ld\n",
			PTR_ERR(info->psy_dvchg2));

	info->log_level = CHRLOG_ERROR_LEVEL;

	info->pd_adapter = get_adapter_by_name("pd_adapter");
	if (!info->pd_adapter)
		chr_err("%s: No pd adapter found\n", __func__);
	else {
		info->pd_nb.notifier_call = notify_adapter_event;
		register_adapter_device_notifier(info->pd_adapter,
						 &info->pd_nb);
	}

	info->sc.daemo_nl_sk = netlink_kernel_create(&init_net, NETLINK_CHG, &cfg);

	if (info->sc.daemo_nl_sk == NULL)
		chr_err("sc netlink_kernel_create error id:%d\n", NETLINK_CHG);
	else
		chr_err("sc_netlink_kernel_create success id:%d\n", NETLINK_CHG);
	sc_init(&info->sc);

	info->chg_alg_nb.notifier_call = chg_alg_event;

	info->enable_meta_current_limit = 1;
	info->is_charging = false;
	info->safety_timer_cmd = -1;

	/* 8 = KERNEL_POWER_OFF_CHARGING_BOOT */
	/* 9 = LOW_POWER_OFF_CHARGING_BOOT */
#ifndef CONFIG_MOTO_CHARGER_SGM415XX
	if (info != NULL && info->bootmode != 8 && info->bootmode != 9 && info->atm_enabled != true)
		mtk_charger_force_disable_power_path(info, CHG1_SETTING, true);
#endif
	mtk_charger_tcmd_register(info);
	mmi_info = info;
	mmi_init(info);
	kthread_run(charger_routine_thread, info, "charger_thread");

	return 0;
}

static int mtk_charger_remove(struct platform_device *dev)
{
	return 0;
}

static void mtk_charger_shutdown(struct platform_device *dev)
{
	struct mtk_charger *info = platform_get_drvdata(dev);
	int i;

	for (i = 0; i < MAX_ALG_NO; i++) {
		if (info->alg[i] == NULL)
			continue;
		chg_alg_stop_algo(info->alg[i]);
	}
}

static const struct of_device_id mtk_charger_of_match[] = {
	{.compatible = "mediatek,charger",},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_charger_of_match);

struct platform_device mtk_charger_device = {
	.name = "charger",
	.id = -1,
};

static struct platform_driver mtk_charger_driver = {
	.probe = mtk_charger_probe,
	.remove = mtk_charger_remove,
	.shutdown = mtk_charger_shutdown,
	.driver = {
		   .name = "charger",
		   .of_match_table = mtk_charger_of_match,
	},
};

static int __init mtk_charger_init(void)
{
	return platform_driver_register(&mtk_charger_driver);
}
module_init(mtk_charger_init);

static void __exit mtk_charger_exit(void)
{
	platform_driver_unregister(&mtk_charger_driver);
}
module_exit(mtk_charger_exit);


MODULE_AUTHOR("wy.chuang <wy.chuang@mediatek.com>");
MODULE_DESCRIPTION("MTK Charger Driver");
MODULE_LICENSE("GPL");
