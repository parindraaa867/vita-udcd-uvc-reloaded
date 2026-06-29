#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/udcd.h>
#include <psp2kern/display.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/lowio/iftu.h>
#include <taihen.h>
#include <string.h>
#include "usb_descriptors.h"
#include "uvc.h"

#define ksceKernelCpuDcacheAndL2WritebackInvalidateRange ksceKernelDcacheCleanInvalidateRange
#define ksceKernelCpuDcacheAndL2WritebackRange ksceKernelDcacheCleanRange

#ifdef DEBUG

#include "log.h"
#include "draw.h"
#include "console.h"

#define LOG(s, ...) \
	do { \
		char __buffer[128]; \
		snprintf(__buffer, sizeof(__buffer), s, ##__VA_ARGS__); \
		LOG_TO_FILE(__buffer); \
		log_flush(); \
		/*console_print(__buffer);*/ \
	} while (0)
#else
#define LOG(...) (void)0
#endif

#define ALIGN(x, a)			(((x) + ((a) - 1)) & ~((a) - 1))
#define UNUSED(x)			((void)x)

#define UVC_DRIVER_NAME			"VITAUVC00"
#define UVC_USB_PID			0x1337

/* Hold this combo while capturing to manually toggle the screen on/off. */
#define UVC_TOGGLE_COMBO		(SCE_CTRL_SELECT | SCE_CTRL_UP)

/*
 * Encode (IFTU color-convert) and send (USB transfer) frames in separate
 * threads, ping-ponging between two framebuffers so the conversion of frame
 * N+1 overlaps with the USB transfer of frame N. This roughly doubles the
 * achievable frame rate at the cost of twice the framebuffer memory.
 *
 * Override from the Makefile with PARALLEL=0 (i.e. -DENCODE_SEND_PARALLELIZE=0)
 * to fall back to a single buffer.
 */
#ifndef ENCODE_SEND_PARALLELIZE
#define ENCODE_SEND_PARALLELIZE 1
#endif

#if ENCODE_SEND_PARALLELIZE
# define UVC_FRAMEBUFFER_COUNT 2
#else
# define UVC_FRAMEBUFFER_COUNT 1
#endif

/*
 * The framebuffer is allocated once, at its maximum possible size (960x544,
 * the Vita's native resolution), and kept for the whole lifetime of the
 * plugin. 960x544 is a 1:1 mapping, so there's no scaling and the image stays
 * sharp.
 */
#define MAX_UVC_VIDEO_FRAME_SIZE	VIDEO_FRAME_SIZE_NV12(960, 544)

#define UVC_PAYLOAD_HEADER_SIZE		12
#define UVC_PAYLOAD_SIZE(frame_size)	(UVC_PAYLOAD_HEADER_SIZE + (frame_size))
#define MAX_UVC_PAYLOAD_TRANSFER_SIZE	UVC_PAYLOAD_SIZE(MAX_UVC_VIDEO_FRAME_SIZE)

#define SCE_DISPLAY_PIXELFORMAT_BGRA5551 0x50000000

/*
 * Display power. Both sets are linked via weak stubs, so this one binary loads
 * on every model; we only ever call the one that's actually present.
 */
int ksceOledDisplayOn();
int ksceOledDisplayOff();
int ksceOledGetBrightness();
int ksceOledSetBrightness(int brightness);

int ksceLcdDisplayOn();
int ksceLcdDisplayOff();
int ksceLcdGetBrightness();
int ksceLcdSetBrightness(int brightness);

/* Copied from DolceSDK */
typedef struct SceIftuPlaneState_updated {
	SceIftuFrameBuf fb;
	unsigned int unk20;             /* not observed to be non-zero */
	unsigned int unk24;             /* not observed to be non-zero */
	unsigned int unk28;             /* not observed to be non-zero */
	unsigned int src_w;             /* inverse scaling factor in 16.16 fixed point, greater than or equal to 0.25 */
	unsigned int src_h;             /* inverse scaling factor in 16.16 fixed point, greater than or equal to 0.25 */
	unsigned int dst_x;             /* offset into the destination buffer */
	unsigned int dst_y;             /* offset into the destination buffer */
	unsigned int src_x;             /* offset into the source buffer in 8.8 fixed point, strictly less than 4.0 */
	unsigned int src_y;             /* offset into the source buffer in 8.8 fixed point, strictly less than 4.0 */
	unsigned int crop_top;
	unsigned int crop_bot;
	unsigned int crop_left;
	unsigned int crop_right;
} SceIftuPlaneState_updated;

/*
 * We want the data field (raw pixel data) to be aligned to 16B for the IFTU CSC to work properly.
 * It seems the USB controller is fine with data aligned to 4B (starts reading from header field).
 */
#define UVC_FRAME_PADDING_SIZE (16 - UVC_PAYLOAD_HEADER_SIZE)
struct uvc_frame {
	unsigned char padding[UVC_FRAME_PADDING_SIZE];
	unsigned char header[UVC_PAYLOAD_HEADER_SIZE];
	unsigned char data[];
} __attribute__((packed));

/*
 * Defaults, overridable at boot via a plain-text config file (see read_config).
 */
static int cfg_screen_off	= 1;	/* blank the screen while a host is capturing  */
static int cfg_prevent_suspend	= 1;	/* keep the console awake while capturing       */
static int cfg_livearea_delay	= 15;	/* seconds to wait at boot before going live    */
static int cfg_default_frame	= 1;	/* 1=960x544, 2=896x504, 3=864x488, 4=480x272   */
static int cfg_default_fps	= 60;	/* default frame rate advertised to the host    */
static int cfg_screen_toggle	= 1;	/* allow SELECT+UP to toggle the screen manually*/
static int cfg_low_latency	= 0;	/* serialize convert/send for lowest latency    */
static int cfg_idle_screen_off	= 0;	/* blank screen after N min idle (0 = off)       */
static int cfg_compat		= 0;	/* compatibility mode: single framebuffer        */

/* dwFrameInterval (100ns units) for a given fps. */
#define FPS_INTERVAL(fps)	(10000000u / (fps))

static struct uvc_streaming_control uvc_probe_control_setting_default = {
	.bmHint				= 0,
	.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_NV12,
	.bFrameIndex			= 1,
	.dwFrameInterval		= FPS_TO_INTERVAL(60),
	.wKeyFrameRate			= 0,
	.wPFrameRate			= 0,
	.wCompQuality			= 0,
	.wCompWindowSize		= 0,
	.wDelay				= 0,
	.dwMaxVideoFrameSize		= MAX_UVC_VIDEO_FRAME_SIZE,
	.dwMaxPayloadTransferSize	= MAX_UVC_PAYLOAD_TRANSFER_SIZE,
	.dwClockFrequency		= 0,
	.bmFramingInfo			= 0,
	.bPreferedVersion		= 1,
	.bMinVersion			= 0,
	.bMaxVersion			= 0,
};

static struct uvc_streaming_control uvc_probe_control_setting;

static struct {
	unsigned char buffer[64];
	SceUdcdEP0DeviceRequest ep0_req;
} pending_recv;

typedef struct {
	int fb_idx;
	int frame_size;
} NextTransferState;

static SceUID uvc_convert_thread_id;
static SceUID uvc_housekeeping_thread_id;
static SceUID eventflag_srcfb_ready;
#if ENCODE_SEND_PARALLELIZE
static SceUID uvc_send_thread_id;
static SceUID eventflag_dstfb_ready;
static SceUID eventflag_sent;	/* signalled after each USB send (low-latency) */
#endif
static volatile NextTransferState next_xfer;
static int uvc_thread_run;
static int stream;

static volatile SceUID uvc_frame_buffer_uid[UVC_FRAMEBUFFER_COUNT];
static struct uvc_frame *uvc_frame_buffer_addr[UVC_FRAMEBUFFER_COUNT];
static uintptr_t uvc_frame_buffer_paddr[UVC_FRAMEBUFFER_COUNT];
SceUID uvc_frame_req_evflag;

/* Saved alloc params so a single slot can be (de)allocated live for compat mode. */
static SceKernelMemBlockType fb_type;
static unsigned int fb_size;
static SceKernelAllocMemBlockKernelOpt *fb_optp;
static volatile int g_single_buf;	/* compatibility mode: hold only 1 framebuffer */

/* ---------------------------------------------------------------------------
 * Display power (universal OLED/LCD), driven from the housekeeping thread.
 */
enum { DISPLAY_NONE, DISPLAY_OLED, DISPLAY_LCD };
static int display_type;
static int prev_brightness;
static int display_is_off;

/* High while a host is actively capturing (debounced; survives OBS res/FPS changes). */
static volatile int streaming_active;
/* Manual screen override toggled by the button combo (reset on each capture start/stop). */
static volatile int user_toggle;
/* Set by the idle timer to blank the screen after inactivity (idle_screen_off). */
static volatile int idle_blank;

static void display_detect(void)
{
	if (ksceKernelSearchModuleByName("SceOled") >= 0)
		display_type = DISPLAY_OLED;
	else if (ksceKernelSearchModuleByName("SceLcd") >= 0)
		display_type = DISPLAY_LCD;
	else
		display_type = DISPLAY_NONE;

	LOG("display_type = %d\n", display_type);
}

static void display_set_off(void)
{
	/* Save the brightness only on the first off transition... */
	if (!display_is_off) {
		display_is_off = 1;
		if (display_type == DISPLAY_OLED)
			prev_brightness = ksceOledGetBrightness();
		else if (display_type == DISPLAY_LCD)
			prev_brightness = ksceLcdGetBrightness();
	}

	/*
	 * ...but re-issue DisplayOff every call. display_apply() runs each
	 * housekeeping tick, so if the system's idle timer flicks the panel
	 * back on, we blank it again within ~50ms instead of being fooled by
	 * the (stale) display_is_off flag.
	 */
	if (display_type == DISPLAY_OLED)
		ksceOledDisplayOff();
	else if (display_type == DISPLAY_LCD)
		ksceLcdDisplayOff();
}

static void display_set_on(void)
{
	if (!display_is_off)
		return;
	display_is_off = 0;

	if (display_type == DISPLAY_OLED) {
		ksceOledDisplayOn();
		ksceOledSetBrightness(prev_brightness);
	} else if (display_type == DISPLAY_LCD) {
		ksceLcdDisplayOn();
		ksceLcdSetBrightness(prev_brightness);
	}
}

/* Apply the desired display state from the current stream + override flags. */
static void display_apply(void)
{
	int off = cfg_screen_off && streaming_active;

	if (user_toggle)
		off = !off;

	if (idle_blank)		/* idle timeout forces the screen off */
		off = 1;

	if (off)
		display_set_off();
	else
		display_set_on();
}

/* ---------------------------------------------------------------------------
 * Config file
 */
static unsigned int parse_uint(const char *s)
{
	unsigned int v = 0;
	while (*s == ' ' || *s == '\t')
		s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (unsigned int)(*s++ - '0');
	return v;
}

static void apply_config_kv(const char *key, const char *val)
{
	if (!strcmp(key, "screen_off"))
		cfg_screen_off = parse_uint(val) ? 1 : 0;
	else if (!strcmp(key, "prevent_suspend"))
		cfg_prevent_suspend = parse_uint(val) ? 1 : 0;
	else if (!strcmp(key, "livearea_delay"))
		cfg_livearea_delay = (int)parse_uint(val);
	else if (!strcmp(key, "screen_toggle"))
		cfg_screen_toggle = parse_uint(val) ? 1 : 0;
	else if (!strcmp(key, "low_latency"))
		cfg_low_latency = parse_uint(val) ? 1 : 0;
	else if (!strcmp(key, "idle_screen_off"))
		cfg_idle_screen_off = (int)parse_uint(val);
	else if (!strcmp(key, "compat_mode"))
		cfg_compat = parse_uint(val) ? 1 : 0;
	else if (!strcmp(key, "fps")) {
		unsigned int f = parse_uint(val);
		if (f)
			cfg_default_fps = (int)f;
	} else if (!strcmp(key, "res")) {
		unsigned int w = parse_uint(val);
		if (w >= 960)
			cfg_default_frame = 1;
		else if (w >= 896)
			cfg_default_frame = 2;
		else if (w >= 864)
			cfg_default_frame = 3;
		else
			cfg_default_frame = 4;
	}
}

static void read_config(void)
{
	static const char *paths[] = {
		"ux0:/data/udcd_uvc.txt",
		"ur0:/tai/udcd_uvc.txt",
	};
	char buf[1024];
	char key[32], val[32];
	int ki = 0, vi = 0, in_val = 0, comment = 0;
	SceUID fd = -1;
	int n, i;

	for (i = 0; i < 2; i++) {
		fd = ksceIoOpen(paths[i], SCE_O_RDONLY, 0);
		if (fd >= 0)
			break;
	}
	if (fd < 0)
		return;

	n = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';

	for (i = 0; i <= n; i++) {
		char c = buf[i];

		if (c == '\n' || c == '\0') {
			key[ki] = '\0';
			val[vi] = '\0';
			if (ki > 0 && in_val)
				apply_config_kv(key, val);
			ki = vi = 0;
			in_val = comment = 0;
			continue;
		}
		if (comment || c == '\r')
			continue;
		if (c == '#' || c == ';') {
			comment = 1;
			continue;
		}
		if (!in_val) {
			if (c == '=')
				in_val = 1;
			else if (c != ' ' && c != '\t' && ki < 31)
				key[ki++] = c;
		} else if (c != ' ' && c != '\t' && vi < 31) {
			val[vi++] = c;
		}
	}

	LOG("config: screen_off=%d prevent_suspend=%d livearea_delay=%d frame=%d fps=%d\n",
	    cfg_screen_off, cfg_prevent_suspend, cfg_livearea_delay,
	    cfg_default_frame, cfg_default_fps);
}

static int usb_ep0_req_send(const void *data, unsigned int size)
{
	static SceUdcdDeviceRequest req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)data,
		.attributes = 0,
		.size = size,
		.isControlRequest = 0,
		.onComplete = NULL,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	return ksceUdcdReqSend(&req);
}

static void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req);

static int usb_ep0_enqueue_recv_for_req(const SceUdcdEP0DeviceRequest *ep0_req)
{
	static SceUdcdDeviceRequest req;

	pending_recv.ep0_req = *ep0_req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)pending_recv.buffer,
		.attributes = 0,
		.size = pending_recv.ep0_req.wLength,
		.isControlRequest = 0,
		.onComplete = &usb_ep0_req_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelCpuDcacheAndL2WritebackInvalidateRange(pending_recv.buffer,
		pending_recv.ep0_req.wLength);

	return ksceUdcdReqRecv(&req);
}

static int uvc_frame_req_init(void)
{
	uvc_frame_req_evflag = ksceKernelCreateEventFlag("uvc_frame_req_evflag", 0, 0, NULL);
	if (uvc_frame_req_evflag < 0) {
		return uvc_frame_req_evflag;
	}

	return 0;
}

static int uvc_frame_req_fini(void)
{
	int ret;

	ret = ksceKernelDeleteEventFlag(uvc_frame_req_evflag);
	if (ret < 0)
		return ret;

	return 0;
}

static void uvc_frame_req_submit_phycont_on_complete(SceUdcdDeviceRequest *req)
{
	ksceKernelSetEventFlag(uvc_frame_req_evflag, 1);
}

static int uvc_frame_req_submit_phycont(const void *data, unsigned int size)
{
	static SceUdcdDeviceRequest req;
	int ret;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[1],
		.data = (void *)data,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = size,
		.isControlRequest = 0,
		.onComplete = uvc_frame_req_submit_phycont_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ret = ksceUdcdReqSend(&req);
	if (ret < 0)
		return ret;

	ret = ksceKernelWaitEventFlagCB(uvc_frame_req_evflag, 1, SCE_EVENT_WAITOR |
					SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);

	return ret;
}

static void uvc_handle_video_streaming_req_recv(const SceUdcdEP0DeviceRequest *req)
{
	struct uvc_streaming_control *streaming_control =
		(struct uvc_streaming_control *)pending_recv.buffer;

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Probe SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Commit SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);

			stream = 1;
			ksceKernelSetEventFlag(eventflag_srcfb_ready, 1);
			break;
		}
		break;
	}
}

void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req)
{
	switch (pending_recv.ep0_req.wIndex & 0xFF) {
	case STREAM_INTERFACE:
		uvc_handle_video_streaming_req_recv(&pending_recv.ep0_req);
		break;
	}
}

static void uvc_handle_interface_ctrl_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_interface_ctrl_req\n");
}

static void uvc_handle_input_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_input_terminal_req %x, %x\n", req->wValue, req->bRequest);
}

static void uvc_handle_output_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_output_terminal_req\n");
}

static void uvc_handle_video_streaming_req(const SceUdcdEP0DeviceRequest *req)
{
	LOG("  uvc_handle_video_streaming_req %x, %x\n", req->wValue, req->bRequest);

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_MIN:
		case UVC_GET_MAX:
		case UVC_GET_DEF:
			LOG("Probe GET_DEF, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting_default.bFormatIndex,
			    uvc_probe_control_setting_default.bmFramingInfo);
			usb_ep0_req_send(&uvc_probe_control_setting_default,
					 sizeof(uvc_probe_control_setting_default));
			break;
		case UVC_GET_CUR:
			LOG("Probe GET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			ksceKernelCpuDcacheAndL2WritebackRange(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_CUR:
			ksceKernelCpuDcacheAndL2WritebackRange(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	}
}

static void uvc_handle_video_abort(void)
{
	LOG("uvc_handle_video_abort\n");

	if (stream) {
		stream = 0;

		ksceUdcdClearFIFO(&endpoints[1]);
		ksceUdcdReqCancelAll(&endpoints[1]);
	}

	/*
	 * The screen is NOT restored here. OBS stops and immediately restarts
	 * the stream when you change resolution/FPS; restoring on every stop
	 * made it blink on. The restore is debounced in the housekeeping thread
	 * (only after ~1s with no stream).
	 */
}

static void uvc_handle_set_interface(const SceUdcdEP0DeviceRequest *req)
{
	LOG("uvc_handle_set_interface %x %x\n", req->wIndex, req->wValue);

	/* MAC OS sends Set Interface Alternate Setting 0 command after
	 * stopping to stream. This application needs to stop streaming. */
	if ((req->wIndex == STREAM_INTERFACE) && (req->wValue == 0))
		uvc_handle_video_abort();
}

static void uvc_handle_clear_feature(const SceUdcdEP0DeviceRequest *req)
{
	LOG("uvc_handle_clear_feature\n");

	/* Windows OS sends Clear Feature Request after it stops streaming,
	 * however MAC OS sends clear feature request right after it sends a
	 * Commit -> SET_CUR request. Hence, stop streaming only of streaming
	 * has started. */
	switch (req->wValue) {
	case USB_FEATURE_ENDPOINT_HALT:
		if ((req->wIndex & USB_ENDPOINT_ADDRESS_MASK) ==
		    endpoints[1].endpointNumber) {
			uvc_handle_video_abort();
		}
		break;
	}
}

static int uvc_udcd_process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user_data)
{
	LOG("usb_driver_process_request(recipient: %x, arg: %x)\n", recipient, arg);
	LOG("  request: %x type: %x wValue: %x wIndex: %x wLength: %x\n",
		req->bRequest, req->bmRequestType, req->wValue, req->wIndex, req->wLength);

	if (arg < 0)
		return -1;

	switch (req->bmRequestType) {
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0xA1 */
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x21 */
		switch (req->wIndex & 0xFF) {
		case CONTROL_INTERFACE:
			switch (req->wIndex >> 8) {
			case INTERFACE_CTRL_ID:
				uvc_handle_interface_ctrl_req(req);
				break;
			case INPUT_TERMINAL_ID:
				uvc_handle_input_terminal_req(req);
				break;
			case OUTPUT_TERMINAL_ID:
				uvc_handle_output_terminal_req(req);
				break;
			}
			break;
		case STREAM_INTERFACE:
			uvc_handle_video_streaming_req(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x01 */
		switch (req->bRequest) {
		case USB_REQ_SET_INTERFACE:
			uvc_handle_set_interface(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_ENDPOINT: /* 0x02 */
		switch (req->bRequest) {
		case USB_REQ_CLEAR_FEATURE:
			uvc_handle_clear_feature(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_DEVICE: /* 0x80 */
		switch (req->wValue >> 8) {
		case 0x0A: /* USB_DT_DEBUG */
			break;
		}
		break;
	default:
		LOG("Unknown bmRequestType: 0x%02X\n", req->bmRequestType);
	}

	return 0;
}

static int uvc_udcd_change_setting(int interfaceNumber, int alternateSetting, int bus)
{
	LOG("uvc_udcd_change %d %d\n", interfaceNumber, alternateSetting);

	return 0;
}

static int uvc_udcd_attach(int usb_version, void *user_data)
{
	LOG("uvc_udcd_attach %d\n", usb_version);

	ksceUdcdClearFIFO(&endpoints[1]);

	/*
	 * Note: the display is intentionally NOT touched here. Driving it from
	 * attach/detach was what made the screen flicker during connection.
	 * It's handled by the stream start/stop (housekeeping thread) instead.
	 */

	return 0;
}

static void uvc_udcd_detach(void *user_data)
{
	LOG("uvc_udcd_detach\n");

	uvc_handle_video_abort();
	/* The housekeeping thread restores the screen once streaming is idle. */
}

static void uvc_udcd_configure(int usb_version, int desc_count, SceUdcdInterfaceSettings *settings, void *user_data)
{
	LOG("uvc_udcd_configure %d %d %p %d\n", usb_version, desc_count, settings, settings->numDescriptors);
}

static int uvc_driver_start(int size, void *p, void *user_data)
{
	LOG("uvc_driver_start\n");

	return 0;
}

static int uvc_driver_stop(int size, void *p, void *user_data)
{
	LOG("uvc_driver_stop\n");

	return 0;
}

static SceUdcdDriver uvc_udcd_driver = {
	.driverName			= UVC_DRIVER_NAME,
	.numEndpoints			= 2,
	.endpoints			= endpoints,
	.interface			= &interface,
	.descriptor_hi			= &devdesc_hi,
	.configuration_hi		= &config_hi,
	.descriptor			= &devdesc_full,
	.configuration			= &config_full,
	.stringDescriptors		= NULL,
	.stringDescriptorProduct	= &string_descriptor_product,
	.stringDescriptorSerial		= &string_descriptor_serial,
	.processRequest			= &uvc_udcd_process_request,
	.changeSetting			= &uvc_udcd_change_setting,
	.attach				= &uvc_udcd_attach,
	.detach				= &uvc_udcd_detach,
	.configure			= &uvc_udcd_configure,
	.start				= &uvc_driver_start,
	.stop				= &uvc_driver_stop,
	.user_data			= NULL
};

static unsigned int uvc_frame_transfer(struct uvc_frame *frame,
				       unsigned int frame_size,
				       int fid, int eof)
{
	int ret;

	frame->header[0] = UVC_PAYLOAD_HEADER_SIZE;
	frame->header[1] = UVC_STREAM_EOH;

	if (fid)
		frame->header[1] |= UVC_STREAM_FID;
	if (eof)
		frame->header[1] |= UVC_STREAM_EOF;

	ret = uvc_frame_req_submit_phycont(frame->header, frame_size);
	if (ret < 0) {
		LOG("Error sending frame: 0x%08X\n", ret);
		return ret;
	}

	return 0;
}

int uvc_start(void);
static void frame_set_single(int single);
static int uvc_frame_init(void);
static int uvc_frame_term(void);
int uvc_stop(void);

static inline unsigned int display_to_iftu_pixelformat(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
	default:
		return SCE_IFTU_PIXELFORMAT_BGRX8888;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return SCE_IFTU_PIXELFORMAT_BGRA5551;
	}
}

static inline unsigned int display_pixelformat_bpp(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
	default:
		return 4;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return 2;
	}
}

static int frame_convert_to_nv12(int fid, const SceDisplayFrameBufInfo *fb_info,
					int dst_width, int dst_height)
{
	uintptr_t src_paddr = fb_info->paddr;
	unsigned int src_width = fb_info->framebuf.width;
	unsigned int src_width_aligned = ALIGN(src_width, 16);
	unsigned int src_pitch = fb_info->framebuf.pitch;
	unsigned int src_height = fb_info->framebuf.height;
	unsigned int src_pixelfmt = fb_info->framebuf.pixelformat;
	unsigned int src_pixelfmt_bpp = display_pixelformat_bpp(src_pixelfmt);
	/* Physical address cached at alloc time: the memblock never moves. */
	uintptr_t dst_paddr = uvc_frame_buffer_paddr[fid];

	static SceIftuCscParams RGB_to_YCbCr_JPEG_csc_params = {
		0, 0x202, 0x3FF,
		0, 0x3FF,     0,
		{
			{ 0x99, 0x12C,  0x3A},
			{0xFAA, 0xF57, 0x100},
			{0x100, 0xF2A, 0xFD7}
		}
	};

	SceIftuConvParams params;
	memset(&params, 0, sizeof(params));
	params.size = sizeof(params);
	params.unk04 = 1;
	params.csc_params1 = &RGB_to_YCbCr_JPEG_csc_params;
	params.csc_params2 = NULL;
	params.csc_control = 1;
	params.unk14 = 0;
	params.unk18 = 0;
	params.unk1C = 0;
	params.alpha = 0xFF;
	params.unk24 = 0;

	SceIftuPlaneState_updated src;
	memset(&src, 0, sizeof(src));
	src.fb.pixelformat = display_to_iftu_pixelformat(src_pixelfmt);
	src.fb.width = src_width_aligned;
	src.fb.height = src_height;
	src.fb.leftover_stride = (src_pitch - src_width_aligned) * src_pixelfmt_bpp;
	src.fb.leftover_align = 0;
	src.fb.paddr0 = src_paddr;
	src.unk20 = 0;
	src.unk24 = 0;
	src.unk28 = 0;
	src.src_w = (src_width * 0x10000) / dst_width;
	src.src_h = (src_height * 0x10000) / dst_height;
	src.dst_x = 0;
	src.dst_y = 0;
	src.src_x = 0;
	src.src_y = 0;
	src.crop_top = 0;
	src.crop_bot = 0;
	src.crop_left = 0;
	src.crop_right = 0;

	SceIftuFrameBuf dst;
	memset(&dst, 0, sizeof(dst));
	dst.pixelformat = SCE_IFTU_PIXELFORMAT_NV12;
	dst.width = dst_width;
	dst.height = dst_height;
	dst.leftover_stride = 0;
	dst.leftover_align = 0;
	dst.paddr0 = dst_paddr;
	dst.paddr1 = dst_paddr + dst_width * dst_height;

	return ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
}

static int convert_frame(void)
{
	int ret;
	SceDisplayFrameBufInfo fb_info;
	int head = ksceDisplayGetPrimaryHead();

	memset(&fb_info, 0, sizeof(fb_info));
	fb_info.size = sizeof(fb_info);
	ret = ksceDisplayGetProcFrameBufInternal(-1, head, 0, &fb_info);
	if (ret < 0 || fb_info.paddr == 0)
		ret = ksceDisplayGetProcFrameBufInternal(-1, head, 1, &fb_info);
	if (ret < 0)
		return ret;

	switch (uvc_probe_control_setting.bFormatIndex) {
	case FORMAT_INDEX_UNCOMPRESSED_NV12: {
		const struct UVC_FRAME_UNCOMPRESSED(4) *frames =
			video_streaming_descriptors.frames_uncompressed_nv12;
		int cur_frame_index = uvc_probe_control_setting.bFrameIndex;
		int dst_width = frames[cur_frame_index - 1].wWidth;
		int dst_height = frames[cur_frame_index - 1].wHeight;

		NextTransferState now_xfer = next_xfer;
		now_xfer.frame_size = VIDEO_FRAME_SIZE_NV12(dst_width, dst_height);
		now_xfer.fb_idx = g_single_buf ? 0 :
			((now_xfer.fb_idx + 1) & (UVC_FRAMEBUFFER_COUNT - 1));

		/* Lazily allocate the framebuffer(s) on the first frame. */
		if (uvc_frame_buffer_uid[now_xfer.fb_idx] < 0) {
			if (uvc_frame_init() < 0)
				return -1;
		}

		ret = frame_convert_to_nv12(now_xfer.fb_idx, &fb_info, dst_width, dst_height);
		if (ret < 0) {
			LOG("Error converting NV12 frame: 0x%08X\n", ret);
			return ret;
		}
		next_xfer = now_xfer;

		break;
	}
	}

#if ENCODE_SEND_PARALLELIZE
	ksceKernelSetEventFlag(eventflag_dstfb_ready, 1);
#endif

	return 0;
}

/*
 * Smoothed time (microseconds) the last USB frame transfers actually took.
 * Used to adaptively pace the capture: if the wire can't sustain the host's
 * requested FPS, we trigger frames no faster than transfers complete, so the
 * effective FPS degrades gracefully instead of building a latency backlog.
 */
static volatile unsigned int xfer_ema_us;

static int send_frame(void)
{
	static int fid = 0;

	int ret;
	uint64_t t0;

	NextTransferState now_xfer = next_xfer;

	if (uvc_frame_buffer_uid[now_xfer.fb_idx] < 0)
		return -1;

	t0 = ksceKernelGetSystemTimeWide();
	ret = uvc_frame_transfer(uvc_frame_buffer_addr[now_xfer.fb_idx],
				 UVC_PAYLOAD_SIZE(now_xfer.frame_size), fid, 1);
	if (ret < 0) {
		LOG("Error sending frame: 0x%08X\n", ret);
		return ret;
	}

	/* Exponential moving average of the transfer time (1/4 weight). */
	{
		unsigned int dt = (unsigned int)(ksceKernelGetSystemTimeWide() - t0);
		xfer_ema_us = xfer_ema_us ? (xfer_ema_us - xfer_ema_us / 4 + dt / 4) : dt;
	}

	fid ^= 1;

	return 0;
}

static int display_vblank_cb_func(int notifyId, int notifyCount, int notifyArg, void *common)
{
	static unsigned int frames = 0;
	unsigned int elapsed;

	if (!stream)
		return 0;

	/*
	 * VBlanks occur at ~60FPS.
	 */
	frames += notifyCount;
	elapsed = FPS_TO_INTERVAL(60 / frames);

	/*
	 * Pace to the slower of the host's requested interval and the measured
	 * USB transfer time (converted us -> 100ns units). When the wire is the
	 * limit, this drops us to a sustainable FPS instead of queueing frames.
	 */
	unsigned int target = uvc_probe_control_setting.dwFrameInterval;
	unsigned int adaptive = xfer_ema_us * 10;
	if (adaptive > target)
		target = adaptive;

	if (elapsed >= target) {
		ksceKernelSetEventFlag(eventflag_srcfb_ready, 1);
		frames = 0;
	}

	return 0;
}

/* Append an unsigned decimal to a buffer; returns chars written. */
static int app_u(char *b, unsigned int v)
{
	char t[12];
	int n = 0, p = 0;
	if (!v) { b[0] = '0'; return 1; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) b[p++] = t[--n];
	return p;
}

/*
 * Publish runtime status for the config app dashboard:
 *   "streaming width height interval_us xfer_us"
 */
static void status_write(void)
{
	char buf[96];
	int p = 0, idx = uvc_probe_control_setting.bFrameIndex;
	unsigned int w = 0, h = 0;
	unsigned int iu = uvc_probe_control_setting.dwFrameInterval / 10;

	if (idx >= 1 && idx <= 4) {
		w = video_streaming_descriptors.frames_uncompressed_nv12[idx - 1].wWidth;
		h = video_streaming_descriptors.frames_uncompressed_nv12[idx - 1].wHeight;
	}
	p += app_u(buf + p, streaming_active); buf[p++] = ' ';
	p += app_u(buf + p, w);                buf[p++] = ' ';
	p += app_u(buf + p, h);                buf[p++] = ' ';
	p += app_u(buf + p, iu);               buf[p++] = ' ';
	p += app_u(buf + p, xfer_ema_us);      buf[p++] = '\n';

	SceUID fd = ksceIoOpen("ux0:/data/udcd_uvc_status.txt",
			       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		ksceIoWrite(fd, buf, p);
		ksceIoClose(fd);
	}
}

/*
 * Housekeeping: owns display power, debounces the stream state, keeps the
 * console awake while capturing, handles the manual screen-toggle combo, the
 * idle screen-off timer, live config reload, and the status dashboard file.
 */
static int uvc_housekeeping_thread(SceSize args, void *argp)
{
	unsigned int prev_buttons = 0;
	uint64_t last_active = 0;
	uint64_t last_cfg_check = 0;
	uint64_t last_status = 0;
	uint64_t last_input;

	display_detect();
	last_input = ksceKernelGetSystemTimeWide();

	while (uvc_thread_run) {
		uint64_t now = ksceKernelGetSystemTimeWide();

		/*
		 * "Apply without reboot": the config app drops a trigger file when
		 * it saves. Re-read the config (at most once/sec) so screen-off,
		 * screen-toggle and keep-awake update live - no reboot needed.
		 */
		if (now - last_cfg_check > 1000000) {
			SceUID tfd = ksceIoOpen("ux0:/data/udcd_uvc_reload",
						SCE_O_RDONLY, 0);
			if (tfd >= 0) {
				ksceIoClose(tfd);
				ksceIoRemove("ux0:/data/udcd_uvc_reload");
				read_config();
			}
			/* Apply compat-mode buffer count live (safe only while idle). */
			frame_set_single(cfg_compat);
			last_cfg_check = now;
		}

		/* Debounce the capture state so quick OBS re-negotiations don't
		 * count as "stopped". */
		if (stream) {
			last_active = now;
			if (!streaming_active) {
				streaming_active = 1;
				user_toggle = 0;
			}
		} else if (streaming_active && (now - last_active) > 1000000) {
			streaming_active = 0;
			user_toggle = 0;
		}

		/*
		 * Keep the console awake mid-capture. The DEFAULT tick resets ALL
		 * idle timers (suspend AND the display dim/off timer), so the
		 * system's own inactivity logic can't turn our blanked screen back
		 * on - which was happening after a few seconds of no input.
		 */
		if (streaming_active && (cfg_prevent_suspend || cfg_screen_off))
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

		/* Poll the controller once: drives the toggle combo and idle-wake. */
		{
			SceCtrlData cd;
			if (ksceCtrlPeekBufferPositive(0, &cd, 1) >= 0) {
				if (cd.buttons || streaming_active)
					last_input = now;
				if (cfg_screen_off && cfg_screen_toggle) {
					int combo_now = (cd.buttons & UVC_TOGGLE_COMBO) == UVC_TOGGLE_COMBO;
					int combo_prev = (prev_buttons & UVC_TOGGLE_COMBO) == UVC_TOGGLE_COMBO;
					if (combo_now && !combo_prev && streaming_active)
						user_toggle ^= 1;
				}
				prev_buttons = cd.buttons;
			}
		}

		/* Idle screen-off: blank after N min with no input and no capture. */
		idle_blank = (cfg_idle_screen_off > 0 && !streaming_active &&
			      (now - last_input) >
			      (uint64_t)cfg_idle_screen_off * 60 * 1000000ull);

		display_apply();

		/* Publish status (~1/sec) for the config app dashboard. */
		if (now - last_status > 1000000) {
			status_write();
			last_status = now;
		}

		ksceKernelDelayThreadCB(50 * 1000);
	}

	return 0;
}

static int uvc_convert_thread(SceSize args, void *argp)
{
	SceUID display_vblank_cb_uid;

#if !ENCODE_SEND_PARALLELIZE
	stream = 0;
	uvc_start();
#endif

	display_vblank_cb_uid = ksceKernelCreateCallback("uvc_display_vblank", 0,
							 display_vblank_cb_func, NULL);

	ksceDisplayRegisterVblankStartCallback(display_vblank_cb_uid);

	while (uvc_thread_run) {
		unsigned int out_bits;

		int ret = ksceKernelWaitEventFlagCB(eventflag_srcfb_ready, 1,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, (SceUInt32[]){1000000});

		if (ret == 0 && stream) {
			convert_frame();
#if !ENCODE_SEND_PARALLELIZE
			send_frame();
#else
			/*
			 * Low-latency mode: wait for the USB send to finish before
			 * grabbing the next frame (1 in flight). Trades the convert/
			 * send overlap for the freshest possible frame each cycle.
			 */
			if (cfg_low_latency || g_single_buf) {
				unsigned int ob;
				ksceKernelWaitEventFlagCB(eventflag_sent, 1,
					SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
					&ob, (SceUInt32[]){200000});
			}
#endif
		} else if (!stream) {
			/* Idle (~1s with no frame requested): free the framebuffers
			 * so the plugin holds no large RAM while you're just gaming. */
			uvc_frame_term();
		}
	}

	ksceDisplayUnregisterVblankStartCallback(display_vblank_cb_uid);
	ksceKernelDeleteCallback(display_vblank_cb_uid);

#if !ENCODE_SEND_PARALLELIZE
	uvc_stop();
#endif

	return 0;
}

#if ENCODE_SEND_PARALLELIZE
static int uvc_send_thread(SceSize args, void *argp)
{
	stream = 0;
	uvc_start();

	ksceKernelSetEventFlag(eventflag_srcfb_ready, 1);

	while (uvc_thread_run) {
		unsigned int out_bits;

		int ret = ksceKernelWaitEventFlagCB(eventflag_dstfb_ready, 1,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, (SceUInt32[]){1000000});

		if (ret == 0 && stream) {
			send_frame();
			ksceKernelSetEventFlag(eventflag_sent, 1);
		}
	}

	uvc_stop();

	return 0;
}
#endif

static int uvc_frame_term(void)
{
	for (int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++) {
		if (uvc_frame_buffer_uid[i] >= 0) {
			ksceKernelFreeMemBlock(uvc_frame_buffer_uid[i]);
			uvc_frame_buffer_uid[i] = -1;
			uvc_frame_buffer_paddr[i] = 0;
		}
	}

	return 0;
}

static int frame_alloc_slot(int i)
{
	int ret;

	if (uvc_frame_buffer_uid[i] >= 0)
		return 0;

	uvc_frame_buffer_uid[i] = ksceKernelAllocMemBlock("uvc_frame_buffer", fb_type, fb_size, fb_optp);
	if (uvc_frame_buffer_uid[i] < 0) {
		LOG("Error allocating CSC%d dest memory: 0x%08X\n", i, uvc_frame_buffer_uid[i]);
		return uvc_frame_buffer_uid[i];
	}
	ret = ksceKernelGetMemBlockBase(uvc_frame_buffer_uid[i], (void **)&uvc_frame_buffer_addr[i]);
	if (ret >= 0)
		ret = ksceKernelGetPaddr(uvc_frame_buffer_addr[i]->data, &uvc_frame_buffer_paddr[i]);
	if (ret < 0) {
		ksceKernelFreeMemBlock(uvc_frame_buffer_uid[i]);
		uvc_frame_buffer_uid[i] = -1;
	}
	return ret;
}

static void frame_free_slot(int i)
{
	if (uvc_frame_buffer_uid[i] >= 0) {
		ksceKernelFreeMemBlock(uvc_frame_buffer_uid[i]);
		uvc_frame_buffer_uid[i] = -1;
		uvc_frame_buffer_paddr[i] = 0;
	}
}

/* Switch between 1 and 2 held framebuffers live. Only safe while not streaming. */
static void frame_set_single(int single)
{
	if (single == g_single_buf)
		return;
	if (streaming_active)
		return;	/* defer until the next idle check */

	if (single) {
		for (int i = 1; i < UVC_FRAMEBUFFER_COUNT; i++)
			frame_free_slot(i);
		g_single_buf = 1;
	} else {
		for (int i = 1; i < UVC_FRAMEBUFFER_COUNT; i++)
			if (frame_alloc_slot(i) < 0)
				return;
		g_single_buf = 0;
	}
}

static int uvc_frame_init(void)
{
	int n;

	/* Ensure we're not leaking a previous allocation. */
	uvc_frame_term();

#ifdef USE_CDRAM
	fb_type = SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_CDRAM_RW;
	fb_size = ALIGN(UVC_FRAME_PADDING_SIZE + MAX_UVC_PAYLOAD_TRANSFER_SIZE, 256 * 1024);
#else
	/*
	 * Kernel-root physically-contiguous, non-cacheable partition (large pool,
	 * no cache flush needed before the IFTU/USB DMA reads from it).
	 */
	fb_type = SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW;
	fb_size = ALIGN(UVC_FRAME_PADDING_SIZE + MAX_UVC_PAYLOAD_TRANSFER_SIZE, 4 * 1024);
#endif
	fb_optp = NULL;

	/* Compatibility mode holds a single framebuffer (less phycont memory). */
	g_single_buf = cfg_compat ? 1 : 0;
	n = g_single_buf ? 1 : UVC_FRAMEBUFFER_COUNT;

	for (int i = 0; i < n; i++) {
		int ret = frame_alloc_slot(i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int uvc_start(void)
{
	int ret;

	/*
	 * Wait until there's a framebuffer set.
	 */
	ksceDisplayWaitSetFrameBufCB();

	/*
	 * The filesystems are mounted by now, so it's safe to read the config.
	 */
	read_config();

	/* Apply the configured default resolution/FPS to what we advertise. */
	if (cfg_default_frame >= 1 && cfg_default_frame <= 4) {
		uvc_probe_control_setting_default.bFrameIndex = cfg_default_frame;
		video_streaming_descriptors.format_uncompressed_nv12.bDefaultFrameIndex =
			cfg_default_frame;
	}
	if (cfg_default_fps > 0)
		uvc_probe_control_setting_default.dwFrameInterval = FPS_INTERVAL(cfg_default_fps);

#ifndef DEBUG
	/*
	 * Wait until LiveArea is more or less ready.
	 */
	if (cfg_livearea_delay > 0)
		ksceKernelDelayThreadCB(cfg_livearea_delay * 1000 * 1000);
#endif

	ret = ksceUdcdDeactivate();
	if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT) {
		LOG("Error deactivating UDCD (0x%08X)\n", ret);
		return ret;
	}

	ksceUdcdStop("USB_MTP_Driver", 0, NULL);
	ksceUdcdStop("USBPSPCommunicationDriver", 0, NULL);
	ksceUdcdStop("USBSerDriver", 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);

	ret = ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	if (ret < 0) {
		LOG("Error starting the USBDeviceControllerDriver driver (0x%08X)\n", ret);
		return ret;
	}

	ret = ksceUdcdStart(UVC_DRIVER_NAME, 0, NULL);
	if (ret < 0) {
		LOG("Error starting the " UVC_DRIVER_NAME " driver (0x%08X)\n", ret);
		goto err_start_uvc_driver;
	}

	ret = ksceUdcdActivate(UVC_USB_PID);
	if (ret < 0) {
		LOG("Error activating the " UVC_DRIVER_NAME " driver (0x%08X)\n", ret);
		goto err_activate;
	}

	/*
	 * Framebuffers are allocated lazily (only while a host is actually
	 * streaming) and freed when idle, so the plugin holds no large
	 * physically-contiguous RAM when you're just playing a game. This is
	 * what the original plugin did and keeps memory-heavy titles stable.
	 */
	ret = uvc_frame_req_init();
	if (ret < 0) {
		LOG("Error allocating USB request (0x%08X)\n", ret);
		goto err_alloc_uvc_frame_req;
	}

	/*
	 * Set the current streaming settings to the default ones.
	 */
	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
	       sizeof(uvc_probe_control_setting));

	return 0;

err_alloc_uvc_frame_req:
	uvc_frame_term();
	ksceUdcdDeactivate();
err_activate:
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
err_start_uvc_driver:
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	return ret;
}

int uvc_stop(void)
{
	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USB_MTP_Driver", 0, NULL);
	ksceUdcdActivate(0x4E4);

	uvc_frame_req_fini();
	uvc_frame_term();

	/* Don't leave the screen off if we're torn down mid-stream. */
	streaming_active = 0;
	user_toggle = 0;
	display_set_on();

	return 0;
}

static SceUID SceUdcd_sub_01E1128C_hook_uid = -1;
static tai_hook_ref_t SceUdcd_sub_01E1128C_ref;

static int SceUdcd_sub_01E1128C_hook_func(const SceUdcdConfigDescriptor *config_descriptor, void *desc_data)
{
	int ret;
	SceUdcdConfigDescriptor *dst = desc_data;

	ret = TAI_CONTINUE(int, SceUdcd_sub_01E1128C_ref, config_descriptor, desc_data);

	/*
	 * SceUdcd doesn't use the extra and extraLength members of the
	 * SceUdcdConfigDescriptor struct, so we have to manually patch
	 * it to add custom descriptors.
	 */
	if (dst->wTotalLength == config_descriptor->wTotalLength) {
		memmove(desc_data + USB_DT_CONFIG_SIZE + sizeof(interface_association_descriptor),
			desc_data + USB_DT_CONFIG_SIZE,
			config_descriptor->wTotalLength - USB_DT_CONFIG_SIZE);

		memcpy(desc_data + USB_DT_CONFIG_SIZE, interface_association_descriptor,
		       sizeof(interface_association_descriptor));

		dst->wTotalLength += sizeof(interface_association_descriptor);

		ksceKernelCpuDcacheAndL2WritebackRange(desc_data, dst->wTotalLength);
	}

	return ret;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;
	tai_module_info_t SceUdcd_modinfo;

	for (int i = 0; i < UVC_FRAMEBUFFER_COUNT; i++)
		uvc_frame_buffer_uid[i] = -1;

#ifdef DEBUG
	log_reset();
	framebuffer_map();
	console_init();
#endif

	LOG("udcd_uvc by xerpi\n");

	SceUdcd_modinfo.size = sizeof(SceUdcd_modinfo);
	taiGetModuleInfoForKernel(KERNEL_PID, "SceUdcd", &SceUdcd_modinfo);

	SceUdcd_sub_01E1128C_hook_uid = taiHookFunctionOffsetForKernel(KERNEL_PID,
		&SceUdcd_sub_01E1128C_ref, SceUdcd_modinfo.modid, 0,
		0x01E1128C - 0x01E10000, 1, SceUdcd_sub_01E1128C_hook_func);

	uvc_convert_thread_id = ksceKernelCreateThread("uvc_convert_thread", uvc_convert_thread,
						       0x3C, 0x1000, 0, 0x10000, 0);
	if (uvc_convert_thread_id < 0) {
		LOG("Error creating the UVC convert thread (0x%08X)\n", uvc_convert_thread_id);
		goto err_return;
	}

	uvc_housekeeping_thread_id = ksceKernelCreateThread("uvc_housekeeping_thread",
		uvc_housekeeping_thread, 0x3C, 0x1000, 0, 0x10000, 0);
	if (uvc_housekeeping_thread_id < 0) {
		LOG("Error creating the UVC housekeeping thread (0x%08X)\n", uvc_housekeeping_thread_id);
		goto err_destroy_convert_thread;
	}

#if ENCODE_SEND_PARALLELIZE
	uvc_send_thread_id = ksceKernelCreateThread("uvc_send_thread", uvc_send_thread,
						    0x3C, 0x1000, 0, 0x10000, 0);
	if (uvc_send_thread_id < 0) {
		LOG("Error creating the UVC send thread (0x%08X)\n", uvc_send_thread_id);
		goto err_destroy_housekeeping_thread;
	}
#endif

	eventflag_srcfb_ready = ksceKernelCreateEventFlag("eventflag_srcfb_ready", 0, 0, NULL);
	if (eventflag_srcfb_ready < 0) {
		LOG("Error creating the UVC srcfb event flag (0x%08X)\n", eventflag_srcfb_ready);
		goto err_destroy_thread;
	}

#if ENCODE_SEND_PARALLELIZE
	eventflag_dstfb_ready = ksceKernelCreateEventFlag("eventflag_dstfb_ready", 0, 0, NULL);
	if (eventflag_dstfb_ready < 0) {
		LOG("Error creating the UVC dstfb event flag (0x%08X)\n", eventflag_dstfb_ready);
		goto err_delete_srcfb_event_flag;
	}
	eventflag_sent = ksceKernelCreateEventFlag("eventflag_sent", 0, 0, NULL);
	if (eventflag_sent < 0) {
		LOG("Error creating the UVC sent event flag (0x%08X)\n", eventflag_sent);
		ksceKernelDeleteEventFlag(eventflag_dstfb_ready);
		goto err_delete_srcfb_event_flag;
	}
#endif

	ret = ksceUdcdRegister(&uvc_udcd_driver);
	if (ret < 0) {
		LOG("Error registering the UDCD driver (0x%08X)\n", ret);
		goto err_delete_event_flag;
	}

	uvc_thread_run = 1;

	ret = ksceKernelStartThread(uvc_housekeeping_thread_id, 0, NULL);
	if (ret < 0) {
		LOG("Error starting the UVC housekeeping thread (0x%08X)\n", ret);
		goto err_unregister;
	}

#if ENCODE_SEND_PARALLELIZE
	ret = ksceKernelStartThread(uvc_send_thread_id, 0, NULL);
	if (ret < 0) {
		LOG("Error starting the UVC send thread (0x%08X)\n", ret);
		goto err_unregister;
	}

	/*
	 * The send thread does uvc_start() (incl. the buffer allocation); wait
	 * until it signals it's ready before kicking off the convert thread.
	 */
	unsigned int out_bits;
	ksceKernelWaitEventFlagCB(eventflag_srcfb_ready, 1,
		SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
		&out_bits, (SceUInt32[]){1000000});
#endif

	ret = ksceKernelStartThread(uvc_convert_thread_id, 0, NULL);
	if (ret < 0) {
		LOG("Error starting the UVC convert thread (0x%08X)\n", ret);
		goto err_unregister;
	}

	return SCE_KERNEL_START_SUCCESS;

err_unregister:
	ksceUdcdUnregister(&uvc_udcd_driver);
err_delete_event_flag:
#if ENCODE_SEND_PARALLELIZE
	ksceKernelDeleteEventFlag(eventflag_sent);
	ksceKernelDeleteEventFlag(eventflag_dstfb_ready);
err_delete_srcfb_event_flag:
#endif
	ksceKernelDeleteEventFlag(eventflag_srcfb_ready);
err_destroy_thread:
#if ENCODE_SEND_PARALLELIZE
	ksceKernelDeleteThread(uvc_send_thread_id);
err_destroy_housekeeping_thread:
#endif
	ksceKernelDeleteThread(uvc_housekeeping_thread_id);
err_destroy_convert_thread:
	ksceKernelDeleteThread(uvc_convert_thread_id);
err_return:
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	uvc_thread_run = 0;

	/* Stop housekeeping first so it can't fight the teardown over the display. */
	ksceKernelWaitThreadEnd(uvc_housekeeping_thread_id, NULL, NULL);

	ksceKernelSetEventFlag(eventflag_srcfb_ready, 1);
	ksceKernelWaitThreadEnd(uvc_convert_thread_id, NULL, NULL);
#if ENCODE_SEND_PARALLELIZE
	ksceKernelSetEventFlag(eventflag_dstfb_ready, 1);
	ksceKernelWaitThreadEnd(uvc_send_thread_id, NULL, NULL);
#endif

	ksceKernelDeleteEventFlag(eventflag_srcfb_ready);
	ksceKernelDeleteThread(uvc_convert_thread_id);
	ksceKernelDeleteThread(uvc_housekeeping_thread_id);
#if ENCODE_SEND_PARALLELIZE
	ksceKernelDeleteEventFlag(eventflag_sent);
	ksceKernelDeleteEventFlag(eventflag_dstfb_ready);
	ksceKernelDeleteThread(uvc_send_thread_id);
#endif

	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdUnregister(&uvc_udcd_driver);

	if (SceUdcd_sub_01E1128C_hook_uid > 0) {
		taiHookReleaseForKernel(SceUdcd_sub_01E1128C_hook_uid,
			SceUdcd_sub_01E1128C_ref);
	}

	/* Make sure we never leave the user staring at a black screen. */
	display_set_on();

#ifdef DEBUG
	console_fini();
	framebuffer_unmap();
	log_flush();
#endif

	return SCE_KERNEL_STOP_SUCCESS;
}
