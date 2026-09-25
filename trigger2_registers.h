/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TRIGGER2_REGISTERS_H__
#define __TRIGGER2_REGISTERS_H__

/* The opcode namespace depends on the bulk OUT endpoint. */
#define TRIGGER2_CMD_REG_WRITE		0x02	/* EP03 */
#define TRIGGER2_CMD_REG_PAIRS		0x04	/* EP03 */
#define TRIGGER2_CMD_REG_READ		0x05	/* EP03 */
#define TRIGGER2_CMD_GEOMETRY		0x16	/* EP03 */
#define TRIGGER2_CMD_EDID		0x1b	/* EP03 */
#define TRIGGER2_CMD_TIMINGS		0x22	/* EP03 */
#define TRIGGER2_CMD_AUX_PAIRS		0x05	/* EP04 */
#define TRIGGER2_CMD_BITMAP		0x11	/* EP02 */
#define TRIGGER2_CMD_FRAME		0x13	/* EP02 */
#define TRIGGER2_CMD_FRAME_BANK		0x16	/* EP02 */

/* Cold-start commands on EP03, distinct from the register and mode opcodes. */
#define TRIGGER2_CMD_BOOT_INFO		0x91
#define TRIGGER2_CMD_BOOT_ID		0x1d
#define TRIGGER2_CMD_BOOT_CONFIG		0x90
#define TRIGGER2_CMD_BOOT_ZERO		0x0a

/* Registers touched during cold-start before any modeset. */
#define TRIGGER2_REG_FC01		0xfc01
#define TRIGGER2_REG_FC4B		0xfc4b
#define TRIGGER2_REG_FC6A		0xfc6a
#define TRIGGER2_REG_FC6B		0xfc6b
#define TRIGGER2_REG_FCA2		0xfca2
#define TRIGGER2_REG_FCF0		0xfcf0
#define TRIGGER2_REG_FCF1		0xfcf1
#define TRIGGER2_REG_FCF2		0xfcf2
#define TRIGGER2_REG_FBF2		0xfbf2
#define TRIGGER2_REG_FBF4		0xfbf4
#define TRIGGER2_REG_FBFF		0xfbff
#define TRIGGER2_REG_FE36		0xfe36
#define TRIGGER2_REG_FE57		0xfe57
#define TRIGGER2_REG_FE70		0xfe70
#define TRIGGER2_REG_FEB0		0xfeb0
#define TRIGGER2_REG_FEB1		0xfeb1
#define TRIGGER2_REG_FEE0		0xfee0

/* Display geometry and sync/blanking: low byte precedes high byte. */
#define TRIGGER2_REG_WIDTH_LO		0xfcc4
#define TRIGGER2_REG_WIDTH_HI		0xfcc5
#define TRIGGER2_REG_HBACK_MINUS_ONE	0xfcc6
#define TRIGGER2_REG_HSYNC_MINUS_ONE	0xfcc7
#define TRIGGER2_REG_HEIGHT_LO		0xfcc8
#define TRIGGER2_REG_HEIGHT_HI		0xfcc9
#define TRIGGER2_REG_VBACK_MINUS_ONE	0xfcca
#define TRIGGER2_REG_VSYNC_MINUS_ONE	0xfccb
#define TRIGGER2_REG_VTOTAL_LO		0xfccc
#define TRIGGER2_REG_VTOTAL_HI		0xfccd
#define TRIGGER2_REG_HTOTAL_LO		0xfcce
#define TRIGGER2_REG_HTOTAL_HI		0xfccf

/* Framebuffer channel select and 24-bit address (in 4-byte units). */
#define TRIGGER2_REG_CHANNEL_RESET	0xfb60
#define TRIGGER2_REG_CHANNEL_ADDR_LO	0xfb62
#define TRIGGER2_REG_CHANNEL_ADDR_MID	0xfb63
#define TRIGGER2_REG_CHANNEL_ADDR_HI	0xfb64
#define TRIGGER2_REG_CHANNEL_STROBE	0xfb65
#define TRIGGER2_REG_CHANNEL_70		0xfb70
#define TRIGGER2_REG_CHANNEL_71		0xfb71
#define TRIGGER2_REG_CHANNEL_72		0xfb72
#define TRIGGER2_REG_CHANNEL_74		0xfb74
#define TRIGGER2_REG_CHANNEL_75		0xfb75
#define TRIGGER2_REG_CHANNEL_76		0xfb76

/* Display-control writes observed around modesets; semantics partly unknown. */
#define TRIGGER2_REG_FC28		0xfc28
#define TRIGGER2_REG_FC2F		0xfc2f
#define TRIGGER2_REG_FC32		0xfc32
#define TRIGGER2_REG_FC34		0xfc34
#define TRIGGER2_REG_FC59		0xfc59
#define TRIGGER2_REG_FC6F		0xfc6f
#define TRIGGER2_REG_FCA3		0xfca3
#define TRIGGER2_REG_FCB0		0xfcb0
#define TRIGGER2_REG_FCB5		0xfcb5
#define TRIGGER2_REG_FB96		0xfb96
#define TRIGGER2_REG_FBF6		0xfbf6
#define TRIGGER2_REG_FEF2		0xfef2
#define TRIGGER2_REG_FEF3		0xfef3
#define TRIGGER2_REG_FEF4		0xfef4
#define TRIGGER2_REG_FEF5		0xfef5
#define TRIGGER2_REG_FEFB		0xfefb
#define TRIGGER2_REG_FEFC		0xfefc
#define TRIGGER2_REG_FEFE		0xfefe
#define TRIGGER2_REG_FEA8		0xfea8
#define TRIGGER2_REG_FEA9		0xfea9
#define TRIGGER2_REG_FEAA		0xfeaa

#endif /* __TRIGGER2_REGISTERS_H__ */
