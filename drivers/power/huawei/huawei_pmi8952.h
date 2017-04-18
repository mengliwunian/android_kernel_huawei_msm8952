#ifdef CONFIG_HUAWEI_KERNEL

#include <linux/power/huawei_charger.h>
#include <linux/usb/switch_usb.h>
#include <linux/power/huawei_dsm_charger.h>
#ifdef CONFIG_LOG_JANK
#include <linux/log_jank.h>
#endif

#include <linux/power/huawei_charger.h>
#include <linux/usb/switch_usb.h>

#define JEITA_SL_COMP       SMB_MASK(3, 0)
#define JEITA_SL_COMP_CFG   0
#define CURRENT_300_MA		300
#define CURRENT_400_MA		400

struct hw_private_data {
    int                 saved_vfloat_mv;
    int                 vfloat_batt_warm;
    int                 fastchg_current_batt_warm;
    int                 fastchg_current_batt_cool;
    bool                jeita_warm_state;
    bool                jeita_cool_state;
    bool                jeita_volt_limit;
    struct smbchg_chip  *chip;
    struct delayed_work jeita_work;
    int hot_current;
	int chg_timeout_irq;
	struct work_struct usbin_valid_count_work;
	int chg_adaptive_running;
	bool ignore_usb_ibus_setting;
};
static struct hw_private_data hw_data = {
    .chip = NULL,
	.chg_adaptive_running = 0,
};
atomic_t loaded_batterydata = ATOMIC_INIT(0);
static atomic_t usb_switch_done = ATOMIC_INIT(0);
atomic_t usb_switch_ready = ATOMIC_INIT(0);
struct delayed_work      usb_switch_work;
ATOMIC_NOTIFIER_HEAD(hw_chg_notifier);

static int huawei_parse_dt(struct smbchg_chip *);
static int huawei_init(struct smbchg_chip *);
static void huawei_set_jeita_param(struct work_struct *);
static int correct_charger_type(struct smbchg_chip *, int);
static int huawei_correct_usb_current(struct smbchg_chip *, int);
static void usbin_valid_count_work_func(struct work_struct *);
static void usbin_valid_count_invoke(void);
static irqreturn_t huawei_chg_timeout_handler(int, void *);
static int huawei_opt_weak_usb(struct smbchg_chip *);
static int huawei_handle_usb_removal(struct smbchg_chip *);
static int get_prop_batt_charge_full_design(struct smbchg_chip *chip);
#ifndef CONFIG_HLTHERM_RUNTEST
static int chg_no_temp_limit = 0;
static int chg_no_temp_limit_set(const char *val, const struct kernel_param *kp);
static struct kernel_param_ops chg_no_temp_limit_ops = {
    .set = chg_no_temp_limit_set,
    .get = param_get_int,
};
module_param_cb(chg_no_temp_limit, &chg_no_temp_limit_ops, &chg_no_temp_limit, 0644);
#endif

static int chg_usb_current = 0;
static int chg_usb_current_set(const char *val, const struct kernel_param *kp);
static struct kernel_param_ops chg_usb_current_ops = {
	.set = chg_usb_current_set,
	.get = param_get_int,
};
module_param_cb(chg_usb_current, &chg_usb_current_ops, &chg_usb_current, 0644);

#ifndef CONFIG_HLTHERM_RUNTEST
static int chg_no_timer = 0;
static int chg_no_timer_set(const char *val, const struct kernel_param *kp);
static struct kernel_param_ops chg_no_timer_ops = {
	.set = chg_no_timer_set,
	.get = param_get_int,
};
module_param_cb(chg_no_timer, &chg_no_timer_ops, &chg_no_timer, 0644);
#endif

#endif
