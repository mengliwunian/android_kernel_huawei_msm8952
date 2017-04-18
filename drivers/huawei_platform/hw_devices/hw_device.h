/*
 * Copyright (C) 2012-2015 HUAWEI, Inc.
 * antenna board match driver
 * Author:l00345350
 *
 * This program check the antenna board match status
 *
 */
#ifndef _HW_DEVICE
#define _HW_DEVICE
#include <linux/device.h>

struct hw_device_info {
    struct device *dev;
    u8 pmic_status;
};
extern int check_power_on_reason(u8* val);
#endif