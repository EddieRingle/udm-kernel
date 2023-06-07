/*
 *  Copyright (C) 2021, Matt Hsu <matt.hsu@ui.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef __G1320_POWER_H__
#define __G1320_POWER_H__

#include <linux/types.h>

enum g1320_reg {
	G1320_REG_VOUT_MODE		= 0x20,
	G1320_REG_READ_VOUT		= 0x8b,
	G1320_REG_READ_IOUT		= 0x8c,
	G1320_REG_READ_TEMP1	= 0x8d,
	G1320_REG_FAN_SPEED1	= 0x90,
	G1320_REG_READ_POUT		= 0x96,
	G1320_REG_MFR_ID		= 0x99,

	G1320_REG_END,
};

struct g1320_psu {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct power_supply	*psy;
	u32 unit;
};

#endif /* __G1320_POWER_H__ */
