/*
 * Copyright (C) 2012-2015 HUAWEI, Inc.
 * huawei devices detect driver
 * Author:m00264465
 *
 * This program detect the hw devices abnormal
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "hw_device.h"

#define HW_DEVICES_CLASS_NAME "hw_devices"
static struct class *hw_device_class = NULL;
static struct device *hw_device_dev = NULL;
static struct hw_device_info *g_di = NULL;


/*get pmic status*/

static ssize_t get_pmic_status(struct device *dev,
                       struct device_attribute *attr, char *buf)
{
    int ret =0;
    int reason = 1;

    ret = check_power_on_reason(&g_di->pmic_status);
    pr_info("g_di->pmic_status :%d\n",g_di->pmic_status);
    if(g_di->pmic_status == 1)
        reason = 0;
    else
        reason = 1;
    return snprintf(buf, PAGE_SIZE,"%u\n", reason);

}

static DEVICE_ATTR(check_pmic_status, S_IRUGO,
                        get_pmic_status,
                        NULL);
/*create the device attribute table,if want to add the test item,pls fill the table */
static struct device_attribute *hw_dev_attr_tbl[] = 
{
    &dev_attr_check_pmic_status,
};

static int hw_device_probe(struct platform_device *pdev)
{
    int ret = 0;
    struct hw_device_info *di;
    int len =0;
    int i = 0;
    int j = 0;

    di = kzalloc(sizeof(*di), GFP_KERNEL);
    if (!di)
    {
        pr_err("alloc di failed\n");
        return -ENOMEM;
    }
    di->dev = &pdev->dev;
    platform_set_drvdata(pdev, di);
    di->pmic_status = 0;
    len = ARRAY_SIZE(hw_dev_attr_tbl);
    g_di =di;
    /*create the hw device class*/
    if (NULL == hw_device_class)
    {
        hw_device_class = class_create(THIS_MODULE, HW_DEVICES_CLASS_NAME);
        if (NULL == hw_device_class)
        {
            pr_err("hw_device_class create fail");
            goto hw_device_probe_failed;
        }
    }

    /*create the hw device dev*/
    if(hw_device_class)
    {
        if(hw_device_dev == NULL)
        {
            hw_device_dev = device_create(hw_device_class, NULL, 0, NULL,"hw_detect_device");
            if(IS_ERR(hw_device_dev))
            {
                pr_err("create hw_dev failed!\n");
                goto hw_device_probe_failed1;
            }
        }

        /*create the hw dev attr*/
        for(i=0;i<len;i++)
        {
           ret = sysfs_create_file(&hw_device_dev->kobj, &(hw_dev_attr_tbl[i]->attr));
           if(ret<0)
           {
               pr_err("fail to create pmic attr for %s\n", hw_device_dev->kobj.name);
               ++j;
               continue;
           }

        }
        if(len == j)
        {
           pr_err("fail to create all attr  %d\n",j);
           goto hw_device_probe_failed0;
        }
    }
    pr_info("huawei device  probe ok!\n");
    return 0;

hw_device_probe_failed0:
    device_destroy(hw_device_class,0);	
    hw_device_dev = NULL;
hw_device_probe_failed1:
    class_destroy(hw_device_class);
    hw_device_class = NULL;
hw_device_probe_failed:
    kfree(di);
    di = NULL;
    g_di = NULL;
    return -1;
}

/*remove the hw device*/
static int hw_device_remove(struct platform_device *pdev)
{
    struct hw_device_info *di = platform_get_drvdata(pdev);
    if (NULL == di) {
        pr_err("[%s]di is NULL!\n",__func__);
        return -ENODEV;
    }

    kfree(di);	
    di = NULL;
	g_di = NULL;
    return 0;
}

/*
 *probe match table
*/
static struct of_device_id hw_device_table[] = {
    {
        .compatible = "huawei,hw_device",
        .data = NULL,
    },
    {},
};

/*
 *hw device detect driver
 */
static struct platform_driver hw_device_driver = {
    .probe = hw_device_probe,
    .remove = hw_device_remove,
    .driver = {
        .name = "huawei,hw_device",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(hw_device_table),
    },
};
/***************************************************************
 * Function: hw_device_init
 * Description: hw device detect module initialization
 * Parameters:  Null
 * return value: 0-sucess or others-fail
 * **************************************************************/
static int __init hw_device_init(void)
{
    return platform_driver_register(&hw_device_driver);
}
/*******************************************************************
 * Function:       hw_device_exit
 * Description:    hw device module exit
 * Parameters:   NULL
 * return value:  NULL
 * *********************************************************/
static void __exit hw_device_exit(void)
{
    platform_driver_unregister(&hw_device_driver);
}
module_init(hw_device_init);
module_exit(hw_device_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("huawei hw device driver");
MODULE_AUTHOR("HUAWEI Inc");
