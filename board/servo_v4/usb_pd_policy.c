/* Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "atomic.h"
#include "charge_manager.h"
#include "common.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
/* Just want the .h file for PS8742 definitions, not the large object file. */
#define CONFIG_USB_MUX_PS8742
#include "ps8740.h"
#undef CONFIG_USB_MUX_PS8742
#include "registers.h"
#include "system.h"
#include "task.h"
#include "tcpm.h"
#include "timer.h"
#include "util.h"
#include "usb_common.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_config.h"
#include "usb_pd_tcpm.h"

#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ## args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ## args)

#define DUT_PDO_FIXED_FLAGS (PDO_FIXED_DUAL_ROLE | PDO_FIXED_DATA_SWAP |\
			     PDO_FIXED_COMM_CAP)

#define CHG_PDO_FIXED_FLAGS (PDO_FIXED_DATA_SWAP)

#define VBUS_UNCHANGED(curr, pend, new) (curr == new && pend == new)

/* Macros to config the PD role */
#define CONF_SET_CLEAR(c, set, clear) ((c | (set)) & ~(clear))
#define CONF_SRC(c) CONF_SET_CLEAR(c, \
				CC_DISABLE_DTS | CC_ALLOW_SRC, \
				CC_ENABLE_DRP | CC_SNK_WITH_PD)
#define CONF_SNK(c) CONF_SET_CLEAR(c, \
				CC_DISABLE_DTS, \
				CC_ALLOW_SRC | CC_ENABLE_DRP | CC_SNK_WITH_PD)
#define CONF_PDSNK(c) CONF_SET_CLEAR(c, \
				CC_DISABLE_DTS | CC_SNK_WITH_PD, \
				CC_ALLOW_SRC | CC_ENABLE_DRP)
#define CONF_DRP(c) CONF_SET_CLEAR(c, \
				CC_DISABLE_DTS | CC_ALLOW_SRC | CC_ENABLE_DRP, \
				CC_SNK_WITH_PD)
#define CONF_SRCDTS(c) CONF_SET_CLEAR(c, \
				CC_ALLOW_SRC, \
				CC_ENABLE_DRP | CC_DISABLE_DTS | CC_SNK_WITH_PD)
#define CONF_SNKDTS(c) CONF_SET_CLEAR(c, \
				0, \
				CC_ALLOW_SRC | CC_ENABLE_DRP | \
				CC_DISABLE_DTS | CC_SNK_WITH_PD)
#define CONF_PDSNKDTS(c) CONF_SET_CLEAR(c, \
				CC_SNK_WITH_PD, \
				CC_ALLOW_SRC | CC_ENABLE_DRP | CC_DISABLE_DTS)
#define CONF_DRPDTS(c) CONF_SET_CLEAR(c, \
				CC_ALLOW_SRC | CC_ENABLE_DRP, \
				CC_DISABLE_DTS | CC_SNK_WITH_PD)

/* Macros to apply Rd/Rp to CC lines */
#define DUT_ACTIVE_CC_SET(r, flags) \
	gpio_set_flags(cc_config & CC_POLARITY ? \
				CONCAT2(GPIO_USB_DUT_CC2_, r) : \
				CONCAT2(GPIO_USB_DUT_CC1_, r), \
		       flags)
#define DUT_INACTIVE_CC_SET(r, flags) \
	gpio_set_flags(cc_config & CC_POLARITY ? \
				CONCAT2(GPIO_USB_DUT_CC1_, r) : \
				CONCAT2(GPIO_USB_DUT_CC2_, r), \
		       flags)
#define DUT_BOTH_CC_SET(r, flags) \
	do { \
		gpio_set_flags(CONCAT2(GPIO_USB_DUT_CC1_, r), flags); \
		gpio_set_flags(CONCAT2(GPIO_USB_DUT_CC2_, r), flags); \
	} while (0)

#define DUT_ACTIVE_CC_PU(r) DUT_ACTIVE_CC_SET(r, GPIO_OUT_HIGH)
#define DUT_INACTIVE_CC_PU(r) DUT_INACTIVE_CC_SET(r, GPIO_OUT_HIGH)
#define DUT_ACTIVE_CC_PD(r) DUT_ACTIVE_CC_SET(r, GPIO_OUT_LOW)
#define DUT_BOTH_CC_PD(r) DUT_BOTH_CC_SET(r, GPIO_OUT_LOW)
#define DUT_BOTH_CC_OPEN(r) DUT_BOTH_CC_SET(r, GPIO_INPUT)

/*
 * Dynamic PDO that reflects capabilities present on the CHG port. Allow for
 * multiple entries so that we can offer greater than 5V charging. The 1st
 * entry will be fixed 5V, but its current value may change based on the CHG
 * port vbus info. Subsequent entries are used for when offering vbus greater
 * than 5V.
 */
static const uint16_t pd_src_voltages_mv[] = {
		5000, 9000, 10000, 12000, 15000, 20000,
};
static uint32_t pd_src_chg_pdo[ARRAY_SIZE(pd_src_voltages_mv)];
static uint8_t chg_pdo_cnt;

const uint32_t pd_snk_pdo[] = {
		PDO_FIXED(5000, 500, CHG_PDO_FIXED_FLAGS),
		PDO_BATT(4750, 21000, 15000),
		PDO_VAR(4750, 21000, 3000),
};
const int pd_snk_pdo_cnt = ARRAY_SIZE(pd_snk_pdo);

struct vbus_prop {
	int mv;
	int ma;
};
static struct vbus_prop vbus[CONFIG_USB_PD_PORT_MAX_COUNT];
static int active_charge_port = CHARGE_PORT_NONE;
static enum charge_supplier active_charge_supplier;
static uint8_t vbus_rp = TYPEC_RP_RESERVED;

static int cc_config = CC_ALLOW_SRC;

/* Voltage thresholds for no connect in DTS mode */
static int pd_src_vnc_dts[TYPEC_RP_RESERVED][2] = {
	{PD_SRC_3_0_VNC_MV, PD_SRC_1_5_VNC_MV},
	{PD_SRC_1_5_VNC_MV, PD_SRC_DEF_VNC_MV},
	{PD_SRC_3_0_VNC_MV, PD_SRC_DEF_VNC_MV},
};
/* Voltage thresholds for Ra attach in DTS mode */
static int pd_src_rd_threshold_dts[TYPEC_RP_RESERVED][2] = {
	{PD_SRC_3_0_RD_THRESH_MV, PD_SRC_1_5_RD_THRESH_MV},
	{PD_SRC_1_5_RD_THRESH_MV, PD_SRC_DEF_RD_THRESH_MV},
	{PD_SRC_3_0_RD_THRESH_MV, PD_SRC_DEF_RD_THRESH_MV},
};
/* Voltage thresholds for no connect in normal SRC mode */
static int pd_src_vnc[TYPEC_RP_RESERVED] = {
	PD_SRC_DEF_VNC_MV,
	PD_SRC_1_5_VNC_MV,
	PD_SRC_3_0_VNC_MV,
};
/* Voltage thresholds for Ra attach in normal SRC mode */
static int pd_src_rd_threshold[TYPEC_RP_RESERVED] = {
	PD_SRC_DEF_RD_THRESH_MV,
	PD_SRC_1_5_RD_THRESH_MV,
	PD_SRC_3_0_RD_THRESH_MV,
};

/* Saved value for the duration of faking PD disconnect */
static int fake_pd_disconnect_duration_us;

/* Shadow what would be in TCPC register state. */
static int rp_value_stored = TYPEC_RP_USB;
static int cc_pull_stored = TYPEC_CC_RD;

/*
 * Set the USB PD max voltage to value appropriate for the board version.
 * The red/blue versions of servo_v4 have an ESD between VBUS and CC1/CC2
 * that has a breakdown voltage of 11V.
 */
#define MAX_MV_RED_BLUE 9000

static int user_limited_max_mv = 20000;

static uint8_t allow_pr_swap = 1;
static uint8_t allow_dr_swap = 1;

static uint32_t max_supported_voltage(void)
{
	int board_max_mv = board_get_version() >= BOARD_VERSION_BLACK ?
		PD_MAX_VOLTAGE_MV : MAX_MV_RED_BLUE;

	return board_max_mv < user_limited_max_mv ? board_max_mv :
						    user_limited_max_mv;
}

static int charge_port_is_active(void)
{
	return active_charge_port == CHG && vbus[CHG].mv > 0;
}

static int is_charge_through_allowed(void)
{
	return charge_port_is_active() && cc_config & CC_ALLOW_SRC;
}

static int get_dual_role_of_src(void)
{
	return cc_config & CC_ENABLE_DRP ? PD_DRP_TOGGLE_ON :
					   PD_DRP_FORCE_SOURCE;
}

static void dut_allow_charge(void)
{
	/*
	 * Update to charge enable if charger still present and not
	 * already charging.
	 */
	if (is_charge_through_allowed() &&
	    pd_get_dual_role(DUT) != PD_DRP_FORCE_SOURCE &&
	    pd_get_dual_role(DUT) != PD_DRP_TOGGLE_ON) {
		CPRINTS("Enable DUT charge through");
		pd_set_dual_role(DUT, get_dual_role_of_src());
		/*
		 * If DRP role, don't set any CC pull resistor, the PD
		 * state machine will toggle and set the pull resistors
		 * when needed.
		 */
		if (!(cc_config & CC_ENABLE_DRP))
			pd_set_host_mode(DUT, 1);

		/*
		 * Enable PD comm. The PD comm may be disabled during
		 * the power charge-through was detached.
		 */
		pd_comm_enable(DUT, 1);

		pd_update_contract(DUT);
	}
}
DECLARE_DEFERRED(dut_allow_charge);

static void board_manage_dut_port(void)
{
	enum pd_dual_role_states allowed_role;
	enum pd_dual_role_states current_role;

	/*
	 * This function is called by the CHG port whenever there has been a
	 * change in its vbus voltage or current. That change may necessitate
	 * that the DUT port present a different Rp value or renogiate its PD
	 * contract if it is connected.
	 */

	/* Assume the default value of Rd */
	allowed_role = PD_DRP_FORCE_SINK;

	/* If VBUS charge through is available, mark as such. */
	if (is_charge_through_allowed())
		allowed_role = get_dual_role_of_src();

	current_role = pd_get_dual_role(DUT);
	if (current_role != allowed_role) {
		/* Update role. */
		if (allowed_role == PD_DRP_FORCE_SINK) {
			/* We've lost charge through. Disable VBUS. */
			gpio_set_level(GPIO_DUT_CHG_EN, 0);

			/* Mark as SNK only. */
			pd_set_dual_role(DUT, PD_DRP_FORCE_SINK);
			pd_set_host_mode(DUT, 0);

			/*
			 * Disable PD comm. It matches the user expectation that
			 * unplugging the power charge-through makes servo v4 as
			 * a passive hub, without any PD support.
			 *
			 * There is an exception that servo v4 is explicitly set
			 * to have PD, like the "pnsnk" mode.
			 */
			pd_comm_enable(DUT, cc_config & CC_SNK_WITH_PD ? 1 : 0);
		} else {
			/* Allow charge through after PD negotiate. */
			hook_call_deferred(&dut_allow_charge_data, 2000 * MSEC);
		}
	}

	/*
	 * Update PD contract to reflect new available CHG
	 * voltage/current values.
	 */
	pd_update_contract(DUT);
}

static void update_ports(void)
{
	int pdo_index, src_index, snk_index, i;
	uint32_t pdo, max_ma, max_mv;

	/*
	 * CHG Vbus has changed states, update PDO that reflects CHG port
	 * state
	 */
	if (!charge_port_is_active()) {
		/* CHG Vbus has dropped, so become SNK. */
		chg_pdo_cnt = 0;
	} else {
		/* Advertise the 'best' PDOs at various discrete voltages */
		if (active_charge_supplier == CHARGE_SUPPLIER_PD) {
			src_index = 0;
			snk_index = -1;

			for (i = 0; i < ARRAY_SIZE(pd_src_voltages_mv); ++i) {
				/* Adhere to board voltage limits */
				if (pd_src_voltages_mv[i] >
				    max_supported_voltage())
					break;

				/* Find the 'best' PDO <= voltage */
				pdo_index =
				pd_find_pdo_index(pd_get_src_cap_cnt(CHG),
					pd_get_src_caps(CHG),
					pd_src_voltages_mv[i], &pdo);
				/* Don't duplicate PDOs */
				if (pdo_index == snk_index)
					continue;
				/* Skip battery / variable PDOs */
				if ((pdo & PDO_TYPE_MASK) != PDO_TYPE_FIXED)
					continue;

				snk_index = pdo_index;
				pd_extract_pdo_power(pdo, &max_ma, &max_mv);
				pd_src_chg_pdo[src_index++] =
					PDO_FIXED_VOLT(max_mv) |
					PDO_FIXED_CURR(max_ma) |
					DUT_PDO_FIXED_FLAGS |
					PDO_FIXED_UNCONSTRAINED;
			}
			chg_pdo_cnt = src_index;
		} else {
			/* 5V PDO */
			pd_src_chg_pdo[0] = PDO_FIXED_VOLT(PD_MIN_MV) |
				PDO_FIXED_CURR(vbus[CHG].ma) |
				DUT_PDO_FIXED_FLAGS |
				PDO_FIXED_UNCONSTRAINED;

			chg_pdo_cnt = 1;
		}
	}

	/* Call DUT port manager to update Rp and possible PD contract */
	board_manage_dut_port();
}

int board_set_active_charge_port(int charge_port)
{
	if (charge_port == DUT)
		return -1;

	active_charge_port = charge_port;
	update_ports();

	if (!charge_port_is_active())
		/* Don't negotiate > 5V, except in lockstep with DUT */
		pd_set_external_voltage_limit(CHG, PD_MIN_MV);

	return 0;
}

void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv)
{
	if (port != CHG)
		return;

	active_charge_supplier = supplier;

	/* Update the voltage/current values for CHG port */
	vbus[CHG].ma = charge_ma;
	vbus[CHG].mv = charge_mv;
	update_ports();
}

__override uint8_t board_get_src_dts_polarity(int port)
{
	/*
	 * When servo configured as srcdts, the CC polarity is based
	 * on the flags.
	 */
	if (port == DUT)
		return !!(cc_config & CC_POLARITY);

	return 0;
}

int pd_tcpc_cc_nc(int port, int cc_volt, int cc_sel)
{
	int rp_index;
	int nc;

	/* Can never be called from CHG port as it's sink only */
	if (port == CHG)
		return 0;

	rp_index = vbus_rp;
	/*
	 * If rp_index > 2, then always return not connected. This case should
	 * only happen when all Rp GPIO controls are tri-stated.
	 */
	if (rp_index >= TYPEC_RP_RESERVED)
		return 1;

	/* Select the correct voltage threshold for current Rp and DTS mode */
	if (cc_config & CC_DISABLE_DTS)
		nc = cc_volt >= pd_src_vnc[rp_index];
	else
		nc = cc_volt >= pd_src_vnc_dts[rp_index][
				cc_config & CC_POLARITY ? !cc_sel : cc_sel];

	return nc;
}

int pd_tcpc_cc_ra(int port, int cc_volt, int cc_sel)
{
	int rp_index;
	int ra;

	/* Can never be called from CHG port as it's sink only */
	if (port == CHG)
		return 0;

	rp_index = vbus_rp;
	/*
	 * If rp_index > 2, then can't be Ra. This case should
	 * only happen when all Rp GPIO controls are tri-stated.
	 */
	if (rp_index >= TYPEC_RP_RESERVED)
		return 0;

	/* Select the correct voltage threshold for current Rp and DTS mode */
	if (cc_config & CC_DISABLE_DTS)
		ra = cc_volt < pd_src_rd_threshold[rp_index];
	else
		ra = cc_volt < pd_src_rd_threshold_dts[rp_index][
				cc_config & CC_POLARITY ? !cc_sel : cc_sel];

	return ra;
}

int pd_adc_read(int port, int cc)
{
	int mv;

	if (port == 0)
		mv = adc_read_channel(cc ? ADC_CHG_CC2_PD : ADC_CHG_CC1_PD);
	else if (!(cc_config & CC_DETACH)) {
		/*
		 * In servo v4 hardware logic, both CC lines are wired directly
		 * to DUT. When servo v4 as a snk, DUT may source Vconn to CC2
		 * (CC1 if polarity flip) and make the voltage high as vRd-3.0,
		 * which makes the PD state mess up. As the PD state machine
		 * doesn't handle this case. It assumes that CC2 (CC1 if
		 * polarity flip) is separated by a Type-C cable, resulting a
		 * voltage lower than the max of vRa.
		 *
		 * It fakes the voltage within vRa.
		 */
		if ((cc_config & CC_DISABLE_DTS) &&
		    cc_pull_stored == TYPEC_CC_RD && port == DUT &&
		    cc == (cc_config & CC_POLARITY ? 0 : 1))
			mv = 0;
		else
			mv = adc_read_channel(cc ? ADC_DUT_CC2_PD :
						   ADC_DUT_CC1_PD);
	} else {
		/*
		 * When emulating detach, fake the voltage on CC to 0 to avoid
		 * triggering some debounce logic.
		 *
		 * The servo v4 makes Rd/Rp open but the DUT may present Rd/Rp
		 * alternatively that makes the voltage on CC falls into some
		 * unexpected range and triggers the PD state machine switching
		 * between SNK_DISCONNECTED and SNK_DISCONNECTED_DEBOUNCE.
		 */
		mv = 0;
	}

	return mv;
}

static int board_set_rp(int rp)
{
	if (cc_config & CC_DISABLE_DTS) {
		/*
		 * DTS mode is disabled, so only present the requested Rp value
		 * on CC1 (active) and leave all Rp/Rd resistors on CC2
		 * (inactive) disconnected.
		 */
		switch (rp) {
		case TYPEC_RP_USB:
			DUT_ACTIVE_CC_PU(RPUSB);
			break;
		case TYPEC_RP_1A5:
			DUT_ACTIVE_CC_PU(RP1A5);
			break;
		case TYPEC_RP_3A0:
			DUT_ACTIVE_CC_PU(RP3A0);
			break;
		case TYPEC_RP_RESERVED:
			/*
			 * This case can be used to force a detach event since
			 * all values are set to inputs above. Nothing else to
			 * set.
			 */
			break;
		default:
			return EC_ERROR_INVAL;
		}
	} else {
		/* DTS mode is enabled. The rp parameter is used to select the
		 * Type C current limit to advertise. The combinations of Rp on
		 * each CC line is shown in the table below.
		 *
		 * CC values for Debug sources (DTS)
		 *
		 * Source type  Mode of Operation   CC1    CC2
		 * ---------------------------------------------
		 * DTS          Default USB Power   Rp3A0  Rp1A5
		 * DTS          USB-C @ 1.5 A       Rp1A5  RpUSB
		 * DTS          USB-C @ 3 A         Rp3A0  RpUSB
		 */
		switch (rp) {
		case TYPEC_RP_USB:
			DUT_ACTIVE_CC_PU(RP3A0);
			DUT_INACTIVE_CC_PU(RP1A5);
			break;
		case TYPEC_RP_1A5:
			DUT_ACTIVE_CC_PU(RP1A5);
			DUT_INACTIVE_CC_PU(RPUSB);
			break;
		case TYPEC_RP_3A0:
			DUT_ACTIVE_CC_PU(RP3A0);
			DUT_INACTIVE_CC_PU(RPUSB);
			break;
		case TYPEC_RP_RESERVED:
			/*
			 * This case can be used to force a detach event since
			 * all values are set to inputs above. Nothing else to
			 * set.
			 */
			break;
		default:
			return EC_ERROR_INVAL;
		}
	}
	/* Save new Rp value for DUT port */
	vbus_rp = rp;

	return EC_SUCCESS;
}

int pd_set_rp_rd(int port, int cc_pull, int rp_value)
{
	int rv = EC_SUCCESS;

	if (port != 1)
		return EC_ERROR_UNIMPLEMENTED;

	/* CC is disabled for emulating detach. Don't change Rd/Rp. */
	if (cc_config & CC_DETACH)
		return EC_SUCCESS;

	/* By default disconnect all Rp/Rd resistors from both CC lines */
	/* Set Rd for CC1/CC2 to High-Z. */
	DUT_BOTH_CC_OPEN(RD);
	/* Set Rp for CC1/CC2 to High-Z. */
	DUT_BOTH_CC_OPEN(RP3A0);
	DUT_BOTH_CC_OPEN(RP1A5);
	DUT_BOTH_CC_OPEN(RPUSB);
	/* Set TX Hi-Z */
	DUT_BOTH_CC_OPEN(TX_DATA);

	if (cc_pull == TYPEC_CC_RP) {
		rv = board_set_rp(rp_value);
	} else if (cc_pull == TYPEC_CC_RD) {
		/*
		 * The DUT port uses a captive cable. It can present Rd on both
		 * CC1 and CC2. If DTS mode is enabled, then present Rd on both
		 * CC lines. However, if DTS mode is disabled only present Rd on
		 * CC1 (active).
		 */
		if (cc_config & CC_DISABLE_DTS)
			DUT_ACTIVE_CC_PD(RD);
		else
			DUT_BOTH_CC_PD(RD);

	}

	rp_value_stored = rp_value;
	cc_pull_stored = cc_pull;

	return rv;
}

int board_select_rp_value(int port, int rp)
{
	if (port != 1)
		return EC_ERROR_UNIMPLEMENTED;

	/*
	 * Update Rp value to indicate non-pd power available.
	 * Do not change pull direction though.
	 */
	if ((rp != rp_value_stored) && (cc_pull_stored == TYPEC_CC_RP)) {
		rp_value_stored = rp;
		return pd_set_rp_rd(port, TYPEC_CC_RP, rp);
	}

	return EC_SUCCESS;
}

int charge_manager_get_source_pdo(const uint32_t **src_pdo, const int port)
{
	int pdo_cnt = 0;

	/*
	 * If CHG is providing VBUS, then advertise what's available on the CHG
	 * port, otherwise we provide no power.
	 */
	if (charge_port_is_active()) {
		*src_pdo =  pd_src_chg_pdo;
		pdo_cnt = chg_pdo_cnt;
	}

	return pdo_cnt;
}

__override void pd_transition_voltage(int idx)
{
	timestamp_t deadline;
	uint32_t ma, mv;

	pd_extract_pdo_power(pd_src_chg_pdo[idx - 1], &ma, &mv);
	/* Is this a transition to a new voltage? */
	if (charge_port_is_active() && vbus[CHG].mv != mv) {
		/*
		 * Alter voltage limit on charge port, this should cause
		 * the port to select the desired PDO.
		 */
		pd_set_external_voltage_limit(CHG, mv);

		/* Wait for CHG transition */
		deadline.val = get_time().val + PD_T_PS_TRANSITION;
		CPRINTS("Waiting for CHG port transition");
		while (charge_port_is_active() &&
		       vbus[CHG].mv != mv &&
		       get_time().val < deadline.val)
			msleep(10);

		if (vbus[CHG].mv != mv) {
			CPRINTS("Missed CHG transition, resetting DUT");
			pd_power_supply_reset(DUT);
			return;
		}

		CPRINTS("CHG transitioned");
	}

	vbus[DUT].mv = vbus[CHG].mv;
	vbus[DUT].ma = vbus[CHG].ma;
}

int pd_set_power_supply_ready(int port)
{
	/* Port 0 can never provide vbus. */
	if (port == CHG)
		return EC_ERROR_INVAL;

	if (charge_port_is_active()) {
		/* Enable VBUS */
		gpio_set_level(GPIO_DUT_CHG_EN, 1);

		if (vbus[CHG].mv != PD_MIN_MV)
			CPRINTS("ERROR, CHG port voltage %d != PD_MIN_MV",
				vbus[CHG].mv);

		vbus[DUT].mv = vbus[CHG].mv;
		vbus[DUT].ma = vbus[CHG].mv;
		pd_set_dual_role(DUT, get_dual_role_of_src());
	} else {
		vbus[DUT].mv = 0;
		vbus[DUT].ma = 0;
		gpio_set_level(GPIO_DUT_CHG_EN, 0);
		pd_set_dual_role(DUT, PD_DRP_FORCE_SINK);
		return EC_ERROR_NOT_POWERED;
	}

	return EC_SUCCESS; /* we are ready */
}

void pd_power_supply_reset(int port)
{
	/* Port 0 can never provide vbus. */
	if (port == CHG)
		return;

	/* Disable VBUS */
	gpio_set_level(GPIO_DUT_CHG_EN, 0);

	/* DUT is lost, back to 5V limit on CHG */
	pd_set_external_voltage_limit(CHG, PD_MIN_MV);
}

int pd_snk_is_vbus_provided(int port)
{

	return gpio_get_level(port ? GPIO_USB_DET_PP_DUT :
				     GPIO_USB_DET_PP_CHG);
}

__override int pd_check_power_swap(int port)
{
	/*
	 * When only host VBUS is available, then servo_v4 is not setting
	 * PDO_FIXED_UNCONSTRAINED in the src_pdo sent to the DUT. When this bit
	 * is not set, the DUT will always attempt to swap its power role to
	 * SRC. Let servo_v4 have more control over its power role by always
	 * rejecting power swap requests from the DUT.
	 */

	/* Port 0 can never provide vbus. */
	if (port == CHG)
		return 0;

	if (pd_get_power_role(port) == PD_ROLE_SINK && !(cc_config & CC_ALLOW_SRC))
		return 0;

	if (pd_snk_is_vbus_provided(CHG))
		return allow_pr_swap;

	return 0;
}

__override int pd_check_data_swap(int port,
				  enum pd_data_role data_role)
{
	/*
	 * Servo should allow data role swaps to let DUT see the USB hub, but
	 * doing it on CHG port is a waste as its data lines is unconnected.
	 */
	if (port == CHG)
		return 0;

	return allow_dr_swap;
}

__override void pd_execute_data_swap(int port,
				     enum pd_data_role data_role)
{
	/*
	 * TODO(b/137887386): Turn on the fastboot/DFU path when data swap to
	 * DFP?
	 */
}

__override void pd_check_pr_role(int port,
				 enum pd_power_role pr_role,
				 int flags)
{
	/*
	 * Don't define any policy to initiate power role swap.
	 *
	 * CHG port is SNK only. DUT port requires a user to switch its
	 * role by commands. So don't do anything implicitly.
	 */
}

__override void pd_check_dr_role(int port,
				 enum pd_data_role dr_role,
				 int flags)
{
	if (port == CHG)
		return;

	/* If DFP, try to switch to UFP, to let DUT see the USB hub. */
	if ((flags & PD_FLAGS_PARTNER_DR_DATA) && dr_role == PD_ROLE_DFP)
		pd_request_data_swap(port);
}


/* ----------------- Vendor Defined Messages ------------------ */
/*
 * DP alt-mode config, user configurable.
 * Default is the mode disabled, supporting the C and D pin assignment,
 * multi-function preferred, and a plug.
 */
static int alt_dp_config = (ALT_DP_PIN_C | ALT_DP_PIN_D | ALT_DP_MF_PREF |
			    ALT_DP_PLUG);

/**
 * Get the pins based on the user config.
 */
static int alt_dp_config_pins(void)
{
	int pins = 0;

	if (alt_dp_config & ALT_DP_PIN_C)
		pins |= MODE_DP_PIN_C;
	if (alt_dp_config & ALT_DP_PIN_D)
		pins |= MODE_DP_PIN_D;
	if (alt_dp_config & ALT_DP_PIN_E)
		pins |= MODE_DP_PIN_E;
	return pins;
}

/**
 * Get the cable outlet value (plug or receptacle) based on the user config.
 */
static int alt_dp_config_cable(void)
{
	return (alt_dp_config & ALT_DP_PLUG) ? CABLE_PLUG : CABLE_RECEPTACLE;
}

const uint32_t vdo_idh = VDO_IDH(0, /* data caps as USB host */
				 1, /* data caps as USB device */
				 IDH_PTYPE_AMA, /* Alternate mode */
				 1, /* supports alt modes */
				 USB_VID_GOOGLE);

const uint32_t vdo_product = VDO_PRODUCT(CONFIG_USB_PID, CONFIG_USB_BCD_DEV);

const uint32_t vdo_ama = VDO_AMA(CONFIG_USB_PD_IDENTITY_HW_VERS,
				 CONFIG_USB_PD_IDENTITY_SW_VERS,
				 0, 0, 0, 0, /* SS[TR][12] */
				 0, /* Vconn power */
				 0, /* Vconn power required */
				 0, /* Vbus power required */
				 AMA_USBSS_U31_GEN1 /* USB SS support */);

static int svdm_response_identity(int port, uint32_t *payload)
{
	int dp_supported = (alt_dp_config & ALT_DP_ENABLE) != 0;

	if (dp_supported) {
		payload[VDO_I(IDH)] = vdo_idh;
		payload[VDO_I(CSTAT)] = VDO_CSTAT(0);
		payload[VDO_I(PRODUCT)] = vdo_product;
		payload[VDO_I(AMA)] = vdo_ama;
		return VDO_I(AMA) + 1;
	} else {
		return 0;
	}
}

static int svdm_response_svids(int port, uint32_t *payload)
{
	payload[1] = VDO_SVID(USB_SID_DISPLAYPORT, 0);
	return 2;
}

#define MODE_CNT 1
#define OPOS 1

/*
 * The Type-C demux PS8742 supports pin assignment C, D, and E. Response the DP
 * capabilities with supporting all of them.
 */
uint32_t vdo_dp_mode[MODE_CNT];

static int svdm_response_modes(int port, uint32_t *payload)
{
	vdo_dp_mode[0] =
		VDO_MODE_DP(0,             /* UFP pin cfg supported: none */
			    alt_dp_config_pins(),  /* DFP pin */
			    1,             /* no usb2.0 signalling in AMode */
			    alt_dp_config_cable(), /* plug or receptacle */
			    MODE_DP_V13,   /* DPv1.3 Support, no Gen2 */
			    MODE_DP_SNK);  /* Its a sink only */

	/* CCD uses the SBU lines; don't enable DP when dts-mode enabled */
	if (!(cc_config & CC_DISABLE_DTS))
		return 0; /* NAK */

	if (PD_VDO_VID(payload[0]) != USB_SID_DISPLAYPORT)
		return 0; /* NAK */

	memcpy(payload + 1, vdo_dp_mode, sizeof(vdo_dp_mode));
	return MODE_CNT + 1;
}

static int is_typec_dp_muxed(void)
{
	int value;

	i2c_read8(I2C_PORT_MASTER, PS8740_I2C_ADDR0_FLAG, PS8740_REG_MODE,
		  &value);
	return value & PS8740_MODE_DP_ENABLED ? 1 : 0;
}

static void set_typec_mux(int pin_cfg)
{
	int value;

	switch (pin_cfg) {
	case 0:
		value = 0;
		CPRINTS("PinCfg:off");
		break;
	case MODE_DP_PIN_C:
		value = PS8740_MODE_DP_ENABLED;
		CPRINTS("PinCfg:C");
		break;
	case MODE_DP_PIN_D:
		value = PS8740_MODE_DP_ENABLED | PS8740_MODE_USB_ENABLED;
		CPRINTS("PinCfg:D");
		break;
	case MODE_DP_PIN_E:
		value = PS8740_MODE_DP_ENABLED | PS8740_MODE_CE_DP_ENABLED;
		CPRINTS("PinCfg:E");
		break;
	default:
		CPRINTS("PinCfg not supported: %d", pin_cfg);
		return;
	}
	if (value && cc_config & CC_POLARITY)
		value |= PS8740_MODE_POLARITY_INVERTED;
	i2c_write8(I2C_PORT_MASTER, PS8740_I2C_ADDR0_FLAG, PS8740_REG_MODE,
		   value);
}

static int get_hpd_level(void)
{
	if (alt_dp_config & ALT_DP_OVERRIDE_HPD)
		return (alt_dp_config & ALT_DP_HPD_LVL) != 0;
	else
		return gpio_get_level(GPIO_DP_HPD);
}

static int dp_status(int port, uint32_t *payload)
{
	int opos = PD_VDO_OPOS(payload[0]);
	int hpd = get_hpd_level();

	if (opos != OPOS)
		return 0;  /* NAK */

	payload[1] = VDO_DP_STATUS(
		0,                /* IRQ_HPD */
		hpd,              /* HPD_HI|LOW */
		0,                /* request exit DP */
		0,                /* request exit USB */
		(alt_dp_config & ALT_DP_MF_PREF) != 0,  /* MF pref */
		is_typec_dp_muxed(),
		0,                /* power low */
		hpd ? 0x2 : 0);
	return 2;
}

static int dp_config(int port, uint32_t *payload)
{
	if (PD_DP_CFG_DPON(payload[1]))
		set_typec_mux(PD_DP_CFG_PIN(payload[1]));

	return 1;
}

/* Whether alternate mode has been entered or not */
static int alt_mode;

static int svdm_enter_mode(int port, uint32_t *payload)
{
	/* SID & mode request is valid */
	if ((PD_VDO_VID(payload[0]) != USB_SID_DISPLAYPORT) ||
	    (PD_VDO_OPOS(payload[0]) != OPOS))
		return 0;  /* NAK */

	alt_mode = OPOS;
	return 1;
}

int pd_alt_mode(int port, enum tcpm_transmit_type type, uint16_t svid)
{
	if (type == TCPC_TX_SOP && svid == USB_SID_DISPLAYPORT)
		return alt_mode;

	return 0;
}

static int svdm_exit_mode(int port, uint32_t *payload)
{
	if (PD_VDO_VID(payload[0]) == USB_SID_DISPLAYPORT)
		set_typec_mux(0);

	alt_mode = 0;

	return 1; /* Must return ACK */
}

static struct amode_fx dp_fx = {
	.status = &dp_status,
	.config = &dp_config,
};

const struct svdm_response svdm_rsp = {
	.identity = &svdm_response_identity,
	.svids = &svdm_response_svids,
	.modes = &svdm_response_modes,
	.enter_mode = &svdm_enter_mode,
	.amode = &dp_fx,
	.exit_mode = &svdm_exit_mode,
};

__override int pd_custom_vdm(int port, int cnt, uint32_t *payload,
		  uint32_t **rpayload)
{
	int cmd = PD_VDO_CMD(payload[0]);

	/* make sure we have some payload */
	if (cnt == 0)
		return 0;

	switch (cmd) {
	case VDO_CMD_VERSION:
		/* guarantee last byte of payload is null character */
		*(payload + cnt - 1) = 0;
		CPRINTF("ver: %s\n", (char *)(payload+1));
		break;
	case VDO_CMD_CURRENT:
		CPRINTF("Current: %dmA\n", payload[1]);
		break;
	}

	return 0;
}

__override const struct svdm_amode_fx supported_modes[] = {};
__override const int supported_modes_cnt = ARRAY_SIZE(supported_modes);

static void print_cc_mode(void)
{
	/* Get current CCD status */
	ccprintf("cc: %s\n", cc_config & CC_DETACH ? "off" : "on");
	ccprintf("dts mode: %s\n", cc_config & CC_DISABLE_DTS ? "off" : "on");
	ccprintf("chg mode: %s\n",
		 gpio_get_level(GPIO_DUT_CHG_EN) ? "on" : "off");
	ccprintf("chg allowed: %s\n", cc_config & CC_ALLOW_SRC ? "on" : "off");
	ccprintf("drp enabled: %s\n", cc_config & CC_ENABLE_DRP ? "on" : "off");
	ccprintf("cc polarity: %s\n", cc_config & CC_POLARITY ? "cc2" :
								"cc1");
	ccprintf("pd enabled: %s\n", pd_comm_is_enabled(DUT) ? "on" : "off");
}


static void do_cc(int cc_config_new)
{
	int chargeable;
	int dualrole;

	if (cc_config_new != cc_config) {
		if (!(cc_config & CC_DETACH)) {
			/* Force detach */
			pd_power_supply_reset(DUT);
			/* Always set to 0 here so both CC lines are changed */
			cc_config &= ~(CC_DISABLE_DTS & CC_ALLOW_SRC);

			/* Remove Rp/Rd on both CC lines */
			pd_comm_enable(DUT, 0);
			pd_set_rp_rd(DUT, TYPEC_CC_RP, TYPEC_RP_RESERVED);

			/*
			 * If just changing mode (cc keeps enabled), give some
			 * time for DUT to detach, use tErrorRecovery.
			 */
			if (!(cc_config_new & CC_DETACH))
				usleep(PD_T_ERROR_RECOVERY);
		}

		if ((cc_config & ~cc_config_new) & CC_DISABLE_DTS) {
			/* DTS-disabled -> DTS-enabled */
			ccd_enable(1);
			ext_hpd_detection_enable(0);
		} else if ((cc_config_new & ~cc_config) & CC_DISABLE_DTS) {
			/* DTS-enabled -> DTS-disabled */
			ccd_enable(0);
			if (!(alt_dp_config & ALT_DP_OVERRIDE_HPD))
				ext_hpd_detection_enable(1);
		}

		/* Accept new cc_config value */
		cc_config = cc_config_new;

		if (!(cc_config & CC_DETACH)) {
			/* Can we source? */
			chargeable = is_charge_through_allowed();
			dualrole = chargeable ? get_dual_role_of_src() :
						PD_DRP_FORCE_SINK;
			pd_set_dual_role(DUT, dualrole);
			/*
			 * If force_source or force_sink role, explicitly set
			 * the Rp or Rd resistors on CC lines.
			 *
			 * If DRP role, don't set any CC pull resistor, the PD
			 * state machine will toggle and set the pull resistors
			 * when needed.
			 */
			if (dualrole != PD_DRP_TOGGLE_ON)
				pd_set_host_mode(DUT, chargeable);

			/*
			 * For the normal lab use, emulating a sink has no PD
			 * comm, like a passive hub. For the PD FAFT use, we
			 * need to validate some PD behavior, so a flag
			 * CC_SNK_WITH_PD to force enabling PD comm.
			 */
			if (cc_config & CC_SNK_WITH_PD)
				pd_comm_enable(DUT, 1);
			else
				pd_comm_enable(DUT, chargeable);
		}
	}
}

static int command_cc(int argc, char **argv)
{
	int cc_config_new = cc_config;

	if (argc < 2) {
		print_cc_mode();
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "off")) {
		cc_config_new |= CC_DETACH;
	} else if (!strcasecmp(argv[1], "on")) {
		cc_config_new &= ~CC_DETACH;
	} else {
		cc_config_new &= ~CC_DETACH;
		if (!strcasecmp(argv[1], "src"))
			cc_config_new = CONF_SRC(cc_config_new);
		else if (!strcasecmp(argv[1], "snk"))
			cc_config_new = CONF_SNK(cc_config_new);
		else if (!strcasecmp(argv[1], "pdsnk"))
			cc_config_new = CONF_PDSNK(cc_config_new);
		else if (!strcasecmp(argv[1], "drp"))
			cc_config_new = CONF_DRP(cc_config_new);
		else if (!strcasecmp(argv[1], "srcdts"))
			cc_config_new = CONF_SRCDTS(cc_config_new);
		else if (!strcasecmp(argv[1], "snkdts"))
			cc_config_new = CONF_SNKDTS(cc_config_new);
		else if (!strcasecmp(argv[1], "pdsnkdts"))
			cc_config_new = CONF_PDSNKDTS(cc_config_new);
		else if (!strcasecmp(argv[1], "drpdts"))
			cc_config_new = CONF_DRPDTS(cc_config_new);
		else
			return EC_ERROR_PARAM2;
	}

	if (!strcasecmp(argv[2], "cc1"))
		cc_config_new &= ~CC_POLARITY;
	else if (!strcasecmp(argv[2], "cc2"))
		cc_config_new |= CC_POLARITY;
	else if (argc >= 3)
		return EC_ERROR_PARAM3;

	do_cc(cc_config_new);
	print_cc_mode();

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(cc, command_cc,
			"[off|on|src|snk|pdsnk|drp|srcdts|snkdts|pdsnkdts|"
			"drpdts] [cc1|cc2]",
			"Servo_v4 DTS and CHG mode");

static void fake_disconnect_end(void)
{
	/* Reenable CC lines with previous dts and src modes */
	do_cc(cc_config & ~CC_DETACH);
}
DECLARE_DEFERRED(fake_disconnect_end);

static void fake_disconnect_start(void)
{
	/* Disable CC lines */
	do_cc(cc_config | CC_DETACH);

	hook_call_deferred(&fake_disconnect_end_data,
			   fake_pd_disconnect_duration_us);
}
DECLARE_DEFERRED(fake_disconnect_start);

static int cmd_fake_disconnect(int argc, char *argv[])
{
	int delay_ms, duration_ms;
	char *e;

	if (argc < 3)
		return EC_ERROR_PARAM_COUNT;

	delay_ms = strtoi(argv[1], &e, 0);
	if (*e || delay_ms < 0)
		return EC_ERROR_PARAM1;
	duration_ms = strtoi(argv[2], &e, 0);
	if (*e || duration_ms < 0)
		return EC_ERROR_PARAM2;

	/* Cancel any pending function calls */
	hook_call_deferred(&fake_disconnect_start_data, -1);
	hook_call_deferred(&fake_disconnect_end_data, -1);

	fake_pd_disconnect_duration_us = duration_ms * MSEC;
	hook_call_deferred(&fake_disconnect_start_data, delay_ms * MSEC);

	ccprintf("Fake disconnect for %d ms starting in %d ms.\n",
		duration_ms, delay_ms);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(fakedisconnect, cmd_fake_disconnect,
			"<delay_ms> <duration_ms>", NULL);

static int cmd_ada_srccaps(int argc, char *argv[])
{
	int i;
	const uint32_t * const ada_srccaps = pd_get_src_caps(CHG);

	for (i = 0; i < pd_get_src_cap_cnt(CHG); ++i) {
		uint32_t max_ma, max_mv;

		pd_extract_pdo_power(ada_srccaps[i], &max_ma, &max_mv);
		ccprintf("%d: %dmV/%dmA\n", i, max_mv, max_ma);
	}

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(ada_srccaps, cmd_ada_srccaps,
			"",
			"Print adapter SrcCap");

static void chg_pd_disconnect(void)
{
	/* Clear charger PDO on CHG port disconnected. */
	if (pd_is_disconnected(CHG))
		pd_set_src_caps(CHG, 0, NULL);
}
DECLARE_HOOK(HOOK_USB_PD_DISCONNECT, chg_pd_disconnect, HOOK_PRIO_DEFAULT);

static int cmd_dp_action(int argc, char *argv[])
{
	int i;
	char *e;

	if (argc < 1)
		return EC_ERROR_PARAM_COUNT;

	if (argc == 1) {
		CPRINTS("DP alt-mode: %s",
			(alt_dp_config & ALT_DP_ENABLE) ? "enable" : "disable");
	}

	if (!strcasecmp(argv[1], "enable")) {
		alt_dp_config |= ALT_DP_ENABLE;
	} else if (!strcasecmp(argv[1], "disable")) {
		alt_dp_config &= ~ALT_DP_ENABLE;
	} else if (!strcasecmp(argv[1], "pins")) {
		if (argc >= 3) {
			alt_dp_config &= ~(ALT_DP_PIN_C | ALT_DP_PIN_D |
					   ALT_DP_PIN_E);
			for (i = 0; i < 3; i++) {
				if (!argv[2][i])
					break;

				switch (argv[2][i]) {
				case 'c':
				case 'C':
					alt_dp_config |= ALT_DP_PIN_C;
					break;
				case 'd':
				case 'D':
					alt_dp_config |= ALT_DP_PIN_D;
					break;
				case 'e':
				case 'E':
					alt_dp_config |= ALT_DP_PIN_E;
					break;
				}
			}
		}
		CPRINTS("Pins: %s%s%s",
			(alt_dp_config & ALT_DP_PIN_C) ? "C" : "",
			(alt_dp_config & ALT_DP_PIN_D) ? "D" : "",
			(alt_dp_config & ALT_DP_PIN_E) ? "E" : "");
	} else if (!strcasecmp(argv[1], "mf")) {
		if (argc >= 3) {
			i = strtoi(argv[2], &e, 10);
			if (*e)
				return EC_ERROR_PARAM3;
			if (i)
				alt_dp_config |= ALT_DP_MF_PREF;
			else
				alt_dp_config &= ~ALT_DP_MF_PREF;
		}
		CPRINTS("MF pref: %d", (alt_dp_config & ALT_DP_MF_PREF) != 0);
	} else if (!strcasecmp(argv[1], "plug")) {
		if (argc >= 3) {
			i = strtoi(argv[2], &e, 10);
			if (*e)
				return EC_ERROR_PARAM3;
			if (i)
				alt_dp_config |= ALT_DP_PLUG;
			else
				alt_dp_config &= ~ALT_DP_PLUG;
		}
		CPRINTS("Plug or receptacle: %d",
			(alt_dp_config & ALT_DP_PLUG) != 0);
	} else if (!strcasecmp(argv[1], "hpd")) {
		if (argc >= 3) {
			if (!strncasecmp(argv[2], "ext", 3)) {
				alt_dp_config &= ~ALT_DP_OVERRIDE_HPD;
				ext_hpd_detection_enable(1);
			} else if (!strncasecmp(argv[2], "h", 1)) {
				alt_dp_config |= ALT_DP_OVERRIDE_HPD;
				alt_dp_config |= ALT_DP_HPD_LVL;
				/*
				 * Modify the HPD to high. Need to enable the
				 * external HPD signal monitoring. A monitor
				 * may send a IRQ at any time to notify DUT.
				 */
				ext_hpd_detection_enable(1);
				pd_send_hpd(DUT, hpd_high);
			} else if (!strncasecmp(argv[2], "l", 1)) {
				alt_dp_config |= ALT_DP_OVERRIDE_HPD;
				alt_dp_config &= ~ALT_DP_HPD_LVL;
				ext_hpd_detection_enable(0);
				pd_send_hpd(DUT, hpd_low);
			} else if (!strcasecmp(argv[2], "irq")) {
				pd_send_hpd(DUT, hpd_irq);
			}
		}
		CPRINTS("HPD source: %s",
			(alt_dp_config & ALT_DP_OVERRIDE_HPD) ? "overridden"
							      : "external");
		CPRINTS("HPD level: %d", get_hpd_level());
	}  else if (!strcasecmp(argv[1], "help")) {
		CPRINTS("Usage: usbc_action dp [enable|disable|hpd|mf|pins|"
			"plug]");
	}

	return EC_SUCCESS;
}

static int cmd_usbc_action(int argc, char *argv[])
{
	if (argc >= 2 && !strcasecmp(argv[1], "dp"))
		return cmd_dp_action(argc - 1, &argv[1]);

	if (argc != 2 && argc != 3)
		return EC_ERROR_PARAM_COUNT;

	/* TODO(b:140256624): drop *v command if we migrate to chg cmd. */
	if (!strcasecmp(argv[1], "5v")) {
		do_cc(CONF_SRC(cc_config));
		user_limited_max_mv = 5000;
		update_ports();
	} else if (!strcasecmp(argv[1], "12v")) {
		do_cc(CONF_SRC(cc_config));
		user_limited_max_mv = 12000;
		update_ports();
	} else if (!strcasecmp(argv[1], "20v")) {
		do_cc(CONF_SRC(cc_config));
		user_limited_max_mv = 20000;
		update_ports();
	} else if (!strcasecmp(argv[1], "dev")) {
		/* Set the limit back to original */
		user_limited_max_mv = 20000;
		do_cc(CONF_PDSNK(cc_config));
	} else if (!strcasecmp(argv[1], "pol0")) {
		do_cc(cc_config & ~CC_POLARITY);
	} else if (!strcasecmp(argv[1], "pol1")) {
		do_cc(cc_config | CC_POLARITY);
	} else if (!strcasecmp(argv[1], "drp")) {
		/* Toggle the DRP state, compatible with Plankton. */
		do_cc(cc_config ^ CC_ENABLE_DRP);
		CPRINTF("DRP = %d, host_mode = %d\n",
			!!(cc_config & CC_ENABLE_DRP),
			!!(cc_config & CC_ALLOW_SRC));
	} else if (!strcasecmp(argv[1], "chg")) {
		int sink_v;

		if (argc != 3)
			return EC_ERROR_PARAM2;

		sink_v = atoi(argv[2]);
		if (!sink_v)
			return EC_ERROR_PARAM2;

		user_limited_max_mv = sink_v * 1000;
		do_cc(CONF_SRC(cc_config));
		update_ports();
		/*
		 * TODO(b:140256624): servod captures 'chg SRC' keyword to
		 * recognize if this command is supported in the firmware.
		 * Drop this message if when we phase out the usbc_role control.
		 */
		ccprintf("CHG SRC %dmV\n", user_limited_max_mv);
	} else if (!strcasecmp(argv[1], "drswap")) {
		if (argc == 2) {
			CPRINTF("allow_dr_swap = %d\n", allow_dr_swap);
			return EC_SUCCESS;
		}

		if (argc != 3)
			return EC_ERROR_PARAM2;

		allow_dr_swap = !!atoi(argv[2]);

	} else if (!strcasecmp(argv[1], "prswap")) {
		if (argc == 2) {
			CPRINTF("allow_pr_swap = %d\n", allow_pr_swap);
			return EC_SUCCESS;
		}

		if (argc != 3)
			return EC_ERROR_PARAM2;

		allow_pr_swap = !!atoi(argv[2]);
	} else {
		return EC_ERROR_PARAM1;
	}

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(usbc_action, cmd_usbc_action,
			"5v|12v|20v|dev|pol0|pol1|drp|dp|chg x(x=voltage)|"
			"drswap [1|0]|prswap [1|0]",
			"Set Servo v4 type-C port state");
