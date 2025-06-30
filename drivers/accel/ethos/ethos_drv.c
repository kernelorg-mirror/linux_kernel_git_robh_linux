// SPDX-License-Identifier: GPL-2.0-only or MIT
// Copyright (C) 2025 Arm, Ltd.

#include <linux/clk.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_utils.h>
#include <drm/drm_gem.h>
#include <drm/drm_accel.h>
#include <drm/ethos_accel.h>

#include "ethos_drv.h"
#include "ethos_device.h"
#include "ethos_gem.h"
#include "ethos_job.h"

static int ethos_ioctl_dev_query(struct drm_device *ddev, void *data, struct drm_file *file)
{
	struct ethos_device *ethosdev = container_of(ddev, struct ethos_device, base);
	struct drm_ethos_dev_query *args = data;

	if (!args->pointer) {
		switch (args->type) {
		case DRM_ETHOS_DEV_QUERY_NPU_INFO:
			args->size = sizeof(ethosdev->npu_info);
			return 0;
		default:
			return -EINVAL;
		}
	}

	switch (args->type) {
	case DRM_ETHOS_DEV_QUERY_NPU_INFO:
		if (args->size < offsetofend(struct drm_ethos_npu_info, sram_size))
			return -EINVAL;
		return copy_struct_to_user(u64_to_user_ptr(args->pointer),
					   args->size,
					   &ethosdev->npu_info,
					   sizeof(ethosdev->npu_info), NULL);
	default:
		return -EINVAL;
	}
}

#define ETHOS_BO_FLAGS		DRM_ETHOS_BO_NO_MMAP

static int ethos_ioctl_bo_create(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct drm_ethos_bo_create *args = data;
	int cookie, ret;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	if (!args->size || (args->flags & ~ETHOS_BO_FLAGS)) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	ret = ethos_gem_create_with_handle(file, ddev, &args->size,
					   args->flags, &args->handle);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}

static int ethos_ioctl_bo_wait(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct drm_ethos_bo_wait *args = data;
	int cookie, ret;
	unsigned long timeout = drm_timeout_abs_to_jiffies(args->timeout_ns);

	if (args->pad)
		return -EINVAL;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	ret = drm_gem_dma_resv_wait(file, args->handle, true, timeout);

	drm_dev_exit(cookie);
	return ret;
}

static int ethos_ioctl_bo_mmap_offset(struct drm_device *ddev, void *data,
					struct drm_file *file)
{
	struct drm_ethos_bo_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;

	args->offset = drm_vma_node_offset_addr(&obj->vma_node);

out:
	drm_gem_object_put(obj);
	return ret;
}

static int ethos_ioctl_cmdstream_bo_create(struct drm_device *ddev, void *data,
					   struct drm_file *file)
{
	struct drm_ethos_cmdstream_bo_create *args = data;
	int cookie, ret;

	if (!drm_dev_enter(ddev, &cookie))
		return -ENODEV;

	if (!args->size || !args->data || args->pad || args->flags) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	args->flags |= DRM_ETHOS_BO_NO_MMAP;

	ret = ethos_gem_cmdstream_create(file, ddev, &args->size, args->data,
					 args->flags, &args->handle);

out_dev_exit:
	drm_dev_exit(cookie);
	return ret;
}


static int
ethos_open(struct drm_device *ddev, struct drm_file *file)
{
	int ret = 0;
	struct ethos_file_priv *priv;

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_put_mod;
	}
	priv->edev = to_ethos_device(ddev);

	ret = ethos_job_open(priv);
	if (ret)
		goto err_free;

	file->driver_priv = priv;
	return 0;

err_free:
	kfree(priv);
err_put_mod:
	module_put(THIS_MODULE);
	return ret;
}

static void
ethos_postclose(struct drm_device *ddev, struct drm_file *file)
{
	ethos_job_close(file->driver_priv);
	kfree(file->driver_priv);
	module_put(THIS_MODULE);
}

static const struct drm_ioctl_desc ethos_drm_driver_ioctls[] = {
#define ETHOS_IOCTL(n, func, flags) \
	DRM_IOCTL_DEF_DRV(ETHOS_##n, ethos_ioctl_##func, flags)

	ETHOS_IOCTL(DEV_QUERY, dev_query, 0),
	ETHOS_IOCTL(BO_CREATE, bo_create, 0),
	ETHOS_IOCTL(BO_WAIT, bo_wait, 0),
	ETHOS_IOCTL(BO_MMAP_OFFSET, bo_mmap_offset, 0),
	ETHOS_IOCTL(CMDSTREAM_BO_CREATE, cmdstream_bo_create, 0),
	ETHOS_IOCTL(SUBMIT, submit, 0),
};

DEFINE_DRM_ACCEL_FOPS(ethos_drm_driver_fops);

/*
 * Ethos driver version:
 * - 1.0 - initial interface
 */
static const struct drm_driver ethos_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.open = ethos_open,
	.postclose = ethos_postclose,
	.ioctls = ethos_drm_driver_ioctls,
	.num_ioctls = ARRAY_SIZE(ethos_drm_driver_ioctls),
	.fops = &ethos_drm_driver_fops,
	.name = "ethos",
	.desc = "Arm Ethos Accel driver",
	.major = 1,
	.minor = 0,

	.gem_create_object = ethos_gem_create_object,
};

static int ethos_device_resume(struct device *dev)
{
	struct ethos_device *ethosdev = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(ethosdev->core_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ethosdev->apb_clk);
	if (ret)
		goto err_disable_core_clk;

	return 0;

err_disable_core_clk:
	clk_disable_unprepare(ethosdev->core_clk);
	return ret;
}

static int ethos_device_suspend(struct device *dev)
{
	struct ethos_device *ethosdev = dev_get_drvdata(dev);

	clk_disable_unprepare(ethosdev->apb_clk);
	clk_disable_unprepare(ethosdev->core_clk);
	return 0;
}

static int ethos_reset(struct ethos_device *ethosdev)
{
	int ret;
	u32 reg;

	writel_relaxed(0x3, ethosdev->regs + NPU_REG_RESET);

	ret = readl_poll_timeout(ethosdev->regs + NPU_REG_STATUS, reg, !(reg & 0x4),
				 USEC_PER_MSEC, USEC_PER_SEC);
	if (ret)
		return ret;

	if ((readl_relaxed(ethosdev->regs + NPU_REG_PROT) & 0x3) != 0x3) {
		dev_info(ethosdev->base.dev, "read PROT = %x\n", readl_relaxed(ethosdev->regs + NPU_REG_PROT));
//		return -EINVAL;
	}
	// TODO AXI port config, defaults might work

//	writel_relaxed(ethosdev->sramphys, ethosdev->regs + NPU_REGBASEP(1));	// SRAM

	// FIXME, aborts
	// memset(ethosdev->sram, 0, ethosdev->npu_info.sram_size);

	return 0;
}

static int ethos_sram_init(struct ethos_device *ethosdev)
{
	ethosdev->npu_info.sram_size = 0;

	ethosdev->srampool = of_gen_pool_get(ethosdev->base.dev->of_node, "sram", 0);
	if (!ethosdev->srampool) {
		dev_err(ethosdev->base.dev, "sram pool not found\n");
		return -EPROBE_DEFER;
	}
	ethosdev->npu_info.sram_size = gen_pool_size(ethosdev->srampool);

	ethosdev->sram = (void __iomem *)gen_pool_dma_alloc(ethosdev->srampool,
							    ethosdev->npu_info.sram_size,
							    &ethosdev->sramphys);
	if (!ethosdev->sram) {
		dev_err(ethosdev->base.dev, "failed to allocate from TX pool\n");
		return -ENOMEM;
	}

	return 0;
}

static int ethos_init(struct ethos_device *ethosdev)
{
	int ret;
	u32 id, config;

	ret = devm_pm_runtime_enable(ethosdev->base.dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(ethosdev->base.dev);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(ethosdev->base.dev, 50);
	pm_runtime_use_autosuspend(ethosdev->base.dev);

	/* If PM is disabled, we need to call ethos_device_resume() manually. */
	if (!IS_ENABLED(CONFIG_PM)) {
		ret = ethos_device_resume(ethosdev->base.dev);
		if (ret)
			return ret;
	}

	ethosdev->npu_info.id = id = readl_relaxed(ethosdev->regs + NPU_REG_ID);
	ethosdev->npu_info.config = config = readl_relaxed(ethosdev->regs + NPU_REG_CONFIG);

	dev_info(ethosdev->base.dev,
		"Found Ethos NPU, arch v%ld.%ld.%ld, rev r%ldp%ld, cmd stream ver%ld, %d MACs\n",
		FIELD_GET(ID_ARCH_MAJOR_MASK, id),
		FIELD_GET(ID_ARCH_MINOR_MASK, id),
		FIELD_GET(ID_ARCH_PATCH_MASK, id),
		FIELD_GET(ID_VER_MAJOR_MASK, id),
		FIELD_GET(ID_VER_MINOR_MASK, id),
		FIELD_GET(CONFIG_CMD_STREAM_VER_MASK, config),
		1 << FIELD_GET(CONFIG_MACS_PER_CC_MASK, config));

	ethos_sram_init(ethosdev);

	return ethos_reset(ethosdev);
}

static int ethos_probe(struct platform_device *pdev)
{
	int ret;
	struct ethos_device *ethosdev;

	ethosdev = devm_drm_dev_alloc(&pdev->dev, &ethos_drm_driver,
				      struct ethos_device, base);
	if (IS_ERR(ethosdev))
		return -ENOMEM;
	platform_set_drvdata(pdev, ethosdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	ethosdev->regs = devm_platform_ioremap_resource(pdev, 0);

	ethosdev->core_clk = devm_clk_get(&pdev->dev, "core");

	ethosdev->apb_clk = devm_clk_get_optional(&pdev->dev, "apb");

	ret = ethos_job_init(ethosdev);
	if (ret)
		return ret;

	ret = ethos_init(ethosdev);
	if (ret)
		return ret;

	ret = drm_dev_register(&ethosdev->base, 0);
	if (ret)
		pm_runtime_dont_use_autosuspend(ethosdev->base.dev);

	pm_runtime_put_autosuspend(ethosdev->base.dev);
	return ret;
}

static void ethos_remove(struct platform_device *pdev)
{
	struct ethos_device *ethosdev = dev_get_drvdata(&pdev->dev);

	drm_dev_unregister(&ethosdev->base);
	ethos_job_fini(ethosdev);
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "arm,ethos-u65" },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static DEFINE_RUNTIME_DEV_PM_OPS(ethos_pm_ops,
				 ethos_device_suspend,
				 ethos_device_resume,
				 NULL);

static struct platform_driver ethos_driver = {
	.probe = ethos_probe,
	.remove = ethos_remove,
	.driver = {
		.name = "ethos",
		.pm = pm_ptr(&ethos_pm_ops),
		.of_match_table = dt_match,
	},
};
module_platform_driver(ethos_driver);


MODULE_AUTHOR("Rob Herring <robh@kernel.org>");
MODULE_DESCRIPTION("Arm Ethos Accel Driver");
MODULE_LICENSE("Dual MIT/GPL");
