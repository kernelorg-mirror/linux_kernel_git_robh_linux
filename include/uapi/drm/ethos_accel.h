/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2025 Arm, Ltd. */
#ifndef _ETHOS_DRM_H_
#define _ETHOS_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: IOCTL IDs
 *
 * enum drm_ethos_ioctl_id - IOCTL IDs
 *
 * Place new ioctls at the end, don't re-order, don't replace or remove entries.
 *
 * These IDs are not meant to be used directly. Use the DRM_IOCTL_ETHOS_xxx
 * definitions instead.
 */
enum drm_ethos_ioctl_id {
	/** @DRM_ETHOS_DEV_QUERY: Query device information. */
	DRM_ETHOS_DEV_QUERY = 0,

	/** @DRM_ETHOS_BO_CREATE: Create a buffer object. */
	DRM_ETHOS_BO_CREATE,

	/**
	 * @DRM_ETHOS_BO_MMAP_OFFSET: Get the file offset to pass to
	 * mmap to map a GEM object.
	 */
	DRM_ETHOS_BO_MMAP_OFFSET,
};

/**
 * DOC: IOCTL arguments
 */

/**
 * enum drm_ethos_dev_query_type - Query type
 *
 * Place new types at the end, don't re-order, don't remove or replace.
 */
enum drm_ethos_dev_query_type {
	/** @DRM_ETHOS_DEV_QUERY_NPU_INFO: Query NPU information. */
	DRM_ETHOS_DEV_QUERY_NPU_INFO = 0,
};

/**
 * struct drm_ethos_gpu_info - NPU information
 *
 * Structure grouping all queryable information relating to the NPU.
 */
struct drm_ethos_npu_info {
	/** @id : NPU ID. */
	__u32 id;
#define DRM_ETHOS_ARCH_MAJOR(x)			((x) >> 28)
#define DRM_ETHOS_ARCH_MINOR(x)			(((x) >> 20) & 0xff)
#define DRM_ETHOS_ARCH_PATCH(x)			(((x) >> 16) & 0xf)
#define DRM_ETHOS_PRODUCT_MAJOR(x)		(((x) >> 12) & 0xf)
#define DRM_ETHOS_VERSION_MAJOR(x)		(((x) >> 8) & 0xf)
#define DRM_ETHOS_VERSION_MINOR(x)		(((x) >> 4) & 0xff)
#define DRM_ETHOS_VERSION_STATUS(x)		((x) & 0xf)

	/** @gpu_rev: GPU revision. */
	__u32 config;

	__u32 sram_size;

	/** @pad: MBZ. */
	__u32 pad;
};
/**
 * struct drm_ethos_dev_query - Arguments passed to DRM_ETHOS_IOCTL_DEV_QUERY
 */
struct drm_ethos_dev_query {
	/** @type: the query type (see drm_ethos_dev_query_type). */
	__u32 type;

	/**
	 * @size: size of the type being queried.
	 *
	 * If pointer is NULL, size is updated by the driver to provide the
	 * output structure size. If pointer is not NULL, the driver will
	 * only copy min(size, actual_structure_size) bytes to the pointer,
	 * and update the size accordingly. This allows us to extend query
	 * types without breaking userspace.
	 */
	__u32 size;

	/**
	 * @pointer: user pointer to a query type struct.
	 *
	 * Pointer can be NULL, in which case, nothing is copied, but the
	 * actual structure size is returned. If not NULL, it must point to
	 * a location that's large enough to hold size bytes.
	 */
	__u64 pointer;
};

/**
 * enum drm_ethos_bo_flags - Buffer object flags, passed at creation time.
 */
enum drm_ethos_bo_flags {
	/** @DRM_ETHOS_BO_NO_MMAP: The buffer object will never be CPU-mapped in userspace. */
	DRM_ETHOS_BO_NO_MMAP = (1 << 0),
};

/**
 * struct drm_ethos_bo_create - Arguments passed to DRM_IOCTL_ETHOS_BO_CREATE.
 */
struct drm_ethos_bo_create {
	/**
	 * @size: Requested size for the object
	 *
	 * The (page-aligned) allocated size for the object will be returned.
	 */
	__u64 size;

	/**
	 * @flags: Flags. Must be a combination of drm_ethos_bo_flags flags.
	 */
	__u32 flags;

	/**
	 * @handle: Returned handle for the object.
	 *
	 * Object handles are nonzero.
	 */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;
};

/**
 * struct drm_ethos_bo_mmap_offset - Arguments passed to DRM_IOCTL_ETHOS_BO_MMAP_OFFSET.
 */
struct drm_ethos_bo_mmap_offset {
	/** @handle: Handle of the object we want an mmap offset for. */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;

	/** @offset: The fake offset to use for subsequent mmap calls. */
	__u64 offset;
};

/**
 * struct drm_ethos_task - A task to be run on the NPU
 *
 * A task is the smallest unit of work that can be run on the NPU.
 */
struct drm_ethos_task {
	/** Input: DMA address to NPU mapping of register command buffer */
	__u64 cmds;

	/** Input: Number of commands in the register command buffer */
	__u32 cmd_sz;

	/** Reserved, must be zero. */
	__u32 reserved;
};

/* The value written into the cmdstream is logically:
 * relocbuf->gpuaddr + reloc_offset
 *
 * NOTE that reloc's must be sorted by order of increasing submit_offset,
 * otherwise EINVAL.
 */
struct drm_ethos_gem_submit_reloc {
	__u32 submit_offset;  /* in, offset from submit_bo */
	__u32 reloc_idx;      /* in, index of reloc_bo buffer */
	__u64 reloc_offset;   /* in, offset from start of reloc_bo */
	__u32 flags;          /* in, placeholder for now, no defined values */
};

/**
 * struct drm_ethos_job - A job to be run on the NPU
 *
 * The kernel will schedule the execution of this job taking into account its
 * dependencies with other jobs. All tasks in the same job will be executed
 * sequentially on the same core, to benefit from memory residency in SRAM.
 */
struct drm_ethos_job {
	/** Input: Pointer to an array of struct drm_ethos_task. */
	__u64 tasks;

	/** Input: Pointer to a u32 array of the BOs that are read by the job. */
	__u64 in_bo_handles;

	/** Input: Pointer to a u32 array of the BOs that are written to by the job. */
	__u64 out_bo_handles;

	__u64 in_relocs;
	__u64 out_relocs;

	/** Input: Number of tasks passed in. */
	__u32 task_count;

	/** Input: Number of input BO handles passed in (size is that times 4). */
	__u32 in_bo_handle_count;

	/** Input: Number of output BO handles passed in (size is that times 4). */
	__u32 out_bo_handle_count;

	__u32 in_reloc_count;      /* in, number of submit_reloc's */
	__u32 out_reloc_count;      /* in, number of submit_reloc's */

	/** Reserved, must be zero. */
	__u32 reserved;
};

/**
 * struct drm_ethos_submit - ioctl argument for submitting commands to the NPU.
 *
 * The kernel will schedule the execution of these jobs in dependency order.
 */
struct drm_ethos_submit {
	/** Input: Pointer to an array of struct drm_ethos_job. */
	__u64 jobs;

	/** Input: Number of jobs passed in. */
	__u32 job_count;

	/** Reserved, must be zero. */
	__u32 reserved;
};


/**
 * DRM_IOCTL_ETHOS() - Build a ethos IOCTL number
 * @__access: Access type. Must be R, W or RW.
 * @__id: One of the DRM_ETHOS_xxx id.
 * @__type: Suffix of the type being passed to the IOCTL.
 *
 * Don't use this macro directly, use the DRM_IOCTL_ETHOS_xxx
 * values instead.
 *
 * Return: An IOCTL number to be passed to ioctl() from userspace.
 */
#define DRM_IOCTL_ETHOS(__access, __id, __type) \
	DRM_IO ## __access(DRM_COMMAND_BASE + DRM_ETHOS_ ## __id, \
			   struct drm_ethos_ ## __type)

enum {
	DRM_IOCTL_ETHOS_DEV_QUERY =
		DRM_IOCTL_ETHOS(WR, DEV_QUERY, dev_query),
	DRM_IOCTL_ETHOS_BO_CREATE =
		DRM_IOCTL_ETHOS(WR, BO_CREATE, bo_create),
	DRM_IOCTL_ETHOS_BO_MMAP_OFFSET =
		DRM_IOCTL_ETHOS(WR, BO_MMAP_OFFSET, bo_mmap_offset),
};

#if defined(__cplusplus)
}
#endif

#endif /* _ETHOS_DRM_H_ */
