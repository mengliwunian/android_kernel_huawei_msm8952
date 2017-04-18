#include <linux/gpio.h>
#include "lm3642_lib.h"

#ifdef CONFIG_HUAWEI_DSM
#include "flash_dsm.h"
#endif

#define CONFIG_MSMB_CAMERA_DEBUG
#ifdef CONFIG_MSMB_CAMERA_DEBUG
#define LM3642_DBG(fmt, args...) pr_info(fmt, ##args)
#else
#define LM3642_DBG(fmt, args...)
#endif

#define FLASH_CHIP_ID 0x0
#define FLASH_CHIP_ID_REG 0x00
#define FLASH_CHIP_ID_MASK 0x07

#define FLASH_FLAG_REGISTER 0x0B

/*Enable IFVM and set it threshold to 3.2v ,also set flash ramp time to 512us */
static struct msm_camera_i2c_reg_array lm3642_init_array[] = {
	{0x0A, 0x00|0x80, 0x00},	//{ENABLE_REGISTER, MODE_BIT_STANDBY | EBABLE_BIT_IVFM},

	/* disable UVLO */
	{0x01, 0x8C,0x00},
};

/*Enable IFVM and set it threshold to 3.2v ,also set flash ramp time to 512us */
//delete "set current action"
static struct msm_camera_i2c_reg_array lm3642_off_array[] = {
	{0x0A, 0x80,0x00},
};

static struct msm_camera_i2c_reg_array lm3642_release_array[] = {
	{0x0A, 0x80,0x00},
};

static struct msm_camera_i2c_reg_array lm3642_low_array[] = {
	{0x09, 0x40, 0x00},//{CURRENT_REGISTER, 0x40},//234mA
	{0x0A, 0x02 | 0x80, 0x00},//{ENABLE_REGISTER, MODE_BIT_TORCH | EBABLE_BIT_IVFM},
};

static struct msm_camera_i2c_reg_array lm3642_high_array[] = {
	{0x09, 0x07, 0x00},//{CURRENT_REGISTER, 0x07},//750mA
	{0x08, 0x0D, 0x00},//{FLASH_FEATURE_REGISTER,0x0D}frash ramp time 512us and flash timeout 600ms
	{0x0A, 0x03 | 0x80, 0x00},//{ENABLE_REGISTER, MODE_BIT_FLASH | EBABLE_BIT_IVFM},
};

static struct msm_camera_i2c_reg_array lm3642_torch_array[] = {
	{0x09, 0x10, 0x00},//{CURRENT_REGISTER, 0x00},//48.4mA
	{0x0A, 0x02 | 0x80, 0x00},//{ENABLE_REGISTER, MODE_BIT_TORCH | EBABLE_BIT_IVFM},
};

static struct msm_camera_i2c_reg_setting lm3642_init_setting = {
	.reg_setting = lm3642_init_array,
	.size = ARRAY_SIZE(lm3642_init_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3642_off_setting = {
	.reg_setting = lm3642_off_array,
	.size = ARRAY_SIZE(lm3642_off_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3642_release_setting = {
	.reg_setting = lm3642_release_array,
	.size = ARRAY_SIZE(lm3642_release_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3642_low_setting = {
	.reg_setting = lm3642_low_array,
	.size = ARRAY_SIZE(lm3642_low_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3642_high_setting = {
	.reg_setting = lm3642_high_array,
	.size = ARRAY_SIZE(lm3642_high_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static struct msm_camera_i2c_reg_setting lm3642_torch_setting = {
	.reg_setting = lm3642_torch_array,
	.size = ARRAY_SIZE(lm3642_torch_array),
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,
	.delay = 0,
};

static int32_t hw_flash_i2c_lm3642_clear_err(struct msm_flash_ctrl_t *flash_ctrl)
{
        int rc = 0;
        uint16_t reg_value=0;

        LM3642_DBG("%s entry\n", __func__);

        if (!flash_ctrl) {
                pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
                return -EINVAL;
        }

        if (flash_ctrl->flash_i2c_client.i2c_func_tbl) {
                rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_read(
                        &flash_ctrl->flash_i2c_client,
                        FLASH_FLAG_REGISTER,&reg_value, MSM_CAMERA_I2C_BYTE_DATA);
                if (rc < 0){
                        pr_err("clear err and unlock %s:%d failed\n", __func__, __LINE__);
                }
#ifdef CONFIG_HUAWEI_DSM
                else
                {
                    camera_report_flash_dsm_err(reg_value);
                }
#endif
                LM3642_DBG("clear err and unlock success:%02x\n",reg_value);
        }else{
                pr_err("%s:%d flash_i2c_client NULL\n", __func__, __LINE__);
                return -EINVAL;
        }

       return 0;

}

int32_t hw_flash_i2c_lm3642_init(struct msm_flash_ctrl_t *flash_ctrl,
		struct msm_flash_cfg_data_t *flash_data)
{
	int rc = 0;
	struct msm_camera_power_ctrl_t *power_info = NULL;

	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}
	power_info = &flash_ctrl->power_info;

	//clear the err and unlock IC, this function must be called before read and write register
	hw_flash_i2c_lm3642_clear_err(flash_ctrl);
	
	gpio_set_value_cansleep(
		power_info->gpio_conf->gpio_num_info->
		gpio_num[SENSOR_GPIO_FL_NOW],
		GPIO_OUT_LOW);

	gpio_set_value_cansleep(
		power_info->gpio_conf->gpio_num_info->
		gpio_num[SENSOR_GPIO_FL_EN],
		GPIO_OUT_LOW);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_init_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}

	return 0;
}

int32_t hw_flash_i2c_lm3642_release(struct msm_flash_ctrl_t *flash_ctrl)
{
	int rc = 0;
	struct msm_camera_power_ctrl_t *power_info = NULL;

	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}
	power_info = &flash_ctrl->power_info;

	gpio_set_value_cansleep(
		power_info->gpio_conf->gpio_num_info->
		gpio_num[SENSOR_GPIO_FL_NOW],
		GPIO_OUT_LOW);

	gpio_set_value_cansleep(
		power_info->gpio_conf->gpio_num_info->
		gpio_num[SENSOR_GPIO_FL_EN],
		GPIO_OUT_LOW);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_release_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}
	return 0;
}
int32_t hw_flash_i2c_lm3642_low(struct msm_flash_ctrl_t *flash_ctrl,
		struct msm_flash_cfg_data_t *flash_data)
{
	int rc = 0;

	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}
	//clear the err and unlock IC, this function must be called before read and write register

	hw_flash_i2c_lm3642_clear_err(flash_ctrl);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_low_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}

	return 0;
}

int32_t hw_flash_i2c_lm3642_high(struct msm_flash_ctrl_t *flash_ctrl,
		struct msm_flash_cfg_data_t *flash_data)
{
	int rc = 0;
	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	//clear the err and unlock IC, this function must be called before read and write register
	hw_flash_i2c_lm3642_clear_err(flash_ctrl);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_high_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}
	return 0;
}

int32_t hw_flash_i2c_lm3642_torch(struct msm_flash_ctrl_t *flash_ctrl,
		struct msm_flash_cfg_data_t *flash_data)
{
	int rc = 0;

	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}
	//clear the err and unlock IC, this function must be called before read and write register

	hw_flash_i2c_lm3642_clear_err(flash_ctrl);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_torch_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}

	return 0;
}

int32_t hw_flash_i2c_lm3642_off(struct msm_flash_ctrl_t *flash_ctrl,
		struct msm_flash_cfg_data_t *flash_data)
{
	int rc = 0;

	LM3642_DBG("%s:%d called\n", __func__, __LINE__);

	if (!flash_ctrl) {
		pr_err("%s:%d fctrl NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	//clear the err and unlock IC, this function must be called before read and write register
	hw_flash_i2c_lm3642_clear_err(flash_ctrl);

	if(flash_ctrl->flash_i2c_client.i2c_func_tbl){		
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_write_table(
			&flash_ctrl->flash_i2c_client,
			&lm3642_off_setting);
		if (rc < 0)
			pr_err("%s:%d failed\n", __func__, __LINE__);
	}

	return rc;
}

/****************************************************************************
* FunctionName: msm_flash_lm3642_match_id;
* Description :read id and compared with FLASH_CHIP_ID;
***************************************************************************/
int32_t hw_flash_i2c_lm3642_match_id(struct msm_flash_ctrl_t *flash_ctrl,struct i2c_flash_info_t*flash_info)
{
	int32_t rc = 0;
	int32_t i = 0;
	uint16_t id_val = 0;

	if(!flash_ctrl || !flash_info){
		pr_err("%s:%d, flash_ctrl:%p, flash_info:%p", __func__, __LINE__, flash_ctrl, flash_info);
		return -EINVAL;
	}

	for(i = 0; i < 3; i++){
		rc = flash_ctrl->flash_i2c_client.i2c_func_tbl->i2c_read(
			&flash_ctrl->flash_i2c_client,FLASH_CHIP_ID_REG,&id_val, MSM_CAMERA_I2C_BYTE_DATA);
		if(rc < 0){
			pr_err("%s: FLASHCHIP READ I2C error!\n", __func__);
			continue;
		}

		pr_info("%s, lm3642 id:0x%x, rc = %d\n", __func__,id_val,rc );

		if ( FLASH_CHIP_ID == (id_val & FLASH_CHIP_ID_MASK) ){
			rc = 0;
			break;
		}
	}

	if( i >= 3 ){
		pr_err("%s failed\n",__func__);
		rc = -ENODEV;
	}

	return rc;
}
