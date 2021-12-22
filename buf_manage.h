#ifndef _BUF_MANAGE_H
#define _BUF_MANAGE_H

#include <imx/linux/ion.h>
#include <imx/linux/dma-buf.h>
#include <g2d.h>


/**
 * struct custom_buf - buffer return to userspace for allocations
 * @buf_vaddr:		virtual address of buffer
 * @buf_paddr:		physics address of buffer
 * @len:		size of the allocation
 * @fd:			dmabuf fd for buffer
 * @ion_fd:		ion fd for free buffer
 *
 * Provide to userspace
 */
struct custom_buf {
	void *buf_vaddr;
	unsigned long  buf_paddr;
	unsigned long len;
	int fd;
	int ion_fd;
};



/**
 * enum custom_buf_type - passed from userspace for allocations
 * @BUF_TYPE_G2D:	buf type is struct g2d_buf
 * @BUF_TYPE_CUSTOM:	buf type is struct custom_buf
 * @len:		size of the allocation
 * @fd:			dmabuf fd for buffer
 *
 * Provided by userspace as an argument to custom_buf_alloc
 */
typedef enum {
	BUF_TYPE_G2D,
	BUF_TYPE_CUSTOM,
}custom_buf_type;


/**
 * custom_buf_alloc() - called by userspace to alloc a buffer
 * return pointer of custom_buf
 *
 * @len:		allocated bytes
 * @type:		allocated buf type
 *
 * This called by userspace to alloc a buffer. Buffer type is g2d_buf,
 * when type is BUF_TYPE_G2D. Buffer type is custom_buf, when type is
 * BUF_TYPE_CUSTOM.
 */
void* custom_buf_alloc(unsigned long len, int type);

/**
 * custom_buf_free() - called by userspace to free a buffer
 * return NONE
 *
 * @pbuf:		pointer of the buffer
 * @type:		buf type
 *
 * This called by userspace to free a buffer.
 */
void custom_buf_free(void * pbuf, int type);

#endif /* _BUF_MANAGE_H */
