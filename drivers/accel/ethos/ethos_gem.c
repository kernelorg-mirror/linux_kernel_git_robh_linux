// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2023 Collabora ltd. */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/slab.h>

#include <drm/ethos_accel.h>

#include "ethos_device.h"
#include "ethos_gem.h"

static void ethos_gem_free_object(struct drm_gem_object *obj)
{
	struct ethos_gem_object *bo = to_ethos_bo(obj);

	if (bo->info)
		kfree(bo->info);

	drm_gem_free_mmap_offset(&bo->base.base);
	drm_gem_dma_free(&bo->base);
}

static int ethos_gem_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct ethos_gem_object *bo = to_ethos_bo(obj);

	/* Don't allow mmap on objects that have the NO_MMAP flag set. */
	if (bo->flags & DRM_ETHOS_BO_NO_MMAP)
		return -EINVAL;

	return drm_gem_dma_object_mmap(obj, vma);
}

static const struct drm_gem_object_funcs ethos_gem_funcs = {
	.free = ethos_gem_free_object,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
//	.vunmap = drm_gem_dma_object_vunmap,
	.mmap = ethos_gem_mmap,
//	.status = ethos_gem_status,
	.export = drm_gem_prime_export,
	.vm_ops = &drm_gem_dma_vm_ops,
};

/**
 * ethos_gem_create_object - Implementation of driver->gem_create_object.
 * @ddev: DRM device
 * @size: Size in bytes of the memory the object will reference
 *
 * This lets the GEM helpers allocate object structs for us, and keep
 * our BO stats correct.
 */
struct drm_gem_object *ethos_gem_create_object(struct drm_device *ddev, size_t size)
{
	struct ethos_device *ptdev = container_of(ddev, struct ethos_device, base);
	struct ethos_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->base.base.funcs = &ethos_gem_funcs;
	obj->base.map_noncoherent = !ptdev->coherent;

	return &obj->base.base;
}

/**
 * ethos_gem_create_with_handle() - Create a GEM object and attach it to a handle.
 * @file: DRM file.
 * @ddev: DRM device.
 * @exclusive_vm: Exclusive VM. Not NULL if the GEM object can't be shared.
 * @size: Size of the GEM object to allocate.
 * @flags: Combination of drm_ethos_bo_flags flags.
 * @handle: Pointer holding the handle pointing to the new GEM object.
 *
 * Return: Zero on success
 */
int
ethos_gem_create_with_handle(struct drm_file *file,
			       struct drm_device *ddev,
			       u64 *size, u32 flags, u32 *handle)
{
	int ret;
	struct drm_gem_dma_object *mem;
	struct ethos_gem_object *bo;

	mem = drm_gem_dma_create(ddev, *size);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	bo = to_ethos_bo(&mem->base);
	bo->flags = flags;

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &mem->base, handle);
	if (!ret)
		*size = bo->base.base.size;

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mem->base);

	return ret;
}

struct dma_xfer {
	u64 len;
	u16 size0;
	u16 size1;
	s8 src_region;
	s8 dst_region;
	s8 mode;
	u64 src_offset;
	u64 dst_offset;
};

static int ethos_gem_cmdstream_validate(struct drm_device *ddev,
					struct ethos_gem_object *bo, u32 size)
{
	struct ethos_validated_cmdstream_info *info;
	u32 *cmds = bo->base.vaddr;
	struct dma_xfer dma = {};

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->cmd_size = size;

	for (int i = 0; i < size/4; i++) {
		u16 cmd = cmds[i];
		u16 param = cmds[i] >> 16;

		switch(cmd & 0xffff) {
		case 0x10: // NPU_OP_DMA_START
			u64 len = dma.len;
			if (dma.mode == 2)
				len *= dma.size1;
			if (dma.mode >= 1)
				len *= dma.size0;
			if ((dma.src_region >= 0) && (info->region_size[dma.src_region] < len + dma.src_offset))
				info->region_size[dma.src_region] = len + dma.src_offset;
			if (dma.dst_region >= 0) {
				info->output_region[dma.dst_region] = true;
				if (info->region_size[dma.dst_region] < len + dma.dst_offset)
					info->region_size[dma.dst_region] = len + dma.dst_offset;
			}
			dev_info(ddev->dev, "cmdstream: DMA SRC:%d:%llx DST:%d:%llx, len=%lld\n",
				 dma.src_region, dma.src_offset, dma.dst_region, dma.dst_offset, len);
			break;
		case 0x130: // NPU_SET_DMA0_SRC_REGION
			if (param & 0x100)
				dma.src_region = -1;
			else
				dma.src_region = param & 0xff;
			dma.mode = (cmd >> 25) & 0x3;
			break;
		case 0x131: // NPU_SET_DMA0_DST_REGION
			if (param & 0x100)
				dma.dst_region = -1;
			else
				dma.dst_region = param & 0xff;
			break;
		case 0x132: // NPU_SET_DMA0_SIZE0
			dma.size0 = param;
			break;
		case 0x133: // NPU_SET_DMA0_SIZE1
			dma.size1 = param;
			break;
		case 0x4030: // NPU_SET_DMA0_SRC
			dma.src_offset = ((u64)(param & 0xff) << 32) | cmds[i+1];
			break;
		case 0x4031: // NPU_SET_DMA0_DST
			dma.dst_offset = ((u64)(param & 0xff) << 32) | cmds[i+1];
			break;
		case 0x4032: // NPU_SET_DMA0_LEN
			dma.len = ((u64)(param & 0xff) << 32) | cmds[i+1];
			break;
		default:
			break;
		}

		if (cmd & 0x4000)
			i++;
	}

	bo->info = info;
	return 0;
}

/**
 * ethos_gem_cmdstream_create() - Create a GEM object and attach it to a handle.
 * @file: DRM file.
 * @ddev: DRM device.
 * @exclusive_vm: Exclusive VM. Not NULL if the GEM object can't be shared.
 * @size: Size of the GEM object to allocate.
 * @flags: Combination of drm_ethos_bo_flags flags.
 * @handle: Pointer holding the handle pointing to the new GEM object.
 *
 * Return: Zero on success
 */
int
ethos_gem_cmdstream_create(struct drm_file *file,
			       struct drm_device *ddev,
			       u32 *size, u64 data, u32 flags, u32 *handle)
{
	int ret;
	struct drm_gem_dma_object *mem;
	struct ethos_gem_object *bo;

	dev_info(ddev->dev, "creating cmd BO\n");
	mem = drm_gem_dma_create(ddev, *size);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	bo = to_ethos_bo(&mem->base);
	bo->flags = flags;
	dev_info(ddev->dev, "created cmd BO at %llx\n", (u64)bo->base.vaddr);

	if (copy_from_user(bo->base.vaddr,
			     (void __user *)(uintptr_t)data,
			     *size)) {
		ret = -EFAULT;
		goto fail;
	}

	ethos_gem_cmdstream_validate(ddev, bo, *size);

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &mem->base, handle);
	if (!ret)
		*size = bo->base.base.size;
	dev_info(ddev->dev, "created cmd BO handle %x\n", *handle);

fail:
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&mem->base);

	return ret;
}
