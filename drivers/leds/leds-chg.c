#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/spmi.h>
#include <linux/qpnp/pwm.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>


#include "leds.h"
extern int led_debug_mask;

/* Mask/Bit helpers */
#define _SMB_MASK(BITS, POS) \
	((unsigned char)(((1 << (BITS)) - 1) << (POS)))
#define SMB_MASK(LEFT_BIT_POS, RIGHT_BIT_POS) \
		_SMB_MASK((LEFT_BIT_POS) - (RIGHT_BIT_POS) + 1, \
				(RIGHT_BIT_POS))
				
#define CMD_CHG_LED_REG		0x1243
#define CHG_LED_CTRL_BIT		BIT(0)
#define LED_SW_CTRL_BIT		0x1
#define LED_CHG_CTRL_BIT		0x0
#define CHG_LED_ON		0x03
#define CHG_LED_OFF		0x00
#define LED_BLINKING_PATTERN1		0x01
#define LED_BLINKING_PATTERN2		0x02
#define LED_BLINKING_CFG_MASK		SMB_MASK(2, 1)
#define CHG_LED_SHIFT		1

struct chg_led_data {
	struct led_classdev cdev;
	struct spmi_device	*spmi_dev;
	bool 		cfg_chg_led_support;
	bool 		cfg_chg_led_sw_ctrl; 
};


static int
chg_led_write(struct chg_led_data *led, u16 addr, u8 mask, u8 val)
{
	int rc;
	u8 reg;

	rc = spmi_ext_register_readl(led->spmi_dev->ctrl, led->spmi_dev->sid,
		addr, &reg, 1);
	if (rc) 
	{
		led_ERR("[leds]Unable to read from addr=%x, rc(%d)\n", addr, rc);
		return rc;
	}
	
	reg &= ~mask;
	reg |= val;
	rc = spmi_ext_register_writel(led->spmi_dev->ctrl, led->spmi_dev->sid,
		addr, &reg, 1);
	if (rc)
	{
		led_ERR("[leds]Unable to write to addr=%x, rc(%d)\n", addr, rc);
		return rc;
	}
	return 0;
	
}

static int chg_led_read(struct chg_led_data *led, u8 *val,
			u16 addr, int count)
{
	int rc = 0;
	struct spmi_device *spmi = led->spmi_dev;

	if (addr == 0) {
		led_ERR("[leds]addr cannot be zero addr=0x%02x sid=0x%02x rc=%d\n", addr, spmi->sid, rc);
		return -EINVAL;
	}

	rc = spmi_ext_register_readl(spmi->ctrl, spmi->sid, addr, val, count);
	if (rc) {
		led_ERR("[leds]spmi read failed addr=0x%02x sid=0x%02x rc=%d\n", addr, spmi->sid, rc);
		return rc;
	}
	return 0;
}

static int chg_led_config(struct chg_led_data  *led)
{
	u8 reg;
	u8 mask;
	int rc;

	if (led ->cfg_chg_led_sw_ctrl) {
		// turn-off LED by default for software control
		mask = CHG_LED_CTRL_BIT | LED_BLINKING_CFG_MASK;
		reg = LED_SW_CTRL_BIT | (CHG_LED_OFF << CHG_LED_SHIFT);
	} else {
		mask = CHG_LED_CTRL_BIT;
		reg = LED_CHG_CTRL_BIT;
	}
	
	//set the reg to init val
	rc = chg_led_write(led, CMD_CHG_LED_REG, mask, reg);
	if (rc)
	{
		led_ERR("[leds]Couldn't write LED_CTRL_BIT rc=%d\n", rc);
		return rc;
	}
	
	return 0;
}

static void chg_led_brightness_set(struct led_classdev *cdev,
		enum led_brightness value)
{
	struct chg_led_data  *led = container_of(cdev,
			struct chg_led_data, cdev);
	u8 reg;
	int rc;

	reg = (value > LED_OFF) ? CHG_LED_ON << CHG_LED_SHIFT :
		CHG_LED_OFF << CHG_LED_SHIFT;

	led_INFO("[leds]set the charger led brightness to value=%d\n",value);
	
	rc = chg_led_write(led, CMD_CHG_LED_REG, LED_BLINKING_CFG_MASK, reg);
	if (rc) 
		led_ERR("[leds]Couldn't write CHG_LED rc=%d\n", rc);
}

static enum
led_brightness chg_led_brightness_get(struct led_classdev *cdev)
{
	struct chg_led_data  *led = container_of(cdev, struct chg_led_data, cdev);
	u8 reg_val;
	u8 chg_led_sts;
	int rc;
	
	rc = chg_led_read(led, &reg_val, CMD_CHG_LED_REG, 1);

	if (rc < 0) {
		led_ERR("[leds]Couldn't read CHG_LED_REG sts rc=%d\n", rc);
		return rc;
	}

	chg_led_sts = (reg_val & LED_BLINKING_CFG_MASK) >> CHG_LED_SHIFT;
	
	return (chg_led_sts == CHG_LED_OFF) ? LED_OFF : LED_FULL;
}

static void chg_led_blink_set(struct chg_led_data *led,
		unsigned long blinking)
{
	u8 reg;
	int rc;
	
	if (blinking == 0)
		reg = CHG_LED_OFF << CHG_LED_SHIFT;
	else if (blinking == 1)
		reg = LED_BLINKING_PATTERN1 << CHG_LED_SHIFT;
	else if (blinking == 2)
		reg = LED_BLINKING_PATTERN2 << CHG_LED_SHIFT;
	else
		reg = LED_BLINKING_PATTERN1 << CHG_LED_SHIFT;

	rc = chg_led_write(led, CMD_CHG_LED_REG, LED_BLINKING_CFG_MASK, reg);
	if (rc)
		led_ERR("[leds]Couldn't write CHG_LED rc=%d\n", rc);
	
}

static ssize_t chg_led_blink_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct chg_led_data  *chg_led;
	
	unsigned long blinking;
	ssize_t ret = -EINVAL;
	
	ret = kstrtoul(buf, 10, &blinking);
	if (ret) {
		led_ERR("[leds]set blink value is error, setvalue buf=%s", buf);
		return ret;
	}

	chg_led = container_of(led_cdev, struct chg_led_data, cdev);

	chg_led_blink_set(chg_led, blinking);
	led_INFO("[leds]set red led blink mode = %ld.\n", blinking);
	
	return len;
}

static DEVICE_ATTR(blink, 0664, NULL, chg_led_blink_store);

static struct attribute *led_blink_attributes[] = {
	&dev_attr_blink.attr,
	NULL,
};

static struct attribute_group chg_led_attr_group = {
	.attrs = led_blink_attributes,
};

static int chg_led_probe(struct spmi_device *spmi)
{
	struct chg_led_data *led;
	struct device_node *node;
	int rc=0;

	node = spmi->dev.of_node;
	if (node == NULL)
		return -ENODEV;

	led_INFO("[leds]red led enter probe %s.\n",__func__);

	led = devm_kzalloc(&spmi->dev, sizeof(struct chg_led_data), GFP_KERNEL);//create space for led struct
	if (!led) {
		led_INFO("[leds]Unable to allocate memory\n");
		return -ENOMEM;
	}
	led->spmi_dev= spmi;

	//read all dtsi parameters
	rc = of_property_read_string(node, "linux,name", &led->cdev.name);
	if (rc < 0) {
		led_ERR("[leds]Failure reading led name, rc = %d\n", rc);
		goto err;
	}

	led->cfg_chg_led_sw_ctrl = of_property_read_bool(node, "qcom,chg-led-sw-controls");
	led->cfg_chg_led_support = of_property_read_bool(node, "qcom,chg-led-support");

	//define ctrl function
	if (led->cfg_chg_led_support) {
		led->cdev.brightness_set = chg_led_brightness_set;
		led->cdev.brightness_get = chg_led_brightness_get;

		rc = chg_led_config(led);//config led register
		if (rc) {
			led_ERR("[leds]Failed to set charger led controld bit: %d\n",rc);
			goto err;
		}
	}

	//create the class leds device node
	rc = led_classdev_register(&spmi->dev, &led->cdev);
	if (rc) {
		led_ERR("[leds]unable to register charger led, rc=%d\n", rc);
		goto err;
	}

 	rc = sysfs_create_group(&led->cdev.dev->kobj, &chg_led_attr_group);
	if (rc) {
		led_ERR("[leds]unable to register charger led, led sysfs rc: %d\n",  rc);
		goto fail_id_check;
	}
	led_INFO("[leds]red led probe success in %s.\n",__func__);
	
	return 0;

fail_id_check:
	led_classdev_unregister(&led->cdev);
err:
	kfree(led);
	led = NULL;

	return rc;
}

static int chg_led_remove(struct spmi_device *spmi)
{
	struct chg_led_data *led = dev_get_drvdata(&spmi->dev);
	
	sysfs_remove_group(&led->cdev.dev->kobj,  &chg_led_attr_group);
	led_classdev_unregister(&led->cdev);

	kfree(led);
	led = NULL;
	return 0;
}

static const struct of_device_id chg_leds_match[] = {
	{ .compatible = "chg-leds", },
	{},
};

static struct spmi_driver chg_leds_driver = {
	.driver		= {
		.name	= "chg-leds",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(chg_leds_match),
	},
	.probe		= chg_led_probe,
	.remove		= chg_led_remove,
};

static int __init chg_led_init(void)
{
	return spmi_driver_register(&chg_leds_driver);
}
module_init(chg_led_init);

static void __exit chg_led_exit(void)
{
	spmi_driver_unregister(&chg_leds_driver);
}
module_exit(chg_led_exit);

MODULE_DESCRIPTION("QPNP LEDs driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("leds:leds-chg");
