/*
 * Copyright (C) 2011 Samsung Electronics
 * Heungjun Kim <riverful.kim@samsung.com>
 * Kyungmin Park <kyungmin.park@samsung.com>
 * Donghwa Lee <dh09.lee@samsung.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <lcd.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/arch/cpu.h>
#include <asm/arch/pinmux.h>
#include <asm/arch/clock.h>
#include <asm/arch/mipi_dsim.h>
#include <asm/arch/watchdog.h>
#include <asm/arch/power.h>
#include <power/pmic.h>
#include <usb/s3c_udc.h>
#include <power/max8997_pmic.h>
#include <power/max8997_muic.h>
#include <power/battery.h>
#include <power/max17042_fg.h>
#include <libtizen.h>
#include <usb.h>
#include <usb_mass_storage.h>

#include "setup.h"

DECLARE_GLOBAL_DATA_PTR;

unsigned int board_rev;

#ifdef CONFIG_REVISION_TAG
u32 get_board_rev(void)
{
	return board_rev;
}
#endif

static void check_hw_revision(void);
struct s3c_plat_otg_data s5pc210_otg_data;

int exynos_init(void)
{
	check_hw_revision();
	printf("HW Revision:\t0x%x\n", board_rev);

	return 0;
}

void i2c_init_board(void)
{
	int err;

	/* I2C_5 -> PMIC */
	err = exynos_pinmux_config(PERIPH_ID_I2C5, PINMUX_FLAG_NONE);
	if (err) {
		debug("I2C%d not configured\n", (I2C_5));
		return;
	}

	/* I2C_8 -> FG */
	gpio_request(EXYNOS4_GPIO_Y40, "i2c_clk");
	gpio_request(EXYNOS4_GPIO_Y41, "i2c_data");
	gpio_direction_output(EXYNOS4_GPIO_Y40, 1);
	gpio_direction_output(EXYNOS4_GPIO_Y41, 1);
}

static void trats_low_power_mode(void)
{
	struct exynos4_clock *clk =
	    (struct exynos4_clock *)samsung_get_base_clock();
	struct exynos4_power *pwr =
	    (struct exynos4_power *)samsung_get_base_power();

	/* Power down CORE1 */
	/* LOCAL_PWR_CFG [1:0] 0x3 EN, 0x0 DIS */
	writel(0x0, &pwr->arm_core1_configuration);

	/* Change the APLL frequency */
	/* ENABLE (1 enable) | LOCKED (1 locked)  */
	/* [31]              | [29]               */
	/* FSEL      | MDIV          | PDIV            | SDIV */
	/* [27]      | [25:16]       | [13:8]          | [2:0]      */
	writel(0xa0c80604, &clk->apll_con0);

	/* Change CPU0 clock divider */
	/* CORE2_RATIO  | APLL_RATIO   | PCLK_DBG_RATIO | ATB_RATIO  */
	/* [30:28]      | [26:24]      | [22:20]        | [18:16]    */
	/* PERIPH_RATIO | COREM1_RATIO | COREM0_RATIO   | CORE_RATIO */
	/* [14:12]      | [10:8]       | [6:4]          | [2:0]      */
	writel(0x00000100, &clk->div_cpu0);

	/* CLK_DIV_STAT_CPU0 - wait until clock gets stable (0 = stable) */
	while (readl(&clk->div_stat_cpu0) & 0x1111111)
		continue;

	/* Change clock divider ratio for DMC */
	/* DMCP_RATIO                  | DMCD_RATIO  */
	/* [22:20]                     | [18:16]     */
	/* DMC_RATIO | DPHY_RATIO | ACP_PCLK_RATIO   | ACP_RATIO */
	/* [14:12]   | [10:8]     | [6:4]            | [2:0]     */
	writel(0x13113117, &clk->div_dmc0);

	/* CLK_DIV_STAT_DMC0 - wait until clock gets stable (0 = stable) */
	while (readl(&clk->div_stat_dmc0) & 0x11111111)
		continue;

	/* Turn off unnecessary power domains */
	writel(0x0, &pwr->xxti_configuration);	/* XXTI */
	writel(0x0, &pwr->cam_configuration);	/* CAM */
	writel(0x0, &pwr->tv_configuration);    /* TV */
	writel(0x0, &pwr->mfc_configuration);   /* MFC */
	writel(0x0, &pwr->g3d_configuration);   /* G3D */
	writel(0x0, &pwr->gps_configuration);   /* GPS */
	writel(0x0, &pwr->gps_alive_configuration);	/* GPS_ALIVE */

	/* Turn off unnecessary clocks */
	writel(0x0, &clk->gate_ip_cam);	/* CAM */
	writel(0x0, &clk->gate_ip_tv);          /* TV */
	writel(0x0, &clk->gate_ip_mfc);	/* MFC */
	writel(0x0, &clk->gate_ip_g3d);	/* G3D */
	writel(0x0, &clk->gate_ip_image);	/* IMAGE */
	writel(0x0, &clk->gate_ip_gps);	/* GPS */
}

static int pmic_init_max8997(void)
{
	struct pmic *p = pmic_get("MAX8997_PMIC");
	int i = 0, ret = 0;
	u32 val;

	if (pmic_probe(p))
		return -1;

	/* BUCK1 VARM: 1.2V */
	val = (1200000 - 650000) / 25000;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK1DVS1, val);
	val = ENBUCK | ACTIVE_DISCHARGE;		/* DVS OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK1CTRL, val);

	/* BUCK2 VINT: 1.1V */
	val = (1100000 - 650000) / 25000;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK2DVS1, val);
	val = ENBUCK | ACTIVE_DISCHARGE;		/* DVS OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK2CTRL, val);


	/* BUCK3 G3D: 1.1V - OFF */
	ret |= pmic_reg_read(p, MAX8997_REG_BUCK3CTRL, &val);
	val &= ~ENBUCK;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK3CTRL, val);

	val = (1100000 - 750000) / 50000;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK3DVS, val);

	/* BUCK4 CAMISP: 1.2V - OFF */
	ret |= pmic_reg_read(p, MAX8997_REG_BUCK4CTRL, &val);
	val &= ~ENBUCK;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK4CTRL, val);

	val = (1200000 - 650000) / 25000;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK4DVS, val);

	/* BUCK5 VMEM: 1.2V */
	val = (1200000 - 650000) / 25000;
	for (i = 0; i < 8; i++)
		ret |= pmic_reg_write(p, MAX8997_REG_BUCK5DVS1 + i, val);

	val = ENBUCK | ACTIVE_DISCHARGE;		/* DVS OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK5CTRL, val);

	/* BUCK6 CAM AF: 2.8V */
	/* No Voltage Setting Register */
	/* GNSLCT 3.0X */
	val = GNSLCT;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK6CTRL, val);

	/* BUCK7 VCC_SUB: 2.0V */
	val = (2000000 - 750000) / 50000;
	ret |= pmic_reg_write(p, MAX8997_REG_BUCK7DVS, val);

	/* LDO1 VADC: 3.3V */
	val = max8997_reg_ldo(3300000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO1CTRL, val);

	/* LDO1 Disable active discharging */
	ret |= pmic_reg_read(p, MAX8997_REG_LDO1CONFIG, &val);
	val &= ~LDO_ADE;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO1CONFIG, val);

	/* LDO2 VALIVE: 1.1V */
	val = max8997_reg_ldo(1100000) | EN_LDO;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO2CTRL, val);

	/* LDO3 VUSB/MIPI: 1.1V */
	val = max8997_reg_ldo(1100000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO3CTRL, val);

	/* LDO4 VMIPI: 1.8V */
	val = max8997_reg_ldo(1800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO4CTRL, val);

	/* LDO5 VHSIC: 1.2V */
	val = max8997_reg_ldo(1200000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO5CTRL, val);

	/* LDO6 VCC_1.8V_PDA: 1.8V */
	val = max8997_reg_ldo(1800000) | EN_LDO;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO6CTRL, val);

	/* LDO7 CAM_ISP: 1.8V */
	val = max8997_reg_ldo(1800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO7CTRL, val);

	/* LDO8 VDAC/VUSB: 3.3V */
	val = max8997_reg_ldo(3300000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO8CTRL, val);

	/* LDO9 VCC_2.8V_PDA: 2.8V */
	val = max8997_reg_ldo(2800000) | EN_LDO;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO9CTRL, val);

	/* LDO10 VPLL: 1.1V */
	val = max8997_reg_ldo(1100000) | EN_LDO;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO10CTRL, val);

	/* LDO11 TOUCH: 2.8V */
	val = max8997_reg_ldo(2800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO11CTRL, val);

	/* LDO12 VTCAM: 1.8V */
	val = max8997_reg_ldo(1800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO12CTRL, val);

	/* LDO13 VCC_3.0_LCD: 3.0V */
	val = max8997_reg_ldo(3000000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO13CTRL, val);

	/* LDO14 MOTOR: 3.0V */
	val = max8997_reg_ldo(3000000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO14CTRL, val);

	/* LDO15 LED_A: 2.8V */
	val = max8997_reg_ldo(2800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO15CTRL, val);

	/* LDO16 CAM_SENSOR: 1.8V */
	val = max8997_reg_ldo(1800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO16CTRL, val);

	/* LDO17 VTF: 2.8V */
	val = max8997_reg_ldo(2800000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO17CTRL, val);

	/* LDO18 TOUCH_LED 3.3V */
	val = max8997_reg_ldo(3300000) | DIS_LDO;	/* OFF */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO18CTRL, val);

	/* LDO21 VDDQ: 1.2V */
	val = max8997_reg_ldo(1200000) | EN_LDO;
	ret |= pmic_reg_write(p, MAX8997_REG_LDO21CTRL, val);

	/* SAFEOUT for both 1 and 2: 4.9V, Active discharge, Enable */
	val = (SAFEOUT_4_90V << 0) | (SAFEOUT_4_90V << 2) |
		ACTDISSAFEO1 | ACTDISSAFEO2 | ENSAFEOUT1 | ENSAFEOUT2;
	ret |= pmic_reg_write(p, MAX8997_REG_SAFEOUTCTRL, val);

	if (ret) {
		puts("MAX8997 PMIC setting error!\n");
		return -1;
	}
	return 0;
}

int exynos_power_init(void)
{
	int chrg, ret;
	struct power_battery *pb;
	struct pmic *p_fg, *p_chrg, *p_muic, *p_bat;

	/*
	 * For PMIC/MUIC the I2C bus is named as I2C5, but it is connected
	 * to logical I2C adapter 0
	 *
	 * The FUEL_GAUGE is marked as I2C9 on the schematic, but connected
	 * to logical I2C adapter 1
	 */
	ret = pmic_init(I2C_5);
	ret |= pmic_init_max8997();
	ret |= power_fg_init(I2C_9);
	ret |= power_muic_init(I2C_5);
	ret |= power_bat_init(0);
	if (ret)
		return ret;

	p_fg = pmic_get("MAX17042_FG");
	if (!p_fg) {
		puts("MAX17042_FG: Not found\n");
		return -ENODEV;
	}

	p_chrg = pmic_get("MAX8997_PMIC");
	if (!p_chrg) {
		puts("MAX8997_PMIC: Not found\n");
		return -ENODEV;
	}

	p_muic = pmic_get("MAX8997_MUIC");
	if (!p_muic) {
		puts("MAX8997_MUIC: Not found\n");
		return -ENODEV;
	}

	p_bat = pmic_get("BAT_TRATS");
	if (!p_bat) {
		puts("BAT_TRATS: Not found\n");
		return -ENODEV;
	}

	p_fg->parent =  p_bat;
	p_chrg->parent = p_bat;
	p_muic->parent = p_bat;

	p_bat->low_power_mode = trats_low_power_mode;
	p_bat->pbat->battery_init(p_bat, p_fg, p_chrg, p_muic);

	pb = p_bat->pbat;
	chrg = p_muic->chrg->chrg_type(p_muic);
	debug("CHARGER TYPE: %d\n", chrg);

	if (!p_chrg->chrg->chrg_bat_present(p_chrg)) {
		puts("No battery detected\n");
		return 0;
	}

	p_fg->fg->fg_battery_check(p_fg, p_bat);

	if (pb->bat->state == CHARGE && chrg == CHARGER_USB)
		puts("CHARGE Battery !\n");

	return 0;
}

static unsigned int get_hw_revision(void)
{
	int hwrev = 0;
	char str[10];
	int i;

	/* hw_rev[3:0] == GPE1[3:0] */
	for (i = 0; i < 4; i++) {
		int pin = i + EXYNOS4_GPIO_E10;

		sprintf(str, "hw_rev%d", i);
		gpio_request(pin, str);
		gpio_cfg_pin(pin, S5P_GPIO_INPUT);
		gpio_set_pull(pin, S5P_GPIO_PULL_NONE);
	}

	udelay(1);

	for (i = 0; i < 4; i++)
		hwrev |= (gpio_get_value(EXYNOS4_GPIO_E10 + i) << i);

	debug("hwrev 0x%x\n", hwrev);

	return hwrev;
}

static void check_hw_revision(void)
{
	int hwrev;

	hwrev = get_hw_revision();

	board_rev |= hwrev;
}


#ifdef CONFIG_USB_GADGET
static int s5pc210_phy_control(int on)
{
	int ret = 0;
	u32 val = 0;
	struct pmic *p = pmic_get("MAX8997_PMIC");
	if (!p)
		return -ENODEV;

	if (pmic_probe(p))
		return -1;

	if (on) {
		ret |= pmic_set_output(p, MAX8997_REG_SAFEOUTCTRL,
				      ENSAFEOUT1, LDO_ON);
		ret |= pmic_reg_read(p, MAX8997_REG_LDO3CTRL, &val);
		ret |= pmic_reg_write(p, MAX8997_REG_LDO3CTRL, EN_LDO | val);

		ret |= pmic_reg_read(p, MAX8997_REG_LDO8CTRL, &val);
		ret |= pmic_reg_write(p, MAX8997_REG_LDO8CTRL, EN_LDO | val);
	} else {
		ret |= pmic_reg_read(p, MAX8997_REG_LDO8CTRL, &val);
		ret |= pmic_reg_write(p, MAX8997_REG_LDO8CTRL, DIS_LDO | val);

		ret |= pmic_reg_read(p, MAX8997_REG_LDO3CTRL, &val);
		ret |= pmic_reg_write(p, MAX8997_REG_LDO3CTRL, DIS_LDO | val);
		ret |= pmic_set_output(p, MAX8997_REG_SAFEOUTCTRL,
				      ENSAFEOUT1, LDO_OFF);
	}

	if (ret) {
		puts("MAX8997 LDO setting error!\n");
		return -1;
	}

	return 0;
}

struct s3c_plat_otg_data s5pc210_otg_data = {
	.phy_control	= s5pc210_phy_control,
	.regs_phy	= EXYNOS4_USBPHY_BASE,
	.regs_otg	= EXYNOS4_USBOTG_BASE,
	.usb_phy_ctrl	= EXYNOS4_USBPHY_CONTROL,
	.usb_flags	= PHY0_SLEEP,
};

int board_usb_init(int index, enum usb_init_type init)
{
	debug("USB_udc_probe\n");
	return s3c_udc_probe(&s5pc210_otg_data);
}

int g_dnl_board_usb_cable_connected(void)
{
	struct pmic *muic = pmic_get("MAX8997_MUIC");
	if (!muic)
		return 0;

	return !!muic->chrg->chrg_type(muic);
}
#endif

static void pmic_reset(void)
{
	gpio_direction_output(EXYNOS4_GPIO_X07, 1);
	gpio_set_pull(EXYNOS4_GPIO_X27, S5P_GPIO_PULL_NONE);
}

static void board_clock_init(void)
{
	struct exynos4_clock *clk =
		(struct exynos4_clock *)samsung_get_base_clock();

	writel(CLK_SRC_CPU_VAL, (unsigned int)&clk->src_cpu);
	writel(CLK_SRC_TOP0_VAL, (unsigned int)&clk->src_top0);
	writel(CLK_SRC_FSYS_VAL, (unsigned int)&clk->src_fsys);
	writel(CLK_SRC_PERIL0_VAL, (unsigned int)&clk->src_peril0);

	writel(CLK_DIV_CPU0_VAL, (unsigned int)&clk->div_cpu0);
	writel(CLK_DIV_CPU1_VAL, (unsigned int)&clk->div_cpu1);
	writel(CLK_DIV_DMC0_VAL, (unsigned int)&clk->div_dmc0);
	writel(CLK_DIV_DMC1_VAL, (unsigned int)&clk->div_dmc1);
	writel(CLK_DIV_LEFTBUS_VAL, (unsigned int)&clk->div_leftbus);
	writel(CLK_DIV_RIGHTBUS_VAL, (unsigned int)&clk->div_rightbus);
	writel(CLK_DIV_TOP_VAL, (unsigned int)&clk->div_top);
	writel(CLK_DIV_FSYS1_VAL, (unsigned int)&clk->div_fsys1);
	writel(CLK_DIV_FSYS2_VAL, (unsigned int)&clk->div_fsys2);
	writel(CLK_DIV_FSYS3_VAL, (unsigned int)&clk->div_fsys3);
	writel(CLK_DIV_PERIL0_VAL, (unsigned int)&clk->div_peril0);
	writel(CLK_DIV_PERIL3_VAL, (unsigned int)&clk->div_peril3);

	writel(PLL_LOCKTIME, (unsigned int)&clk->apll_lock);
	writel(PLL_LOCKTIME, (unsigned int)&clk->mpll_lock);
	writel(PLL_LOCKTIME, (unsigned int)&clk->epll_lock);
	writel(PLL_LOCKTIME, (unsigned int)&clk->vpll_lock);
	writel(APLL_CON1_VAL, (unsigned int)&clk->apll_con1);
	writel(APLL_CON0_VAL, (unsigned int)&clk->apll_con0);
	writel(MPLL_CON1_VAL, (unsigned int)&clk->mpll_con1);
	writel(MPLL_CON0_VAL, (unsigned int)&clk->mpll_con0);
	writel(EPLL_CON1_VAL, (unsigned int)&clk->epll_con1);
	writel(EPLL_CON0_VAL, (unsigned int)&clk->epll_con0);
	writel(VPLL_CON1_VAL, (unsigned int)&clk->vpll_con1);
	writel(VPLL_CON0_VAL, (unsigned int)&clk->vpll_con0);

	writel(CLK_GATE_IP_CAM_VAL, (unsigned int)&clk->gate_ip_cam);
	writel(CLK_GATE_IP_VP_VAL, (unsigned int)&clk->gate_ip_tv);
	writel(CLK_GATE_IP_MFC_VAL, (unsigned int)&clk->gate_ip_mfc);
	writel(CLK_GATE_IP_G3D_VAL, (unsigned int)&clk->gate_ip_g3d);
	writel(CLK_GATE_IP_IMAGE_VAL, (unsigned int)&clk->gate_ip_image);
	writel(CLK_GATE_IP_LCD0_VAL, (unsigned int)&clk->gate_ip_lcd0);
	writel(CLK_GATE_IP_LCD1_VAL, (unsigned int)&clk->gate_ip_lcd1);
	writel(CLK_GATE_IP_FSYS_VAL, (unsigned int)&clk->gate_ip_fsys);
	writel(CLK_GATE_IP_GPS_VAL, (unsigned int)&clk->gate_ip_gps);
	writel(CLK_GATE_IP_PERIL_VAL, (unsigned int)&clk->gate_ip_peril);
	writel(CLK_GATE_IP_PERIR_VAL, (unsigned int)&clk->gate_ip_perir);
	writel(CLK_GATE_BLOCK_VAL, (unsigned int)&clk->gate_block);
}

static void board_power_init(void)
{
	struct exynos4_power *pwr =
		(struct exynos4_power *)samsung_get_base_power();

	/* PS HOLD */
	writel(EXYNOS4_PS_HOLD_CON_VAL, (unsigned int)&pwr->ps_hold_control);

	/* Set power down */
	writel(0, (unsigned int)&pwr->cam_configuration);
	writel(0, (unsigned int)&pwr->tv_configuration);
	writel(0, (unsigned int)&pwr->mfc_configuration);
	writel(0, (unsigned int)&pwr->g3d_configuration);
	writel(0, (unsigned int)&pwr->lcd1_configuration);
	writel(0, (unsigned int)&pwr->gps_configuration);
	writel(0, (unsigned int)&pwr->gps_alive_configuration);

	/* It is necessary to power down core 1 */
	/* to successfully boot CPU1 in kernel */
	writel(0, (unsigned int)&pwr->arm_core1_configuration);
}

static void exynos_uart_init(void)
{
	/* UART_SEL GPY4[7] (part2) at EXYNOS4 */
	gpio_request(EXYNOS4_GPIO_Y47, "uart_sel");
	gpio_set_pull(EXYNOS4_GPIO_Y47, S5P_GPIO_PULL_UP);
	gpio_direction_output(EXYNOS4_GPIO_Y47, 1);
}

int exynos_early_init_f(void)
{
	wdt_stop();
	pmic_reset();
	board_clock_init();
	exynos_uart_init();
	board_power_init();

	return 0;
}

void exynos_reset_lcd(void)
{
	gpio_request(EXYNOS4_GPIO_Y45, "lcd_reset");
	gpio_direction_output(EXYNOS4_GPIO_Y45, 1);
	udelay(10000);
	gpio_direction_output(EXYNOS4_GPIO_Y45, 0);
	udelay(10000);
	gpio_direction_output(EXYNOS4_GPIO_Y45, 1);
}

int lcd_power(void)
{
	int ret = 0;
	struct pmic *p = pmic_get("MAX8997_PMIC");
	if (!p)
		return -ENODEV;

	if (pmic_probe(p))
		return 0;

	/* LDO15 voltage: 2.2v */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO15CTRL, 0x1c | EN_LDO);
	/* LDO13 voltage: 3.0v */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO13CTRL, 0x2c | EN_LDO);

	if (ret) {
		puts("MAX8997 LDO setting error!\n");
		return -1;
	}

	return 0;
}

int mipi_power(void)
{
	int ret = 0;
	struct pmic *p = pmic_get("MAX8997_PMIC");
	if (!p)
		return -ENODEV;

	if (pmic_probe(p))
		return 0;

	/* LDO3 voltage: 1.1v */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO3CTRL, 0x6 | EN_LDO);
	/* LDO4 voltage: 1.8v */
	ret |= pmic_reg_write(p, MAX8997_REG_LDO4CTRL, 0x14 | EN_LDO);

	if (ret) {
		puts("MAX8997 LDO setting error!\n");
		return -1;
	}

	return 0;
}

void exynos_lcd_misc_init(vidinfo_t *vid)
{
#ifdef CONFIG_TIZEN
	get_tizen_logo_info(vid);
#endif
#ifdef CONFIG_S6E8AX0
	s6e8ax0_init();
	setenv("lcdinfo", "lcd=s6e8ax0");
#endif
}
