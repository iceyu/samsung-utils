/*
 * Samsung S5P FIMC video postprocessor test application.
 * Author: Sylwester Nawrocki, <s.nawrocki@samsung.com>
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>

#include <linux/fb.h>
#include <linux/videodev2.h>

#define _GNU_SOURCE
#include <sched.h>

#define VIDEO_DEV_NAME	"/dev/video"
#define FB_DEV_NAME	"/dev/fb0"

#define NUM_SRC_BUFS	1
#define NUM_DST_BUFS	1

#define BLOCKING_MODE	1

#define perror_exit(cond, func)\
	if (cond) {\
		fprintf(stderr, "%s:%d: ", __func__, __LINE__);\
		perror(func);\
		exit(EXIT_FAILURE);\
	}

#define error_exit(cond, msg)\
	if (cond) {\
		fprintf(stderr, "%s:%d: " msg "\n", __func__, __LINE__);\
		exit(EXIT_FAILURE);\
	}

#define perror_ret(cond, func)\
	if (cond) {\
		fprintf(stderr, "%s:%d: ", __func__, __LINE__);\
		perror(func);\
		return ret;\
	}

#define memzero(x)\
	memset(&(x), 0, sizeof (x));

#define error(msg)	fprintf(stderr, "%s:%d: " msg "\n", __func__, __LINE__);

#define _DEBUG
#ifdef _DEBUG
#define debug(msg, ...)\
	fprintf(stderr, "%s: " msg, __func__, ##__VA_ARGS__);
#else
#define debug(msg, ...)
#endif

#define PAGE_ALIGN(addr)	(((addr) + page_size - 1) & ~(page_size -1))

enum format {
	FMT_420,
	FMT_422,
	FMT_565,
	FMT_888
};

struct buffer {
	char 		*addr[VIDEO_MAX_PLANES];
	unsigned long 	size[VIDEO_MAX_PLANES];
	int 		num_planes;
	unsigned int 	index;
	unsigned int	width;
	unsigned int	height;
};

static int thread_id = 0;
static int vid_fd, fb_fd, src_fd, dst_fd;
static void *fb_addr, *fb_alloc_ptr, *src_addr/* , *dst_addr */;
static char *in_file/* , *out_file */;
static int width, height;
static int out_width, out_height;
static off_t fb_line_w, fb_size, fb_pix_dist;
static int vid_node;
static int rotation, flip;
static enum format format;
static long page_size;

static struct fb_var_screeninfo g_fbinfo;

void sleep_ms(unsigned long time)
{
	struct timespec ts;
	ts.tv_sec = time/1000;
	ts.tv_nsec = (unsigned long)(time%1000)*1000000UL;
	nanosleep(&ts, NULL);
}

static void set_rotation(int angle)
{
	struct v4l2_control ctrl;
	int ret;

	memzero(ctrl);

	ctrl.id = V4L2_CID_ROTATE;
	ctrl.value = (angle==360) ? 0 : angle;
	//debug("ROTATION: %d\n", angle);

	ret = ioctl(vid_fd, VIDIOC_S_CTRL, &ctrl);
	perror_exit(ret != 0, "VIDIOC_S_CTRL ioctl");
}

static void set_flip(int flip)
{
	struct v4l2_control ctrl;
	int ret;

	memzero(ctrl);
	switch (flip) {
	case 1:
		ctrl.id = V4L2_CID_HFLIP;
		break;
	case 2:
		ctrl.id = V4L2_CID_VFLIP;
		break;
	default:
		error_exit(1, "Invalid params\n");
	}

	ctrl.value = 1;

	ret = ioctl(vid_fd, VIDIOC_S_CTRL, &ctrl);
	perror_exit(ret != 0, "ioctl");
}

static void set_src_fmt(enum format format, unsigned int *framesize,
			unsigned int *num_planes)
{
	struct v4l2_format fmt;
	int ret;
	struct v4l2_pix_format_mplane *pix_mp;
	int i;

	if(NULL == framesize || NULL == num_planes)
		return;

	memzero(fmt);
	pix_mp = &fmt.fmt.pix_mp;

	switch (format) {
	case FMT_420:
		pix_mp->pixelformat = V4L2_PIX_FMT_YUV420;
		break;
	case FMT_422:
		pix_mp->pixelformat = V4L2_PIX_FMT_YUYV;
		break;
	case FMT_565:
		pix_mp->pixelformat = V4L2_PIX_FMT_RGB565X;
		break;
	case FMT_888:
		pix_mp->pixelformat = V4L2_PIX_FMT_RGB32;
		break;
	default:
		error_exit(1, "Invalid params\n");
		break;
	}

	/* The same format for output */
	fmt.type 	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	pix_mp->width	= width;
	pix_mp->height	= height;
	pix_mp->field	= V4L2_FIELD_ANY;

	ret = ioctl(vid_fd, VIDIOC_S_FMT, &fmt);
	perror_exit(ret != 0, "ioctl");

	*num_planes = pix_mp->num_planes;
	*framesize = 0;

	for (i = 0; i < *num_planes; i++) {
		debug("i= %d\n", i);
		*framesize += pix_mp->plane_fmt[i].sizeimage;
		debug("plane[%d]: bytesperline: %d, sizeimage: %d\n",
		      i,
		      pix_mp->plane_fmt[i].bytesperline,
		      pix_mp->plane_fmt[i].sizeimage);
	}

	debug("SRC framesize: %u\n", *framesize);

	ret = ioctl(vid_fd, VIDIOC_G_FMT, &fmt);
	perror_exit(ret != 0, "ioctl");

	width  = pix_mp->width;
	height = pix_mp->height;
	debug("width: %d, height: %d\n", width, height)
}



static void set_dst_fmt(enum format format, unsigned int *framesize,
			int *num_planes)
{
	struct v4l2_format fmt;
	int i, ret;
	struct v4l2_pix_format_mplane *pix_mp;

	if(NULL == framesize || NULL == num_planes)
		return;

	debug("out_width: %d, out_height: %d\n", out_width, out_height);

	pix_mp = &fmt.fmt.pix_mp;

	memzero(fmt);
	switch (format) {
	case FMT_420:
		pix_mp->pixelformat = V4L2_PIX_FMT_YUV420;
		break;
	case FMT_422:
		pix_mp->pixelformat = V4L2_PIX_FMT_YUYV;
		break;
	case FMT_565:
		pix_mp->pixelformat = V4L2_PIX_FMT_RGB565X;
		break;
	case FMT_888:
		pix_mp->pixelformat = V4L2_PIX_FMT_RGB32;
		break;
	default:
		error_exit(1, "Invalid params\n");
		break;
	}

	/* Set format for capture */
	fmt.type		= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	pix_mp->width		= out_width;
	pix_mp->height		= out_height;
	pix_mp->field		= V4L2_FIELD_ANY;

	pix_mp->plane_fmt[0].bytesperline = fb_line_w;

	ret = ioctl(vid_fd, VIDIOC_S_FMT, &fmt);

	*num_planes = pix_mp->num_planes;
	*framesize = 0;

	for (i = 0; i < *num_planes; i++) {
		debug("i= %d\n", i);
		*framesize += pix_mp->plane_fmt[i].sizeimage;
		debug("plane[%d]: bytesperline: %d, sizeimage: %d\n",
		      i,
		      pix_mp->plane_fmt[i].bytesperline,
		      pix_mp->plane_fmt[i].sizeimage);
	}

	/* update any values which might have been changed by the driver */
	out_width  = pix_mp->width;
	out_height = pix_mp->height;

	debug("DST framesize: %u\n", *framesize);

	perror_exit(ret != 0, "ioctl");
}


static void verify_caps(void)
{
	struct v4l2_capability cap;
	int ret;

	memzero(cap);
	ret = ioctl(vid_fd, VIDIOC_QUERYCAP, &cap);
	perror_exit(ret != 0, "ioctl");

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
		error_exit(1, "Device does not support capture\n");

	if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE))
		error_exit(1, "Device does not support output\n");

	if (!(cap.capabilities & V4L2_CAP_STREAMING))
		error_exit(1, "Device does not support streaming\n");
}

static void init_video_dev(void)
{
	char devname[64];

	snprintf(devname, 64, "%s%d", VIDEO_DEV_NAME, vid_node);
#ifdef BLOCKING_MODE
	vid_fd = open(devname, O_RDWR, 0);
#else
	vid_fd = open(devname, O_RDWR | O_NONBLOCK, 0);
#endif
	perror_exit(vid_fd < 0, "open");

	verify_caps();
}

static void init_fb(struct fb_var_screeninfo *fbinfo)
{
	int ret;

	perror_exit (fbinfo == NULL, "fbinfo is NULL!\n")

	fb_fd = open(FB_DEV_NAME, O_RDWR);
	perror_exit(fb_fd < 0, "open");

	ret = ioctl(fb_fd, FBIOGET_VSCREENINFO, fbinfo);
	perror_exit(ret != 0, "ioctl");
	debug("fbinfo: xres: %d, xres_virt: %d, yres: %d, yres_virt: %d\n",
		fbinfo->xres, fbinfo->xres_virtual,
		fbinfo->yres, fbinfo->yres_virtual);

	fb_pix_dist = fbinfo->bits_per_pixel/8;
	fb_line_w = fbinfo->xres_virtual * fb_pix_dist;
	debug("fb_line_w: %ld\n", fb_line_w);
	fb_size = fb_line_w * fbinfo->yres_virtual;

	fb_addr = fb_alloc_ptr = mmap(0, fb_size, PROT_WRITE | PROT_READ,
				      MAP_SHARED, fb_fd, 0);
	perror_exit(fb_addr == MAP_FAILED, "mmap");

	out_width = fbinfo->xres;
	out_height = (1 * fbinfo->yres)/2;
}


void print_usage(void) {
	fprintf (stderr, "Usage:\n"
			 "-d[VIDEO NODE NUMBER]\n"
			 "-i[INPUT FILE]\n"
			 "-f[COLOUR FORMAT: 1, 2..4]\n"
			 "-g[INPUT_IMG_WIDTHxINPUT_IMG_HEIGHT]\n"
			 "-p[THREAD_ID] (0..1)\n"
		 );
}

static void parse_args(int argc, char *argv[])
{
	int index;
	int c;

	opterr = 0;
	while ((c = getopt(argc, argv, "d:i:f:g:p:")) != -1) {
		switch (c) {
		case 'd': vid_node	= atoi(optarg);			break;
		case 'i': in_file	= optarg;			break;
		case 'f': format	= atoi(optarg); 		break;
		case 'g': sscanf(optarg, "%dx%d", &width, &height);	break;
		case 'p': thread_id	= atoi(optarg); 		break;
		case '?':
			if (optopt == 'd') {
				fprintf (stderr, "Option -%c requires an argument"
						 "(video device node number).\n", optopt);
			} else if (isprint (optopt)) {
				fprintf (stderr, "Unknown option `-%c'.\n",
					 optopt);
				print_usage();
			} else {
				fprintf (stderr,
					"Unknown option character `\\x%x'.\n",
					optopt);
			}
			return;
		default:
			abort ();
		}
	}

	printf ("vid_node: %d, in_file: %s, format: %d, wxh: %dx%d, thread_id: %d\n",
		vid_node, in_file, format, width, height, thread_id);

	for (index = optind; index < argc; index++)
		printf ("Non-option argument %s\n", argv[index]);
}

static void get_buffer(struct buffer *buf)
{
	buf->addr[0] = (void*)PAGE_ALIGN((long int)fb_alloc_ptr);
	buf->size[0] = PAGE_ALIGN(buf->size[0]);
	fb_alloc_ptr += buf->size[0];

	if ((unsigned int)fb_alloc_ptr > (unsigned int) fb_addr + fb_size)
		error_exit(1, "Out of fb memory\n");
}

static void request_dst_buffers(unsigned int *num_bufs)
{
	struct v4l2_requestbuffers reqbuf;
	int ret;

	memzero(reqbuf);
	reqbuf.count	= *num_bufs;
	reqbuf.type 	= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory	= V4L2_MEMORY_USERPTR;

	ret = ioctl(vid_fd, VIDIOC_REQBUFS, &reqbuf);
	perror_exit(ret != 0, "ioctl");
	*num_bufs = reqbuf.count;
}



static int process(struct buffer *src_buf, struct buffer *dst_buf)
{
	struct v4l2_plane src_planes[VIDEO_MAX_PLANES];
	struct v4l2_plane dst_planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer src_vbuf, dst_vbuf;
	enum v4l2_buf_type type;
	int ret, i;
	struct timeval start, end;
	int num_frames = 0;

	/* Prepare source and destination buffers. */
	memzero(src_vbuf);

	src_vbuf.type		= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vbuf.memory		= V4L2_MEMORY_MMAP;
	src_vbuf.index		= src_buf->index;
#if 0
	for (i = 0; i < src_buf->num_planes; i++) {
		src_planes[i].m.userptr = (unsigned long)src_buf->addr[i];
		src_planes[i].length = src_buf->size[i];
		/* memset(src_planes[i].m.userptr, 0x60, planes[i].length); */
	}
#endif
	src_vbuf.m.planes 	= src_planes;
	src_vbuf.length		= src_buf->num_planes;

	memzero(dst_vbuf);

	dst_vbuf.type		= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vbuf.memory		= V4L2_MEMORY_USERPTR;
	dst_vbuf.index		= dst_buf->index;

	for (i = 0; i < dst_buf->num_planes; i++) {
		dst_planes[i].m.userptr = (unsigned long)dst_buf->addr[i];
		dst_planes[i].length = dst_buf->size[i];
		/* memset(src_planes[i].m.userptr, 0x60, planes[i].length); */
	}

	dst_vbuf.m.planes 	= dst_planes;
	dst_vbuf.length		= dst_buf->num_planes;

	gettimeofday(&start, NULL);

	while (num_frames++ < 3000) {
		ret = ioctl(vid_fd, VIDIOC_QBUF, &src_vbuf);
		perror_ret(ret, "QBUF src ioctl");

		ret  = ioctl(vid_fd, VIDIOC_QBUF, &dst_vbuf);
		perror_ret(ret, "QBUF dst ioctl");

		if (num_frames == 1) {
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			ret = ioctl(vid_fd, VIDIOC_STREAMON, &type);
			perror_ret(ret, "STREAMON CAPTURE ioctl");

			type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			ret  = ioctl(vid_fd, VIDIOC_STREAMON, &type);
			perror_ret(ret, "STREAMON OUTPUT ioctl");
		}

#ifndef BLOCKING_MODE
		test_fd.fd = vid_fd;
		test_fd.events = POLLOUT | POLLERR;
		ret = poll(&test_fd, 1, 2000);
		perror_ret(-1 == ret, "POLL ioctl");
#endif
		ret  = ioctl(vid_fd, VIDIOC_DQBUF, &dst_vbuf);
		perror_ret(ret, "DQBUF dst ioctl");

		ret = ioctl(vid_fd, VIDIOC_DQBUF, &src_vbuf);
		perror_ret(ret, "DQBUF src ioctl");

		static int rotation;
		static int delay = 1;
		if (++rotation > 3)
			rotation = 0;
		set_rotation(rotation * 90);
		//debug("ROTATION: %d\n", rotation * 90);
		if (delay > 1)
			delay -= 1;
		else
			delay = 10;
		//sleep_ms(delay);

	}

	gettimeofday(&end, NULL);

	debug("%d frames processed.\n", num_frames);
	double t1 = start.tv_sec * 1000 + start.tv_usec / 1000;
	double t2 = end.tv_sec * 1000 + end.tv_usec / 1000;
	printf("%.1f frames per second\n",
	       (double)(num_frames * 1000)/(double)(t2 - t1));

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret  = ioctl(vid_fd, VIDIOC_STREAMOFF, &type);
	perror_ret(ret, "STREAMOFF OUTPUT ioctl");

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret  = ioctl(vid_fd, VIDIOC_STREAMOFF, &type);
	perror_ret(ret, "STREAMOFF CAPTURE ioctl");

	return 0;
}

#if 0
static void display(struct buffer *buf,
		    int start_x, int start_y, int width, int height)
{
	char *p_buf, *p_fb;
	int curr_y;

	p_fb = fb_addr + start_y * fb_line_w + start_x * fb_pix_dist;
	p_buf = buf->addr[0];
	unsigned int bytesperline = width * fb_pix_dist;
	for (curr_y = 0; curr_y < height; curr_y++) {
		memcpy(p_fb, p_buf, bytesperline);
		p_fb += fb_line_w;
		p_buf += bytesperline;
	}

}
#endif

static void m2m_prepare_src_buffers(int fd, unsigned w, unsigned h, struct buffer *src_buffers,
				    int *req_buf_count, int num_planes)
{
	struct v4l2_requestbuffers req;
	int index, i;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer buf;

	memzero(req);

	req.count	= *req_buf_count;
	req.type	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req.memory	= V4L2_MEMORY_MMAP;

	if (-1 == ioctl (fd, VIDIOC_REQBUFS, &req)) {
		if (EINVAL == errno) {
			error("REQBUFS failed. No support for memory mapping?\n");
			exit (EXIT_FAILURE);
		} else {
			perror_exit(1, "VIDIOC_REQBUFS");
		}
	}

	if (req.count < 1) {
		error("Insufficient buffer memory\n");
		exit (EXIT_FAILURE);
	}

	*req_buf_count = req.count;

	for (index = 0; index < req.count; ++index) {

		memzero(buf);

		buf.type 	= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                buf.m.planes 	= planes;
                buf.length 	= num_planes; /* number of planes */
		buf.memory	= V4L2_MEMORY_MMAP;
		buf.index	= index;


		if (-1 == ioctl (fd, VIDIOC_QUERYBUF, &buf))
			perror_exit (1, "VIDIOC_QUERYBUF");

		src_buffers[index].num_planes = num_planes;

		for (i = 0; i < num_planes; i++) {
			debug("QUERYBUF: plane [%d]: length: %d, bytesused: %d, offset: %d\n",
			      i,
			      buf.m.planes[i].length,
			      buf.m.planes[i].bytesused,
			      buf.m.planes[i].m.mem_offset);

			src_buffers[index].size[i] = buf.m.planes[i].length;
			src_buffers[index].addr[i] =
				mmap (NULL /* start anywhere */,
				      buf.m.planes[i].length,
				      PROT_READ | PROT_WRITE /* required */,
				      MAP_SHARED /* recommended */,
				      fd, buf.m.planes[i].m.mem_offset);

			if (MAP_FAILED == src_buffers[index].addr[i])
				perror_exit(1, "mmap");

			src_buffers[index].index	= index;
			src_buffers[index].width	= w;
			src_buffers[index].height	= h;

			memset(src_buffers[index].addr[i], 0, src_buffers[index].size[i]);

			debug("mmaped: buf[%d], plane[%d] size: %ld, addr: %p\n",
			      index, i,
			      src_buffers[index].size[i], src_buffers[index].addr[i]);
		}
	}
}


unsigned long mask;


int main(int argc, char *argv[])
{
	struct stat in_stat;
	struct buffer src_buffers[2];
	struct buffer dst_buffers[2];
	unsigned int num_src_buffers = NUM_SRC_BUFS;
	unsigned int num_dst_buffers = NUM_DST_BUFS;
	unsigned int num_src_planes, num_dst_planes;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	unsigned int src_framesize, dst_framesize;
	int in_size;
	int ret = 0;

	struct timeval tv;
	gettimeofday(&tv, NULL);
	srandom(getpid() + tv.tv_sec);

	parse_args(argc, argv);
	page_size = sysconf(_SC_PAGESIZE);


	memset(&mask, 0, sizeof(mask));
	//CPU_SET(thread_id, &mask);
	mask = 1 << thread_id;

	if (sched_setaffinity(0, sizeof(mask), &mask ) == -1)
		printf("WARNING: Could not set CPU Affinity, continuing...\n");


	if (sched_setaffinity(0, sizeof(mask), &mask ) != -1)
		debug("CPU: %d\n", ffs(mask) - 1);

	src_fd = open(in_file, O_RDONLY);
	perror_exit(src_fd < 0, in_file);
	fstat(src_fd, &in_stat);
	in_size = in_stat.st_size;
	src_addr = mmap(0, in_size, PROT_READ, MAP_SHARED, src_fd, 0);
	perror_exit(src_addr == MAP_FAILED, "mmap");

	init_fb(&g_fbinfo);

	init_video_dev();

	debug("in_size: %d\n", in_size);

	set_src_fmt(format, &src_framesize, &num_src_planes);
	set_dst_fmt(FMT_888, &dst_framesize, &num_dst_planes);

	if (rotation >= 0)
		set_rotation(rotation);

	if(flip > 0)
		set_flip(flip);

	request_dst_buffers(&num_dst_buffers);

	get_buffer(&dst_buffers[0]);

	dst_buffers[0].size[0]		= dst_framesize;
	dst_buffers[0].index		= 0;
	dst_buffers[0].num_planes	= 1;

	if (thread_id == 1) {

		int fb_line_w = g_fbinfo.xres_virtual * g_fbinfo.bits_per_pixel/8;

		dst_buffers[0].addr[0] += (fb_line_w * g_fbinfo.yres/2);
	}

	m2m_prepare_src_buffers(vid_fd, width, height,
				src_buffers, &num_src_buffers, 1);

	debug("src_buffers: %d, dst_buffers: %d\n",
	      num_src_buffers, num_dst_buffers);

	int index;
	for (index = 0; index < 1; index++) {
		int i = 0;
		debug("mmaped: buf[%d], plane[%d] size: %ld, addr: %p\n",
		      index, i,
		      src_buffers[index].size[i], src_buffers[index].addr[i]);
	}

	//memset(src_buffers[0].addr[0], 0xF0, src_framesize);
	memcpy(src_buffers[0].addr[0], src_addr, src_framesize);

	/* get capture cropping parameters */
	memset (&cropcap, 0, sizeof (cropcap));
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (-1 == ioctl(vid_fd, VIDIOC_CROPCAP, &cropcap)) {
		perror ("VIDIOC_CROPCAP");
		close(dst_fd);
		exit (EXIT_FAILURE);
	}

	debug("BOUNDS: l: %d  t: %d  w: %d  h: %d\n",
		cropcap.bounds.left, cropcap.bounds.top,
		cropcap.bounds.width, cropcap.bounds.height);

	debug("DEFRECT: l: %d  t: %d  w: %d  h: %d\n",
		cropcap.defrect.left, cropcap.defrect.top,
		cropcap.defrect.width, cropcap.defrect.height);

	debug("PIXELASPECT: n: %d  d: %d\n",
		cropcap.pixelaspect.denominator, cropcap.pixelaspect.numerator);

	/* set capture cropping rectangle */
	memset (&crop, 0, sizeof (crop));
	crop.type 	= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	crop.c 		= cropcap.defrect;
	crop.c.width 	= (rand() >> 16) % cropcap.defrect.width;
	crop.c.height 	= (rand() >> 16) % cropcap.defrect.height;

	crop.c.left	= (rand() >> 16)%(cropcap.defrect.width - crop.c.width);
	crop.c.top	= (rand() >> 16)%(cropcap.defrect.height - crop.c.height);
	//crop.c.left 	= (cropcap.defrect.width/4);
	//crop.c.top 	= (cropcap.defrect.height/4);

	if (-1 == ioctl (vid_fd, VIDIOC_S_CROP, &crop) && errno != EINVAL) {
		perror ("VIDIOC_S_CROP");
		exit (EXIT_FAILURE);
	}

	if (-1 == ioctl (vid_fd, VIDIOC_G_CROP, &crop) && errno != EINVAL) {
		perror ("VIDIOC_G_CROP");
		exit (EXIT_FAILURE);
	}
	debug("CROPPING WINDOW: l: %d  t: %d  w: %d  h: %d\n",
		crop.c.left, crop.c.top, crop.c.width, crop.c.height);


	ret = process(&src_buffers[0], &dst_buffers[0]);
	if(ret)
		return ret;

#if 0
	/* Write the result to the output file. */
	dst_fd = open(out_file, O_RDWR | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
	perror_exit(dst_fd < 0, out_file);

	ftruncate(dst_fd, dst_framesize);

	dst_addr = mmap(0, dst_framesize, PROT_WRITE, MAP_SHARED, dst_fd, 0);
	perror_exit(dst_addr == MAP_FAILED, "mmap");

	memcpy(dst_addr, dst_buffers[0].addr[0], dst_framesize);
#endif
	close(dst_fd);
	close(vid_fd);
	return 0;
}

