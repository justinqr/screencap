#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
//#include <linux/time.h>
#include <sys/mman.h>
#include <errno.h>
#include <g2d.h>
#include <g2dExt.h>

void* g2d_init(void)
{
	void * handle;

	if(g2d_open(&handle)) {
		printf("open g2d fail\n");
		return NULL;
	}

	return handle;
}

void g2d_deinit(void* handle)
{
	if (handle != NULL)
		g2d_close(handle);
}

void format_convert_NV12_RGB(void *handle, unsigned long ybuf, unsigned long uvbuf, unsigned long rgbbuf,
		int src_w, int src_h, int dst_w, int dst_h)
{
	struct g2d_surface tG2DDst ;
	struct g2d_surface tG2DSrc ;


	tG2DSrc.planes[0] = ybuf;
	tG2DSrc.planes[1] = uvbuf;
	tG2DSrc.left = 0;
	tG2DSrc.top = 0;
	tG2DSrc.right = src_w;
	tG2DSrc.bottom = src_h;
	tG2DSrc.stride = src_w;
	tG2DSrc.width  = src_w;
	tG2DSrc.height = src_h;
	tG2DSrc.format = G2D_NV12;

	tG2DDst.planes[0] = rgbbuf;
	tG2DDst.left = 0;
	tG2DDst.top = 0;
	tG2DDst.right = dst_w;
	tG2DDst.bottom = dst_h;
	tG2DDst.stride = dst_w;
	tG2DDst.width  = dst_w;
	tG2DDst.height = dst_h;
	tG2DDst.format = G2D_BGRA8888;
	//tG2DSrc.blendfunc = G2D_SRC_ALPHA;
	//tG2DDst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
	//g2d_enable(handle, G2D_BLEND);
	int ret = g2d_blit(handle, &tG2DSrc, &tG2DDst);
	if (ret != 0){
		printf("zd :%s g2d_blit fail !\n", __func__);
		return ;
	}
	g2d_finish(handle);
	//g2d_disable(handle,G2D_BLEND);
}

void format_convert_NV12TILED_NV12(void *handle, unsigned long ybuf, unsigned long stride,
		unsigned long plane_buf0, unsigned long plane_buf1,
		int src_w, int src_h, int dst_w, int dst_h)
{
	struct g2d_surfaceEx tG2DDstEx;
	struct g2d_surfaceEx tG2DSrcEx;
	struct g2d_surface *tG2DDst = &tG2DDstEx.base;
	struct g2d_surface *tG2DSrc = &tG2DSrcEx.base;


	printf("src stride=%d, w=0x%x, h=0x%x\n",
			stride, src_w, src_h);

	tG2DSrc->planes[0] = plane_buf0;
	tG2DSrc->planes[1] = plane_buf1;
	tG2DSrc->left = 0;
	tG2DSrc->top = 0;
	tG2DSrc->right = src_w;
	tG2DSrc->bottom = src_h;
	tG2DSrc->stride = stride;
	tG2DSrc->width  = src_w;
	tG2DSrc->height = src_h;
	tG2DSrc->format = G2D_NV12;
	tG2DSrcEx.tiling = G2D_AMPHION_TILED;

	printf("dst, w=0x%x, h=0x%x\n",
			dst_w, dst_h);
	tG2DDst->planes[0] = ybuf;
	tG2DDst->planes[1] = ybuf + dst_w * dst_h;
	tG2DDst->left = 0;
	tG2DDst->top = 0;
	tG2DDst->right = dst_w;
	tG2DDst->bottom = dst_h;
	tG2DDst->stride = stride;
	tG2DDst->width  = dst_w;
	tG2DDst->height = dst_h;
	tG2DDst->format = G2D_NV12;
	tG2DDstEx.tiling = G2D_LINEAR;
	int ret = g2d_blitEx(handle, &tG2DSrcEx, &tG2DDstEx);
	if (ret != 0){
		printf("zd %s: g2d_blit fail !\n", __func__);
		return ;
	}
        g2d_flush(handle);
	g2d_finish(handle);
}

void format_convert_RGBTILED_RGB(void *handle, unsigned long stride, int src_fmt, int src_tiled,
		unsigned long srcbuf, int src_w, int src_h,
		unsigned long dstbuf, int dst_w, int dst_h)
{
	struct g2d_surfaceEx tG2DDstEx;
	struct g2d_surfaceEx tG2DSrcEx;
	struct g2d_surface *tG2DDst = &tG2DDstEx.base;
	struct g2d_surface *tG2DSrc = &tG2DSrcEx.base;


	printf("src paddr=0x%x, stride=%d, w*h=%d*%d\n",
			srcbuf, stride, src_w, src_h);
	printf("dst paddr=0x%x, w*h=%d*%d\n",
			dstbuf, dst_w, dst_h);

	tG2DSrc->planes[0] = srcbuf;
	tG2DSrc->left = 0;
	tG2DSrc->top = 0;
	tG2DSrc->right = src_w;
	tG2DSrc->bottom = src_h;
	tG2DSrc->stride = stride;
	tG2DSrc->width  = src_w;
	tG2DSrc->height = src_h;
	tG2DSrc->format = G2D_ARGB8888;
	tG2DSrcEx.tiling = G2D_SUPERTILED;

	tG2DDst->planes[0] = dstbuf;
	tG2DDst->left = 0;
	tG2DDst->top = 0;
	tG2DDst->right = dst_w;
	tG2DDst->bottom = dst_h;
	tG2DDst->stride = dst_w;
	tG2DDst->width  = dst_w;
	tG2DDst->height = dst_h;
	tG2DDst->format = G2D_UYVY;
	tG2DDstEx.tiling = G2D_LINEAR;
	int ret = g2d_blitEx(handle, &tG2DSrcEx, &tG2DDstEx);
	if (ret != 0){
		printf("zd %s: g2d_blit fail !\n", __func__);
		return ;
	}
	g2d_finish(handle);
}
