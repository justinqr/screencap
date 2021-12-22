/*
 * Copyright (c) 2021 HSAE Inc.
 * Author : Dan
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
//#include <linux/time.h>
#include <sys/mman.h>
#include <errno.h>
#include "buf_manage.h"


extern int hsae_ion_fd;
int ion_dev_fd;

struct custom_buf * alloc_ion_dma_buff(unsigned long bufferLength) {
	struct ion_allocation_data allocation;
	struct ion_heap_query query;
	int heapCnt;
	struct dma_buf_phys dma_phys;
	struct custom_buf *ion_buf;

	ion_buf = (struct custom_buf*)malloc(bufferLength);
	if (ion_buf == NULL) {
		printf("[%s] buf malloc fail, %s", __func__, strerror(errno));
		return NULL;
	} else
		memset(ion_buf, 0, sizeof(struct custom_buf));

	ion_dev_fd = hsae_ion_fd;
	if(ion_dev_fd < 0){
		printf("[%s] open ion fail", __func__);
		return NULL;
	}

	memset(&query, 0, sizeof(query));
	if(ioctl(ion_dev_fd, ION_IOC_HEAP_QUERY, &query) < 0){
		printf("[%s] ion heap query fail %d %s ",
				__func__,errno,strerror(errno));
		close(ion_dev_fd);
		return NULL;
	}

	heapCnt = query.cnt;
	struct ion_heap_data ihd[heapCnt];

	memset(&ihd, 0, sizeof(ihd));
	memset(&query, 0, sizeof(query));
	query.cnt = heapCnt;
	query.heaps = (__u64)&ihd;
	if(ioctl(ion_dev_fd, ION_IOC_HEAP_QUERY, &query) < 0){
		printf("[%s] ion heap query fail %d %s ",
				__func__,errno,strerror(errno));
	}

	// add heap ids from heap type.
	int mCNHeapIds = 0;
	for (int i=0; i<heapCnt; i++) {
		if (ihd[i].type == ION_HEAP_TYPE_DMA) {
			mCNHeapIds |=  1 << ihd[i].heap_id;
			continue;
		}
	}

	allocation.len = bufferLength ;
	allocation.heap_id_mask = mCNHeapIds;
	allocation.flags = ION_HEAP_TYPE_DMA;

	if(ioctl(ion_dev_fd, ION_IOC_ALLOC, &allocation) < 0){
		printf("[%s] ion alloc fail %d %s \n",__func__,errno,strerror(errno));
		close(ion_dev_fd);
		return NULL;
	}

	if(ioctl(allocation.fd, DMA_BUF_IOCTL_PHYS, &dma_phys)){
		printf("[%s] ion DMA_BUF_IOCTL_PHYS fail %d %s ", __func__ ,
				errno,strerror(errno));
	}

	ion_buf->buf_paddr = dma_phys.phys;

	ion_buf->buf_vaddr = (void * )mmap(0, bufferLength, PROT_READ|PROT_WRITE, 
			MAP_SHARED, allocation.fd, 0);
	if (ion_buf->buf_paddr == MAP_FAILED) {
		printf("Could not mmap %s", strerror(errno));
		close(ion_dev_fd);
		return NULL;
	}

	ion_buf->fd = allocation.fd;
	ion_buf->ion_fd = ion_dev_fd;
	ion_buf->len = allocation.len;

	printf("dmabuf vaddr=0x%p, paddr=0x%x\n",
			ion_buf->buf_vaddr, ion_buf->buf_paddr);


	//close(ion_dev_fd);
	return ion_buf;
}

struct g2d_buf * alloc_g2d_buff(int fd, int len, int type)
{
	void * handle;
	struct g2d_buf* pbuf;

	if(g2d_open(&handle)) {
		printf("open g2d fail\n");
		return NULL;
	}

	if(type == 0)
		pbuf = g2d_buf_from_fd(fd);
	else
		pbuf = g2d_alloc(len, 0);

	if((void *)pbuf == NULL)
		printf("[%s] g2d_buf get fail, %s\n", __func__, strerror(errno));
	else {
		printf("g2d_buf vaddr=0x%p, paddr=0x%x \n",
				pbuf->buf_vaddr, pbuf->buf_paddr);
	}

	g2d_close(handle);

	return pbuf;
}

void* custom_buf_alloc(unsigned long len, int type)
{
	struct custom_buf *custom_ion_buf;
	struct g2d_buf* custom_g2d_buf;
	printf("%s : %d  len=%d \n", __func__, __LINE__, len);

	custom_ion_buf = alloc_ion_dma_buff(len);
	if ((void *)custom_ion_buf == NULL) {
		printf("[%s] ion_buf alloc fail, %s\n", __func__, strerror(errno));
		return NULL;
	}

	switch(type) {
	case BUF_TYPE_CUSTOM:
		return (void *)custom_ion_buf;
	case BUF_TYPE_G2D:
		custom_g2d_buf = alloc_g2d_buff(custom_ion_buf->fd, len, 0);
		if ((void *)custom_g2d_buf == NULL) {
			printf("[%s] g2d_buf alloc fail, %s\n", __func__, strerror(errno));
			return NULL;
		}
		return (void *)custom_g2d_buf;

	default:
		printf("[%s] type is undefined\n", __func__);
		return NULL;
	}
}

void custom_buf_free(void * p_buf, int type)
{
	struct custom_buf *pbuf = p_buf;
	switch(type) {
	case BUF_TYPE_CUSTOM:
		//todo:
		//dmabuf free
		printf("dmabuf fd=%d vaddr=0x%p\n",
			pbuf->fd, pbuf->buf_vaddr);
		munmap(pbuf->buf_vaddr, pbuf->len);
		pbuf->buf_vaddr = 0;
		pbuf->buf_paddr = 0;
		free(p_buf);
		break;
	case BUF_TYPE_G2D:
		g2d_free(pbuf);
		break;
	default:
		printf("[%s] type is undefined\n", __func__);
		break;
	}
	return ;
}

