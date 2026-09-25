/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TRIGGER2_H__
#define __TRIGGER2_H__

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME		"trigger2"
#define DRIVER_DESC		"MCT Trigger II USB display adapter"

#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

#define TRIGGER2_NUM_TRANSFERS	2
#define TRIGGER2_CMD_BUF_LEN	769
#define TRIGGER2_REPLY_BUF_LEN	512
#define TRIGGER2_FRAME_HEADER_LEN	36
#define TRIGGER2_BULK_TIMEOUT_MS	5000

struct trigger2_transfer_buf {
	void *data;
	size_t len;
};

struct trigger2_transfer {
	struct trigger2_device *trigger2;

	struct trigger2_transfer_buf buf;
	size_t frame_len;
	struct drm_rect transfer_rect;

	u8 *header;

	struct timer_list timer;
	struct usb_sg_request sgr;

	struct work_struct transfer_work;
	struct completion frame_complete;
};

struct trigger2_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;
	unsigned int cmd_pipe;
	unsigned int aux_pipe;
	unsigned int reply_pipe;
	struct mutex cmd_lock;
	u8 *cmd_buf;
	u8 *reply_buf;
	u32 frame_base;
	u32 frame_end;

	struct drm_connector connector;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;

	int current_transfer;
	struct drm_rect pending_rect;
	struct workqueue_struct *transfer_wq;
	bool display_enabled;
	bool mode_programmed;

	struct trigger2_transfer transfers[TRIGGER2_NUM_TRANSFERS];
};

struct trigger2_crtc_state {
	struct drm_crtc_state base;

	/* Staged transfer buffer to be swapped in during enable */
	struct trigger2_transfer_buf bufs[TRIGGER2_NUM_TRANSFERS];
};

typedef struct drm_atomic_commit trigger2_atomic_state;

#define to_trigger2(x) container_of(x, struct trigger2_device, drm)

static inline struct trigger2_crtc_state *
to_trigger2_crtc_state(struct drm_crtc_state *state)
{
	return container_of(state, struct trigger2_crtc_state, base);
}

int trigger2_command_locked(struct trigger2_device *trigger2, u8 endpoint,
			    const void *data, size_t len);
int trigger2_reply_locked(struct trigger2_device *trigger2,
			  void *data, size_t len);
int trigger2_reg_read_locked(struct trigger2_device *trigger2,
			     u16 reg, u8 *value);
int trigger2_reg_write_locked(struct trigger2_device *trigger2,
			      u16 reg, u8 value);
int trigger2_edid_read_locked(struct trigger2_device *trigger2, u8 data[512]);
int trigger2_boot_locked(struct trigger2_device *trigger2);

int trigger2_alloc_bulk_buffer(struct trigger2_transfer_buf *buf, size_t len);
void trigger2_free_bulk_buffer(struct trigger2_transfer_buf *buf);
void trigger2_transfer_init(struct trigger2_device *trigger2);
void trigger2_stop_io(struct trigger2_device *trigger2);
int trigger2_transfer_mode_init(struct trigger2_device *trigger2,
			       const struct drm_display_mode *mode);
int trigger2_transfer_blank_frame(struct trigger2_device *trigger2,
				  u16 width, u16 height);
void trigger2_plane_atomic_update(struct drm_plane *plane,
				  trigger2_atomic_state *state);

int trigger2_modeset_init(struct trigger2_device *trigger2);

int trigger2_connector_init(struct trigger2_device *trigger2,
			    int connector_type);
#endif /* __TRIGGER2_H__ */
