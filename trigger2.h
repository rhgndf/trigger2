/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TRIGGER2_H__
#define __TRIGGER2_H__

#include <linux/completion.h>
#include <linux/scatterlist.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME		"trigger2"
#define DRIVER_DESC		"MCT Trigger 5 USB display adapter"

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

#define TRIGGER2_NUM_TRANSFERS	2

struct trigger2_transfer_buf {
	void *data;
	struct sg_table sgt;
	size_t len;
};

struct trigger2_transfer {
	struct trigger2_device *trigger2;

	struct trigger2_transfer_buf buf;
	size_t frame_len;
	struct drm_rect transfer_rect;

	struct timer_list timer;
	struct usb_sg_request sgr;

	struct work_struct transfer_work;
	struct completion frame_complete;
};

struct trigger2_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;

	struct drm_connector connector;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;

	u16 frame_counter;

	int current_transfer;
	struct drm_rect pending_rect;
	struct workqueue_struct *transfer_wq;
	bool display_enabled;
	struct delayed_work keepalive_work;

	struct trigger2_transfer transfers[TRIGGER2_NUM_TRANSFERS];
};

struct trigger2_crtc_state {
	struct drm_crtc_state base;

	/* Staged transfer buffer to be swapped in during enable */
	struct trigger2_transfer_buf bufs[TRIGGER2_NUM_TRANSFERS];
};

struct trigger2_pll {
	u8 prediv;
	u8 mul1;
	u8 mul2;
	u8 div1;
	u8 div2;
} __packed;

struct trigger2_mode_request {
	__be16 height;
	__be16 width;
	__be16 line_total_pixels; /* minus one */
	__be16 line_sync_pulse; /* minus one */
	__be16 line_back_porch; /* minus one */
	__be16 unknown1;
	__be16 unknown2;
	__be16 width_minus_one;
	__be16 frame_total_lines; /* minus one */
	__be16 frame_sync_pulse; /* minus one */
	__be16 frame_back_porch; /* minus one */
	__be16 unknown3;
	__be16 unknown4;
	__be16 height_minus_one;
	struct trigger2_pll pll;
	u8 hsync_polarity;
	u8 vsync_polarity;
} __packed;

/* gm12u320.c uses the same header format */
struct trigger2_bulk_header {
	u8 magic; /* 0xfb */
	u8 length; /* 0x14 */
	__le16 counter; /* lower 12-bit counter, upper 4-bit packet flags */
	__le16 horizontal_offset; /* lower 13-bit offset, upper 3-bit unknown */
	__le16 vertical_offset; /* lower 13-bit offset, upper 3-bit unknown */
	__le16 width; /* lower 13-bit width, upper 3-bit unknown */
	__le16 height; /* lower 13-bit height, upper 3-bit unknown */
	__le32 payload_length; /* lower 28-bit length, upper 4-bit flags */
	u8 flags; /* bit 0 must be set */
	u8 unknown1;
	u8 unknown2;
	u8 checksum;
} __packed;

#define TRIGGER2_REQUEST_KEEPALIVE		0x91
#define TRIGGER2_REQUEST_GET_REGISTER		0xA5
#define TRIGGER2_REQUEST_GET_STATUS		0xA6
#define TRIGGER2_REQUEST_GET_EDID		0xA8
#define TRIGGER2_REQUEST_SET_MODE		0xC3
#define TRIGGER2_REQUEST_SET_REGISTER		0xC4
#define TRIGGER2_REQUEST_SET_CURSOR_POSITION	0xC8
#define TRIGGER2_REQUEST_FIRMWARE_RESET		0xD1

#define TRIGGER2_KEEPALIVE_INTERVAL_MS	2000
#define TRIGGER2_BULK_TIMEOUT_MS		5000

#define to_trigger2(x) container_of(x, struct trigger2_device, drm)

static inline struct trigger2_crtc_state *
to_trigger2_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct trigger2_crtc_state, base);
}

int trigger2_read_register(struct trigger2_device *trigger2, u16 reg,
			   void *data, size_t len);
int trigger2_write_register(struct trigger2_device *trigger2, u16 reg,
			    const void *data, size_t len);

int trigger2_alloc_bulk_buffer(struct trigger2_transfer_buf *buf, size_t len);
void trigger2_free_bulk_buffer(struct trigger2_transfer_buf *buf);
void trigger2_transfer_init(struct trigger2_device *trigger2);
void trigger2_stop_io(struct trigger2_device *trigger2);
void trigger2_plane_atomic_update(struct drm_plane *plane,
				  struct drm_atomic_commit *state);

int trigger2_modeset_init(struct trigger2_device *trigger2, bool is_hdmi);

int trigger2_connector_init(struct trigger2_device *trigger2,
			    int connector_type);
#endif /* __TRIGGER2_H__ */
