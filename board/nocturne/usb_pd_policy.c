/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "charge_manager.h"
#include "common.h"
#include "console.h"
#include "compile_time_macros.h"
#include "ec_commands.h"
#include "gpio.h"
#include "system.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usbc_ppc.h"
#include "util.h"

#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ## args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ## args)

int pd_check_vconn_swap(int port)
{
	/* Do not allow VCONN swap is 5V is off. */
	return gpio_get_level(GPIO_EN_5V);
}

__override void pd_execute_data_swap(int port, int data_role)
{
	/* Do nothing */
}

void pd_power_supply_reset(int port)
{
	/* Disable VBUS. */
	ppc_vbus_source_enable(port, 0);

#ifdef CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT
	/* Give back the current quota we are no longer using */
	charge_manager_source_port(port, 0);
#endif /* defined(CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT) */

	/* Notify host of power info change. */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);
}

int pd_set_power_supply_ready(int port)
{
	int rv;

	if (port >= ppc_cnt)
		return EC_ERROR_INVAL;

	/* Disable charging. */
	rv = ppc_vbus_sink_enable(port, 0);
	if (rv)
		return rv;

	/* Provide Vbus. */
	rv = ppc_vbus_source_enable(port, 1);
	if (rv)
		return rv;

#ifdef CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT
	/* Ensure we advertise the proper available current quota */
	charge_manager_source_port(port, 1);
#endif /* defined(CONFIG_USB_PD_MAX_SINGLE_SOURCE_CURRENT) */

	/* Notify host of power info change. */
	pd_send_host_event(PD_EVENT_POWER_CHANGE);

	return EC_SUCCESS;
}

/* ----------------- Vendor Defined Messages ------------------ */
__override void svdm_safe_dp_mode(int port)
{
	/* make DP interface safe until configure */
	usb_mux_set(port, TYPEC_MUX_NONE,
		USB_SWITCH_CONNECT, pd_get_polarity(port));
}
