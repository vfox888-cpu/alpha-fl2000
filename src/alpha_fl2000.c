// SPDX-License-Identifier: GPL-2.0
/*
 * alpha_fl2000 — DRM/KMS-драйвер USB→HDMI адаптера Fresco Logic FL2000 (1d5c:2000)
 * с HDMI-передатчиком ITE IT66121. Протокол сверен с github.com/klogg/fl2000_drm (GPL-2.0).
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_cache.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

static unsigned int max_mbytes;
module_param(max_mbytes, uint, 0444);
MODULE_PARM_DESC(max_mbytes, "Предел полосы USB для видео, МБ/с (0 = по скорости порта)");

#define FL_VID 0x1d5c
#define FL_PID 0x2000
#define FL_IF_AVCONTROL 0
#define STREAM_EP 1
#define NUM_URBS 2
#define CUR_SIZE 64

/* ---------------- Регистры FL2000 ---------------- */

#define FL_REQ_READ  64
#define FL_REQ_WRITE 65

#define FL_USB_LPM      0x0070
#define LPM_VENDOR_QUIRK BIT(13)
#define LPM_U2_REJECT   BIT(19)
#define LPM_U1_REJECT   BIT(20)
#define FL_USB_CTRL     0x0078
#define USB_WAKE_NRDY   BIT(17)

#define FL_PXCLK        0x8004
#define PX_CLEAR_WM     BIT(0)
#define PX_FRAME_SYNC   BIT(1)
#define PX_HSYNC_POS    BIT(2)
#define PX_VSYNC_POS    BIT(3)
#define PX_565          BIT(6)
#define PX_DAC_EN       BIT(7)
#define PX_DROP_CNT     BIT(13)
#define PX_COMPRESS     BIT(24)
#define PX_332          BIT(25)
#define PX_PALETTE      BIT(26)
#define PX_FIRST_BT     BIT(27)
#define PX_555          BIT(31)
#define FL_HSYNC1       0x8008
#define FL_HSYNC2       0x800c
#define FL_VSYNC1       0x8010
#define FL_VSYNC2       0x8014
#define FL_TEST         0x8018
#define FL_ISOCH        0x801c
#define ISOCH_MFRAME    GENMASK(29, 16)
#define FL_I2C_SC       0x8020
#define I2C_ADDR        GENMASK(6, 0)
#define I2C_CMD_READ    BIT(7)
#define I2C_OFFSET      GENMASK(15, 8)
#define I2C_STATUS      GENMASK(27, 24)
#define I2C_MON_DET     BIT(28)
#define I2C_EDID_DET    BIT(30)
#define I2C_DONE        BIT(31)
#define FL_I2C_RD       0x8024
#define FL_I2C_WR       0x8028
#define FL_PLL          0x802c
#define FL_ACLK         0x803c
#define ACLK_VGA_ERR_INT BIT(24)
#define ACLK_FORCE_CONN BIT(25)
#define ACLK_FORCE_PLL  BIT(26)
#define ACLK_ZERO_TD    BIT(27)
#define ACLK_ZERO_PKT   BIT(28)
#define ACLK_PKT_PEND   BIT(29)
#define FL_RST_CTRL     0x8048
#define RST_SW          BIT(15)
#define FL_CTRL3        0x8088
#define CTRL3_WAKE_CLR  BIT(10)

#define FL_XTAL         10000000U
#define FL_VCO_MIN      62500000U
#define FL_VCO_MAX      1000000000U
#define FL_CLK_MIN_KHZ  25000
#define FL_CLK_MAX_KHZ  148500
#define FL_PPM_MAX      500

/* ---------------- Регистры IT66121 ---------------- */

#define IT_ID_IT66121   0x06124954	/* 54 49 12 x6 */
#define IT_SW_RST       0x04
#define IT_SW_RST_VID   BIT(3)
#define IT_SW_RST_REF   BIT(5)
#define IT_INT          0x05
#define IT_INT_TXCLKOFF BIT(0)
#define IT_SYS_STATUS   0x0e
#define IT_HPD          BIT(6)
#define IT_CLK_BANK     0x0f
#define IT_PWROFF_TXCLK BIT(4)
#define IT_PWROFF_RCLK  BIT(6)
#define IT_AFE_DRV      0x61
#define IT_AFE_DRV_RST  BIT(4)
#define IT_AFE_DRV_PWD  BIT(5)
#define IT_AFE_XP       0x62
#define IT_AFE_XP_PWDI  BIT(2)
#define IT_AFE_XP_RESETB BIT(3)
#define IT_AFE_XP_ENO   BIT(4)
#define IT_AFE_XP_PWDPLL BIT(6)
#define IT_AFE_XP_GAIN  BIT(7)
#define IT_AFE_IP       0x64
#define IT_AFE_IP_EC1   BIT(0)
#define IT_AFE_IP_RESETB BIT(2)
#define IT_AFE_IP_ER0   BIT(3)
#define IT_AFE_IP_PWDPLL BIT(6)
#define IT_AFE_IP_GAIN  BIT(7)
#define IT_AFE_XP_EC1   0x68
#define IT_AFE_XP_EC1_LOWCLK BIT(4)
#define IT_INPUT_MODE   0x70
#define IT_INPUT_RGB_PCLK_DELAY1 0x01	/* так FL2000 разведён на плате */
#define IT_INPUT_CSC    0x72
#define IT_HDMI_MODE    0xc0
#define IT_AV_MUTE      0xc1
#define IT_AV_MUTE_ON   BIT(0)
#define IT_PKT_GEN      0xc6
#define IT_AVI_PKT      0xcd
#define IT_AFE_HIGH_KHZ 80000

#define EDID_I2C_ADDR   0x50

struct alpha_fl {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	struct usb_device *udev;
	struct mutex i2c_lock;		/* один движок I2C на мост и DDC монитора */
	u8 bridge;
	u64 max_bw;			/* байт/с, доступные под видео */

	/* Кадр в «проводном» формате FL2000, крутится по кругу в NUM_URBS передачах. */
	u8 *frame;
	size_t frame_size;
	unsigned int width, height, bpp;
	/* DMA-отображение пишется в сам scatterlist — у каждой передачи свой, иначе IOMMU ломается. */
	struct sg_table sgt[NUM_URBS];
	struct urb *urbs[NUM_URBS];
	atomic_t streaming;
	u32 *row;			/* рабочая строка для наложения курсора */

	/*
	 * Основной кадр и курсор приходят разными коммитами (курсор может идти асинхронно),
	 * поэтому сборка проводного кадра — под render_lock.
	 */
	struct mutex render_lock;
	u32 *mirror;			/* копия основного кадра XRGB8888 в обычной памяти */
	struct {
		u32 img[CUR_SIZE * CUR_SIZE];	/* ARGB, альфа предумножена */
		int x, y, w, h;
		bool on;
	} cur;

	/* Замеры обновлений кадра; «last_*» — снимок на момент прошлого чтения status. */
	struct {
		u64 updates, full, pixels, ns, ns_access, ns_max;
		u64 last_updates, last_pixels, last_ns, last_ns_access, last_t;
		bool imported;
	} st;
};

#define to_fl(d) container_of(d, struct alpha_fl, drm)

/* ---------------- Доступ к FL2000 ---------------- */

static int fl_read(struct usb_device *udev, u16 reg, u32 *val)
{
	__le32 d;
	int ret = usb_control_msg_recv(udev, 0, FL_REQ_READ, USB_DIR_IN | USB_TYPE_VENDOR,
				       0, reg, &d, sizeof(d), USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (!ret)
		*val = le32_to_cpu(d);
	return ret;
}

static int fl_write(struct usb_device *udev, u16 reg, u32 val)
{
	__le32 d = cpu_to_le32(val);

	return usb_control_msg_send(udev, 0, FL_REQ_WRITE, USB_DIR_OUT | USB_TYPE_VENDOR,
				    0, reg, &d, sizeof(d), USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
}

static int fl_update(struct usb_device *udev, u16 reg, u32 mask, u32 val)
{
	u32 v;
	int ret = fl_read(udev, reg, &v);

	return ret ? ret : fl_write(udev, reg, (v & ~mask) | (val & mask));
}

/* Движок I2C FL2000 работает только выровненными словами по 4 байта. */
static int fl_i2c_xfer32(struct alpha_fl *fl, bool read, u8 addr, u8 offset, u32 *data)
{
	struct usb_device *u = fl->udev;
	u32 sc;
	int ret, i;

	mutex_lock(&fl->i2c_lock);
	if (!read) {
		ret = fl_write(u, FL_I2C_WR, *data);
		if (ret)
			goto out;
	}
	ret = fl_read(u, FL_I2C_SC, &sc);
	if (ret)
		goto out;
	sc &= ~(I2C_ADDR | I2C_CMD_READ | I2C_OFFSET | I2C_STATUS | I2C_DONE);
	/* Биты автоопределения читаются как 0 — их нужно выставлять в каждой команде. */
	sc |= I2C_MON_DET | I2C_EDID_DET | FIELD_PREP(I2C_ADDR, addr) |
	      FIELD_PREP(I2C_OFFSET, offset) | (read ? I2C_CMD_READ : 0);
	ret = fl_write(u, FL_I2C_SC, sc);
	if (ret)
		goto out;

	for (i = 0; i < 1000; i++) {
		ret = fl_read(u, FL_I2C_SC, &sc);
		if (ret || (sc & I2C_DONE))
			break;
		usleep_range(200, 400);
	}
	if (!ret && !(sc & I2C_DONE))
		ret = -ETIMEDOUT;
	if (!ret && FIELD_GET(I2C_STATUS, sc))
		ret = -EIO;
	if (!ret && read)
		ret = fl_read(u, FL_I2C_RD, data);
out:
	mutex_unlock(&fl->i2c_lock);
	return ret;
}

static int fl_ctrl_init(struct usb_device *u)
{
	int ret = fl_update(u, FL_RST_CTRL, RST_SW, RST_SW);

	if (ret)
		return ret;
	msleep(10);
	/* Перезапуск автоопределения монитора (порядок из мануала FL2000). */
	ret = fl_update(u, FL_I2C_SC, I2C_MON_DET | I2C_EDID_DET, 0);
	if (!ret)
		ret = fl_update(u, FL_I2C_SC, I2C_MON_DET | I2C_EDID_DET, I2C_EDID_DET);
	if (ret)
		return ret;
	msleep(750);
	ret = fl_update(u, FL_I2C_SC, I2C_MON_DET | I2C_EDID_DET, I2C_MON_DET | I2C_EDID_DET);
	if (!ret)
		ret = fl_update(u, FL_CTRL3, CTRL3_WAKE_CLR, 0);
	if (!ret)
		ret = fl_update(u, FL_USB_LPM, LPM_U1_REJECT | LPM_U2_REJECT,
				LPM_U1_REJECT | LPM_U2_REJECT);
	if (!ret)
		ret = fl_update(u, FL_USB_CTRL, USB_WAKE_NRDY, 0);
	return ret;
}

/* ---------------- Частота пикселей и режим FL2000 ---------------- */

struct fl_pll {
	u32 prescaler, multiplier, divisor, function, clock;
};

/* Лучшая пара множитель/делитель от кварца 10 МГц; возвращает ошибку в ppm. */
static u32 fl_pll_calc(u64 target_hz, struct fl_pll *best)
{
	u32 best_err = U32_MAX, p, m;
	int k;

	for (p = 1; p <= 2; p++)
		for (m = 1; m <= 128; m++) {
			u32 vco = FL_XTAL / p * m;
			u32 d0;

			if (vco < FL_VCO_MIN || vco > FL_VCO_MAX)
				continue;
			d0 = (u32)div64_u64(vco + target_hz / 2, target_hz);
			for (k = -1; k <= 1; k++) {
				u32 d = d0 + k, clk, err;

				if (d < 2 || d > 128 || d == 3 || d == 5)
					continue;
				clk = vco / d;
				err = (u32)div64_u64(abs_diff((u64)clk, target_hz) * 1000000, target_hz);
				if (err >= best_err)
					continue;
				best_err = err;
				*best = (struct fl_pll){
					.prescaler = p, .multiplier = m, .divisor = d, .clock = clk,
					.function = vco < 125000000 ? 0 : vco < 250000000 ? 1 :
						    vco < 500000000 ? 2 : 3,
				};
			}
		}
	return best_err;
}

/* Если частота не ложится в PLL, сдвигаем ширину строки на ±10 тактов, как в эталоне. */
static int fl_pll_fit(const struct drm_display_mode *m, struct fl_pll *pll, int *htotal)
{
	int i;

	for (i = 0; i <= 20; i++) {
		int adj = (i & 1) ? -((i + 1) / 2) : i / 2;
		int ht = m->htotal + adj;

		if (ht < m->hsync_end || ht > 4095)
			continue;
		if (fl_pll_calc(div_u64((u64)m->clock * 1000 * ht, m->htotal), pll) < FL_PPM_MAX) {
			*htotal = ht;
			return 0;
		}
	}
	return -ERANGE;
}

/* Байт на пиксель на проводе: 3 (RGB888) если хватает полосы USB, иначе 2 (RGB565), иначе 0. */
static unsigned int fl_wire_bpp(const struct alpha_fl *fl, const struct drm_display_mode *m)
{
	u64 total = (u64)m->htotal * m->vtotal, rate;

	if (!total)
		return 0;
	rate = div64_u64((u64)m->clock * 1000 * m->hdisplay * m->vdisplay, total);
	if (!rate)
		return 0;
	if (fl->max_bw >= rate * 3)
		return 3;
	if (fl->max_bw >= rate * 2)
		return 2;
	return 0;
}

static int fl_set_mode(struct alpha_fl *fl, const struct drm_display_mode *m, unsigned int bpp)
{
	struct usb_device *u = fl->udev;
	struct fl_pll pll;
	u32 vstart;
	int htotal, ret;

	ret = fl_pll_fit(m, &pll, &htotal);
	if (ret)
		return ret;
	drm_dbg_kms(&fl->drm, "PLL %u*%u/%u = %u Гц, htotal %d\n", FL_XTAL / pll.prescaler,
		    pll.multiplier, pll.divisor, pll.clock, htotal);

	vstart = m->vtotal - m->vsync_start + 1;
	ret = fl_write(u, FL_PLL, pll.divisor | pll.prescaler << 8 | pll.function << 13 |
		       pll.multiplier << 16);
	if (!ret)
		ret = fl_update(u, FL_ACLK, ACLK_FORCE_PLL | ACLK_FORCE_CONN, ACLK_FORCE_PLL);
	if (!ret)
		ret = fl_write(u, FL_HSYNC1, htotal | (u32)m->hdisplay << 16);
	if (!ret)
		ret = fl_write(u, FL_HSYNC2, (htotal - m->hsync_start + 1) |
			       (u32)(m->hsync_end - m->hsync_start) << 16);
	if (!ret)
		ret = fl_write(u, FL_VSYNC1, m->vtotal | (u32)m->vdisplay << 16);
	if (!ret)
		ret = fl_write(u, FL_VSYNC2, vstart | (u32)(m->vsync_end - m->vsync_start) << 16 |
			       min_t(u32, vstart, 0x3ff) << 20);
	if (!ret)
		ret = fl_update(u, FL_PXCLK, PX_HSYNC_POS | PX_VSYNC_POS,
				(m->flags & DRM_MODE_FLAG_PHSYNC ? PX_HSYNC_POS : 0) |
				(m->flags & DRM_MODE_FLAG_PVSYNC ? PX_VSYNC_POS : 0));
	if (!ret)
		ret = fl_update(u, FL_PXCLK, PX_DROP_CNT | PX_FRAME_SYNC | PX_565 | PX_332 | PX_555 |
				PX_COMPRESS | PX_PALETTE | PX_FIRST_BT | PX_CLEAR_WM, 0);
	if (!ret)
		ret = fl_update(u, FL_PXCLK, PX_CLEAR_WM, PX_CLEAR_WM);
	if (!ret)
		ret = fl_update(u, FL_PXCLK, PX_565, bpp == 2 ? PX_565 : 0);
	if (!ret)
		ret = fl_update(u, FL_PXCLK, PX_DAC_EN, PX_DAC_EN);
	if (!ret)
		ret = fl_update(u, FL_ACLK, ACLK_PKT_PEND | ACLK_ZERO_TD | ACLK_ZERO_PKT |
				ACLK_VGA_ERR_INT, ACLK_ZERO_PKT | ACLK_VGA_ERR_INT);
	if (!ret)
		ret = fl_update(u, FL_ISOCH, ISOCH_MFRAME, 0);
	if (!ret)
		ret = fl_write(u, FL_TEST, 0);
	if (!ret)
		ret = fl_update(u, FL_USB_LPM, LPM_VENDOR_QUIRK, LPM_VENDOR_QUIRK);
	return ret;
}

/* ---------------- HDMI-передатчик IT66121 ---------------- */

static int it_read8(struct alpha_fl *fl, u8 reg, u8 *val)
{
	u32 w;
	int ret = fl_i2c_xfer32(fl, true, fl->bridge, reg & ~3, &w);

	if (!ret)
		*val = w >> ((reg & 3) * 8);
	return ret;
}

/* Чтение-изменение-запись слова; не для 0x14..0x17, где чтение съедает FIFO DDC. */
static int it_update8(struct alpha_fl *fl, u8 reg, u8 mask, u8 val)
{
	unsigned int sh = (reg & 3) * 8;
	u32 w;
	int ret = fl_i2c_xfer32(fl, true, fl->bridge, reg & ~3, &w);

	if (ret)
		return ret;
	w = (w & ~((u32)mask << sh)) | ((u32)(val & mask) << sh);
	return fl_i2c_xfer32(fl, false, fl->bridge, reg & ~3, &w);
}

static int it_apply(struct alpha_fl *fl, const u8 (*seq)[3], unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = it_update8(fl, seq[i][0], seq[i][1], seq[i][2]);
		if (ret)
			return ret;
	}
	return 0;
}

static int it_power_on(struct alpha_fl *fl)
{
	static const u8 seq[][3] = {
		{ IT_CLK_BANK, IT_PWROFF_RCLK, 0 },
		{ IT_INT, IT_INT_TXCLKOFF, 0 },
		{ IT_AFE_DRV, IT_AFE_DRV_PWD, 0 },
		{ IT_AFE_XP, IT_AFE_XP_PWDI | IT_AFE_XP_PWDPLL, 0 },
		{ IT_AFE_IP, IT_AFE_IP_PWDPLL, 0 },
		{ IT_AFE_DRV, IT_AFE_DRV_RST, 0 },
		{ IT_AFE_XP, IT_AFE_XP_RESETB, IT_AFE_XP_RESETB },
		{ IT_AFE_IP, IT_AFE_IP_RESETB, IT_AFE_IP_RESETB },
		{ IT_SW_RST, IT_SW_RST_REF, IT_SW_RST_REF },
	};
	int ret = it_apply(fl, seq, ARRAY_SIZE(seq));

	msleep(50);
	return ret;
}

/* Выход в режиме DVI (без инфокадров HDMI) — любому монитору этого достаточно. */
static int it_enable_output(struct alpha_fl *fl, u32 clock_khz)
{
	bool high = clock_khz > IT_AFE_HIGH_KHZ;
	const u8 seq[][3] = {
		{ IT_AVI_PKT, 0xff, 0 },
		{ IT_HDMI_MODE, 0xff, 0 },
		{ IT_CLK_BANK, IT_PWROFF_TXCLK, IT_PWROFF_TXCLK },
		{ IT_INPUT_MODE, 0xff, IT_INPUT_RGB_PCLK_DELAY1 },
		{ IT_INPUT_CSC, 0xff, 0 },
		{ IT_AFE_DRV, 0xff, IT_AFE_DRV_RST },
		{ IT_AFE_XP, IT_AFE_XP_GAIN | IT_AFE_XP_ENO, high ? IT_AFE_XP_GAIN : IT_AFE_XP_ENO },
		{ IT_AFE_IP, IT_AFE_IP_GAIN | IT_AFE_IP_ER0 | IT_AFE_IP_EC1,
		  high ? IT_AFE_IP_GAIN : IT_AFE_IP_ER0 | IT_AFE_IP_EC1 },
		{ IT_AFE_XP_EC1, IT_AFE_XP_EC1_LOWCLK, high ? 0 : IT_AFE_XP_EC1_LOWCLK },
		{ IT_SW_RST, IT_SW_RST_REF | IT_SW_RST_VID, 0 },
		{ IT_AFE_DRV, 0xff, 0 },
		{ IT_CLK_BANK, IT_PWROFF_TXCLK, 0 },
		{ IT_AV_MUTE, IT_AV_MUTE_ON, 0 },
		{ IT_PKT_GEN, 0xff, BIT(0) | BIT(1) },
	};

	return it_apply(fl, seq, ARRAY_SIZE(seq));
}

static int fl_find_bridge(struct alpha_fl *fl)
{
	static const u8 addrs[] = { 0x4c, 0x4d };
	unsigned int i;
	u32 id;

	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		if (fl_i2c_xfer32(fl, true, addrs[i], 0, &id))
			continue;
		if ((id & 0x0fffffff) == IT_ID_IT66121) {
			fl->bridge = addrs[i];
			return 0;
		}
	}
	return -ENODEV;
}

/* ---------------- Поток кадров по USB ---------------- */

/*
 * FL2000 ждёт пиксели подряд младшим байтом вперёд (RGB888 = B,G,R; RGB565 — 2 байта),
 * а в каждых 8 байтах две половины по 4 байта переставлены местами.
 */
static inline void wire_put(u8 *dst, size_t off, u32 xrgb, unsigned int bpp)
{
	if (bpp == 2) {
		u16 v = ((xrgb >> 8) & 0xf800) | ((xrgb >> 5) & 0x07e0) | ((xrgb >> 3) & 0x001f);

		dst[off ^ 4] = v;
		dst[(off + 1) ^ 4] = v >> 8;
		return;
	}
	dst[off ^ 4] = xrgb;
	dst[(off + 1) ^ 4] = xrgb >> 8;
	dst[(off + 2) ^ 4] = xrgb >> 16;
}

static void fl_urb_done(struct urb *urb)
{
	struct alpha_fl *fl = urb->context;

	if (!atomic_read(&fl->streaming))
		return;
	if (urb->status) {
		dev_err_ratelimited(&fl->udev->dev, "передача кадра: ошибка %d\n", urb->status);
		return;
	}
	if (usb_submit_urb(urb, GFP_ATOMIC))
		dev_err_ratelimited(&fl->udev->dev, "не удалось перезапустить передачу\n");
}

static void fl_stream_stop(struct alpha_fl *fl)
{
	unsigned int i;

	atomic_set(&fl->streaming, 0);
	for (i = 0; i < NUM_URBS; i++) {
		usb_kill_urb(fl->urbs[i]);
		usb_free_urb(fl->urbs[i]);
		fl->urbs[i] = NULL;
	}
	if (fl->frame) {
		for (i = 0; i < NUM_URBS; i++)
			sg_free_table(&fl->sgt[i]);
		vfree(fl->frame);
		fl->frame = NULL;
		kvfree(fl->row);
		fl->row = NULL;
		vfree(fl->mirror);
		fl->mirror = NULL;
	}
}

static int fl_stream_alloc(struct alpha_fl *fl, unsigned int w, unsigned int h, unsigned int bpp)
{
	struct page **pages;
	unsigned int i, n, npages;
	int ret = 0;

	fl->width = w;
	fl->height = h;
	fl->bpp = bpp;
	fl->frame_size = ALIGN((size_t)w * h * bpp, 8);
	fl->frame = vzalloc(fl->frame_size);
	if (!fl->frame)
		return -ENOMEM;
	fl->row = kvmalloc_array(w, sizeof(u32), GFP_KERNEL);
	fl->mirror = vzalloc(array3_size(w, h, sizeof(u32)));
	if (!fl->row || !fl->mirror) {
		ret = -ENOMEM;
		goto free_frame;
	}

	npages = DIV_ROUND_UP(fl->frame_size, PAGE_SIZE);
	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto free_frame;
	}
	for (i = 0; i < npages; i++)
		pages[i] = vmalloc_to_page(fl->frame + (size_t)i * PAGE_SIZE);
	for (n = 0; n < NUM_URBS; n++) {
		ret = sg_alloc_table_from_pages(&fl->sgt[n], pages, npages, 0, fl->frame_size,
						GFP_KERNEL);
		if (ret)
			break;
	}
	kvfree(pages);
	if (!ret)
		return 0;
	while (n--)
		sg_free_table(&fl->sgt[n]);
free_frame:
	vfree(fl->mirror);
	fl->mirror = NULL;
	kvfree(fl->row);
	fl->row = NULL;
	vfree(fl->frame);
	fl->frame = NULL;
	return ret;
}

static int fl_stream_submit(struct alpha_fl *fl)
{
	unsigned int i;
	int ret;

	atomic_set(&fl->streaming, 1);
	for (i = 0; i < NUM_URBS; i++) {
		struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!urb)
			return -ENOMEM;
		fl->urbs[i] = urb;
		usb_fill_bulk_urb(urb, fl->udev, usb_sndbulkpipe(fl->udev, STREAM_EP), NULL,
				  fl->frame_size, fl_urb_done, fl);
		urb->sg = fl->sgt[i].sgl;
		urb->num_sgs = fl->sgt[i].orig_nents;
		urb->transfer_flags = URB_ZERO_PACKET;
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret)
			return ret;
	}
	return 0;
}

static inline u16 rgb565(u32 p)
{
	return ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f);
}

/*
 * Строка из n пикселей начиная с пикселя idx кадра. Группа из 4 пикселей RGB888 (12 байт)
 * или 2 пикселей RGB565 (4 байта) начинается с адреса, кратного 4, а перестановка
 * половинок в 8 байтах — это просто запись 32-битного слова по адресу off ^ 4.
 */
static void fl_row_fast(u8 *dst, size_t idx, const u32 *src, int n, unsigned int bpp)
{
	size_t off = idx * bpp;

	if (bpp == 3) {
		for (; n && (idx & 3); n--, idx++, off += 3)
			wire_put(dst, off, *src++, 3);
		for (; n >= 4; n -= 4, idx += 4, off += 12, src += 4) {
			u32 a = src[0], b = src[1], c = src[2], d = src[3];

			*(__le32 *)(dst + (off ^ 4)) = cpu_to_le32((a & 0xffffff) | (b << 24));
			*(__le32 *)(dst + ((off + 4) ^ 4)) =
				cpu_to_le32(((b >> 8) & 0xffff) | (c << 16));
			*(__le32 *)(dst + ((off + 8) ^ 4)) =
				cpu_to_le32(((c >> 16) & 0xff) | (d << 8));
		}
	} else {
		for (; n && (idx & 1); n--, idx++, off += 2)
			wire_put(dst, off, *src++, 2);
		for (; n >= 2; n -= 2, idx += 2, off += 4, src += 2)
			*(__le32 *)(dst + (off ^ 4)) =
				cpu_to_le32(rgb565(src[0]) | (u32)rgb565(src[1]) << 16);
	}
	for (; n > 0; n--, off += bpp)
		wire_put(dst, off, *src++, bpp);
}

static inline u32 blend_px(u32 d, u32 s)
{
	u32 a = s >> 24, ia = 255 - a, r, g, b;

	if (a == 255)
		return s;
	if (!a)
		return d;
	r = ((s >> 16) & 0xff) + (((d >> 16) & 0xff) * ia + 127) / 255;
	g = ((s >> 8) & 0xff) + (((d >> 8) & 0xff) * ia + 127) / 255;
	b = (s & 0xff) + ((d & 0xff) * ia + 127) / 255;
	return min(r, 255U) << 16 | min(g, 255U) << 8 | min(b, 255U);
}

/* Собрать прямоугольник экрана: копия кадра + курсор поверх. Вызывать под render_lock. */
static void fl_render(struct alpha_fl *fl, struct drm_rect r)
{
	const struct drm_rect scr = DRM_RECT_INIT(0, 0, fl->width, fl->height);
	struct drm_rect cr = DRM_RECT_INIT(fl->cur.x, fl->cur.y, fl->cur.w, fl->cur.h);
	bool cursor;
	int y, x;

	if (!fl->frame || !drm_rect_intersect(&r, &scr))
		return;
	cursor = fl->cur.on && drm_rect_intersect(&cr, &r);
	for (y = r.y1; y < r.y2; y++) {
		const u32 *src = fl->mirror + (size_t)y * fl->width + r.x1;
		int n = r.x2 - r.x1;

		if (cursor && y >= cr.y1 && y < cr.y2) {
			const u32 *c = fl->cur.img + (y - fl->cur.y) * CUR_SIZE;

			memcpy(fl->row, src, n * 4);
			for (x = cr.x1; x < cr.x2; x++)
				fl->row[x - r.x1] = blend_px(fl->row[x - r.x1], c[x - fl->cur.x]);
			src = fl->row;
		}
		fl_row_fast(fl->frame, (size_t)y * fl->width + r.x1, src, n, fl->bpp);
		if (!(y & 63))
			cond_resched();
	}
}

/* Прямоугольник буфера GNOME (XRGB8888) → копия кадра → проводной кадр. */
static void fl_blit(struct alpha_fl *fl, const struct drm_plane_state *ps,
		    const struct iosys_map *map, const struct drm_rect *clip)
{
	const struct drm_framebuffer *fb = ps->fb;
	int sx = ps->src.x1 >> 16, sy = ps->src.y1 >> 16;
	int dx = ps->dst.x1, dy = ps->dst.y1;
	int y;

	for (y = clip->y1; y < clip->y2; y++) {
		int ty = y - sy + dy, x = max(clip->x1, sx - dx);
		int n = min(clip->x2, (int)fl->width + sx - dx) - x;
		struct iosys_map d, sm = *map;

		if (ty < 0 || ty >= fl->height || n <= 0)
			continue;
		/* Буфер часто лежит в памяти видеокарты (WC) — читаем широкими загрузками. */
		iosys_map_set_vaddr(&d, fl->mirror + (size_t)ty * fl->width + (x - sx + dx));
		iosys_map_incr(&sm, (size_t)y * fb->pitches[0] + (size_t)x * 4);
		drm_memcpy_from_wc(&d, &sm, (unsigned long)n * 4);
	}
	fl_render(fl, DRM_RECT_INIT(clip->x1 - sx + dx, clip->y1 - sy + dy,
				    drm_rect_width(clip), drm_rect_height(clip)));
}

static u64 fl_copy_plane(struct alpha_fl *fl, struct drm_plane_state *old,
			 struct drm_plane_state *ps, unsigned int *nclips)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(ps);
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	u64 t0, pixels = 0;

	*nclips = 0;
	if (!fl->frame || !ps->fb || !ps->visible)
		return 0;
	fl->st.imported = drm_gem_is_imported(ps->fb->obj[0]);
	t0 = ktime_get_ns();
	if (drm_gem_fb_begin_cpu_access(ps->fb, DMA_FROM_DEVICE))
		return 0;
	fl->st.ns_access += ktime_get_ns() - t0;
	if (old) {
		drm_atomic_helper_damage_iter_init(&iter, old, ps);
		drm_atomic_for_each_plane_damage(&iter, &clip) {
			(*nclips)++;
			pixels += (u64)drm_rect_width(&clip) * drm_rect_height(&clip);
			fl_blit(fl, ps, &shadow->data[0], &clip);
		}
	} else {
		clip = DRM_RECT_INIT(ps->src.x1 >> 16, ps->src.y1 >> 16,
				     drm_rect_width(&ps->src) >> 16, drm_rect_height(&ps->src) >> 16);
		*nclips = 1;
		pixels = (u64)drm_rect_width(&clip) * drm_rect_height(&clip);
		fl_blit(fl, ps, &shadow->data[0], &clip);
	}
	drm_gem_fb_end_cpu_access(ps->fb, DMA_FROM_DEVICE);
	return pixels;
}

/* ---------------- DRM: плоскости, CRTC, коннектор ---------------- */

static const u32 fl_formats[] = { DRM_FORMAT_XRGB8888 };
static const u32 fl_cursor_formats[] = { DRM_FORMAT_ARGB8888 };
static const u64 fl_modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };

static int fl_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	bool is_cursor = plane->type == DRM_PLANE_TYPE_CURSOR;
	struct drm_crtc_state *cs = NULL;

	if (is_cursor && ps->fb && (ps->crtc_w > CUR_SIZE || ps->crtc_h > CUR_SIZE))
		return -EINVAL;
	if (ps->crtc)
		cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING, is_cursor, is_cursor);
}

static void fl_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct alpha_fl *fl = to_fl(plane->dev);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct drm_crtc_state *cs;
	unsigned int nclips;
	u64 acc0, t0, px, dt;
	int idx;

	if (!ps->crtc)
		return;
	cs = drm_atomic_get_new_crtc_state(state, ps->crtc);
	/* При смене режима весь кадр копирует включение CRTC. */
	if (!cs->active || drm_atomic_crtc_needs_modeset(cs))
		return;
	if (!drm_dev_enter(plane->dev, &idx))
		return;
	mutex_lock(&fl->render_lock);
	acc0 = fl->st.ns_access;
	t0 = ktime_get_ns();
	px = fl_copy_plane(fl, old, ps, &nclips);
	dt = ktime_get_ns() - t0;
	mutex_unlock(&fl->render_lock);

	/* Медленные обновления — в журнал с разбивкой, чтобы видеть причину. */
	if (dt > 30 * NSEC_PER_MSEC)
		dev_info_ratelimited(fl->drm.dev,
			"медленно: %llu мс, %llu пикс в %u кусках, %llu мкс/Мпикс, доступ %llu мкс, буфер %s, ГПУ-буфер %d, cpu %d\n",
			div_u64(dt, NSEC_PER_MSEC), px, nclips, px ? div64_u64(dt * 1000, px) : 0,
			div_u64(fl->st.ns_access - acc0, 1000),
			old->fb != ps->fb ? "новый" : "тот же", fl->st.imported,
			raw_smp_processor_id());
	fl->st.updates++;
	fl->st.pixels += px;
	fl->st.ns += dt;
	fl->st.ns_max = max(fl->st.ns_max, dt);
	if (px >= (u64)fl->width * fl->height)
		fl->st.full++;
	drm_dev_exit(idx);
}

/* Курсор: запоминаем картинку и место, перерисовываем только старое и новое место. */
static void fl_cursor_set(struct alpha_fl *fl, struct drm_plane_state *ps)
{
	struct drm_rect was = DRM_RECT_INIT(fl->cur.x, fl->cur.y, fl->cur.w, fl->cur.h);
	bool was_on = fl->cur.on;

	fl->cur.on = false;
	if (ps && ps->fb && ps->visible &&
	    !drm_gem_fb_begin_cpu_access(ps->fb, DMA_FROM_DEVICE)) {
		struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(ps);
		/* Необрезанные координаты: обрезку по краю экрана делает fl_render. */
		int sx = ps->src_x >> 16, sy = ps->src_y >> 16;
		int w = min_t(int, ps->crtc_w, CUR_SIZE), h = min_t(int, ps->crtc_h, CUR_SIZE);
		int r;

		for (r = 0; r < h; r++) {
			struct iosys_map d = IOSYS_MAP_INIT_VADDR(fl->cur.img + r * CUR_SIZE);
			struct iosys_map sm = shadow->data[0];

			iosys_map_incr(&sm, (size_t)(sy + r) * ps->fb->pitches[0] + (size_t)sx * 4);
			drm_memcpy_from_wc(&d, &sm, w * 4);
		}
		drm_gem_fb_end_cpu_access(ps->fb, DMA_FROM_DEVICE);
		fl->cur.x = ps->crtc_x;
		fl->cur.y = ps->crtc_y;
		fl->cur.w = w;
		fl->cur.h = h;
		fl->cur.on = true;
	}
	if (was_on)
		fl_render(fl, was);
	if (fl->cur.on)
		fl_render(fl, DRM_RECT_INIT(fl->cur.x, fl->cur.y, fl->cur.w, fl->cur.h));
}

static void fl_cursor_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct alpha_fl *fl = to_fl(plane->dev);
	int idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	mutex_lock(&fl->render_lock);
	fl_cursor_set(fl, drm_atomic_get_new_plane_state(state, plane));
	mutex_unlock(&fl->render_lock);
	drm_dev_exit(idx);
}

static void fl_cursor_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct alpha_fl *fl = to_fl(plane->dev);
	int idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	mutex_lock(&fl->render_lock);
	fl_cursor_set(fl, NULL);
	mutex_unlock(&fl->render_lock);
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs fl_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl_plane_check,
	.atomic_update = fl_plane_update,
};

static const struct drm_plane_helper_funcs fl_cursor_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = fl_plane_check,
	.atomic_update = fl_cursor_update,
	.atomic_disable = fl_cursor_disable,
};

static const struct drm_plane_funcs fl_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static enum drm_mode_status fl_crtc_mode_valid(struct drm_crtc *crtc,
					       const struct drm_display_mode *m)
{
	struct alpha_fl *fl = to_fl(crtc->dev);
	struct fl_pll pll;
	int htotal;

	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return MODE_NO_INTERLACE;
	if (m->clock < FL_CLK_MIN_KHZ)
		return MODE_CLOCK_LOW;
	if (m->clock > FL_CLK_MAX_KHZ)
		return MODE_CLOCK_HIGH;
	if (m->htotal > 4095 || m->vtotal > 4095)
		return MODE_BAD;
	if (m->hsync_end - m->hsync_start > 255)
		return MODE_HSYNC_WIDE;
	if (m->vsync_end - m->vsync_start > 7)
		return MODE_VSYNC_WIDE;
	if (!fl_wire_bpp(fl, m))
		return MODE_BAD;
	if (fl_pll_fit(m, &pll, &htotal))
		return MODE_CLOCK_RANGE;
	return MODE_OK;
}

static int fl_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	int ret = drm_crtc_helper_atomic_check(crtc, state);

	if (ret)
		return ret;
	/* Настоящего vblank у USB нет — события перелистывания шлёт DRM сам. */
	cs->no_vblank = true;
	return drm_atomic_add_affected_planes(state, crtc);
}

static void fl_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct alpha_fl *fl = to_fl(crtc->dev);
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, &fl->plane);
	const struct drm_display_mode *m = &cs->adjusted_mode;
	unsigned int bpp = fl_wire_bpp(fl, m);
	int idx, ret;

	if (!drm_dev_enter(crtc->dev, &idx))
		return;
	ret = fl_set_mode(fl, m, bpp);
	mutex_lock(&fl->render_lock);
	if (!ret)
		ret = fl_stream_alloc(fl, m->hdisplay, m->vdisplay, bpp);
	if (!ret) {
		unsigned int nclips;

		if (ps && ps->fb && ps->visible)
			fl_copy_plane(fl, NULL, ps, &nclips);
		else
			fl_render(fl, DRM_RECT_INIT(0, 0, fl->width, fl->height));
		ret = fl_stream_submit(fl);
	}
	mutex_unlock(&fl->render_lock);
	if (!ret)
		ret = it_enable_output(fl, m->clock);
	if (ret) {
		drm_err(&fl->drm, "не удалось включить %dx%d: %d\n", m->hdisplay, m->vdisplay, ret);
		mutex_lock(&fl->render_lock);
		fl_stream_stop(fl);
		mutex_unlock(&fl->render_lock);
	} else {
		drm_info(&fl->drm, "режим %dx%d@%d, %u бит на пиксель\n", m->hdisplay, m->vdisplay,
			 drm_mode_vrefresh(m), bpp * 8);
	}
	drm_dev_exit(idx);
}

static void fl_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct alpha_fl *fl = to_fl(crtc->dev);
	int idx;

	if (drm_dev_enter(crtc->dev, &idx)) {
		it_update8(fl, IT_AV_MUTE, IT_AV_MUTE_ON, IT_AV_MUTE_ON);
		drm_dev_exit(idx);
	}
	mutex_lock(&fl->render_lock);
	fl_stream_stop(fl);
	mutex_unlock(&fl->render_lock);
}

static const struct drm_crtc_helper_funcs fl_crtc_helper_funcs = {
	.mode_valid = fl_crtc_mode_valid,
	.atomic_check = fl_crtc_check,
	.atomic_enable = fl_crtc_enable,
	.atomic_disable = fl_crtc_disable,
};

static const struct drm_crtc_funcs fl_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs fl_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/* DDC монитора на той же шине I2C, что и мост: EDID читается с 0x50 напрямую. */
static int fl_edid_block(void *ctx, u8 *buf, unsigned int block, size_t len)
{
	struct alpha_fl *fl = ctx;
	unsigned int base = (block & 1) * 128, off;
	u8 bytes[4];
	u32 w;
	int ret;

	if (block > 1)
		return -EINVAL;	/* сегменты E-DDC (больше двух блоков) не поддерживаются */
	for (off = 0; off < len; off += 4) {
		ret = fl_i2c_xfer32(fl, true, EDID_I2C_ADDR, base + off, &w);
		if (ret)
			return ret;
		put_unaligned_le32(w, bytes);
		memcpy(buf + off, bytes, min_t(size_t, 4, len - off));
	}
	return 0;
}

static int fl_conn_get_modes(struct drm_connector *c)
{
	struct alpha_fl *fl = to_fl(c->dev);
	const struct drm_edid *edid = drm_edid_read_custom(c, fl_edid_block, fl);
	int n;

	drm_edid_connector_update(c, edid);
	n = drm_edid_connector_add_modes(c);
	drm_edid_free(edid);
	return n;
}

static int fl_conn_detect(struct drm_connector *c, struct drm_modeset_acquire_ctx *ctx,
			  bool force)
{
	struct alpha_fl *fl = to_fl(c->dev);
	int idx, status = connector_status_disconnected;
	u8 st;

	if (!drm_dev_enter(c->dev, &idx))
		return connector_status_disconnected;
	if (it_read8(fl, IT_SYS_STATUS, &st))
		status = connector_status_unknown;
	else if (st & IT_HPD)
		status = connector_status_connected;
	drm_dev_exit(idx);
	return status;
}

static const struct drm_connector_helper_funcs fl_conn_helper_funcs = {
	.get_modes = fl_conn_get_modes,
	.detect_ctx = fl_conn_detect,
};

static const struct drm_connector_funcs fl_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs fl_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(fl_fops);

/*
 * Чужие буферы (из видеопамяти AMD) не принимаем: процессор читает их в 30–40 раз медленнее
 * обычной памяти, отсюда рывки. Получив отказ, GNOME сам копирует кадр силами видеокарты
 * в наш буфер в ОЗУ. Свои экспортированные буферы принимаем как обычно.
 */
static struct drm_gem_object *fl_prime_import(struct drm_device *dev, struct dma_buf *buf)
{
	if (drm_gem_is_prime_exported_dma_buf(dev, buf))
		return drm_gem_shmem_prime_import_no_map(dev, buf);
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct drm_driver fl_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops = &fl_fops,
	.gem_prime_import = fl_prime_import,
	.dumb_create = drm_gem_shmem_dumb_create,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
	.name = "alpha_fl2000",
	.desc = "Alpha FL2000 USB display",
	.major = 1,
	.minor = 0,
};

static int fl_pipeline_init(struct alpha_fl *fl)
{
	struct drm_device *drm = &fl->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &fl_mode_config_funcs;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 4095;
	drm->mode_config.max_height = 4095;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.cursor_width = CUR_SIZE;
	drm->mode_config.cursor_height = CUR_SIZE;

	ret = drm_universal_plane_init(drm, &fl->plane, 0, &fl_plane_funcs, fl_formats,
				       ARRAY_SIZE(fl_formats), fl_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&fl->plane, &fl_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&fl->plane);

	ret = drm_universal_plane_init(drm, &fl->cursor, 0, &fl_plane_funcs, fl_cursor_formats,
				       ARRAY_SIZE(fl_cursor_formats), fl_modifiers,
				       DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&fl->cursor, &fl_cursor_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &fl->crtc, &fl->plane, &fl->cursor, &fl_crtc_funcs,
					NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&fl->crtc, &fl_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &fl->encoder, &fl_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	fl->encoder.possible_crtcs = drm_crtc_mask(&fl->crtc);

	ret = drm_connector_init(drm, &fl->connector, &fl_conn_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	drm_connector_helper_add(&fl->connector, &fl_conn_helper_funcs);
	/* Прерывание HPD через USB не используем — DRM опрашивает разъём сам. */
	fl->connector.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
	return drm_connector_attach_encoder(&fl->connector, &fl->encoder);
}

/* ---------------- Диагностика: /sys/.../status ---------------- */

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct alpha_fl *fl = usb_get_intfdata(to_usb_interface(dev));
	u32 a = 0, b = 0, px = 0, aclk = 0;
	u8 sys = 0;

	fl_read(fl->udev, 0x8000, &a);
	msleep(50);
	fl_read(fl->udev, 0x8000, &b);
	fl_read(fl->udev, FL_PXCLK, &px);
	fl_read(fl->udev, FL_ACLK, &aclk);
	it_read8(fl, IT_SYS_STATUS, &sys);
	{
		u64 now = ktime_get_ns(), dt = now - fl->st.last_t;
		u64 du = fl->st.updates - fl->st.last_updates;
		u64 dp = fl->st.pixels - fl->st.last_pixels;
		u64 dns = fl->st.ns - fl->st.last_ns;
		u64 dacc = fl->st.ns_access - fl->st.last_ns_access;
		int len = sysfs_emit(buf,
			"обновлений %llu (полных %llu), буфер из видеокарты %d, курсор %d %d,%d\n"
			"за %llu мс: %llu обн/с, в среднем %llu пикс/обн, %llu мкс/обн "
			"(доступ %llu мкс), максимум %llu мкс\n",
			fl->st.updates, fl->st.full, fl->st.imported, fl->cur.on, fl->cur.x, fl->cur.y,
			div_u64(dt, 1000000), dt ? div64_u64(du * 1000000000ULL, dt) : 0,
			du ? div64_u64(dp, du) : 0, du ? div64_u64(dns, du * 1000) : 0,
			du ? div64_u64(dacc, du * 1000) : 0, div_u64(fl->st.ns_max, 1000));

		fl->st.last_t = now;
		fl->st.last_updates = fl->st.updates;
		fl->st.last_pixels = fl->st.pixels;
		fl->st.last_ns = fl->st.ns;
		fl->st.last_ns_access = fl->st.ns_access;
		fl->st.ns_max = 0;
		return len + sysfs_emit_at(buf, len,
		"vga_status %08x -> %08x кадры %u -> %u lbuf_over %d under %d\n"
		"pxclk %08x aclk %08x\n"
		"it66121 sys %02x hpd %d rxsense %d vid_stable %d\n"
		"поток %d %ux%u %u байт/пикс\n",
		a, b, (a >> 10) & 0xffff, (b >> 10) & 0xffff, !!(b & BIT(9)), !!(b & BIT(8)),
		px, aclk, sys, !!(sys & BIT(6)), !!(sys & BIT(5)), !!(sys & BIT(4)),
		atomic_read(&fl->streaming), fl->width, fl->height, fl->bpp);
	}
}
/* Координаты курсора и частота апдейтов — диагностика, не для всех локальных пользователей. */
static DEVICE_ATTR(status, 0400, status_show, NULL);

/* ---------------- USB ---------------- */

static int alpha_fl_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct device *dma_dev;
	struct alpha_fl *fl;
	int ret;

	fl = devm_drm_dev_alloc(&intf->dev, &fl_drm_driver, struct alpha_fl, drm);
	if (IS_ERR(fl))
		return PTR_ERR(fl);
	fl->udev = udev;
	ret = drmm_mutex_init(&fl->drm, &fl->i2c_lock);
	if (!ret)
		ret = drmm_mutex_init(&fl->drm, &fl->render_lock);
	if (ret)
		return ret;
	if (max_mbytes)
		fl->max_bw = (u64)max_mbytes * 1000000;
	else if (udev->speed >= USB_SPEED_SUPER)
		fl->max_bw = 5000000000ULL * 80 / 100 / 8;	/* 80% от 5 Гбит/с */
	else
		fl->max_bw = 480000000ULL * 80 / 100 / 8;

	ret = fl_ctrl_init(udev);
	if (!ret)
		ret = usb_set_interface(udev, FL_IF_AVCONTROL, 1);
	if (ret)
		return dev_err_probe(&intf->dev, ret, "начальная настройка FL2000\n");
	ret = fl_find_bridge(fl);
	if (ret)
		return dev_err_probe(&intf->dev, ret, "HDMI-передатчик IT66121 не найден\n");
	ret = it_power_on(fl);
	if (ret)
		return dev_err_probe(&intf->dev, ret, "включение IT66121\n");

	dma_dev = usb_intf_get_dma_device(intf);
	if (dma_dev) {
		drm_dev_set_dma_dev(&fl->drm, dma_dev);
		put_device(dma_dev);
	}
	ret = fl_pipeline_init(fl);
	if (ret)
		return ret;
	drm_mode_config_reset(&fl->drm);
	usb_set_intfdata(intf, fl);

	ret = device_create_file(&intf->dev, &dev_attr_status);
	if (ret)
		return ret;
	drm_kms_helper_poll_init(&fl->drm);
	ret = drm_dev_register(&fl->drm, 0);
	if (ret) {
		drm_kms_helper_poll_fini(&fl->drm);
		device_remove_file(&intf->dev, &dev_attr_status);
		return ret;
	}
	drm_info(&fl->drm, "FL2000 (%s), IT66121 @0x%02x, полоса %llu МБ/с\n",
		 usb_speed_string(udev->speed), fl->bridge, div_u64(fl->max_bw, 1000000));
	drm_client_setup_with_fourcc(&fl->drm, DRM_FORMAT_XRGB8888);
	return 0;
}

static void alpha_fl_disconnect(struct usb_interface *intf)
{
	struct alpha_fl *fl = usb_get_intfdata(intf);

	device_remove_file(&intf->dev, &dev_attr_status);
	drm_kms_helper_poll_fini(&fl->drm);
	drm_dev_unplug(&fl->drm);
	drm_atomic_helper_shutdown(&fl->drm);
	mutex_lock(&fl->render_lock);
	fl_stream_stop(fl);
	mutex_unlock(&fl->render_lock);
}

static const struct usb_device_id alpha_fl_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(FL_VID, FL_PID, FL_IF_AVCONTROL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, alpha_fl_ids);

static struct usb_driver alpha_fl_driver = {
	.name = KBUILD_MODNAME,
	.probe = alpha_fl_probe,
	.disconnect = alpha_fl_disconnect,
	.id_table = alpha_fl_ids,
};
module_usb_driver(alpha_fl_driver);

MODULE_DESCRIPTION("Alpha FL2000 USB display driver");
MODULE_LICENSE("GPL");
