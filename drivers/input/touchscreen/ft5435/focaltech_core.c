/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2010-2017, FocalTech Systems, Ltd., all rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "focaltech_core.h"

#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#define FTS_SUSPEND_LEVEL 1     /* Early-suspend level */
#endif

#ifdef CONFIG_MACH_XIAOMI
#include <linux/xiaomi_series.h>
extern int xiaomi_series_read(void);
#endif

#define FTS_DRIVER_NAME                     "ft5435_ts"
#define FTS_INPUT_DEV_NAME                  "fts_ts"
#define INTERVAL_READ_REG                   20
#define TIMEOUT_READ_REG                    300
#if FTS_POWER_SOURCE_CUST_EN
#define FTS_VTG_MIN_UV                      2600000
#define FTS_VTG_MAX_UV                      3300000
#define FTS_I2C_VTG_MIN_UV                  1800000
#define FTS_I2C_VTG_MAX_UV                  1800000
#endif
#define FTS_READ_TOUCH_BUFFER_DIVIDED       0

#define WANGHAN_FT5435_PINCTRL				1
#define WANGHAN_FT5435_LDO 					0
#define WANGHAN_FT5435_VCC_I2C				1
#define WANGHAN_FT5435_VDD 					0

struct i2c_client *ft5435_fts_i2c_client;
struct fts_ts_data *ft5435_fts_wq_data;
struct input_dev *ft5435_fts_input_dev;


#if WANGHAN_FT5435_PINCTRL
struct pctrl_data {
	struct pinctrl *tpctrl;
	struct pinctrl_state *pctrl_state_active;
	struct pinctrl_state *pctrl_state_suspend;
};
#endif

#if WANGHAN_FT5435_LDO
int def_power_ldo_gpio;
u32 def_power_ldo_gpio_flags;
#endif
#if WANGHAN_FT5435_PINCTRL
#define PINCTRL_STATE_ACTIVE "pmx_ts_active"
#define PINCTRL_STATE_SUSPEND "pmx_ts_suspend"
struct pctrl_data *pin_data = NULL;
#endif
u8 g_fwver = 255;
char g_fwver_buff[128];
extern char g_lcd_id[128];
static struct work_struct g_resume_work;
struct mutex ft5435_resume_mutex;


#if FTS_DEBUG_EN
int ft5435_g_show_log = 1;
#else
int ft5435_g_show_log = 0;
#endif

#if (FTS_DEBUG_EN && (FTS_DEBUG_LEVEL == 2))
char g_sz_debug[1024] = {0};
#endif

static void fts_release_all_finger(void);
static int fts_ts_suspend(struct device *dev);
static int fts_ts_resume(struct device *dev);

static void do_ts_resume_work(struct work_struct *work)
{
	int ret;
	FTS_DEBUG("do_ts_resume_work start.");
	mutex_lock(&ft5435_resume_mutex);
	ret = fts_ts_resume(&ft5435_fts_wq_data->client->dev);
	if(ret)
		FTS_DEBUG("fts_ts_resume fail.");
	mutex_unlock(&ft5435_resume_mutex);
	FTS_DEBUG("do_ts_resume_work end.");
}

int ft5435_fts_wait_tp_to_valid(struct i2c_client *client)
{
	int ret = 0;
	int cnt = 0;
	u8  reg_value = 0;

	do {
		ret = ft5435_ft5435_fts_i2c_read_reg(client, FTS_REG_CHIP_ID, &reg_value);
		if ((ret < 0) || (reg_value != ft5435_chip_types.chip_idh)) {
			FTS_INFO("TP Not Ready, ReadData = 0x%x", reg_value);
		} else if (reg_value == ft5435_chip_types.chip_idh) {
			FTS_INFO("TP Ready, Device ID = 0x%x", reg_value);
			return 0;
		}
		cnt++;
		msleep(INTERVAL_READ_REG);
	}
	while ((cnt * INTERVAL_READ_REG) < TIMEOUT_READ_REG);


	ret = ft5435_fts_ctpm_fw_upgrade_ReadBootloadorID(client);
	if(!ret) {
		FTS_INFO("TP Need Upgrade, ret = 0x%x", ret);
		return 0;
	}

	return -1;
}

void ft5435_fts_tp_state_recovery(struct i2c_client *client)
{
	ft5435_fts_wait_tp_to_valid(client);
	ft5435_fts_ex_mode_recovery(client);
#if FTS_GESTURE_EN
	ft5435_fts_gesture_recovery(client);
#endif
}

int ft5435_fts_reset_proc(int hdelayms)
{
	gpio_direction_output(ft5435_fts_wq_data->pdata->reset_gpio, 0);
	msleep(20);
	gpio_direction_output(ft5435_fts_wq_data->pdata->reset_gpio, 1);
	msleep(hdelayms);

	return 0;
}
void ft5435_fts_irq_disable(void)
{
	unsigned long irqflags;
	spin_lock_irqsave(&ft5435_fts_wq_data->irq_lock, irqflags);

	if (!ft5435_fts_wq_data->irq_disable) {
		disable_irq_nosync(ft5435_fts_wq_data->client->irq);
		ft5435_fts_wq_data->irq_disable = 1;
	}

	spin_unlock_irqrestore(&ft5435_fts_wq_data->irq_lock, irqflags);
}

void ft5435_fts_irq_enable(void)
{
	unsigned long irqflags = 0;
	spin_lock_irqsave(&ft5435_fts_wq_data->irq_lock, irqflags);

	if (ft5435_fts_wq_data->irq_disable) {
		enable_irq(ft5435_fts_wq_data->client->irq);
		ft5435_fts_wq_data->irq_disable = 0;
	}

	spin_unlock_irqrestore(&ft5435_fts_wq_data->irq_lock, irqflags);
}

static int ft5435_fts_input_dev_init( struct i2c_client *client, struct fts_ts_data *data,  struct input_dev *input_dev, struct fts_ts_platform_data *pdata)
{
	int  err, len;

	FTS_FUNC_ENTER();

	input_dev->name = FTS_INPUT_DEV_NAME;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	input_set_drvdata(input_dev, data);
	i2c_set_clientdata(client, data);

	__set_bit(EV_KEY, input_dev->evbit);
	if (data->pdata->have_key) {
		FTS_DEBUG("set key capabilities");
		for (len = 0; len < data->pdata->key_number; len++) {
			input_set_capability(input_dev, EV_KEY, data->pdata->keys[len]);
		}
	}
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

#if FTS_MT_PROTOCOL_B_EN
	input_mt_init_slots(input_dev, pdata->max_touch_number, INPUT_MT_DIRECT);
#else
	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID, 0, 0x0f, 0, 0);
#endif
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, pdata->x_min, pdata->x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, pdata->y_min, pdata->y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xFF, 0, 0);
#if FTS_REPORT_PRESSURE_EN
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 0xFF, 0, 0);
#endif


#if FTS_GESTURE_EN
	input_dev->event = fts_select_gesture_mode;
#endif


	err = input_register_device(input_dev);
	if (err) {
		FTS_ERROR("Input device registration failed");
		goto free_inputdev;
	}

	FTS_FUNC_EXIT();

	return 0;

free_inputdev:
	input_free_device(input_dev);
	FTS_FUNC_EXIT();
	return err;

}

#if FTS_POWER_SOURCE_CUST_EN
static int fts_power_source_init(struct fts_ts_data *data)
{
	int rc;

	FTS_FUNC_ENTER();

	data->vdd = regulator_get(&data->client->dev, "vdd");
	if (IS_ERR(data->vdd)) {
		rc = PTR_ERR(data->vdd);
		FTS_ERROR("Regulator get failed vdd rc=%d", rc);
	}

	if (regulator_count_voltages(data->vdd) > 0) {
		rc = regulator_set_voltage(data->vdd, FTS_VTG_MIN_UV, FTS_VTG_MAX_UV);
		if (rc) {
			FTS_ERROR("Regulator set_vtg failed vdd rc=%d", rc);
			goto reg_vdd_put;
		}
	}


#if WANGHAN_FT5435_VCC_I2C
	data->vcc_i2c = regulator_get(&data->client->dev, "vcc_i2c");
	if (IS_ERR(data->vcc_i2c)) {
		rc = PTR_ERR(data->vcc_i2c);
		FTS_ERROR("Regulator get failed vcc_i2c rc=%d", rc);
		goto reg_vdd_set_vtg;
	}

	if (regulator_count_voltages(data->vcc_i2c) > 0) {
		rc = regulator_set_voltage(data->vcc_i2c, FTS_I2C_VTG_MIN_UV, FTS_I2C_VTG_MAX_UV);
		if (rc) {
			FTS_ERROR("Regulator set_vtg failed vcc_i2c rc=%d", rc);
			goto reg_vcc_i2c_put;
		}
	}
#endif


	FTS_FUNC_EXIT();
	return 0;


#if WANGHAN_FT5435_VCC_I2C
reg_vcc_i2c_put:
	regulator_put(data->vcc_i2c);
reg_vdd_set_vtg:
	if (regulator_count_voltages(data->vdd) > 0)
		regulator_set_voltage(data->vdd, 0, FTS_VTG_MAX_UV);
#endif

reg_vdd_put:
	regulator_put(data->vdd);
	FTS_FUNC_EXIT();
	return rc;
}

static int fts_power_source_ctrl(struct fts_ts_data *data, int enable)
{
	int rc;

	FTS_FUNC_ENTER();
	if (enable) {
		rc = regulator_enable(data->vdd);
		if (rc) {
			FTS_ERROR("Regulator vdd enable failed rc=%d", rc);
		}


#if WANGHAN_FT5435_LDO
		if (gpio_is_valid(def_power_ldo_gpio)) {
			printk("%s, def_power_ldo_gpio\n", __func__);
			rc = gpio_direction_output(def_power_ldo_gpio, 1);
			if (rc) {
				printk("[FTS] set_direction for power ldo gpio failed\n");
				goto free_ldo_gpio;
			}
		}
#endif


#if WANGHAN_FT5435_VCC_I2C

		msleep(2);
		rc = regulator_enable(data->vcc_i2c);
		if (rc) {
			FTS_ERROR("Regulator vcc_i2c enable failed rc=%d", rc);
		}
#endif

	} else {
		rc = regulator_disable(data->vdd);
		if (rc) {
			FTS_ERROR("Regulator vdd disable failed rc=%d", rc);
		}

#if WANGHAN_FT5435_VCC_I2C
		rc = regulator_disable(data->vcc_i2c);
		if (rc) {
			FTS_ERROR("Regulator vcc_i2c disable failed rc=%d", rc);
		}
#endif


#if WANGHAN_FT5435_LDO
		if (gpio_is_valid(def_power_ldo_gpio)) {
			printk("%s,def_power_ldo_gpio\n", __func__);
			rc = gpio_direction_output(def_power_ldo_gpio, 0);
			if (rc) {
				printk("[FTS] set_direction for power ldo gpio failed\n");
				goto free_ldo_gpio;
			}
		}
#endif

	}
	FTS_FUNC_EXIT();
	return 0;

#if WANGHAN_FT5435_LDO
free_ldo_gpio:
	if (gpio_is_valid(def_power_ldo_gpio))
		gpio_free(def_power_ldo_gpio);
	return rc;
#endif

}

#endif

static void fts_release_all_finger(void)
{
#if FTS_MT_PROTOCOL_B_EN
	unsigned int finger_count=0;
#endif

	mutex_lock(&ft5435_fts_wq_data->report_mutex);
#if FTS_MT_PROTOCOL_B_EN
	for (finger_count = 0; finger_count < ft5435_fts_wq_data->pdata->max_touch_number; finger_count++) {
		input_mt_slot(ft5435_fts_input_dev, finger_count);
		input_mt_report_slot_state(ft5435_fts_input_dev, MT_TOOL_FINGER, false);
	}
#else
	input_mt_sync(ft5435_fts_input_dev);
#endif
	input_report_key(ft5435_fts_input_dev, BTN_TOUCH, 0);
	input_sync(ft5435_fts_input_dev);
	mutex_unlock(&ft5435_fts_wq_data->report_mutex);
}


#if (FTS_DEBUG_EN && (FTS_DEBUG_LEVEL == 2))
static void fts_show_touch_buffer(u8 *buf, int point_num)
{
	int len = point_num * FTS_ONE_TCH_LEN;
	int count = 0;
	int i;

	memset(g_sz_debug, 0, 1024);
	if (len > (POINT_READ_BUF-3)) {
		len = POINT_READ_BUF-3;
	} else if (len == 0) {
		len += FTS_ONE_TCH_LEN;
	}
	count += sprintf(g_sz_debug, "%02X,%02X,%02X", buf[0], buf[1], buf[2]);
	for (i = 0; i < len; i++) {
		count += sprintf(g_sz_debug+count, ",%02X", buf[i+3]);
	}
	FTS_DEBUG("buffer: %s", g_sz_debug);
}
#endif

static int get_key_value(struct ts_event *event, struct fts_ts_data *data, int i)
{
	if (event->au16_x[i] > (data->pdata->key_x_coords[0] - FTS_KEY_WIDTH) &&
		event->au16_x[i] < (data->pdata->key_x_coords[0] + FTS_KEY_WIDTH)) {
		return 1;
	} else if(event->au16_x[i] > (data->pdata->key_x_coords[1] - FTS_KEY_WIDTH) &&
			event->au16_x[i] < (data->pdata->key_x_coords[1] + FTS_KEY_WIDTH)) {
		return 2;
	} else if(event->au16_x[i] > (data->pdata->key_x_coords[2] - FTS_KEY_WIDTH) &&
			event->au16_x[i] < (data->pdata->key_x_coords[2] + FTS_KEY_WIDTH)) {
		return 3;
	}
	return 0;
}

static int ft5435_fts_input_dev_report_key_event(struct ts_event *event, struct fts_ts_data *data)
{
	int i;
	unsigned char key_value;
	unsigned char key_flag = 0;

	if (data->pdata->have_key) {


		if (event->au16_y[0] == data->pdata->key_y_coord) {

			if (event->point_num == 0) {

				for (i = 0; i < data->pdata->key_number; i++) {
					input_report_key(data->input_dev, data->pdata->keys[i], 0);
				}
			} else {
				for (i = 0; i < data->pdata->key_number; i++) {


					key_value = get_key_value(event, data, i);
					if(key_value)
					{
						if (event->au8_touch_event[i]== 0 ||
							event->au8_touch_event[i] == 2) {
							key_flag = 1;
							input_report_key(data->input_dev, data->pdata->keys[key_value - 1], 1);
							data->key_state |= (1 << (key_value - 1));

						} else
						{
							key_flag = 1;
							input_report_key(data->input_dev, data->pdata->keys[key_value - 1], 0);
							data->key_state &= ~(1 << (key_value - 1));

						}

					}
				}
			}
			if(key_flag) {
				input_sync(data->input_dev);
				return 0;
			}
		}
	}

	return -1;
}

#if FTS_MT_PROTOCOL_B_EN
static int ft5435_fts_input_dev_report_b(struct ts_event *event, struct fts_ts_data *data)
{
	int i = 0;
	int uppoint = 0;
	int touchs = 0;
	for (i = 0; i < event->touch_point; i++) {
		if (event->au8_finger_id[i] >= data->pdata->max_touch_number) {
			break;
		}
		input_mt_slot(data->input_dev, event->au8_finger_id[i]);

		if (event->au8_touch_event[i] == FTS_TOUCH_DOWN || event->au8_touch_event[i] == FTS_TOUCH_CONTACT) {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, true);

#if FTS_REPORT_PRESSURE_EN
#if FTS_FORCE_TOUCH_EN
			if (event->pressure[i] <= 0) {

				event->pressure[i] = 1;
			}
#else
			event->pressure[i] = 0x3f;
#endif
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, event->pressure[i]);
#endif

			if (event->area[i] <= 0) {

				event->area[i] = 1;
			}
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, event->area[i]);

			input_report_abs(data->input_dev, ABS_MT_POSITION_X, event->au16_x[i]);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, event->au16_y[i]);
			touchs |= BIT(event->au8_finger_id[i]);
			data->touchs |= BIT(event->au8_finger_id[i]);

#if FTS_REPORT_PRESSURE_EN

					 event->au16_y[i], event->pressure[i], event->area[i]);
#else

#endif
		} else {
			uppoint++;
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
#if FTS_REPORT_PRESSURE_EN
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, 0);
#endif
			data->touchs &= ~BIT(event->au8_finger_id[i]);

		}
	}

	if (unlikely(data->touchs ^ touchs)) {
		for (i = 0; i < data->pdata->max_touch_number; i++) {
			if (BIT(i) & (data->touchs ^ touchs)) {

				input_mt_slot(data->input_dev, i);
				input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
#if FTS_REPORT_PRESSURE_EN
				input_report_abs(data->input_dev, ABS_MT_PRESSURE, 0);
#endif
			}
		}
	}

	data->touchs = touchs;
	if (event->touch_point == uppoint) {

		input_report_key(data->input_dev, BTN_TOUCH, 0);
	} else {
		input_report_key(data->input_dev, BTN_TOUCH, event->touch_point > 0);
	}

	input_sync(data->input_dev);

	return 0;

}

#else
static int ft5435_fts_input_dev_report_a(struct ts_event *event, struct fts_ts_data *data)
{
	int i =0;
	int uppoint = 0;
	int touchs = 0;

	for (i = 0; i < event->touch_point; i++) {

		if (event->au8_touch_event[i] == FTS_TOUCH_DOWN || event->au8_touch_event[i] == FTS_TOUCH_CONTACT) {
			input_report_abs(data->input_dev, ABS_MT_TRACKING_ID, event->au8_finger_id[i]);
#if FTS_REPORT_PRESSURE_EN
#if FTS_FORCE_TOUCH_EN
			if (event->pressure[i] <= 0) {

				event->pressure[i] = 1;
			}
#else
			event->pressure[i] = 0x3f;
#endif
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, event->pressure[i]);
#endif

			if (event->area[i] <= 0) {

				event->area[i] = 1;
			}
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, event->area[i]);

			input_report_abs(data->input_dev, ABS_MT_POSITION_X, event->au16_x[i]);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, event->au16_y[i]);

			input_mt_sync(data->input_dev);

#if FTS_REPORT_PRESSURE_EN

					 event->au16_y[i], event->pressure[i], event->area[i]);
#else

#endif
		} else {
			uppoint++;
		}
	}

	data->touchs = touchs;
	if (event->touch_point == uppoint) {

		input_report_key(data->input_dev, BTN_TOUCH, 0);
		input_mt_sync(data->input_dev);
	} else {
		input_report_key(data->input_dev, BTN_TOUCH, event->touch_point > 0);
	}

	input_sync(data->input_dev);

	return 0;
}
#endif

static int fts_read_touchdata(struct fts_ts_data *data)
{
	u8 buf[POINT_READ_BUF] = { 0 };
	u8 pointid = FTS_MAX_ID;
	int ret = -1;
	int i;
	struct ts_event * event = &(data->event);

#if FTS_GESTURE_EN
	{
		u8 state;
		if (data->suspended) {
			ft5435_ft5435_fts_i2c_read_reg(data->client, FTS_REG_GESTURE_EN, &state);
			if (state ==1) {
				ft5435_fts_gesture_readdata(data->client);
				return 1;
			}
		}
	}
#endif

#if FTS_PSENSOR_EN
	if ( (fts_sensor_read_data(data) != 0) && (data->suspended == 1) ) {
		return 1;
	}
#endif


#if FTS_READ_TOUCH_BUFFER_DIVIDED
	memset(buf, 0xFF, POINT_READ_BUF);
	memset(event, 0, sizeof(struct ts_event));

	buf[0] = 0x00;
	ret = ft5435_fts_i2c_read(data->client, buf, 1, buf, (3 + FTS_ONE_TCH_LEN));
	if (ret < 0) {

		return ret;
	}
	event->touch_point = 0;
	event->point_num=buf[FTS_TOUCH_POINT_NUM] & 0x0F;
	if (event->point_num > data->pdata->max_touch_number)
		event->point_num = data->pdata->max_touch_number;

	if (event->point_num > 1) {
		buf[9] = 0x09;
		ft5435_fts_i2c_read(data->client, buf+9, 1, buf+9, (event->point_num - 1) * FTS_ONE_TCH_LEN);
	}
#else
	ret = ft5435_fts_i2c_read(data->client, buf, 1, buf, POINT_READ_BUF);
	if (ret < 0) {
		FTS_ERROR("[B]Read touchdata failed, ret: %d", ret);
		return ret;
	}

#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_queue_work();
#endif

	memset(event, 0, sizeof(struct ts_event));
	event->point_num = buf[FTS_TOUCH_POINT_NUM] & 0x0F;
	if (event->point_num > data->pdata->max_touch_number)
		event->point_num = data->pdata->max_touch_number;
	event->touch_point = 0;
#endif

#if (FTS_DEBUG_EN && (FTS_DEBUG_LEVEL == 2))
	fts_show_touch_buffer(buf, event->point_num);
#endif

	for (i = 0; i < data->pdata->max_touch_number; i++) {
		pointid = (buf[FTS_TOUCH_ID_POS + FTS_ONE_TCH_LEN * i]) >> 4;
		if (pointid >= FTS_MAX_ID)
			break;
		else
			event->touch_point++;

		event->au16_x[i] =
			(s16) (buf[FTS_TOUCH_X_H_POS + FTS_ONE_TCH_LEN * i] & 0x0F) <<
			8 | (s16) buf[FTS_TOUCH_X_L_POS + FTS_ONE_TCH_LEN * i];
		event->au16_y[i] =
			(s16) (buf[FTS_TOUCH_Y_H_POS + FTS_ONE_TCH_LEN * i] & 0x0F) <<
			8 | (s16) buf[FTS_TOUCH_Y_L_POS + FTS_ONE_TCH_LEN * i];
		event->au8_touch_event[i] =
			buf[FTS_TOUCH_EVENT_POS + FTS_ONE_TCH_LEN * i] >> 6;
		event->au8_finger_id[i] =
			(buf[FTS_TOUCH_ID_POS + FTS_ONE_TCH_LEN * i]) >> 4;
		event->area[i] =
			(buf[FTS_TOUCH_AREA_POS + FTS_ONE_TCH_LEN * i]) >> 4;
		event->pressure[i] =
			(s16) buf[FTS_TOUCH_PRE_POS + FTS_ONE_TCH_LEN * i];

		if (0 == event->area[i])
			event->area[i] = 0x09;

		if (0 == event->pressure[i])
			event->pressure[i] = 0x3f;

		if ((event->au8_touch_event[i]==0 || event->au8_touch_event[i]==2)&&(event->point_num==0)) {

			return -1;
		}
	}
	if(event->touch_point == 0) {
		return -1;
	}
	return 0;
}

static void fts_report_value(struct fts_ts_data *data)
{
	struct ts_event *event = &data->event;



	if (0 == ft5435_fts_input_dev_report_key_event(event, data)) {
		return;
	}

	if(data->key_state) {
		if(data->key_state && (~0x01)) input_report_key(data->input_dev, data->pdata->keys[0], 0);
		if(data->key_state && (~0x02)) input_report_key(data->input_dev, data->pdata->keys[1], 0);
		if(data->key_state && (~0x04)) input_report_key(data->input_dev, data->pdata->keys[2], 0);
		input_sync(data->input_dev);
		data->key_state = 0;
	}

#if FTS_MT_PROTOCOL_B_EN
	ft5435_fts_input_dev_report_b(event, data);
#else
	ft5435_fts_input_dev_report_a(event, data);
#endif


	return;

}

static irqreturn_t fts_ts_interrupt(int irq, void *dev_id)
{
	struct fts_ts_data *fts_ts = dev_id;
	int ret = -1;

	if (!fts_ts) {
		FTS_ERROR("[INTR]: Invalid fts_ts");
		return IRQ_HANDLED;
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(1);
#endif

	ret = fts_read_touchdata(ft5435_fts_wq_data);

	if (ret == 0) {
		mutex_lock(&ft5435_fts_wq_data->report_mutex);
		fts_report_value(ft5435_fts_wq_data);
		mutex_unlock(&ft5435_fts_wq_data->report_mutex);
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(0);
#endif

	return IRQ_HANDLED;
}

static int fts_gpio_configure(struct fts_ts_data *data)
{
	int err = 0;

	FTS_FUNC_ENTER();
	if (gpio_is_valid(data->pdata->irq_gpio)) {
		err = gpio_request(data->pdata->irq_gpio, "fts_irq_gpio");
		if (err) {
			FTS_ERROR("[GPIO]irq gpio request failed");
			goto err_irq_gpio_req;
		}

		err = gpio_direction_input(data->pdata->irq_gpio);
		if (err) {
			FTS_ERROR("[GPIO]set_direction for irq gpio failed");
			goto err_irq_gpio_dir;
		}
	}
	if (gpio_is_valid(data->pdata->reset_gpio)) {
		err = gpio_request(data->pdata->reset_gpio, "fts_reset_gpio");
		if (err) {
			FTS_ERROR("[GPIO]reset gpio request failed");
			goto err_irq_gpio_dir;
		}

		err = gpio_direction_output(data->pdata->reset_gpio, 1);
		if (err) {
			FTS_ERROR("[GPIO]set_direction for reset gpio failed");
			goto err_reset_gpio_dir;
		}
	}

	FTS_FUNC_EXIT();
	return 0;

err_reset_gpio_dir:
	if (gpio_is_valid(data->pdata->reset_gpio))
		gpio_free(data->pdata->reset_gpio);
err_irq_gpio_dir:
	if (gpio_is_valid(data->pdata->irq_gpio))
		gpio_free(data->pdata->irq_gpio);
err_irq_gpio_req:
	FTS_FUNC_EXIT();
	return err;
}

static int fts_get_dt_coords(struct device *dev, char *name,
							struct fts_ts_platform_data *pdata)
{
	u32 coords[FTS_COORDS_ARR_SIZE];
	struct property *prop;
	struct device_node *np = dev->of_node;
	int coords_size, rc;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;


	coords_size = prop->length / sizeof(u32);
	if (coords_size != FTS_COORDS_ARR_SIZE) {
		FTS_ERROR("invalid %s", name);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(np, name, coords, coords_size);
	if (rc && (rc != -EINVAL)) {
		FTS_ERROR("Unable to read %s", name);
		return rc;
	}

	if (!strcmp(name, "focaltech,display-coords")) {
		pdata->x_min = coords[0];
		pdata->y_min = coords[1];
		pdata->x_max = coords[2];
		pdata->y_max = coords[3];
	} else {
		FTS_ERROR("unsupported property %s", name);
		return -EINVAL;
	}

	return 0;
}

static int fts_parse_dt(struct device *dev, struct fts_ts_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;
	u32 temp_val;

	FTS_FUNC_ENTER();

	rc = fts_get_dt_coords(dev, "focaltech,display-coords", pdata);
	if (rc)
		FTS_ERROR("Unable to get display-coords");

	pdata->have_key = of_property_read_bool(np, "focaltech,have-key");
	if (pdata->have_key) {
		rc = of_property_read_u32(np, "focaltech,key-number", &pdata->key_number);
		if (rc) {
			FTS_ERROR("Key number undefined!");
		}
		rc = of_property_read_u32_array(np, "focaltech,keys",
										pdata->keys, pdata->key_number);
		if (rc) {
			FTS_ERROR("Keys undefined!");
		}
		rc = of_property_read_u32(np, "focaltech,key-y-coord", &pdata->key_y_coord);
		if (rc) {
			FTS_ERROR("Key Y Coord undefined!");
		}
		rc = of_property_read_u32_array(np, "focaltech,key-x-coords",
										pdata->key_x_coords, pdata->key_number);
		if (rc) {
			FTS_ERROR("Key X Coords undefined!");
		}
		FTS_DEBUG("%d: (%d, %d, %d), [%d, %d, %d][%d]",
				 pdata->key_number, pdata->keys[0], pdata->keys[1], pdata->keys[2],
				 pdata->key_x_coords[0], pdata->key_x_coords[1], pdata->key_x_coords[2],
				 pdata->key_y_coord);
	}

	pdata->reset_gpio = of_get_named_gpio_flags(np, "focaltech,reset-gpio", 0, &pdata->reset_gpio_flags);
	if (pdata->reset_gpio < 0) {
		FTS_ERROR("Unable to get reset_gpio");
	}

#if WANGHAN_FT5435_LDO
	def_power_ldo_gpio = of_get_named_gpio_flags(np, "focaltech,power_ldo-gpio", 0, &def_power_ldo_gpio_flags);
#endif


	pdata->irq_gpio = of_get_named_gpio_flags(np, "focaltech,irq-gpio", 0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0) {
		FTS_ERROR("Unable to get irq_gpio");
	}

	rc = of_property_read_u32(np, "focaltech,max-touch-number", &temp_val);
	if (!rc) {
		pdata->max_touch_number = temp_val;
		FTS_DEBUG("max_touch_number=%d", pdata->max_touch_number);
	} else {
		FTS_ERROR("Unable to get max-touch-number");
		pdata->max_touch_number = FTS_MAX_POINTS;
	}



	FTS_FUNC_EXIT();
	return 0;
}

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
						unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct fts_ts_data *fts_data =
		container_of(self, struct fts_ts_data, fb_notif);

	if (evdata && evdata->data && fts_data && fts_data->client) {
		blank = evdata->data;
		if (event == FB_EARLY_EVENT_BLANK && *blank == FB_BLANK_UNBLANK) {

			schedule_work(&g_resume_work);
		} else if (event == FB_EVENT_BLANK && *blank == FB_BLANK_POWERDOWN) {
			flush_work(&g_resume_work);
			mutex_lock(&ft5435_resume_mutex);
			fts_ts_suspend(&fts_data->client->dev);
			mutex_unlock(&ft5435_resume_mutex);
		}
	}

	return 0;
}
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void fts_ts_early_suspend(struct early_suspend *handler)
{
	struct fts_ts_data *data = container_of(handler,
						   struct fts_ts_data,
						   early_suspend);

	fts_ts_suspend(&data->client->dev);
}

static void fts_ts_late_resume(struct early_suspend *handler)
{
	struct fts_ts_data *data = container_of(handler,
						   struct fts_ts_data,
						   early_suspend);

	fts_ts_resume(&data->client->dev);
}
#endif


#if WANGHAN_FT5435_PINCTRL
int fts_pinctrl_init(struct i2c_client *client, struct pctrl_data *pin_data)
{

	printk("[FTS][pinctrl]%s", __func__);


	pin_data->tpctrl = devm_pinctrl_get(&(client->dev));
	if (!pin_data->tpctrl) {
		FTS_ERROR( "%s:Target does not use pinctrl\n", __func__);
		return -EINVAL;
	}

	pin_data->pctrl_state_active = pinctrl_lookup_state(pin_data->tpctrl, PINCTRL_STATE_ACTIVE);
	if (!pin_data->pctrl_state_active) {
		FTS_ERROR( "Can not lookup %s pinstate\n", PINCTRL_STATE_ACTIVE);
		return -EINVAL;
	}

	pin_data->pctrl_state_suspend = pinctrl_lookup_state(pin_data->tpctrl, PINCTRL_STATE_SUSPEND);
	if (!pin_data->pctrl_state_suspend) {
		FTS_ERROR("Can not lookup %s pinstate\n", PINCTRL_STATE_SUSPEND);
		return -EINVAL;
	}

	return 0;
}
#endif

#ifdef CONFIG_MACH_XIAOMI
extern bool xiaomi_ts_probed;
#endif

static int fts_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct fts_ts_platform_data *pdata;
	struct fts_ts_data *data;
	struct input_dev *input_dev;
	int err;

#ifdef CONFIG_MACH_XIAOMI
	if (xiaomi_ts_probed)
		return -ENODEV;
#endif

	FTS_FUNC_ENTER();
	if (strstr(g_lcd_id, "shenchao") == NULL) {
	FTS_INFO("not the focaltech tp,skiping the focaltech probe func");
		return -ENODEV;
	}
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
							sizeof(struct fts_ts_platform_data),
							GFP_KERNEL);
		if (!pdata) {
			FTS_ERROR("[MEMORY]Failed to allocate memory");
			FTS_FUNC_EXIT();
			return -ENOMEM;
		}
		err = fts_parse_dt(&client->dev, pdata);
		if (err) {
			FTS_ERROR("[DTS]DT parsing failed");
		}
	} else {
		pdata = client->dev.platform_data;
	}

	if (!pdata) {
		FTS_ERROR("Invalid pdata");
		FTS_FUNC_EXIT();
		return -EINVAL;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		FTS_ERROR("I2C not supported");
		FTS_FUNC_EXIT();
		return -ENODEV;
	}

	data = devm_kzalloc(&client->dev, sizeof(struct fts_ts_data), GFP_KERNEL);
	if (!data) {
		FTS_ERROR("[MEMORY]Failed to allocate memory");
		FTS_FUNC_EXIT();
		return -ENOMEM;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		FTS_ERROR("[INPUT]Failed to allocate input device");
		FTS_FUNC_EXIT();
		return -ENOMEM;
	}


#if WANGHAN_FT5435_PINCTRL
	pin_data = devm_kzalloc(&(client->dev), sizeof(*pin_data), GFP_KERNEL);
	err = fts_pinctrl_init(client, pin_data);
	if(err<0) {
		FTS_ERROR("fts pinctrl_init failed\n");
	}

	pinctrl_select_state(pin_data->tpctrl, pin_data->pctrl_state_active);

#endif


	data->input_dev = input_dev;
	data->client = client;
	data->pdata = pdata;

	ft5435_fts_wq_data = data;
	ft5435_fts_i2c_client = client;
	ft5435_fts_input_dev = input_dev;

	spin_lock_init(&ft5435_fts_wq_data->irq_lock);
	mutex_init(&ft5435_fts_wq_data->report_mutex);

	ft5435_fts_input_dev_init(client, data, input_dev, pdata);

#if FTS_POWER_SOURCE_CUST_EN
	fts_power_source_init(data);
	fts_power_source_ctrl(data, 1);
#endif

	err = fts_gpio_configure(data);
	if (err < 0) {
		FTS_ERROR("[GPIO]Failed to configure the gpios");
		goto free_gpio;
	}

	ft5435_fts_reset_proc(200);

	msleep(200);

	ft5435_fts_ctpm_get_upgrade_array();

	err = ft5435_fts_wait_tp_to_valid(client);
	if(err < 0) {
		FTS_ERROR("tp is not exist!!! remove it");
		goto free_gpio;
	}


	fts_LockDownInfo_get(client, data->tp_lockdown_info_temp);
	printk("[FTS][tp_lockdown_info] lockdown=%s\n", data->tp_lockdown_info_temp);


	err = request_threaded_irq(client->irq, NULL, fts_ts_interrupt,
							  pdata->irq_gpio_flags | IRQF_ONESHOT | IRQF_TRIGGER_FALLING | IRQF_PERF_CRITICAL,
							  client->dev.driver->name, data);
	if (err) {
		FTS_ERROR("Request irq failed!");
		goto free_gpio;
	}

	ft5435_fts_irq_disable();

#if FTS_PSENSOR_EN
	if ( fts_sensor_init(data) != 0) {
		FTS_ERROR("fts_sensor_init failed!");
		FTS_FUNC_EXIT();
		return 0;
	}
#endif

#if FTS_APK_NODE_EN
	ft5435_fts_create_apk_debug_channel(client);
#endif

#if FTS_SYSFS_NODE_EN
	ft5435_fts_create_sysfs(client);
#endif

#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_init();
#endif

	ft5435_fts_ex_mode_init(client);

#if FTS_GESTURE_EN
	ft5435_fts_gesture_init(input_dev, client);
#endif

#if FTS_ESDCHECK_EN
	fts_esdcheck_init();
#endif

	ft5435_fts_irq_enable();

#if FTS_AUTO_UPGRADE_EN
	fts_ctpm_upgrade_init();
#endif

#if FTS_TEST_EN
	fts_test_init(client);
#endif

#if defined(CONFIG_FB)
	data->fb_notif.notifier_call = fb_notifier_callback;
	err = fb_register_client(&data->fb_notif);
	if (err)
		FTS_ERROR("[FB]Unable to register fb_notifier: %d", err);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + FTS_SUSPEND_LEVEL;
	data->early_suspend.suspend = fts_ts_early_suspend;
	data->early_suspend.resume = fts_ts_late_resume;
	register_early_suspend(&data->early_suspend);
#endif


	if (ft5435_ft5435_fts_i2c_read_reg(ft5435_fts_i2c_client, FTS_REG_FW_VER, &g_fwver) < 0)
		printk("[FTS][ERROR][tp_info] I2c transfer error!\n");
	if(255 != g_fwver) {
		printk("[FTS][tp_info] FT5435 fw-version is right!\n");
		memset(g_fwver_buff, 0, sizeof(g_fwver_buff));
		sprintf(g_fwver_buff, "[FW]0x%02x, [IC]FT5435", g_fwver);
	}

#if FTS_GESTURE_EN
	data->gesture_data->mode = 0;
	printk("[wanghan]data->gesture_data->mode=%d", data->gesture_data->mode);
#endif

	INIT_WORK(&g_resume_work, do_ts_resume_work);
	mutex_init(&ft5435_resume_mutex);

#ifdef CONFIG_MACH_XIAOMI
	xiaomi_ts_probed = true;
#endif

	FTS_FUNC_EXIT();
	return 0;

free_gpio:

	input_unregister_device(data->input_dev);

	if (gpio_is_valid(pdata->reset_gpio))
		gpio_free(pdata->reset_gpio);

#if WANGHAN_FT5435_LDO
	if (gpio_is_valid(def_power_ldo_gpio))
		gpio_free(def_power_ldo_gpio);
#endif

	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
	return err;

}
static int fts_ts_remove(struct i2c_client *client)
{
	struct fts_ts_data *data = i2c_get_clientdata(client);

	FTS_FUNC_ENTER();
	cancel_work_sync(&data->touch_event_work);

#if FTS_PSENSOR_EN
	fts_sensor_remove(data);
#endif

#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_exit();
#endif

#if FTS_APK_NODE_EN
	ft5435_fts_release_apk_debug_channel();
#endif

#if FTS_SYSFS_NODE_EN
	ft5435_fts_remove_sysfs(client);
#endif

	ft5435_fts_ex_mode_exit(client);

#if FTS_AUTO_UPGRADE_EN
	cancel_work_sync(&ft5435_fw_update_work);
#endif

#if defined(CONFIG_FB)
	if (fb_unregister_client(&data->fb_notif))
		FTS_ERROR("Error occurred while unregistering fb_notifier.");
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&data->early_suspend);
#endif
	free_irq(client->irq, data);

	if (gpio_is_valid(data->pdata->reset_gpio))
		gpio_free(data->pdata->reset_gpio);

	if (gpio_is_valid(data->pdata->irq_gpio))
		gpio_free(data->pdata->irq_gpio);

	input_unregister_device(data->input_dev);

#if FTS_TEST_EN
	fts_test_exit(client);
#endif

#if FTS_ESDCHECK_EN
	fts_esdcheck_exit();
#endif

#ifdef CONFIG_MACH_XIAOMI
	xiaomi_ts_probed = false;
#endif

	FTS_FUNC_EXIT();
	return 0;
}

static int fts_ts_suspend(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);
	int retval = 0;

	FTS_FUNC_ENTER();
	if (data->suspended) {
		FTS_INFO("Already in suspend state");
		FTS_FUNC_EXIT();
		return -1;
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_suspend();
#endif

#if FTS_GESTURE_EN
	retval = ft5435_fts_gesture_suspend(data->client);
	if (retval == 0) {
		retval = enable_irq_wake(ft5435_fts_wq_data->client->irq);
		if (retval)
			FTS_ERROR("%s: set_irq_wake failed", __func__);
		data->suspended = true;

		FTS_FUNC_EXIT();
		return 0;
	}
#endif

#if FTS_PSENSOR_EN
	if ( fts_sensor_suspend(data) != 0 ) {
		enable_irq_wake(data->client->irq);
		data->suspended = true;
		return 0;
	}
#endif

	ft5435_fts_irq_disable();

	retval = ft5435_ft5435_fts_i2c_write_reg(data->client, FTS_REG_POWER_MODE, FTS_REG_POWER_MODE_SLEEP_VALUE);
	if (retval < 0) {
		FTS_ERROR("Set TP to sleep mode fail, ret=%d!", retval);
	}
	else
	{
		FTS_DEBUG("TP set sleep mode , ret=%d!", retval);
	}
	data->suspended = true;


#if WANGHAN_FT5435_VDD
#if FTS_GESTURE_EN
	if (data->gesture_data->mode == 0) {
		FTS_DEBUG("gesture_mode is OFF");

		if(regulator_disable(data->vdd)) {
			FTS_ERROR("Regulator vdd disable failed");
		}
	}
#else
	if(regulator_force_disable(data->vdd)) {
		FTS_ERROR("Regulator vdd disable failed");
	}
#endif
#endif


	FTS_FUNC_EXIT();

	return 0;
}

static int fts_ts_resume(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);

	FTS_FUNC_ENTER();
	if (!data->suspended) {
		FTS_DEBUG("Already in awake state");
		FTS_FUNC_EXIT();
		return -1;
	}


#if WANGHAN_FT5435_VDD
#if FTS_GESTURE_EN
	if (data->gesture_data->mode == 0) {
		FTS_DEBUG("gesture_mode is OFF");
		if(regulator_enable(data->vdd)) {
			FTS_ERROR("Regulator vdd enable failed");
		}
	} else {
		FTS_DEBUG("gesture_mode is ON!");
	}
#else
	if(regulator_enable(data->vdd)) {
		FTS_ERROR("Regulator vdd enable failed");
	}
#endif
#endif


	fts_release_all_finger();

#if (!FTS_CHIP_IDC)
	ft5435_fts_reset_proc(200);
#endif
	ft5435_fts_tp_state_recovery(data->client);

#if FTS_ESDCHECK_EN
	fts_esdcheck_resume();
#endif

#if FTS_GESTURE_EN
	if (ft5435_fts_gesture_resume(data->client) == 0) {
		int err;
		err = disable_irq_wake(data->client->irq);
		if (err)
			FTS_ERROR("%s: disable_irq_wake failed", __func__);
		data->suspended = false;
		FTS_FUNC_EXIT();
		return 0;
	}
#endif

#if FTS_PSENSOR_EN
	if ( fts_sensor_resume(data) != 0 ) {
		disable_irq_wake(data->client->irq);
		data->suspended = false;
		FTS_FUNC_EXIT();
		return 0;
	}
#endif

	data->suspended = false;

	ft5435_fts_irq_enable();

	FTS_FUNC_EXIT();
	return 0;
}

static const struct i2c_device_id fts_ts_id[] =
{
	{FTS_DRIVER_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, fts_ts_id);

static struct of_device_id fts_match_table[] =
{
	{ .compatible = "focaltech,ft5435", },
	{ },
};

static struct i2c_driver fts_ts_driver =
{
	.probe = fts_ts_probe,
	.remove = fts_ts_remove,
	.driver = {
		.name = FTS_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = fts_match_table,
	},
	.id_table = fts_ts_id,
};

static int __init fts_ts_init(void)
{
	int ret = 0;

	FTS_FUNC_ENTER();
#ifdef CONFIG_MACH_XIAOMI
	if (xiaomi_series_read() != XIAOMI_SERIES_ULYSSE)
		return -ENODEV;
#endif
	ret = i2c_add_driver(&fts_ts_driver);
	if ( ret != 0 ) {
		FTS_ERROR("Focaltech touch screen driver init failed!");
	}
	FTS_FUNC_EXIT();
	return ret;
}

static void __exit fts_ts_exit(void)
{
	i2c_del_driver(&fts_ts_driver);
}

module_init(fts_ts_init);
module_exit(fts_ts_exit);

MODULE_AUTHOR("FocalTech Driver Team");
MODULE_DESCRIPTION("FocalTech Touchscreen Driver");
MODULE_LICENSE("GPL v2");
