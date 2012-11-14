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

#define LOG(...) fprintf(stdout, __VA_ARGS__)

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
	bool help;
};

struct state {
	struct config config;
	int vivi_fd;
	int fimc0_fd;
	int fimc1_fd;
};

static void usage(void)
{
#define HELP(...) fprintf(stderr, __VA_ARGS__)
	HELP("Usage:\n");
	HELP("\t-v path    path to vivi [default /dev/video0]\n");
	HELP("\t-0 path    path to fimc0 [default /dev/video1]\n");
	HELP("\t-1 path    path to fimc1 [default /dev/video3]\n");
	HELP("\t-V         vertical flip\n");
	HELP("\t-H         horizontal flip\n");
	HELP("\t-R angle   rotation by angle [default 0]\n");
	HELP("\t-h         print this help\n");
	HELP("\n");
#undef HELP
}

static int config_create(struct config *config, int argc, char *argv[])
{
	int opt, ret = -EINVAL;

	CLEAR(*config);
	strcpy(config->vivi_path, "/dev/video0");
	strcpy(config->fimc0_path, "/dev/video1");
	strcpy(config->fimc1_path, "/dev/video3");

	/* parse options */
	while ((opt = getopt(argc, argv, ":v:0:1:VHR:h")) != -1) {
		switch (opt) {
		case 'v':
			strcpy(config->vivi_path, optarg);
			break;
		case '0':
			strcpy(config->fimc0_path, optarg);
			break;
		case '1':
			strcpy(config->fimc1_path, optarg);
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
		case 'h':
			config->help = true;
			break;
		case ':':
			ERR("missing argument\n");
			return -EINVAL;
		default: /* '?' */
			ERR("invalid option\n");
			return -EINVAL;
		}
	}

	return 0;
}

void dump_format(char *str, struct v4l2_format *fmt)
{
	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type)) {
		struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
		LOG("%s: width=%u height=%u format=%.4s\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat);
	} else {
		struct v4l2_pix_format *pix = &fmt->fmt.pix;
		LOG("%s: width=%u height=%u format=%.4s\n", str,
			pix->width, pix->height, (char*)&pix->pixelformat);
	}
}

int dump_yuyv(int fd, uint8_t *src, int w, int h)
{
	uint8_t buf[512], *dst = buf;
	for (int wh = w * h; wh; wh--) {
		int y0, y1, u, v;
		int r, g, b;
		y0 = *src++;
		u  = *src++;
		y1 = *src++;
		v  = *src++;
		u -= 128;
		v -= 128;
		g = MAX(y0 - (u + v) / 4, 0);
		r = MIN(u + g, 0xff);
		b = MIN(v + g, 0xff);
		*dst++ = r;
		*dst++ = g;
		*dst++ = b;
		g = MAX(y1 - (u + v) / 4, 0);
		r = MIN(u + g, 0xff);
		b = MIN(v + g, 0xff);
		*dst++ = r;
		*dst++ = g;
		*dst++ = b;
		if (dst - buf < ARRAY_SIZE(buf) - 6)
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
	default:
		ERR("format %.4s not supported\n", (char*)&fourcc);
		ret = -EINVAL;
	}
	close(fd);
	ERR_ON(ret, "failed to dump %s\n", name);
	return ret;
}

int setup_formats(struct state *st)
{
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	/* get default format from VIVI */
	int ret = ioctl(st->vivi_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;
	dump_format("vivi", &fmt);

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

	/* set the same format on fimc0 capture */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
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

	/* and the same at FIMC1 output */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = 32;
	fmt.fmt.pix_mp.height = 32;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
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
	rb.memory = V4L2_MEMORY_DMABUF;
	ret = ioctl(st->fimc0_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	/* request buffers for FIMC1 */
	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(st->fimc1_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

	rb.count = 1;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_USERPTR;
	ret = ioctl(st->fimc1_fd, VIDIOC_REQBUFS, &rb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_REQBUFS: %s\n", ERRSTR))
		return -errno;

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

	/* TODO: mmap DMABUF and dump result */
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(st->vivi_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "vivi: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;

	/* mapping DMABUF for dumping */
	void *ptr = mmap(NULL, fmt.fmt.pix.sizeimage, PROT_READ,
		MAP_SHARED, eb.fd, 0);
	if (ERR_ON(ptr == MAP_FAILED, "mmap: %s\n", ERRSTR))
		return -errno;

	/* dump image, ignore errors */
	dump_image("dmabuf-vivi-fimc0.ppm", fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.width, fmt.fmt.pix.height, ptr);

	/* small cleanup */
	munmap(ptr, fmt.fmt.pix.sizeimage);
	close(eb.fd);

	return 0;
}

int process_fimc0(struct state *st)
{
	int ret;
	struct v4l2_buffer b;
	struct v4l2_plane plane;
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

	/* export the first buffer from FIMC1 */
	struct v4l2_exportbuffer eb;
	CLEAR(eb);
	eb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_EXPBUF, &eb);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_EXPBUF: %s\n", ERRSTR))
		return -errno;

	/* enqueue the DMABUF as FIMC0's cature */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_DMABUF;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;
	plane.m.fd = eb.fd;

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

	/* grab processed buffers */
	ret = ioctl(st->fimc0_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc0: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;

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

	return 0;
}

int process_fimc1(struct state *st)
{
	int ret;
	/* enqueue buffer 0 as FIMC1's output */
	struct v4l2_buffer b;
	struct v4l2_plane plane;
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;

	ret = ioctl(st->fimc1_fd, VIDIOC_QBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_QBUF: %s\n", ERRSTR))
		return -errno;

	/* allocate malloc memory suitable for FIMC1 */
	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ret = ioctl(st->fimc1_fd, VIDIOC_G_FMT, &fmt);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_G_FMT: %s\n", ERRSTR))
		return -errno;

	long page_size = sysconf(_SC_PAGESIZE);
	if (ERR_ON(page_size == -1, "sysconf: %s\n", ERRSTR))
		return -errno;

	/* someone should be shot because of the layout of v4l2_format */
	size_t size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	/* align to page size */
	size_t size_align = (size + page_size - 1) & ~(page_size - 1);

	void *ptr;
	ret = posix_memalign(&ptr, page_size, size_align);
	if (ERR_ON(ret, "posix_memalign: %s\n", ERRSTR))
		return -errno;

	/* enqueue userptr as FIMC1's capture */
	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_USERPTR;
	b.index = 0;
	b.m.planes = &plane;
	b.length = 1;
	plane.m.userptr = (unsigned long)ptr;
	plane.length = size_align;

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

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_USERPTR;
	b.m.planes = &plane;

	/* grab processed buffers */
	ret = ioctl(st->fimc1_fd, VIDIOC_DQBUF, &b);
	if (ERR_ON(ret < 0, "fimc1: VIDIOC_DQBUF: %s\n", ERRSTR))
		return -errno;

	CLEAR(plane);
	CLEAR(b);
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = &plane;

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

	/* dump image, ignore errors */
	dump_image("fimc1-cature.ppm", fmt.fmt.pix_mp.pixelformat,
		fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
		ptr);

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

	return EXIT_SUCCESS;
}
