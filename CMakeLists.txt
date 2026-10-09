# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
#
# rockblock9704-zephyr: Ground Control's RockBLOCK 9704 library built for
# Zephyr, with the serial port, the timing calls and one worker thread to own
# it. The library itself is not copied or forked: it is Ground Control's
# repository, pristine, pinned in west.yml at modules/lib/rockblock9704, and
# this file compiles the part of it that runs on a microcontroller.

if(CONFIG_RB9704)
  if(CONFIG_RB9704_UPSTREAM_DIR)
    set(RB9704_UP ${CONFIG_RB9704_UPSTREAM_DIR})
  elseif(DEFINED WEST_TOPDIR)
    set(RB9704_UP ${WEST_TOPDIR}/modules/lib/rockblock9704)
  else()
    set(RB9704_UP ${ZEPHYR_BASE}/../modules/lib/rockblock9704)
  endif()
  get_filename_component(RB9704_UP ${RB9704_UP} ABSOLUTE)
  if(NOT EXISTS ${RB9704_UP}/src/rockblock_9704.h)
    message(FATAL_ERROR
      "rockblock9704-zephyr: Ground Control's library is not at ${RB9704_UP}. "
      "Add rock7/RockBLOCK-9704 to your west.yml at modules/lib/rockblock9704 "
      "(this repository's west.yml has the pin) and run `west update`, or set "
      "CONFIG_RB9704_UPSTREAM_DIR.")
  endif()

  # The library's portable core. Left out: crossplatform.c (no Zephyr branch;
  # src/rb9704_shims.c instead), serial_presets/ (src/rb9704_serial.c is the
  # Zephyr preset), gpio.c (libgpiod), kermit (modem firmware update).
  set(RB9704_UP_SRCS
    ${RB9704_UP}/src/rockblock_9704.c
    ${RB9704_UP}/src/jspr.c
    ${RB9704_UP}/src/jspr_command.c
    ${RB9704_UP}/src/imt_queue.c
    ${RB9704_UP}/src/serial.c
    ${RB9704_UP}/src/third_party/cJSON/cJSON.c
    ${RB9704_UP}/src/third_party/base64/base64.c)

  zephyr_library_named(rb9704)
  zephyr_library_sources(
    src/rb9704_serial.c
    src/rb9704_shims.c
    src/rb9704_worker.c
    ${RB9704_UP_SRCS})
  zephyr_library_sources_ifdef(CONFIG_RB9704_SHELL src/rb9704_shell.c)
  zephyr_library_sources_ifdef(CONFIG_RB9704_POWER src/rb9704_power.c)
  zephyr_include_directories(include)
  zephyr_library_include_directories(${RB9704_UP}/src src)

  # Both the library and the worker see these: the message buffers' size,
  # CRC included, and the library's hook for a platform's serial port.
  math(EXPR RB9704_PAYLOAD "${CONFIG_RB9704_MESSAGE_MAX} + 2")
  zephyr_library_compile_definitions(
    IMT_PAYLOAD_SIZE=${RB9704_PAYLOAD}U
    SERIAL_CONTEXT_SETUP_FUNC=rb9704_serial_set_context)

  # The library's own files: the prototypes it calls without a declaration
  # in scope outside its presets, the POSIX declarations picolibc guards
  # behind feature macros, and -w because it is built pristine and its
  # warnings are Ground Control's to fix.
  set_source_files_properties(${RB9704_UP_SRCS} PROPERTIES COMPILE_OPTIONS
    "-include${CMAKE_CURRENT_LIST_DIR}/src/rb9704_glue.h;-D_POSIX_C_SOURCE=200809L;-D_DEFAULT_SOURCE;-w")
endif()
