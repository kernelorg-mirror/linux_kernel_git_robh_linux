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
