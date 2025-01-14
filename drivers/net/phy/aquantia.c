// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Aquantia PHY
 *
 * Author: Shaohui Xie <Shaohui.Xie@freescale.com>
 *
 * Copyright 2015 Freescale Semiconductor, Inc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/phy.h>

#define PHY_ID_AQ1202	0x03a1b445
#define PHY_ID_AQ2104	0x03a1b460
#define PHY_ID_AQR105	0x03a1b4a2
#define PHY_ID_AQR106	0x03a1b4d0
#define PHY_ID_AQR107	0x03a1b4e0
#define PHY_ID_AQR405	0x03a1b4b0

#define MDIO_AN_TX_VEND_STATUS1			0xc800
#define MDIO_AN_TX_VEND_STATUS1_10BASET		(0x0 << 1)
#define MDIO_AN_TX_VEND_STATUS1_100BASETX	(0x1 << 1)
#define MDIO_AN_TX_VEND_STATUS1_1000BASET	(0x2 << 1)
#define MDIO_AN_TX_VEND_STATUS1_10GBASET	(0x3 << 1)
#define MDIO_AN_TX_VEND_STATUS1_2500BASET	(0x4 << 1)
#define MDIO_AN_TX_VEND_STATUS1_5000BASET	(0x5 << 1)
#define MDIO_AN_TX_VEND_STATUS1_RATE_MASK	(0x7 << 1)
#define MDIO_AN_TX_VEND_STATUS1_FULL_DUPLEX	BIT(0)

#define MDIO_AN_TX_VEND_INT_STATUS2		0xcc01

#define MDIO_AN_TX_VEND_INT_MASK2		0xd401
#define MDIO_AN_TX_VEND_INT_MASK2_LINK		BIT(0)

/* Vendor specific 1, MDIO_MMD_VEND1 */
#define VEND1_GLOBAL_INT_STD_STATUS		0xfc00
#define VEND1_GLOBAL_INT_VEND_STATUS		0xfc01

#define VEND1_GLOBAL_INT_STD_MASK		0xff00
#define VEND1_GLOBAL_INT_STD_MASK_PMA1		BIT(15)
#define VEND1_GLOBAL_INT_STD_MASK_PMA2		BIT(14)
#define VEND1_GLOBAL_INT_STD_MASK_PCS1		BIT(13)
#define VEND1_GLOBAL_INT_STD_MASK_PCS2		BIT(12)
#define VEND1_GLOBAL_INT_STD_MASK_PCS3		BIT(11)
#define VEND1_GLOBAL_INT_STD_MASK_PHY_XS1	BIT(10)
#define VEND1_GLOBAL_INT_STD_MASK_PHY_XS2	BIT(9)
#define VEND1_GLOBAL_INT_STD_MASK_AN1		BIT(8)
#define VEND1_GLOBAL_INT_STD_MASK_AN2		BIT(7)
#define VEND1_GLOBAL_INT_STD_MASK_GBE		BIT(6)
#define VEND1_GLOBAL_INT_STD_MASK_ALL		BIT(0)

#define VEND1_GLOBAL_INT_VEND_MASK		0xff01
#define VEND1_GLOBAL_INT_VEND_MASK_PMA		BIT(15)
#define VEND1_GLOBAL_INT_VEND_MASK_PCS		BIT(14)
#define VEND1_GLOBAL_INT_VEND_MASK_PHY_XS	BIT(13)
#define VEND1_GLOBAL_INT_VEND_MASK_AN		BIT(12)
#define VEND1_GLOBAL_INT_VEND_MASK_GBE		BIT(11)
#define VEND1_GLOBAL_INT_VEND_MASK_GLOBAL1	BIT(2)
#define VEND1_GLOBAL_INT_VEND_MASK_GLOBAL2	BIT(1)
#define VEND1_GLOBAL_INT_VEND_MASK_GLOBAL3	BIT(0)

static int aqr_config_aneg(struct phy_device *phydev)
{
	linkmode_copy(phydev->supported, phy_10gbit_features);
	linkmode_copy(phydev->advertising, phydev->supported);

	return 0;
}

static int aqr_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = phy_write_mmd(phydev, MDIO_MMD_AN,
				    MDIO_AN_TX_VEND_INT_MASK2,
				    MDIO_AN_TX_VEND_INT_MASK2_LINK);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1,
				    VEND1_GLOBAL_INT_STD_MASK,
				    VEND1_GLOBAL_INT_STD_MASK_ALL);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1,
				    VEND1_GLOBAL_INT_VEND_MASK,
				    VEND1_GLOBAL_INT_VEND_MASK_GLOBAL3 |
				    VEND1_GLOBAL_INT_VEND_MASK_AN);
	} else {
		err = phy_write_mmd(phydev, MDIO_MMD_AN,
				    MDIO_AN_TX_VEND_INT_MASK2, 0);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1,
				    VEND1_GLOBAL_INT_STD_MASK, 0);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1,
				    VEND1_GLOBAL_INT_VEND_MASK, 0);
	}

	return err;
}

static int aqr_ack_interrupt(struct phy_device *phydev)
{
	int reg;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN,
			   MDIO_AN_TX_VEND_INT_STATUS2);
	return (reg < 0) ? reg : 0;
}

static int aqr_read_status(struct phy_device *phydev)
{
	int reg;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	if (reg & MDIO_STAT1_LSTATUS)
		phydev->link = 1;
	else
		phydev->link = 0;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_TX_VEND_STATUS1);
	mdelay(10);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_TX_VEND_STATUS1);

	switch (reg & MDIO_AN_TX_VEND_STATUS1_RATE_MASK) {
	case MDIO_AN_TX_VEND_STATUS1_2500BASET:
		phydev->speed = SPEED_2500;
		break;
	case MDIO_AN_TX_VEND_STATUS1_1000BASET:
		phydev->speed = SPEED_1000;
		break;
	case MDIO_AN_TX_VEND_STATUS1_100BASETX:
		phydev->speed = SPEED_100;
		break;
	default:
		phydev->speed = SPEED_10000;
		break;
	}
	phydev->duplex = DUPLEX_FULL;

	return 0;
}

static struct phy_driver aqr_driver[] = {
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQ1202),
	.name		= "Aquantia AQ1202",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQ2104),
	.name		= "Aquantia AQ2104",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQR105),
	.name		= "Aquantia AQR105",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQR106),
	.name		= "Aquantia AQR106",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQR107),
	.name		= "Aquantia AQR107",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
{
	PHY_ID_MATCH_MODEL(PHY_ID_AQR405),
	.name		= "Aquantia AQR405",
	.features	= PHY_10GBIT_FULL_FEATURES,
	.aneg_done	= genphy_c45_aneg_done,
	.config_aneg    = aqr_config_aneg,
	.config_intr	= aqr_config_intr,
	.ack_interrupt	= aqr_ack_interrupt,
	.read_status	= aqr_read_status,
},
};

module_phy_driver(aqr_driver);

static struct mdio_device_id __maybe_unused aqr_tbl[] = {
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQ1202) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQ2104) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQR105) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQR106) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQR107) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_AQR405) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, aqr_tbl);

MODULE_DESCRIPTION("Aquantia PHY driver");
MODULE_AUTHOR("Shaohui Xie <Shaohui.Xie@freescale.com>");
MODULE_LICENSE("GPL v2");
