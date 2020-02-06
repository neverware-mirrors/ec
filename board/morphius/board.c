/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Morphius board configuration */

#include "button.h"
#include "driver/accelgyro_bmi160.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "lid_switch.h"
#include "power.h"
#include "power_button.h"
#include "switch.h"
#include "system.h"
#include "usb_charge.h"

#include "gpio_list.h"

/* These GPIOs moved. Temporarily detect and support the V0 HW. */
enum gpio_signal GPIO_PCH_PWRBTN_L = GPIO_EC_FCH_PWR_BTN_L;
enum gpio_signal GPIO_PCH_SYS_PWROK = GPIO_EC_FCH_PWROK;

void board_update_sensor_config_from_sku(void)
{
	int data;

	/*
	 * If the CBI EEPROM is found on the battery I2C port then we are
	 * running on V0 HW so re-map the GPIOs that moved.
	 */
	if ((system_get_sku_id() == 0)
	    && (i2c_read8(I2C_PORT_BATTERY, I2C_ADDR_EEPROM_FLAGS, 0, &data)
		== EC_SUCCESS)) {
		ccprints("V0 HW detected");
		GPIO_PCH_PWRBTN_L = GPIO_EC_FCH_PWR_BTN_L_V0;
		GPIO_PCH_SYS_PWROK = GPIO_EC_FCH_PWROK_V0;
	}

	/* Enable Gyro interrupts */
	gpio_enable_interrupt(GPIO_6AXIS_INT_L);
}

void board_init(void)
{
	gpio_enable_interrupt(GPIO_EN_PWR_TOUCHPAD_PS2);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_INIT_I2C + 1);

static void trackpoint_reset_deferred(void)
{
	gpio_set_level(GPIO_EC_PS2_RESET, 1);
	msleep(2);
	gpio_set_level(GPIO_EC_PS2_RESET, 0);
}
DECLARE_DEFERRED(trackpoint_reset_deferred);


void ps2_pwr_en_interrupt(enum gpio_signal signal)
{
	hook_call_deferred(&trackpoint_reset_deferred_data, MSEC);
}
