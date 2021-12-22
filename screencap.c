#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <setjmp.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <string.h>
#include "buf_manage.h"

typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef unsigned long int uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

enum
{
    LINEAR_BASE      = 0, /* linear. */
    TILED_BASE       = 1, /* 4x4 tile. */
    SUPERTILED_BASE  = 2, /* 64x64 super tile. */
    MINORTILED_BASE  = 3, /* Minor tiled. */

    MULTITILED_BIT   = 0x10,
    MULTICHIP_BIT    = 0x20,

    LINEAR           = LINEAR_BASE,

    TILED            = TILED_BASE,
    SUPERTILED       = SUPERTILED_BASE,
    MINORTILED       = MINORTILED_BASE,

    MULTI_TILED      = TILED | MULTITILED_BIT,
    MULTI_SUPERTILED = SUPERTILED | MULTITILED_BIT,
    MULTI_MINORTILED = MINORTILED | MULTITILED_BIT
};

enum
{
    LEGACY_SUPERTILE   = 0, /* legacy supertile mode. */
    HZ_SUPERTILE       = 1, /* supertile mode when HZ. */
    FASTMSAA_SUPERTILE = 2  /* supertile mode when fast msaa. */
};


struct custom_buf *src_buf;
struct custom_buf *dst_buf;
int src_fmt = 6;
int src_tiled = 8;

int hsae_ion_fd = -1;

void* g2d_init(void);
void g2d_deinit(void* handle);
void format_convert_NV12TILED_NV12(void *handle, unsigned long ybuf, unsigned long stride,
		unsigned long plane_buf0, unsigned long plane_buf1,
		int src_w, int src_h, int dst_w, int dst_h);


typedef struct tagBITMAPFILEHEADER {
	WORD bfType;		//值为0x4d42
	DWORD bfSize;		//位图文件的大小
	DWORD bfReserved;
	DWORD bfOffBits;

} __attribute__ ((packed)) BITMAPFILEHEADER;

typedef struct tagBITMAPINFOHEADER {
	DWORD biSize;		//位图信息头的字节数sizeof(BITMAPINFOHEADER)
	DWORD biWidth;		//以像素为单位的图像宽度
	DWORD biHeight;		//以像素为单位的图像长度
	WORD biPlanes;		//目标设备的位平面数
	WORD biBitCount;	//每个像素的位数【1】
	DWORD biCompression;	//图像的压缩格式（这个值几乎总是为0）
	DWORD biSizeImage;	//以字节为单位的图像数据的大小
	DWORD biXPelsPerMeter;	//水平方向上的每米的像素个数
	DWORD biYPelsPerMeter;	//垂直方向上的每米的像素个数
	DWORD biClrUsed;	//调色板中实际使用的颜色数，通常为0
	DWORD biClrImportant;	//实现位图时必须的颜色数，通常为0
} __attribute__ ((packed)) BITMAPINFOHEADER;

struct drm_screeninfo {
	uint32_t width;
	uint32_t height;
	uint32_t bits_per_pixel;
	uint8_t *fb_addr[4];
	uint32_t paddr;
};

uint8_t raw_save_flag = 0;

// Definition for drmModeGetFB2.
// If the libdrm already contains this, remove the following definition
// ----------------------------------------------------------------------------------
#ifndef drmModeGetFB2
typedef struct _drmModeFB2 {
	uint32_t fb_id;
	uint32_t width, height;
	uint32_t pixel_format; /* fourcc code from drm_fourcc.h */
	uint32_t modifier; /* applies to all buffers */
	uint32_t flags;

	/* per-plane GEM handle; may be duplicate entries for multiple planes */
	uint32_t handles[4];
	uint32_t pitches[4]; /* bytes */
	uint32_t offsets[4]; /* bytes */
} drmModeFB2, *drmModeFB2Ptr;

#define DRM_IOCTL_MODE_GETFB2       DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
static inline int DRM_IOCTL(int fd, unsigned long cmd, void *arg)
{
	int ret = drmIoctl(fd, cmd, arg);
	return ret < 0 ? -errno : ret;
}

	drmModeFB2Ptr
drmModeGetFB2(int fd, uint32_t fb_id)
{
	struct drm_mode_fb_cmd2 get;
	drmModeFB2Ptr ret;
	int err;

	memset(&get, 0, sizeof(get));
	get.fb_id = fb_id;

	err = DRM_IOCTL(fd, DRM_IOCTL_MODE_GETFB2, &get);
	if (err != 0)
		return NULL;

	ret = drmMalloc(sizeof(drmModeFB2));
	if (!ret)
		return NULL;

	ret->fb_id = fb_id;
	ret->width = get.width;
	ret->height = get.height;
	ret->pixel_format = get.pixel_format;
	ret->flags = get.flags;
	ret->modifier = get.modifier[0];
	memcpy(ret->handles, get.handles, sizeof(uint32_t) * 4);
	memcpy(ret->pitches, get.pitches, sizeof(uint32_t) * 4);
	memcpy(ret->offsets, get.offsets, sizeof(uint32_t) * 4);

	return ret;
}

void drmModeFreeFB2(drmModeFB2Ptr ptr)
{
	if (!ptr)
		return;

	/* we might add more frees later. */
	drmFree(ptr);
}
#endif
// ----------------------------------------------------------------------------------

static unsigned int NV12Tile2linear(unsigned int nPicWidth,
					unsigned int nPicHeight,
					unsigned int uVOffsetLuma,
					unsigned int uVOffsetChroma,
					unsigned int nFsWidth,
					uint8_t **nBaseAddr,
					uint8_t *pDstBuffer
				       )
{
	unsigned int i;
	unsigned int h_tiles, v_tiles, v_offset, nLines, vtile, htile;
	unsigned int nLinesLuma  = nPicHeight;
	unsigned int nLinesChroma = nPicHeight >> 1;
	uint8_t      *cur_addr;
	unsigned int *pBuffer = (unsigned int *)pDstBuffer;
	unsigned int *pTmpBuffer;

	pTmpBuffer = (unsigned int*)malloc(256 * 4);
	if (!pTmpBuffer) {
		printf("failed to alloc pTmpBuffer\r\n");
		return -1;
	}

        //printf("%s:%d\n", __func__, __LINE__);
        // Read Top Luma
	nLines    = nLinesLuma;
	v_offset  = uVOffsetLuma;
        // Tile: 8 x 256
	h_tiles   = (nPicWidth + 7) >> 3;
	v_tiles   = (nLines + v_offset + 127) >> 7;

	for (vtile = 0; vtile < v_tiles; vtile++)
	{
		unsigned int v_base_offset = nFsWidth * 128 * vtile;

		for (htile = 0; htile < h_tiles; htile++)
		{
			cur_addr = (uint8_t *)(nBaseAddr[0] + htile * 1024 + v_base_offset);
			memcpy(pTmpBuffer, cur_addr, 256 * 4);

			for (i = 0; i < 128; i++)
			{
				int line_num  = (i + 128 * vtile) - v_offset;
				unsigned int line_base = (line_num * nPicWidth) >> 2;
				// Skip data that is off the bottom of the pic
				if (line_num == (int)nLines)
					break;
				// Skip data that is off the top of the pic
				if (line_num < 0)
					continue;
				pBuffer[line_base + (2 * htile) + 0] = pTmpBuffer[2 * i + 0];
				pBuffer[line_base + (2 * htile) + 1] = pTmpBuffer[2 * i + 1];
			}        
		}
	}

	pBuffer += (nPicWidth * nPicHeight) >> 2;

	// Read Top Chroma
	nLines    = nLinesChroma;
	v_offset  = uVOffsetChroma;
	h_tiles = (nPicWidth + 7) >> 3;
	v_tiles = (nLines + v_offset + 127) >> 7;
        printf("ht=%d, vt=%d\n", h_tiles,v_tiles);
	for (vtile = 0; vtile < v_tiles; vtile++)
	{
		unsigned int v_base_offset = nFsWidth * 128 * vtile;

		for (htile = 0; htile < h_tiles; htile++)
		{
			cur_addr = (uint8_t *)(nBaseAddr[1] + htile * 1024 + v_base_offset);
			memcpy(pTmpBuffer, cur_addr, 256 * 4);

			for (i = 0; i < 128; i++)
			{
				int line_num = (i + 128 * vtile) - v_offset;
				unsigned int line_base = (line_num * nPicWidth) >> 2;
				// Skip data that is off the bottom of the pic
				if (line_num == (int)nLines)
					break;
				// Skip data that is off the top of the pic
				if (line_num < 0)
					continue;
				pBuffer[line_base + (2 * htile) + 0] = pTmpBuffer[2 * i + 0];
				pBuffer[line_base + (2 * htile) + 1] = pTmpBuffer[2 * i + 1];
			}
		}
	}

        free(pTmpBuffer);
	return(0);
}

static inline uintptr_t _tile_addr(
    const void * addr0,
    const void * addr1,
    int tiling,
    int mode,
    int x,
    int y,
    int stride,
    int bypp
    )
{
    uintptr_t addr;
    int xt, yt;
    int xxt, yyt;

    if (tiling & MULTITILED_BIT)
    {
        /* addr = (((x >> 3) ^ (y >> 2)) & 0x01) ? addr1 : addr0; */
        addr = (((x >> 1) ^ y) & 0x04) ? (uintptr_t) addr1
             : (uintptr_t) addr0;

        /* X' = 15'{ X[14:4], Y[2], X[2:0] } */
        xt = (x & ~0x8) | ((y & 0x4) << 1);

        /* Y' = 15'{ 0, Y[14:3], Y[1:0] } */
        yt = ((y & ~0x7) >> 1) | (y & 0x3);
    }
    else
    {
        addr = (uintptr_t) addr0;
        xt = x;
        yt = y;
    }

    /* Check super tiling. */
    if ((tiling & SUPERTILED) == SUPERTILED)
    {
        /* Super tiled. */
        switch (mode)
        {
        case LEGACY_SUPERTILE:
            /* coord = 21'{ X[21-1:6],Y[5:2],X[5:2],Y[1:0],X[1:0] }. */
            xxt = ((xt &  0x03) << 0) |
                  ((yt &  0x03) << 2) |
                  ((xt &  0x3C) << 2) |
                  ((yt &  0x3C) << 6) |
                  ((xt & ~0x3F) << 6);
            break;

        case HZ_SUPERTILE:
        default:
            /* coord = 21'{ X[21-1:6], Y[5:4],X[5:3],Y[3:2],X[2],Y[1:0],X[1:0] }. */
            xxt = ((xt &  0x03) << 0) |
                  ((yt &  0x03) << 2) |
                  ((xt &  0x04) << 2) |
                  ((yt &  0x0C) << 3) |
                  ((xt &  0x38) << 4) |
                  ((yt &  0x30) << 6) |
                  ((xt & ~0x3F) << 6);
            break;

        case FASTMSAA_SUPERTILE:
            /* 21'{ X[21-1:6], Y[5],X[5],Y[4],X[4], Y[3],X[3],Y[2],X[2],Y[1:0],X[1:0] } */
            xxt = ((xt &  0x03) << 0) |
                  ((yt &  0x03) << 2) |
                  ((xt &  0x04) << 2) |
                  ((yt &  0x04) << 3) |
                  ((xt &  0x08) << 3) |
                  ((yt &  0x08) << 4) |
                  ((xt &  0x10) << 4) |
                  ((yt &  0x10) << 5) |
                  ((xt &  0x20) << 5) |
                  ((yt &  0x20) << 6) |
                  ((xt & ~0x3F) << 6);
            break;
        }

        yyt = yt & ~0x3F;
    }
    else if ((tiling & MINORTILED) == MINORTILED)
    {
        /* Minor tiled. */
        xxt = ((xt &  0x01) << 0) |
              ((yt &  0x01) << 1) |
              ((xt &  0x02) << 1) |
              ((yt &  0x02) << 2) |
              ((xt & ~0x03) << 2);

        yyt = yt & ~0x03;
    }
    else /* TILED */
    {
        /* 4x4 tiled. */
        xxt = ((xt &  0x03) << 0) +
              ((yt &  0x03) << 2) +
              ((xt & ~0x03) << 2);

        yyt = yt & ~0x03;
    }

     return (addr + yyt * stride + xxt * bypp);
}


void * tile(
    const void * linear,
    unsigned int width,
    unsigned int height,
    int bypp,
    int tiling,
    int mode,
    int * stride,
    int * vstride
    )
{
    void * addr0;
    void * addr1;
    int halign, valign;
    int align_width, align_height;
    int hstride;
    unsigned int x, y;

    /* Check address input. */
    if (linear == NULL)
    {
        fprintf(stderr, "tile: Invalid address: NULL\n");
        return NULL;
    }

    /* Check bytes per pixel. */
    if (bypp != 1 && bypp != 2 && bypp != 4 && bypp != 8)
    {
        fprintf(stderr, "tile: Bytes per pixel(%d) not supported.", bypp);
        return NULL;
    }

    /* Check tiling and get alignment. */
    switch (tiling & 0xF)
    {
    case TILED:
        halign = 4;
        valign = 4;
        break;

    case SUPERTILED:
        halign = 64;
        valign = 64;
        break;

    case MINORTILED:
        halign = 4;
        valign = 4;
        break;

    default:
        fprintf(stderr, "detile: Invalid tiling:%d\n", tiling);
        return NULL;
    }

    /* Check multi-tiled bit. */
    if (tiling & MULTITILED_BIT)
    {
        valign *= 2;
    }

    /* Align to tile. */
    align_width  = (width  + halign - 1) & ~(halign - 1);
    align_height = (height + valign - 1) & ~(valign - 1);
    hstride      = align_width * bypp;

    /* Allocate linear video memory. */
    addr0 = malloc(align_height * hstride);

    if (addr0 == NULL)
    {
        fprintf(stderr, "Out of memory.\n");
        return NULL;
    }

    /* Clear output memory. */
    memset(addr0, 0, align_height * hstride);

    /* Second pe address. */
    addr1 = (void *) ((long) addr0 + hstride * align_height / 2);

    /* Go through all pixels on linear surface. */
    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            void * addr = (void *) _tile_addr(addr0, addr1, tiling, mode, x, y, hstride, bypp);

            /* Detile. */
            switch (bypp)
            {
            case 8:
                *(uint64_t *) addr = ((uint64_t *) linear)[y * width + x];
                break;

            case 4:
                *(uint32_t *) addr = ((uint32_t *) linear)[y * width + x];
                break;

            case 2:
                *(uint16_t *) addr = ((uint16_t *) linear)[y * width + x];
                break;

            case 1:
            default:
                *(uint8_t *) addr = ((uint8_t *) linear)[y * width + x];
                break;
            }
        }
    }

    /* Output. */
    if (stride != NULL)
    {
        *stride  = hstride;
    }

    if (vstride != NULL)
    {
        *vstride = align_height;
    }

    return addr0;
}


void * detile(
    const void * addr0,
    const void * addr1,
    unsigned int width,
    unsigned int height,
    int bypp,
    int tiling,
    int mode
    )
{
    uintptr_t linear;
    int halign,  valign;
    unsigned int x, y;
    unsigned int xoffset, yoffset;
    unsigned int stride;
    uintptr_t addr;
    uintptr_t laddr;


    /* Check address input. */
    if (addr0 == NULL)
    {
        fprintf(stderr, "detile: Invalid address: NULL\n");
        return NULL;
    }

    /* Check bytes per pixel. */
    if (bypp != 1 && bypp != 2 && bypp != 4 && bypp != 8)
    {
        fprintf(stderr, "detile: Bytes per pixel(%d) not supported.", bypp);
        return NULL;
    }

    /* Check tiling and get alignment. */
    switch (tiling & 0xF)
    {
    case TILED:
        halign = 4;
        valign = 4;
        break;

    case SUPERTILED:
        halign = 64;
        valign = 64;
        break;

    case MINORTILED:
        halign = 4;
        valign = 4;
        break;

    default:
        fprintf(stderr, "detile: Invalid tiling:%d\n", tiling);
        return NULL;
    }

    /* Check multi-tiled bit. */
    if (tiling & MULTITILED_BIT)
    {
        valign *= 2;
    }

    /* Check width. */
    if ((width & (halign - 1)) != 0)
    {
        fprintf(stderr, "detile: width not aligned: %d\n", width);
        return NULL;
    }

    /* Check height. */
    if ((height & (valign - 1)) != 0)
    {
        fprintf(stderr, "detile: height not aligned: %d\n", height);
        return NULL;
    }

    /* Compute stride. */
    stride = width * bypp;

    /* Move to correct address if not set. */
    if (addr1 == NULL && (tiling & MULTITILED_BIT))
    {
        addr1 = (const void *) ((long) addr0 + stride * height / 2);
    }

    /* Allocate linear video memory. */
    linear = (uintptr_t) malloc(stride * height);

    if (linear == 0U)
    {
        printf("Out of memory.\n");
        return NULL;
    }

    /* Clear output memory. */
    memset((void *) linear, 0, stride * height);

    xoffset = 0;
    yoffset = 0;
    width  -= 0;
    height -= 0;

    /* Top. */
    for (y = yoffset; y < ((yoffset + 3) & ~3); y++)
    {
        /* Top-left. */
        for (x = xoffset; x < ((xoffset + 3) & ~3); x++)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) laddr = *(uint64_t *) addr;
                break;

            case 4:
                *(uint32_t *) laddr = *(uint32_t *) addr;
                break;

            case 2:
                *(uint16_t *) laddr = *(uint16_t *) addr;
                break;

            case 1:
            default:
                *(uint8_t *) laddr  = *(uint8_t *) addr;
                break;
            }
        }

        /* Top-middle. */
        for (x = ((xoffset + 3) & ~3); x < (width & ~3); x += 4)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            memcpy((void *) laddr, (void *) addr, bypp * 4);
        }

        /* Top-right. */
        for (x = (width & ~3); x < width; x++)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) laddr = *(uint64_t *) addr;
                break;

            case 4:
                *(uint32_t *) laddr = *(uint32_t *) addr;
                break;

            case 2:
                *(uint16_t *) laddr = *(uint16_t *) addr;
                break;

            case 1:
            default:
                *(uint8_t *) laddr  = *(uint8_t *) addr;
                break;
            }
        }
    }

    /* Middle. */
    for (y = ((yoffset + 3) & ~3); y < (height & ~3); y += 4)
    {
        /* Middle-left. */
        for (x = xoffset; x < ((xoffset + 3) & ~3); x += 4)
        {
            addr  = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) (laddr + stride * 0) = *(uint64_t *) (addr + bypp * 0);
                *(uint64_t *) (laddr + stride * 1) = *(uint64_t *) (addr + bypp * 4);
                *(uint64_t *) (laddr + stride * 2) = *(uint64_t *) (addr + bypp * 8);
                *(uint64_t *) (laddr + stride * 3) = *(uint64_t *) (addr + bypp * 12);
                break;

            case 4:
                *(uint32_t *) (laddr + stride * 0) = *(uint32_t *) (addr + bypp * 0);
                *(uint32_t *) (laddr + stride * 1) = *(uint32_t *) (addr + bypp * 4);
                *(uint32_t *) (laddr + stride * 2) = *(uint32_t *) (addr + bypp * 8);
                *(uint32_t *) (laddr + stride * 3) = *(uint32_t *) (addr + bypp * 12);
                break;

            case 2:
                *(uint16_t *) (laddr + stride * 0) = *(uint16_t *) (addr + bypp * 0);
                *(uint16_t *) (laddr + stride * 1) = *(uint16_t *) (addr + bypp * 4);
                *(uint16_t *) (laddr + stride * 2) = *(uint16_t *) (addr + bypp * 8);
                *(uint16_t *) (laddr + stride * 3) = *(uint16_t *) (addr + bypp * 12);
                break;

            case 1:
            default:
                *(uint8_t *) (laddr + stride * 0) = *(uint8_t *) (addr + bypp * 0);
                *(uint8_t *) (laddr + stride * 1) = *(uint8_t *) (addr + bypp * 4);
                *(uint8_t *) (laddr + stride * 2) = *(uint8_t *) (addr + bypp * 8);
                *(uint8_t *) (laddr + stride * 3) = *(uint8_t *) (addr + bypp * 12);
                break;
            }
        }

        /* Middle-middle. */
        for (x = ((xoffset + 3) & ~3); x < (width & ~3); x += 4)
        {
            addr  = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            memcpy((void *) (laddr + stride * 0), (void *) (addr + bypp * 0), bypp * 4);
            memcpy((void *) (laddr + stride * 1), (void *) (addr + bypp * 4), bypp * 4);
            memcpy((void *) (laddr + stride * 2), (void *) (addr + bypp * 8), bypp * 4);
            memcpy((void *) (laddr + stride * 3), (void *) (addr + bypp * 12), bypp * 4);
        }

        /* Middle-right. */
        for (x = (width & ~3); x < width; x++)
        {
            addr  = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) (laddr + stride * 0) = *(uint64_t *) (addr + bypp * 0);
                *(uint64_t *) (laddr + stride * 1) = *(uint64_t *) (addr + bypp * 4);
                *(uint64_t *) (laddr + stride * 2) = *(uint64_t *) (addr + bypp * 8);
                *(uint64_t *) (laddr + stride * 3) = *(uint64_t *) (addr + bypp * 12);
                break;

            case 4:
                *(uint32_t *) (laddr + stride * 0) = *(uint32_t *) (addr + bypp * 0);
                *(uint32_t *) (laddr + stride * 1) = *(uint32_t *) (addr + bypp * 4);
                *(uint32_t *) (laddr + stride * 2) = *(uint32_t *) (addr + bypp * 8);
                *(uint32_t *) (laddr + stride * 3) = *(uint32_t *) (addr + bypp * 12);
                break;

            case 2:
                *(uint16_t *) (laddr + stride * 0) = *(uint16_t *) (addr + bypp * 0);
                *(uint16_t *) (laddr + stride * 1) = *(uint16_t *) (addr + bypp * 4);
                *(uint16_t *) (laddr + stride * 2) = *(uint16_t *) (addr + bypp * 8);
                *(uint16_t *) (laddr + stride * 3) = *(uint16_t *) (addr + bypp * 12);
                break;

            case 1:
            default:
                *(uint8_t *) (laddr + stride * 0) = *(uint8_t *) (addr + bypp * 0);
                *(uint8_t *) (laddr + stride * 1) = *(uint8_t *) (addr + bypp * 4);
                *(uint8_t *) (laddr + stride * 2) = *(uint8_t *) (addr + bypp * 8);
                *(uint8_t *) (laddr + stride * 3) = *(uint8_t *) (addr + bypp * 12);
                break;
            }
        }
    }

    /* Bottom. */
    for (y = (height & ~3); y < height; y++)
    {
        /* Bottom-left. */
        for (x = xoffset; x < ((xoffset + 3) & ~3); x++)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) laddr = *(uint64_t *) addr;
                break;

            case 4:
                *(uint32_t *) laddr = *(uint32_t *) addr;
                break;

            case 2:
                *(uint16_t *) laddr = *(uint16_t *) addr;
                break;

            case 1:
            default:
                *(uint8_t *) laddr  = *(uint8_t *) addr;
                break;
            }
        }

        /* Bottom-middle. */
        for (x = ((xoffset + 3) & ~3); x < (width & ~3); x += 4)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            memcpy((void *) laddr, (void *) addr, bypp * 4);
        }

        /* Bottom-right. */
        for (x = (width & ~3); x < width; x++)
        {
            addr = _tile_addr(addr0, addr1, tiling, mode, x, y, stride, bypp);
            laddr = linear + y * stride + x * bypp;

            switch (bypp)
            {
            case 8:
                *(uint64_t *) laddr = *(uint64_t *) addr;
                break;

            case 4:
                *(uint32_t *) laddr = *(uint32_t *) addr;
                break;

            case 2:
                *(uint16_t *) laddr = *(uint16_t *) addr;
                break;

            case 1:
            default:
                *(uint8_t *) laddr  = *(uint8_t *) addr;
                break;
            }
        }
    }

    return (void *) linear;
}


/*XRGB->XBGR*/
void csc(uint8_t * in, uint8_t * out, uint32_t pixelcount)
{
    int i = 0;
    for (i = 0; i < pixelcount; i++) {
        out[i * 4 + 3] = in[i * 4 + 3];
        out[i * 4 + 2] = in[i * 4 + 0];
        out[i * 4 + 1] = in[i * 4 + 1];
        out[i * 4 + 0] = in[i * 4 + 2];
    }

}

void rotate(uint8_t * in, uint32_t * out, uint32_t width, uint32_t height)
{
    uint32_t i, j;
    uint8_t *rotate_out = in;
    uint32_t pixelcount = width * height;
    rotate_out = (uint8_t *) malloc(pixelcount * 4);
    uint32_t *p = (uint32_t *) rotate_out;
    /*rotate&BGRX->BGRX */
    for (i = 0; i < pixelcount; i++) {
        rotate_out[i * 4 + 3] = in[4 * pixelcount - i * 4 + 3];
        rotate_out[i * 4 + 2] = in[4 * pixelcount - i * 4 + 2];
        rotate_out[i * 4 + 1] = in[4 * pixelcount - i * 4 + 1];
        rotate_out[i * 4 + 0] = in[4 * pixelcount - i * 4 + 0];
    }

    /*monitor */
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++)
            out[i * width + j] = p[i * width + (width - j)];
    }

    free(rotate_out);
}

void write_fb_to_bmp(struct drm_screeninfo sinfo, char *fname)
{
    FILE *fp1;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    uint32_t *csc_tmp = NULL;
    printf("sinfo.xres=%d\n", sinfo.width);
    printf("sinfo.yres=%d\n", sinfo.height);
    printf("sinfo.bit_per_bits=%d\n", sinfo.bits_per_pixel);

    fp1 = fopen(fname, "wb");
    if (!fp1) {
        printf("open %s error\n", fname);
        exit(4);
    }
    //Set BITMAPINFOHEADER
    bi.biSize = 40;
    bi.biWidth = sinfo.width;
    bi.biHeight = sinfo.height;
    bi.biPlanes = 1;
    bi.biBitCount = sinfo.bits_per_pixel;
    bi.biCompression = 0;
    bi.biSizeImage = bi.biWidth * bi.biHeight * bi.biBitCount / 8;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;
    //Set BITMAPFILEHEADER
    bf.bfType = 0x4d42;
    bf.bfSize = 54 + bi.biSizeImage;
    bf.bfReserved = 0;
    bf.bfOffBits = 54;
    if (raw_save_flag != 1) {
        fwrite(&bf, 14, 1, fp1);
        fwrite(&bi, 40, 1, fp1);
        csc_tmp = (uint32_t *) malloc(bi.biSizeImage);
        rotate(sinfo.fb_addr, csc_tmp, bi.biWidth, bi.biHeight);
        fwrite((uint8_t *) (csc_tmp), bi.biSizeImage, 1, fp1);
        free(csc_tmp);
    } else {
        printf("!!!save raw data!!!!!\n");
        fwrite((uint8_t *) (sinfo.fb_addr), bi.biSizeImage, 1, fp1);
    }

    printf("save %s OK\n\n", fname);
    fclose(fp1);
}

void write_fb_to_raw(uint8_t *fb_addr, int len, char *fname)
{
	FILE *fp1;

	fp1 = fopen(fname, "wb");
	if (!fp1) {
		printf("open %s error\n", fname);
		exit(4);
	}

	fwrite(fb_addr, len, 1, fp1);

	printf("%s: %d ==> %s OK\n\n", __func__, len, fname);
	fclose(fp1);
}

void write_fb_to_raw_bmp(uint8_t *fb_addr, int width, int height, int bpp)
{
    FILE *fp0, *fp1;
    char rawname[20], bmpname[20];
    uint32_t *tmp0 =NULL;
    void *tmp1 = NULL;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    uint32_t *csc_tmp = NULL;

    sprintf(rawname, "%dx%d-%d.rgb", width, height, bpp);
    fp0 = fopen(rawname, "wb");
    if (!fp0) {
        printf("open %s error\n", rawname);
        exit(4);
    }

    sprintf(bmpname, "%dx%d-%d.bmp", width, height, bpp);
    fp1 = fopen(bmpname, "wb");
    if (!fp1) {
        printf("open %s error\n", bmpname);
        exit(4);
    }

    //Set BITMAPFILEHEADER
    bf.bfType = 0x4d42;
    bf.bfSize = 54 + bi.biSizeImage;
    bf.bfReserved = 0;
    bf.bfOffBits = 54;

    //Set BITMAPINFOHEADER
    bi.biSize = 40;
    bi.biWidth = width;
    bi.biHeight = height;
    bi.biPlanes = 1;
    bi.biBitCount = bpp;
    bi.biCompression = 0;
    bi.biSizeImage = bi.biWidth * bi.biHeight * bi.biBitCount / 8;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    fwrite(&bf, 14, 1, fp1);
    fwrite(&bi, 40, 1, fp1);

    tmp0 = (uint32_t *) malloc(bi.biSizeImage);
    tmp1 = (uint8_t *) malloc(bi.biSizeImage*8);

    tmp1 = detile((const void * )fb_addr, NULL, width, height, 4, SUPERTILED, FASTMSAA_SUPERTILE);
    fwrite((uint8_t *)tmp1, bi.biSizeImage, 1, fp0);
    printf("%s: write size %d ==> %s OK\n\n", __func__, bi.biSizeImage, rawname);

    rotate(tmp1, tmp0, bi.biWidth, bi.biHeight);
    fwrite((uint8_t *)tmp0, bi.biSizeImage, 1, fp1);
    printf("%s: write size %d ==> %s OK\n\n", __func__, bi.biSizeImage+54, bmpname);

    free(tmp0);
    free(tmp1);
    fclose(fp0);
    fclose(fp1);
}

void plane_2_fb_rgb(uint32_t fd, drmModePlane * ovr)
{
    drmModeFBPtr fb;
    struct drm_mode_map_dumb map = { };
    uint8_t * buf;
    int imgsize;

    fb = drmModeGetFB(fd, ovr->fb_id);
    if(!fb)
        return;
    else
		printf("FB:fb_id=%d, width=%d, height=%d, pitch=%d, bpp=%d, depth=%d\n",
				fb->fb_id, fb->width, fb->height, fb->pitch, fb->bpp, fb->depth);

    map.handle = fb->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    buf = (uint8_t *) mmap(0,
                 (fb->pitch * fb->height),
                 PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 map.offset);
    
    imgsize = fb->pitch * fb->height;
    write_fb_to_raw_bmp(buf, fb->width, fb->height, fb->bpp);
}


void plane_2_fb_yuv(uint32_t fd, drmModePlane * ovr)
{
	struct drm_screeninfo fb_info;
	struct drm_screeninfo screen_info;
	drmModeFB2Ptr fb2;
	drmModeCrtcPtr crtc;
	struct drm_mode_map_dumb map = { };
	char bmpname[20];
	void *g2d_handle;
	unsigned int imgsize;
	int i;
	//unsigned int ylen, uvlen;
    unsigned int ylenp, uvlenp;
    uint8_t *pTmpBuffer=NULL;

	g2d_handle = g2d_init();
	if (g2d_handle == NULL)
		return;

	if(1)
	{
		fb2 = drmModeGetFB2(fd, ovr->fb_id);
		if(!fb2)
		{
			printf("%s:%d: Error: %s\n", __func__, __LINE__, strerror(errno));
			return;
		}
		printf("FB2:fb_id=%d, width=%d, height=%d, pixel_format=0x%x, modifier=0x%x\n",
				fb2->fb_id, fb2->width, fb2->height, fb2->pixel_format, fb2->modifier);

		if ((fb2->pixel_format != 0x3231564e) || (fb2->modifier != 1)) {
			printf("%s:%d: Error: only support NV12 tiled to NV12 now\n", __func__, __LINE__);
			return;
		}

		for(i=0;i<4;i++)
		{
			printf("FB2:handles[%d]=0x%x\n", i, fb2->handles[i]);
			printf("FB2:pitches[%d]=0x%x\n", i, fb2->pitches[i]);
			printf("FB2:offsets[%d]=0x%x\n", i, fb2->offsets[i]);
		}
	} 

	//alloc src buf by ion
	// imgsize = (fb2->width * fb2->height * 12) / 8;        
	imgsize = fb2->pitches[0] * fb2->height + fb2->pitches[1] * fb2->height / 2;
	printf("%d: Image size: %d\n", __LINE__, imgsize);
	src_buf = (struct custom_buf *)custom_buf_alloc(imgsize, BUF_TYPE_CUSTOM);
	if (src_buf == NULL)
		return;

	//get y buf
	fb_info.width = fb2->width;
	fb_info.height = fb2->height;
	fb_info.bits_per_pixel = 12;
	map.handle = fb2->handles[0];
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	//ylen = fb_info.width * fb_info.height;
	ylenp = fb2->pitches[0] * fb_info.height;
	printf("%d: y_len_p: %d\n", __LINE__, ylenp);
	fb_info.fb_addr[0] = (uint8_t *) mmap(0, ylenp,
                                        PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                        map.offset);
	memcpy(src_buf->buf_vaddr, fb_info.fb_addr[0], ylenp);

	//get uv buf
	map.handle = fb2->handles[1];
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	//uvlen = ylen / 2;
        uvlenp = fb2->pitches[1] * fb_info.height / 2;
	printf("%d: uv_len_p: %d\n", __LINE__, uvlenp);
	fb_info.fb_addr[1] =
		(uint8_t *) mmap(0, uvlenp,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				map.offset);
	memcpy(src_buf->buf_vaddr + ylenp,
			fb_info.fb_addr[1], uvlenp);

	//alloc dst buf by ion
	crtc = drmModeGetCrtc(fd, ovr->crtc_id);
        screen_info.width = crtc->width;
        screen_info.height = crtc->height;
	screen_info.bits_per_pixel = 12;
	dst_buf = (struct custom_buf *)custom_buf_alloc(
			(screen_info.width * screen_info.height * 12) / 8 + 204800,
			BUF_TYPE_CUSTOM);
	if (dst_buf == NULL)
		return;
	screen_info.fb_addr[0] = dst_buf->buf_vaddr;
	
#if 1
    sprintf(bmpname, "./%d_%d.til", ovr->plane_id, ovr->fb_id);
	write_fb_to_raw(src_buf->buf_vaddr, imgsize, bmpname);
#endif

	if (0) {
        printf("Un-tile using G2D-OCL!\n");
		format_convert_NV12TILED_NV12(g2d_handle, dst_buf->buf_paddr, fb2->pitches[0],
				src_buf->buf_paddr, src_buf->buf_paddr + (fb_info.width * fb_info.height), 
				fb_info.width, fb_info.height, screen_info.width, screen_info.height);
	} else {
        unsigned char *nBaseAddr[2];

        printf("Un-tile using SW!\n");
        nBaseAddr[0] = (unsigned char *)(fb_info.fb_addr[0]);
        nBaseAddr[1] = (unsigned char *)(fb_info.fb_addr[1]);

        pTmpBuffer = (uint8_t *)malloc((fb_info.width * fb_info.height * 12) / 8);
        NV12Tile2linear(fb_info.width, fb_info.height,  0, 0, fb2->pitches[0], nBaseAddr, pTmpBuffer);
        screen_info.fb_addr[0] = pTmpBuffer;
	}

	sprintf(bmpname, "./%d_%d.yuv", ovr->plane_id, ovr->fb_id);
	write_fb_to_raw(screen_info.fb_addr[0], (fb_info.width * fb_info.height * 12) /8 , bmpname);

	custom_buf_free(src_buf, BUF_TYPE_CUSTOM);
	custom_buf_free(dst_buf, BUF_TYPE_CUSTOM);
	g2d_deinit(g2d_handle);

    if(!pTmpBuffer)
        free(pTmpBuffer);
}

void plane_2_fb(uint32_t fd, drmModePlane * ovr)
{
    drmModeFBPtr fb;
    fb = drmModeGetFB(fd, ovr->fb_id);
    if(!fb) {
        printf("FB ID: %d (YUV)\n", ovr->fb_id);
        plane_2_fb_yuv(fd, ovr);
	}
    else {
        printf("FB ID: %d (RGB)\n", ovr->fb_id);
        plane_2_fb_rgb(fd, ovr);
    }
    return;
}

int main(int argc, char *argv[])
{
	int fd = 0;
	drmModeResPtr res;
	drmModePlaneResPtr planeres;
	drmModePlane *ovr;
	int i;

	printf("screen capture test app. \n");

	if ((argv[1] != NULL && argv[1][0] == '-' && argv[1][1] == 'h')
			|| (argv[1] == NULL)) {
		printf("example: drm2bmp /dev/dri/card0  raw \n");
		printf
			("if raw==1 save raw fb data else save bmp [BMP only support RGB]\n");
		printf("save planeid_fbid.bmp \n");
		return 1;
	}

	if (argv[2] != NULL && argv[2][0] == '1')
		raw_save_flag = 1;
	else
		raw_save_flag = 0;

	if (argv[3] != NULL)
		src_fmt = atoi(argv[3]);
	if (argv[4] != NULL)
		src_tiled = atoi(argv[4]);

	/* open ion device for alloc phy addr */
	hsae_ion_fd = open("/dev/ion", O_RDONLY, 0);
	if (hsae_ion_fd < 0) {
		printf("[%s] open ion fail", __func__);
		return -1;
	}

	fd = open(argv[1], O_RDWR);
	//fd = drmOpen("imx-drm", NULL);
	if (fd < 0) {
		printf("Error:cannot open the drm device!\n");
		return -1;
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	res = drmModeGetResources(fd);
	if (res == 0) {
		printf("Failed to get resources from card\n");
		drmClose(fd);
		return -2;
	}
	planeres = drmModeGetPlaneResources(fd);
	if (!planeres) {
		printf("drmModeGetPlaneResources failed\n");
		return -3;
	}
	printf("count_planes: %d\n", planeres->count_planes);
	printf("%s\t%s\t%s\t%s,%s\t%s,%s\t%s\t%s\n", "plane_id", "crtc_id",
			"fb_id", "crtc_x", "crtc_y", "x", "y", "gamma_size",
			"possible_crtcs");
	for (i = 0; i < planeres->count_planes; i++) {
		ovr = drmModeGetPlane(fd, planeres->planes[i]);
		if (!ovr)
			continue;
		if ((ovr->fb_id > 0)) {
    		printf
	    		("  %d\t\t  %d\t  %d\t   %d  ,  %d\t%d,%d\t%-8d\t0x%08x\n",
		    	 ovr->plane_id, ovr->crtc_id, ovr->fb_id, ovr->crtc_x,
			     ovr->crtc_y, ovr->x, ovr->y, ovr->gamma_size,
    			 ovr->possible_crtcs);

			plane_2_fb(fd, ovr);
        }
	}

	drmModeFreeResources(res);
	drmClose(fd);
	close(hsae_ion_fd);
	return 0;
}
