/* SPDX-License-Identifier: GPL-2.0+ */
#include "generated/map-shift.h"
#include <stdint.h>
#include <urcu.h>
#ifndef U8_MAX
#define U8_MAX UINT8_MAX
#endif
#ifndef cpu_relax
#define cpu_relax caa_cpu_relax
#endif
#define CC_XA_CHUNK_SHIFT XA_CHUNK_SHIFT
#include "../../../../include/linux/cc_xarray.h"
