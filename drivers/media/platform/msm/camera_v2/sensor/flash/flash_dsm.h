#ifdef CONFIG_HUAWEI_DSM
#include "msm_camera_dsm.h"

//these bits should be the same to LM3642's spec exactly 
#define LED_TIMEOUT                  (1<<0)
#define LED_THERMAL_SHUTDOWN         (1<<1)
#define LED_SHORT                    (1<<2)
#define LED_OVP                      (1<<3)
#define LED_UVLO                     (1<<4)
#define LED_IVFM                     (1<<5)

//thermal engine shut down the flash
#define LED_POWER_SAVING             (1<<8)

struct flash_dsm_report
{
    int report_mask;
    const char *report_msg;
    int dsm_error_no;
};

void camera_report_flash_dsm_err(int flash_flag);
#endif
