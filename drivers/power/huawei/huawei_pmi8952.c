
#ifdef CONFIG_HUAWEI_KERNEL

static void hw_usb_switch_work(struct work_struct *work)
{
    bool src_detected = false;
    struct smbchg_chip *chip = hw_data.chip;
    if (!chip) {
        /* driver is initializing, delayed 1s */
        pr_err("driver is initializing\n");
        return;
    }

    src_detected = is_src_detect_high(chip);
    if (src_detected && atomic_read(&usb_switch_done)) {
        pr_err("%s: rerun apsd\n", __func__);
        /* rerun APSD */
        rerun_apsd(chip);
    }
}

static int usb_switch_callback(struct notifier_block *nb,
            unsigned long action, void *data)
{
    if (USB_SWITCH_ATTACH == action) {
        atomic_set(&usb_switch_done, 1);
        /* schedule usb switch check work */
        if (1 == atomic_read(&usb_switch_ready))
        {
            schedule_delayed_work(&usb_switch_work, 0);
            atomic_set(&usb_switch_ready, 0);
        }
        return 0;
    }

    atomic_set(&usb_switch_done, 0);
    return 0;
}

static struct notifier_block usb_switch_nblk = {
    .notifier_call = usb_switch_callback,
};

/*
 * jeita support for huawei
 * 1. use hardware jeita interrupt when temperature range
 * changed
 * 2. use software jeita current/voltage setting
 */
static int huawei_smbchg_set_fastchg_current_jeita(struct smbchg_chip *chip, int current_ma)
{
	int rc = 0;
	bool enable = current_ma > 0 ? true : false;

	pr_smb(PR_STATUS, "User setting FCC to %d\n", current_ma);
	rc = vote(chip->fcc_votable, HW_JEITA_VOTER, enable, current_ma);
	if (rc < 0)
		pr_err("Couldn't vote en rc %d\n", rc);
	return rc;
}

static void huawei_set_jeita_param(struct work_struct *work)
{
    int state, saved_volt;
    struct smbchg_chip *chip = hw_data.chip;

    if (!atomic_read(&loaded_batterydata)) {
        pr_info("need to wait for batterydata loaded!\n");
        return;
    }

    state = get_prop_batt_health(chip);
#ifdef CONFIG_HLTHERM_RUNTEST
    state = POWER_SUPPLY_HEALTH_GOOD;
#else
    if (chg_no_temp_limit)
        state = POWER_SUPPLY_HEALTH_GOOD;
#endif

    switch (state) {
    case POWER_SUPPLY_HEALTH_WARM:
        if (!hw_data.jeita_warm_state) {
			saved_volt = chip->vfloat_mv;
            smbchg_float_voltage_set(chip, hw_data.vfloat_batt_warm);
			huawei_smbchg_set_fastchg_current_jeita(chip,
                        hw_data.fastchg_current_batt_warm);
            hw_data.saved_vfloat_mv = saved_volt;
            hw_data.jeita_warm_state = true;
            hw_data.jeita_volt_limit = true;
        }
        break;
    case POWER_SUPPLY_HEALTH_COOL:
        if (!hw_data.jeita_cool_state) {
			huawei_smbchg_set_fastchg_current_jeita(chip,
                        hw_data.fastchg_current_batt_cool);
            hw_data.jeita_cool_state = true;
        }
        break;
    case POWER_SUPPLY_HEALTH_GOOD:
        if (hw_data.jeita_warm_state) {
            smbchg_float_voltage_set(chip, hw_data.saved_vfloat_mv);
			huawei_smbchg_set_fastchg_current_jeita(chip,
                        chip->cfg_fastchg_current_ma);
            hw_data.jeita_warm_state = false;
            hw_data.jeita_volt_limit = false;
        }
        if (hw_data.jeita_cool_state) {
			huawei_smbchg_set_fastchg_current_jeita(chip,
                        chip->cfg_fastchg_current_ma);
            hw_data.jeita_cool_state = false;
        }
        break;
    default:
        break;
    }
}

#ifndef CONFIG_HLTHERM_RUNTEST
static int chg_no_temp_limit_set(const char *val, const struct kernel_param *kp)
{
    struct power_supply *batt_psy;
    struct smbchg_chip *chip;
    int rc;
    u8 enable;

    batt_psy = power_supply_get_by_name("battery");
    if (!batt_psy) {
        pr_err("batt psy not found!\n");
        return 0;
    }
    chip = container_of(batt_psy, struct smbchg_chip, batt_psy);

    rc = param_set_int(val, kp);
    if (rc) {
        pr_err("Unable to set chg_no_temp_limit: %d\n", rc);
        return rc;
    }

	/* configure jeita temperature hard limit */
    enable = (chip->jeita_temp_hard_limit && !chg_no_temp_limit)
                ? 0 : JEITA_TEMP_HARD_LIMIT_BIT;
	rc = smbchg_sec_masked_write(chip,
		chip->chgr_base + CHGR_CCMP_CFG,
		JEITA_TEMP_HARD_LIMIT_BIT, enable);
	if (rc < 0) {
		dev_err(chip->dev,
			"Couldn't set jeita temp hard limit rc = %d\n",
			rc);
		return rc;
	}

    /* schedule jeita work */
    schedule_delayed_work(&hw_data.jeita_work, 0);

    return 0;
}
#endif

static int chg_usb_current_set(const char *val, const struct kernel_param *kp)
{
	struct power_supply *batt_psy;
    union power_supply_propval prop = {0, };
	struct smbchg_chip *chip;
	enum power_supply_type usb_supply_type;
	char *usb_type_name = "null";
	int current_limit, rc;

	batt_psy = power_supply_get_by_name("battery");
	if (!batt_psy) {
		pr_err("batt psy not found!\n");
		return 0;
	}
	chip = container_of(batt_psy, struct smbchg_chip, batt_psy);

	rc = param_set_int(val, kp);
	if (rc) {
		pr_err("Unable to set chg_usb_current: %d\n", rc);
		return rc;
	}

	/* change the current if needed */
	read_usb_type(chip, &usb_type_name, &usb_supply_type);
	if (POWER_SUPPLY_TYPE_USB == usb_supply_type) {
		if (chg_usb_current > 0) {
			rc = smbchg_masked_write(chip,
						chip->usb_chgpth_base + CMD_IL,
						ICL_OVERRIDE_BIT, ICL_OVERRIDE_BIT);
			if (rc < 0)
				pr_err("Couldn't set override rc = %d\n", rc);
			smbchg_set_usb_current_max(chip, chg_usb_current);
		} else {
			rc = smbchg_masked_write(chip,
						chip->usb_chgpth_base + CMD_IL,
						ICL_OVERRIDE_BIT, 0);
			if (rc < 0)
				pr_err("Couldn't set override rc = %d\n", rc);
			rc = chip->usb_psy->get_property(chip->usb_psy,
						POWER_SUPPLY_PROP_CURRENT_MAX, &prop);
			if (!rc)
				current_limit = prop.intval / 1000;
			smbchg_set_usb_current_max(chip, current_limit);
		}
	}

	return 0;
}

#ifndef CONFIG_HLTHERM_RUNTEST
static int chg_no_timer_set(const char *val, const struct kernel_param *kp)
{
	struct power_supply *batt_psy;
	struct smbchg_chip *chip;
	int rc;

	batt_psy = power_supply_get_by_name("battery");
	if (!batt_psy) {
		pr_err("batt psy not found!\n");
		return 0;
	}
	chip = container_of(batt_psy, struct smbchg_chip, batt_psy);

	rc = param_set_int(val, kp);
	if (rc) {
		pr_err("Unable to set chg_usb_900ma: %d\n", rc);
		return rc;
	}

	return smbchg_safety_timer_enable(chip, !chg_no_timer);
}
#endif

static int correct_charger_type(struct smbchg_chip *chip, int default_type)
{
    union power_supply_propval prop = {0,};
	int type = default_type, rc;

    rc = chip->usb_psy->get_property(chip->usb_psy,
                POWER_SUPPLY_PROP_TYPE, &prop);
	if (rc) {
		pr_smb(PR_STATUS, "%s: Get usb type fail\n", __func__);
		type = default_type;
	} else {
		if (POWER_SUPPLY_TYPE_USB_DCP == prop.intval)
			type = prop.intval;
	}

	return type;
}

static int huawei_smbchg_set_in_thermal(int current_ma)
{
	int rc = 0;
	bool enable = current_ma > 0 ? true : false;
	struct smbchg_chip *chip = hw_data.chip;

	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	pr_smb(PR_STATUS, "User setting FCC to %d\n", current_ma);

	hw_data.hot_current = current_ma;

	rc = vote(chip->usb_icl_votable, HW_IIN_THERMAL_VOTER, enable, current_ma);
	if (rc < 0)
		pr_err("Couldn't vote en rc %d\n", rc);
	return rc;
}

#define BAT_IF_SHIP_MODE	0x40
static int huawei_smbchg_set_ship_mode(int val, bool factory_flag)
{
	struct smbchg_chip *chip = hw_data.chip;
	int rc = 0;
	u8 reg;

	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	if (!factory_flag) {
		return 1;	
	}

	rc = smbchg_sec_masked_write(chip, chip->bat_if_base + BAT_IF_SHIP_MODE,
							BIT(0), val);
	if (rc) {
		dev_err(chip->dev, "spmi write fail: addr=0x%x, rc=%d\n",
				chip->bat_if_base + BAT_IF_SHIP_MODE, rc);
		return rc;
	}

	rc = smbchg_read(chip, &reg, chip->bat_if_base + BAT_IF_SHIP_MODE, 1);
	if (rc) {
		dev_err(chip->dev, "spmi read fail: addr=0x%x, rc=%d\n",
				chip->bat_if_base + BAT_IF_SHIP_MODE, rc);
		return rc;
	}

	return !!(reg & BIT(0));
}
static int huawei_smbchg_get_iin_max(void)
{
	int rc = 0;
	u8 iin_max;
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	rc = smbchg_read(chip, &iin_max, chip->usb_chgpth_base + IL_CFG, 1);
	if (rc) {
		dev_err(chip->dev, "spmi read fail: rc = %d\n", rc);
		return -EINVAL;
	}

	iin_max &= USBIN_INPUT_MASK;
	if (iin_max >= chip->tables.usb_ilim_ma_len) {
		pr_warn("Invalid AICL value: %02x\n", iin_max);
		return -EINVAL;
	}

	return chip->tables.usb_ilim_ma_table[iin_max];
}

/* input : value 
               0   release wake lock 
               1   keep wake lock
*/
#define SMBCHG_RELAX		0
#define SMBCHG_STAY_AWAKE 	1
static void huawei_smbchg_set_chg_lock(unsigned int value)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return;
	}

    if(SMBCHG_RELAX == value)
    {
		device_wakeup_disable(chip->dev);
    }
    else if(SMBCHG_STAY_AWAKE == value)
    {
		device_wakeup_enable(chip->dev);
		smbchg_stay_awake(chip, chip->wake_reasons);
    }
    else
    {
        pr_err(" %s input invalid parameter \n", __func__);
    }
    pr_info("set sleep mode test into sleep flag %d\n", value);
    return;
}

int is_usb_chg_exist(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	return is_usb_present(chip);
}

EXPORT_SYMBOL(is_usb_chg_exist);

static int huawei_smbchg_set_factory_diag(int value)
{
	int rc;
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	rc = vote(chip->battchg_suspend_votable,
				BATTCHG_USER_EN_VOTER, !value, 0);

	return rc;
}

static int huawei_smbchg_set_enable_charger(int value)
{
	int rc;
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	rc = vote(chip->usb_suspend_votable, USER_EN_VOTER,
			!value, 0);
	rc = vote(chip->dc_suspend_votable, USER_EN_VOTER,
			!value, 0);

	chip->chg_enabled = value;
	schedule_work(&chip->usb_set_online_work);

	return rc;
}

static int huawei_smbchg_get_in_thermal(void)
{
	return hw_data.hot_current;
}

static int huawei_smbchg_is_otg_mode(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	return chip->chg_otg_enabled;
}

static int huawei_smbchg_is_usb_ovp(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	return chip->usb_ov_det;
}

static int huawei_smbchg_get_aicl_status(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	return (int)chip->aicl_complete;
}

static int huawei_smbchg_get_aicl_level(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return 0;
	}

	return smbchg_get_aicl_level_ma(chip);
}

static char DUMP_CHG[] = {0x0c, 0x0d, 0x0e, 0x10, 0xf1, 0xf2, 0xf4, 0xf6, 0xf8, 0xfa, 0xfc};
static char DUMP_OTG[] = {0x08};
static char DUMP_BAT_IF[] = {0x08, 0x10, 0x42, 0xf3};
static char DUMP_USB_CHGPATH[] = {0x07, 0x08, 0x09, 0x10, 0x0d, 0x40, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5};
static char DUMP_DC_CHGPATH[] = {0x08};
static char DUMP_CHG_MISC[] = {0x08, 0x10, 0xf5};
#define BUFFER_SIZE          30
static void get_charger_register_info(struct smbchg_chip *chip,
						char * reg_value, u16 base_addr,
						char *offset, int length)
{
    char buff[BUFFER_SIZE] = {0};
    int i = 0;
    int rc = 0;
    char reg = 0;

    if(NULL == reg_value || NULL == offset)
    {
        pr_err("the reg_value or offset is NULL\n");
        return;
    }

    for(i = 0; i < length; i++)
    {
        rc = smbchg_read(chip, &reg, base_addr + offset[i], 1);
        if (rc)
        {
            pr_err("spmi read failed: addr=%03X, rc=%d\n", base_addr + offset[i], rc);
            return;
        }
        snprintf(buff, BUFFER_SIZE, "0x%-8.4x",reg);
        strncat_length_protect(reg_value, buff);
    }
}

static void get_charger_register_head(char *reg_head, u16 base_addr,
						char *offset, int length)
{
    char buff[BUFFER_SIZE] = {0};
    int i = 0;

    if(NULL == reg_head || NULL == offset)
    {
        pr_err("the reg_head or offset is NULL\n");
        return;
    }

    for(i = 0; i < length; i++)
    {
        snprintf(buff, BUFFER_SIZE, "R[0x%4x] ",base_addr + offset[i]);
        strncat_length_protect(reg_head, buff);
    }
}

static void huawei_smbchg_dump_reg(char *reg_value)
{
	int system_level = 0, bch_en = 0, icur_max = 0;
	struct smbchg_chip *chip = hw_data.chip;
    if(NULL == reg_value)
    {
        pr_err("the reg_value is NULL\n");
        return;
    }

	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return;
	}

    memset(reg_value, 0, CHARGELOG_SIZE);

    /*add 14 item info, the head is chars*/
	system_level = chip->therm_lvl_sel;
	bch_en = !get_effective_result(chip->battchg_suspend_votable);
	icur_max = huawei_smbchg_get_aicl_level();
    snprintf(reg_value, MAX_SIZE, "%-16d %-9d %-11d ",
                system_level,bch_en,icur_max);

	/*add the six bases reg item,the headlog is address*/
    get_charger_register_info(chip, reg_value, chip->chgr_base, DUMP_CHG, sizeof(DUMP_CHG));
    get_charger_register_info(chip, reg_value, chip->otg_base, DUMP_OTG, sizeof(DUMP_OTG));
    get_charger_register_info(chip, reg_value, chip->bat_if_base, DUMP_BAT_IF, sizeof(DUMP_BAT_IF));
    get_charger_register_info(chip, reg_value, chip->usb_chgpth_base, DUMP_USB_CHGPATH, sizeof(DUMP_USB_CHGPATH));
    get_charger_register_info(chip, reg_value, chip->dc_chgpth_base, DUMP_DC_CHGPATH, sizeof(DUMP_DC_CHGPATH));
    get_charger_register_info(chip, reg_value, chip->misc_base, DUMP_CHG_MISC, sizeof(DUMP_CHG_MISC));
}

static void huawei_smbchg_get_reg_head(char *reg_head)
{
	struct smbchg_chip *chip = hw_data.chip;
    if(NULL == reg_head)
    {
        pr_err("the reg_head is NULL\n");
        return;
    }

	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return;
	}

    memset(reg_head, 0, CHARGELOG_SIZE);

    /*add the charger headinfo,the head is chars*/
    snprintf(reg_head, MAX_SIZE, "system_level    bch_en    icur_max    ");

	/*add the six bases reg item,the headlog is address*/
    get_charger_register_head(reg_head, chip->chgr_base, DUMP_CHG, sizeof(DUMP_CHG));
    get_charger_register_head(reg_head, chip->otg_base, DUMP_OTG, sizeof(DUMP_OTG));
    get_charger_register_head(reg_head, chip->bat_if_base, DUMP_BAT_IF, sizeof(DUMP_BAT_IF));
    get_charger_register_head(reg_head, chip->usb_chgpth_base, DUMP_USB_CHGPATH, sizeof(DUMP_USB_CHGPATH));
    get_charger_register_head(reg_head, chip->dc_chgpth_base, DUMP_DC_CHGPATH, sizeof(DUMP_DC_CHGPATH));
    get_charger_register_head(reg_head, chip->misc_base, DUMP_CHG_MISC, sizeof(DUMP_CHG_MISC));
}

static struct charge_device_ops smbchg_ops =
{
	.set_factory_diag_charger = huawei_smbchg_set_factory_diag,
	.set_enable_charger = huawei_smbchg_set_enable_charger,
	.set_in_thermal = huawei_smbchg_set_in_thermal,
	.get_in_thermal = huawei_smbchg_get_in_thermal,
	.is_otg_mode = huawei_smbchg_is_otg_mode,
	.is_usb_ovp = huawei_smbchg_is_usb_ovp,
	.set_chg_lock = huawei_smbchg_set_chg_lock,
	.set_ship_mode = huawei_smbchg_set_ship_mode,
	.get_aicl_status = huawei_smbchg_get_aicl_status,
	.get_aicl_level = huawei_smbchg_get_aicl_level,
	.get_iin_max = huawei_smbchg_get_iin_max,
	.dump_register = huawei_smbchg_dump_reg,
	.get_register_head = huawei_smbchg_get_reg_head,
};

/*
 * there are two special cases need to handle:
 * 1. when chg_usb_current is greater than 0, which means it's
 * test mode, and need to draw chg_usb_current mA current from USB port
 * 2. for weak usb case, we need to set 300/400mA.
 *
 * if handled the special cases successful, return zero. otherwise
 * return non-zero value.
 */
static int huawei_correct_usb_current(struct smbchg_chip *chip, int current_ma)
{
	int rc = -1;

	if (POWER_SUPPLY_TYPE_USB ==  chip->usb_supply_type) {
		chip->usb_supply_type = correct_charger_type(chip,
									chip->usb_supply_type);
		if (chg_usb_current > 0) {
			rc = smbchg_masked_write(chip,
						chip->usb_chgpth_base + CMD_IL,
						ICL_OVERRIDE_BIT, ICL_OVERRIDE_BIT);
			if (rc < 0) {
				pr_err("Couldn't set override rc = %d\n", rc);
				goto skip_force_usb;
			}
	
			rc = smbchg_set_high_usb_chg_current(chip, chg_usb_current);
			if (rc < 0) {
				pr_err("Couldn't set %dmA rc = %d\n", chg_usb_current, rc);
				goto skip_force_usb;
			}

			rc = smbchg_sec_masked_write(chip,
				chip->usb_chgpth_base + USB_AICL_CFG,
				AICL_EN_BIT, AICL_EN_BIT);
			if (rc < 0) {
				pr_err("Couldn't enable aicl\n");
				goto skip_force_usb;
			}

			rc = 0;
			goto out;
		}

skip_force_usb:
		switch(chip->usb_supply_type) {
		case POWER_SUPPLY_TYPE_USB:
			if ((current_ma == CURRENT_400_MA) ||
				(current_ma == CURRENT_300_MA)) {
				rc = smbchg_masked_write(chip,
						chip->usb_chgpth_base + CMD_IL,
						ICL_OVERRIDE_BIT, ICL_OVERRIDE_BIT);
				if (rc < 0) {
					pr_err("Couldn't set override rc = %d\n", rc);
				}
				rc = smbchg_set_high_usb_chg_current(chip, current_ma);
				if (rc < 0) {
					pr_err("Couldn't set %dmA rc = %d\n", current_ma, rc);
				}
				rc = smbchg_sec_masked_write(chip,
					chip->usb_chgpth_base + USB_AICL_CFG,
						AICL_EN_BIT, 0);
				if (rc < 0) {
					pr_err("Couldn't disable aicl\n");
				}
				pr_err("%s: usb supply type %d, has corrected\n", __func__,
						chip->usb_supply_type);
				rc = 0;
			}
			break;
		case POWER_SUPPLY_TYPE_USB_DCP:
			rc = smbchg_masked_write(chip,
					chip->usb_chgpth_base + CMD_IL,
					ICL_OVERRIDE_BIT, ICL_OVERRIDE_BIT);
			if (rc < 0)
				pr_err("Couldn't set override rc = %d\n", rc);
			rc = -1;
			break;
		default:
			break;
		}
	}

out:
	return rc;
}

/* load huawei private parameters */
static int huawei_parse_dt(struct smbchg_chip *chip)
{
    int rc = 0;
    struct device_node *node = chip->dev->of_node;

	if (!node) {
		dev_err(chip->dev, "device tree info. missing\n");
		return -EINVAL;
	}

    /* override the origin setting from QCOM */
    chip->parallel.avail = of_property_read_bool(node,
                "qcom,hw-enable-parallel-charger");
    /* remove useless code */

    /* get jeita parameters */
    OF_PROP_READ(chip, hw_data.fastchg_current_batt_warm,
                "fastchg-current-batt-warm", rc, 0);
    OF_PROP_READ(chip, hw_data.fastchg_current_batt_cool,
                "fastchg-current-batt-cool", rc, 0);
    OF_PROP_READ(chip, hw_data.vfloat_batt_warm,
                "vfloat-batt-warm", rc, 0);
    chip->force_aicl_rerun = of_property_read_bool(node,
                "qcom,hw-force-aicl-rerun");
	hw_data.ignore_usb_ibus_setting = of_property_read_bool(node, "qcom,ignore-usb-ibus-setting");

    return rc;
}

/* huawei special init operation */
#define CHGR_TRIM_OPT_15_8		0xF5
#define AICL_INIT				BIT(7)
static int huawei_init(struct smbchg_chip *chip)
{
    int rc = 0;

    /*
     * disable hardware voltage and current compensation
     * when battery is warm/cool, as we use software solution.
     */
    rc = smbchg_sec_masked_write(chip,
                chip->chgr_base + CHGR_CCMP_CFG,
                JEITA_SL_COMP,
                JEITA_SL_COMP_CFG);
    if (rc < 0) {
        dev_err(chip->dev, "Couldn't set jeita temp software"
                " limit comp rc = %d\n", rc);
        return rc;
    }

#ifdef CONFIG_HLTHERM_RUNTEST
    /* if define the macro CONFIG_HLTHERM_RUNTEST, Disenable the temp hard limit */
        rc = smbchg_sec_masked_write(chip, chip->chgr_base + CHGR_CCMP_CFG, JEITA_TEMP_HARD_LIMIT_BIT,JEITA_TEMP_HARD_LIMIT_BIT);
        if (rc < 0) {
            dev_err(chip->dev,"Couldn't set jeita temp hard limit rc = %d\n",rc);
            return rc;
        }
#endif

    hw_data.chip = chip;
    /* jeita related */
    hw_data.jeita_warm_state = false;
    hw_data.jeita_cool_state = false;
    hw_data.jeita_volt_limit = false;
    INIT_DELAYED_WORK(&hw_data.jeita_work, huawei_set_jeita_param);

    INIT_DELAYED_WORK(&usb_switch_work, hw_usb_switch_work);
    usb_switch_register_notifier(&usb_switch_nblk);

	rc = smbchg_sec_masked_write(chip,
				chip->misc_base + CHGR_TRIM_OPT_15_8,
				AICL_INIT, 0);
	if (rc < 0) {
		dev_err(chip->dev, "Setting AICL init value failed\n");
	}

	charge_ops_register(&smbchg_ops);

	INIT_WORK(&hw_data.usbin_valid_count_work, usbin_valid_count_work_func);

    return 0;
}

#define HALF_MINUTE	30
#define MAX_COUNT 20
static void usbin_valid_count_work_func(struct work_struct *work)
{
	static int usbin_irq_invoke_count = 0;
	static int start_flag = 0;
	static unsigned long start_tm_sec = 0;
	unsigned long now_tm_sec = 0;

	/* if usbin irq is invokes more than 20 times in 30 seconds,
	 * dump pmic registers and adc values, and notify to dsm server
	 */
	if (!start_flag) {
		get_current_time(&start_tm_sec);
		start_flag = 1;
	}

	get_current_time(&now_tm_sec);
	if (HALF_MINUTE >= (now_tm_sec - start_tm_sec)) {
		usbin_irq_invoke_count++;
		pr_smb(PR_STATUS, "usbin valid count work func is invoked!"
						"usbin_irq_invoke_count is %d\n", usbin_irq_invoke_count);
		if (MAX_COUNT <= usbin_irq_invoke_count) {
			usbin_irq_invoke_count = 0;
			dsm_post_chg_bms_info(DSM_USBIN_IRQ_INVOKE_TOO_QUICK, "Usbin irq"
							"is called too quick\n");
		}
	} else {
		start_flag = 0;
		start_tm_sec = 0;
		usbin_irq_invoke_count = 0;
	}
}

static void usbin_valid_count_invoke(void)
{
	struct smbchg_chip *chip = hw_data.chip;
	if (!chip) {
		pr_err("%s: Call before init\n", __func__);
		return;
	}

	schedule_work(&hw_data.usbin_valid_count_work);
}

#define CHG_TIMEOUT_BIT	BIT(3)
static irqreturn_t huawei_chg_timeout_handler(int irq, void *_chip)
{
	struct smbchg_chip *chip = _chip;
	int rc = 0;
	u8 reg = 0;
	bool timeout = false;

	pr_smb(PR_INTERRUPT, "chg timeout triggered\n");
	rc = smbchg_read(chip, &reg, chip->chgr_base + RT_STS, 1);
	if (rc) {
		dev_err(chip->dev, "Error reading RT_STS rc = %d\n", rc);
	} else {
		timeout = !!(reg & CHG_TIMEOUT_BIT);
		pr_smb(PR_INTERRUPT, "triggered: 0x%02x\n", reg);
	}

	if (timeout) {
		dsm_post_chg_bms_info(DSM_CHG_TIMEOUT, "Charging timeout\n");
	}

	return IRQ_HANDLED;
}
#define DEFAULT_BATT_CHARGE_FULL_DESIGN	2000000
static int get_prop_batt_charge_full_design(struct smbchg_chip *chip)
{
	int chg_full, rc;

	rc = get_property_from_fg(chip,POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &chg_full);
	if (rc) {
		pr_smb(PR_STATUS, "Couldn't get chg_full rc = %d\n", rc);
		chg_full = DEFAULT_BATT_CHARGE_FULL_DESIGN;
	}
	return chg_full;
}

/* handle weak usb case, if we handled it successful,
 * return non-zero value, otherwise return zero which
 * means we cant handle it in this function.
 */
static int huawei_opt_weak_usb(struct smbchg_chip *chip)
{
	int current_ma = chip->usb_max_current_ma;

	vote(chip->usb_suspend_votable,
				WEAK_CHARGER_EN_VOTER, true, 0);
	pr_err("%s: current_ma %d\n", __func__, current_ma);

	if (!chip->very_weak_charger) {
		return 0;
	}

	if (current_ma <= CURRENT_150_MA) {
		pr_err("%s: too weak, disable it\n", __func__);
		return 0;
	} else if (current_ma <= CURRENT_300_MA) {
		vote(chip->usb_icl_votable, PSY_ICL_VOTER, true, CURRENT_150_MA);
	} else if (current_ma <= CURRENT_400_MA) {
		vote(chip->usb_icl_votable, PSY_ICL_VOTER, true, CURRENT_300_MA);
	} else if (current_ma <= CURRENT_500_MA) {
		vote(chip->usb_icl_votable, PSY_ICL_VOTER, true, CURRENT_400_MA);
	} else if (current_ma <= CURRENT_900_MA) {
		vote(chip->usb_icl_votable, PSY_ICL_VOTER, true, CURRENT_500_MA);
	} else {
		vote(chip->usb_icl_votable, PSY_ICL_VOTER, true, CURRENT_900_MA);
	}

	vote(chip->usb_suspend_votable,
			WEAK_CHARGER_EN_VOTER, false, 0);

	return 1;
}

static int huawei_handle_usb_removal(struct smbchg_chip *chip)
{
	int target_icl_ma = 0;
	int rc;
	vote(chip->usb_icl_votable, HW_AICL_VOTER, false, 0);
	target_icl_ma = get_effective_result_locked(chip->usb_icl_votable);
	smbchg_set_usb_current_max(chip, target_icl_ma);
	rc = smbchg_sec_masked_write(chip, chip->usb_chgpth_base + USB_AICL_CFG,
			AICL_EN_BIT, AICL_EN_BIT);
	if (rc < 0)
		pr_err("Couldn't enable AICL rc=%d\n", rc);

	return 0;
}

#endif
