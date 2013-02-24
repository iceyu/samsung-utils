/*
 * Demo application for DMA buffer sharing between V4L2 and DRM
 * Tomasz Stanislawski <t.stanislaws@samsung.com>
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
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#include <linux/videodev2.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define ERRSTR strerror(errno)

#define BYE_ON(cond, ...) \
do { \
	if (cond) { \
		int errsv = errno; \
		fprintf(stderr, "ERROR(%s:%d) : ", \
			__FILE__, __LINE__); \
		errno = errsv; \
		fprintf(stderr,  __VA_ARGS__); \
		abort(); \
	} \
} while(0)

static inline int warn(const char *file, int line, const char *fmt, ...)
{
	int errsv = errno;
	va_list va;
	va_start(va, fmt);
	fprintf(stderr, "WARN(%s:%d): ", file, line);
	vfprintf(stderr, fmt, va);
	va_end(va);
	errno = errsv;
	return 1;
}

#define WARN_ON(cond, ...) \
	((cond) ? warn(__FILE__, __LINE__, __VA_ARGS__) : 0)

struct setup {
	char module[32];
	int conId;
	uint32_t crtId;
	char modestr[32];
	char video[32];
	unsigned int w, h;
	unsigned int use_wh : 1;
	unsigned int in_fourcc;
	unsigned int out_fourcc;
	unsigned int buffer_count;
	unsigned int use_crop : 1;
	unsigned int use_compose : 1;
	struct v4l2_rect crop;
	struct v4l2_rect compose;
};

struct drm_device {
	const char *module;
	int fd;

	unsigned int crtc_id;
	unsigned int con_id;

	drmModeModeInfo mode;
	const char *modestr;
	unsigned int format;
};

struct v4l2_device {
	const char *devname;
	int fd;

	struct v4l2_pix_format format;
};

struct buffer {
	unsigned int index;
	unsigned int bo_handle;
	unsigned int fb_handle;
	int dbuf_fd;
};

struct stream {
	struct drm_device *drm;
	struct v4l2_device *v4l2;

	struct buffer *buffers;
	unsigned int num_buffers;
	int current_buffer;
} stream;

static void usage(char *name)
{
	fprintf(stderr, "usage: %s [-bFfhiMoSst]\n", name);

	fprintf(stderr, "\nCapture options:\n\n");
	fprintf(stderr, "\t-i <video-node>\tset video node like /dev/video*\n");
	fprintf(stderr, "\t-f <fourcc>\tset input format using 4cc\n");
	fprintf(stderr, "\t-S <width,height>\tset input resolution\n");
	fprintf(stderr, "\t-s <width,height>@<left,top>\tset crop area\n");

	fprintf(stderr, "\nDisplay options:\n\n");
	fprintf(stderr, "\t-M <drm-module>\tset DRM module\n");
	fprintf(stderr, "\t-o <connector_id>:<crtc_id>:<mode>\tset a mode\n");
	fprintf(stderr, "\t-F <fourcc>\tset output format using 4cc\n");
	fprintf(stderr, "\t-t <width,height>@<left,top>\tset compose area\n");

	fprintf(stderr, "\nGeneric options:\n\n");
	fprintf(stderr, "\t-b buffer_count\tset number of buffers\n");
	fprintf(stderr, "\t-h\tshow this help\n");
}

static inline int parse_rect(char *s, struct v4l2_rect *r)
{
	return sscanf(s, "%d,%d@%d,%d", &r->width, &r->height,
		&r->top, &r->left) != 4;
}

static int parse_args(int argc, char *argv[], struct setup *s)
{
	if (argc <= 1)
		usage(argv[0]);

	int c, ret;
	memset(s, 0, sizeof(*s));

	while ((c = getopt(argc, argv, "b:F:f:hi:M:o:S:s:t:")) != -1) {
		switch (c) {
		case 'b':
			ret = sscanf(optarg, "%u", &s->buffer_count);
			if (WARN_ON(ret != 1, "incorrect buffer count\n"))
				return -1;
			break;
		case 'F':
			if (WARN_ON(strlen(optarg) != 4, "invalid fourcc\n"))
				return -1;
			s->out_fourcc = ((unsigned)optarg[0] << 0) |
				((unsigned)optarg[1] << 8) |
				((unsigned)optarg[2] << 16) |
				((unsigned)optarg[3] << 24);
			break;
		case 'f':
			if (WARN_ON(strlen(optarg) != 4, "invalid fourcc\n"))
				return -1;
			s->in_fourcc = ((unsigned)optarg[0] << 0) |
				((unsigned)optarg[1] << 8) |
				((unsigned)optarg[2] << 16) |
				((unsigned)optarg[3] << 24);
			break;
		case '?':
		case 'h':
			usage(argv[0]);
			return -1;
		case 'i':
			strncpy(s->video, optarg, 31);
			break;
		case 'M':
			strncpy(s->module, optarg, 31);
			break;
		case 'o':
			ret = sscanf(optarg, "%u:%u:%31s", &s->conId, &s->crtId,
				s->modestr);
			if (WARN_ON(ret != 3, "incorrect mode description\n"))
				return -1;
			break;
		case 'S':
			ret = sscanf(optarg, "%u,%u", &s->w, &s->h);
			if (WARN_ON(ret != 2, "incorrect input size\n"))
				return -1;
			s->use_wh = 1;
			break;
		case 's':
			ret = parse_rect(optarg, &s->crop);
			if (WARN_ON(ret, "incorrect crop area\n"))
				return -1;
			s->use_crop = 1;
			break;
		case 't':
			ret = parse_rect(optarg, &s->compose);
			if (WARN_ON(ret, "incorrect compose area\n"))
				return -1;
			s->use_compose = 1;
			break;
		}
	}

	return 0;
}

static int drm_buffer_create(struct drm_device *dev, struct buffer *b,
			     const struct v4l2_pix_format *fmt)
{
	struct drm_mode_create_dumb gem;
	struct drm_mode_destroy_dumb gem_destroy;
	int ret;

	memset(&gem, 0, sizeof gem);
	gem.width = fmt->width;
	gem.height = fmt->height;
	gem.bpp = fmt->bytesperline / fmt->width * 8;
	gem.size = fmt->sizeimage;
	ret = ioctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem);
	if (WARN_ON(ret, "CREATE_DUMB failed: %s\n", ERRSTR))
		return -1;
	b->bo_handle = gem.handle;

	struct drm_prime_handle prime;
	memset(&prime, 0, sizeof prime);
	prime.handle = b->bo_handle;

	ret = ioctl(dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (WARN_ON(ret, "PRIME_HANDLE_TO_FD failed: %s\n", ERRSTR))
		goto fail_gem;
	printf("dbuf_fd = %d\n", prime.fd);
	b->dbuf_fd = prime.fd;

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { fmt->bytesperline };
	uint32_t bo_handles[4] = { b->bo_handle };
	unsigned int fourcc = dev->format;
	if (!fourcc)
		fourcc = fmt->pixelformat;
	ret = drmModeAddFB2(dev->fd, fmt->width, fmt->height, fourcc, bo_handles,
		pitches, offsets, &b->fb_handle, 0);
	if (WARN_ON(ret, "drmModeAddFB2 failed: %s\n", ERRSTR))
		goto fail_prime;

	return 0;

fail_prime:
	close(b->dbuf_fd);

fail_gem:
	memset(&gem_destroy, 0, sizeof gem_destroy);
	gem_destroy.handle = b->bo_handle,
	ret = ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &gem_destroy);
	WARN_ON(ret, "DESTROY_DUMB failed: %s\n", ERRSTR);

	return -1;
}

static int drm_find_mode(struct drm_device *dev, drmModeModeInfo *m)
{
	int ret = -1;
	drmModeRes *res = drmModeGetResources(dev->fd);
	if (WARN_ON(!res, "drmModeGetResources failed: %s\n", ERRSTR))
		return -1;

	if (WARN_ON(res->count_crtcs <= 0, "drm: no crts\n"))
		goto fail_res;

	if (WARN_ON(res->count_connectors <= 0, "drm: no connectors\n"))
		goto fail_res;

	drmModeConnector *c;
	c = drmModeGetConnector(dev->fd, dev->con_id);
	if (WARN_ON(!c, "drmModeGetConnector failed: %s\n", ERRSTR))
		goto fail_res;

	if (WARN_ON(!c->count_modes, "connector supports no mode\n"))
		goto fail_conn;

	drmModeModeInfo *found = NULL;
	for (int i = 0; i < c->count_modes; ++i) {
		if (strcmp(c->modes[i].name, dev->modestr) == 0) {
			found = &c->modes[i];
			break;
		}
	}

	if (WARN_ON(!found, "mode %s not supported\n", dev->modestr)) {
		fprintf(stderr, "Valid modes:");
		for (int i = 0; i < c->count_modes; ++i)
			fprintf(stderr, " %s", c->modes[i].name);
		fprintf(stderr, "\n");
		goto fail_conn;
	}

	memcpy(m, found, sizeof *found);
	ret = 0;

fail_conn:
	drmModeFreeConnector(c);

fail_res:
	drmModeFreeResources(res);

	return ret;
}

static void drm_init(struct drm_device *dev, const struct v4l2_pix_format *fmt,
		     unsigned int num_buffers, struct buffer *buffers)
{
	int ret;

	dev->fd = drmOpen(dev->module, NULL);
	BYE_ON(dev->fd < 0, "drmOpen(%s) failed: %s\n", dev->module, ERRSTR);

	/* TODO: add support for multiplanar formats */
	for (unsigned int i = 0; i < num_buffers; ++i) {
		ret = drm_buffer_create(dev, &buffers[i], fmt);
		BYE_ON(ret, "failed to create buffer%d\n", i);
	}
	printf("buffers ready\n");

	ret = drm_find_mode(dev, &dev->mode);
	BYE_ON(ret, "failed to find valid mode\n");
}

static void drm_page_flip(struct drm_device *dev, struct buffer *buffer)
{
	int ret;

	ret = drmModePageFlip(dev->fd, dev->crtc_id, buffer->fb_handle,
		DRM_MODE_PAGE_FLIP_EVENT, (void*)(unsigned long)buffer->index);
	BYE_ON(ret, "drmModePageFlip failed: %s\n", ERRSTR);
}

static void v4l2_init(struct v4l2_device *dev, unsigned int num_buffers)
{
	int ret;

	dev->fd = open(dev->devname, O_RDWR);
	BYE_ON(dev->fd < 0, "failed to open %s: %s\n", dev->devname, ERRSTR);

	struct v4l2_capability caps;
	memset(&caps, 0, sizeof caps);

	ret = ioctl(dev->fd, VIDIOC_QUERYCAP, &caps);
	BYE_ON(ret, "VIDIOC_QUERYCAP failed: %s\n", ERRSTR);

	/* TODO: add single plane support */
	BYE_ON(~caps.capabilities & V4L2_CAP_VIDEO_CAPTURE,
		"video: singleplanar capture is not supported\n");

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	printf("G_FMT(start): width = %u, height = %u, 4cc = %.4s\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		(char*)&fmt.fmt.pix.pixelformat);

	fmt.fmt.pix = dev->format;

	ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
	BYE_ON(ret < 0, "VIDIOC_S_FMT failed: %s\n", ERRSTR);

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);
	printf("G_FMT(final): width = %u, height = %u, 4cc = %.4s\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		(char*)&fmt.fmt.pix.pixelformat);

	struct v4l2_requestbuffers rqbufs;
	memset(&rqbufs, 0, sizeof(rqbufs));
	rqbufs.count = num_buffers;
	rqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rqbufs.memory = V4L2_MEMORY_DMABUF;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rqbufs);
	BYE_ON(ret < 0, "VIDIOC_REQBUFS failed: %s\n", ERRSTR);
	BYE_ON(rqbufs.count < num_buffers, "video node allocated only "
		"%u of %u buffers\n", rqbufs.count, num_buffers);

	dev->format = fmt.fmt.pix;
}

static void v4l2_queue_buffer(struct v4l2_device *dev, const struct buffer *buffer)
{
	struct v4l2_buffer buf;
	int ret;

	memset(&buf, 0, sizeof buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = buffer->index;
	buf.m.fd = buffer->dbuf_fd;

	ret = ioctl(dev->fd, VIDIOC_QBUF, &buf);
	BYE_ON(ret, "VIDIOC_QBUF(index = %d) failed: %s\n", buffer->index, ERRSTR);
}

static struct buffer *v4l2_dequeue_buffer(struct v4l2_device *dev, struct buffer *buffers)
{
	struct v4l2_buffer buf;
	int ret;

	memset(&buf, 0, sizeof buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_DMABUF;
	ret = ioctl(dev->fd, VIDIOC_DQBUF, &buf);
	BYE_ON(ret, "VIDIOC_DQBUF failed: %s\n", ERRSTR);

	return &buffers[buf.index];
}

static void page_flip_handler(int fd __attribute__((__unused__)),
	unsigned int frame __attribute__((__unused__)),
	unsigned int sec __attribute__((__unused__)),
	unsigned int usec __attribute__((__unused__)),
	void *data)
{
	int index = stream.current_buffer;

	stream.current_buffer = (unsigned long)data;
	if (index < 0)
		return;

	v4l2_queue_buffer(stream.v4l2, &stream.buffers[index]);
}

int main(int argc, char *argv[])
{
	struct v4l2_device v4l2;
	struct drm_device drm;
	struct setup s;
	int ret;

	ret = parse_args(argc, argv, &s);
	BYE_ON(ret, "failed to parse arguments\n");
	BYE_ON(s.module[0] == 0, "DRM module is missing\n");
	BYE_ON(s.video[0] == 0, "video node is missing\n");

	memset(&v4l2, 0, sizeof v4l2);
	v4l2.devname = s.video;

	if (s.use_wh) {
		v4l2.format.width = s.w;
		v4l2.format.height = s.h;
	}
	if (s.in_fourcc)
		v4l2.format.pixelformat = s.in_fourcc;

	v4l2_init(&v4l2, s.buffer_count);

	struct buffer buffers[s.buffer_count];

	for (unsigned int i = 0; i < s.buffer_count; ++i)
		buffers[i].index = i;

	memset(&drm, 0, sizeof drm);
	drm.module = s.module;
	drm.modestr = s.modestr;
	drm.format = s.out_fourcc;
	drm.crtc_id = s.crtId;
	drm.con_id = s.conId;

	drm_init(&drm, &v4l2.format, s.buffer_count, buffers);

	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, buffers[0].fb_handle, 0, 0,
		&drm.con_id, 1, &drm.mode);
	BYE_ON(ret, "drmModeSetCrtc failed: %s\n", ERRSTR);

	/* The first buffer is used for the initial CRTC frame buffer. Enqueue
	 * all others to V4L2.
	 */
	for (unsigned int i = 1; i < s.buffer_count; ++i)
		v4l2_queue_buffer(&v4l2, &buffers[i]);

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(v4l2.fd, VIDIOC_STREAMON, &type);
	BYE_ON(ret < 0, "STREAMON failed: %s\n", ERRSTR);

	struct pollfd fds[] = {
		{ .fd = v4l2.fd, .events = POLLIN },
		{ .fd = drm.fd, .events = POLLIN },
	};

	/* buffer currently used by drm */
	stream.v4l2 = &v4l2;
	stream.current_buffer = 0;
	stream.buffers = buffers;
	stream.num_buffers = s.buffer_count;

	while ((ret = poll(fds, 2, 5000)) > 0) {
		if (fds[0].revents & POLLIN) {
			struct buffer *buffer;

			buffer = v4l2_dequeue_buffer(&v4l2, buffers);
			drm_page_flip(&drm, buffer);
		}

		if (fds[1].revents & POLLIN) {
			drmEventContext evctx;
			memset(&evctx, 0, sizeof evctx);
			evctx.version = DRM_EVENT_CONTEXT_VERSION;
			evctx.page_flip_handler = page_flip_handler;

			ret = drmHandleEvent(drm.fd, &evctx);
			BYE_ON(ret, "drmHandleEvent failed: %s\n", ERRSTR);
		}
	}

	return 0;
}

