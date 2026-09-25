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
#include "trigger2_registers.h"

static const struct drm_mode_config_funcs trigger2_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

struct trigger2_clock {
	u8 divider;
	u8 multiplier;
	u8 band;
	u8 range;
};

/*
 * Captures give 12 MHz * F4 * F7 / F3. Keep observed F3/F5 pairs;
 * equivalent products need not give equivalent PLL operating points.
 */
static const struct {
	u8 divider, band, multiplier, range;
} settings[] = {
	{ 12, 0, 13, 5 }, { 48, 16, 23, 17 }, { 6, 1, 11, 7 },
};

static u32 trigger2_calculate_clock(struct trigger2_clock *clock, u32 target)
{
	u32 best = U32_MAX, best_distance = U32_MAX;
	u32 actual, error, distance;
	unsigned int i, multiplier, range;

	for (i = 0; i < ARRAY_SIZE(settings); i++) {
		for (multiplier = 1; multiplier <= 63; multiplier++) {
			for (range = 1; range <= 31; range++) {
				actual = DIV_ROUND_CLOSEST(12000 * multiplier *
							  range, settings[i].divider);
				error = abs_diff(actual, target);
				distance = abs_diff(multiplier,
						    (unsigned int)settings[i].multiplier) +
					   abs_diff(range,
						    (unsigned int)settings[i].range);
				if (error > best ||
				    (error == best && distance >= best_distance))
					continue;
				best = error;
				best_distance = distance;
				clock->divider = settings[i].divider;
				clock->multiplier = multiplier;
				clock->band = settings[i].band;
				clock->range = range;
			}
		}
	}

	return best;
}

/*
 * Swap the new buffers in here because atomic_enable is not called for
 * a CRTC that is enabled but inactive.
 */
static void trigger2_atomic_commit_tail(trigger2_atomic_state *state)
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

static void trigger2_crtc_destroy_state(struct drm_crtc *crtc,
					struct drm_crtc_state *state);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
static struct drm_crtc_state *
trigger2_crtc_create_state(struct drm_crtc *crtc)
{
	struct trigger2_crtc_state *tstate = kzalloc_obj(*tstate);

	if (!tstate)
		return ERR_PTR(-ENOMEM);

	__drm_atomic_helper_crtc_state_init(&tstate->base, crtc);
	return &tstate->base;
}
#else
static void trigger2_crtc_reset(struct drm_crtc *crtc)
{
	struct trigger2_crtc_state *tstate;

	if (crtc->state)
		trigger2_crtc_destroy_state(crtc, crtc->state);

	tstate = kzalloc_obj(*tstate);
	if (tstate)
		__drm_atomic_helper_crtc_reset(crtc, &tstate->base);
}
#endif

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
	return array3_size(mode->hdisplay, ALIGN(mode->vdisplay, 16), 3);
}

static int trigger2_crtc_atomic_check(struct drm_crtc *crtc,
				      trigger2_atomic_state *state)
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

struct trigger2_reg_write {
	u16 reg;
	u8 value;
};

static int trigger2_write_regs_locked(struct trigger2_device *trigger2,
				      const struct trigger2_reg_write *writes,
				      size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = trigger2_reg_write_locked(trigger2, writes[i].reg,
						writes[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int trigger2_bank_locked(struct trigger2_device *trigger2,
				u8 channel, u32 addr)
{
	u8 cmd[] = { TRIGGER2_CMD_FRAME_BANK, 0xfc, 0x06,
		     0xb4, channel == 1 ? 0x11 : 0x22,
		     0xb1, addr, 0xb2, addr >> 8,
		     0xb3, addr >> 16, 0xb4,
		     channel == 1 ? 0x15 : 0x2a, 0xb4,
		     channel == 1 ? 0x11 : 0x22 };

	return trigger2_command_locked(trigger2, 2, cmd, sizeof(cmd));
}

static int trigger2_channel_locked(struct trigger2_device *trigger2,
				   u8 channel, u32 addr)
{
	struct trigger2_reg_write writes[] = {
		{ TRIGGER2_REG_CHANNEL_RESET, 0 },
		{ TRIGGER2_REG_CHANNEL_STROBE, channel },
		{ TRIGGER2_REG_CHANNEL_ADDR_LO, addr },
		{ TRIGGER2_REG_CHANNEL_ADDR_MID, addr >> 8 },
		{ TRIGGER2_REG_CHANNEL_ADDR_HI, addr >> 16 },
		{ TRIGGER2_REG_CHANNEL_STROBE, channel == 1 ? 0x15 : 0x2a },
		{ TRIGGER2_REG_CHANNEL_STROBE, channel == 1 ? 0x11 : 0x22 },
	};

	return trigger2_write_regs_locked(trigger2, writes,
					  ARRAY_SIZE(writes));
}

static int trigger2_prime_channels_locked(struct trigger2_device *trigger2,
					  u32 first, u32 second)
{
	const u8 reset[] = { TRIGGER2_CMD_AUX_PAIRS, 2, 0x36, 0x24 };
	const u8 release[] = { TRIGGER2_CMD_AUX_PAIRS, 2, 0x36, 0x04 };
	const u8 activate[] = {
		TRIGGER2_CMD_FRAME_BANK, TRIGGER2_REG_CHANNEL_STROBE >> 8,
		1, (u8)TRIGGER2_REG_CHANNEL_STROBE, 0x10,
	};
	unsigned int i;
	int ret;

	/* The first captured modeset arms both banks twice after blank frames. */
	for (i = 0; i < 2; i++) {
		ret = trigger2_reg_write_locked(trigger2,
						TRIGGER2_REG_CHANNEL_RESET, 0);
		if (ret)
			return ret;
		ret = trigger2_command_locked(trigger2, 4, reset, sizeof(reset));
		if (ret)
			return ret;
		ret = trigger2_command_locked(trigger2, 4, release,
					      sizeof(release));
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0, 4);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0,
						0x20);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0, 4);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0, 9);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2,
						TRIGGER2_REG_CHANNEL_STROBE, 0x11);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2,
						TRIGGER2_REG_CHANNEL_RESET, 0);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB5, 3);
		if (ret)
			return ret;
		ret = trigger2_bank_locked(trigger2, 2, first);
		if (ret)
			return ret;
		ret = trigger2_bank_locked(trigger2, 1,
					   trigger2->frame_base / 4);
		if (ret)
			return ret;
		ret = trigger2_channel_locked(trigger2, 1, first);
		if (ret)
			return ret;
		ret = trigger2_channel_locked(trigger2, 2, second);
		if (ret)
			return ret;
		if (!i) {
			ret = trigger2_bank_locked(trigger2, 1,
						   trigger2->frame_base / 4);
			if (ret)
				return ret;
			ret = trigger2_command_locked(trigger2, 2, activate,
						      sizeof(activate));
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int trigger2_program_mode_locked(struct trigger2_device *trigger2,
					const struct drm_display_mode *mode)
{
	const struct trigger2_reg_write prepare[] = {
		{ TRIGGER2_REG_FC28, 1 }, { TRIGGER2_REG_FC59, 1 },
		{ TRIGGER2_REG_FC32, 0 }, { TRIGGER2_REG_FC34, 0 },
	};
	const u8 reset_channel[] = {
		TRIGGER2_CMD_AUX_PAIRS, 2, 0x36, 0x24,
	};
	const u8 clear_channels[] = {
		TRIGGER2_CMD_AUX_PAIRS, 0x08, 0x30, 0, 0x31, 0,
		0x32, 0, 0x33, 0,
	};
	const u8 release_channel[] = {
		TRIGGER2_CMD_AUX_PAIRS, 2, 0x36, 0x04,
	};
	const u8 latch[] = {
		TRIGGER2_CMD_REG_PAIRS, 0x0c, 0x00, 0x28, 1, 0x28, 0, 0x32, 0,
		0x59, 1, 0x59, 0, 0x34, 0,
	};
	struct trigger2_clock clock;
	struct trigger2_reg_write timing[] = {
		{ TRIGGER2_REG_FEF5, 0 },
		{ TRIGGER2_REG_WIDTH_LO, mode->hdisplay - 1 },
		{ TRIGGER2_REG_WIDTH_HI, (mode->hdisplay - 1) >> 8 },
		{ TRIGGER2_REG_FEF4, 0 },
		{ TRIGGER2_REG_HBACK_MINUS_ONE,
		  mode->htotal - mode->hsync_end - 1 },
		{ TRIGGER2_REG_FEFC, 0 },
		{ TRIGGER2_REG_HSYNC_MINUS_ONE,
		  mode->hsync_end - mode->hsync_start - 1 },
		{ TRIGGER2_REG_FEFB, 0 },
		{ TRIGGER2_REG_HTOTAL_LO, mode->htotal - 1 },
		{ TRIGGER2_REG_HTOTAL_HI, (mode->htotal - 1) >> 8 },
		{ TRIGGER2_REG_FEF3, 0 },
		{ TRIGGER2_REG_HEIGHT_LO, mode->vdisplay - 1 },
		{ TRIGGER2_REG_HEIGHT_HI, (mode->vdisplay - 1) >> 8 },
		{ TRIGGER2_REG_FEF2, 0 },
		{ TRIGGER2_REG_VBACK_MINUS_ONE,
		  mode->vtotal - mode->vsync_end - 1 },
		{ TRIGGER2_REG_FEFE, 0 },
		{ TRIGGER2_REG_VSYNC_MINUS_ONE,
		  mode->vsync_end - mode->vsync_start - 1 },
		{ TRIGGER2_REG_VTOTAL_LO, mode->vtotal - 1 },
		{ TRIGGER2_REG_VTOTAL_HI, (mode->vtotal - 1) >> 8 },
		{ TRIGGER2_REG_FC6F,
		  (mode->flags & DRM_MODE_FLAG_NHSYNC ?
		   TRIGGER2_FC6F_NEG_HSYNC : 0) |
		  (mode->flags & DRM_MODE_FLAG_NVSYNC ?
		   TRIGGER2_FC6F_NEG_VSYNC : 0) },
	};
	u8 geometry[35] = { TRIGGER2_CMD_GEOMETRY, 0x20, 0 };
	u8 pll[15] = { TRIGGER2_CMD_REG_PAIRS, 0x0c, 0 };
	u8 output[] = {
		TRIGGER2_CMD_REG_PAIRS, 0x14, 0, 0x40, 0, 0x41, 0x40,
		0x42, 0, 0x43, 0x40,
		0x36, mode->hdisplay, 0x37, mode->hdisplay >> 8,
		0x38, mode->vdisplay, 0x39, mode->vdisplay >> 8,
		0x34, 0x0e, 0x32, 0,
	};
	struct trigger2_reg_write tail[] = {
		{ TRIGGER2_REG_FB96, 0x91 }, { TRIGGER2_REG_FCB0, 9 },
	};
	u32 raw, phase[2];
	u16 geometry_values[] = {
		mode->hdisplay * 3 / 4, mode->vdisplay * 3 / 4,
		mode->hdisplay, mode->vdisplay,
	};
	u8 table[2 + ARRAY_SIZE(timing) * 3] = {
		TRIGGER2_CMD_TIMINGS, ARRAY_SIZE(timing),
	};
	unsigned int i;
	u8 status;
	int ret;

	/* Captured 0x13 descriptors use a framebuffer address divided by 4
	 * in the two channel register banks.
	 */
	trigger2->frame_base = 0xc000;
	raw = trigger2_mode_buf_len(mode);
	phase[0] = (trigger2->frame_base + raw) / 4;
	phase[1] = (trigger2->frame_base + raw * 2) / 4;
	trigger2->frame_end = trigger2->frame_base + raw * 3;

	if (trigger2->mode_programmed) {
		ret = trigger2_reg_write_locked(trigger2,
						TRIGGER2_REG_CHANNEL_RESET, 0);
		if (ret)
			return ret;
		ret = trigger2_command_locked(trigger2, 4, reset_channel,
					      sizeof(reset_channel));
		if (ret)
			return ret;
		ret = trigger2_command_locked(trigger2, 4, release_channel,
					      sizeof(release_channel));
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0, 4);
		if (ret)
			return ret;
		ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB0,
						0x20);
		if (ret)
			return ret;
	}

	ret = trigger2_write_regs_locked(trigger2, prepare,
					 ARRAY_SIZE(prepare));
	if (ret)
		return ret;

	for (i = 0; i < 8; i++) {
		u16 value = geometry_values[i % 4];

		geometry[3 + 4 * i] = 0x20 + 2 * i;
		geometry[4 + 4 * i] = value;
		geometry[5 + 4 * i] = 0x21 + 2 * i;
		geometry[6 + 4 * i] = value >> 8;
	}
	ret = trigger2_command_locked(trigger2, 3, geometry, sizeof(geometry));
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FC2F,
					mode->hdisplay >= 1600 ? 3 : 0);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FBF6,
					mode->hdisplay >= 1600 ? 2 : 0);
	if (ret)
		return ret;

	ret = trigger2_reg_read_locked(trigger2, TRIGGER2_REG_FCA3, &status);
	if (ret)
		return ret;

	trigger2_calculate_clock(&clock, mode->clock);
	pll[3] = 0xf3; pll[4] = clock.divider;
	pll[5] = 0xf4; pll[6] = clock.multiplier;
	pll[7] = 0xf6; pll[8] = 1;
	pll[9] = 0xf7; pll[10] = clock.range;
	pll[11] = 0xf5; pll[12] = clock.band;
	pll[13] = 0x4b; pll[14] = 7;
	ret = trigger2_command_locked(trigger2, 3, pll, sizeof(pll));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, latch, sizeof(latch));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(timing); i++) {
		table[2 + 3 * i] = timing[i].reg >> 8;
		table[3 + 3 * i] = timing[i].reg;
		table[4 + 3 * i] = timing[i].value;
	}
	ret = trigger2_command_locked(trigger2, 3, table, sizeof(table));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 3, output, sizeof(output));
	if (ret)
		return ret;
	ret = trigger2_write_regs_locked(trigger2, tail, ARRAY_SIZE(tail));
	if (ret)
		return ret;
	ret = trigger2_command_locked(trigger2, 4, clear_channels,
				      sizeof(clear_channels));
	if (ret)
		return ret;

	ret = trigger2_bank_locked(trigger2, 1, trigger2->frame_base / 4);
	if (ret)
		return ret;
	ret = trigger2_channel_locked(trigger2, 1, phase[0]);
	if (ret)
		return ret;
	ret = trigger2_channel_locked(trigger2, 2, phase[1]);
	if (ret)
		return ret;
	ret = trigger2_bank_locked(trigger2, 2, phase[0]);
	if (ret)
		return ret;
	ret = trigger2_bank_locked(trigger2, 1, trigger2->frame_base / 4);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2,
					TRIGGER2_REG_CHANNEL_STROBE, 0x11);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2,
					TRIGGER2_REG_CHANNEL_RESET, 0);
	if (ret)
		return ret;
	ret = trigger2_reg_write_locked(trigger2, TRIGGER2_REG_FCB5, 3);
	if (ret)
		return ret;
	ret = trigger2_channel_locked(trigger2, 2, phase[1]);
	if (ret)
		return ret;
	ret = trigger2_channel_locked(trigger2, 1, phase[0]);
	if (ret)
		return ret;
	{
		const struct trigger2_reg_write clear[] = {
			{ TRIGGER2_REG_CHANNEL_RESET, 0 },
			{ TRIGGER2_REG_CHANNEL_70, 0 },
			{ TRIGGER2_REG_CHANNEL_71, 0 },
			{ TRIGGER2_REG_CHANNEL_72, 0 },
			{ TRIGGER2_REG_CHANNEL_74, 0 },
			{ TRIGGER2_REG_CHANNEL_75, 0 },
			{ TRIGGER2_REG_CHANNEL_76, 0 },
			{ TRIGGER2_REG_FEA8, 0 },
			{ TRIGGER2_REG_FEA9, 0 },
			{ TRIGGER2_REG_FEAA, 0 },
		};

		ret = trigger2_write_regs_locked(trigger2, clear,
						 ARRAY_SIZE(clear));
	}
	if (ret)
		return ret;

	ret = trigger2_transfer_mode_init(trigger2, mode);
	if (ret)
		return ret;
	ret = trigger2_transfer_blank_frame(trigger2, mode->hdisplay,
					    mode->vdisplay);
	if (ret)
		return ret;
	ret = trigger2_transfer_blank_frame(trigger2, 64, 16);
	if (ret)
		return ret;
	if (!trigger2->mode_programmed) {
		ret = trigger2_prime_channels_locked(trigger2, phase[0],
						      phase[1]);
		if (ret)
			return ret;
	}
	trigger2->mode_programmed = true;
	return 0;
}

static void trigger2_crtc_atomic_enable(struct drm_crtc *crtc,
					trigger2_atomic_state *state)
{
	struct trigger2_device *trigger2 = to_trigger2(crtc->dev);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger2_stop_io(trigger2);
	mutex_lock(&trigger2->cmd_lock);
	ret = trigger2_program_mode_locked(trigger2, &crtc_state->mode);
	mutex_unlock(&trigger2->cmd_lock);
	if (ret)
		drm_err(&trigger2->drm, "failed to configure mode: %d\n", ret);
	else
		WRITE_ONCE(trigger2->display_enabled, true);
	drm_dev_exit(idx);
}

static void trigger2_crtc_atomic_disable(struct drm_crtc *crtc,
					 trigger2_atomic_state *state)
{
	struct trigger2_device *trigger2 = to_trigger2(crtc->dev);
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;

	trigger2_stop_io(trigger2);
	mutex_lock(&trigger2->cmd_lock);
	ret = trigger2_reg_write_locked(trigger2, 0xfb60, 0);
	if (!ret)
		ret = trigger2_reg_write_locked(trigger2, 0xfcb0, 4);
	mutex_unlock(&trigger2->cmd_lock);
	if (ret)
		drm_err(&trigger2->drm, "failed to disable display: %d\n", ret);
	drm_dev_exit(idx);
}

static enum drm_mode_status
trigger2_crtc_mode_valid(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode)
{
	struct trigger2_clock clock;
	u32 width = mode->hdisplay, height = mode->vdisplay;
	u32 hsync = mode->hsync_end - mode->hsync_start;
	u32 hback = mode->htotal - mode->hsync_end;
	u32 vsync = mode->vsync_end - mode->vsync_start;
	u32 vback = mode->vtotal - mode->vsync_end;
	u64 pixels;
	u32 error;

	if (width < 64 || width > 2048 || (width & 3) ||
	    !hsync || hsync > 256 || !hback || hback > 256 ||
	    mode->htotal > U16_MAX)
		return MODE_H_ILLEGAL;
	if (height < 16 || height > 1536 || (height & 3) ||
	    !vsync || vsync > 256 || !vback || vback > 256 ||
	    mode->vtotal > U16_MAX)
		return MODE_V_ILLEGAL;
	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return MODE_BAD;

	pixels = (u64)width * ALIGN(height, 16);
	if (3 * pixels > 0xffffff || 0xc000 + 9 * pixels > SZ_32M ||
	    (u64)width * height * 32 + 4 > U32_MAX)
		return MODE_MEM;
	if (!mode->clock || mode->clock > 200000)
		return MODE_CLOCK_RANGE;
	error = trigger2_calculate_clock(&clock, mode->clock);
	if ((u64)error * 1000000 > (u64)mode->clock * 10000)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

static int trigger2_plane_atomic_check(struct drm_plane *plane,
				       trigger2_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state;

	if (!new_plane_state->fb)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state,
						       new_plane_state->crtc);
	return drm_atomic_helper_check_plane_state(new_plane_state,
						    new_crtc_state,
						    DRM_PLANE_NO_SCALING,
						    DRM_PLANE_NO_SCALING,
						    false, false);
}

static const struct drm_crtc_helper_funcs trigger2_crtc_helper_funcs = {
	.mode_valid = trigger2_crtc_mode_valid,
	.atomic_disable = trigger2_crtc_atomic_disable,
	.atomic_check = trigger2_crtc_atomic_check,
	.atomic_enable = trigger2_crtc_atomic_enable,
};

static const struct drm_crtc_funcs trigger2_crtc_funcs = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
	.atomic_create_state = trigger2_crtc_create_state,
#else
	.reset = trigger2_crtc_reset,
#endif
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


int trigger2_modeset_init(struct trigger2_device *trigger2)
{
	struct drm_device *dev = &trigger2->drm;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 64;
	dev->mode_config.max_width = 2048;
	dev->mode_config.min_height = 16;
	dev->mode_config.max_height = 1536;

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

	ret = trigger2_connector_init(trigger2, DRM_MODE_CONNECTOR_DVII);
	if (ret)
		return ret;

	ret = drm_encoder_init(dev, &trigger2->encoder, &trigger2_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	trigger2->encoder.possible_crtcs = drm_crtc_mask(&trigger2->crtc);

	ret = drm_connector_attach_encoder(&trigger2->connector,
					   &trigger2->encoder);
	if (ret)
		return ret;

	return 0;
}
