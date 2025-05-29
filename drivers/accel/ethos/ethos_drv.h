/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ETHOS_DRV_H__
#define __ETHOS_DRV_H__

#include <drm/gpu_scheduler.h>

#include "ethos_device.h"

struct ethos_file_priv {
	struct ethos_device *edev;

	struct drm_sched_entity sched_entity;
};

#endif
