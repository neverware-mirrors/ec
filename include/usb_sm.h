/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* USB State Machine Framework */

#ifndef __CROS_EC_USB_SM_H
#define __CROS_EC_USB_SM_H

/* Function pointer that implements a portion of a usb state */
typedef void (*state_execution)(const int port);

/*
 * General usb state that can be used in multiple state machines.
 *
 * entry - Optional method that will be run when this state is entered
 * run   - Optional method that will be run repeatedly during state machine loop
 * exit  - Optional method that will be run when this state exists
 * parent- Optional parent usb_state that contains common entry/run/exit
 *	implementation between various usb state. All parent entry/run
 *	functions will before any child entry/run functions. All parent exit
 *	functions will run after any child exit functions.
 */
struct usb_state {
	const state_execution entry;
	const state_execution run;
	const state_execution exit;
	const struct usb_state *parent;
};

typedef const struct usb_state *usb_state_ptr;

/* Defines the current context of the usb statemachine. */
struct sm_ctx {
	usb_state_ptr current;
	usb_state_ptr previous;
	/* We use intptr_t type to accommodate host tests ptr size variance */
	intptr_t internal[2];
};

/* Local state machine states */
enum sm_local_state {
	SM_INIT = 0, /* Ensure static variables initialize to SM_INIT */
	SM_RUN,
	SM_PAUSED,
};

/**
 * Changes a state machines state. This handles exiting the previous state and
 * entering the target state. A common parent state will not exited nor be
 * re-entered.
 *
 * @param port      USB-C port number
 * @param ctx       State machine context
 * @param new_state State to transition to (NULL is valid and exits all states)
 */
void set_state(int port, struct sm_ctx *ctx, usb_state_ptr new_state);

/**
 * Runs one iteration of a state machine (including any parent states)
 *
 * @param port USB-C port number
 * @param ctx  State machine context
 */
void run_state(int port, struct sm_ctx *ctx);

#ifdef TEST_BUILD
/*
 * Struct for test builds that allow unit tests to easily iterate through
 * state machines
 */
struct test_sm_data {
	/* Base pointer of the state machine array */
	const usb_state_ptr base;
	/* Size fo the state machine array above */
	const int size;
	/* The array of names for states, can be NULL */
	const char * const * const names;
	/* The size of the above names array */
	const int names_size;
};
#endif

#endif /* __CROS_EC_USB_SM_H */