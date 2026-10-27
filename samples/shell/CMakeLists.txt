# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rb9704_shell)
target_sources(app PRIVATE src/main.c src/rbtest.c)
target_sources_ifdef(CONFIG_WIFI app PRIVATE src/wifi.c)
# The RGB LED and button, on boards whose overlay gives a led-strip alias
# and an sw0 button (boards/stamp_c3.*).
target_sources_ifdef(CONFIG_LED_STRIP app PRIVATE src/indicator.c)
