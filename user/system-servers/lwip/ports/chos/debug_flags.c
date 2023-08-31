// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lwip/opt.h"

/*
 * debug_flags dynamically controls what LWIP_DEBUGF() prints.
 *
 * LWIP_DEBUGF(debug, message) prints if (debug & debug_flags) is true
 * because:
 *  lwipopts.h defines LWIP_DBG_TYPES_ON as debug_flags
 *  lwip/debug.h defines LWIP_DEBUGF() to test (debug & LWIP_DBG_TYPS)
 *
 * You should also check lwipopts.h to understand how 'debug's are defined.
 *
 * You can enable/disable the debug messages with these flags:
 *
 * LWIP_DBG_ON - enables all debug messages
 * LWIP_DBG_OFF - disables all debug messages
 *
 * You can also selectively enable the debug messages by types:
 *
 * LWIP_DBG_TRACE - enables if debug includes LWP_DBG_TRACE
 * LWIP_DBG_STATE - enables if debug includes LWP_DBG_STATE
 * LWIP_DBG_FRESH - enables if debug includes LWP_DBG_FRESH
 */

unsigned char lwip_debug_flags = LWIP_DBG_ON;
