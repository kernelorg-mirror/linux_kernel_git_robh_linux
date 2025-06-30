// SPDX-License-Identifier: GPL-2.0-only or MIT
/* Copyright 2025 Arm, Ltd. */

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
	struct ethos_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->base.base.funcs = &ethos_gem_funcs;
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

struct dma {
	s8 region;
	u64 len;
	u64 offset;
	s64 stride[2];
};

struct buffer {
	u64 base;
	u32 length;
	s8 region;
};

struct feat_matrix {
	u64 base[4];
	s64 stride_x;
	s64 stride_y;
	s64 stride_c;
	s8 region;
	u8 broadcast;
	u16 stride_kernel;
	u16 precision;
	u16 depth;
	u16 width;
	u16 width0;
	u16 height[3];
	u8 pad_top;
	u8 pad_left;
	u8 pad_bottom;
	u8 pad_right;
};

struct cmd_state {
	struct {
		u16 size0;
		u16 size1;
		s8 mode;
		struct dma src;
		struct dma dst;
	} dma;
	struct buffer scale[2];
	struct buffer weight[4];
	struct feat_matrix ofm;
	struct feat_matrix ifm;
	struct feat_matrix ifm2;
};

static u64 cmd_to_addr(u32 *cmd)
{
	return ((u64)((cmd[0] & 0xff0000) << 16)) | cmd[1];
}

static u64 dma_length(struct ethos_validated_cmdstream_info *info,
		      s8 mode, u16 size0, u16 size1, struct dma *dma)
{
	u64 len = dma->len;
	if (mode >= 1) {
		len += dma->stride[0];
		len *= size0;
	}
	if (mode == 2) {
		len += dma->stride[1];
		len *= size1;
	}
	if (dma->region >= 0)
		info->region_size[dma->region] = max(info->region_size[dma->region],
						     len + dma->offset);

	return len;
}

static u64 feat_matrix_length(struct ethos_validated_cmdstream_info *info,
			      struct feat_matrix *fm,
			      u32 x, u32 y, u32 c)
{
	int storage = 0; // FIXME: support U85 3x1 tile
	int tile = 0;

	switch (storage) {
	case 0:
		if (x >= fm->width0 + 1) {
			x -= fm->width0 + 1;
			tile += 1;
		}
		if (y >= fm->height[tile] + 1) {
			y -= fm->height[tile] + 1;
			tile += 2;
		}
		break;
	case 1:
		if (y >= fm->height[1] + 1) {
			y -= fm->height[1] + 1;
			tile = 2;
		} else if (y >= fm->height[0] + 1) {
			y -= fm->height[0] + 1;
			tile = 1;
		}
		break;
	}
	u64 addr = fm->base[tile] + y * fm->stride_y;

	switch ((fm->precision >> 6) & 0x3) { // format
	case 0: //nhwc:
		addr += x * fm->stride_x + c;
		break;
	case 1: //nhcwb16:
		u32 element_size = BIT((fm->precision >> 1) & 0x3);
		addr += (c / 16) * fm->stride_c + (16 * x + (c & 0xf)) * element_size;
		break;
	}

	info->region_size[fm->region] = max(info->region_size[fm->region], addr + 1);

	return addr;
}

static void calc_sizes(struct drm_device *ddev,
		       struct ethos_validated_cmdstream_info *info,
		       u16 op, struct cmd_state *st,
		       bool ifm, bool ifm2, bool weight, bool scale)
{
	u64 len;
	char str[80];
	size_t slen = 0;

	if (ifm) {
		u32 stride_y = ((st->ifm.stride_kernel >> 8) & 0x2) + ((st->ifm.stride_kernel >> 1) & 0x1) + 1;
		u32 stride_x = ((st->ifm.stride_kernel >> 5) & 0x2) + (st->ifm.stride_kernel & 0x1) + 1;
		u32 ifm_height = st->ofm.height[2] * stride_y + st->ifm.height[2] - (st->ifm.pad_top + st->ifm.pad_bottom);
		u32 ifm_width  = st->ofm.width * stride_x + st->ifm.width - (st->ifm.pad_left + st->ifm.pad_right);

		len = feat_matrix_length(info, &st->ifm, ifm_width,
					 ifm_height, st->ifm.depth);
		slen += snprintf(str, 80, "IFM:%d:0x%llx-0x%llx ",
				 st->ifm.region, st->ifm.base[0], len);

	}

	if (ifm2) {
		len = feat_matrix_length(info, &st->ifm2, st->ifm.depth,
					 0, st->ofm.depth);
		slen += snprintf(str + slen, 80 - slen, "IFM2:%d:0x%llx-0x%llx ",
				 st->ifm2.region, st->ifm2.base[0], len);
	}

	if (weight) {
		info->region_size[st->weight[0].region] = max(info->region_size[st->weight[0].region],
								st->weight[0].base + st->weight[0].length);
		slen += snprintf(str + slen, 80 - slen, "W:%d:0x%llx-0x%llx ",
				 st->weight[0].region, st->weight[0].base,
				 st->weight[0].base + st->weight[0].length - 1);
	}

	if (scale) {
		info->region_size[st->scale[0].region] = max(info->region_size[st->scale[0].region],
								st->scale[0].base + st->scale[0].length);
		slen += snprintf(str + slen, 80 - slen, "S:%d:0x%llx-0x%llx ",
				 st->scale[0].region, st->scale[0].base,
				 st->scale[0].base + st->scale[0].length - 1);
	}

	len = feat_matrix_length(info, &st->ofm, st->ofm.width,
				 st->ofm.height[2], st->ofm.depth);
	info->output_region[st->ofm.region] = true;

	dev_info(ddev->dev, "cmd: OP:%d %sOFM:%d:0x%llx-0x%llx\n",
			op, str, st->ofm.region, st->ofm.base[0], len);
}

static int
ethos_gem_cmdstream_copy_and_validate(struct drm_device *ddev,
				      u32 __user *ucmds,
				      struct ethos_gem_object *bo, u32 size)
{
	struct ethos_validated_cmdstream_info *info;
	u32 *bocmds = bo->base.vaddr;
	struct cmd_state st = {};
	int i;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->cmd_size = size;

	for (i = 0; i < size/4; i++) {
		bool use_ifm, use_ifm2, use_scale;
		u32 cmds[2];
		u64 addr;

		if (get_user(cmds[0], ucmds++))
			goto fault;

		bocmds[i] = cmds[0];

		u16 cmd = cmds[0];
		u16 param = cmds[0] >> 16;

		if (cmd & 0x4000) {
			if (get_user(cmds[1], ucmds++))
				goto fault;
			i++;
			bocmds[i] = cmds[1];
			addr = cmd_to_addr(cmds);
		}

		switch(cmd) {
		case 0x10: // NPU_OP_DMA_START
			u64 srclen = dma_length(info, st.dma.mode, st.dma.size0, st.dma.size1, &st.dma.src);
			u64 dstlen = dma_length(info, st.dma.mode, st.dma.size0, st.dma.size1, &st.dma.dst);
			if (st.dma.dst.region >= 0)
				info->output_region[st.dma.dst.region] = true;
			dev_info(ddev->dev, "cmd: DMA SRC:%d:0x%llx+0x%llx DST:%d:0x%llx+0x%llx\n",
				 st.dma.src.region, st.dma.src.offset, srclen,
				 st.dma.dst.region, st.dma.dst.offset, dstlen);
			break;
		case 0x2: // NPU_OP_CONV
		case 0x3: // NPU_OP_DEPTHWISE
			use_ifm2 = param & 0x1;  // weights_ifm2
			use_scale = !(st.ofm.precision & 0x100);
			calc_sizes(ddev, info, cmd, &st, true, use_ifm2, !use_ifm2, use_scale);
			break;
		case 0x5: // NPU_OP_POOL
			use_ifm = param != 0x4;  // pooling mode
			use_scale = !(st.ofm.precision & 0x100);
			calc_sizes(ddev, info, cmd, &st, use_ifm, false, false, use_scale);
			break;
		case 0x6: // NPU_OP_ELEMENTWISE
			use_ifm2 = !((st.ifm2.broadcast == 8) || (param == 5) || (param == 6) || (param == 7) || (param == 0x24));
			use_ifm = st.ifm.broadcast != 8;
			calc_sizes(ddev, info, cmd, &st, use_ifm, use_ifm2, false, false);
			break;
		case 0x7: // NPU_OP_RESIZE (U85)
			WARN_ON(1); // TODO
			break;
		case 0x120: // NPU_SET_KERNEL_WIDTH_M1
			st.ifm.width = param;
			break;
		case 0x121: // NPU_SET_KERNEL_HEIGHT_M1
			st.ifm.height[2] = param;
			break;
		case 0x100: // NPU_SET_IFM_PAD_TOP
			st.ifm.pad_top = param & 0x7f;
			break;
		case 0x101: // NPU_SET_IFM_PAD_LEFT
			st.ifm.pad_left = param & 0x7f;
			break;
		case 0x102: // NPU_SET_IFM_PAD_RIGHT
			st.ifm.pad_right = param & 0xff;
			break;
		case 0x103: // NPU_SET_IFM_PAD_BOTTOM
			st.ifm.pad_bottom = param & 0xff;
			break;
		case 0x104: // NPU_SET_IFM_DEPTH_M1
			st.ifm.depth = param;
			break;
		case 0x105: // NPU_SET_IFM_PRECISION
			st.ifm.precision = param;
			break;
		case 0x108: // NPU_SET_IFM_BROADCAST
			st.ifm.broadcast = param;
			break;
		case 0x10f: // NPU_SET_IFM_REGION
			st.ifm.region = param & 0x7f;
			break;
		case 0x10a: // NPU_SET_IFM_WIDTH0_M1
			st.ifm.width0 = param;
			break;
		case 0x10b: // NPU_SET_IFM_HEIGHT0_M1
			st.ifm.height[0] = param;
			break;
		case 0x10c: // NPU_SET_IFM_HEIGHT1_M1
			st.ifm.height[1] = param;
			break;
		case 0x4000: // NPU_SET_IFM_BASE0
		case 0x4001: // NPU_SET_IFM_BASE1
		case 0x4002: // NPU_SET_IFM_BASE2
		case 0x4003: // NPU_SET_IFM_BASE3
			st.ifm.base[cmd & 0x3] = addr;
			break;
		case 0x4004: // NPU_SET_IFM_STRIDE_X
			st.ifm.stride_x = addr;
			break;
		case 0x4005: // NPU_SET_IFM_STRIDE_Y
			st.ifm.stride_y = addr;
			break;
		case 0x4006: // NPU_SET_IFM_STRIDE_C
			st.ifm.stride_c = addr;
			break;

		case 0x111: // NPU_SET_OFM_WIDTH_M1
			st.ofm.width = param;
			break;
		case 0x112: // NPU_SET_OFM_HEIGHT_M1
			st.ofm.height[2] = param;
			break;
		case 0x113: // NPU_SET_OFM_DEPTH_M1
			st.ofm.depth = param;
			break;
		case 0x114: // NPU_SET_OFM_PRECISION
			st.ofm.precision = param;
			break;
		case 0x11f: // NPU_SET_OFM_REGION
			st.ofm.region = param & 0x7;
			break;
		case 0x11a: // NPU_SET_OFM_WIDTH0_M1
			st.ofm.width0 = param;
			break;
		case 0x11b: // NPU_SET_OFM_HEIGHT0_M1
			st.ofm.height[0] = param;
			break;
		case 0x11c: // NPU_SET_OFM_HEIGHT1_M1
			st.ofm.height[1] = param;
			break;
		case 0x4010: // NPU_SET_OFM_BASE0
		case 0x4011: // NPU_SET_OFM_BASE1
		case 0x4012: // NPU_SET_OFM_BASE2
		case 0x4013: // NPU_SET_OFM_BASE3
			st.ofm.base[cmd & 0x3] = addr;
			break;
		case 0x4014: // NPU_SET_OFM_STRIDE_X
			st.ofm.stride_x = addr;
			break;
		case 0x4015: // NPU_SET_OFM_STRIDE_Y
			st.ofm.stride_y = addr;
			break;
		case 0x4016: // NPU_SET_OFM_STRIDE_C
			st.ofm.stride_c = addr;
			break;

		case 0x180: // NPU_SET_IFM2_BROADCAST
			st.ifm2.broadcast = param;
			break;
		case 0x185: // NPU_SET_IFM2_PRECISION
			st.ifm2.precision = param;
			break;
		case 0x18f: // NPU_SET_IFM2_REGION
			st.ifm2.region = param & 0x7;
			break;
		case 0x18a: // NPU_SET_IFM2_WIDTH0_M1
			st.ifm2.width0 = param;
			break;
		case 0x18b: // NPU_SET_IFM2_HEIGHT0_M1
			st.ifm2.height[0] = param;
			break;
		case 0x18c: // NPU_SET_IFM2_HEIGHT1_M1
			st.ifm2.height[1] = param;
			break;
		case 0x4080: // NPU_SET_IFM2_BASE0
		case 0x4081: // NPU_SET_IFM2_BASE1
		case 0x4082: // NPU_SET_IFM2_BASE2
		case 0x4083: // NPU_SET_IFM2_BASE3
			st.ifm2.base[cmd & 0x3] = addr;
			break;
		case 0x4084: // NPU_SET_IFM_STRIDE_X
			st.ifm2.stride_x = addr;
			break;
		case 0x4085: // NPU_SET_IFM_STRIDE_Y
			st.ifm2.stride_y = addr;
			break;
		case 0x4086: // NPU_SET_IFM2_STRIDE_C
			st.ifm2.stride_c = addr;
			break;

		case 0x128: // NPU_SET_WEIGHT_REGION
			st.weight[0].region = param & 0x7;
			break;
		case 0x129: // NPU_SET_SCALE_REGION
			st.scale[0].region = param & 0x7;
			break;
		case 0x4020: // NPU_SET_WEIGHT_BASE
			st.weight[0].base = addr;
			break;
		case 0x4021: // NPU_SET_WEIGHT_LENGTH
			st.weight[0].length = cmds[1];
			break;
		case 0x4022: // NPU_SET_SCALE_BASE
			st.scale[0].base = addr;
			break;
		case 0x4023: // NPU_SET_SCALE_LENGTH
			st.scale[0].length = cmds[1];
			break;
		case 0x4090: // NPU_SET_WEIGHT1_BASE
			st.weight[1].base = addr;
			break;
		case 0x4091: // NPU_SET_WEIGHT1_LENGTH
			st.weight[1].length = cmds[1];
			break;
		case 0x4092: // NPU_SET_SCALE1_BASE/NPU_SET_WEIGHT2_BASE
			if (0 /*U85*/)
				st.weight[2].base = addr;
			else
				st.scale[1].base = addr;
			break;
		case 0x4093: // NPU_SET_SCALE1_LENGTH/NPU_SET_WEIGHT2_LENGTH
			if (0 /*U85*/)
				st.weight[1].length = cmds[1];
			else
				st.scale[1].length = cmds[1];
			break;
		case 0x4094: // NPU_SET_WEIGHT3_BASE
			st.weight[3].base = addr;
			break;
		case 0x4095: // NPU_SET_WEIGHT3_LENGTH
			st.weight[3].length = cmds[1];
			break;

		case 0x130: // NPU_SET_DMA0_SRC_REGION
			if (param & 0x100)
				st.dma.src.region = -1;
			else
				st.dma.src.region = param & 0x7;
			st.dma.mode = (param >> 9) & 0x3;
			break;
		case 0x131: // NPU_SET_DMA0_DST_REGION
			if (param & 0x100)
				st.dma.dst.region = -1;
			else
				st.dma.dst.region = param & 0x7;
			break;
		case 0x132: // NPU_SET_DMA0_SIZE0
			st.dma.size0 = param;
			break;
		case 0x133: // NPU_SET_DMA0_SIZE1
			st.dma.size1 = param;
			break;
		case 0x4033: // NPU_SET_DMA0_SRC_STRIDE0
			st.dma.src.stride[0] = ((s64)addr << 24) >> 24;
			break;
		case 0x4034: // NPU_SET_DMA0_SRC_STRIDE1
			st.dma.src.stride[1] = ((s64)addr << 24) >> 24;
			break;
		case 0x4035: // NPU_SET_DMA0_DST_STRIDE0
			st.dma.dst.stride[0] = ((s64)addr << 24) >> 24;
			break;
		case 0x4036: // NPU_SET_DMA0_DST_STRIDE1
			st.dma.dst.stride[1] = ((s64)addr << 24) >> 24;
			break;
		case 0x4030: // NPU_SET_DMA0_SRC
			st.dma.src.offset = addr;
			break;
		case 0x4031: // NPU_SET_DMA0_DST
			st.dma.dst.offset = addr;
			break;
		case 0x4032: // NPU_SET_DMA0_LEN
			st.dma.src.len = st.dma.dst.len = addr;
			break;
		default:
			break;
		}

	}

	for (i = 0; i < NPU_BASEP_REGION_MAX; i++) {
		if (!info->region_size[i])
			continue;
		dev_info(ddev->dev, "region %d max size: 0x%llx\n", i, info->region_size[i]);
	}

	bo->info = info;
	return 0;

fault:
	kfree(info);
	return -EFAULT;
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

	ret = ethos_gem_cmdstream_copy_and_validate(ddev, (void __user *)(uintptr_t)data, bo, *size);
	if (ret)
		goto fail;

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
