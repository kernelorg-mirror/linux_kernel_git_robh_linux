/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */

#ifndef __ETHOS_DEVICE_H__
#define __ETHOS_DEVICE_H__

#include <drm/drm_device.h>
#include <drm/gpu_scheduler.h>

#include <drm/ethos_accel.h>

struct clk;
struct gen_pool;

#define NPU_REG_ID		0x0000
#define NPU_REG_STATUS		0x0004
#define NPU_REG_CMD		0x0008
#define NPU_REG_RESET		0x000C
#define NPU_REG_QBASE		0x0010
#define NPU_REG_QBASE_HI	0x0014
#define NPU_REG_QREAD		0x0018
#define NPU_REG_QCONFIG		0x001C
#define NPU_REG_QSIZE		0x0020
#define NPU_REG_PROT		0x0024
#define NPU_REG_CONFIG		0x0028
#define NPU_REG_REGIONCFG	0x003C

#define NPU_REG_BASEP(x)	(0x0080 + (x)*8)
#define NPU_REG_BASEP_HI(x)	(0x0084 + (x)*8)
#define NPU_BASEP_REGION_MAX	8

#define ID_ARCH_MAJOR_MASK	GENMASK(31, 28)
#define ID_ARCH_MINOR_MASK	GENMASK(27, 20)
#define ID_ARCH_PATCH_MASK	GENMASK(19, 16)
#define ID_VER_MAJOR_MASK	GENMASK(11, 8)
#define ID_VER_MINOR_MASK	GENMASK(7, 4)

#define CONFIG_MACS_PER_CC_MASK	GENMASK(3, 0)
#define CONFIG_CMD_STREAM_VER_MASK	GENMASK(7, 4)

#define STATUS_IRQ_RAISED	BIT(1)
#define STATUS_BUS_STATUS	BIT(2)
#define STATUS_CMD_PARSE_ERR	BIT(4)
#define STATUS_CMD_END_REACHED	BIT(5)

#define CMD_CLEAR_IRQ		BIT(1)
/**
 * struct ethos_device - Ethos device
 */
struct ethos_device {
	/** @base: Base drm_device. */
	struct drm_device base;

	/** @iomem: CPU mapping of the registers. */
	void __iomem *regs;

	void __iomem *sram;
	struct gen_pool *srampool;
	dma_addr_t sramphys;

	struct clk *core_clk;
	struct clk *apb_clk;

	int irq;

	bool coherent;

	struct drm_ethos_npu_info npu_info;

	struct ethos_job *in_flight_job;

	spinlock_t job_lock;

	struct {
		struct workqueue_struct *wq;
		struct work_struct work;
		atomic_t pending;
	} reset;

	struct drm_gpu_scheduler sched;
	struct mutex sched_lock;
	u64 fence_context;
	u64 emit_seqno;
};

#define to_ethos_device(drm_dev) \
	((struct ethos_device *)container_of(drm_dev, struct ethos_device, base))


#endif
