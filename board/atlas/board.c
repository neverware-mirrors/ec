/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Atlas board-specific configuration */

#include "adc_chip.h"
#include "bd99992gw.h"
#include "board_config.h"
#include "charge_manager.h"
#include "charger.h"
#include "charge_state.h"
#include "chipset.h"
#include "console.h"
#include "driver/accelgyro_bmi160.h"
#include "driver/als_opt3001.h"
#include "driver/pmic_bd99992gw.h"
#include "driver/tcpm/ps8xxx.h"
#include "driver/tcpm/tcpci.h"
#include "driver/tcpm/tcpm.h"
#include "espi.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "keyboard_8042_sharedlib.h"
#include "keyboard_scan.h"
#include "lid_switch.h"
#include "motion_sense.h"
#include "power_button.h"
#include "power.h"
#include "pwm_chip.h"
#include "pwm.h"
#include "spi.h"
#include "switch.h"
#include "system.h"
#include "system_chip.h"
#include "task.h"
#include "temp_sensor.h"
#include "timer.h"
#include "uart.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_SYSTEM, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_SYSTEM, format, ## args)

static void tcpc_alert_event(enum gpio_signal signal)
{
	int port = -1;

	switch (signal) {
	case GPIO_USB_C0_PD_INT_ODL:
		port = 0;
		break;
	case GPIO_USB_C1_PD_INT_ODL:
		port = 1;
		break;
	default:
		return;
	}

	schedule_deferred_pd_interrupt(port);
}

#include "gpio_list.h"

/* power signal list.  Must match order of enum power_signal. */
const struct power_signal_info power_signal_list[] = {
	{GPIO_SLP_S0_L,
	 POWER_SIGNAL_ACTIVE_HIGH | POWER_SIGNAL_DISABLE_AT_BOOT,
	 "SLP_S0_DEASSERTED"},
	{VW_SLP_S3_L,		POWER_SIGNAL_ACTIVE_HIGH, "SLP_S3_DEASSERTED"},
	{VW_SLP_S4_L,		POWER_SIGNAL_ACTIVE_HIGH, "SLP_S4_DEASSERTED"},
	{GPIO_PCH_SLP_SUS_L,	POWER_SIGNAL_ACTIVE_HIGH, "SLP_SUS_DEASSERTED"},
	{GPIO_RSMRST_L_PGOOD,	POWER_SIGNAL_ACTIVE_HIGH, "RSMRST_L_PGOOD"},
	{GPIO_PMIC_DPWROK,	POWER_SIGNAL_ACTIVE_HIGH, "PMIC_DPWROK"},
};
BUILD_ASSERT(ARRAY_SIZE(power_signal_list) == POWER_SIGNAL_COUNT);

/* Keyboard scan. Increase output_settle_us to 80us from default 50us. */
struct keyboard_scan_config keyscan_config = {
	.output_settle_us = 80,
	.debounce_down_us = 9 * MSEC,
	.debounce_up_us = 30 * MSEC,
	.scan_period_us = 3 * MSEC,
	.min_post_scan_delay_us = 1000,
	.poll_timeout_us = 100 * MSEC,
	.actual_key_mask = {
		0x3c, 0xff, 0xff, 0xff, 0xff, 0xf5, 0xff,
		0xa4, 0xff, 0xfe, 0x55, 0xfa, 0xca  /* full set */
	},
};

/* PWM channels. Must be in the exactly same order as in enum pwm_channel. */
const struct pwm_t pwm_channels[] = {
	[PWM_CH_KBLIGHT]       = { 3, 0, 10000 },
	[PWM_CH_DB0_LED_BLUE]  = {
		0, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
	[PWM_CH_DB0_LED_RED]   = {
		2, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
	[PWM_CH_DB0_LED_GREEN] = {
		6, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
	[PWM_CH_DB1_LED_BLUE]  = {
		1, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
	[PWM_CH_DB1_LED_RED]   = {
		7, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
	[PWM_CH_DB1_LED_GREEN] = {
		5, PWM_CONFIG_ACTIVE_LOW | PWM_CONFIG_DSLEEP, 2400 },
};
BUILD_ASSERT(ARRAY_SIZE(pwm_channels) == PWM_CH_COUNT);

/* Hibernate wake configuration */
const enum gpio_signal hibernate_wake_pins[] = {
	GPIO_ROP_EC_ACOK,
	GPIO_LID_OPEN,
	GPIO_MECH_PWR_BTN_ODL,
};
const int hibernate_wake_pins_used = ARRAY_SIZE(hibernate_wake_pins);

const struct adc_t adc_channels[] = {
	/*
	 * Adapter current output or battery charging/discharging current (uV)
	 * 18x amplification on charger side.
	 */
	[ADC_AMON_BMON] = {
		"AMON_BMON",
		NPCX_ADC_CH2,
		ADC_MAX_VOLT*1000/18,
		ADC_READ_MAX+1,
		0
	},
	/*
	 * ISL9238 PSYS output is 1.44 uA/W over 12.4K resistor, to read
	 * 0.8V @ 45 W, i.e. 56250 uW/mV. Using ADC_MAX_VOLT*56250 and
	 * ADC_READ_MAX+1 as multiplier/divider leads to overflows, so we
	 * only divide by 2 (enough to avoid precision issues).
	 */
	[ADC_PSYS] = {
		"PSYS",
		NPCX_ADC_CH3,
		ADC_MAX_VOLT*56250*2/(ADC_READ_MAX+1),
		2,
		0
	},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

/* I2C port map */
const struct i2c_port_t i2c_ports[]  = {
	{"power",   I2C_PORT_POWER,   100,
	 GPIO_EC_I2C0_POWER_SCL,      GPIO_EC_I2C0_POWER_SDA},
	{"tcpc0",   I2C_PORT_TCPC0,   1000,
	 GPIO_EC_I2C1_USB_C0_SCL,     GPIO_EC_I2C1_USB_C0_SDA},
	{"tcpc1",   I2C_PORT_TCPC1,   1000,
	 GPIO_EC_I2C2_USB_C1_SCL,     GPIO_EC_I2C2_USB_C1_SDA},
	{"sensor",  I2C_PORT_SENSOR,  100,
	 GPIO_EC_I2C3_SENSOR_3V3_SCL, GPIO_EC_I2C3_SENSOR_3V3_SDA},
	{"battery", I2C_PORT_BATTERY, 100,
	 GPIO_EC_I2C4_BATTERY_SCL,    GPIO_EC_I2C4_BATTERY_SDA},
	{"gyro",    I2C_PORT_GYRO,    100,
	 GPIO_EC_I2C5_GYRO_SCL,       GPIO_EC_I2C5_GYRO_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

/* TCPC mux configuration */
const struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_MAX_COUNT] = {
	{
		/* left port */
		.i2c_host_port = I2C_PORT_TCPC0,
		.i2c_slave_addr = I2C_ADDR_TCPC,
		.drv = &ps8xxx_tcpm_drv,
		/* Alert is active-low, push-pull */
		.flags = 0,
	},
	{
		/* right port */
		.i2c_host_port = I2C_PORT_TCPC1,
		.i2c_slave_addr = I2C_ADDR_TCPC,
		.drv = &ps8xxx_tcpm_drv,
		/* Alert is active-low, push-pull */
		.flags = 0,
	},
};

struct usb_mux usb_muxes[CONFIG_USB_PD_PORT_MAX_COUNT] = {
	{
		.driver = &tcpci_tcpm_usb_mux_driver,
		.hpd_update = &ps8xxx_tcpc_update_hpd_status,
	},
	{
		.driver = &tcpci_tcpm_usb_mux_driver,
		.hpd_update = &ps8xxx_tcpc_update_hpd_status,
	},
};

void board_reset_pd_mcu(void)
{
	gpio_set_level(GPIO_USB_PD_RST_L, 0);
	msleep(PS8XXX_RST_L_RST_H_DELAY_MS);
	gpio_set_level(GPIO_USB_PD_RST_L, 1);
}

void board_tcpc_init(void)
{
	int port;

	/* Only reset TCPC if not sysjump */
	if (!system_jumped_to_this_image())
		board_reset_pd_mcu();

	gpio_enable_interrupt(GPIO_USB_C0_PD_INT_ODL);
	gpio_enable_interrupt(GPIO_USB_C1_PD_INT_ODL);

	/*
	 * Initialize HPD to low; after sysjump SOC needs to see
	 * HPD pulse to enable video path
	 */
	for (port = 0; port < CONFIG_USB_PD_PORT_MAX_COUNT; port++) {
		const struct usb_mux *mux = &usb_muxes[port];

		mux->hpd_update(port, 0, 0);
	}
}
DECLARE_HOOK(HOOK_INIT, board_tcpc_init, HOOK_PRIO_INIT_I2C+1);

uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;

	if (!gpio_get_level(GPIO_USB_C0_PD_INT_ODL)) {
		if (gpio_get_level(GPIO_USB_C0_PD_RST_L))
			status |= PD_STATUS_TCPC_ALERT_0;
	}

	if (!gpio_get_level(GPIO_USB_C1_PD_INT_ODL)) {
		if (gpio_get_level(GPIO_USB_C1_PD_RST_L))
			status |= PD_STATUS_TCPC_ALERT_1;
	}

	return status;
}

const struct temp_sensor_t temp_sensors[] = {
	{"Battery", TEMP_SENSOR_TYPE_BATTERY, charge_get_battery_temp, 0, 4},
	/* BD99992GW temp sensors are only readable in S0 */
	{"Ambient", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM0, 4},
	{"Charger", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM1, 4},
	{"DRAM", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM2, 4},
	{"eMMC", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM3, 4},
	{"gyro", TEMP_SENSOR_TYPE_BOARD, bmi160_get_sensor_temp, BASE_GYRO, 1},
};
BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

/*
 * Check if PMIC fault registers indicate VR fault. If yes, print out fault
 * register info to console. Additionally, set panic reason so that the OS can
 * check for fault register info by looking at offset 0x14(PWRSTAT1) and
 * 0x15(PWRSTAT2) in cros ec panicinfo.
 */
static void board_report_pmic_fault(const char *str)
{
	int vrfault, pwrstat1 = 0, pwrstat2 = 0;
	uint32_t info;

	/* RESETIRQ1 -- Bit 4: VRFAULT */
	if (i2c_read8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		      BD99992GW_REG_RESETIRQ1, &vrfault) != EC_SUCCESS)
		return;

	if (!(vrfault & (1 << 4)))
		return;

	/* VRFAULT has occurred, print VRFAULT status bits. */

	/* PWRSTAT1 */
	i2c_read8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		  BD99992GW_REG_PWRSTAT1, &pwrstat1);

	/* PWRSTAT2 */
	i2c_read8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		  BD99992GW_REG_PWRSTAT2, &pwrstat2);

	CPRINTS("PMIC VRFAULT: %s", str);
	CPRINTS("PMIC VRFAULT: PWRSTAT1=0x%02x PWRSTAT2=0x%02x", pwrstat1,
		pwrstat2);

	/* Clear all faults -- Write 1 to clear. */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_RESETIRQ1, (1 << 4));
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_PWRSTAT1, pwrstat1);
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_PWRSTAT2, pwrstat2);

	/*
	 * Status of the fault registers can be checked in the OS by looking at
	 * offset 0x14(PWRSTAT1) and 0x15(PWRSTAT2) in cros ec panicinfo.
	 */
	info = ((pwrstat2 & 0xFF) << 8) | (pwrstat1 & 0xFF);
	panic_set_reason(PANIC_SW_PMIC_FAULT, info, 0);
}

static void board_pmic_disable_slp_s0_vr_decay(void)
{
	/*
	 * VCCIOCNT:
	 * Bit 6    (0)   - Disable decay of VCCIO on SLP_S0# assertion
	 * Bits 5:4 (11)  - Nominal output voltage: 0.850V
	 * Bits 3:2 (10)  - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10)  - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_VCCIOCNT, 0x3a);

	/*
	 * V18ACNT:
	 * Bits 7:6 (00) - Disable low power mode on SLP_S0# assertion
	 * Bits 5:4 (10) - Nominal voltage set to 1.8V
	 * Bits 3:2 (10) - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10) - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_V18ACNT, 0x2a);

	/*
	 * V085ACNT:
	 * Bits 7:6 (00) - Disable low power mode on SLP_S0# assertion
	 * Bits 5:4 (10) - Nominal voltage 0.85V
	 * Bits 3:2 (10) - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10) - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_V085ACNT, 0x2a);
}

static void board_pmic_enable_slp_s0_vr_decay(void)
{
	/*
	 * VCCIOCNT:
	 * Bit 6    (1)   - Enable decay of VCCIO on SLP_S0# assertion
	 * Bits 5:4 (11)  - Nominal output voltage: 0.850V
	 * Bits 3:2 (10)  - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10)  - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_VCCIOCNT, 0x7a);

	/*
	 * V18ACNT:
	 * Bits 7:6 (01) - Enable low power mode on SLP_S0# assertion
	 * Bits 5:4 (10) - Nominal voltage set to 1.8V
	 * Bits 3:2 (10) - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10) - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_V18ACNT, 0x6a);

	/*
	 * V085ACNT:
	 * Bits 7:6 (01) - Enable low power mode on SLP_S0# assertion
	 * Bits 5:4 (10) - Nominal voltage 0.85V
	 * Bits 3:2 (10) - VR set to AUTO on SLP_S0# de-assertion
	 * Bits 1:0 (10) - VR set to AUTO operating mode
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_V085ACNT, 0x6a);
}

void power_board_handle_host_sleep_event(enum host_sleep_event state)
{
	if (state == HOST_SLEEP_EVENT_S0IX_SUSPEND)
		board_pmic_enable_slp_s0_vr_decay();
	else if (state == HOST_SLEEP_EVENT_S0IX_RESUME)
		board_pmic_disable_slp_s0_vr_decay();
}

static void board_pmic_init(void)
{
	board_report_pmic_fault("SYSJUMP");

	/* Clear power source events */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_PWRSRCINT, 0xff);

	/* Disable power button shutdown timer */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_PBCONFIG, 0x00);

	if (system_jumped_to_this_image())
		return;

	/* DISCHGCNT2 - enable 100 ohm discharge on V5.0A, V3.3A and V1.8A */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_DISCHGCNT2, 0x45);

	/* DISCHGCNT3 - enable 100 ohm discharge on V1.00A */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_DISCHGCNT3, 0x04);

	/* VRMODECTRL - disable low-power mode for all rails */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_VRMODECTRL, 0x1f);

	board_pmic_disable_slp_s0_vr_decay();
}
DECLARE_HOOK(HOOK_INIT, board_pmic_init, HOOK_PRIO_DEFAULT);

void board_hibernate(void)
{
	int p;

	/* Configure PSL pins */
	for (p = 0; p < hibernate_wake_pins_used; p++)
		system_config_psl_mode(hibernate_wake_pins[p]);

	/*
	 * Enter PSL mode.  Note that on Atlas, simply enabling PSL mode does
	 * not cut the EC's power.  Therefore, we'll need to cut off power via
	 * the ROP PMIC afterwards.
	 */
	system_enter_psl_mode();

	/* Cut off DSW power via the ROP PMIC. */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992,
		   BD99992GW_REG_SDWNCTRL, BD99992GW_SDWNCTRL_SWDN);

	/* Wait for power to be cut. */
	while (1)
		;
}

/* Initialize board. */
static void board_init(void)
{
	/* Provide AC status to the PCH */
	gpio_set_level(GPIO_PCH_ACOK, extpower_is_present());

	/* Enable interrupts from BMI160 sensor. */
	gpio_enable_interrupt(GPIO_ACCELGYRO3_INT_L);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

static void board_extpower(void)
{
	gpio_set_level(GPIO_PCH_ACOK, extpower_is_present());
}
DECLARE_HOOK(HOOK_AC_CHANGE, board_extpower, HOOK_PRIO_DEFAULT);

/**
 * Set active charge port -- only one port can be active at a time.
 *
 * @param charge_port   Charge port to enable.
 *
 * Returns EC_SUCCESS if charge port is accepted and made active,
 * EC_ERROR_* otherwise.
 */
int board_set_active_charge_port(int charge_port)
{
	/* charge port is a physical port */
	int is_real_port = (charge_port >= 0 &&
			    charge_port < CONFIG_USB_PD_PORT_MAX_COUNT);
	/* check if we are sourcing VBUS on the port */
	int is_source = gpio_get_level(charge_port == 0 ?
			GPIO_USB_C0_5V_EN : GPIO_USB_C1_5V_EN);

	if (is_real_port && is_source) {
		CPRINTF("No charging on source port p%d is ", charge_port);
		return EC_ERROR_INVAL;
	}

	CPRINTF("New chg p%d", charge_port);

	if (charge_port == CHARGE_PORT_NONE) {
		/* Disable both ports */
		gpio_set_level(GPIO_EN_USB_C0_CHARGE_L, 1);
		gpio_set_level(GPIO_EN_USB_C1_CHARGE_L, 1);
	} else {
		/* Make sure non-charging port is disabled */
		gpio_set_level(charge_port ? GPIO_EN_USB_C0_CHARGE_L :
					     GPIO_EN_USB_C1_CHARGE_L, 1);
		/* Enable charging port */
		gpio_set_level(charge_port ? GPIO_EN_USB_C1_CHARGE_L :
					     GPIO_EN_USB_C0_CHARGE_L, 0);
	}

	return EC_SUCCESS;
}

/**
 * Set the charge limit based upon desired maximum.
 *
 * @param port          Port number.
 * @param supplier      Charge supplier type.
 * @param charge_ma     Desired charge limit (mA).
 * @param charge_mv     Negotiated charge voltage (mV).
 */
void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv)
{
	/*
	 * Limit the input current to 95% negotiated limit,
	 * to account for the charger chip margin.
	 */
	charge_ma = (charge_ma * 95) / 100;
	charge_set_input_current_limit(MAX(charge_ma,
				   CONFIG_CHARGER_INPUT_CURRENT), charge_mv);
}

static void board_chipset_suspend(void)
{
	gpio_set_level(GPIO_ENABLE_BACKLIGHT, 0);
	gpio_set_level(GPIO_KBD_BL_EN, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, board_chipset_suspend, HOOK_PRIO_DEFAULT);

static void board_chipset_resume(void)
{
	gpio_set_level(GPIO_ENABLE_BACKLIGHT, 1);
	gpio_set_level(GPIO_KBD_BL_EN, 1);
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, board_chipset_resume, HOOK_PRIO_DEFAULT);

static void board_chipset_reset(void)
{
	board_report_pmic_fault("CHIPSET RESET");
}
DECLARE_HOOK(HOOK_CHIPSET_RESET, board_chipset_reset, HOOK_PRIO_DEFAULT);

int board_get_version(void)
{
	static int ver;

	if (!ver) {
		/*
		 * Read the board EC ID on the tristate strappings
		 * using ternary encoding: 0 = 0, 1 = 1, Hi-Z = 2
		 */
		uint8_t id0, id1, id2;

		id0 = gpio_get_ternary(GPIO_BOARD_VERSION1);
		id1 = gpio_get_ternary(GPIO_BOARD_VERSION2);
		id2 = gpio_get_ternary(GPIO_BOARD_VERSION3);

		ver = (id2 * 9) + (id1 * 3) + id0;
		CPRINTS("Board ID = %d", ver);
	}

	return ver;
}

/* Base Sensor mutex */
static struct mutex g_base_mutex;

static struct bmi160_drv_data_t g_bmi160_data;
static struct opt3001_drv_data_t g_opt3001_data = {
	.scale = 1,
	.uscale = 0,
	.offset = 0,
};

/* Matrix to rotate accelerometer into standard reference frame */
const mat33_fp_t base_standard_ref = {
	{ FLOAT_TO_FP(-1), 0, 0},
	{ 0,  FLOAT_TO_FP(1), 0},
	{ 0, 0, FLOAT_TO_FP(-1)}
};

struct motion_sensor_t motion_sensors[] = {
	[BASE_ACCEL] = {
		.name = "Base Accel",
		.active_mask = SENSOR_ACTIVE_S0_S3_S5,
		.chip = MOTIONSENSE_CHIP_BMI160,
		.type = MOTIONSENSE_TYPE_ACCEL,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &bmi160_drv,
		.mutex = &g_base_mutex,
		.drv_data = &g_bmi160_data,
		.port = I2C_PORT_GYRO,
		.addr = BMI160_ADDR0,
		.rot_standard_ref = &base_standard_ref,
		.default_range = 2,  /* g, enough for laptop. */
		.min_frequency = BMI160_ACCEL_MIN_FREQ,
		.max_frequency = BMI160_ACCEL_MAX_FREQ,
		.config = {
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 10000 | ROUND_UP_FLAG,
				.ec_rate = 100 * MSEC,
			},
			[SENSOR_CONFIG_EC_S3] = {
				.odr = 10000 | ROUND_UP_FLAG,
			},
		},
	},
	[BASE_GYRO] = {
		.name = "Base Gyro",
		.active_mask = SENSOR_ACTIVE_S0_S3_S5,
		.chip = MOTIONSENSE_CHIP_BMI160,
		.type = MOTIONSENSE_TYPE_GYRO,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &bmi160_drv,
		.mutex = &g_base_mutex,
		.drv_data = &g_bmi160_data,
		.port = I2C_PORT_GYRO,
		.addr = BMI160_ADDR0,
		.default_range = 1000, /* dps */
		.rot_standard_ref = &base_standard_ref,
		.min_frequency = BMI160_GYRO_MIN_FREQ,
		.max_frequency = BMI160_GYRO_MAX_FREQ,
	},
	[LID_ALS] = {
		.name = "Light",
		.active_mask = SENSOR_ACTIVE_S0,
		.chip = MOTIONSENSE_CHIP_OPT3001,
		.type = MOTIONSENSE_TYPE_LIGHT,
		.location = MOTIONSENSE_LOC_LID,
		.drv = &opt3001_drv,
		.drv_data = &g_opt3001_data,
		.port = I2C_PORT_SENSOR,
		.addr = OPT3001_I2C_ADDR,
		.rot_standard_ref = NULL,
		.default_range = 0x2b11a1, /* from nocturne */
		.min_frequency = OPT3001_LIGHT_MIN_FREQ,
		.max_frequency = OPT3001_LIGHT_MAX_FREQ,
		.config = {
			/* Sensor on in S0 */
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 1000,
			},
		},
	},
};
const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);

/* ALS instances when LPC mapping is needed. Each entry directs to a sensor. */
const struct motion_sensor_t *motion_als_sensors[] = {
	&motion_sensors[LID_ALS],
};
BUILD_ASSERT(ARRAY_SIZE(motion_als_sensors) == ALS_COUNT);
