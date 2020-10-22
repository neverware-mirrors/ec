# Copyright 2020 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Note: this cmake system implements only a zephyr module, and is not
# intended to build a complete EC.  To build projects in platform/ec,
# you should continue to use the Makefile system.
#
# Googlers can find the design doc at go/zephyr-shim.

if(NOT DEFINED ZEPHYR_CURRENT_MODULE_DIR)
  message(FATAL_ERROR "This Cmake system implements only a Zephyr module, and
          should not be invoked directly.  Please continue to use the Makefile
          system for non-Zephyr builds.")
endif()

set(PLATFORM_EC "${ZEPHYR_CURRENT_MODULE_DIR}" CACHE PATH
    "Path to the platform/ec repo.")
# Zephyr 2.3 will set ZEPHYR_CURRENT_MODULE_DIR to the directory of the
# CMakeLists.txt file, whereas 2.4 will set it to the actual module
# directory.  Try to detect the condition by searching for
# zephyr/module.yml.
if(NOT EXISTS "${PLATFORM_EC}/zephyr/module.yml")
  set(PLATFORM_EC "${PLATFORM_EC}/..")
  assert_exists("${PLATFORM_EC}/zephyr/module.yml")
endif()

zephyr_include_directories_ifdef(
  CONFIG_PLATFORM_EC
  "${PLATFORM_EC}/zephyr/shim/include"
  "${PLATFORM_EC}/fuzz"
  "${PLATFORM_EC}/test"
  "${PLATFORM_EC}/include")

add_subdirectory_ifdef(CONFIG_PLATFORM_EC "shim/src")

# Shimmed modules
zephyr_sources_ifdef(CONFIG_PLATFORM_EC "${PLATFORM_EC}/common/base32.c")
zephyr_sources_ifdef(CONFIG_PLATFORM_EC_TIMER "${PLATFORM_EC}/common/timer.c")
zephyr_sources_ifdef(CONFIG_SHELL "${PLATFORM_EC}/common/gpio_commands.c")