/*
 * V4L2 VIVI + FIMC + DMABUF sharing Test Application
 * Tomasz Stanislawski <t.stanislaws@samsung.com>
 *
 * This application is used to test DMABUF sharing between VIVI and FIMC
 * device.
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define ARRAY_SIZE(tab) \
	(sizeof(tab) / sizeof(*tab))

#define CLEAR(s) memset(&s, 0, sizeof(s))

#define MIN(x,y) ((x) < (y) ? (x) : (y))
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define CLAMP(x,l,h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))

static inline int __info(const char *prefix, const char *file, int line,
	const char *fmt, ...)
{
	int errsv = errno;
	va_list va;

	va_start(va, fmt);
	fprintf(stderr, "%s(%s:%d): ", prefix, file, line);
	vfprintf(stderr, fmt, va);
	va_end(va);
	errno = errsv;

	return 1;
}

#define ERRSTR strerror(errno)

#define LOG(...) fprintf(stderr, __VA_ARGS__)

#define ERR(...) __info("Error", __FILE__, __LINE__, __VA_ARGS__)
#define ERR_ON(cond, ...) ((cond) ? ERR(__VA_ARGS__) : 0)

#define CRIT(...) \
	do { \
		__info("Critical", __FILE__, __LINE__, __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while(0)
#define CRIT_ON(cond, ...) do { if (cond) CRIT(__VA_ARGS__); } while(0)

struct format {
	unsigned long fourcc;
	unsigned long width;
	unsigned long height;
};

struct config {
	char vivi_path[256];
	char fimc0_path[256];
	char fimc1_path[256];
	int rotate;
	bool hflip;
	bool vflip;
	bool dump;
	bool help;
	char dst_path[256];
	int dst_size;
	int dst_offset;
	struct format fmt[3];
};

struct state {
	struct config config;
	int vivi_fd;
	int fimc0_fd;
	int fimc1_fd;
	void *dst_ptr;
	unsigned long dst_size;
};

static void usage(void)
{
#define HELP(...) fprintf(stderr, __VA_ARGS__)
	HELP("Usage:\n");
	HELP("\t-v path    path to vivi [default /dev/video0]\n");
	HELP("\t-f path    path to fimc0 [default /dev/video1]\n");
	HELP("\t-F path    path to fimc1 [default /dev/video3]\n");
	HELP("\t-0 4cc@WxH format between VIVI and FIMC0\n");
	HELP("\t-1 4cc@WxH format between FIMC0 and FIMC1\n");
	HELP("\t-2 4cc@WxH format between FIMC1 and destination\n");
	HELP("\t-V         vertical flip\n");
	HELP("\t-H         horizontal flip\n");
	HELP("\t-R angle   rotation by angle [default 0]\n");
	HELP("\t-m size[@file[+offset]]  destination mapping\n");
	HELP("\t-d         dump PPMs\n");
	HELP("\t-h         print this help\n");
	HELP("\n");
#undef HELP
}

int parse_format(char *s, struct format *fmt)
{
	char fourcc[5] = "";
	int ret = sscanf(s, "%4[^@]@%lux%lu", fourcc,
		&fmt->width, &fmt->height);

	if (ERR_ON(ret != 3, "'%s' is not in 4cc@WxH format\n", s)) {
		CLEAR(*fmt);
		return -EILSEQ;
	}

	fmt->fourcc = ((unsigned)fourcc[0] << 0) |
		((unsigned)fourcc[1] << 8) |
		((unsigned)fourcc[2] << 16) |
		((unsigned)fourcc[3] << 24);

	return 0;
}

static int config_create(struct config *config, int argc, char *argv[])
{
	int opt, ret = -EINVAL;

	CLEAR(*config);
	strcpy(config->vivi_path, "/dev/video0");
	strcpy(config->fimc0_path, "/dev/video1");
	strcpy(config->fimc1_path, "/dev/video3");

	/* parse options */
	while ((opt = getopt(argc, argv, ":v:f:F:0:1:2:VHR:m:dh")) != -1) {
		switch (opt) {
		case 'v':
			strcpy(config->vivi_path, optarg);
			break;
		case 'f':
			strcpy(config->fimc0_path, optarg);
			break;
		case 'F':
			strcpy(config->fimc1_path, optarg);
			break;
		case '0':
		case '1':
		case '2':
			ret = parse_format(optarg, &config->fmt[opt - '0']);
			if (ERR_ON(ret < 0, "invalid format\n"))
				return ret;
			break;
		case 'V':
			config->vflip = true;
			break;
		case 'H':
			config->hflip = true;
			break;
		case 'R':
			ret = sscanf(optarg, "%d", &config->rotate);
			if (ERR_ON(ret != 1, "invalid rotation\n"))
				return -EILSEQ;
			break;
		case 'd':
			config->dump = true;
			break;
		case 'h':
			config->help = true;
			break;
		case 'm':
			ret = sscanf(optarg, "%i@%255[^+-]%i", &config->dst_size,
				config->dst_path, &config->dst_offset);
			if (ERR_ON(ret < 1, "invalid mapping\n"))
				return -EILSEQ;
			break;
		case ':':
			ERR("missing argument for option %c\n", optopt);
			return -EINVAL;
		default: /* '?' */
			ERR("invalid option %c\n", optopt);
			return -EINVAL;
		}
	}

	return 0;
}

void dump_format(char *str, struct v4l2_format *fmt)
{
	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
		LOG("%s: width=%u height=%u format=%.4s bpl=%u\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat,
			pix->plane_fmt[0].bytesperline);
	} else {
		struct v4l2_pix_format *pix = &fmt->fmt.pix;
		LOG("%s: width=%u height=%u format=%.4s bpl=%u\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat,
			pix->bytesperline);
	}
}

int dump_yuyv(int fd, uint8_t *src, int w, int h)
{
	uint8_t buf[6 * 256], *dst = buf;
	for (int wh = w * h; wh; wh--) {
		int y0, y1, u, v;
		int r, g, b;
		y0 = *src++;
		u  = *src++;
		y1 = *src++;
		v  = *src++;
		r = (298 * y0 + 409 * v - 56992) >> 8;
		g = (298 * y0 - 100 * u - 208 * v + 34784) >> 8;
		b = (298 * y0 + 516 * u - 70688) >> 8;
		*dst++ = CLAMP(r, 0, 255);
		*dst++ = CLAMP(g, 0, 255);
		*dst++ = CLAMP(b, 0, 255);
		r = (298 * y1 + 409 * v - 56992) >> 8;
		g = (298 * y1 - 100 * u - 208 * v + 34784) >> 8;
		b = (298 * y1 + 516 * u - 70688) >> 8;
		*dst++ = CLAMP(r, 0, 255);
		*dst++ = CLAMP(g, 0, 255);
		*dst++ = CLAMP(b, 0, 255);
		if (dst - buf < ARRAY_SIZE(buf))
			continue;
		int ret = write(fd, buf, dst - buf);
		if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
			return -errno;
		dst = buf;
	}
	int ret = write(fd, buf, dst - buf);
	if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
		return -errno;
	return 0;
}

static inline uint8_t expand8(int v, int size)
{
	switch (size) {
		case 0:
			return 0xff;
		case 1:
			return v & 0x01 ? 0xff : 0;
		case 2:
			v &= 0x03;
			return (v << 6) | (v << 4) | (v << 2) | v;
		case 3:
			v &= 0x07;
			return (v << 5) | (v << 2) | (v >> 1);
		case 4:
			v &= 0x0f;
			return (v << 4) | v;
		case 5:
			v &= 0x1f;
			return (v << 3) | (v >> 2);
		case 6:
			v &= 0x3f;
			return (v << 2) | (v >> 4);
		case 7:
			v &= 0x7f;
			return (v << 1) | (v >> 6);
		default:
			return v;
	}
}

int dump_rgb565(int fd, uint8_t *src, int w, int h)
{
	uint8_t buf[3 * 256], *dst = buf;
	for (int wh = w * h; wh; wh--) {
		int v = 0;
		v |= (int)(*src++);
		v |= (int)(*src++) << 8;
		*dst++ = expand8(v, 5);
		*dst++ = expand8(v >> 5, 6);
		*dst++ = expand8(v >> 11, 5);
		if (dst - buf < ARRAY_SIZE(buf))
			continue;
		int ret = write(fd, buf, dst - buf);
		if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
			return -errno;
		dst = buf;
	}
	int ret = write(fd, buf, dst - buf);
	if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
		return -errno;
	return 0;
}

int dump_rgb32(int fd, uint8_t *src, int w, int h)
{
	uint8_t buf[3 * 256], *dst = buf;
	for (int wh = w * h; wh; wh--) {
		++src;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		if (dst - buf < ARRAY_SIZE(buf))
			continue;
		int ret = write(fd, buf, dst - buf);
		if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
			return -errno;
		dst = buf;
	}
	int ret = write(fd, buf, dst - buf);
	if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
		return -errno;
	return 0;
}

int dump_image(char *name, unsigned long fourcc, int w, int h, void *data)
{
	int fd = open(name, O_WRONLY | O_CREAT, 0644);
	if (ERR_ON(fd < 0, "open: %s\n", ERRSTR))
		return -errno;
	char buf[64];
	sprintf(buf, "P6\n%d %d\n255\n", w, h);
	int ret = write(fd, buf, strlen(buf));
	if (ERR_ON(ret < 0, "write: %s\n", ERRSTR))
		return -errno;
	ret = 0;
	switch (fourcc) {
	case V4L2_PIX_FMT_YUYV:
		ret = dump_yuyv(fd, data, w, h);
		break;
	case V4L2_PIX_FMT_RGB565:
		ret = dump_rgb565(fd, data, w, h);
		break;
	case V4L2_PIX_FMT_RGB32:
		ret = dump_rgb32(fd, data, w, h);
		break;
	default:
		ERR("format %.4s not supported\n", (char*)&fourcc);
		ret = -EINVAL;
	}
	close(fd);
	if (ret)
		ERR("failed to dump %s\n", name);
	else
		LOG("%s dumped successfully\n", name);
	return ret;
}

int setup_formats(struct state *st)
{
	int ret = 0;
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	/* apply cmdline format if available */
	if (st->config.fmt[0].fourcc) {
		struct v4l2_pix_format *pix = &fmt.fmt.pix;
		pix->pixelformat = st->config.fmt[0].fourcc;
		pix->width = st->config.fmt[0].width;
		pix->height = st->config.fmt[0].height;
		ret = ioctl(st->vivi_fd, VIDIOC_S_FMT, &fmt);
		if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
			return -errno;
		dump_format("pre-vivi-capture", &fmt);
	}

	/* get format from VIVI */
	ret = ioctl(st->vivi_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("vivi-capture", &fmt);

	/* setup format for FIMC 0 */
	/* keep copy of format for to-mplane conversion */
	struct v4l2_pix_format pix = fmt.fmt.pix;

	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	struct v4l2_pix_format_mplane *pix_mp = &fmt.fmt.pix_mp;

	pix_mp->width = pix.width;
	pix_mp->height = pix.height;
	pix_mp->pixelformat = pix.pixelformat;
	pix_mp->num_planes = 1;
	pix_mp->plane_fmt[0].bytesperline = pix.bytesperline;

	ret = ioctl(st->fimc0_fd, VIDIOC_S_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("fimc0-output", &fmt);

	/* setup image conversion */
	struct v4l2_control ctrl;

	if (st->config.hflip) {
		CLEAR(ctrl);
		ctrl.id = V4L2_CID_HFLIP;
		ctrl.value = 1;
		ret = ioctl(st->fimc0_fd, VIDIOC_S_CTRL, &ctrl);
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_CTRL(hflip): %s\n",
		           ERRSTR))
			return -errno;
	}

	if (st->config.vflip) {
		CLEAR(ctrl);
		ctrl.id = V4L2_CID_VFLIP;
		ctrl.value = 1;
		ret = ioctl(st->fimc0_fd, VIDIOC_S_CTRL, &ctrl);
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_CTRL(vflip): %s\n",
		           ERRSTR))
			return -errno;
	}

	if (st->config.rotate) {
		CLEAR(ctrl);
		ctrl.id = V4L2_CID_ROTATE;
		ctrl.value = st->config.rotate;
		ret = ioctl(st->fimc0_fd, VIDIOC_S_CTRL, &ctrl);
		if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_CTRL(rotate): %s\n",
		           ERRSTR))
			return -errno;
	}

	/* set format on fimc0 capture */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	/* try cmdline format, or use fimc0-output instead */
	if (st->config.fmt[1].fourcc) {
		struct v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
		CLEAR(*pix);
		pix->pixelformat = st->config.fmt[1].fourcc;
		pix->width = st->config.fmt[1].width;
		pix->height = st->config.fmt[1].height;
		pix->plane_fmt[0].bytesperline = 0;
	}

	dump_format("pre-fimc0-capture", &fmt);
	ret = ioctl(st->fimc0_fd, VIDIOC_S_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;

	/* copy format from fimc0 capture to fimc1 output */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("fimc0-capture", &fmt);

	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_S_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;
	ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("fimc1-output", &fmt);

	/* set format on fimc1 capture */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	/* try cmdline format, or use fimc1-output instead */
	if (st->config.fmt[2].fourcc) {
		struct v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
		pix->pixelformat = st->config.fmt[2].fourcc;
		pix->width = st->config.fmt[2].width;
		pix->height = st->config.fmt[2].height;
		pix->plane_fmt[0].bytesperline = 0;
	}

	ret = ioctl(st->fimc1_fd, VIDIOC_S_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_S_FMT: %s\n", ERRSTR))
		return -errno;

	ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("fimc1-capture", &fmt);

	return 0;
}

int allocate_buffers(struct state *st)
{
	int ret;
	struct v4l2_requestbuffers rb;
	CLEAR(rb);

	/* request buffers for VIVI */
	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_DMABUF;
	ret = ioctl(st->vivi_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	/* request buffers for FIMC0 */
	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(st->fimc0_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(st->fimc0_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	/* request buffers for FIMC1 */
	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_DMABUF;
	ret = ioctl(st->fimc1_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_USERPTR;
	ret = ioctl(st->fimc1_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	/* allocate memory for destination data */
	int fd = -1; /* assume anonymous mapping */
	int flags = MAP_ANONYMOUS | MAP_PRIVATE;
	if (st->config.dst_path[0]) {
		fd = open(st->config.dst_path, O_RDWR);
		if (ERR_ON(fd < 0, "open: %s\n", ERRSTR))
			return -errno;
		flags = MAP_SHARED;
	}

	LOG("dst_path=%s dst_size=%i dst_offset=%i\n",
		st->config.dst_path, st->config.dst_size,
		st->config.dst_offset);

	size_t size = st->config.dst_size;
	/* get size from FIMC1 format if none is given at cmdline */
	if (!size) {
		struct v4l2_format fmt;
		CLEAR(fmt);
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
		if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
			return -errno;
		/* someone should be shot for the layout of v4l2_format */
		size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	}

	st->dst_ptr = mmap(NULL, size, PROT_READ, flags, fd,
		st->config.dst_offset);

	if (ERR_ON(st->dst_ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	st->dst_size = size;

	close(fd);

	return 0;
}

int process_vivi(struct state *st)
{
	int ret;
	struct v4l2_exportbuffer eb;
	CLEAR(eb);

	/* export buffer index=0 from FIMC0 */
	eb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_EXPBUF, &eb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_EXPBUF: %s\n", ERRSTR))
		return -errno;

	/* enqueue the dmabuf to vivi */
	struct v4l2_buffer b;
	CLEAR(b);

	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_DMABUF;
	b.m.fd = eb.fd;
	ret = ioctl(st->vivi_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(st->vivi_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	/* dequeue buffer from VIVI */
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_DMABUF;
	
	ret = ioctl(st->vivi_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	/* stop streaming */
	ret = ioctl(st->vivi_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_STREAMOFF: %s\n", ERRSTR))
		return -errno;

	LOG("VIVI worked correctly\n");

	/* mmap DMABUF */
	struct v4l2_plane plane;
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QUERYBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QUERYBUF: %s\n", ERRSTR))
		return -errno;

	void *ptr = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, eb.fd, 0);
	if (ERR_ON(ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	LOG("DMABUF from FIMC0 OUTPUT mmapped correctly\n");

	/* get format for dumping */
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(st->vivi_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;

	/* dump image, ignore errors */
	if (st->config.dump)
		dump_image("0-vivi-capture-dmabuf.ppm", fmt.fmt.pix.pixelformat,
			fmt.fmt.pix.width, fmt.fmt.pix.height, ptr);

	/* small cleanup */
	munmap(ptr, plane.length);
	close(eb.fd);

	return 0;
}

int process_fimc0(struct state *st)
{
	int ret;
	struct v4l2_buffer b;
	struct v4l2_plane plane;

	/* enqueue buffer to fimc0 output */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	/* enqueue buffer to fimc0 capture */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	/* start processing */
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_DMABUF;
	b.m.planes = &plane;
	b.length = 1;

	/* grab processed buffers */
	ret = ioctl(st->fimc0_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	/* stop processing */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMOFF: %s\n", ERRSTR))
		return -errno;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_STREAMOFF: %s\n", ERRSTR))
		return -errno;

	LOG("FIMC0 worked correctly\n");

	/* querybuf and mmap fimc0 output */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QUERYBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QUERYBUF: %s\n", ERRSTR))
		return -errno;

	/* mmap FIMC0 output and dump it */
	void *ptr = mmap(NULL, plane.length,
		PROT_READ | PROT_WRITE, MAP_SHARED, st->fimc0_fd,
		plane.m.mem_offset);
	if (ERR_ON(ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	LOG("FIMC0 output mmapped correctly\n");

	/* get format, dump image, ignore result */
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = ioctl(st->fimc0_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;

	if (st->config.dump)
		dump_image("1-fimc0-output-mmap.ppm",
			fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, ptr);

	munmap(ptr, plane.length);

	/* querybuf and mmap fimc0 capture */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QUERYBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QUERYBUF: %s\n", ERRSTR))
		return -errno;

	ptr = mmap(NULL, plane.length,
		PROT_READ | PROT_WRITE, MAP_SHARED, st->fimc0_fd,
		plane.m.mem_offset);
	if (ERR_ON(ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	LOG("FIMC0 capture mmapped correctly\n");

	/* get format, dump image, ignore result */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	ret = ioctl(st->fimc0_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;

	if (st->config.dump)
		dump_image("2-fimc0-capture-mmap.ppm",
			fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, ptr);

	munmap(ptr, plane.length);

	return 0;
}

int process_fimc1(struct state *st)
{
	int ret;

	/* export the first buffer from FIMC0 capture */
	struct v4l2_exportbuffer eb;
	CLEAR(eb);
	eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc0_fd, VIDIOC_EXPBUF, &eb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_EXPBUF: %s\n", ERRSTR))
		return -errno;

	/* enqueue the DMABUF as FIMC1's output */
	struct v4l2_buffer b;
	struct v4l2_plane plane;
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_DMABUF;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;
	plane.m.fd = eb.fd;

	ret = ioctl(st->fimc1_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	/* enqueue userptr as FIMC1's capture */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_USERPTR;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;
	plane.m.userptr = (unsigned long)st->dst_ptr;
	plane.length = st->dst_size;

	ret = ioctl(st->fimc1_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	/* start processing */
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_STREAMON, &type);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_STREAMON: %s\n", ERRSTR))
		return -errno;

	/* grab processed buffers */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_USERPTR;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc1_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc1_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	/* stop processing */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_STREAMOFF: %s\n", ERRSTR))
		return -errno;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_STREAMOFF, &type);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_STREAMOFF: %s\n", ERRSTR))
		return -errno;

	LOG("FIMC1 worked correctly\n");

	/* mmap FIMC0 capture DMABUF and dump result */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc0_fd, VIDIOC_QUERYBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_QUERYBUF: %s\n", ERRSTR))
		return -errno;

	/* mapping DMABUF */
	void *ptr = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, eb.fd, 0);
	if (ERR_ON(ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	LOG("DMABUF from FIMC0 capture mmapped correctly\n");

	/* get format, dump image, ignore result */
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
		return 0;

	if (st->config.dump)
		dump_image("3-fimc1-output-dmabuf.ppm",
			fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, ptr);

	/* cleanup DMABUF stuff */
	munmap(ptr, plane.length);
	close(eb.fd);

	/* get format, dump fimc1-capture image, ignore errors */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
		return 0;

	msync(st->dst_ptr, st->dst_size, MS_SYNC);

	if (st->config.dump)
		dump_image("4-fimc1-capture-userptr.ppm",
			fmt.fmt.pix_mp.pixelformat,
			fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
			st->dst_ptr);

	return 0;
}
int main(int argc, char *argv[])
{
	struct state st;
	CLEAR(st);

	int ret = config_create(&st.config, argc, argv);
	if (ret) {
		usage();
		CRIT("bad arguments\n");
	}

	if (st.config.help) {
		usage();
		return EXIT_SUCCESS;
	}

	st.vivi_fd = open(st.config.vivi_path, O_RDWR);
	CRIT_ON(st.vivi_fd < 0, "failed to open VIVI at %s: %s\n",
		st.config.vivi_path, ERRSTR);

	st.fimc0_fd = open(st.config.fimc0_path, O_RDWR);
	CRIT_ON(st.vivi_fd < 0, "failed to open FIMC0 at %s: %s\n",
		st.config.fimc0_path, ERRSTR);

	st.fimc1_fd = open(st.config.fimc1_path, O_RDWR);
	CRIT_ON(st.vivi_fd < 0, "failed to open FIMC1 at %s: %s\n",
		st.config.fimc1_path, ERRSTR);

	ret = setup_formats(&st);
	CRIT_ON(ret, "failed to setup formats\n");

	ret = allocate_buffers(&st);
	CRIT_ON(ret, "failed to allocate buffers\n");

	ret = process_vivi(&st);
	CRIT_ON(ret, "failed to do vivi processing\n");

	ret = process_fimc0(&st);
	CRIT_ON(ret, "failed to do fimc0 processing\n");

	ret = process_fimc1(&st);
	CRIT_ON(ret, "failed to do fimc1 processing\n");

	LOG("Test passed\n");

	return EXIT_SUCCESS;
}
