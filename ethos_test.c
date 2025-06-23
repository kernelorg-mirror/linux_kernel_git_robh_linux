

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "ethos_accel.h"

typedef uint32_t bo_handle;

int dev_query(int fd)
{
	struct drm_ethos_npu_info info;
	struct drm_ethos_dev_query dev_query = {
		.type = DRM_ETHOS_DEV_QUERY_NPU_INFO,
		.size = sizeof(info),
		.pointer = (uintptr_t)&info,
	};

	ioctl(fd, DRM_IOCTL_ETHOS_DEV_QUERY, &dev_query);
	printf("id = %x, sram size = %d\n", info.id, info.sram_size);
}

void *bo_create(int fd, int size, bo_handle *handle)
{
	uint32_t *buf;
	struct drm_ethos_bo_create bo_create = {
		.size = size,
	};
	struct drm_ethos_bo_mmap_offset mmap_offset = {};

	ioctl(fd, DRM_IOCTL_ETHOS_BO_CREATE, &bo_create);

	mmap_offset.handle = bo_create.handle;
	ioctl(fd, DRM_IOCTL_ETHOS_BO_MMAP_OFFSET, &mmap_offset);

	buf = mmap(NULL, bo_create.size, PROT_WRITE | PROT_READ,
		   MAP_SHARED, fd, mmap_offset.offset);
	if (!buf)
		return NULL;

	*handle = bo_create.handle;
	return buf;
}

int bo_wait(int fd, bo_handle handle)
{
	struct drm_ethos_bo_wait bo_wait = {
		.handle = handle,
		.timeout_ns = INT64_MAX,
	};
	return ioctl(fd, DRM_IOCTL_ETHOS_BO_WAIT, &bo_wait);
}


bo_handle cmd_bo_create(int fd, void *buf, int size)
{
	struct drm_ethos_cmdstream_bo_create cmd_bo_create = {
		.size = size,
		.data = (uintptr_t)buf,
	};
	ioctl(fd, DRM_IOCTL_ETHOS_CMDSTREAM_BO_CREATE, &cmd_bo_create);

	return cmd_bo_create.handle;
}

int submit_job(int fd, bo_handle cmd, bo_handle *region_bos)
{
	struct drm_ethos_job jobs[] = {
		{
			.cmd_bo = cmd,
		},
	};

	memcpy(&jobs[0].region_bo_handles, region_bos, sizeof(jobs[0].region_bo_handles));

	struct drm_ethos_submit submit = {
		.jobs = (uintptr_t)jobs,
		.job_count = 1,
	};

	ioctl(fd, DRM_IOCTL_ETHOS_SUBMIT, &submit);

}

#define BO_SIZE 0x00100000UL

uint32_t cmds[] = {
	0x00000123,
	0x00000130, // cmd0.NPU_SET_DMA0_SRC_REGION
	0x00004030, 0x00000000, // cmd1.NPU_SET_DMA0_SRC
	0x00020131, // cmd0.NPU_SET_DMA0_DST_REGION
	0x00004031, 0x00000000, // cmd1.NPU_SET_DMA0_DST
	0x00004032, BO_SIZE, // cmd1.NPU_SET_DMA0_LEN
	0x00000010, // cmd0.NPU_OP_DMA_START
	0x00000011, // cmd0.NPU_OP_DMA_WAIT               0
	0xffff0000, // cmd0.NPU_OP_STOP               65535
};

int main(int argc, char **argv)
{
	int fd, ret;
	bo_handle cmd_handle;
	bo_handle region_bos[8] = {};
	uint32_t *src_bo, *dst_bo;

	fd = open("/dev/accel/accel0", O_RDWR | O_CLOEXEC);
	dev_query(fd);

	src_bo = bo_create(fd, BO_SIZE, &region_bos[0]);
	dst_bo = bo_create(fd, BO_SIZE, &region_bos[2]);

	for (int i = 0; i < BO_SIZE/4; i++)
		src_bo[i] = 0xdeadbeef;

	printf("cmd buffer = %p\n", cmds);
	cmd_handle = cmd_bo_create(fd, cmds, sizeof(cmds));
	submit_job(fd, cmd_handle, region_bos);

	ret = bo_wait(fd, region_bos[2]);
	if (ret)
		printf("error waiting on BO - %d\n", ret);

	printf("src %llx: 0x%x 0x%x 0x%x 0x%x\n", src_bo, src_bo[0], src_bo[1], src_bo[2], src_bo[3]);
	printf("dst %llx: 0x%x 0x%x 0x%x 0x%x\n", dst_bo, dst_bo[0], dst_bo[1], dst_bo[2], dst_bo[3]);
	return 0;
}