// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/err.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>

#include "trigger2.h"

static const struct drm_mode_config_funcs trigger2_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static u64 trigger2_calculate_pll(struct trigger2_pll *pll, int clock)
{
	u64 ref_clock = 10000000;
	u64 target_clock = (u64)clock * 1000;
	u64 calculated_clock, calculated_err, best_err = U64_MAX;
	int prediv_div2, prediv, mul1, mul2, div1, div2;

	/* Use values found in the capture */
	for (prediv_div2 = 0x02; prediv_div2 <= 0x100; prediv_div2 <<= 1) {
		for (mul1 = 1; mul1 <= 0x32; mul1++) {
			for (mul2 = mul1; mul2 <= 0x32; mul2++) {
				for (div1 = 1; div1 <= 0x32; div1++) {
					if (!best_err)
						break;
					calculated_clock =
						div_u64(ref_clock * mul1 * mul2,
							prediv_div2 * div1);
					calculated_err =
						abs_diff(calculated_clock,
							 target_clock);
					if (prediv_div2 <= 0x10) {
						div2 = prediv_div2;
						prediv = 1;
					} else {
						div2 = 0x10;
						prediv = prediv_div2 >> 4;
					}
					if (calculated_err < best_err) {
						best_err =
							calculated_err;
						pll->mul1 = mul1;
						pll->mul2 = mul2;
						pll->div1 = div1;
						pll->div2 = div2;
						pll->prediv = prediv;
					}
				}
			}
		}
	}
	return best_err;
}

/*
 * Swap the new buffers in here because atomic_enable is not called for
 * a CRTC that is enabled but inactive.
 */
static void trigger2_atomic_commit_tail(struct drm_atomic_commit *state)
{
	struct trigger2_device *trigger2 = to_trigger2(state->dev);
	struct drm_crtc_state *crtc_state;
	struct trigger2_crtc_state *tstate;
	int idx, i;

	crtc_state = drm_atomic_get_new_crtc_state(state, &trigger2->crtc);
	if (!crtc_state)
		goto commit;

	tstate = to_trigger2_crtc_state(crtc_state);
	if (!tstate->bufs[0].data)
		goto commit;

	if (!drm_dev_enter(state->dev, &idx))
		goto commit;

	trigger2_stop_io(trigger2);
	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++) {
		trigger2_free_bulk_buffer(&trigger2->transfers[i].buf);
		trigger2->transfers[i].buf = tstate->bufs[i];
		memset(&tstate->bufs[i], 0, sizeof(tstate->bufs[i]));
	}

	drm_dev_exit(idx);
commit:
	drm_atomic_helper_commit_tail_rpm(state);
}

static const struct drm_mode_config_helper_funcs
trigger2_mode_config_helper_funcs = {
	.atomic_commit_tail = trigger2_atomic_commit_tail,
};

static struct drm_crtc_state *
trigger2_crtc_create_state(struct drm_crtc *crtc)
{
	struct trigger2_crtc_state *tstate = kzalloc_obj(*tstate);

	if (!tstate)
		return ERR_PTR(-ENOMEM);

	__drm_atomic_helper_crtc_state_init(&tstate->base, crtc);

	return &tstate->base;
}

static struct drm_crtc_state *
trigger2_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct trigger2_crtc_state *tstate;

	if (drm_WARN_ON(crtc->dev, !crtc->state))
		return NULL;

	/* Staged buffers stay with the state that allocated them */
	tstate = kzalloc_obj(*tstate);
	if (!tstate)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &tstate->base);

	return &tstate->base;
}

static void trigger2_crtc_destroy_state(struct drm_crtc *crtc,
					struct drm_crtc_state *state)
{
	struct trigger2_crtc_state *tstate = to_trigger2_crtc_state(state);
	int i;

	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++)
		trigger2_free_bulk_buffer(&tstate->bufs[i]);

	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(tstate);
}

static size_t trigger2_mode_buf_len(const struct drm_display_mode *mode)
{
	return size_add(array3_size(mode->hdisplay, mode->vdisplay, 3),
			sizeof(struct trigger2_bulk_header));
}

static int trigger2_crtc_atomic_check(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	struct drm_crtc_state *old_crtc_state =
		drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct trigger2_crtc_state *tstate = to_trigger2_crtc_state(crtc_state);
	size_t len;
	int ret, i;

	ret = drm_crtc_helper_atomic_check(crtc, state);
	if (ret)
		return ret;

	if (!drm_atomic_crtc_needs_modeset(crtc_state) || !crtc_state->enable)
		return 0;

	/* Same size? do nothing */
	len = trigger2_mode_buf_len(&crtc_state->mode);
	if (old_crtc_state->enable &&
	    trigger2_mode_buf_len(&old_crtc_state->mode) == len)
		return 0;

	/*
	 * Allocate the transfer buffers for the new mode here so that
	 * failure is reported to userspace.
	 */
	for (i = 0; i < TRIGGER2_NUM_TRANSFERS; i++) {
		ret = trigger2_alloc_bulk_buffer(&tstate->bufs[i], len);
		if (ret)
			return ret;
	}

	return 0;
}

static void trigger2_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct trigger2_device *trigger2 = to_trigger2(crtc->dev);
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_display_mode *mode = &crtc_state->mode;
	struct trigger2_mode_request request = {};
	u8 data[4];
	u64 clk;
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger2_stop_io(trigger2);

	/* Sequence recovered from USB captures. */
	ret = usb_control_msg_recv(udev, 0,
				   TRIGGER2_REQUEST_FIRMWARE_RESET,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0x0000, 0x0000, data, 1,
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		goto err;

	request.height = cpu_to_be16(mode->vdisplay);
	request.height_minus_one = cpu_to_be16(mode->vdisplay - 1);
	request.width = cpu_to_be16(mode->hdisplay);
	request.width_minus_one = cpu_to_be16(mode->hdisplay - 1);

	request.line_total_pixels = cpu_to_be16(mode->htotal - 1);
	request.line_sync_pulse =
		cpu_to_be16(mode->hsync_end - mode->hsync_start - 1);
	request.line_back_porch =
		cpu_to_be16(mode->htotal - mode->hsync_end - 1);

	request.frame_total_lines = cpu_to_be16(mode->vtotal - 1);
	request.frame_sync_pulse =
		cpu_to_be16(mode->vsync_end - mode->vsync_start - 1);
	request.frame_back_porch =
		cpu_to_be16(mode->vtotal - mode->vsync_end - 1);
	request.unknown1 = cpu_to_be16(0xff);
	request.unknown2 = cpu_to_be16(0xff);
	request.unknown3 = cpu_to_be16(0xff);
	request.unknown4 = cpu_to_be16(0xff);

	request.hsync_polarity = (mode->flags & DRM_MODE_FLAG_PHSYNC) ? 0 : 1;
	request.vsync_polarity = (mode->flags & DRM_MODE_FLAG_PVSYNC) ? 0 : 1;

	trigger2_calculate_pll(&request.pll, mode->clock);
	clk = div_u64(10000000ULL * request.pll.mul1 * request.pll.mul2,
		      (u32)request.pll.prediv * request.pll.div1 *
			      request.pll.div2 * 1000);
	drm_dbg_kms(&trigger2->drm,
		    "pll: %02x %02x %02x %02x %02x -> %llu kHz (want %d kHz)\n",
		    request.pll.prediv, request.pll.mul1, request.pll.mul2,
		    request.pll.div1, request.pll.div2, clk, mode->clock);

	/* wValue can be any value since we are sending a custom mode */
	ret = usb_control_msg_send(udev, 0, TRIGGER2_REQUEST_SET_MODE,
				   USB_DIR_OUT | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0, 0, &request, sizeof(request),
				   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
	if (ret)
		goto err;

	ret = usb_control_msg_recv(udev, 0,
				   TRIGGER2_REQUEST_FIRMWARE_RESET,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0x0201, 0x0000, data, 1,
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		goto err;

	ret = trigger2_read_register(trigger2, 0xec34, data, sizeof(data));
	if (ret)
		goto err;

	data[0] = 0x60;
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x10;
	ret = trigger2_write_register(trigger2, 0xec34, data, sizeof(data));
	if (ret)
		goto err;

	WRITE_ONCE(trigger2->display_enabled, true);

	goto exit;

err:
	drm_err(&trigger2->drm, "failed to configure display mode: %d\n", ret);
exit:
	drm_dev_exit(idx);
}

static void trigger2_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct trigger2_device *trigger2 = to_trigger2(crtc->dev);
	struct usb_device *udev = interface_to_usbdev(trigger2->intf);
	u8 data;
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger2_stop_io(trigger2);

	ret = usb_control_msg_recv(udev, 0,
				   TRIGGER2_REQUEST_FIRMWARE_RESET,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0x0001, 0x0000, &data, 1,
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (ret)
		drm_err(&trigger2->drm, "failed to disable display: %d\n", ret);

	drm_dev_exit(idx);
}

static enum drm_mode_status
trigger2_crtc_mode_valid(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode)
{
	struct trigger2_pll pll;
	u64 err, ppm;

	/*
	 * The protocol stores totals, sync pulses, and back porches minus one
	 * in 16-bit fields.
	 */
	if (mode->hsync_end <= mode->hsync_start ||
	    mode->htotal <= mode->hsync_end ||
	    mode->htotal > U16_MAX + 1)
		return MODE_H_ILLEGAL;

	if (mode->vsync_end <= mode->vsync_start ||
	    mode->vtotal <= mode->vsync_end ||
	    mode->vtotal > U16_MAX + 1)
		return MODE_V_ILLEGAL;

	if (trigger2_mode_buf_len(mode) > SZ_16M)
		return MODE_MEM;

	err = trigger2_calculate_pll(&pll, mode->clock);
	ppm = div64_u64(err * 1000, mode->clock);
	if (ppm > 10000)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

static int trigger2_plane_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(new_plane_state);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state;
	size_t len;
	int ret;

	if (!new_plane_state->fb)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  new_crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !new_plane_state->visible)
		return ret;

	/* For drm_fb_xrgb8888_to_rgb888 temp buffer */
	len = new_plane_state->fb->width * sizeof(u32);
	if (!drm_format_conv_state_reserve(&shadow_plane_state->fmtcnv_state,
					   len, GFP_KERNEL))
		return -ENOMEM;

	return 0;
}

static const struct drm_crtc_helper_funcs trigger2_crtc_helper_funcs = {
	.mode_valid = trigger2_crtc_mode_valid,
	.atomic_disable = trigger2_crtc_atomic_disable,
	.atomic_check = trigger2_crtc_atomic_check,
	.atomic_enable = trigger2_crtc_atomic_enable,
};

static const struct drm_crtc_funcs trigger2_crtc_funcs = {
	.atomic_create_state = trigger2_crtc_create_state,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = trigger2_crtc_duplicate_state,
	.atomic_destroy_state = trigger2_crtc_destroy_state,
};

static const struct drm_plane_helper_funcs trigger2_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = trigger2_plane_atomic_check,
	.atomic_update = trigger2_plane_atomic_update,
};

static const struct drm_plane_funcs trigger2_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const struct drm_encoder_funcs trigger2_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const u32 trigger2_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};


int trigger2_modeset_init(struct trigger2_device *trigger2, bool is_hdmi)
{
	struct drm_device *dev = &trigger2->drm;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	/*
	 * The device has a built-in mode list, however we ignore
	 * the mode list because the device accepts custom modes
	 */
	dev->mode_config.min_width = 1;
	dev->mode_config.max_width = 8191;
	dev->mode_config.min_height = 1;
	dev->mode_config.max_height = 8191;

	dev->mode_config.funcs = &trigger2_mode_config_funcs;
	dev->mode_config.helper_private = &trigger2_mode_config_helper_funcs;

	ret = drm_universal_plane_init(dev, &trigger2->plane, 0,
				       &trigger2_plane_funcs,
				       trigger2_plane_formats,
				       ARRAY_SIZE(trigger2_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&trigger2->plane, &trigger2_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&trigger2->plane);

	ret = drm_crtc_init_with_planes(dev, &trigger2->crtc, &trigger2->plane,
					NULL, &trigger2_crtc_funcs, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(&trigger2->crtc, &trigger2_crtc_helper_funcs);

	ret = trigger2_connector_init(trigger2, is_hdmi ?
					      DRM_MODE_CONNECTOR_HDMIA :
					      DRM_MODE_CONNECTOR_VGA);
	if (ret)
		return ret;

	ret = drm_encoder_init(dev, &trigger2->encoder, &trigger2_encoder_funcs,
			       is_hdmi ? DRM_MODE_ENCODER_TMDS :
					 DRM_MODE_ENCODER_DAC, NULL);
	if (ret)
		return ret;
	trigger2->encoder.possible_crtcs = drm_crtc_mask(&trigger2->crtc);

	ret = drm_connector_attach_encoder(&trigger2->connector,
					   &trigger2->encoder);
	if (ret)
		return ret;

	return 0;
}
