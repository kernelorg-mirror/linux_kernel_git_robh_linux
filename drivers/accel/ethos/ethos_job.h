/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ETHOS_JOB_H__
#define __ETHOS_JOB_H__

#include <drm/drm_drv.h>
#include <drm/gpu_scheduler.h>

#include "ethos_device.h"

struct ethos_task {
	u64 cmds;
	u32 cmd_sz;
};

struct ethos_job {
	struct drm_sched_job base;

	struct ethos_device *dev;

	struct drm_gem_object **in_bos;
	struct drm_gem_object **out_bos;

	u32 in_bo_count;
	u32 out_bo_count;

	struct ethos_task *tasks;
	u32 task_count;
	u32 next_task_idx;

	/* Fence to be signaled by drm-sched once its done with the job */
	struct dma_fence *inference_done_fence;

	/* Fence to be signaled by IRQ handler when the job is complete. */
	struct dma_fence *done_fence;

	struct kref refcount;
};

int ethos_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);

int ethos_job_init(struct ethos_device *dev);
void ethos_job_fini(struct ethos_device *dev);
int ethos_job_open(struct ethos_file_priv *ethos_priv);
void ethos_job_close(struct ethos_file_priv *ethos_priv);
int ethos_job_is_idle(struct ethos_device *dev);

#endif
