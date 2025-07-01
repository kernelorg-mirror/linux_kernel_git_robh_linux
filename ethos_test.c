

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


bo_handle cmd_bo_create(int fd, const void *buf, int size)
{
	struct drm_ethos_cmdstream_bo_create cmd_bo_create = {
		.size = size,
		.data = (uintptr_t)buf,
	};
	ioctl(fd, DRM_IOCTL_ETHOS_CMDSTREAM_BO_CREATE, &cmd_bo_create);

	return cmd_bo_create.handle;
}

int submit_job(int fd, bo_handle cmd, bo_handle *region_bos, int sram_size)
{
	struct drm_ethos_job jobs[] = {
		{
			.cmd_bo = cmd,
			.sram_size = sram_size,
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

void dma_test(void)
{
	int fd, ret;
	bo_handle cmd_handle;
	bo_handle region_bos[8] = {};
	uint32_t *src_bo, *dst_bo;
	static const uint32_t cmds[] = {
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

	fd = open("/dev/accel/accel0", O_RDWR | O_CLOEXEC);
	dev_query(fd);

	src_bo = bo_create(fd, BO_SIZE, &region_bos[0]);
	dst_bo = bo_create(fd, BO_SIZE, &region_bos[2]);

	for (int i = 0; i < BO_SIZE/4; i++)
		src_bo[i] = 0xdeadbeef;

	printf("cmd buffer = %p\n", cmds);
	cmd_handle = cmd_bo_create(fd, cmds, sizeof(cmds));
	submit_job(fd, cmd_handle, region_bos, 0);

	ret = bo_wait(fd, region_bos[2]);
	if (ret)
		printf("error waiting on BO - %d\n", ret);

	//src_bo += BO_SIZE/4 - 4;
	//printf("src %llx: 0x%x 0x%x 0x%x 0x%x\n", src_bo, src_bo[0], src_bo[1], src_bo[2], src_bo[3]);
	dst_bo += BO_SIZE/4 - 4;
	printf("dst %llx: 0x%x 0x%x 0x%x 0x%x\n", dst_bo, dst_bo[0], dst_bo[1], dst_bo[2], dst_bo[3]);
	close(fd);
	sleep(1);
}

#define SRAM_SIZE 0x10000

void sram_dma_test(void)
{
	int fd, ret;
	bo_handle cmd_handle;
	bo_handle region_bos[8] = {};
	uint32_t *src_bo, *dst_bo;
	static const uint32_t cmds[] = {
		0x00000123,
		0x00000130, // cmd0.NPU_SET_DMA0_SRC_REGION
		0x00004030, 0x00000000, // cmd1.NPU_SET_DMA0_SRC
		0x00020131, // cmd0.NPU_SET_DMA0_DST_REGION
		0x00004031, 0x00000000, // cmd1.NPU_SET_DMA0_DST
		0x00004032, SRAM_SIZE, // cmd1.NPU_SET_DMA0_LEN
		0x00000010, // cmd0.NPU_OP_DMA_START
		0x00000011, // cmd0.NPU_OP_DMA_WAIT               0
		0x00020130, // cmd0.NPU_SET_DMA0_SRC_REGION
		0x00004030, 0x00000000, // cmd1.NPU_SET_DMA0_SRC
		0x00010131, // cmd0.NPU_SET_DMA0_DST_REGION
		0x00004031, 0x00000000, // cmd1.NPU_SET_DMA0_DST
		0x00004032, SRAM_SIZE, // cmd1.NPU_SET_DMA0_LEN
		0x00000010, // cmd0.NPU_OP_DMA_START
		0xffff0000, // cmd0.NPU_OP_STOP               65535
	};

	fd = open("/dev/accel/accel0", O_RDWR | O_CLOEXEC);
	dev_query(fd);

	src_bo = bo_create(fd, SRAM_SIZE, &region_bos[0]);
	dst_bo = bo_create(fd, SRAM_SIZE, &region_bos[1]);

	for (int i = 0; i < SRAM_SIZE/4; i++)
		src_bo[i] = 0xdeadbeef;

	printf("cmd buffer = %p\n", cmds);
	cmd_handle = cmd_bo_create(fd, cmds, sizeof(cmds));
	submit_job(fd, cmd_handle, region_bos, SRAM_SIZE);

	ret = bo_wait(fd, region_bos[1]);
	if (ret)
		printf("error waiting on BO - %d\n", ret);

	dst_bo += SRAM_SIZE/4 - 4;
	printf("dst %llx: 0x%x 0x%x 0x%x 0x%x\n", dst_bo, dst_bo[0], dst_bo[1], dst_bo[2], dst_bo[3]);
	close(fd);
	sleep(1);
}



void cmd_validate_test(const char *file)
{
	int fd, cmdfd, ret, size;
	bo_handle cmd_handle;
	char buf[0x10000];

	if (!file) {
		printf("Missing cmd file\n");
		return;
	}

	fd = open("/dev/accel/accel0", O_RDWR | O_CLOEXEC);

	cmdfd = open(file, O_RDWR);
	lseek(cmdfd, 0x20, SEEK_SET);
	size = read(cmdfd, buf, 0x10000);

	printf("cmd stream is %d bytes\n", size);
	cmd_handle = cmd_bo_create(fd, buf, size);

	close(fd);
	close(cmdfd);
}

int main(int argc, char **argv)
{
	dma_test();
	sram_dma_test();

	if (argc == 2)
		cmd_validate_test(argv[1]);

	return 0;
}