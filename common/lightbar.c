/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * LED controls.
 */

#include "board.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "lightbar.h"
#include "task.h"
#include "timer.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_LIGHTBAR, outstr)
#define CPRINTF(format, args...) cprintf(CC_LIGHTBAR, format, ## args)

/******************************************************************************/
/* How to talk to the controller */
/******************************************************************************/

/* Since there's absolutely nothing we can do about it if an I2C access
 * isn't working, we're completely ignoring any failures. */

static const uint8_t i2c_addr[] = { 0x54, 0x56 };

static inline void controller_write(int ctrl_num, uint8_t reg, uint8_t val)
{
	i2c_write8(I2C_PORT_LIGHTBAR, i2c_addr[ctrl_num], reg, val);
}

static inline uint8_t controller_read(int ctrl_num, uint8_t reg)
{
	int val = 0;
	i2c_read8(I2C_PORT_LIGHTBAR, i2c_addr[ctrl_num], reg, &val);
	return val;
}

/******************************************************************************/
/* Controller details. We have an ADP8861 and and ADP8863, but we can treat
 * them identically for our purposes */
/******************************************************************************/

/* We need to limit the total current per ISC to no more than 20mA (5mA per
 * color LED, but we have four LEDs in parallel on each ISC). Any more than
 * that runs the risk of damaging the LED component. A value of 0x67 is as high
 * as we want (assuming Square Law), but the blue LED is the least bright, so
 * I've lowered the other colors until they all appear approximately equal
 * brightness when full on. That's still pretty bright and a lot of current
 * drain on the battery, so we'll probably rarely go that high. */
#define MAX_RED   0x5c
#define MAX_GREEN 0x38
#define MAX_BLUE  0x67

/* How many LEDs do we have? */
#define NUM_LEDS 4

/* How we'd like to see the driver chips initialized. The controllers have some
 * auto-cycling capability, but it's not much use for our purposes. For now,
 * we'll just control all color changes actively. */
struct initdata_s {
	uint8_t reg;
	uint8_t val;
};

static const struct initdata_s init_vals[] = {
	{0x04, 0x00},				/* no backlight function */
	{0x05, 0x3f},				/* xRGBRGB per chip */
	{0x0f, 0x01},				/* square law looks better */
	{0x10, 0x3f},				/* enable independent LEDs */
	{0x11, 0x00},				/* no auto cycling */
	{0x12, 0x00},				/* no auto cycling */
	{0x13, 0x00},				/* instant fade in/out */
	{0x14, 0x00},				/* not using LED 7 */
	{0x15, 0x00},				/* current for LED 6 (blue) */
	{0x16, 0x00},				/* current for LED 5 (red) */
	{0x17, 0x00},				/* current for LED 4 (green) */
	{0x18, 0x00},				/* current for LED 3 (blue) */
	{0x19, 0x00},				/* current for LED 2 (red) */
	{0x1a, 0x00},				/* current for LED 1 (green) */
};

static void set_from_array(const struct initdata_s *data, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		controller_write(0, data[i].reg, data[i].val);
		controller_write(1, data[i].reg, data[i].val);
	}
}

/* Controller register lookup tables. */
static const uint8_t led_to_ctrl[] = { 0, 0, 1, 1 };
static const uint8_t led_to_isc[] = { 0x15, 0x18, 0x15, 0x18 };

/* Scale 0-255 into max value */
static inline uint8_t scale_abs(int val, int max)
{
	return (val * max)/255 + max/256;
}

/* It will often be simpler to provide an overall brightness control. */
static int brightness = 255;

/* So that we can make brightness changes happen instantly, we need to track
 * the current values. The values in the controllers aren't very helpful. */
static uint8_t current[NUM_LEDS][3];

/* Scale 0-255 by brightness */
static inline uint8_t scale(int val, int max)
{
	return scale_abs((val * brightness)/255, max);
}

static void lightbar_init_vals(void)
{
	CPRINTF("[%T LB_init_vals]\n");
	set_from_array(init_vals, ARRAY_SIZE(init_vals));
	memset(current, 0, sizeof(current));
}


/* Helper function. */
static void setrgb(int led, int red, int green, int blue)
{
	int ctrl, bank;
	current[led][0] = red;
	current[led][1] = green;
	current[led][2] = blue;
	ctrl = led_to_ctrl[led];
	bank = led_to_isc[led];
	controller_write(ctrl, bank, scale(blue, MAX_BLUE));
	controller_write(ctrl, bank+1, scale(red, MAX_RED));
	controller_write(ctrl, bank+2, scale(green, MAX_GREEN));
}


/******************************************************************************/
/* Basic LED control functions. */
/******************************************************************************/

static void lightbar_off(void)
{
	CPRINTF("[%T LB_off]\n");
	/* Just go into standby mode. No register values should change. */
	controller_write(0, 0x01, 0x00);
	controller_write(1, 0x01, 0x00);
}

static void lightbar_on(void)
{
	CPRINTF("[%T LB_on]\n");
	/* Come out of standby mode. */
	controller_write(0, 0x01, 0x20);
	controller_write(1, 0x01, 0x20);
}


/* LEDs are numbered 0-3, RGB values should be in 0-255.
 * If you specify too large an LED, it sets them all. */
static void lightbar_setrgb(int led, int red, int green, int blue)
{
	int i;
	if (led >= NUM_LEDS)
		for (i = 0; i < NUM_LEDS; i++)
			setrgb(i, red, green, blue);
	else
		setrgb(led, red, green, blue);
}

static inline void lightbar_brightness(int newval)
{
	int i;
	CPRINTF("[%T LB_bright 0x%02x]\n", newval);
	brightness = newval;
	for (i = 0; i < NUM_LEDS; i++)
		lightbar_setrgb(i, current[i][0],
				  current[i][1], current[i][2]);
}


/******************************************************************************/

/* Major colors */
static const struct {
	uint8_t r, g, b;
} testy[] = {
	{0xff, 0x00, 0x00},
	{0x00, 0xff, 0x00},
	{0x00, 0x00, 0xff},
	{0xff, 0xff, 0x00},		/* The first four are Google colors */
	{0x00, 0xff, 0xff},
	{0xff, 0x00, 0xff},
	{0xff, 0xff, 0xff},
};


/******************************************************************************/
/* Now for the pretty patterns */
/******************************************************************************/

/* Interruptible delay */
#define WAIT_OR_RET(A) do { \
	uint32_t msg = task_wait_event(A); \
	if (TASK_EVENT_CUSTOM(msg)) \
		return TASK_EVENT_CUSTOM(msg); } while (0)

/* CPU is off */
static uint32_t sequence_S5(void)
{
	/* Just wait forever. */
	lightbar_off();
	WAIT_OR_RET(-1);
	return 0;
}

/* CPU is powering up. The lightbar loses power when the CPU is in S5, so this
 * might not be useful. */
static uint32_t sequence_S5S3(void)
{
	int i;

	/* The controllers need 100us after power is applied before they'll
	 * respond. */
	usleep(100);
	lightbar_init_vals();

	/* For now, do something to indicate this transition.
	 * We might see it. */
	lightbar_on();
	for (i = 0; i < NUM_LEDS; i++)
		lightbar_setrgb(i, 0, 255, 0);
	WAIT_OR_RET(500000);
	return 0;
}

/* CPU is fully on */
static uint32_t sequence_S0(void)
{
	lightbar_on();
	lightbar_setrgb(NUM_LEDS, 255, 255, 255);
	WAIT_OR_RET(-1);
	return 0;
}

/* CPU is going to sleep */
static uint32_t sequence_S0S3(void)
{
	lightbar_on();
	lightbar_setrgb(0, 0, 0, 0);
	WAIT_OR_RET(200000);
	lightbar_setrgb(1, 0, 0, 0);
	WAIT_OR_RET(200000);
	lightbar_setrgb(2, 0, 0, 0);
	WAIT_OR_RET(200000);
	lightbar_setrgb(3, 0, 0, 0);
	return 0;
}

/* CPU is sleeping */
static uint32_t sequence_S3(void)
{
	lightbar_off();
	lightbar_init_vals();
	lightbar_setrgb(NUM_LEDS, 0, 0, 0);
	while (1) {
		WAIT_OR_RET(3000000);
		lightbar_on();
		/* FIXME: indicate battery level? */
		lightbar_setrgb(NUM_LEDS, 255, 255, 255);
		WAIT_OR_RET(100000);
		lightbar_setrgb(NUM_LEDS, 0, 0, 0);
		lightbar_off();
	}
	return 0;
}

/* CPU is waking from sleep */
static uint32_t sequence_S3S0(void)
{
	int i;
	lightbar_init_vals();
	lightbar_on();
	for (i = 0; i < NUM_LEDS; i++) {
		lightbar_setrgb(i, 255, 255, 255);
		WAIT_OR_RET(200000);
	}
	return 0;
}

/* Sleep to off. */
static uint32_t sequence_S3S5(void)
{
	int i;

	/* For now, do something to indicate this transition.
	 * We might see it. */
	lightbar_on();
	for (i = 0; i < NUM_LEDS; i++)
		lightbar_setrgb(i, 255, 0, 0);
	WAIT_OR_RET(500000);
	return 0;
}

/* FIXME: This can be removed. */
static uint32_t sequence_TEST(void)
{
	int i, j, k, r, g, b;
	int kmax = 254;
	int kstep = 8;

	lightbar_init_vals();
	lightbar_on();
	for (i = 0; i < ARRAY_SIZE(testy); i++) {
		for (k = 0; k <= kmax; k += kstep) {
			for (j = 0; j < NUM_LEDS; j++) {
				r = testy[i].r ? k : 0;
				g = testy[i].g ? k : 0;
				b = testy[i].b ? k : 0;
				lightbar_setrgb(j, r, g, b);
			}
			WAIT_OR_RET(10000);
		}
		for (k = kmax; k >= 0; k -= kstep) {
			for (j = 0; j < NUM_LEDS; j++) {
				r = testy[i].r ? k : 0;
				g = testy[i].g ? k : 0;
				b = testy[i].b ? k : 0;
				lightbar_setrgb(j, r, g, b);
			}
			WAIT_OR_RET(10000);
		}
	}
	return 0;
}

/* This uses the auto-cycling features of the controllers to make a semi-random
 * pattern of slowly fading colors. This is interesting only because it doesn't
 * require any effort from the EC. */
static uint32_t sequence_PULSE(void)
{
	uint32_t msg;
	int r = scale(255, MAX_RED);
	int g = scale(255, MAX_BLUE);
	int b = scale(255, MAX_GREEN);
	struct initdata_s pulse_vals[] = {
		{0x11, 0xce},
		{0x12, 0x67},
		{0x13, 0xef},
		{0x15, b},
		{0x16, r},
		{0x17, g},
		{0x18, b},
		{0x19, r},
		{0x1a, g},
	};

	lightbar_init_vals();
	lightbar_on();

	set_from_array(pulse_vals, ARRAY_SIZE(pulse_vals));
	controller_write(1, 0x13, 0xcd);	/* this one's different */

	/* Not using WAIT_OR_RET() here, because we want to clean up when we're
	 * done. The only way out is to get a message. */
	msg = task_wait_event(-1);
	lightbar_init_vals();
	return TASK_EVENT_CUSTOM(msg);
}



/* The host CPU (or someone) is going to poke at the lightbar directly, so we
 * don't want the EC messing with it. We'll just sit here and ignore all
 * other messages until we're told to continue. */
static uint32_t sequence_STOP(void)
{
	uint32_t msg;

	do {
		msg = TASK_EVENT_CUSTOM(task_wait_event(-1));
		CPRINTF("[%T LB_stop got msg 0x%x]\n", msg);
	} while (msg != LIGHTBAR_RUN);
	/* FIXME: What should we do if the host shuts down? */

	CPRINTF("[%T LB_stop->running]\n");
	return 0;
}

/* Telling us to run when we're already running should do nothing. */
static uint32_t sequence_RUN(void)
{
	return 0;
}

/* We shouldn't come here, but if we do it shouldn't hurt anything */
static uint32_t sequence_ERROR(void)
{
	lightbar_init_vals();
	lightbar_on();

	lightbar_setrgb(0, 255, 255, 255);
	lightbar_setrgb(1, 255, 0, 255);
	lightbar_setrgb(2, 0, 255, 255);
	lightbar_setrgb(3, 255, 255, 255);

	WAIT_OR_RET(10000000);
	return 0;
}


static const struct {
	uint8_t led;
	uint8_t r, g, b;
	unsigned int delay;
} konami[] = {

	{1, 0xff, 0xff, 0x00, 0},
	{2, 0xff, 0xff, 0x00, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 100000},

	{1, 0xff, 0xff, 0x00, 0},
	{2, 0xff, 0xff, 0x00, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0x00, 0xff, 0},
	{3, 0x00, 0x00, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0x00, 0xff, 0},
	{3, 0x00, 0x00, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0xff, 0x00, 0x00, 0},
	{1, 0xff, 0x00, 0x00, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{1, 0x00, 0x00, 0x00, 100000},

	{2, 0x00, 0xff, 0x00, 0},
	{3, 0x00, 0xff, 0x00, 100000},
	{2, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0xff, 0x00, 0x00, 0},
	{1, 0xff, 0x00, 0x00, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{1, 0x00, 0x00, 0x00, 100000},

	{2, 0x00, 0xff, 0x00, 0},
	{3, 0x00, 0xff, 0x00, 100000},
	{2, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0xff, 0xff, 0},
	{2, 0x00, 0xff, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 150000},

	{1, 0xff, 0x00, 0xff, 0},
	{3, 0xff, 0x00, 0xff, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 250000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

};

static uint32_t sequence_KONAMI(void)
{
	int i;
	int tmp;

	lightbar_init_vals();
	lightbar_on();

	tmp = brightness;
	brightness = 255;

	for (i = 0; i < ARRAY_SIZE(konami); i++) {
		lightbar_setrgb(konami[i].led,
				  konami[i].r, konami[i].g, konami[i].b);
		if (konami[i].delay)
			usleep(konami[i].delay);
	}

	brightness = tmp;
	return 0;
}

/****************************************************************************/
/* The main lightbar task. It just cycles between various pretty patterns. */
/****************************************************************************/

/* Link each sequence with a command to invoke it. */
struct lightbar_cmd_t {
	const char * const string;
	uint32_t (*sequence)(void);
};

#define LBMSG(state) { #state, sequence_##state }
#include "lightbar_msg_list.h"
static struct lightbar_cmd_t lightbar_cmds[] = {
	LIGHTBAR_MSG_LIST
};
#undef LBMSG

static enum lightbar_sequence current_state, previous_state;

void lightbar_task(void)
{
	uint32_t msg;

	/* Reset lightbar to a known state */
	/* TODO: (crosbug.com/p/9561) only needed on proto1, since on EVT the
	 * lightbar loses power when +5VALW goes away, and will come back on
	 * with these default values anyway.
	 *
	 * Also note than on EVT, it costs more power to deassert lightbar
	 * reset than it does to hold the lightbar in reset, because the
	 * lightbar reset line is a leakage path from +3VALW.  On proto1,
	 * that's reversed, because the LED drivers consume more power held in
	 * reset than out of reset. */
	lightbar_init_vals();
	lightbar_off();
	lightbar_brightness(0x40);		/* default brightness */

	current_state = LIGHTBAR_S5;
	previous_state = LIGHTBAR_S5;

	while (1) {
		CPRINTF("[%T LB task %d = %s]\n",
			current_state, lightbar_cmds[current_state].string);
		msg = lightbar_cmds[current_state].sequence();
		msg = TASK_EVENT_CUSTOM(msg);
		if (msg && msg < LIGHTBAR_NUM_SEQUENCES) {
			CPRINTF("[%T LB msg %d = %s]\n", msg,
				lightbar_cmds[msg].string);
			previous_state = current_state;
			current_state = TASK_EVENT_CUSTOM(msg);
		} else {
			CPRINTF("[%T LB msg %d]\n", msg);
			switch (current_state) {
			case LIGHTBAR_S5S3:
				current_state = LIGHTBAR_S3;
				break;
			case LIGHTBAR_S3S0:
				current_state = LIGHTBAR_S0;
				break;
			case LIGHTBAR_S0S3:
				current_state = LIGHTBAR_S3;
				break;
			case LIGHTBAR_S3S5:
				current_state = LIGHTBAR_S5;
				break;
			case LIGHTBAR_TEST:
			case LIGHTBAR_STOP:
			case LIGHTBAR_RUN:
			case LIGHTBAR_ERROR:
			case LIGHTBAR_KONAMI:
				current_state = previous_state;
			default:
				break;
			}
		}
	}
}

/* Function to request a preset sequence from the lightbar task. */
void lightbar_sequence(enum lightbar_sequence num)
{
	if (num && num < LIGHTBAR_NUM_SEQUENCES) {
		CPRINTF("[%T LB_seq %d = %s]\n", num,
			lightbar_cmds[num].string);
		task_set_event(TASK_ID_LIGHTBAR,
			       TASK_EVENT_WAKE | TASK_EVENT_CUSTOM(num), 0);
	} else
		CPRINTF("[%T LB_seq %d - ignored]\n", num);
}

/****************************************************************************/
/* Get notifications from other parts of the system */

static int lightbar_startup(void)
{
	lightbar_sequence(LIGHTBAR_S5S3);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, lightbar_startup, HOOK_PRIO_DEFAULT);

static int lightbar_resume(void)
{
	lightbar_sequence(LIGHTBAR_S3S0);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, lightbar_resume, HOOK_PRIO_DEFAULT);

static int lightbar_suspend(void)
{
	lightbar_sequence(LIGHTBAR_S0S3);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, lightbar_suspend, HOOK_PRIO_DEFAULT);

static int lightbar_shutdown(void)
{
	lightbar_sequence(LIGHTBAR_S3S5);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, lightbar_shutdown, HOOK_PRIO_DEFAULT);

/****************************************************************************/
/* Generic command-handling (should work the same for both console & LPC) */
/****************************************************************************/

static const uint8_t dump_reglist[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a,                         0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a
};

static void do_cmd_dump(struct ec_params_lightbar_cmd *ptr)
{
	int i;
	uint8_t reg;

	BUILD_ASSERT(ARRAY_SIZE(dump_reglist) ==
		     ARRAY_SIZE(ptr->out.dump.vals));

	for (i = 0; i < ARRAY_SIZE(dump_reglist); i++) {
		reg = dump_reglist[i];
		ptr->out.dump.vals[i].reg = reg;
		ptr->out.dump.vals[i].ic0 = controller_read(0, reg);
		ptr->out.dump.vals[i].ic1 = controller_read(1, reg);
	}
}

static void do_cmd_rgb(uint8_t led,
		       uint8_t red, uint8_t green, uint8_t blue)
{
	int i;

	if (led >= NUM_LEDS)
		for (i = 0; i < NUM_LEDS; i++)
			lightbar_setrgb(i, red, green, blue);
	else
		lightbar_setrgb(led, red, green, blue);
}


/****************************************************************************/
/* Host commands via LPC bus */
/****************************************************************************/

static int lpc_cmd_lightbar(uint8_t *data, int *resp_size)
{
	struct ec_params_lightbar_cmd *ptr =
		(struct ec_params_lightbar_cmd *)data;

	switch (ptr->in.cmd) {
	case LIGHTBAR_CMD_DUMP:
		do_cmd_dump(ptr);
		*resp_size = sizeof(struct ec_params_lightbar_cmd);
		break;
	case LIGHTBAR_CMD_OFF:
		lightbar_off();
		break;
	case LIGHTBAR_CMD_ON:
		lightbar_on();
		break;
	case LIGHTBAR_CMD_INIT:
		lightbar_init_vals();
		break;
	case LIGHTBAR_CMD_BRIGHTNESS:
		lightbar_brightness(ptr->in.brightness.num);
		break;
	case LIGHTBAR_CMD_SEQ:
		lightbar_sequence(ptr->in.seq.num);
		break;
	case LIGHTBAR_CMD_REG:
		controller_write(ptr->in.reg.ctrl,
				 ptr->in.reg.reg,
				 ptr->in.reg.value);
		break;
	case LIGHTBAR_CMD_RGB:
		do_cmd_rgb(ptr->in.rgb.led,
			   ptr->in.rgb.red,
			   ptr->in.rgb.green,
			   ptr->in.rgb.blue);
		break;
	case LIGHTBAR_CMD_GET_SEQ:
		ptr->out.get_seq.num = current_state;
		*resp_size = sizeof(struct ec_params_lightbar_cmd);
		break;
	default:
		CPRINTF("[%T LB bad cmd 0x%x]\n", ptr->in.cmd);
		return EC_RES_INVALID_PARAM;
	}

	return EC_RES_SUCCESS;
}

DECLARE_HOST_COMMAND(EC_CMD_LIGHTBAR_CMD, lpc_cmd_lightbar);


/****************************************************************************/
/* EC console commands */
/****************************************************************************/

#ifdef CONSOLE_COMMAND_LIGHTBAR_HELP
static int help(const char *cmd)
{
	ccprintf("Usage:\n");
	ccprintf("  %s                       - dump all regs\n", cmd);
	ccprintf("  %s off                   - enter standby\n", cmd);
	ccprintf("  %s on                    - leave standby\n", cmd);
	ccprintf("  %s init                  - load default vals\n", cmd);
	ccprintf("  %s brightness NUM        - set intensity (0-ff)\n", cmd);
	ccprintf("  %s seq [NUM|SEQUENCE]    - run given pattern"
		 " (no arg for list)\n", cmd);
	ccprintf("  %s CTRL REG VAL          - set LED controller regs\n", cmd);
	ccprintf("  %s LED RED GREEN BLUE    - set color manually"
		 " (LED=4 for all)\n", cmd);
	return EC_SUCCESS;
}
#endif

static uint8_t find_msg_by_name(const char *str)
{
	uint8_t i;
	for (i = 0; i < LIGHTBAR_NUM_SEQUENCES; i++)
		if (!strcasecmp(str, lightbar_cmds[i].string))
			return i;

	return LIGHTBAR_NUM_SEQUENCES;
}

static void show_msg_names(void)
{
	int i;
	ccprintf("Sequences:");
	for (i = 0; i < LIGHTBAR_NUM_SEQUENCES; i++)
		ccprintf(" %s", lightbar_cmds[i].string);
	ccprintf("\nCurrent = 0x%x %s\n", current_state,
		 lightbar_cmds[current_state].string);
}

static int command_lightbar(int argc, char **argv)
{
	int i;
	uint8_t num;
	struct ec_params_lightbar_cmd params;

	if (1 == argc) {		/* no args = dump 'em all */
		do_cmd_dump(&params);
		for (i = 0; i < ARRAY_SIZE(dump_reglist); i++)
			ccprintf(" %02x     %02x     %02x\n",
				 params.out.dump.vals[i].reg,
				 params.out.dump.vals[i].ic0,
				 params.out.dump.vals[i].ic1);

		return EC_SUCCESS;
	}

	if (argc == 2 && !strcasecmp(argv[1], "init")) {
		lightbar_init_vals();
		return EC_SUCCESS;
	}

	if (argc == 2 && !strcasecmp(argv[1], "off")) {
		lightbar_off();
		return EC_SUCCESS;
	}

	if (argc == 2 && !strcasecmp(argv[1], "on")) {
		lightbar_on();
		return EC_SUCCESS;
	}

	if (argc == 3 && !strcasecmp(argv[1], "brightness")) {
		char *e;
		num = 0xff & strtoi(argv[2], &e, 16);
		lightbar_brightness(num);
		return EC_SUCCESS;
	}

	if (argc >= 2 && !strcasecmp(argv[1], "seq")) {
		char *e;
		uint8_t num;
		if (argc == 2) {
			show_msg_names();
			return 0;
		}
		num = 0xff & strtoi(argv[2], &e, 16);
		if (e && *e)
			num = find_msg_by_name(argv[2]);
		if (num >= LIGHTBAR_NUM_SEQUENCES)
			return EC_ERROR_INVAL;
		lightbar_sequence(num);
		return EC_SUCCESS;
	}

	if (argc == 4) {
		char *e;
		uint8_t ctrl, reg, val;
		ctrl = 0xff & strtoi(argv[1], &e, 16);
		reg = 0xff & strtoi(argv[2], &e, 16);
		val = 0xff & strtoi(argv[3], &e, 16);
		controller_write(ctrl, reg, val);
		return EC_SUCCESS;
	}

	if (argc == 5) {
		char *e;
		uint8_t led, r, g, b;
		led = strtoi(argv[1], &e, 16);
		r = strtoi(argv[2], &e, 16);
		g = strtoi(argv[3], &e, 16);
		b = strtoi(argv[4], &e, 16);
		do_cmd_rgb(led, r, g, b);
		return EC_SUCCESS;
	}

#ifdef CONSOLE_COMMAND_LIGHTBAR_HELP
	help(argv[0]);
#endif

	return EC_ERROR_INVAL;
}
DECLARE_CONSOLE_COMMAND(lightbar, command_lightbar);
