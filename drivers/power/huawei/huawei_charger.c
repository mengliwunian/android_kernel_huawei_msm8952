/*
 * drivers/power/huawei_charger.c
 *
 *huawei charger driver
 *
 * Copyright (C) 2012-2015 HUAWEI, Inc.
 * Author: HUAWEI, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/wakelock.h>
#include <linux/usb/otg.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/power_supply.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/spmi.h>
#include <linux/power/huawei_charger.h>
#include <linux/string.h>
#ifdef CONFIG_HUAWEI_DSM
#include <dsm/dsm_pub.h>

static struct dsm_client *dsm_chargemonitor_dclient = NULL;
static struct dsm_dev dsm_charge_monitor = 
{
    .name = "dsm_charge_monitor",
    .fops = NULL,
    .buff_size = 4096,
};
#endif
struct class *power_class = NULL;
struct device *charge_dev = NULL;
struct charge_device_ops *g_ops = NULL;
struct charge_device_info *g_charger_device_para = NULL;
extern struct atomic_notifier_head  hw_chg_notifier;

static struct kobject *g_sysfs_poll = NULL;

static ssize_t get_poll_charge_start_event(struct device *dev,
                       struct device_attribute *attr, char *buf)
{
    struct charge_device_info *chip = g_charger_device_para;

    if(chip)
    {
        return snprintf(buf, PAGE_SIZE,"%d\n", chip->input_event);
    }
    else
    {
        return 0;
    }
}

static ssize_t set_poll_charge_event(struct device *dev,
                       struct device_attribute *attr, const char *buf, size_t count)
{
    struct charge_device_info *chip = g_charger_device_para;
    long val = 0;
    if(chip)
    {
        if((strict_strtol(buf, 10, &val) < 0) || (val < 0) || (val > 3000))
        {
            return -EINVAL;
        }
        chip->input_event = val;
        sysfs_notify(g_sysfs_poll, NULL, "poll_charge_start_event");
    }
    return count;
}

static DEVICE_ATTR(poll_charge_start_event, (S_IWUSR | S_IRUGO),
                        get_poll_charge_start_event,
                        set_poll_charge_event);

static int charge_event_poll_register(struct device *dev)
{
    int ret;
    ret = sysfs_create_file(&dev->kobj, &dev_attr_poll_charge_start_event.attr);
    if(ret)
    {
        pr_err("fail to create poll node for %s\n", dev->kobj.name);
        return ret;
    }
    g_sysfs_poll = &dev->kobj;
    return ret;
}

static void charge_event_notify(unsigned int event)
{
    struct charge_device_info *chip = g_charger_device_para;
    if(!chip)
    {
        pr_info("smb device is not init, do nothing!\n");
        return;
    }
    /* avoid notify charge stop event continuously without charger inserted */
    if((chip->input_event != event) || (event == SMB_START_CHARGING))
    {
        chip->input_event = event;
        if(g_sysfs_poll)
        {
            sysfs_notify(g_sysfs_poll, NULL, "poll_charge_start_event");
        }
    }
}

static void smb_update_status(struct charge_device_info *di)
{
    unsigned int events = 0;
    int charging_enabled = 0;
    int battery_present = 0;
    union power_supply_propval val = {0,};
    if(di->batt_psy)
    {
       di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CHARGING_ENABLED,&val);
       charging_enabled = val.intval;
       di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_PRESENT,&val);
       battery_present = val.intval;
    }
    if(!battery_present)
    {
        events = SMB_STOP_CHARGING;
    }
    if(!events)
    {
        if(charging_enabled && battery_present)
        {
            events = SMB_START_CHARGING;
        }
    }
    charge_event_notify(events);
}

static void smb_charger_work(struct work_struct *work)
{
    struct charge_device_info *chip = container_of(work,
                                      struct charge_device_info, smb_charger_work.work);
    smb_update_status(chip);
    schedule_delayed_work(&chip->smb_charger_work,
                          msecs_to_jiffies(QPNP_SMBCHARGER_TIMEOUT));
}

int huawei_handle_charger_event(void)
{
    struct charge_device_info *di = NULL;
    static int smb_charge_work_flag = 1;
    di = g_charger_device_para;
    if(NULL == di)
    {
        pr_err(" %s charge ic  is unregister !\n", __func__);
        return -1 ;
    }
    if(is_usb_chg_exist() && smb_charge_work_flag == 1)
    {
        charge_event_notify(SMB_START_CHARGING);
        schedule_delayed_work(&di->smb_charger_work, msecs_to_jiffies(0));
        smb_charge_work_flag = 0;
    }
    if(!is_usb_chg_exist())
    {
        charge_event_notify(SMB_STOP_CHARGING);
        cancel_delayed_work_sync(&di->smb_charger_work);
        smb_charge_work_flag = 1;
    }
    return 0;
}

/*===========================================
FUNCTION: get_running_test_result
DESCRIPTION: For running test apk to get the running test result and status
IPNUT:	struct smbchg_di *di
RETURN:	a int value, we use bit0 to bit10 to tell running test apk the test
result and status, if bit0 is 0, the result is fail, bit5 to bit10 is the
failed reason
if bit0 is 1, the result is pass.
=============================================*/
static int get_running_test_result(struct charge_device_info *di)
{
    int result = 0;
    int cur_status = 0;
    int is_temp_vol_current_ok = 1;
    int vol = 0, temp = 250, health = 0, current_ma =0, capacity;
    int boost_mode = 0;
    int battery_present = 0;
    union power_supply_propval val = {0,};
    if(di->batt_psy)
    {
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_STATUS,&val);
        cur_status = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CURRENT_NOW,&val);
        current_ma = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_VOLTAGE_NOW,&val);
        vol = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_TEMP,&val);
        temp = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CAPACITY,&val);
        capacity = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_HEALTH,&val);
        health = val.intval;
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_PRESENT,&val);
        battery_present = val.intval;
        if ((di != NULL)&&(di->ops != NULL)) {
             boost_mode = di->ops->is_otg_mode();
        }
    }
    pr_debug("get_running_test_result info: usb=%d batt_pres=%d batt_volt=%d batt_temp=%d"
        " cur_status=%d current_ma=%d setting status=%d mode =%d\n",
        is_usb_chg_exist(),
        battery_present,
        vol,
        temp,
        cur_status,
        current_ma,
        di->running_test_settled_status,
        boost_mode
        );

#ifdef CONFIG_HLTHERM_RUNTEST
    pr_info("Open HL therm test ,runningtest return success.\n");
    return 1;
#endif

    if((CHARGE_OCP_THR > current_ma) || (BATTERY_OCP_THR < current_ma)){
        result |= OCP_ABNORML;
        is_temp_vol_current_ok = 0;
        pr_info("Find OCP! current_ma is %d\n", current_ma);
    }

    if((BATTERY_VOL_THR_HI < vol) || (BATTERY_VOL_THR_LO > vol)){
        result |= BATTERY_VOL_ABNORML;
        is_temp_vol_current_ok = 0;
        pr_info("Battery voltage is abnormal! voltage is %d\n", vol);
    }

    if((BATTERY_TEMP_HI < temp) || (BATTERY_TEMP_LO > temp)){
        result |= BATTERY_TEMP_ABNORML;
        is_temp_vol_current_ok = 0;
        pr_info("Battery temperature is abnormal! temp is %d\n", temp);
    }

    if(!is_temp_vol_current_ok){
        result |= CHARGE_STATUS_FAIL;
        pr_info("running test find abnormal battery status, the result is 0x%x\n", result);
        return result;
    }

    if(cur_status == di->running_test_settled_status){
        result |= CHARGE_STATUS_PASS;
        return result;
    }else if((POWER_SUPPLY_STATUS_CHARGING == cur_status)
        &&(POWER_SUPPLY_STATUS_DISCHARGING == di->running_test_settled_status)){
        result |= CHARGE_STATUS_FAIL;
        pr_info("when set discharging, still charging, test failed! result is 0x%x\n", result);
        return result;
    }else if(POWER_SUPPLY_STATUS_CHARGING == di->running_test_settled_status){
        if((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
            && (BATT_FULL == capacity) && (battery_present)
            && is_usb_chg_exist()){
            cur_status = POWER_SUPPLY_STATUS_FULL;
    }

        if(POWER_SUPPLY_STATUS_FULL == cur_status){
            result |= BATTERY_FULL;
    }

        /* usb/charger is absent*/
        if(!is_usb_chg_exist()){
            result |= USB_NOT_PRESENT;
        }

        /* charge ic is in boost mode*/
        if(1 == boost_mode){
            result |= REGULATOR_BOOST;
        }

        if((vol >= (WARM_VOL_THR -WARM_VOL_BUFFER)*1000)
            && (WARM_TEMP_THR <= temp)){
            result |= CHARGE_LIMIT;
        }

        if((POWER_SUPPLY_STATUS_DISCHARGING == cur_status)
            || (POWER_SUPPLY_STATUS_NOT_CHARGING == cur_status)){
            if(((temp >= (HOT_TEMP - COLD_HOT_TEMP_BUFFER))
                 &&(HOT_TEMP_THR > temp))
                 ||(temp <= (COLD_TEMP + COLD_HOT_TEMP_BUFFER))){
                result |= CHARGE_LIMIT;
                pr_info("settled_status %d cur_status=%d temp=%d\n",
                    di->running_test_settled_status, cur_status, temp);
            }
        }

        if((POWER_SUPPLY_HEALTH_OVERHEAT == health)
            || (POWER_SUPPLY_HEALTH_COLD ==health)){
            result |= BATTERY_HEALTH;
        }

        /* OV condition is detected */
        if ((di != NULL)&&(di->ops != NULL)&&(di->ops->is_usb_ovp())) {
            result |= CHARGER_OVP;
        }

        /* battery is absent */
        if(!battery_present){
            result |= BATTERY_ABSENT;
        }

        /* add FAIL_MASK, if pass and fail reasons are */
        /* meet at the same time, report fail */
        if((result & PASS_MASK) && (!(result & FAIL_MASK))){
            result |= CHARGE_STATUS_PASS;
        }else{
            result |= CHARGE_STATUS_FAIL;
            pr_info("get_running_test_result: usb=%d batt_pres=%d batt_volt=%d batt_temp=%d"
                " capacity=%d cur_status=%d current_ma=%d setting status=%d result=0x%x\n",
                is_usb_chg_exist(),
                battery_present,
                vol,
                temp,
                capacity,
                cur_status,
                current_ma,
                di->running_test_settled_status,
                result
             );
        }

        return result;
    }else{
        /* if the setting status is discharging, meanwhile */
        /* if(cur_status != POWER_SUPPLY_STATUS_CHARGING*/
       /* && cur_status != POWER_SUPPLY_STATUS_DISCHARGING) */
       /* We return 1(PASS) directly, as when set discharging*/
       /* it do not need to care high temperature, battery full or unknow*/
        pr_info("usb=%d batt_pres=%d batt_volt=%d batt_temp=%d"
        " cur_status=%d current_ma=%d setting status=%d\n",
        is_usb_chg_exist(),
        battery_present,
            vol,
            temp,
            cur_status,
            current_ma,
            di->running_test_settled_status
        );
        return 1;
    }
}

static int set_running_test_flag(struct charge_device_info *di,int value)
{

    if(NULL == di)
    {
        pr_err("charge_device is not ready! cannot set runningtest flag\n");
        return -1;
    }
    if(value)
    {
        di->running_test_settled_status = POWER_SUPPLY_STATUS_CHARGING;
    }
    else
    {
        di->running_test_settled_status = POWER_SUPPLY_STATUS_DISCHARGING;
    }
    return 0;
}

static int huawei_charger_set_runningtest(struct charge_device_info *di,int val)
{
    union power_supply_propval ret = {0, };
    if((di == NULL)||(di->ops == NULL))
    {
        pr_err("charge_device is not ready! cannot set runningtest\n");
      return -1;
    }

    ret.intval = val;
    di->ops->set_enable_charger(val);
    set_running_test_flag(di,val);
    di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CHARGING_ENABLED,&ret);
    di->sysfs_data.iin_rt = ret.intval;

    return 0 ;

}
static int huawei_charger_factory_diag_charge(struct charge_device_info *di,int val)
{
        union power_supply_propval ret = {0, };
        if((di == NULL)||(di->ops == NULL))
        {
            pr_err("charge_device is not ready! cannot set factory diag\n");
            return -1;
        }

        ret.intval = val;
        di->ops->set_factory_diag_charger(val);
        di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED,&ret);
        di->factory_diag = ret.intval;
        return 0;
}

static int huawei_charger_enable_charge(struct charge_device_info *di,int val)
{
    union power_supply_propval ret = {0, };
    if((di == NULL)||(di->ops == NULL))
    {
        pr_err("charge_device is not ready! cannot enable charge\n");
        return -1;
    }

    ret.intval = val;
    di->ops->set_enable_charger(val);
    di->batt_psy->get_property(di->batt_psy,POWER_SUPPLY_PROP_CHARGING_ENABLED,&ret);
    di->chrg_config = ret.intval;
    return 0;
}

static int huawei_charger_set_in_thermal(struct charge_device_info *di,int val)
{
    if((di == NULL)||(di->ops == NULL))
    {
        pr_err("charge_device is not ready! cannot get in thermal\n");
        return -1;
    }

    di->ops->set_in_thermal(val);
    di->sysfs_data.iin_thl = di->ops->get_in_thermal();	

    return 0;
}

int get_loginfo_int(struct power_supply *psy,int propery)
{
    int rc =0;
    union power_supply_propval ret = {0,};

    if (!psy)
    {
        pr_err("get input source power supply node failed!\n");
        return -EINVAL;
    }

    rc = psy->get_property(psy,propery, &ret);
    if(rc)
    {
        pr_err("Couldn't get type rc = %d\n", rc);
        ret.intval = -EINVAL;
    }
    return ret.intval;
}
EXPORT_SYMBOL_GPL(get_loginfo_int);

void strncat_length_protect(char * dest, char* src)
{
    int str_length = 0;

    if(NULL == dest || NULL == src)
    {
        pr_err("the dest or src is NULL");
    }
    if(strlen(dest) >= CHARGELOG_SIZE)
    {
        pr_err("strncat dest is full!\n");
        return;
    }

    str_length = min(CHARGELOG_SIZE - strlen(dest), strlen(src));
    if(str_length > 0)
    {
        strncat(dest, src, str_length);
    }

}
EXPORT_SYMBOL_GPL(strncat_length_protect);

static void conver_usbtype(int val, char * p_str)
{
    if(NULL == p_str)
    {
        pr_err("the p_str is NULL\n");
        return;
    }

    switch(val)
    {
        case POWER_SUPPLY_TYPE_UNKNOWN:
            strncpy(p_str, "UNKNOWN", sizeof("UNKNOWN"));
            break;
        case POWER_SUPPLY_TYPE_USB_CDP:
            strncpy(p_str, "CDP", sizeof("CDP"));
            break;
        case POWER_SUPPLY_TYPE_USB:
            strncpy(p_str, "USB", sizeof("USB"));
            break;
        case POWER_SUPPLY_TYPE_USB_DCP:
            strncpy(p_str, "DC", sizeof("DC"));
            break;
        default:
            strncpy(p_str, "UNSTANDARD", sizeof("UNSTANDARD"));
            break;
    }
}

static void conver_charging_status(int val, char * p_str)
{
    if(NULL == p_str)
    {
        pr_err("the p_str is NULL\n");
        return;
    }

    switch(val)
    {
        case POWER_SUPPLY_STATUS_UNKNOWN:
            strncpy(p_str, "UNKNOWN", sizeof("UNKNOWN"));
            break;
        case POWER_SUPPLY_STATUS_CHARGING:
            strncpy(p_str, "CHARGING", sizeof("CHARGING"));
            break;
        case POWER_SUPPLY_STATUS_DISCHARGING:
            strncpy(p_str, "DISCHARGING", sizeof("DISCHARGING"));
            break;
        case POWER_SUPPLY_STATUS_NOT_CHARGING:
            strncpy(p_str, "NOTCHARGING", sizeof("NOTCHARGING"));
            break;
        case POWER_SUPPLY_STATUS_FULL:
            strncpy(p_str, "FULL", sizeof("FULL"));
            break;
        default:
            break;
    }
}

static void conver_charger_health(int val, char * p_str)
{
    if(NULL == p_str)
    {
        pr_err("the p_str is NULL\n");
        return;
    }

    switch(val)
    {
        case POWER_SUPPLY_HEALTH_OVERHEAT:
            strncpy(p_str, "OVERHEAT", sizeof("OVERHEAT"));
            break;
        case POWER_SUPPLY_HEALTH_COLD:
            strncpy(p_str, "COLD", sizeof("COLD"));
            break;
        case POWER_SUPPLY_HEALTH_WARM:
            strncpy(p_str, "WARM", sizeof("WARM"));
            break;
        case POWER_SUPPLY_HEALTH_COOL:
            strncpy(p_str, "COOL", sizeof("COOL"));
            break;
        case POWER_SUPPLY_HEALTH_GOOD:
            strncpy(p_str, "GOOD", sizeof("GOOD"));
            break;
        default:
            break;
    }
}

static bool charger_shutdown_flag = false;
static int __init early_parse_shutdown_flag(char * p)
{
    if(p)
    {
        if(!strcmp(p,"charger"))
        {
            charger_shutdown_flag = true;
        }
    }
    return 0;
}
early_param("androidboot.mode",early_parse_shutdown_flag);

static void get_charger_shutdown_flag(bool flag, char * p_str)
{
    if(NULL == p_str)
    {
        pr_err("the p_str is NULL\n");
        return;
    }
    if(flag)
    {
        strncpy(p_str, "OFF", sizeof("OFF"));
    }
    else
    {
        strncpy(p_str, "ON", sizeof("ON"));
    }
}

static bool charger_factory_flag = false;

static int __init early_parse_factory_flag(char * p)
{
    if(p)
    {
        if(!strcmp(p,"factory"))
        {
            charger_factory_flag = true;
        }
    }
    return 0;
}
early_param("androidboot.huawei_swtype",early_parse_factory_flag);

/*    input : value                               ******
                  0  release lock , 1 get lock ******
       out    : NULL                                ******
*/
void charge_into_sleep_mode(unsigned int value)
{
    struct charge_device_info *di = NULL;

    di = g_charger_device_para;
    if(NULL == di || NULL == di->ops || NULL ==di->ops->set_chg_lock)
    {
        pr_err(" %s charge ic ops is unregister !\n", __func__);
        return ;
    }
    if(true == charger_factory_flag)
    {
        di->ops->set_chg_lock(value);
    }
    else
    {
        pr_err("charger_factory_flag is %d!\n",charger_factory_flag);
    }
    return ;
}

#define CHARGE_SYSFS_FIELD(_name, n, m, store)                \
{                                                   \
    .attr = __ATTR(_name, m, charge_sysfs_show, store),    \
    .name = CHARGE_SYSFS_##n,          \
}

#define CHARGE_SYSFS_FIELD_RW(_name, n)               \
        CHARGE_SYSFS_FIELD(_name, n, S_IWUSR | S_IRUGO,       \
                charge_sysfs_store)

#define CHARGE_SYSFS_FIELD_RO(_name, n)               \
        CHARGE_SYSFS_FIELD(_name, n, S_IRUGO, NULL)

static ssize_t charge_sysfs_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);
static ssize_t charge_sysfs_store(struct device *dev,
                                  struct device_attribute *attr, const char *buf, size_t count);

struct charge_sysfs_field_info
{
    struct device_attribute    attr;
    char name;
};

static struct charge_sysfs_field_info charge_sysfs_field_tbl[] =
{
    CHARGE_SYSFS_FIELD_RW(iin_thermal,       IIN_THERMAL),
    CHARGE_SYSFS_FIELD_RW(iin_runningtest,    IIN_RUNNINGTEST),
    CHARGE_SYSFS_FIELD_RW(enable_charger,    ENABLE_CHARGER),
    CHARGE_SYSFS_FIELD_RW(factory_diag,    FACTORY_DIAG_CHARGER),
    CHARGE_SYSFS_FIELD_RO(running_test_status,    RUNNING_TEST_STATUS),
    CHARGE_SYSFS_FIELD_RO(chargelog_head,    CHARGELOG_HEAD),
    CHARGE_SYSFS_FIELD_RO(chargelog,    CHARGELOG),
    CHARGE_SYSFS_FIELD_RW(update_volt_now,    UPDATE_VOLT_NOW),
    CHARGE_SYSFS_FIELD_RW(ship_mode,    SHIP_MODE),
    CHARGE_SYSFS_FIELD_RO(ibus,    IBUS),
};

static struct attribute *charge_sysfs_attrs[ARRAY_SIZE(charge_sysfs_field_tbl) + 1];

static const struct attribute_group charge_sysfs_attr_group =
{
    .attrs = charge_sysfs_attrs,
};

/**********************************************************
*  Function:       charge_sysfs_init_attrs
*  Discription:    initialize charge_sysfs_attrs[] for charge attribute
*  Parameters:   NULL
*  return value:  NULL
**********************************************************/
static void charge_sysfs_init_attrs(void)
{
    int i, limit = ARRAY_SIZE(charge_sysfs_field_tbl);

    for (i = 0; i < limit; i++)
    {
        charge_sysfs_attrs[i] = &charge_sysfs_field_tbl[i].attr.attr;
    }

    charge_sysfs_attrs[limit] = NULL; /* Has additional entry for this */
}
/**********************************************************
*  Function:       charge_sysfs_field_lookup
*  Discription:    get the current device_attribute from charge_sysfs_field_tbl by attr's name
*  Parameters:   name:device attribute name
*  return value:  charge_sysfs_field_tbl[]
**********************************************************/
static struct charge_sysfs_field_info *charge_sysfs_field_lookup(const char *name)
{
    int i, limit = ARRAY_SIZE(charge_sysfs_field_tbl);

    for (i = 0; i < limit; i++)
    {
        if (!strcmp(name, charge_sysfs_field_tbl[i].attr.attr.name))
            break;
    }

    if (i >= limit)
    {
        return NULL;
    }

    return &charge_sysfs_field_tbl[i];
}

/**********************************************************
*  Function:       charge_sysfs_show
*  Discription:    show the value for all charge device's node
*  Parameters:   dev:device
*                      attr:device_attribute
*                      buf:string of node value
*  return value:  0-sucess or others-fail
**********************************************************/
static ssize_t charge_sysfs_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct charge_sysfs_field_info *info = NULL;
    struct charge_device_info *di = dev_get_drvdata(dev);
    int ret = 0,online = 0,usb_vol = 0,in_type = 0,ch_en = 0,status = 0,health = 0,bat_present = 0,temp = 0,vol = 0,cur = 0,capacity = 0,ibus = 0;
    char cType[30] = {0},cStatus[30] = {0},cHealth[30] = {0},cOn[30] = {0};

    info = charge_sysfs_field_lookup(attr->attr.name);
    if (!info)
    {
        return -EINVAL;
    }

    switch(info->name)
    {
        case CHARGE_SYSFS_IIN_THERMAL:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", di->sysfs_data.iin_thl);
        }
        case CHARGE_SYSFS_IIN_RUNNINGTEST:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", di->sysfs_data.iin_rt);
        }
        case CHARGE_SYSFS_RUNNING_TEST_STATUS:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", get_running_test_result(di));
        }
        case CHARGE_SYSFS_ENABLE_CHARGER:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", di->chrg_config);
        }
        case CHARGE_SYSFS_FACTORY_DIAG_CHARGER:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", di->factory_diag);
        }
        case CHARGE_SYSFS_CHARGELOG_HEAD:
        {
            mutex_lock(&di->sysfs_data.dump_reg_head_lock);
            di->ops->get_register_head(di->sysfs_data.reg_head);
            ret = snprintf(buf, MAX_SIZE, " online   in_type     usb_vol     iin_thl     ch_en   status         health    bat_present   temp    vol       cur       capacity   ibus       %s Mode\n",
                                    di->sysfs_data.reg_head);
            mutex_unlock(&di->sysfs_data.dump_reg_head_lock);
            return ret;
        }
        case CHARGE_SYSFS_CHARGELOG:
        {
            online = get_loginfo_int(di->usb_psy,POWER_SUPPLY_PROP_ONLINE);
            in_type = get_loginfo_int(di->usb_psy,POWER_SUPPLY_PROP_TYPE);
            conver_usbtype(in_type,cType);
            usb_vol = get_loginfo_int(di->usb_psy,POWER_SUPPLY_PROP_VOLTAGE_NOW);
            ch_en = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_CHARGING_ENABLED);
            status = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_STATUS);
            conver_charging_status(status,cStatus);
            health = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_HEALTH);
            conver_charger_health(health,cHealth);
            bat_present = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_PRESENT);
            temp = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_TEMP);
            vol = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_VOLTAGE_NOW);
            cur = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_CURRENT_NOW);
            capacity = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_CAPACITY);
            ibus = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_INPUT_CURRENT_NOW);
            get_charger_shutdown_flag(charger_shutdown_flag, cOn);
            mutex_lock(&di->sysfs_data.dump_reg_lock);
            di->ops->dump_register(di->sysfs_data.reg_value);
            ret = snprintf(buf, MAX_SIZE, " %-8d %-11s %-11d %-11d %-7d %-14s %-9s %-13d %-7d %-9d %-9d %-10d %-10d %-16s %s\n",
                                    online, cType, usb_vol, di->sysfs_data.iin_thl, ch_en, cStatus, cHealth, bat_present, temp, vol, cur, capacity, ibus, di->sysfs_data.reg_value, cOn);
            mutex_unlock(&di->sysfs_data.dump_reg_lock);
            return ret;
            }
        case CHARGE_SYSFS_UPDATE_VOLT_NOW:
        {
            return snprintf(buf, MAX_SIZE, "%u\n", 1);
        }
        case CHARGE_SYSFS_SHIP_MODE:
        {
            return snprintf(buf,MAX_SIZE,"%u\n",di->ship_mode);
        }
        case CHARGE_SYSFS_IBUS:
        {
            ibus = get_loginfo_int(di->batt_psy,POWER_SUPPLY_PROP_INPUT_CURRENT_NOW);
            return snprintf(buf,MAX_SIZE,"%d\n",ibus);
        }
        default:
        {
            pr_err("(%s)NODE ERR!!HAVE NO THIS NODE:(%d)\n",__func__,info->name);
            break;
        }
    }

    return 0;
}
/**********************************************************
*  Function:       charge_sysfs_store
*  Discription:    set the value for charge_data's node which is can be written
*  Parameters:   dev:device
*                      attr:device_attribute
*                      buf:string of node value
*                      count:unused
*  return value:  0-sucess or others-fail
**********************************************************/
static ssize_t charge_sysfs_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
    struct charge_sysfs_field_info *info = NULL;
    struct charge_device_info *di = dev_get_drvdata(dev);
    long val = 0;

    info = charge_sysfs_field_lookup(attr->attr.name);
    if (!info)
    {
        return -EINVAL;
    }

    switch(info->name)
    {
        case CHARGE_SYSFS_IIN_THERMAL: /* hot current limit function*/
        {
            if((strict_strtol(buf, 10, &val) < 0)||(val < 0)||(val > 3000))
            {
                return -EINVAL;
            }
            huawei_charger_set_in_thermal(di,val);
            pr_info("THERMAL set input current = %ld\n", val);
            break;
        }

        case CHARGE_SYSFS_IIN_RUNNINGTEST: /* running test charging/discharge function*/
        {
            if((strict_strtol(buf, 10, &val) < 0)||(val < 0)||(val > 3000))
                return -EINVAL;
            pr_info("THERMAL set running test val = %ld\n", val);
            huawei_charger_set_runningtest(di,val);
            pr_info("THERMAL set running test iin_rt = %d\n", di->sysfs_data.iin_rt);
            break;
        }

        case CHARGE_SYSFS_ENABLE_CHARGER:  /* charging/discharging function*/
        {
            if((strict_strtol(buf, 10, &val) < 0)||(val < 0) || (val > 1))
            {
                return -EINVAL;
            }

            pr_info("ENABLE_CHARGER set enable charger val = %ld\n", val);
            huawei_charger_enable_charge(di,val);
            pr_info("ENABLE_CHARGER set chrg_config = %d\n", di->chrg_config);
            break;
        }
        case CHARGE_SYSFS_FACTORY_DIAG_CHARGER:  /* factory diag function*/
        {
            if((strict_strtol(buf, 10, &val) < 0)||(val < 0) || (val > 1))
            {
                return -EINVAL;
            }
            pr_info("ENABLE_CHARGER set factory diag val = %ld\n", val);
            huawei_charger_factory_diag_charge(di,val);
            pr_info("ENABLE_CHARGER set factory_diag = %d\n", di->factory_diag);
            break;
        }
        case CHARGE_SYSFS_UPDATE_VOLT_NOW:
        {
            union power_supply_propval propval = {1, };
            if((strict_strtol(buf, 10, &val) < 0) || (1 != val))
                return -EINVAL;
            if(NULL == di || NULL == di->bms_psy)
            {
                pr_err("fail: UPDATE_VOLT_NOW, di or di->bms_psy is NULL!\n");
                return -EINVAL;
            }
            di->bms_psy->set_property(di->bms_psy, POWER_SUPPLY_PROP_UPDATE_NOW, &propval);
            break;
        }
        case CHARGE_SYSFS_SHIP_MODE:
        {
            if((strict_strtol(buf, 10, &val) < 0)||(val < 0) || (val > 1))
            {
                return -EINVAL;
            }

            pr_info("ENABLE_CHARGER set ship mode val = %ld\n", val);
            di->ship_mode = di->ops->set_ship_mode(val,charger_factory_flag);
            pr_info("ENABLE_CHARGER set ship mode = %d\n", di->ship_mode);
            break;
        }
        default:
        {
            pr_err("(%s)NODE ERR!!HAVE NO THIS NODE:(%d)\n",__func__,info->name);
            break;
        }
    }
    return count;
}

/**********************************************************
*  Function:       charge_sysfs_create_group
*  Discription:    create the charge device sysfs group
*  Parameters:   di:charge_device_info
*  return value:  0-sucess or others-fail
**********************************************************/
static int charge_sysfs_create_group(struct charge_device_info *di)
{
    charge_sysfs_init_attrs();
    return sysfs_create_group(&di->dev->kobj, &charge_sysfs_attr_group);
}
/**********************************************************
*  Function:       charge_sysfs_remove_group
*  Discription:    remove the charge device sysfs group
*  Parameters:   di:charge_device_info
*  return value:  NULL
**********************************************************/
static inline void charge_sysfs_remove_group(struct charge_device_info *di)
{
    sysfs_remove_group(&di->dev->kobj, &charge_sysfs_attr_group);
}

 /**********************************************************
 *  Function:       charge_ops_register
 *  Discription:    register the handler ops for chargerIC
 *  Parameters:   ops:operations interface of charge device
 *  return value:  0-sucess or others-fail
 **********************************************************/
int charge_ops_register(struct charge_device_ops *charge_ops)
{
    if(NULL == charge_ops)
    {
        pr_err("charge ops is NULL!\n");
    }

    if (g_ops)
    {
        pr_err("charge ops have registered already\n");
        return -EBUSY;
    }
    g_ops = charge_ops;

    return 0;
}

static void free_ops_info(void)
{
    struct charge_device_info *di;
    di = g_charger_device_para;
    if(NULL != di)
    {
        di->ops->get_in_thermal= NULL;
        di->ops->set_in_thermal = NULL;
        di->ops->set_enable_charger = NULL;
        di->ops->set_factory_diag_charger = NULL;
        di->ops->is_otg_mode = NULL;
        di->ops->is_usb_ovp = NULL;
	    di->ops->set_chg_lock = NULL;
        di->ops->set_ship_mode = NULL;
        di->ops->dump_register = NULL;
        di->ops->get_register_head = NULL;
    }

    di->ops = NULL;
    return ;
}

#define DCP_MAX_CURRENT_1A      1000
#define DCP_MAX_CURRENT_1P1A    1100
#define DCP_MAX_CURRENT_1P5A    1500
#define DCP_UP_TH_MAX           200
#define DCP_WEAK_UP_TH			100
static void hw_chg_adaptive_work(struct work_struct *work)
{
    struct charge_device_info *di = g_charger_device_para;
    union power_supply_propval propval = {0, };
    int aicl_level = 0, iin_max = 0;

    if ((NULL == di) || (NULL == di->batt_psy)
                || (NULL == di->ops)
                || (NULL == di->ops->get_aicl_status)
                || (NULL == di->ops->get_aicl_level))
    {
        pr_err("Charge device not init yet!\n");
        return;
    }

    if (di->ops->get_aicl_status()) {
        aicl_level = di->ops->get_aicl_level();
        iin_max = di->ops->get_iin_max();
        if ((aicl_level <= 0) || (iin_max <= 0)) {
            pr_err("Get error aicl/icl level %d\n", aicl_level);
            return;
        }
		pr_info("%s: AICL level %d\n", __func__, aicl_level);
        if (aicl_level == iin_max) {
            return;
        }

		if (aicl_level <= (DCP_MAX_CURRENT_1A + DCP_UP_TH_MAX)) {
            /* if the original setting is less than 1A, then keep it */
            if (iin_max > DCP_MAX_CURRENT_1A) {
                propval.intval = DCP_MAX_CURRENT_1A;
            } else {
                pr_debug("Needn't correct ICL\n");
                return;
            }
        } else if (aicl_level <= DCP_MAX_CURRENT_1P5A + DCP_UP_TH_MAX) {
            propval.intval = DCP_MAX_CURRENT_1P5A;

        } else {
            pr_debug("Keep 2A current!\n");
            return;
        }

        if (iin_max == propval.intval) {
            pr_info("Same ICL, needn't to set again\n");
            return;
        }

        pr_info("%s: correct ICL to %d\n", __func__, propval.intval);
        di->batt_psy->set_property(di->batt_psy,
                    POWER_SUPPLY_PROP_INPUT_CURRENT_MAX, &propval);
	} else {
        pr_debug("aicl still running\n");
    }
}

static int hw_chg_adaptive_callback(struct notifier_block *nb,
    unsigned long action, void *data)
{
    struct charge_device_info *di;
    di = g_charger_device_para;

    if (NULL != di)
        schedule_delayed_work(&di->chg_adaptive_work,
                  msecs_to_jiffies(1000));

    return 0;
}

static struct notifier_block hw_chg_adaptive_nblk = {
    .notifier_call = hw_chg_adaptive_callback,
};

static struct class *hw_power_class = NULL;
struct class *hw_power_get_class(void)
{
    if(NULL == hw_power_class)
    {
        hw_power_class = class_create(THIS_MODULE, "hw_power");
        if (IS_ERR(hw_power_class))
        {
            pr_err("hw_power_class create fail");
            return NULL;
        }
    }
    return hw_power_class;
}
EXPORT_SYMBOL_GPL(hw_power_get_class);

 /**********************************************************
*  Function:       charge_probe
*  Discription:    chargre module probe
*  Parameters:   pdev:platform_device
*  return value:  0-sucess or others-fail
**********************************************************/

static int charge_probe(struct spmi_device *pdev)
{
    struct charge_device_info *di = NULL;
    struct class *power_class = NULL;
    struct power_supply *usb_psy;
    struct power_supply *batt_psy;
    struct power_supply *bms_psy;
    int ret = 0;

    usb_psy = power_supply_get_by_name("usb");
    if (!usb_psy)
    {
        pr_err("usb supply not found deferring probe\n");
        return -EPROBE_DEFER;
    }
    batt_psy = power_supply_get_by_name("battery");
    if (!batt_psy)
    {
        pr_err("batt supply not found deferring probe\n");
        return -EPROBE_DEFER;
    }
    bms_psy = power_supply_get_by_name("bms");
    if (!bms_psy)
    {
        pr_err("bms supply not found deferring probe\n");
        return -EPROBE_DEFER;
    }
    di = devm_kzalloc(&pdev->dev, sizeof(struct charge_device_info),GFP_KERNEL);
    if (!di)
    {
        pr_err("memory allocation failed.\n");
        return -ENOMEM;
    }

    INIT_DELAYED_WORK(&di->smb_charger_work, smb_charger_work);
    di->dev = &(pdev->dev);
    dev_set_drvdata(&(pdev->dev), di);
    di->usb_psy = usb_psy;
    di->batt_psy = batt_psy;
    di->bms_psy = bms_psy;
    di->running_test_settled_status = POWER_SUPPLY_STATUS_CHARGING;


    di->ops = g_ops;
    if((NULL == di->ops->set_in_thermal)||(NULL == di->ops->set_enable_charger)
            ||(NULL == di->ops->set_factory_diag_charger)||(NULL == di->ops->get_in_thermal)
            ||(NULL == di->ops->is_otg_mode)||(NULL == di->ops->is_usb_ovp)
            ||(NULL == di->ops->set_chg_lock) \
            ||(NULL == di->ops->set_ship_mode) \
            || (NULL == di->ops->dump_register) \
            || (NULL == di->ops->get_register_head))
    {
        pr_err("charge ops is NULL!\n");
        goto charge_fail_1;
    }

    di->sysfs_data.iin_thl = 1500;
    di->sysfs_data.iin_rt = 1;
    di->chrg_config = 1;
    di->factory_diag = 1;
    di->ship_mode = 1;
    mutex_init(&di->sysfs_data.dump_reg_lock);
    mutex_init(&di->sysfs_data.dump_reg_head_lock);

    ret = charge_sysfs_create_group(di);
    if (ret)
    {
        pr_err("can't create charge sysfs entries\n");
        free_ops_info();
        goto charge_fail_0;
    }

    power_class = hw_power_get_class();
    if(power_class)
    {
        if(charge_dev == NULL)
        {
            charge_dev = device_create(power_class, NULL, 0, NULL,"charger");
        }

        ret = sysfs_create_link(&charge_dev->kobj, &di->dev->kobj, "charge_data");
        if(ret)
        {
            pr_err("create link to charge_data fail.\n");
        }
        charge_event_poll_register(charge_dev);
            /* register dsm client for chargemonitor */
#ifdef CONFIG_HUAWEI_DSM
        dsm_register_client(&dsm_charge_monitor);
#endif
    }

    g_charger_device_para = di;
    //delete the nouse code
    /* register notifier for charger adaptive */
    INIT_DELAYED_WORK(&di->chg_adaptive_work, hw_chg_adaptive_work);
    atomic_notifier_chain_register(&hw_chg_notifier, &hw_chg_adaptive_nblk);
    /* schedule once to avoid AICL done before this charger probe */
    schedule_delayed_work(&di->chg_adaptive_work, 0);
    pr_err("huawei charger probe ok!\n");
    return 0;
    huawei_handle_charger_event();

charge_fail_1:
    dev_set_drvdata(&pdev->dev, NULL);
charge_fail_0:
    kfree(di);
    di = NULL;

    return 0;
}
static void charge_event_poll_unregister(struct device *dev)
{
    sysfs_remove_file(&dev->kobj, &dev_attr_poll_charge_start_event.attr);
    g_sysfs_poll = NULL;
}
/**********************************************************
*  Function:       charge_remove
*  Discription:    charge module remove
*  Parameters:   pdev:platform_device
*  return value:  0-sucess or others-fail
**********************************************************/
static int charge_remove(struct spmi_device *pdev)
{
    
    struct charge_device_info *di = dev_get_drvdata(&pdev->dev);
    cancel_delayed_work_sync(&di->smb_charger_work);
    charge_event_poll_unregister(charge_dev);
#ifdef CONFIG_HUAWEI_DSM
    dsm_unregister_client(dsm_chargemonitor_dclient,&dsm_charge_monitor);
#endif
    charge_sysfs_remove_group(di);
    if (NULL != di->ops)
    {
        di->ops = NULL;
        g_ops = NULL;
    }
    kfree(di);
    di = NULL;
   

    return 0;
}
/**********************************************************
*  Function:       charge_shutdown
*  Discription:    charge module shutdown
*  Parameters:   pdev:platform_device
*  return value:  NULL
**********************************************************/
static void charge_shutdown(struct spmi_device  *pdev)
{
    return;
}

#ifdef CONFIG_PM
/**********************************************************
*  Function:       charge_suspend
*  Discription:    charge module suspend
*  Parameters:   pdev:platform_device
*                      state:unused
*  return value:  0-sucess or others-fail
**********************************************************/
static int charge_suspend(struct spmi_device *pdev, pm_message_t state)
{
    return 0;
}
/**********************************************************
*  Function:       charge_resume
*  Discription:    charge module resume
*  Parameters:   pdev:platform_device
*  return value:  0-sucess or others-fail
**********************************************************/
static int charge_resume(struct spmi_device *pdev)
{
    return 0;
}
#endif

static struct of_device_id charge_match_table[] =
{
    {
        .compatible = "huawei,charger",
        .data = NULL,
    },
    {
    },
};

static struct spmi_driver charge_driver =
{
    .probe = charge_probe,
    .remove = charge_remove,
    .suspend = charge_suspend,
    .resume = charge_resume,
    .shutdown = charge_shutdown,
    .driver =
    {
        .name = "huawei,charger",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(charge_match_table),
    },
};


/**********************************************************
*  Function:       charge_init
*  Discription:    charge module initialization
*  Parameters:   NULL



*  return value:  0-sucess or others-fail
**********************************************************/
static int __init charge_init(void)
{
    spmi_driver_register(&charge_driver);

    return 0;
}

/**********************************************************
*  Function:       charge_exit
*  Discription:    charge module exit
*  Parameters:   NULL
*  return value:  NULL
**********************************************************/
static void __exit charge_exit(void)
{
    spmi_driver_unregister(&charge_driver);
}

late_initcall(charge_init);
module_exit(charge_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("huawei charger module driver");
MODULE_AUTHOR("HUAWEI Inc");
