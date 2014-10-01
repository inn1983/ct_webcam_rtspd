#define LOG_NDEBUG 0
#define LOG_TAG "venc-file"
#include "CDX_Debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
//#include <linux/videodev2.h>
#include <time.h>
#include <errno.h>

#include <deque>
#include <cc++/thread.h>

#ifdef ZQSENDER
#  include "zqsender.h"
#  include "zq_atom_types.h"
#  include "zq_atom_pkt.h"
#endif 

#include "type.h"
#include "H264encLibApi.h"
#include "V4L2.h"
#include "venc.h"
#include "CameraSource.h"
#include "water_mark.h"
#include "cedarv_osal_linux.h"

#include "capture_slice.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))

unsigned int mwidth = 720;
unsigned int mheight = 480;

typedef struct Venc_context
{
	cedarv_encoder_t *venc_device;
	VencBaseConfig base_cfg;
	VencInputBuffer input_buffer;
	VencOutputBuffer output_buffer;
	VencAllocateBufferParam alloc_parm;
	VencSeqHeader header_data;
	AWCameraDevice *CameraDevice;	
	pthread_t thread_enc_id;
	int mstart;
	FILE *out_file;
	WaterMark*	waterMark;
}Venc_context;

Venc_context *g_venc_cam_cxt;
//VencAllocateBufferParam alloc_parm;	// 用于保存inputbuffer申请时的参数

motion_par g_motionPar;

// 描述一个 h264 slice
typedef struct slice_t
{
	void *data_;
	int len_;
	int64_t pts_;
} slice_t;
typedef std::deque<slice_t*> SLICES;

// 对应一个 v4l2 的内存映射
typedef struct Buffer 
{
	void * start;		// mmap ret
	size_t length;		// 

	//void *cedar_vaddr;	// v4l2 使用的是 yuyv 格式，但 cedar enc 需要 yuv420p 格式，
				// 这个 cedar_vaddr 分配的用于保存 yuv420p 数据的连续物理内存。
	//unsigned long cedar_phyaddr;
	
	VencInputBuffer input_buffer; //merge new cedarx api by inn.
} Buffer;

typedef struct capture_t
{
	std::string devname_;	// "/dev/video0"
	unsigned int width_, height_;	// 
	int fps_;
	int kbits_;

	slice_t *priv;		// 保存 sps, pps, ...
	
	SLICES fifo_;		// 用于缓冲
	ost::Mutex cs_fifo_;	
	ost::Semaphore sem_fifo_;

	int fd_;		// v4l2 fd

	Buffer *buffers_;	// 对应 v4l2 的缓冲
	unsigned int buf_cnt_;

	void *outbuf_;		// 
	int outbuf_size_;

} capture_t;

// 工作线程，从摄像头得到数据，交给 cedar 压缩，将压缩后的数据保存到 fifo_ 队列中
class GrabThread : ost::Thread
{
	bool quit_;

public:
	GrabThread()
	{
		quit_ = false;
		start();
	}

	~GrabThread()
	{
		quit_ = true;
		join();
	}

private:
	void run();
};

static GrabThread *_thread = 0;
static capture_t *_cap = 0;

//////// 
//#define DUMP_YUV420P
static void yuyv_nv12(const unsigned char *pyuyv, unsigned char *pnv12, int width, int height)
{
	unsigned char *Y = pnv12;
	unsigned char *UV = Y + width * height;
	//unsigned char *V = U + width * height / 4;
	int i, j;

	for (i = 0; i < height / 2; i++) {
		// 奇数行保留 U/V
		for (j = 0; j < width / 2; j++) {
			*Y++ = *pyuyv++;
			*UV++ = *pyuyv++;	//U
			*Y++ = *pyuyv++;
			*UV++ = *pyuyv++;	//V
		}

		// 偶数行的 UV 直接扔掉
		for (j = 0; j < width / 2; j++) {
			*Y++ = *pyuyv++;
			pyuyv++;		// 跳过 U
			*Y++ = *pyuyv++;
			pyuyv++;		// 跳过 V
		}
	}

#ifdef DUMP_YUV420P
	// 使用 ffmpeg -s <width>x<height> -pix_fmt yuv420p -i img-xx.yuv img-xx.jpg
	// 然后检查 img-xx.jpg 是否正确？
	// 经过检查，此处 yuv420p 图像正常，这么说，肯定是 GetFrmBufCB() 里面设置问题了 :(
#define CNT 10
	static int _ind = 0;
	char fname[128];
	snprintf(fname, sizeof(fname), "img-%02d.yuv", _ind);
	_ind++;
	_ind %= CNT;
	FILE *fp = fopen(fname, "wb");
	if (fp) {
		fwrite(q, 1, width*height*3/2, fp);
		fclose(fp);
	}
#endif // 
}

int CameraSourceCallback(void *cookie,  void *data)
{
	Venc_context * venc_cam_cxt = (Venc_context *)cookie;
	cedarv_encoder_t *venc_device = venc_cam_cxt->venc_device;
	AWCameraDevice *CameraDevice = venc_cam_cxt->CameraDevice;
	static int has_alloc_buffer = 0;
	
	VencInputBuffer input_buffer;
	int result = 0;
	//int has_alloc_buffer = 0;
	

	struct v4l2_buffer *p_buf = (struct v4l2_buffer *)data;
	v4l2_mem_map_t* p_v4l2_mem_map = GetMapmemAddress(getV4L2ctx(CameraDevice));	

	void *buffer = (void *)p_v4l2_mem_map->mem[p_buf->index];
	int size_y = venc_cam_cxt->base_cfg.input_width*venc_cam_cxt->base_cfg.input_height; 

	memset(&input_buffer, 0, sizeof(VencInputBuffer));
	
	result = venc_device->ioctrl(venc_device, VENC_CMD_GET_ALLOCATE_INPUT_BUFFER, &input_buffer);
	
	if(result != 0) {
		LOGD("no alloc input buffer right now");
		usleep(10*1000);
		return -1;
	}
	yuyv_nv12( (unsigned char*)buffer, input_buffer.addrvirY, 720, 480);
	
	//input_buffer.id = p_buf->index;
	//input_buffer.addrphyY = p_buf->m.offset;
	//input_buffer.addrphyC = p_buf->m.offset + size_y;

	//LOGD("buffer address is %x", buffer);
	//input_buffer.addrvirY = buffer;
	//input_buffer.addrvirC = buffer + size_y;

	input_buffer.pts = 1000000 * (long long)p_buf->timestamp.tv_sec + (long long)p_buf->timestamp.tv_usec;
	LOGD("pts = %ll", input_buffer.pts);

#if 1
	if(input_buffer.addrphyY >=  (void*)0x40000000)
		input_buffer.addrphyY -=0x40000000;
#endif

	if(!venc_cam_cxt->mstart) {
		LOGD("p_buf->index = %d\n", p_buf->index);
		CameraDevice->returnFrame(CameraDevice, p_buf->index);
	}

    // enquene buffer to input buffer quene
    
	LOGD("ID = %d\n", input_buffer.id);
	result = venc_device->ioctrl(venc_device, VENC_CMD_ENQUENE_INPUT_BUFFER, &input_buffer);

	if(result < 0) {
		CameraDevice->returnFrame(CameraDevice, p_buf->index);
		LOGW("video input buffer is full , skip this frame");
	}

	return 0;
}

static slice_t *slice_alloc(const void *data, int len, int64_t pts)
{
	slice_t *ns = (slice_t *)malloc(sizeof(slice_t));
	ns->data_ = malloc(len);
	memcpy(ns->data_, data, len);
	ns->pts_ = pts;
	ns->len_ = len;

	return ns;
}

static slice_t *slice_alloc(const void *p1, int l1, const void *p2, int l2, int64_t pts)
{
	slice_t *ns = (slice_t*)malloc(sizeof(slice_t));
	ns->data_ = malloc(l1+l2);
	memcpy(ns->data_, p1, l1);
	memcpy((unsigned char*)ns->data_ + l1, p2, l2);
	ns->pts_ = pts;
	ns->len_ = l1+l2;
	return ns;
}

static void slice_free(slice_t *s)
{
	free(s->data_);
	free(s);
}

//static VENC_DEVICE *g_pCedarV = 0;
static int g_cur_id = -1;

static int GetPreviewFrame(capture_t *cap, V4L2BUF_t *pBuf);

/*
// FIXME: 貌似这个有个 mode 的参数 ？？
extern "C" int cedarx_hardware_init();
extern "C" void cedarx_hardware_exit();
extern "C" __s32 cedarv_wait_ve_ready();
extern "C" int cdxalloc_open(void);
extern "C" int cdxalloc_close(void);
extern "C" void* cdxalloc_alloc(int size);
extern "C" void* cdxalloc_allocregs();
extern "C" void cdxalloc_free(void *address);
extern "C" unsigned int cdxalloc_vir2phy(void *address);
*/
/*
// FIXME: 这个 uParam1 如何传递呢？照理说应该传递 capture_t* 就方便了
static __s32 WaitFinishCB(__s32 uParam1, void *pMsg)
{
	return cedarv_wait_ve_ready();
}

// FIXME: 这个 uParam1 如何传递呢？照理说应该传递 capture_t* 就方便了
static __s32 GetFrmBufCB(__s32 uParam1,  void *pFrmBufInfo)
{
	int ret = -1;
	V4L2BUF_t Buf;
	VEnc_FrmBuf_Info encBuf;

	// get one frame
	ret = GetPreviewFrame(_cap, &Buf);
	if (ret != 0) {
		printf("GetPreviewFrame failed\n");
		return -1;
	}
	
	memset((void*)&encBuf, 0, sizeof(VEnc_FrmBuf_Info));

	int width = _cap->width_, height = _cap->height_;

	// FIXME: 貌似输入的 yuv420p 是正确的，但经过压缩输出的看起来不对，
	//	  Y 数据正常：把 u,v 数据都置为 0x80，看到正确的黑白图像了；
	//	  加上 u/v 分量，色彩不对！
	encBuf.addrY = (unsigned char*)Buf.addrPhyY;
	encBuf.addrCb = (unsigned char*)Buf.addrPhyY + width * height;
	encBuf.addrCr = (unsigned char*)Buf.addrPhyY + width * height * 5 / 4;
	encBuf.pts_valid = 1;
	encBuf.pts = Buf.timeStamp;
	encBuf.color_fmt = PIXEL_YUV420;
	encBuf.color_space = BT601;

	memcpy(pFrmBufInfo, (void*)&encBuf, sizeof(VEnc_FrmBuf_Info));

	g_cur_id = Buf.index;
	
	return 0;
}
*/

static int CedarvEncInit(unsigned int width, unsigned int height, int fps, int bitratek)
{
	int ret = -1;
	/*
	//VENC_DEVICE *pCedarV = NULL;
	
	//pCedarV = H264EncInit(&ret);
	if (ret < 0) {
		printf("H264EncInit failed\n");
		return -1;
	}
	*/	
	
	g_venc_cam_cxt->base_cfg.framerate = 30;
	g_venc_cam_cxt->base_cfg.input_width = mwidth;
	g_venc_cam_cxt->base_cfg.input_height= mheight;
	g_venc_cam_cxt->base_cfg.dst_width = mwidth;
	g_venc_cam_cxt->base_cfg.dst_height = mheight;
	g_venc_cam_cxt->base_cfg.maxKeyInterval = 25;
	g_venc_cam_cxt->base_cfg.inputformat = VENC_PIXEL_YUV420; //uv combined
	g_venc_cam_cxt->base_cfg.targetbitrate = 3*1024*1024;
	// init allocate param
	g_venc_cam_cxt->alloc_parm.buffernum = 4;
	
	g_venc_cam_cxt->venc_device = cedarvEncInit();
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_BASE_CONFIG, &g_venc_cam_cxt->base_cfg);
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_ALLOCATE_INPUT_BUFFER, &g_venc_cam_cxt->alloc_parm.buffernum);
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_OPEN, 0);
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_HEADER_DATA, &g_venc_cam_cxt->header_data);
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_SET_MOTION_PAR_FLAG, &g_motionPar);
	
	/* start encoder */
	g_venc_cam_cxt->mstart = 1;
/*
	__video_encode_format_t enc_fmt;
	enc_fmt.src_width = width;
	enc_fmt.src_height = height;
	enc_fmt.width = width;
	enc_fmt.height = height;
	enc_fmt.frame_rate = fps * 1000;
	enc_fmt.color_format = PIXEL_YUV420;
	enc_fmt.color_space = BT601;
	enc_fmt.qp_max = 40;
	enc_fmt.qp_min = 20;
	enc_fmt.avg_bit_rate = bitratek * 1024;
	enc_fmt.maxKeyInterval = fps * 2;
	enc_fmt.profileIdc = 66; // 100: high profile, 77: main profile, 66: base profile 
	enc_fmt.levelIdc = 31;
	
	if (pCedarV->IoCtrl(pCedarV, VENC_SET_ENC_INFO_CMD, (__u32)&enc_fmt) == -1) {
		fprintf(stderr, "%s: VENC_SET_ENC_INFO_CMD err\n", __func__);
		return -1;
	}

	ret = pCedarV->open(pCedarV);
	if (ret < 0) {
		printf("open H264Enc failed\n");
		return -1;
	}
	printf("open H264Enc ok\n");

	pCedarV->GetFrmBufCB = GetFrmBufCB;
	pCedarV->WaitFinishCB = WaitFinishCB;

	g_pCedarV = pCedarV;
*/
	return 0;
}

#ifdef ZQSENDER

static unsigned int now0()
{
	struct timeval tv;
	gettimeofday(&tv, 0);

	return tv.tv_sec*1000 | tv.tv_usec/1000;
}

int zq_send_h264(ZqSenderTcpCtx *snd, const void *data, int size, int key, const void *extra, int len, __s64 pts)
{
	if (key) {
		zqsnd_tcp_send(snd, CONST_ATOM_SYNC, 16);
	}
	
	struct zq_atom_header hdr;
	hdr.type.type_i = ZQ_ATOMS_TYPE_VIDEO;
	hdr.size = sizeof(hdr) +
		sizeof(struct zq_atom_header) + sizeof(struct zq_atom_video_header_data) +
		sizeof(struct zq_atom_header) + size + len;
	zqsnd_tcp_send(snd, &hdr, sizeof(hdr));

	struct zq_atom_header hdr_video_header;
	hdr_video_header.type.type_i = ZQ_ATOM_TYPE_VIDEO_HEADER;
	hdr_video_header.size = sizeof(hdr_video_header) + sizeof(struct zq_atom_video_header_data);
	zqsnd_tcp_send(snd, &hdr_video_header, sizeof(hdr_video_header));

	struct zq_atom_video_header_data vh;
	vh.stream_codec_type = 0x1b;
	vh.frame_type = key ? 'I' : 'P';
	vh.width = 0;
	vh.height = 0;
	vh.pts = 45 * pts / 1000;
	vh.dts = 45 * pts / 1000;
	zqsnd_tcp_send(snd, &vh, sizeof(vh));

	struct zq_atom_header dh;
	dh.type.type_i = ZQ_ATOM_TYPE_DATA;
	dh.size = sizeof(dh) + size + len;
	zqsnd_tcp_send(snd, &dh, sizeof(dh));
	zqsnd_tcp_send(snd, extra, len);
	zqsnd_tcp_send(snd, data, size);

	return 0;
}

static int send_data(ZqSenderTcpCtx *snd, __vbv_data_ctrl_info_t *data)
{
	unsigned char *ptr = 0;
	int len = 0;

	static unsigned char *extra = 0;
	static int extra_len = 0;

	if (!extra) {
		extra = (unsigned char *)malloc(data->privateDataLen);
		memcpy(extra, data->privateData, data->privateDataLen);
		extra_len = data->privateDataLen;
	}

	if (data->uSize0 > 0) {
		ptr = data->pData0;
		len = data->uSize0;
	}
	
	if (data->uSize1 > 0) {
		ptr = data->pData1;
		len = data->uSize1;
	}

	if (ptr) {
		if (ptr[4] == 0x65) {
			static __s64 last_pts = -1;
			fprintf(stderr, "key frame: len=%d, now=%u, pts delta=%lld\n", len, now0(), data->pts - last_pts);
			zq_send_h264(snd, ptr, len, 1, extra, extra_len, data->pts);
			last_pts = data->pts;
		}
		else {
			zq_send_h264(snd, ptr, len, 0, 0, 0, data->pts);
		}
	}

	return len;
}
#endif // zqsender

//////////// 以下实现 capture_slice.h 接口 ///////////////

static int InitCapture(capture_t *cap)
{
	// cedarx 驱动？
/*
	if (cdxalloc_open()) {
		printf("cdxalloc open error!\n");
		return -1;
	}
*/
	
	//g_venc_cam_cxt = (Venc_context *)malloc(sizeof(Venc_context));
	//memset(g_venc_cam_cxt, 0, sizeof(Venc_context));
	
	/* create source */
	g_venc_cam_cxt->CameraDevice = CreateCamera(mwidth, mheight);
	
	/* set camera source callback */
	g_venc_cam_cxt->CameraDevice->setCameraDatacallback(g_venc_cam_cxt->CameraDevice, 
		(void *)g_venc_cam_cxt, (void*)&CameraSourceCallback);
	
	/* start camera */
	g_venc_cam_cxt->CameraDevice->startCamera(g_venc_cam_cxt->CameraDevice);
	
	/*
	// open /dev/video0
	cap->fd_ = open (cap->devname_.c_str(), O_RDWR | O_NONBLOCK, 0);
	if (cap->fd_ == 0) {
		printf("open %s failed\n", cap->devname_.c_str());
		return -1;
	}

	struct v4l2_capability capi; 
	if (ioctl(cap->fd_, VIDIOC_QUERYCAP, &capi) == -1) {
		fprintf(stderr, "%s: ioctl VIDIOC_QUERYCAP err\n", __func__);
		return -1;
	}

	struct v4l2_format fmt;
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = cap->width_;
	fmt.fmt.pix.height = cap->height_;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // 多数 UVC 摄像头都支持 :)
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (ioctl(cap->fd_, VIDIOC_S_FMT, &fmt) == -1) {
		fprintf(stderr, "ioctl VIDIOC_S_FMT err\n");
		return -1;
	}

	struct v4l2_requestbuffers req;
	CLEAR (req);
	req.count = 3;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(cap->fd_, VIDIOC_REQBUFS, &req) == -1) {
		fprintf(stderr, "ioctl VIDIOC_REQBUFS err\n");
		return -1;
	}

	cap->buffers_ = new Buffer[req.count];

	// 映射
	for (cap->buf_cnt_ = 0; cap->buf_cnt_ < req.count; cap->buf_cnt_++) {
		struct v4l2_buffer buf; 
	   	CLEAR (buf);
	   	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	   	buf.memory = V4L2_MEMORY_MMAP;
	   	buf.index = cap->buf_cnt_;

	   	if (-1 == ioctl(cap->fd_, VIDIOC_QUERYBUF, &buf)) {
			printf ("VIDIOC_QUERYBUF error\n");
		   	return -1;
	   	}

		if (buf.length != cap->width_ * cap->height_ * 2) {	// YUYV
			fprintf(stderr, "??? buf.length=%d\n", buf.length);
			return -1;
		}

	   	cap->buffers_[cap->buf_cnt_].length = buf.length;
	   	cap->buffers_[cap->buf_cnt_].start = mmap (0 	// start anywhere ,    
			   buf.length,
			   PROT_READ | PROT_WRITE 	// required ,
			   MAP_SHARED 			// recommended ,
			   cap->fd_, buf.m.offset);

	   	if (MAP_FAILED == cap->buffers_[cap->buf_cnt_].start) {
			printf ("mmap failed\n");
		   	return -1;
	   	}

		// cedar 使用 yuv420p[_n_buffers].start) {
		//cap->buffers_[cap->buf_cnt_].cedar_vaddr = cdxalloc_alloc(cap->width_ * cap->height_ * 3 / 2);
		//cap->buffers_[cap->buf_cnt_].cedar_phyaddr = cdxalloc_vir2phy(cap->buffers_[cap->buf_cnt_].cedar_vaddr);
	}

	// 使能缓冲
	for (unsigned int i = 0; i < cap->buf_cnt_; ++i) {
		struct v4l2_buffer buf;
		CLEAR (buf);

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == ioctl(cap->fd_, VIDIOC_QBUF, &buf)) {
			printf("VIDIOC_QBUF failed\n");
			return -1;
		}
	}
*/
	return 0;
}
/*
static void DeInitCapture(capture_t *cap)
{
	unsigned int i;
	enum v4l2_buf_type type;

	printf("DeInitCapture");

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (-1 == ioctl(cap->fd_, VIDIOC_STREAMOFF, &type)) 	
		printf("VIDIOC_STREAMOFF failed\n");	
	else		
		printf("VIDIOC_STREAMOFF ok\n");
	
	for (i = 0; i < cap->buf_cnt_; ++i) {
		if (-1 == munmap (cap->buffers_[i].start, cap->buffers_[i].length)) {
			printf ("munmap error\n");
		}

		cdxalloc_free(cap->buffers_[i].cedar_vaddr);
	}

	if (cap->fd_ != 0) {
		close(cap->fd_);
		cap->fd_ = 0;
	}

	//cdxalloc_close make it crash, why? 
	//if (cdxalloc_close()) printf("DBG: cdxalloc_close error!\n");
	//else printf("DBG: cdxalloc closed\n");
}
*/

/*
static int StartStreaming(capture_t *cap)
{
    	int ret = -1; 
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 

	printf("V4L2Camera::v4l2StartStreaming\n");
  
    	ret = ioctl(cap->fd_, VIDIOC_STREAMON, &type); 
    	if (ret < 0) { 
        	printf("StartStreaming: Unable to start capture: %s\n", strerror(errno)); 
        	return ret; 
    	} 

    	printf("V4L2Camera::v4l2StartStreaming OK\n");
    	return 0; 
}
*/
/*
static void ReleaseFrame(capture_t *cap, int buf_id)
{	
	struct v4l2_buffer v4l2_buf;
	int ret;

	memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
	v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	v4l2_buf.memory = V4L2_MEMORY_MMAP;
	v4l2_buf.index = buf_id;		// buffer index

	ret = ioctl(cap->fd_, VIDIOC_QBUF, &v4l2_buf);
	if (ret < 0) {
		printf("VIDIOC_QBUF failed, id: %d\n", v4l2_buf.index);
		return ;
	}
}
*/
/*
static int WaitCamerReady(capture_t *cap)
{
	fd_set fds;		
	struct timeval tv;
	int r;

	FD_ZERO(&fds);
	FD_SET(cap->fd_, &fds);
	
	// Timeout
	tv.tv_sec  = 2;
	tv.tv_usec = 0;
	
	r = select(cap->fd_ + 1, &fds, NULL, NULL, &tv);
	if (r == -1) {
		printf("select err\n");
		return -1;
	} 
	else if (r == 0) {
		printf("select timeout\n");
		return -1;
	}

	return 0;
}
*/

/*
static int GetPreviewFrame(capture_t *cap, V4L2BUF_t *pBuf)	// DQ buffer for preview or encoder
{
	int ret = -1; 
	struct v4l2_buffer buf;

	ret = WaitCamerReady(cap);
	if (ret != 0) {
		printf("wait time out\n");
		return __LINE__;
	}

	// 取出数据，直接从 Buffer.start 获取
	memset(&buf, 0, sizeof(struct v4l2_buffer));
    	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
    	buf.memory = V4L2_MEMORY_MMAP; 
    	ret = ioctl(cap->fd_, VIDIOC_DQBUF, &buf); 
    	if (ret < 0) { 
	        printf("GetPreviewFrame: VIDIOC_DQBUF Failed\n"); 
        	return __LINE__; 
	}

	// 从 _buffers[x] 复制到 _cedarbufs[x]，格式从 YUYV 转换到 YUV420P
	yuyv_yuv420p((const unsigned char*)cap->buffers_[buf.index].start, (unsigned char*)cap->buffers_[buf.index].cedar_vaddr, 
			cap->width_, cap->height_);

	// 填充 
	pBuf->addrPhyY = cap->buffers_[buf.index].cedar_phyaddr;
	pBuf->index = buf.index;
	pBuf->timeStamp = (int64_t)((int64_t)buf.timestamp.tv_usec + (((int64_t)buf.timestamp.tv_sec) * 1000000));

	return 0;
}
*/

// 工作线程
void GrabThread::run()
{
	g_venc_cam_cxt = (Venc_context *)malloc(sizeof(Venc_context));
	int ret = -1;
#ifdef ZQSENDER
	ZqSenderTcpCtx *sender = 0;

	ret = zqsnd_open_tcp_server(&sender, 4000, 0);
	if (ret < 0) {
		fprintf(stderr, "can't open zqsender for port %d\n", 4000);
		::exit(-1);
	}
	fprintf(stderr, "start tcp srv port 4000\n");
#endif // zqsender
	
	ret = cedarx_hardware_init(0);
	if (ret < 0) {
		printf("cedarx_hardware_init failed\n");
		::exit(-1);
	}
	
	LOGD("Codec version = %s", getCodecVision());
	g_venc_cam_cxt->waterMark = (WaterMark*)malloc(sizeof(WaterMark));	
	memset(g_venc_cam_cxt->waterMark, 0x0, sizeof(WaterMark));

	g_venc_cam_cxt->waterMark->bgInfo.width = mwidth;
	g_venc_cam_cxt->waterMark->bgInfo.height = mheight;
	g_venc_cam_cxt->waterMark->srcPathPrefix = "/mnt/res/icon_720p_";
	g_venc_cam_cxt->waterMark->srcNum = 13;
	waterMarkInit(g_venc_cam_cxt->waterMark);
	
	/*移动侦测*/
	g_motionPar.motion_detect_enable = 1;
	g_motionPar.motion_detect_ratio = 0;
	
	ret = InitCapture(_cap);
	if(ret != 0) {
		printf("InitCapture failed\n");
		::exit(-1);
	}

	ret = CedarvEncInit(_cap->width_, _cap->height_, _cap->fps_, _cap->kbits_);
	if (ret != 0) {
		printf("CedarvEncInit failed\n");
		::exit(-1);
	}

	printf("to stream on\n");
	//StartStreaming(_cap);
	static int bFirstFrame = 1;
	while (!quit_) {
		//__vbv_data_ctrl_info_t data_info;
		int result = 0;
		VencInputBuffer input_buffer;
		VencOutputBuffer data_info;
		cedarv_encoder_t *venc_device = g_venc_cam_cxt->venc_device;
		
		int motion_flag = 0;

		// FIXME: encode 需要消耗一定时间，这里不准确
		usleep(1000 * 1000 / _cap->fps_);	// 25fps
		
		memset(&input_buffer, 0, sizeof(VencInputBuffer));
		// dequene buffer from input buffer quene;
	    result = venc_device->ioctrl(venc_device, VENC_CMD_DEQUENE_INPUT_BUFFER, &input_buffer);
		if(result<0)
		{
			LOGV("enquene input buffer is empty");
			usleep(10*1000);
			continue;
		}
		
		g_venc_cam_cxt->waterMark->bgInfo.y = input_buffer.addrvirY;
		g_venc_cam_cxt->waterMark->bgInfo.c = input_buffer.addrvirC;

		waterMarkShowTime(g_venc_cam_cxt->waterMark);
		
		cedarx_cache_op((long int)input_buffer.addrvirY, 
				(long int)input_buffer.addrvirY + g_venc_cam_cxt->waterMark->bgInfo.width * g_venc_cam_cxt->waterMark->bgInfo.height * 3/2, 0);
		
		//ret = g_pCedarV->encode(g_pCedarV);
		result = venc_device->ioctrl(venc_device, VENC_CMD_ENCODE, &input_buffer);
		
		// return the buffer to the alloc buffer quene after encoder
		venc_device->ioctrl(venc_device, VENC_CMD_RETURN_ALLOCATE_INPUT_BUFFER, &input_buffer);
		
		if (result != 0) {
			usleep(10000);
			printf("not encode, ret: %d\n", ret);
			::exit(-1);
		}

		//ReleaseFrame(_cap, g_cur_id);

		//memset(&data_info, 0 , sizeof(__vbv_data_ctrl_info_t));
		//ret = g_pCedarV->GetBitStreamInfo(g_pCedarV, &data_info);
		
		memset(&data_info, 0, sizeof(VencOutputBuffer));
		result = venc_device->ioctrl(venc_device, VENC_CMD_GET_BITSTREAM, &data_info);
		
		if(result == 0) {
			// 有数据
#ifdef ZQSENDER
			send_data(sender, &data_info);
#endif // zqsender
/*
			if (data_info.privateDataLen > 0) {
				if (_cap->priv)
					slice_free(_cap->priv);
				_cap->priv = slice_alloc(data_info.privateData, data_info.privateDataLen, 0);
				fprintf(stderr, "save priv: len=%d\n", data_info.privateDataLen);

				//ost::MutexLock al(_cap->cs_fifo_);
				//_cap->fifo_.push_back(slice_alloc(data_info.privateData, data_info.privateDataLen, 0));
			}
*/			if (bFirstFrame == 1){
				if (_cap->priv)
					slice_free(_cap->priv);
				_cap->priv = slice_alloc(g_venc_cam_cxt->header_data.bufptr, g_venc_cam_cxt->header_data.length, 0);
				fprintf(stderr, "save priv: len=%d\n", g_venc_cam_cxt->header_data.length);
				bFirstFrame = 0;
			}
			
			if (data_info.size0 > 0) {
				ost::MutexLock al(_cap->cs_fifo_);

				unsigned char *p = data_info.ptr0;

				if (_cap->fifo_.size() > 100 && p[4] == 0x65) {
					fprintf(stderr, "fifo overflow ...\n");
					// 当积累的太多，并且收到关键帧清空
					while (!_cap->fifo_.empty()) {
						slice_t *s = _cap->fifo_.front();
						_cap->fifo_.pop_front();
						slice_free(s);
						fprintf(stderr, "E");
					}
				}

				// 为每个关键帧之前保存 sps. pps
				if (p[4] == 0x65) {
					fprintf(stderr, "patch pps/sps\n");
					_cap->fifo_.push_back(slice_alloc(_cap->priv->data_, _cap->priv->len_,
								data_info.ptr0, data_info.size0, data_info.pts));
				}
				else {
					_cap->fifo_.push_back(slice_alloc(data_info.ptr0, data_info.size0, data_info.pts));
				}

				_cap->sem_fifo_.post();
			}

			if (data_info.size1 > 0) {
				ost::MutexLock al(_cap->cs_fifo_);

				unsigned char *p = data_info.ptr1;

				if (_cap->fifo_.size() > 100 && p[4] == 0x65) {
					fprintf(stderr, "fifo overflow ...\n");
					// 当积累的太多，并且收到关键帧清空
					while (!_cap->fifo_.empty()) {
						slice_t *s = _cap->fifo_.front();
						_cap->fifo_.pop_front();
						slice_free(s);
						fprintf(stderr, "E");
					}
				}

				// 为每个关键帧之前保存 sps. pps
				if (p[4] == 0x65) {
					fprintf(stderr, "patch pps/sps\n");
					_cap->fifo_.push_back(slice_alloc(_cap->priv->data_, _cap->priv->len_,
								data_info.ptr1, data_info.size1, data_info.pts));
				}
				else {
					_cap->fifo_.push_back(slice_alloc(data_info.ptr1, data_info.size1, data_info.pts));
				}

				_cap->sem_fifo_.post();
			}
		}
		
		venc_device->ioctrl(venc_device, VENC_CMD_GET_MOTION_FLAG, &motion_flag);
		if (motion_flag == 1)
		{
			/*检测到移动侦测*/
			LOGD("motion_flag = %d", motion_flag);
		}

		//g_pCedarV->ReleaseBitStreamInfo(g_pCedarV, data_info.idx);
		venc_device->ioctrl(venc_device, VENC_CMD_RETURN_BITSTREAM, &data_info);
	}
	
	//DeInitCapture(_cap);
/*
	if (g_pCedarV != NULL) {
		g_pCedarV->close(g_pCedarV);
		H264EncExit(g_pCedarV);
		g_pCedarV = NULL;
	}
*/
	/* stop camera */
	g_venc_cam_cxt->CameraDevice->stopCamera(g_venc_cam_cxt->CameraDevice);
	
	DestroyCamera(g_venc_cam_cxt->CameraDevice);
	g_venc_cam_cxt->CameraDevice = NULL;
	
	g_venc_cam_cxt->venc_device->ioctrl(g_venc_cam_cxt->venc_device, VENC_CMD_CLOSE, 0);
	cedarvEncExit(g_venc_cam_cxt->venc_device);
	g_venc_cam_cxt->venc_device = NULL;

	cedarx_hardware_exit(0);
	
	free(g_venc_cam_cxt);
}


capture_t *capture_open(const char *name, int width, int height, int fps, int kbits)
{
	if (_cap) {
		fprintf(stderr, "%s: other instance running...\n", __func__);
		return 0;
	}

	_cap = new capture_t;
	_cap->devname_ = name;
	_cap->width_ = width;
	_cap->height_ = height;
	_cap->fps_ = fps;
	_cap->kbits_ = kbits;
	_cap->priv = 0;
	_cap->outbuf_ = malloc(32*1024);
	_cap->outbuf_size_ = 32*1024;

	_thread = new GrabThread;

	return _cap;
}

void capture_close(capture_t *cap)
{
	if (_cap) {
		free(_cap->outbuf_);
		delete _thread;
		delete _cap;
		_cap = 0;
	}
}

int capture_next_slice(capture_t *cap, void **data, int64_t *pts)
{
	_cap->cs_fifo_.enter();
	while (_cap->fifo_.empty()) {
		_cap->cs_fifo_.leave();
		_cap->sem_fifo_.wait();
		_cap->cs_fifo_.enter();
	}

	slice_t *s = _cap->fifo_.front();
	_cap->fifo_.pop_front();

	if (s->len_ > _cap->outbuf_size_) {
		_cap->outbuf_size_ = (s->len_ + 4095)/4096*4096;
		_cap->outbuf_ = realloc(_cap->outbuf_, _cap->outbuf_size_);
	}

	memcpy(_cap->outbuf_, s->data_, s->len_);
	*data = _cap->outbuf_;
	*pts = s->pts_;

	int rc = s->len_;
	slice_free(s);

	_cap->cs_fifo_.leave();

	return rc;
}

