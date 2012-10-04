#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

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

#define BUFFER_CNT	3

struct buffer {
	int index;
	void *data;
	size_t size;
	size_t width;
	size_t height;

	/* buffer state */
	double t;
};

struct setup {
	char *path;
	int width;
	int height;
	int xoffset;
	int yoffset;
};

struct context {
	int fd;
	struct buffer *buffer;
	size_t buffer_cnt;
};

int parse_args(struct setup *setup, int argc, char *argv[]);

int showtime(struct context *ctx);

int main(int argc, char *argv[])
{
	int ret;

	struct setup setup;
	ret = parse_args(&setup, argc, argv);
	BYE_ON(ret, "syntax error\n");

	int fd;
	fd = open(argv[1], O_RDWR);
	BYE_ON(fd < 0, "open failed: %s\n", ERRSTR);

	/* configure desired image size */
	struct v4l2_format fmt;
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix.width = setup.width;
	fmt.fmt.pix.height = setup.height;
	/* format is hardcoded: draw procedures work only in 32-bit mode */
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR32;

	ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
	BYE_ON(ret < 0, "VIDIOC_S_FMT failed: %s\n", ERRSTR);

	/* update format struct to values adjusted by a driver */
	ret = ioctl(fd, VIDIOC_G_FMT, &fmt);
	BYE_ON(ret < 0, "VIDIOC_G_FMT failed: %s\n", ERRSTR);

	/* allocate buffers */
	struct v4l2_requestbuffers rqbufs;
	rqbufs.count = BUFFER_CNT;
	rqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rqbufs.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(fd, VIDIOC_REQBUFS, &rqbufs);
	BYE_ON(ret < 0, "VIDIOC_REQBUFS failed: %s\n", ERRSTR);
	BYE_ON(rqbufs.count < BUFFER_CNT, "failed to get %d buffers\n",
		BUFFER_CNT);

	struct buffer buffer[BUFFER_CNT];
	/* size_t size = 4 * setup.width * setup.height; */

	/* buffers initalization */
	for (int i = 0; i < BUFFER_CNT; ++i) {
		struct v4l2_plane plane;
		struct v4l2_buffer buf;
		buf.index = i;
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = &plane;
		buf.length = 1;
		/* get buffer properties from a driver */
		ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
		BYE_ON(ret < 0, "VIDIOC_QUERYBUF for buffer %d failed: %s\n",
			buf.index, ERRSTR);

		buffer[i].index = i;
		/* mmap buffer to user space */
		buffer[i].data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, plane.m.mem_offset);
		BYE_ON(buffer[i].data == MAP_FAILED, "mmap failed: %s\n",
			ERRSTR);
		buffer[i].size = plane.length;
		buffer[i].width = fmt.fmt.pix.width;
		buffer[i].height = fmt.fmt.pix.height;
		/* fill buffer with black */
		for (size_t j = 0; 4 * j < buffer[i].size; ++j)
			((uint32_t*)buffer[i].data)[j] = 0xff000000;
	}

	/* crop output area on display */
	struct v4l2_crop crop;
	crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	crop.c.left = setup.xoffset;
	crop.c.top = setup.yoffset;
	crop.c.width = setup.width;
	crop.c.height = setup.height;
	ret = ioctl(fd, VIDIOC_S_CROP, &crop);
	BYE_ON(ret < 0, "VIDIOC_S_CROP failed: %s\n", ERRSTR);

	struct context context;
	context.fd = fd;
	context.buffer = buffer;
	context.buffer_cnt = BUFFER_CNT;
	/* It's show-time !!! */
	showtime(&context);

	return 0;
}

int parse_args(struct setup *setup, int argc, char *argv[])
{
	setup->path = argv[1];
	setup->width = atoi(argv[2]);
	setup->height = atoi(argv[3]);
	setup->xoffset = atoi(argv[4]);
	setup->yoffset = atoi(argv[5]);
	return 0;
}

double gettime(void);
void buffer_fill(struct buffer *buf, double t);
int queue(int fd, int index);
int dequeue(int fd, int *index);

int showtime(struct context *ctx)
{
	double t;

	/* fill and pass all buffers to the driver */
	for (int i = 0; i < ctx->buffer_cnt; ++i) {
		t = gettime();
		buffer_fill(&ctx->buffer[i], t);
		queue(ctx->fd, i);
	}
	fprintf(stderr, "start\n");
	/* start streaming */
	int ret, type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
	BYE_ON(ret, "VIDIOC_STREAMON failed: %s\n", ERRSTR);

	int index;
	/* main loop, every dequeued buffer is refilled and enqueued again */
	while (dequeue(ctx->fd, &index) == 0) {
		t = gettime();
		buffer_fill(&ctx->buffer[index], t);
		queue(ctx->fd, index);
	}
	return 0;
}

double gettime(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	double t = tv.tv_sec + 1e-6 * tv.tv_usec;
	return t;
}

void draw_rect(struct buffer *buf, struct v4l2_rect *r, uint32_t v)
{
	uint32_t *ptr = buf->data;
	ptr += buf->width * r->top + r->left;
	for (int i = 0; i < r->height; ++i) {
		for (int j = 0; j < r->width; ++j)
			ptr[j] = v;
		ptr += buf->width;
	}
}

void prepare_rect(struct buffer *buf, struct v4l2_rect *r, double t)
{
	r->left = 0.8 * buf->width * (0.5 + 0.5 * sin(t));
	r->top = 0.8 * buf->height * (0.5 + 0.5 * sin(1.41 * t));
	r->width = 0.2 * buf->width;
	r->height = 0.2 * buf->height;
}

void buffer_fill(struct buffer *buf, double t)
{
	struct v4l2_rect r;
	prepare_rect(buf, &r, buf->t);
	draw_rect(buf, &r, 0xff000000);
	prepare_rect(buf, &r, t);
	int R, G, B;
	R = 125 + 125 * sin(3 * t);
	G = 125 + 125 * sin(4 * t);
	B = 125 + 125 * sin(5 * t);
	draw_rect(buf, &r, (R << 0) | (G << 8) | (B << 16) | (0xff << 24));
	buf->t = t;
}

int queue(int fd, int index)
{
	struct v4l2_buffer buf;
	struct v4l2_plane plane;
	memset(&buf, 0, sizeof buf);
	memset(&plane, 0, sizeof plane);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.index = index;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = &plane;
	buf.length = 1;
	int ret;
	ret = ioctl(fd, VIDIOC_QBUF, &buf);
	BYE_ON(ret, "VIDIOC_QBUF(index = %d) failed: %s\n",
		index, ERRSTR);
	return 0;
}

int dequeue(int fd, int *index)
{
	int ret;
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(fd, VIDIOC_DQBUF, &buf);
	BYE_ON(ret, "VIDIOC_DQBUF failed: %s\n", ERRSTR);
	*index = buf.index;
	return 0;
}
