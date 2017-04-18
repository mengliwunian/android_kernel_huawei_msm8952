

//#define HW_CMR_LOGSWC 0   //file log switch set 0 off,default is 1 on
#define HW_CMR_LOG_TAG "sensor_otp_common_if"

#include <linux/hw_camera_common.h>
#include "msm_sensor.h"
#include "sensor_otp_common_if.h"
#define IMX214_FOXCONN_RG_RATIO_TYPICAL 0x1DD
#define IMX214_FOXCONN_BG_RATIO_TYPICAL 0x182
#define IMX214_SUNNY_RG_RATIO_TYPICAL 0x1D6
#define IMX214_SUNNY_BG_RATIO_TYPICAL 0x19A
#define OV8856_SUNNY_RG_RATIO_TYPICAL 0x21B
#define OV8856_SUNNY_BG_RATIO_TYPICAL 0x27D
#define HI843S_OFILM_RG_RATIO_TYPICAL 0x0254
#define HI843S_OFILM_BG_RATIO_TYPICAL 0x0271
#define IMX214_OFILM_RG_RATIO_TYPICAL 0x1D6
#define IMX214_OFILM_BG_RATIO_TYPICAL 0x19A
#define IMX219_LITEON_RG_RATIO_TYPICAL 0x21F
#define IMX219_LITEON_BG_RATIO_TYPICAL 0x26B

#define OV13850_LITEON_167_RG_RATIO_TYPICAL    0x258
#define OV13850_LITEON_167_BG_RATIO_TYPICAL    0x257

#define OV13850_OFILM_167_RG_RATIO_TYPICAL    0x258
#define OV13850_OFILM_167_BG_RATIO_TYPICAL    0x257

#define IMX328_SUNNY_RG_RATIO_TYPICAL    0x01ED
#define IMX328_SUNNY_BG_RATIO_TYPICAL    0x0172

#define IMX219_SUNNY_RG_RATIO_TYPICAL 0x239
#define IMX219_SUNNY_BG_RATIO_TYPICAL 0x248
struct otp_function_t otp_function_lists[]=
{
	#include "sensor_otp_venus.h"
	#include "sensor_otp_carmel.h"
};

/*************************************************
  Function    : is_exist_otp_function
  Description: Detect the otp we support
  Calls:
  Called By  : msm_sensor_config
  Input       : s_ctrl
  Output     : index
  Return      : true describe the otp we support
                false describe the otp we don't support

*************************************************/
bool is_exist_otp_function( struct msm_sensor_ctrl_t *s_ctrl, int32_t *index)
{
	int32_t i = 0;

	for (i=0; i<(sizeof(otp_function_lists)/sizeof(otp_function_lists[0])); ++i)
	{
        if(strlen(s_ctrl->sensordata->sensor_name) != strlen(otp_function_lists[i].sensor_name))
            continue;
		if (0 == strncmp(s_ctrl->sensordata->sensor_name, otp_function_lists[i].sensor_name, strlen(s_ctrl->sensordata->sensor_name)))
		{
			*index = i;
			CMR_LOGI("is_exist_otp_function success i = %d\n", i);
			return true;
		}
	}
	return false;
}
