/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012-2014 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _WIN32
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")
#endif

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef _WIN32
#include <unistd.h>
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#include <libusb.h>
#ifndef WIN32
#include "rtlsdr_rpc.h"
#endif

#include <pthread.h>

/* cond dumbness */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)


/*
 * All libusb callback functions should be marked with the LIBUSB_CALL macro
 * to ensure that they are compiled with the same calling convention as libusb.
 *
 * If the macro isn't available in older libusb versions, we simply define it.
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

/* libusb < 1.0.9 doesn't have libusb_handle_events_timeout_completed */
#ifndef HAVE_LIBUSB_HANDLE_EVENTS_TIMEOUT_COMPLETED
#define libusb_handle_events_timeout_completed(ctx, tv, c) \
	libusb_handle_events_timeout(ctx, tv)
#endif

/* two raised to the power of n */
#define TWO_POW(n)		((double)(1ULL<<(n)))

#include "rtl-sdr.h"
#include "tuner_e4k.h"
#include "tuner_fc0012.h"
#include "tuner_fc0013.h"
#include "tuner_fc2580.h"
#include "tuner_r82xx.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
	#include <sys/socket.h>
	#include <sys/un.h>
#endif
#include <signal.h>


typedef struct rtlsdr_tuner_iface {
	/* tuner interface */
	int (*init)(void *);
	int (*exit)(void *);
	int (*set_freq)(void *, uint32_t freq /* Hz */);
	int (*set_bw)(void *, int bw /* Hz */, uint32_t *applied_bw /* configured bw in Hz */, int apply /* 1 == configure it!, 0 == deliver applied_bw */);
	int (*set_bw_center)(void *, int32_t if_band_center_freq);
	int (*set_gain)(void *, int gain /* tenth dB */);
	int (*set_if_gain)(void *, int stage, int gain /* tenth dB */);
	int (*set_gain_mode)(void *, int manual);
	int (*set_i2c_register)(void *, unsigned i2c_register, unsigned data /* byte */, unsigned mask /* byte */ );
	int (*set_i2c_override)(void *, unsigned i2c_register, unsigned data /* byte */, unsigned mask /* byte */ );
	unsigned (*get_i2c_register)(void *, int i2c_register);
} rtlsdr_tuner_iface_t;

enum rtlsdr_async_status {
	RTLSDR_INACTIVE = 0,
	RTLSDR_CANCELING,
	RTLSDR_RUNNING
};

#define FIR_LEN 16

/*
 * FIR coefficients.
 *
 * The filter is running at XTal frequency. It is symmetric filter with 32
 * coefficients. Only first 16 coefficients are specified, the other 16
 * use the same values but in reversed order. The first coefficient in
 * the array is the outer one, the last, the last is the inner one.
 * First 8 coefficients are 8 bit signed integers, the next 8 coefficients
 * are 12 bit signed integers. All coefficients have the same weight.
 *
 * Default FIR coefficients used for DAB/FM by the Windows driver,
 * the DVB driver uses different ones
 */
static const int fir_default[FIR_LEN] = {
	-54, -36, -41, -40, -32, -14, 14, 53,	/* 8 bit signed */
	101, 156, 215, 273, 327, 372, 404, 421	/* 12 bit signed */
};


enum softagc_mode {
	SOFTAGC_OFF = 0,	/* off */
	SOFTAGC_ON_CHANGE,	/* activate on initial start and on relevant changes .. and deactivate afterwards */
	SOFTAGC_AUTO_ATTEN,	/* operate full time - but do only attenuate after initial control (ON_CHANGE) */
	SOFTAGC_AUTO		/* operate full time - attenuate and gain */
};

enum softagc_stateT {
	SOFTSTATE_OFF = 0,
	SOFTSTATE_ON,
	SOFTSTATE_RESET_CONT,
	SOFTSTATE_RESET,
	SOFTSTATE_INIT
};

struct softagc_state {
	pthread_t		command_thread;
	pthread_mutex_t	mutex;
	pthread_cond_t	cond;
	volatile int	exit_command_thread;
	volatile int	command_newGain;
	volatile int	command_changeGain;

	enum softagc_stateT	agcState;	/* active: don't forward samples while active for initial measurement */
	enum softagc_mode	softAgcMode;

	float	scanTimeMs;         /* scan duration per gain level - to look for maximum */
	float	deadTimeMs;         /* dead time in ms - after changing tuner gain */
	int		scanTimeSps;        /* scan duration in samples */
	int		deadTimeSps;        /* dead time in samples */
	volatile int	remainingDeadSps;   /* dead time in samples */
	int		remainingScanSps;   /* scan duration in samples */
	int		numInHisto;         /* number of values in histogram */
	int		histo[16];          /* count histogram over high 4 bits */

	int		gainIdx;            /* currently tested gain idx */
	int		softAgcBiasT;

	int		rpcNumGains;		/* local copy for RPC speedup */
	int *	rpcGainValues;
};

struct rtlsdr_dev {
	libusb_context *ctx;
	struct libusb_device_handle *devh;
	uint32_t xfer_buf_num;
	uint32_t xfer_buf_len;
	struct libusb_transfer **xfer;
	unsigned char **xfer_buf;
	rtlsdr_read_async_cb_t cb;
	void *cb_ctx;
	enum rtlsdr_async_status async_status;
	int async_cancel;
	int use_zerocopy;
	/* rtl demod context */
	uint32_t rate; /* Hz */
	uint32_t rtl_xtal; /* Hz */
	int fir[FIR_LEN];
	int direct_sampling;
	int rtl_vga_control;
	/* tuner context */
	enum rtlsdr_tuner tuner_type;
	rtlsdr_tuner_iface_t *tuner;
	uint32_t tun_xtal; /* Hz */
	uint32_t freq; /* Hz */
	uint32_t bw;
	uint32_t offs_freq; /* Hz */
	int32_t  if_band_center_freq; /* Hz - rtlsdr_set_tuner_band_center() */
	int corr; /* ppm */
	int gain; /* tenth dB */
	enum rtlsdr_ds_mode direct_sampling_mode;
	uint32_t direct_sampling_threshold; /* Hz */
	struct e4k_state e4k_s;
	struct r82xx_config r82xx_c;
	struct r82xx_priv r82xx_p;
	/* soft tuner agc */
	struct softagc_state softagc;

	/* UDP controller server */
#ifdef WITH_UDP_SERVER
#define UDP_TX_BUFLEN   1024
	unsigned udpPortNo;		/* default: 32323 */
	int      override_if_freq;
	int      override_if_flag;
	int      last_if_freq;
	pthread_t srv_thread;
	SOCKET   udpS;
	struct sockaddr_in server;
	struct sockaddr_in si_other;
	int      srv_started;
	int      slen;
	int      recv_len;
	char     buf[UDP_TX_BUFLEN];
	WSADATA  wsa;
#endif

	/* status */
	int dev_lost;
	int driver_active;
	unsigned int xfer_errors;
	int rc_active;
	int verbose;
	int dev_num;
	uint8_t saved_27;
	int handled;
};

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val);
static int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len);
static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq);
static int rtlsdr_update_ds(rtlsdr_dev_t *dev, uint32_t freq);

static void softagc_init(rtlsdr_dev_t *dev);
static void softagc_uninit(rtlsdr_dev_t *dev);
static int reactivate_softagc(rtlsdr_dev_t *dev, enum softagc_stateT newState);

/* generic tuner interface functions, shall be moved to the tuner implementations */
int e4000_init(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	devt->e4k_s.i2c_addr = E4K_I2C_ADDR;
	rtlsdr_get_xtal_freq(devt, NULL, &devt->e4k_s.vco.fosc);
	devt->e4k_s.rtl_dev = dev;
	return e4k_init(&devt->e4k_s);
}
int e4000_exit(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_standby(&devt->e4k_s, 1);
}
int e4000_set_freq(void *dev, uint32_t freq) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_tune_freq(&devt->e4k_s, freq);
}

int e4000_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) {
	int r = 0;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	if(!apply)
		return 0;

	r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_MIX, bw);
	r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_RC, bw);
	r |= e4k_if_filter_bw_set(&devt->e4k_s, E4K_IF_FILTER_CHAN, bw);

	return r;
}

int e4000_set_gain(void *dev, int gain) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	int mixgain = (gain > 340) ? 12 : 4;
#if 0
	int enhgain = (gain - 420);
#endif
	if(e4k_set_lna_gain(&devt->e4k_s, min(300, gain - mixgain * 10)) == -EINVAL)
		return -1;
	if(e4k_mixer_gain_set(&devt->e4k_s, mixgain) == -EINVAL)
		return -1;
#if 0 /* enhanced mixer gain seems to have no effect */
	if(enhgain >= 0)
		if(e4k_set_enh_gain(&devt->e4k_s, enhgain) == -EINVAL)
			return -1;
#endif
	return 0;
}
int e4000_set_if_gain(void *dev, int stage, int gain) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_if_gain_set(&devt->e4k_s, (uint8_t)stage, (int8_t)(gain / 10));
}
int e4000_set_gain_mode(void *dev, int manual) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return e4k_enable_manual_gain(&devt->e4k_s, manual);
}

int _fc0012_init(void *dev) { return fc0012_init(dev); }
int fc0012_exit(void *dev) { return 0; }
int fc0012_set_freq(void *dev, uint32_t freq) {
	/* select V-band/U-band filter */
	rtlsdr_set_gpio_bit(dev, 6, (freq > 300000000) ? 1 : 0);
	return fc0012_set_params(dev, freq, 6000000);
}
int fc0012_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) { return 0; }
int _fc0012_set_gain(void *dev, int gain) { return fc0012_set_gain(dev, gain); }
int fc0012_set_gain_mode(void *dev, int manual) { return 0; }

int _fc0013_init(void *dev) { return fc0013_init(dev); }
int fc0013_exit(void *dev) { return 0; }
int fc0013_set_freq(void *dev, uint32_t freq) {
	return fc0013_set_params(dev, freq, 6000000);
}
int fc0013_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) { return 0; }
int _fc0013_set_gain(void *dev, int gain) { return fc0013_set_lna_gain(dev, gain); }

int fc2580_init(void *dev) { return fc2580_Initialize(dev); }
int fc2580_exit(void *dev) { return 0; }
int _fc2580_set_freq(void *dev, uint32_t freq) {
	return fc2580_SetRfFreqHz(dev, freq);
}
int fc2580_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) {
	if(!apply)
		return 0;
	return fc2580_SetBandwidthMode(dev, 1);
}
int fc2580_set_gain(void *dev, int gain) { return 0; }
int fc2580_set_gain_mode(void *dev, int manual) { return 0; }

int r820t_init(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	devt->r82xx_p.rtl_dev = dev;

	if (devt->tuner_type == RTLSDR_TUNER_R828D) {
		devt->r82xx_c.i2c_addr = R828D_I2C_ADDR;
		devt->r82xx_c.rafael_chip = CHIP_R828D;
	} else {
		devt->r82xx_c.i2c_addr = R820T_I2C_ADDR;
		devt->r82xx_c.rafael_chip = CHIP_R820T;
	}

	rtlsdr_get_xtal_freq(devt, NULL, &devt->r82xx_c.xtal);

	devt->r82xx_c.max_i2c_msg_len = 8;
	devt->r82xx_c.use_predetect = 0;
	devt->r82xx_p.cfg = &devt->r82xx_c;

	return r82xx_init(&devt->r82xx_p);
}
int r820t_exit(void *dev) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_standby(&devt->r82xx_p);
}

int r820t_set_freq(void *dev, uint32_t freq) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_freq(&devt->r82xx_p, freq);
}

#define BWC_MUL_SIGN		+

int r820t_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply) {
	int r, iffreq;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;

	iffreq = r82xx_set_bandwidth(&devt->r82xx_p, bw, devt->rate, applied_bw, apply);
	if(!apply)
		return 0;
	if(iffreq < 0) {
		r = iffreq;
		return r;
	}

	iffreq = iffreq BWC_MUL_SIGN devt->if_band_center_freq;
	r = rtlsdr_set_if_freq(devt, iffreq );
	if (r)
		return r;

	return rtlsdr_set_center_freq(devt, devt->freq);
}

int r820t_set_bw_center(void *dev, int32_t if_band_center_freq) {
	int r, iffreq;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;

	iffreq = r82xx_set_bw_center(&devt->r82xx_p, if_band_center_freq);
	if(iffreq < 0) {
		r = iffreq;
		return r;
	}

	devt->if_band_center_freq = if_band_center_freq;
	iffreq = iffreq BWC_MUL_SIGN devt->if_band_center_freq;
	r = rtlsdr_set_if_freq(devt, iffreq );
	if (r)
		return r;

	return rtlsdr_set_center_freq(devt, devt->freq);
}

int rtlsdr_vga_control( rtlsdr_dev_t* devt, int rc, int rtl_vga_control ) {
	if (rc < 0)
		return rc;
	if ( rtl_vga_control != devt->rtl_vga_control ) {
		/* enable/disable RF AGC loop */

		rc = rtlsdr_demod_write_reg(devt, 1, 0x04, rtl_vga_control ? 0x80 : 0x00, 1);
		if ( devt->verbose )
			fprintf(stderr, "rtlsdr_vga_control(%s) returned %d\n"
				, rtl_vga_control ? "activate" : "deactivate", rc );
		devt->rtl_vga_control = rtl_vga_control;
	}
	return rc;
}

int r820t_set_gain(void *dev, int gain) {
	int rc, rtl_vga_control = 0;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	rc = r82xx_set_gain(&devt->r82xx_p, 1, gain, 0, 0, 0, 0, &rtl_vga_control);
	rc = rtlsdr_vga_control(devt, rc, rtl_vga_control);
	return rc;
}

int r820t_set_gain_ext(void *dev, int lna_gain, int mixer_gain, int vga_gain) {
	int rc, rtl_vga_control = 0;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	rc = r82xx_set_gain(&devt->r82xx_p, 0, 0, 1, lna_gain, mixer_gain, vga_gain, &rtl_vga_control);
	rc = rtlsdr_vga_control(devt, rc, rtl_vga_control);
	return rc;
}

int r820t_set_agc_mode(void *dev, int agc_variant) {
	int rc, rtl_vga_control = 0;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	rc = r82xx_set_agc_mode(&devt->r82xx_p, agc_variant, &rtl_vga_control);
	rc = rtlsdr_vga_control(devt, rc, rtl_vga_control);
	return rc;
}

int r820t_set_gain_mode(void *dev, int manual) {
	int rc, rtl_vga_control = 0;
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	rc = r82xx_set_gain(&devt->r82xx_p, manual, 0, 0, 0, 0, 0, &rtl_vga_control);
	rc = rtlsdr_vga_control(devt, rc, rtl_vga_control);
	return rc;
}


unsigned r820t_get_i2c_register(void *dev, int reg) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_read_cache_reg(&devt->r82xx_p,reg);
}
int r820t_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask ) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_i2c_register(&devt->r82xx_p, i2c_register, data, mask);
}
int r820t_set_i2c_override(void *dev, unsigned i2c_register, unsigned data, unsigned mask ) {
	rtlsdr_dev_t* devt = (rtlsdr_dev_t*)dev;
	return r82xx_set_i2c_override(&devt->r82xx_p, i2c_register, data, mask);
}


/* definition order must match enum rtlsdr_tuner */
static rtlsdr_tuner_iface_t tuners[] = {
	{
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL /* dummy for unknown tuners */
	},
	{
		e4000_init, e4000_exit,
		e4000_set_freq, e4000_set_bw, NULL, e4000_set_gain, e4000_set_if_gain,
		e4000_set_gain_mode, NULL, NULL, NULL
	},
	{
		_fc0012_init, fc0012_exit,
		fc0012_set_freq, fc0012_set_bw, NULL, _fc0012_set_gain, NULL,
		fc0012_set_gain_mode, NULL, NULL, NULL
	},
	{
		_fc0013_init, fc0013_exit,
		fc0013_set_freq, fc0013_set_bw, NULL, _fc0013_set_gain, NULL,
		fc0013_set_gain_mode, NULL, NULL, NULL
	},
	{
		fc2580_init, fc2580_exit,
		_fc2580_set_freq, fc2580_set_bw, NULL, fc2580_set_gain, NULL,
		fc2580_set_gain_mode, NULL, NULL, NULL
	},
	{
		r820t_init, r820t_exit,
		r820t_set_freq, r820t_set_bw, r820t_set_bw_center, r820t_set_gain, NULL,
		r820t_set_gain_mode, r820t_set_i2c_register, r820t_set_i2c_override, r820t_get_i2c_register
	},
	{
		r820t_init, r820t_exit,
		r820t_set_freq, r820t_set_bw, r820t_set_bw_center, r820t_set_gain, NULL,
		r820t_set_gain_mode, r820t_set_i2c_register, r820t_set_i2c_override, r820t_get_i2c_register
	},
};


typedef struct rtlsdr_dongle {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} rtlsdr_dongle_t;

/*
 * Please add your device here and send a patch to osmocom-sdr@lists.osmocom.org
 */
static rtlsdr_dongle_t known_devices[] = {
	{ 0x0bda, 0x2832, "Generic RTL2832U" },
	{ 0x0bda, 0x2838, "Generic RTL2832U OEM" },
	{ 0x0413, 0x6680, "DigitalNow Quad DVB-T PCI-E card" },
	{ 0x0413, 0x6f0f, "Leadtek WinFast DTV Dongle mini D" },
	{ 0x0458, 0x707f, "Genius TVGo DVB-T03 USB dongle (Ver. B)" },
	{ 0x0ccd, 0x00a9, "Terratec Cinergy T Stick Black (rev 1)" },
	{ 0x0ccd, 0x00b3, "Terratec NOXON DAB/DAB+ USB dongle (rev 1)" },
	{ 0x0ccd, 0x00b4, "Terratec Deutschlandradio DAB Stick" },
	{ 0x0ccd, 0x00b5, "Terratec NOXON DAB Stick - Radio Energy" },
	{ 0x0ccd, 0x00b7, "Terratec Media Broadcast DAB Stick" },
	{ 0x0ccd, 0x00b8, "Terratec BR DAB Stick" },
	{ 0x0ccd, 0x00b9, "Terratec WDR DAB Stick" },
	{ 0x0ccd, 0x00c0, "Terratec MuellerVerlag DAB Stick" },
	{ 0x0ccd, 0x00c6, "Terratec Fraunhofer DAB Stick" },
	{ 0x0ccd, 0x00d3, "Terratec Cinergy T Stick RC (Rev.3)" },
	{ 0x0ccd, 0x00d7, "Terratec T Stick PLUS" },
	{ 0x0ccd, 0x00e0, "Terratec NOXON DAB/DAB+ USB dongle (rev 2)" },
	{ 0x1209, 0x2832, "Generic RTL2832U" },
	{ 0x1554, 0x5020, "PixelView PV-DT235U(RN)" },
	{ 0x15f4, 0x0131, "Astrometa DVB-T/DVB-T2" },
	{ 0x15f4, 0x0133, "HanfTek DAB+FM+DVB-T" },
	{ 0x185b, 0x0620, "Compro Videomate U620F"},
	{ 0x185b, 0x0650, "Compro Videomate U650F"},
	{ 0x185b, 0x0680, "Compro Videomate U680F"},
	{ 0x1b80, 0xd393, "GIGABYTE GT-U7300" },
	{ 0x1b80, 0xd394, "DIKOM USB-DVBT HD" },
	{ 0x1b80, 0xd395, "Peak 102569AGPK" },
	{ 0x1b80, 0xd397, "KWorld KW-UB450-T USB DVB-T Pico TV" },
	{ 0x1b80, 0xd398, "Zaapa ZT-MINDVBZP" },
	{ 0x1b80, 0xd39d, "SVEON STV20 DVB-T USB & FM" },
	{ 0x1b80, 0xd3a4, "Twintech UT-40" },
	{ 0x1b80, 0xd3a8, "ASUS U3100MINI_PLUS_V2" },
	{ 0x1b80, 0xd3af, "SVEON STV27 DVB-T USB & FM" },
	{ 0x1b80, 0xd3b0, "SVEON STV21 DVB-T USB & FM" },
	{ 0x1d19, 0x1101, "Dexatek DK DVB-T Dongle (Logilink VG0002A)" },
	{ 0x1d19, 0x1102, "Dexatek DK DVB-T Dongle (MSI DigiVox mini II V3.0)" },
	{ 0x1d19, 0x1103, "Dexatek Technology Ltd. DK 5217 DVB-T Dongle" },
	{ 0x1d19, 0x1104, "MSI DigiVox Micro HD" },
	{ 0x1f4d, 0xa803, "Sweex DVB-T USB" },
	{ 0x1f4d, 0xb803, "GTek T803" },
	{ 0x1f4d, 0xc803, "Lifeview LV5TDeluxe" },
	{ 0x1f4d, 0xd286, "MyGica TD312" },
	{ 0x1f4d, 0xd803, "PROlectrix DV107669" },
};

#define DEFAULT_BUF_NUMBER	15
#define DEFAULT_BUF_LENGTH	(16 * 32 * 512)
/* buf_len:
 * must be multiple of 512 - else it will be overwritten
 * in rtlsdr_read_async() in librtlsdr.c with DEFAULT_BUF_LENGTH (= 16*32 *512 = 512 *512)
 *
 * -> 512*512 -> 1048 ms @ 250 kS  or  81.92 ms @ 3.2 MS (internal default)
 * ->  32*512 ->   65 ms @ 250 kS  or   5.12 ms @ 3.2 MS (new default)
 */


#define DEF_RTL_XTAL_FREQ	28800000
#define MIN_RTL_XTAL_FREQ	(DEF_RTL_XTAL_FREQ - 1000)
#define MAX_RTL_XTAL_FREQ	(DEF_RTL_XTAL_FREQ + 1000)

#define CTRL_IN			(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)
#define CTRL_TIMEOUT	300
#define BULK_TIMEOUT	0

#define EEPROM_ADDR	0xa0

enum usb_reg {
	USB_SYSCTL			= 0x2000,
	USB_CTRL			= 0x2010,
	USB_STAT			= 0x2014,
	USB_EPA_CFG			= 0x2144,
	USB_EPA_CTL			= 0x2148,
	USB_EPA_MAXPKT		= 0x2158,
	USB_EPA_MAXPKT_2	= 0x215a,
	USB_EPA_FIFO_CFG	= 0x2160,
};

enum sys_reg {
	DEMOD_CTL		= 0x3000,
	GPO				= 0x3001,
	GPI				= 0x3002,
	GPOE			= 0x3003,
	GPD				= 0x3004,
	SYSINTE			= 0x3005,
	SYSINTS			= 0x3006,
	GP_CFG0			= 0x3007,
	GP_CFG1			= 0x3008,
	SYSINTE_1		= 0x3009,
	SYSINTS_1		= 0x300a,
	DEMOD_CTL_1		= 0x300b,
	IR_SUSPEND		= 0x300c,

	/* IrDA registers */
	SYS_IRRC_PSR		= 0x3020, /* IR protocol selection */
	SYS_IRRC_PER		= 0x3024, /* IR protocol extension */
	SYS_IRRC_SF		= 0x3028, /* IR sampling frequency */
	SYS_IRRC_DPIR		= 0x302C, /* IR data package interval */
	SYS_IRRC_CR		= 0x3030, /* IR control */
	SYS_IRRC_RP		= 0x3034, /* IR read port */
	SYS_IRRC_SR		= 0x3038, /* IR status */
	/* I2C master registers */
	SYS_I2CCR		= 0x3040, /* I2C clock */
	SYS_I2CMCR		= 0x3044, /* I2C master control */
	SYS_I2CMSTR		= 0x3048, /* I2C master SCL timing */
	SYS_I2CMSR		= 0x304C, /* I2C master status */
	SYS_I2CMFR		= 0x3050, /* I2C master FIFO */

	/*
	 * IR registers
	 */
	IR_RX_BUF		= 0xFC00,
	IR_RX_IE		= 0xFD00,
	IR_RX_IF		= 0xFD01,
	IR_RX_CTRL		= 0xFD02,
	IR_RX_CFG		= 0xFD03,
	IR_MAX_DURATION0	= 0xFD04,
	IR_MAX_DURATION1	= 0xFD05,
	IR_IDLE_LEN0		= 0xFD06,
	IR_IDLE_LEN1		= 0xFD07,
	IR_GLITCH_LEN		= 0xFD08,
	IR_RX_BUF_CTRL		= 0xFD09,
	IR_RX_BUF_DATA		= 0xFD0A,
	IR_RX_BC		= 0xFD0B,
	IR_RX_CLK		= 0xFD0C,
	IR_RX_C_COUNT_L		= 0xFD0D,
	IR_RX_C_COUNT_H		= 0xFD0E,
	IR_SUSPEND_CTRL		= 0xFD10,
	IR_ERR_TOL_CTRL		= 0xFD11,
	IR_UNIT_LEN		= 0xFD12,
	IR_ERR_TOL_LEN		= 0xFD13,
	IR_MAX_H_TOL_LEN	= 0xFD14,
	IR_MAX_L_TOL_LEN	= 0xFD15,
	IR_MASK_CTRL		= 0xFD16,
	IR_MASK_DATA		= 0xFD17,
	IR_RES_MASK_ADDR	= 0xFD18,
	IR_RES_MASK_T_LEN	= 0xFD19,
};

enum blocks {
	DEMODB			= 0,
	USBB			= 1,
	SYSB			= 2,
	TUNB			= 3,
	ROMB			= 4,
	IRB				= 5,
	IICB			= 6,
};

int rtlsdr_read_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
	int r;
	uint16_t index = (block << 8);
	if (block == IRB) index = (SYSB << 8) | 0x01;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, array, len, CTRL_TIMEOUT);
#if 0
	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);
#endif
	return r;
}

int rtlsdr_write_array(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
	int r;
	uint16_t index = (block << 8) | 0x10;
	if (block == IRB) index = (SYSB << 8) | 0x11;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, array, len, CTRL_TIMEOUT);
#if 0
	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);
#endif
	return r;
}

int rtlsdr_i2c_write_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t val)
{
	uint16_t addr = i2c_addr;
	uint8_t data[2];

	data[0] = reg;
	data[1] = val;
	return rtlsdr_write_array(dev, IICB, addr, (uint8_t *)&data, 2);
}

uint8_t rtlsdr_i2c_read_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t reg)
{
	uint16_t addr = i2c_addr;
	uint8_t data = 0;

	rtlsdr_write_array(dev, IICB, addr, &reg, 1);
	rtlsdr_read_array(dev, IICB, addr, &data, 1);

	return data;
}

int rtlsdr_i2c_write(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
	uint16_t addr = i2c_addr;

	if (!dev)
		return -1;

	return rtlsdr_write_array(dev, IICB, addr, buffer, len);
}

int rtlsdr_i2c_read(rtlsdr_dev_t *dev, uint8_t i2c_addr, uint8_t *buffer, int len)
{
	uint16_t addr = i2c_addr;

	if (!dev)
		return -1;

	return rtlsdr_read_array(dev, IICB, addr, buffer, len);
}

uint16_t rtlsdr_read_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t len)
{
	int r;
	unsigned char data[2];
	uint16_t reg;
	uint16_t index = (block << 8);
	if (block == IRB) index = (SYSB << 8) | 0x01;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, data, len, CTRL_TIMEOUT);

	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	reg = (data[1] << 8) | data[0];

	return reg;
}

int rtlsdr_write_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];

	uint16_t index = (block << 8) | 0x10;
	if (block == IRB) index = (SYSB << 8) | 0x11;

	if (len == 1)
		data[0] = val & 0xff;
	else
		data[0] = val >> 8;

	data[1] = val & 0xff;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, data, len, CTRL_TIMEOUT);

	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	return r;
}

uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint8_t len)
{
	int r;
	unsigned char data[2];

	uint16_t index = page;
	uint16_t reg;
	addr = (addr << 8) | 0x20;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, data, len, CTRL_TIMEOUT);

	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	reg = (data[1] << 8) | data[0];

	return reg;
}

int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
	int r;
	unsigned char data[2];
	uint16_t index = 0x10 | page;
	addr = (addr << 8) | 0x20;

	if (len == 1)
		data[0] = val & 0xff;
	else
		data[0] = val >> 8;

	data[1] = val & 0xff;

	r = libusb_control_transfer(dev->devh, CTRL_OUT, 0, addr, index, data, len, CTRL_TIMEOUT);

	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	rtlsdr_demod_read_reg(dev, 0x0a, 0x01, 1);

	return (r == len) ? 0 : -1;
}

void rtlsdr_set_gpio_bit(rtlsdr_dev_t *dev, uint8_t gpio, int val)
{
	uint16_t r;

	gpio = 1 << gpio;
	r = rtlsdr_read_reg(dev, SYSB, GPO, 1);
	r = val ? (r | gpio) : (r & ~gpio);
	rtlsdr_write_reg(dev, SYSB, GPO, r, 1);
}

void rtlsdr_set_gpio_output(rtlsdr_dev_t *dev, uint8_t gpio)
{
	int r;
	gpio = 1 << gpio;

	r = rtlsdr_read_reg(dev, SYSB, GPD, 1);
	rtlsdr_write_reg(dev, SYSB, GPD, r & ~gpio, 1);
	r = rtlsdr_read_reg(dev, SYSB, GPOE, 1);
	rtlsdr_write_reg(dev, SYSB, GPOE, r | gpio, 1);
}

void rtlsdr_set_i2c_repeater(rtlsdr_dev_t *dev, int on)
{
	rtlsdr_demod_write_reg(dev, 1, 0x01, on ? 0x18 : 0x10, 1);
}

int rtlsdr_set_fir(rtlsdr_dev_t *dev)
{
	uint8_t fir[20];

	int i;
	/* format: int8_t[8] */
	for (i = 0; i < 8; ++i) {
		const int val = dev->fir[i];
		if (val < -128 || val > 127) {
			return -1;
		}
		fir[i] = val;
	}
	/* format: int12_t[8] */
	for (i = 0; i < 8; i += 2) {
		const int val0 = dev->fir[8+i];
		const int val1 = dev->fir[8+i+1];
		if (val0 < -2048 || val0 > 2047 || val1 < -2048 || val1 > 2047) {
			return -1;
		}
		fir[8+i*3/2] = val0 >> 4;
		fir[8+i*3/2+1] = (val0 << 4) | ((val1 >> 8) & 0x0f);
		fir[8+i*3/2+2] = val1;
	}

	for (i = 0; i < (int)sizeof(fir); i++) {
		if (rtlsdr_demod_write_reg(dev, 1, 0x1c + i, fir[i], 1))
				return -1;
	}

	return 0;
}

void rtlsdr_init_baseband(rtlsdr_dev_t *dev)
{
	unsigned int i;

	/* initialize USB */
	rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1);
	rtlsdr_write_reg(dev, USBB, USB_EPA_MAXPKT, 0x0002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);

	/* poweron demod */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL_1, 0x22, 1);
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0xe8, 1);

	/* reset demod (bit 3, soft_rst) */
	rtlsdr_demod_write_reg(dev, 1, 0x01, 0x14, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);

	/* disable spectrum inversion and adjacent channel rejection */
	rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x16, 0x0000, 2);

	/* clear both DDC shift and IF frequency registers	*/
	for (i = 0; i < 6; i++)
		rtlsdr_demod_write_reg(dev, 1, 0x16 + i, 0x00, 1);

	rtlsdr_set_fir(dev);

	/* enable SDR mode, disable DAGC (bit 5) */
	rtlsdr_demod_write_reg(dev, 0, 0x19, 0x05, 1);

	/* init FSM state-holding register */
	rtlsdr_demod_write_reg(dev, 1, 0x93, 0xf0, 1);
	rtlsdr_demod_write_reg(dev, 1, 0x94, 0x0f, 1);

	/* disable AGC (en_dagc, bit 0) (this seems to have no effect) */
	rtlsdr_demod_write_reg(dev, 1, 0x11, 0x00, 1);

	/* disable RF and IF AGC loop */
	rtlsdr_demod_write_reg(dev, 1, 0x04, 0x00, 1);
	dev->rtl_vga_control = 0;

	/* disable PID filter (enable_PID = 0) */
	rtlsdr_demod_write_reg(dev, 0, 0x61, 0x60, 1);

	/* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
	rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

	/* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
	 * IQ estimation/compensation (en_iq_comp, en_iq_est) */
	rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);

	/* disable 4.096 MHz clock output on pin TP_CK0 */
	rtlsdr_demod_write_reg(dev, 0, 0x0d, 0x83, 1);
}

int rtlsdr_deinit_baseband(rtlsdr_dev_t *dev)
{
	int r = 0;

	if (!dev)
		return -1;

	if (dev->tuner && dev->tuner->exit) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->exit(dev); /* deinitialize tuner */
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	/* poweroff demodulator and ADCs */
	rtlsdr_write_reg(dev, SYSB, DEMOD_CTL, 0x20, 1);

	return r;
}

static int rtlsdr_set_if_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
	uint32_t rtl_xtal;
	int32_t if_freq;
	uint8_t tmp;
	int r;

	if (!dev)
		return -1;

	/* read corrected clock value */
	if (rtlsdr_get_xtal_freq(dev, &rtl_xtal, NULL))
		return -2;

#ifdef WITH_UDP_SERVER
	dev->last_if_freq = freq;
	if ( dev->override_if_flag ) {
		if ( dev->verbose )
			fprintf(stderr, "overriding rtlsdr_set_if_freq(): modifying %u to %d Hz\n"
					, freq, dev->override_if_freq );
		freq = dev->override_if_freq;
		if ( dev->override_if_flag == 1 )
			dev->override_if_flag = 0;
	}
#endif

	if_freq = ((freq * TWO_POW(22)) / rtl_xtal) * (-1);

	tmp = (if_freq >> 16) & 0x3f;
	r = rtlsdr_demod_write_reg(dev, 1, 0x19, tmp, 1);
	tmp = (if_freq >> 8) & 0xff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x1a, tmp, 1);
	tmp = if_freq & 0xff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x1b, tmp, 1);

	return r;
}

int rtlsdr_set_sample_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
	int r = 0;
	uint8_t tmp;
	int16_t offs = ppm * (-1) * TWO_POW(24) / 1000000;

	tmp = offs & 0xff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x3f, tmp, 1);
	tmp = (offs >> 8) & 0x3f;
	r |= rtlsdr_demod_write_reg(dev, 1, 0x3e, tmp, 1);

	return r;
}

int rtlsdr_set_xtal_freq(rtlsdr_dev_t *dev, uint32_t rtl_freq, uint32_t tuner_freq)
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_xtal_freq(dev, rtl_freq, tuner_freq);
	}
	#endif

	if (!dev)
		return -1;

	if (rtl_freq > 0 &&
		(rtl_freq < MIN_RTL_XTAL_FREQ || rtl_freq > MAX_RTL_XTAL_FREQ))
		return -2;

	if (rtl_freq > 0 && dev->rtl_xtal != rtl_freq) {
		dev->rtl_xtal = rtl_freq;

		/* update xtal-dependent settings */
		if (dev->rate)
			r = rtlsdr_set_sample_rate(dev, dev->rate);
	}

	if (dev->tun_xtal != tuner_freq) {
		if (0 == tuner_freq)
			dev->tun_xtal = dev->rtl_xtal;
		else
			dev->tun_xtal = tuner_freq;

		/* read corrected clock value into e4k and r82xx structure */
		if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
			rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
			return -3;

		/* update xtal-dependent settings */
		if (dev->freq)
			r = rtlsdr_set_center_freq(dev, dev->freq);
	}

	return r;
}

int rtlsdr_get_xtal_freq(rtlsdr_dev_t *dev, uint32_t *rtl_freq, uint32_t *tuner_freq)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_xtal_freq(dev, rtl_freq, tuner_freq);
	}
	#endif

	if (!dev)
		return -1;

	#define APPLY_PPM_CORR(val,ppm) (((val) * (1.0 + (ppm) / 1e6)))

	if (rtl_freq)
		*rtl_freq = (uint32_t) APPLY_PPM_CORR(dev->rtl_xtal, dev->corr);

	if (tuner_freq)
		*tuner_freq = (uint32_t) APPLY_PPM_CORR(dev->tun_xtal, dev->corr);

	return 0;
}

int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *manufact, char *product,
							char *serial)
{
	struct libusb_device_descriptor dd;
	libusb_device *device = NULL;
	const int buf_max = 256;
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_usb_strings(dev, manufact, product, serial);
	}
	#endif

	if (!dev || !dev->devh)
		return -1;

	device = libusb_get_device(dev->devh);

	r = libusb_get_device_descriptor(device, &dd);
	if (r < 0)
		return -1;

	if (manufact) {
		memset(manufact, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iManufacturer,
							 (unsigned char *)manufact,
							 buf_max);
	}

	if (product) {
		memset(product, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iProduct,
							 (unsigned char *)product,
							 buf_max);
	}

	if (serial) {
		memset(serial, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iSerialNumber,
							 (unsigned char *)serial,
							 buf_max);
	}

	return 0;
}

int rtlsdr_write_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
	int r = 0;
	int i;
	uint8_t cmd[2];

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_write_eeprom(dev, data, offset, len);
	}
	#endif

	if (!dev)
		return -1;

	if ((len + offset) > 256)
		return -2;

	for (i = 0; i < len; i++) {
		cmd[0] = i + offset;
		r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 1);
		r = rtlsdr_read_array(dev, IICB, EEPROM_ADDR, &cmd[1], 1);

		/* only write the byte if it differs */
		if (cmd[1] == data[i])
			continue;

		cmd[1] = data[i];
		r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, cmd, 2);
		if (r != sizeof(cmd))
			return -3;

		/* for some EEPROMs (e.g. ATC 240LC02) we need a delay
		 * between write operations, otherwise they will fail */
#ifdef _WIN32
		Sleep(5);
#else
		usleep(5000);
#endif
	}

	return 0;
}

int rtlsdr_read_eeprom(rtlsdr_dev_t *dev, uint8_t *data, uint8_t offset, uint16_t len)
{
	int r = 0;
	int i;
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_read_eeprom(dev, data, offset, len);
	}
	#endif

	if (!dev)
		return -1;

	if ((len + offset) > 256)
		return -2;

	r = rtlsdr_write_array(dev, IICB, EEPROM_ADDR, &offset, 1);
	if (r < 0)
		return -3;

	for (i = 0; i < len; i++) {
		r = rtlsdr_read_array(dev, IICB, EEPROM_ADDR, data + i, 1);

		if (r < 0)
			return -3;
	}

	return r;
}

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t freq)
{
	int r = -1;
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_center_freq(dev, freq);
	}
	#endif
	if (!dev || !dev->tuner)
		return -1;

	if (dev->direct_sampling_mode > RTLSDR_DS_Q)
		rtlsdr_update_ds(dev, freq);

	if (dev->direct_sampling) {
		r = rtlsdr_set_if_freq(dev, freq);
	} else if (dev->tuner && dev->tuner->set_freq) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_freq(dev, freq - dev->offs_freq);
		rtlsdr_set_i2c_repeater(dev, 0);
		reactivate_softagc(dev, SOFTSTATE_RESET);
	}

	if (!r)
		dev->freq = freq;
	else
		dev->freq = 0;

	// restore filters
	if (dev->handled) {
		rtlsdr_set_i2c_repeater(dev, 1);
		dev->tuner->set_i2c_register(dev, 27, dev->saved_27, 255);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	return r;
}

uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_center_freq(dev);
	}
	#endif

	if (!dev)
		return 0;

	return dev->freq;
}

int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm)
{
	int r = 0;
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_freq_correction(dev, ppm);
	}
	#endif

	if (!dev)
		return -1;

	if (dev->corr == ppm)
		return -2;

	dev->corr = ppm;

	r |= rtlsdr_set_sample_freq_correction(dev, ppm);

	/* read corrected clock value into e4k and r82xx structure */
	if (rtlsdr_get_xtal_freq(dev, NULL, &dev->e4k_s.vco.fosc) ||
			rtlsdr_get_xtal_freq(dev, NULL, &dev->r82xx_c.xtal))
		return -3;

	if (dev->freq) /* retune to apply new correction value */
		r |= rtlsdr_set_center_freq(dev, dev->freq);

	return r;
}

int rtlsdr_get_freq_correction(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_freq_correction(dev);
	}
	#endif

	if (!dev)
		return 0;

	return dev->corr;
}

enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return (enum rtlsdr_tuner)rtlsdr_rpc_get_tuner_type(dev);
	}
	#endif

	if (!dev)
		return RTLSDR_TUNER_UNKNOWN;

	return dev->tuner_type;
}

static
const int * get_tuner_gains(rtlsdr_dev_t *dev, int *pNum )
{
	/* all gain values are expressed in tenths of a dB */
	static const int e4k_gains[] = { -10, 15, 40, 65, 90, 115, 140, 165, 190, 215,
					240, 290, 340, 420 };
	static const int fc0012_gains[] = { -99, -40, 71, 179, 192 };
	static const int fc0013_gains[] = { -99, -73, -65, -63, -60, -58, -54, 58, 61,
							 	  63, 65, 67, 68, 70, 71, 179, 181, 182,
							 	  184, 186, 188, 191, 197 };
	static const int fc2580_gains[] = { 0 /* no gain values */ };
	static const int r82xx_gains[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
								166, 197, 207, 229, 254, 280, 297, 328,
						 		338, 364, 372, 386, 402, 421, 434, 439,
						 		445, 480, 496 };
	static const int unknown_gains[] = { 0 /* no gain values */ };

	const int *ptr = NULL;
	int len = 0;

#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
		if ( !dev->softagc.rpcGainValues )
		{
			dev->softagc.rpcNumGains = rtlsdr_rpc_get_tuner_gains(dev, NULL);
			if ( dev->softagc.rpcNumGains > 0 )
			{
				dev->softagc.rpcGainValues = malloc( dev->softagc.rpcNumGains * sizeof(int) );
				if ( dev->softagc.rpcGainValues )
					rtlsdr_get_tuner_gains(dev, dev->softagc.rpcGainValues);
			}
		}
		if ( dev->softagc.rpcGainValues )
		{
			*pNum = dev->softagc.rpcNumGains;
			return dev->softagc.rpcGainValues;
		}
		*pNum = 0;
		return NULL;
	}
#endif

	switch (dev->tuner_type) {
	case RTLSDR_TUNER_E4000:
		ptr = e4k_gains; len = sizeof(e4k_gains);
		break;
	case RTLSDR_TUNER_FC0012:
		ptr = fc0012_gains; len = sizeof(fc0012_gains);
		break;
	case RTLSDR_TUNER_FC0013:
		ptr = fc0013_gains; len = sizeof(fc0013_gains);
		break;
	case RTLSDR_TUNER_FC2580:
		ptr = fc2580_gains; len = sizeof(fc2580_gains);
		break;
	case RTLSDR_TUNER_R820T:
	case RTLSDR_TUNER_R828D:
		ptr = r82xx_gains; len = sizeof(r82xx_gains);
		break;
	default:
		ptr = unknown_gains; len = sizeof(unknown_gains);
		break;
	}

	*pNum = len / sizeof(int);
	return ptr;
}


int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains)
{
	const int *ptr = NULL;
	int len = 0;

	if (!dev)
		return -1;

	ptr = get_tuner_gains(dev, &len );
	len = len * sizeof(int);

	if (!gains) { /* no buffer provided, just return the count */
		return len / sizeof(int);
	} else {
		if (len)
			memcpy(gains, ptr, len);

		return len / sizeof(int);
	}
}

int rtlsdr_set_and_get_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw, uint32_t *applied_bw, int apply_bw )
{
	int r = 0;

	*applied_bw = 0;		/* unknown */

	if (!dev || !dev->tuner)
		return -1;

	if(!apply_bw)
	{
		if (dev->tuner->set_bw) {
			r = dev->tuner->set_bw(dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);
		}
		return r;
	}

	if (dev->tuner->set_bw) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_bw(dev, bw > 0 ? bw : dev->rate, applied_bw, apply_bw);
		rtlsdr_set_i2c_repeater(dev, 0);
		reactivate_softagc(dev, SOFTSTATE_RESET);
		if (r)
			return r;
		dev->bw = bw;
	}
	return r;
}

int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t *dev, uint32_t bw )
{
	uint32_t applied_bw = 0;
	return rtlsdr_set_and_get_tuner_bandwidth(dev, bw, &applied_bw, 1 /* =apply_bw */ );
}

int rtlsdr_set_tuner_band_center(rtlsdr_dev_t *dev, int32_t if_band_center_freq )
{
	int r = -1;
	if (!dev || !dev->tuner || !dev->tuner->set_bw_center)
		return -1;
	return dev->tuner->set_bw_center(dev, if_band_center_freq);
}


int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain)
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_tuner_gain(dev, gain);
	}
	#endif

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_gain((void *)dev, gain);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (!r)
		dev->gain = gain;
	else
		dev->gain = 0;

	return r;
}

int rtlsdr_set_tuner_gain_ext(rtlsdr_dev_t *dev, int lna_gain, int mixer_gain, int vga_gain)
{
	int r = 0;

	if (!dev || ( dev->tuner_type != RTLSDR_TUNER_R820T && dev->tuner_type != RTLSDR_TUNER_R828D ) )
		return -1;

	if (dev->tuner->set_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = r820t_set_gain_ext((void *)dev, lna_gain, mixer_gain, vga_gain);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (!r)
		dev->gain = lna_gain + mixer_gain + vga_gain;
	else
		dev->gain = 0;

	return r;
}

int rtlsdr_set_tuner_agc_mode(rtlsdr_dev_t *dev, int agc_variant)
{
	int r = 0;

	if (!dev || ( dev->tuner_type != RTLSDR_TUNER_R820T && dev->tuner_type != RTLSDR_TUNER_R828D ) )
		return -1;

	if (dev->tuner->set_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = r820t_set_agc_mode((void *)dev, agc_variant);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	return r;
}


int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_tuner_gain(dev);
	}
	#endif

	if (!dev)
		return 0;

	return dev->gain;
}

int rtlsdr_set_tuner_if_gain(rtlsdr_dev_t *dev, int stage, int gain)
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_tuner_if_gain(dev, stage, gain);
	}
	#endif

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_if_gain) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_if_gain(dev, stage, gain);
		rtlsdr_set_i2c_repeater(dev, 0);
		reactivate_softagc(dev, SOFTSTATE_RESET);
	}

	return r;
}

int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int mode)
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_tuner_gain_mode(dev, mode);
	}
	#endif

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain_mode) {
		if ( dev->softagc.softAgcMode != SOFTAGC_OFF ) {
			mode = 1;		/* use manual gain mode - for softagc */
			if ( dev->verbose )
				fprintf(stderr, "rtlsdr_set_tuner_gain_mode() - overridden for softagc!\n");
		}
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_gain_mode((void *)dev, mode);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	return r;
}

int rtlsdr_set_tuner_i2c_register(rtlsdr_dev_t *dev, unsigned i2c_register, unsigned mask /* byte */, unsigned data /* byte */ )
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
		/* TODO */
		return -1;
	}
	#endif

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_i2c_register) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_i2c_register((void *)dev, i2c_register, data, mask);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	return r;
}

int rtlsdr_set_tuner_i2c_override(rtlsdr_dev_t *dev, unsigned i2c_register, unsigned mask /* byte */, unsigned data /* byte */ )
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
		/* TODO */
		return -1;
	}
	#endif

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_i2c_override) {
		rtlsdr_set_i2c_repeater(dev, 1);
		r = dev->tuner->set_i2c_override((void *)dev, i2c_register, data, mask);
		rtlsdr_set_i2c_repeater(dev, 0);
	}
	return r;
}


int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
	int r = 0;
	uint16_t tmp;
	uint32_t rsamp_ratio, real_rsamp_ratio;
	double real_rate;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_sample_rate(dev, samp_rate);
	}
	#endif

	if (!dev)
		return -1;

	/* check if the rate is supported by the resampler */
	if ((samp_rate <= 225000) || (samp_rate > 3200000) ||
		 ((samp_rate > 300000) && (samp_rate <= 900000))) {
		fprintf(stderr, "Invalid sample rate: %u Hz\n", samp_rate);
		return -EINVAL;
	}

	rsamp_ratio = (dev->rtl_xtal * TWO_POW(22)) / samp_rate;
	rsamp_ratio &= 0x0ffffffc;

	real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x08000000) << 1);
	real_rate = (dev->rtl_xtal * TWO_POW(22)) / real_rsamp_ratio;

	if ( ((double)samp_rate) != real_rate )
		fprintf(stderr, "Exact sample rate is: %f Hz\n", real_rate);

	dev->rate = (uint32_t)real_rate;

	if (dev->tuner && dev->tuner->set_bw) {
		uint32_t applied_bw = 0;
		rtlsdr_set_i2c_repeater(dev, 1);
		dev->tuner->set_bw(dev, dev->bw > 0 ? dev->bw : dev->rate, &applied_bw, 1);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	tmp = (rsamp_ratio >> 16);
	r |= rtlsdr_demod_write_reg(dev, 1, 0x9f, tmp, 2);
	tmp = rsamp_ratio & 0xffff;
	r |= rtlsdr_demod_write_reg(dev, 1, 0xa1, tmp, 2);

	r |= rtlsdr_set_sample_freq_correction(dev, dev->corr);

	/* reset demod (bit 3, soft_rst) */
	r |= rtlsdr_demod_write_reg(dev, 1, 0x01, 0x14, 1);
	r |= rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);

	/* recalculate offset frequency if offset tuning is enabled */
	if (dev->offs_freq)
		rtlsdr_set_offset_tuning(dev, 1);

	if ( reactivate_softagc(dev, SOFTSTATE_RESET) ) {
		dev->softagc.deadTimeSps = 0;
		dev->softagc.scanTimeSps = 0;
	}

	return r;
}

uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_sample_rate(dev);
	}
	#endif

	if (!dev)
		return 0;

	return dev->rate;
}

int rtlsdr_set_testmode(rtlsdr_dev_t *dev, int on)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_testmode(dev, on);
	}
	#endif

	if (!dev)
		return -1;

	return rtlsdr_demod_write_reg(dev, 0, 0x19, on ? 0x03 : 0x05, 1);
}

int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
		return rtlsdr_rpc_set_agc_mode(dev, on);
	}
	#endif

	if (!dev)
		return -1;

	return rtlsdr_demod_write_reg(dev, 0, 0x19, on ? 0x25 : 0x05, 1);
}

int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on)
{
	int r = 0;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_direct_sampling(dev, on);
	}
	#endif

	if (!dev)
		return -1;

	if (on) {
		if (dev->tuner && dev->tuner->exit) {
			rtlsdr_set_i2c_repeater(dev, 1);
			r = dev->tuner->exit(dev);
			rtlsdr_set_i2c_repeater(dev, 0);
		}

		/* disable Zero-IF mode */
		r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);

		/* disable spectrum inversion */
		r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x00, 1);

		/* only enable In-phase ADC input */
		r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);

		/* swap I and Q ADC, this allows to select between two inputs */
		r |= rtlsdr_demod_write_reg(dev, 0, 0x06, (on > 1) ? 0x90 : 0x80, 1);

		fprintf(stderr, "Enabled direct sampling mode, input %i\n", on);
		dev->direct_sampling = on;
	} else {
		if (dev->tuner && dev->tuner->init) {
			rtlsdr_set_i2c_repeater(dev, 1);
			r |= dev->tuner->init(dev);
			rtlsdr_set_i2c_repeater(dev, 0);
		}

		if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
				(dev->tuner_type == RTLSDR_TUNER_R828D)) {
			r |= rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);

			/* enable spectrum inversion */
			r |= rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
		} else {
			r |= rtlsdr_set_if_freq(dev, 0);

			/* enable In-phase + Quadrature ADC input */
			r |= rtlsdr_demod_write_reg(dev, 0, 0x08, 0xcd, 1);

			/* Enable Zero-IF mode */
			r |= rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1b, 1);
		}

		/* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
		r |= rtlsdr_demod_write_reg(dev, 0, 0x06, 0x80, 1);

		fprintf(stderr, "Disabled direct sampling mode\n");
		dev->direct_sampling = 0;
	}

	r |= rtlsdr_set_center_freq(dev, dev->freq);

	return r;
}

int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_direct_sampling(dev);
	}
	#endif
 
	if (!dev)
		return -1;

	return dev->direct_sampling;
}

int rtlsdr_set_ds_mode(rtlsdr_dev_t *dev, enum rtlsdr_ds_mode mode, uint32_t freq_threshold)
{
	uint32_t center_freq;
	if (!dev)
		return -1;

	center_freq = rtlsdr_get_center_freq(dev);
	if ( !center_freq )
		return -2;

	if (!freq_threshold) {
		switch(dev->tuner_type) {
		default:
		case RTLSDR_TUNER_UNKNOWN:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_E4000:	freq_threshold = 50*1000000; break; /* E4K_FLO_MIN_MHZ */
		case RTLSDR_TUNER_FC0012:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_FC0013:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_FC2580:	freq_threshold = 28800000; break; /* no idea!!! */
		case RTLSDR_TUNER_R820T:	freq_threshold = 24000000; break; /* ~ */
		case RTLSDR_TUNER_R828D:	freq_threshold = 28800000; break; /* no idea!!! */
		}
	}

	dev->direct_sampling_mode = mode;
	dev->direct_sampling_threshold = freq_threshold;

	if (mode <= RTLSDR_DS_Q)
		rtlsdr_set_direct_sampling(dev, mode);

	return rtlsdr_set_center_freq(dev, center_freq);
}

static int rtlsdr_update_ds(rtlsdr_dev_t *dev, uint32_t freq)
{
	int new_ds = 0;
	int curr_ds = rtlsdr_get_direct_sampling(dev);
	if ( curr_ds < 0 )
		return -1;

	switch (dev->direct_sampling_mode) {
	default:
	case RTLSDR_DS_IQ:		break;
	case RTLSDR_DS_I:		new_ds = 1; break;
	case RTLSDR_DS_Q:		new_ds = 2; break;
	case RTLSDR_DS_I_BELOW:	new_ds = (freq < dev->direct_sampling_threshold) ? 1 : 0; break;
	case RTLSDR_DS_Q_BELOW:	new_ds = (freq < dev->direct_sampling_threshold) ? 2 : 0; break;
	}

	if ( curr_ds != new_ds )
		return rtlsdr_set_direct_sampling(dev, new_ds);

	return 0;
}

int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on)
{
	int r = 0;
	int bw;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_set_offset_tuning(dev, on);
	}
	#endif

	if (!dev)
		return -1;

	if ((dev->tuner_type == RTLSDR_TUNER_R820T) ||
			(dev->tuner_type == RTLSDR_TUNER_R828D))
		return -2;

	if (dev->direct_sampling)
		return -3;

	/* based on keenerds 1/f noise measurements */
	dev->offs_freq = on ? ((dev->rate / 2) * 170 / 100) : 0;
	r |= rtlsdr_set_if_freq(dev, dev->offs_freq);

	if (dev->tuner && dev->tuner->set_bw) {
		uint32_t applied_bw = 0;
		rtlsdr_set_i2c_repeater(dev, 1);
		if (on) {
			bw = 2 * dev->offs_freq;
		} else if (dev->bw > 0) {
			bw = dev->bw;
		} else {
			bw = dev->rate;
		}
		dev->tuner->set_bw(dev, bw, &applied_bw, 1);
		rtlsdr_set_i2c_repeater(dev, 0);
	}

	if (dev->freq > dev->offs_freq)
		r |= rtlsdr_set_center_freq(dev, dev->freq);

	return r;
}

int rtlsdr_get_offset_tuning(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_offset_tuning(dev);
	}
	#endif

	if (!dev)
		return -1;

	return (dev->offs_freq) ? 1 : 0;
}

static rtlsdr_dongle_t *find_known_device(uint16_t vid, uint16_t pid)
{
	unsigned int i;
	rtlsdr_dongle_t *device = NULL;

	for (i = 0; i < sizeof(known_devices)/sizeof(rtlsdr_dongle_t); i++ ) {
		if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
			device = &known_devices[i];
			break;
		}
	}

	return device;
}

uint32_t rtlsdr_get_device_count(void)
{
	int i,r;
	libusb_context *ctx;
	libusb_device **list;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	ssize_t cnt;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_device_count();
	}
	#endif

	r = libusb_init(&ctx);
	if(r < 0)
		return 0;

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return device_count;
}

const char *rtlsdr_get_device_name(uint32_t index)
{
	int i,r;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	rtlsdr_dongle_t *device = NULL;
	uint32_t device_count = 0;
	ssize_t cnt;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_device_name(index);
	}
	#endif

	r = libusb_init(&ctx);
	if(r < 0)
		return "";

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			device_count++;

			if (index == device_count - 1)
				break;
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	if (device)
		return device->name;
	else
		return "";
}

int rtlsdr_get_device_usb_strings(uint32_t index, char *manufact,
					 char *product, char *serial)
{
	int r = -2;
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	rtlsdr_dongle_t *device = NULL;
	rtlsdr_dev_t devt;
	uint32_t device_count = 0;
	ssize_t cnt;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_device_usb_strings
	    (index, manufact, product, serial);
	}
	#endif

	r = libusb_init(&ctx);
	if(r < 0)
		return r;

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			device_count++;

			if (index == device_count - 1) {
				r = libusb_open(list[i], &devt.devh);
				if (!r) {
					r = rtlsdr_get_usb_strings(&devt,
									 manufact,
									 product,
									 serial);
					libusb_close(devt.devh);
				}
				break;
			}
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return r;
}

int rtlsdr_get_index_by_serial(const char *serial)
{
	int i, cnt, r;
	char str[256];

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_get_index_by_serial(serial);
	}
	#endif

	if (!serial)
		return -1;

	cnt = rtlsdr_get_device_count();

	if (!cnt)
		return -2;

	for (i = 0; i < cnt; i++) {
		r = rtlsdr_get_device_usb_strings(i, NULL, NULL, str);
		if (!r && !strcmp(serial, str))
			return i;
	}

	return -3;
}

/* UDP controller server */
#ifdef WITH_UDP_SERVER

static int parseNum(const char * pacNum) {
	int numBase = 10;			/* assume decimal */
	int sgn = 1;				/* sign: +/- 1 */
	int val = 0;
	const char * pac = pacNum + 1;
	if ( !pacNum || !pacNum[0] )
		return 0;

	if ( pacNum[0] == 'd' )			/* decimal system */
		numBase = 10;
	else if ( pacNum[0] == 'x' )		/* hexadecimal system */
		numBase = 16;
	else if ( pacNum[0] == 'b' )	/* binary system */
		numBase = 2;
	else
		pac = pacNum;

	if ( *pac == '-' ) {
		sgn = -1;
		++pac;
	}

	while ( *pac )
	{
		int digitValue = ( '0' <= *pac && *pac <= '9' ) ? (*pac - '0')
			: ( 'A' <= *pac && *pac <= 'F' ) ? (*pac + 10 - 'A') : (*pac + 10 - 'a');
		if ( digitValue >= 0 && digitValue < numBase ) {
			val = val * numBase + digitValue;
			++pac;
			continue;
		}
		else if ( *pac == '\'' || *pac == '.' || *pac == '_' ) {	/* ignore some delimiter chars */
			++pac;
			continue;
		}
		else
			break;
	}

	return val * sgn;
}

static double parseFreq(char *s)
/* standard suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	/* allow formatting spaces from .csv command file */
	while ( len > 1 && isspace(s[len-1]) )	--len;
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'g':
		case 'G':
			suff *= 1e3;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 1e3;
			/* fall-through */
		case 'k':
		case 'K':
			suff *= 1e3;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}


static const char * formatInHex(char * buf, int v, int num_digits) {
	static const char tab[] = "0123456789ABCDEF";
	int nibbleVal, nibbleNo, off = 0;
	buf[off++] = 'x';
	for ( nibbleNo = num_digits -1; nibbleNo >= 0; --nibbleNo ) {
		if ( (nibbleNo % 4) == 3 && nibbleNo != num_digits -1 )
			buf[off++] = '\'';
		nibbleVal = ( ((uint32_t)v) >> (nibbleNo * 4) ) & 0x0f;
		buf[off++] = tab[nibbleVal];
	}
	buf[off++] = 0;
	return buf;
}

static const char * formatInBin(char * buf, int v, int num_digits) {
	static const char tab[] = "01";
	int bitVal, bitNo, off = 0;
	buf[off++] = 'b';
	for ( bitNo = num_digits -1; bitNo >= 0; --bitNo ) {
		if ( (bitNo % 4) == 3 && bitNo != num_digits -1 )
			buf[off++] = '\'';
		bitVal = ( ((uint32_t)v) >> bitNo ) & 1;
		buf[off++] = tab[bitVal];
	}
	buf[off++] = 0;
	return buf;
}

static int parse(char *message, rtlsdr_dev_t *dev)
{
	char binBufA[64], binBufB[64];
	char hexBufA[16], hexBufB[16];
	char *str1, *token, *saveptr;
	char response[UDP_TX_BUFLEN];
	double freqVal = -1;
	int comm = 0, parsedVal = 0, iVal = 0;
	int val = 0;
	uint32_t applied_bw = 0;
	uint8_t mask = 0xff, reg=0;
	uint32_t freq;
	int32_t bandcenter;
	int retCode;

	str1 = message;
	memset(response,'\0', UDP_TX_BUFLEN);

	str1[100] = 0;
	str1[strlen(str1)-1] = 0;

	/* first token == command */
	token = strtok_r(str1, " \t", &saveptr);
	if ( !token )
	{
		sprintf(response,"?\n");
		sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
		return 0;
	}

	/* commands:
	 * g <register>            # get tuner i2c register
	 * s <register> <value> [<mask>] # set tuner i2c register once
	 * S <register> <value> [<mask>]   # set tuner i2c register permanent
	 * 
	 * i <IFfrequency>         # set tuner IF frequency once. value in [ 0 .. 28 800 000 ] or < 0 for reset
	 * I <IFfrequency>         # set tuner IF frequency permanent
	 * 
	 * f <RFfrequency>         # set rtl center frequency
	 * b <bandwidth>           # set tuner bandwidth
	 * c <frequency>           # set tuner bandwidth center in output. value in [ -1 600 000 .. 1 600 000 ]
	 * 
	 * a <tunerAgcVariant>     #  0: LNA/Mixer = auto; VGA = fixed 26.5 dB
	 *                         # -1: LNA/Mixer = last value from prev rtlsdr_set_tuner_gain; VGA = auto
	 *                         # >0: LNA/Mixer = from rtlsdr_set_tuner_gain(tunerAgcMode); VGA = auto
	 * m <gain>                # set tuner gain
	 * 
	 * M <gainMode>            # 0 : tuner agc off; digital rtl agc off
	 *                         # 1 : tuner agc on ; digital rtl agc off
	 *                         # 2 : tuner agc off; digital rtl agc on
	 *                         # 3 : tuner agc on ; digital rtl agc on
	 * 
	 * 
	 * ********** not implemented yet
	 * 
	 * e <lna> <mixer> <vga>   # set extended tuner gain - for R820 tuner

	 * RTLSDR_API int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual);
	 * RTLSDR_API int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on);
	 * 
	 * RTLSDR_API int rtlsdr_set_tuner_gain_ext(rtlsdr_dev_t *dev, int lna_gain, int mixer_gain, int vga_gain);
	 * RTLSDR_API int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev);
	 * RTLSDR_API int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain);
	 * 
	 */

	/* commands with register args: 64 | x */
	if (!strcmp(token, "g")) comm = 64 + 1;
	if (!strcmp(token, "s")) comm = 64 + 2;
	if (!strcmp(token, "S")) comm = 64 + 3;
	if (!strcmp(token, "i")) comm = 128 + 1;
	if (!strcmp(token, "I")) comm = 128 + 2;
	if (!strcmp(token, "f")) comm = 256 + 1;
	if (!strcmp(token, "b")) comm = 256 + 2;
	if (!strcmp(token, "c")) comm = 256 + 3;
	if (!strcmp(token, "a")) comm = 512 + 1;
	if (!strcmp(token, "m")) comm = 512 + 2;
	if (!strcmp(token, "M")) comm = 512 + 3;
	if (!strcmp(token, "h")) comm = 1024;

	if ( comm & 64 ) {
		token = strtok_r(NULL, " \t", &saveptr);
		parsedVal = parseNum(token);
		if ( (!token) || (comm >= (64+2) && parsedVal < 5) || (parsedVal > 32) ) {
			sprintf(response,"?\n");
			if (sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen) == SOCKET_ERROR) {
				// perror("send");
				return -1;
			}
			return 0;
		}
		reg = (uint8_t)parsedVal;	/* 1st arg: register address */
		if ( dev->verbose )
			fprintf(stderr, "parsed register %d from token '%s'\n", reg, token);

		if (token)
			token = strtok_r(NULL, " \t", &saveptr);
		if ( (!token) && (comm >= 64+2)) {	/* set requires additional parameter: the value */
			sprintf(response,"?\n");
			if (sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen) == SOCKET_ERROR) {
				// perror("send");
				return -1;
			}
			return 0;
		} else if (comm >= 64+2) {
			parsedVal = parseNum(token);		/* set: 2nd arg: value */
			iVal = parsedVal;
			if ( dev->verbose )
				fprintf(stderr, "parsed value %d = %03X from token '%s'\n", iVal, iVal, token);
		}

		if (token)
			token = strtok_r(NULL, " \t", &saveptr);
		if (!token) {
			mask = 0xff;		/* default mask */
		} else  {
			parsedVal = parseNum(token);		/* set: 3rd optional arg: mask */
			mask = (uint8_t)( parsedVal & 0xff );
			if ( dev->verbose )
				fprintf(stderr, "parsed mask %d = %02X from token '%s'\n", parsedVal, parsedVal, token);
		}

		if (comm == 64 + 1) {
			val = dev->tuner->get_i2c_register(dev, reg);
			sprintf(response,"! %d = %s = %s\n", val
				, formatInHex(hexBufA, val, 2)
				, formatInBin(binBufA, val, 8) );
			if ( dev->verbose )
			{
				fprintf(stderr, "parsed 'get i2c register %d = x%02X'\n", reg, reg);
				fprintf(stderr, "\tresponse: %s\n", response);
			}
			val = sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
			if (val<0) {
				// printf("error sending\n");
				return -1;
			}
		} else if (comm == 64 +2 || comm == 64 +3 ) {
			dev->saved_27 = dev->tuner->get_i2c_register(dev,27);
			if ( dev->verbose )
			{
				fprintf(stderr, "parsed 'set i2c register %s %d = x%02X  value %d = %s = %s  with mask %s = %s'\n"
						, ( comm == (64 +3) ? (iVal > 255 ? "override clear " : "override ") : "" )
						, reg, reg
						, val, formatInHex(hexBufA, iVal, 3), formatInBin(binBufA, iVal, 12)
						, formatInHex(hexBufB, (int)mask, 2), formatInBin(binBufB, (int)mask, 8) );
				fprintf(stderr, "\tresponse: %s\n", response);
			}
			if ( dev->tuner->set_i2c_register && dev->tuner->set_i2c_override ) {
				rtlsdr_set_i2c_repeater(dev, 1);
				if (comm == 64 +2) {
					fprintf(stderr, "calling tuner->set_i2c_register( reg %d, value %02X, mask %02X)\n", reg, iVal, mask);
					val = dev->tuner->set_i2c_register(dev, reg, iVal, mask);
				}
				else {
					fprintf(stderr, "calling tuner->set_i2c_override( reg %d, value %02X, mask %02X)\n", reg, iVal, mask);
					val = dev->tuner->set_i2c_override(dev, reg, iVal, mask);
				}
				rtlsdr_set_i2c_repeater(dev, 0);
			}
			sprintf(response,"! %d\n", (int)val);
			// printf("%d %d %d\n", reg, val, mask);
			val = sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
			if ( val < 0 ) {
				// printf("error sending\n");
				return -1;
			}
		} else {
			sprintf(response,"?\n");
			if ( dev->verbose )
			{
				fprintf(stderr, "parsed unknown command!\n");
				fprintf(stderr, "\tresponse: %s\n", response);
			}
			sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
		}
	}
	else if ( comm & 128 ) {
		token = strtok_r(NULL, " \t", &saveptr);
		freqVal = parseFreq(token);
		if ( freqVal < 0 ) {
			dev->override_if_freq = 0;
			dev->override_if_flag = 0;
		}
		else
		{
			dev->override_if_freq = (int)freqVal;
			dev->override_if_flag = ( comm == (128 + 1) ) ? 1 : 2;
		}
		/* set last bandwidth .. which also has to set the IF frequency */
		rtlsdr_set_and_get_tuner_bandwidth(dev, dev->bw, &applied_bw, 1 );
		rtlsdr_set_center_freq(dev, dev->freq);
	}
	else if ( comm & 256 ) {
		token = strtok_r(NULL, " \t", &saveptr);
		freqVal = parseFreq(token);
		retCode = -1;
		switch (comm & 63) {
		case 1: /* frequency */
			freq = (uint32_t)freqVal;
			if ( dev->verbose )
				fprintf(stderr, "parsed RF frequency = %u Hz from token '%s'\n", (unsigned)freq, token);
			retCode = rtlsdr_set_center_freq(dev, freq);
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_center_freq() returned %d\n", retCode);
			break;
		case 2: /* bandwidth */
			freq = (uint32_t)freqVal;
			if ( dev->verbose )
				fprintf(stderr, "parsed bandwidth = %u Hz from token '%s'\n", (unsigned)freq, token);
			retCode = rtlsdr_set_and_get_tuner_bandwidth(dev, freq, &applied_bw, 1);
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_and_get_tuner_bandwidth() returned %d and bw %u\n", retCode, applied_bw);
			break;
		case 3: /* band center */
			bandcenter = (int32_t)freqVal;
			if ( dev->verbose )
				fprintf(stderr, "parsed bandcenter = %d Hz from token '%s'\n", (int)bandcenter, token);
			retCode = rtlsdr_set_tuner_band_center(dev, bandcenter);
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_tuner_band_center() returned %d\n", retCode);
			break;
		default:
			break;
		}
	}
	else if ( comm & 512 ) {
		token = strtok_r(NULL, " \t", &saveptr);
		parsedVal = parseNum(token);
		if ( !token ) {
			sprintf(response,"?\n");
			if (sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen) == SOCKET_ERROR) {
				// perror("send");
				return -1;
			}
			return 0;
		}
		switch (comm & 63) {
		case 1: /* agc variant */
			if ( dev->verbose )
				fprintf(stderr, "parsed agc variant %d from token '%s'\n", parsedVal, token);
			retCode = rtlsdr_set_tuner_agc_mode(dev, parsedVal);
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_tuner_agc_mode() returned %d\n", retCode);
			break;
		case 2: /* manual gain */
			if ( dev->verbose )
				fprintf(stderr, "parsed tuner gain %d tenth dB from token '%s'\n", parsedVal, token);
			retCode = rtlsdr_set_tuner_gain(dev, parsedVal);
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_tuner_gain() returned %d\n", retCode);
			break;
		case 3: /* gainMode */
			if ( dev->verbose )
				fprintf(stderr, "parsed gainMode %d with tuner AGC '%s' and RTL AGC '%s' from token '%s'\n"
						, parsedVal
						, ( (parsedVal & 1) == 1 ) ? "on" : "off"
						, ( (parsedVal & 2) == 2 ) ? "on" : "off"
						, token );
			retCode = rtlsdr_set_tuner_gain_mode(dev, ( (parsedVal & 1) == 0 ) ? 1 : 0 );
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_tuner_gain_mode() returned %d\n", retCode);
			retCode = rtlsdr_set_agc_mode(dev, ( (parsedVal & 2) == 2 ) ? 1 : 0 );
			if ( dev->verbose )
				fprintf(stderr, "  rtlsdr_set_agc_mode() returned %d\n", retCode);
			break;
		}
	}
	else if ( comm & 1024 ) {
		sprintf(response,
			"g <register>\n"
			"s <register> <value> [<mask>]\n"
			"i <IFfrequency>\n"
			"f <RFfrequency>\n"
			"b <bandwidth>\n"
			"c <frequency>\n"
			"a <tunerAgcVariant>\n"
			"m <tuner gain>\n"
			"M <gainMode>\n" );
		sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
		fprintf(stderr, "udp server command help:\n%s\n", response);
	}

	{
		sprintf(response,"?\n");
		sendto(dev->udpS, response, strlen(response), 0, (struct sockaddr*) &dev->si_other, dev->slen);
	}
	return 0;
}

void * srv_server(void *vdev)
{
	rtlsdr_dev_t * dev = (rtlsdr_dev_t *)vdev;
	dev->slen = sizeof(dev->si_other);

	//Initialise winsock
	if (WSAStartup(MAKEWORD(2,2),&dev->wsa) != 0) {
		// printf("Failed. Error Code : %d",WSAGetLastError());
		exit(EXIT_FAILURE);
	}
	//Create a socket
	if((dev->udpS = socket(AF_INET , SOCK_DGRAM , IPPROTO_UDP )) == INVALID_SOCKET) {
		// printf("Could not create socket : %d" , WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	dev->server.sin_family = AF_INET;
	dev->server.sin_addr.s_addr = INADDR_ANY;
	dev->server.sin_port = htons( dev->udpPortNo );

	if( bind(dev->udpS, (struct sockaddr *)&dev->server , sizeof(dev->server)) == SOCKET_ERROR) {
		// printf("Bind failed with error code : %d" , WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	//keep listening for data
	while(1) {
		//clear the buffer by filling null, it might have previously received data
		memset(dev->buf,'\0', UDP_TX_BUFLEN);
		//try to receive some data, this is a blocking call
		if ((dev->recv_len = recvfrom(dev->udpS, dev->buf, UDP_TX_BUFLEN-1, 0, (struct sockaddr *) &dev->si_other, &dev->slen)) == SOCKET_ERROR) {
			// printf("recvfrom() failed with error code : %d" , WSAGetLastError());
			exit(EXIT_FAILURE);
		}

		if ( dev->verbose )
			fprintf(stderr, "received udp: %s\n", dev->buf);
		parse( dev->buf, dev );
		//print details of the client/peer and the data received
		// printf("Received packet from %s:%d\n", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));
		// printf("Data: %s\n" , buf);
	}
	closesocket(dev->udpS);
	WSACleanup();
	return NULL;
}

#endif


int rtlsdr_open(rtlsdr_dev_t **out_dev, uint32_t index)
{
	int r;
	int i;
	libusb_device **list;
	rtlsdr_dev_t *dev = NULL;
	libusb_device *device = NULL;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	uint8_t reg;
	ssize_t cnt;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_open((void**)out_dev, index);
	}
	#endif

	dev = malloc(sizeof(rtlsdr_dev_t));
	if (NULL == dev)
		return -ENOMEM;

	memset(dev, 0, sizeof(rtlsdr_dev_t));
	memcpy(dev->fir, fir_default, sizeof(fir_default));

	r = libusb_init(&dev->ctx);
	if(r < 0){
		free(dev);
		return -1;
	}

	dev->rtl_vga_control = 0;

	/* dev->softagc.command_thread; */
	dev->softagc.agcState = SOFTSTATE_OFF;
	dev->softagc.softAgcMode = SOFTAGC_OFF;	/* SOFTAGC_FREQ_CHANGE SOFTAGC_ATTEN SOFTAGC_ALL */
	dev->softagc.scanTimeMs = 100;	/* parameter: default: 100 ms */
	dev->softagc.deadTimeMs = 1;	/* parameter: default: 1 ms */
	dev->softagc.scanTimeSps = 0;
	dev->softagc.deadTimeSps = 0;
	dev->softagc.rpcNumGains = 0;
	dev->softagc.rpcGainValues = NULL;

	/* UDP controller server */
#ifdef WITH_UDP_SERVER
	dev->udpPortNo = 0;	/* default port 32323 .. but deactivated - by default */
	dev->override_if_freq = 0;
	dev->override_if_flag = 0;
#endif

	dev->dev_num = index;
	dev->dev_lost = 1;

	cnt = libusb_get_device_list(dev->ctx, &list);

	for (i = 0; i < cnt; i++) {
		device = list[i];

		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct)) {
			device_count++;
		}

		if (index == device_count - 1)
			break;

		device = NULL;
	}

	if (!device) {
		r = -1;
		goto err;
	}

	r = libusb_open(device, &dev->devh);
	if (r < 0) {
		libusb_free_device_list(list, 1);
		fprintf(stderr, "usb_open error %d\n", r);
		if(r == LIBUSB_ERROR_ACCESS)
			fprintf(stderr, "Please fix the device permissions, e.g. "
			"by installing the udev rules file rtl-sdr.rules\n");
		goto err;
	}

	libusb_free_device_list(list, 1);

	if (libusb_kernel_driver_active(dev->devh, 0) == 1) {
		dev->driver_active = 1;

#ifdef DETACH_KERNEL_DRIVER
		if (!libusb_detach_kernel_driver(dev->devh, 0)) {
			fprintf(stderr, "Detached kernel driver\n");
		} else {
			fprintf(stderr, "Detaching kernel driver failed!");
			goto err;
		}
#else
		fprintf(stderr, "\nKernel driver is active, or device is "
				"claimed by second instance of librtlsdr."
				"\nIn the first case, please either detach"
				" or blacklist the kernel module\n"
				"(dvb_usb_rtl28xxu), or enable automatic"
				" detaching at compile time.\n\n");
#endif
	}

	r = libusb_claim_interface(dev->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		goto err;
	}

	dev->rtl_xtal = DEF_RTL_XTAL_FREQ;

	/* perform a dummy write, if it fails, reset the device */
	if (rtlsdr_write_reg(dev, USBB, USB_SYSCTL, 0x09, 1) < 0) {
		fprintf(stderr, "Resetting device...\n");
		libusb_reset_device(dev->devh);
	}

	rtlsdr_init_baseband(dev);
	dev->dev_lost = 0;

	/* Probe tuners */
	rtlsdr_set_i2c_repeater(dev, 1);

	reg = rtlsdr_i2c_read_reg(dev, E4K_I2C_ADDR, E4K_CHECK_ADDR);
	if (reg == E4K_CHECK_VAL) {
		fprintf(stderr, "Found Elonics E4000 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_E4000;
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, FC0013_I2C_ADDR, FC0013_CHECK_ADDR);
	if (reg == FC0013_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0013 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_FC0013;
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, R820T_I2C_ADDR, R82XX_CHECK_ADDR);
	if (reg == R82XX_CHECK_VAL) {
		fprintf(stderr, "Found Rafael Micro R820T tuner\n");
		dev->tuner_type = RTLSDR_TUNER_R820T;
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, R828D_I2C_ADDR, R82XX_CHECK_ADDR);
	if (reg == R82XX_CHECK_VAL) {
		fprintf(stderr, "Found Rafael Micro R828D tuner\n");
		dev->tuner_type = RTLSDR_TUNER_R828D;
		goto found;
	}

	/* initialise GPIOs */
	rtlsdr_set_gpio_output(dev, 4);

	/* reset tuner before probing */
	rtlsdr_set_gpio_bit(dev, 4, 1);
	rtlsdr_set_gpio_bit(dev, 4, 0);

	reg = rtlsdr_i2c_read_reg(dev, FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
	if ((reg & 0x7f) == FC2580_CHECK_VAL) {
		fprintf(stderr, "Found FCI 2580 tuner\n");
		dev->tuner_type = RTLSDR_TUNER_FC2580;
		goto found;
	}

	reg = rtlsdr_i2c_read_reg(dev, FC0012_I2C_ADDR, FC0012_CHECK_ADDR);
	if (reg == FC0012_CHECK_VAL) {
		fprintf(stderr, "Found Fitipower FC0012 tuner\n");
		rtlsdr_set_gpio_output(dev, 6);
		dev->tuner_type = RTLSDR_TUNER_FC0012;
		goto found;
	}

found:
	/* use the rtl clock value by default */
	dev->tun_xtal = dev->rtl_xtal;
	dev->tuner = &tuners[dev->tuner_type];

	switch (dev->tuner_type) {
	case RTLSDR_TUNER_R828D:
		dev->tun_xtal = R828D_XTAL_FREQ;
		/* fall-through */
	case RTLSDR_TUNER_R820T:
		/* disable Zero-IF mode */
		rtlsdr_demod_write_reg(dev, 1, 0xb1, 0x1a, 1);

		/* only enable In-phase ADC input */
		rtlsdr_demod_write_reg(dev, 0, 0x08, 0x4d, 1);

		/* the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode, and
		 * 4.57 MHz for the 8 MHz mode */
		rtlsdr_set_if_freq(dev, R82XX_IF_FREQ);

		/* enable spectrum inversion */
		rtlsdr_demod_write_reg(dev, 1, 0x15, 0x01, 1);
		break;
	case RTLSDR_TUNER_UNKNOWN:
		fprintf(stderr, "No supported tuner found\n");
		rtlsdr_set_direct_sampling(dev, 1);
		break;
	default:
		break;
	}

	if (dev->tuner->init)
		r = dev->tuner->init(dev);

	rtlsdr_set_i2c_repeater(dev, 0);

	*out_dev = dev;

	return 0;
err:
	if (dev) {
		if (dev->ctx)
			libusb_exit(dev->ctx);

		free(dev);
	}

	return r;
}

int rtlsdr_close(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_close(dev);
	}
	#endif

	if (!dev)
		return -1;

	/* automatic de-activation of bias-T */
	rtlsdr_set_bias_tee(dev, 0);

	if(!dev->dev_lost) {
		/* block until all async operations have been completed (if any) */
		while (RTLSDR_INACTIVE != dev->async_status) {
#ifdef _WIN32
			Sleep(1);
#else
			usleep(1000);
#endif
		}

		rtlsdr_deinit_baseband(dev);
	}

	softagc_uninit(dev);

	libusb_release_interface(dev->devh, 0);

#ifdef DETACH_KERNEL_DRIVER
	if (dev->driver_active) {
		if (!libusb_attach_kernel_driver(dev->devh, 0))
			fprintf(stderr, "Reattached kernel driver\n");
		else
			fprintf(stderr, "Reattaching kernel driver failed!\n");
	}
#endif

	libusb_close(dev->devh);

	libusb_exit(dev->ctx);

	if (dev->handled) {
		dev->handled = 0;
	}

	free(dev);

	return 0;
}

int rtlsdr_reset_buffer(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_reset_buffer(dev);
	}
	#endif

	if (!dev)
		return -1;

	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x1002, 2);
	rtlsdr_write_reg(dev, USBB, USB_EPA_CTL, 0x0000, 2);

	return 0;
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_read_sync(dev, buf, len, n_read);
	}
	#endif

	if (!dev)
		return -1;

	return libusb_bulk_transfer(dev->devh, 0x81, buf, len, n_read, BULK_TIMEOUT);
}


/* return == softagc got activated */
static int reactivate_softagc(rtlsdr_dev_t *dev, enum softagc_stateT newState)
{
	if ( dev->softagc.softAgcMode > SOFTAGC_OFF )
	{
		if ( dev->softagc.agcState != SOFTSTATE_OFF
			 && dev->softagc.softAgcMode >= SOFTAGC_AUTO )
		{
			/* softagc already running -> nothing to do */
			if ( dev->verbose )
				fprintf(stderr, "rtlsdr reactivate_softagc(): state already %d\n", dev->softagc.agcState);
			return 1;
		}
		else
		{
			dev->softagc.agcState =  newState;
			if ( dev->verbose )
				fprintf(stderr, "rtlsdr reactivate_softagc switched to state %d\n", newState);
			return 1;
		}
	}
	if ( dev->verbose )
		fprintf(stderr, "*** rtlsdr reactivate_softagc(): Soft AGC is inactive!\n");
	return 0;
}

static void *softagc_control_worker(void *arg)
{
	rtlsdr_dev_t *dev = (rtlsdr_dev_t *)arg;
	struct softagc_state *agc = &dev->softagc;
	while(1) {
		safe_cond_wait(&agc->cond, &agc->mutex);

		if ( agc->exit_command_thread )
			pthread_exit(0);

		if ( agc->command_changeGain )
		{
			/* no need for extra mutex/buffer: next call is after DEAD_TIME */
			agc->command_changeGain = 0;
			rtlsdr_set_tuner_gain( dev, dev->softagc.command_newGain );
			dev->softagc.remainingDeadSps = dev->softagc.deadTimeSps;
			if ( dev->verbose )
				fprintf(stderr, "rtlsdr softagc_control_worker(): applied gain %d\n"
					, dev->softagc.command_newGain );
		}
	}
}

static void softagc_init(rtlsdr_dev_t *dev)
{
	pthread_attr_t attr;
	/* prepare thread */
	dev->softagc.exit_command_thread = 0;
	dev->softagc.command_newGain = 0;
	dev->softagc.command_changeGain = 0;
	/* create thread */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	pthread_mutex_init(&dev->softagc.mutex, NULL);
	pthread_cond_init(&dev->softagc.cond, NULL);
	pthread_create( &dev->softagc.command_thread, &attr, softagc_control_worker, dev);
	pthread_attr_destroy(&attr);
	/* manual gain mode for "softagc" */
	rtlsdr_set_tuner_gain_mode(dev, 1 );
}

static void softagc_uninit(rtlsdr_dev_t *dev)
{
	if ( dev->softagc.softAgcMode == SOFTAGC_OFF )
		return;

	dev->softagc.exit_command_thread = 1;
	safe_cond_signal(&dev->softagc.cond, &dev->softagc.mutex);
	pthread_join(dev->softagc.command_thread, NULL);
	pthread_cond_destroy(&dev->softagc.cond);
	pthread_mutex_destroy(&dev->softagc.mutex);
}

/* return == keepBlock */
static int softagc(rtlsdr_dev_t *dev, unsigned char *buf, int len)
{
	struct softagc_state * agc = &dev->softagc;
	int distrib[16];

	if ( agc->agcState == SOFTSTATE_INIT )
	{
		agc->agcState = SOFTSTATE_RESET;
#if 0
		fprintf(stderr, "*** init softagc gainmode\n");
#endif
		return 0;		/* throw away this block */
	}
	else if ( agc->agcState == SOFTSTATE_RESET )
	{
		int k, numGains = 0;
		const int * gains = get_tuner_gains(dev, &numGains );
#if 0
		fprintf(stderr, "*** rtlsdr softagc: get_tuner_gains() delivered %d values\n", numGains);
#endif

		if ( ! numGains )
		{
			// device is not initialized yet
			return 1;
		}

		if ( numGains == 1 )
		{
			agc->softAgcMode = SOFTAGC_OFF;
			agc->agcState = SOFTSTATE_OFF;
			if ( dev->verbose )
				fprintf(stderr, "*** rtlsdr softagc(): just single gain -> deactivating\n");
			return 1;
		}

		/* initialize measurement */
		if (!agc->scanTimeSps)
			agc->scanTimeSps = (int)( (agc->scanTimeMs * dev->rate) / 1000 );
		if (!agc->deadTimeSps)
			agc->deadTimeSps = (int)( (agc->deadTimeMs * dev->rate) / 1000 );

		agc->remainingDeadSps = INT_MAX;
		agc->remainingScanSps = agc->scanTimeSps;

		agc->numInHisto = 0;
		for ( k = 0; k < 16; ++k )
			agc->histo[k] = 0;

		dev->softagc.gainIdx = numGains - 1;
		dev->softagc.command_newGain = gains[dev->softagc.gainIdx];
		dev->softagc.command_changeGain = 1;
		safe_cond_signal(&dev->softagc.cond, &dev->softagc.mutex);
		if ( dev->verbose )
			fprintf(stderr, "rtlsdr softagc(): set maximum gain %d / 10 dB at idx %d\n"
				, gains[dev->softagc.gainIdx]
				, dev->softagc.gainIdx );

		agc->agcState = SOFTSTATE_RESET_CONT;
		return 0;
	}

	if ( agc->remainingDeadSps == INT_MAX )
		return 0;
	if ( agc->remainingDeadSps )
	{
		if ( agc->remainingDeadSps >= len/2 )
		{
			/* fprintf(stderr, "cont waiting dead samples: received %d of remaining %d smp\n", len, agc->remainingDeadSps); */
			agc->remainingDeadSps -= len/2;
			return ( agc->agcState == SOFTSTATE_RESET_CONT ) ? 0 : 1;
		}
		else
		{
			buf = buf + ( 2 * agc->remainingDeadSps);
			len -= 2 * agc->remainingDeadSps;
			agc->remainingDeadSps = 0;
		}
	}

	/* finish when arrived at lowest possible gain */
	if ( ! agc->gainIdx && agc->agcState == SOFTSTATE_RESET_CONT )
	{
		agc->agcState = SOFTSTATE_OFF;
		/* TODO: try deactivating Bias-T */
		if ( dev->verbose )
			fprintf(stderr, "rtlsdr softagc(): gain idx is 0 -> finish soft agc\n");
		return 1;
	}

	/* calculate histogram and distribution */
	{
		int * histo = &(agc->histo[0]);
		int i, k;
		for ( i = 0; i < len; ++i )
		{
			if ( buf[i] >= 128 )
				++histo[ ( (unsigned)buf[i] -128) >> 3 ];		/* -128 ==> max is then 127 == 7 bit */
			else
				++histo[ ( 127 - (unsigned)buf[i] ) >> 3 ];
		}
		agc->numInHisto += len;
		agc->remainingScanSps -= len/2;

		distrib[15] = histo[15];
		for ( k = 14; k >= 8; --k )
			distrib[k] = distrib[k+1] + histo[k];
	}

	/* detect oversteering */
	if ( 64 * distrib[15] >= agc->numInHisto	/* max more often than 1.56% (= 100/64) of all near 1 */
	   ||16 * distrib[12] >= agc->numInHisto	/* more often than 6.25% of all >= 0.75 */
	   || 4 * distrib[ 8] >= agc->numInHisto )	/* more often than 25% of all >= 0.5 */
	{
		const int N = agc->numInHisto;
#if 0
		fprintf(stderr, "dp[8-15]: ");
		for ( int k = 8; k < 16; ++k )
			fprintf(stderr, "%d:%d, ", k, (distrib[k] * 100) / N);
		fprintf(stderr, "\ttotal %d\n", N);
#endif

		if ( agc->gainIdx > 0 )
		{
			int k, numGains = 0;
			const int * gains = get_tuner_gains(dev, &numGains );

			agc->remainingDeadSps = INT_MAX;
			agc->remainingScanSps = agc->scanTimeSps;
			agc->numInHisto = 0;
			for ( k = 0; k < 16; ++k )
				agc->histo[k] = 0;

			-- agc->gainIdx;
			agc->command_newGain = gains[agc->gainIdx];
			agc->command_changeGain = 1;
			safe_cond_signal(&agc->cond, &agc->mutex);
		}
		return ( agc->agcState == SOFTSTATE_RESET_CONT ) ? 0 : 1;
	}

	if ( agc->remainingScanSps < 0 )
	{
		/* TODO: check if we should increase gain .. or even activate Bias-T */
		if ( dev->verbose )
			fprintf(stderr, "*** rtlsdr softagc(): no more remaining samples to wait for\n");

		agc->remainingScanSps = 0;
		switch ( agc->softAgcMode )
		{
		case SOFTAGC_OFF:
		case SOFTAGC_ON_CHANGE:
			switch ( agc->agcState )
			{
			case SOFTSTATE_OFF:
			case SOFTSTATE_RESET_CONT:
				agc->agcState = SOFTSTATE_OFF;
				if ( dev->verbose )
					fprintf(stderr, "softagc finished. now mode %d, state %d\n", agc->softAgcMode, agc->agcState);
				return 1;
			case SOFTSTATE_ON:
			case SOFTSTATE_RESET:
			case SOFTSTATE_INIT:
				return 1;
			}
			break;
		case SOFTAGC_AUTO_ATTEN:
		case SOFTAGC_AUTO:
			agc->agcState == SOFTSTATE_ON;
			return 1;
		}
	}
	
	return ( agc->agcState == SOFTSTATE_RESET_CONT ) ? 0 : 1;
}


static void LIBUSB_CALL _libusb_callback(struct libusb_transfer *xfer)
{
	rtlsdr_dev_t *dev = (rtlsdr_dev_t *)xfer->user_data;

	if (LIBUSB_TRANSFER_COMPLETED == xfer->status) {
		int keepBlock = 1;
		if ( dev->softagc.agcState != SOFTSTATE_OFF )
			keepBlock = softagc(dev, xfer->buffer, xfer->actual_length);

		if (dev->cb && keepBlock)
			dev->cb(xfer->buffer, xfer->actual_length, dev->cb_ctx);

		libusb_submit_transfer(xfer); /* resubmit transfer */
		dev->xfer_errors = 0;
	} else if (LIBUSB_TRANSFER_CANCELLED != xfer->status) {
#ifndef _WIN32
		if (LIBUSB_TRANSFER_ERROR == xfer->status)
			dev->xfer_errors++;

		if (dev->xfer_errors >= dev->xfer_buf_num ||
				LIBUSB_TRANSFER_NO_DEVICE == xfer->status) {
#endif
			dev->dev_lost = 1;
			rtlsdr_cancel_async(dev);
			fprintf(stderr, "cb transfer status: %d, "
				"canceling...\n", xfer->status);
#ifndef _WIN32
		}
#endif
	}
}

int rtlsdr_wait_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_wait_async(dev, cb, ctx);
	}
	#endif

	return rtlsdr_read_async(dev, cb, ctx, 0, 0);
}

static int _rtlsdr_alloc_async_buffers(rtlsdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (!dev->xfer) {
		dev->xfer = malloc(dev->xfer_buf_num *
					 sizeof(struct libusb_transfer *));

		for(i = 0; i < dev->xfer_buf_num; ++i)
			dev->xfer[i] = libusb_alloc_transfer(0);
	}

	if (dev->xfer_buf)
		return -2;

	dev->xfer_buf = malloc(dev->xfer_buf_num * sizeof(unsigned char *));
	memset(dev->xfer_buf, 0, dev->xfer_buf_num * sizeof(unsigned char *));

#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
	fprintf(stderr, "Allocating %d zero-copy buffers\n", dev->xfer_buf_num);

	dev->use_zerocopy = 1;
	for (i = 0; i < dev->xfer_buf_num; ++i) {
		dev->xfer_buf[i] = libusb_dev_mem_alloc(dev->devh, dev->xfer_buf_len);

		if (!dev->xfer_buf[i]) {
			fprintf(stderr, "Failed to allocate zero-copy "
					"buffer for transfer %d\nFalling "
					"back to buffers in userspace\n", i);

			dev->use_zerocopy = 0;
			break;
		}
	}

	/* zero-copy buffer allocation failed (partially or completely)
	 * we need to free the buffers again if already allocated */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i])
				libusb_dev_mem_free(dev->devh,
						    dev->xfer_buf[i],
						    dev->xfer_buf_len);
		}
	}
#endif

	/* no zero-copy available, allocate buffers in userspace */
	if (!dev->use_zerocopy) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			dev->xfer_buf[i] = malloc(dev->xfer_buf_len);

			if (!dev->xfer_buf[i])
				return -ENOMEM;
		}
	}

	return 0;
}

static int _rtlsdr_free_async_buffers(rtlsdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (dev->xfer) {
		for(i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer[i]) {
				libusb_free_transfer(dev->xfer[i]);
			}
		}

		free(dev->xfer);
		dev->xfer = NULL;
	}

	if (dev->xfer_buf) {
		for (i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i]) {
				if (dev->use_zerocopy) {
#if defined (__linux__) && LIBUSB_API_VERSION >= 0x01000105
					libusb_dev_mem_free(dev->devh,
							    dev->xfer_buf[i],
							    dev->xfer_buf_len);
#endif
				} else {
					free(dev->xfer_buf[i]);
				}
			}
		}

		free(dev->xfer_buf);
		dev->xfer_buf = NULL;
	}

	return 0;
}

int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx,
				uint32_t buf_num, uint32_t buf_len)
{
	unsigned int i;
	int r = 0;
	struct timeval tv = { 1, 0 };
	struct timeval zerotv = { 0, 0 };
	enum rtlsdr_async_status next_status = RTLSDR_INACTIVE;

	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_read_async(dev, cb, ctx, buf_num, buf_len);
	}
	#endif

	if (!dev)
		return -1;

	if (RTLSDR_INACTIVE != dev->async_status)
		return -2;

	dev->async_status = RTLSDR_RUNNING;
	dev->async_cancel = 0;

	dev->cb = cb;
	dev->cb_ctx = ctx;

	if (buf_num > 0)
		dev->xfer_buf_num = buf_num;
	else
		dev->xfer_buf_num = DEFAULT_BUF_NUMBER;

	if (buf_len > 0 && buf_len % 512 == 0) /* len must be multiple of 512 */
		dev->xfer_buf_len = buf_len;
	else
		dev->xfer_buf_len = DEFAULT_BUF_LENGTH;

	_rtlsdr_alloc_async_buffers(dev);

	for(i = 0; i < dev->xfer_buf_num; ++i) {
		libusb_fill_bulk_transfer(dev->xfer[i],
						dev->devh,
						0x81,
						dev->xfer_buf[i],
						dev->xfer_buf_len,
						_libusb_callback,
						(void *)dev,
						BULK_TIMEOUT);

		r = libusb_submit_transfer(dev->xfer[i]);
		if (r < 0) {
			fprintf(stderr, "Failed to submit transfer %i\n"
					"Please increase your allowed " 
					"usbfs buffer size with the "
					"following command:\n"
					"echo 0 > /sys/module/usbcore"
					"/parameters/usbfs_memory_mb\n", i);
			dev->async_status = RTLSDR_CANCELING;
			break;
		}
	}

	while (RTLSDR_INACTIVE != dev->async_status) {
		r = libusb_handle_events_timeout_completed(dev->ctx, &tv,
								 &dev->async_cancel);
		if (r < 0) {
			/*fprintf(stderr, "handle_events returned: %d\n", r);*/
			if (r == LIBUSB_ERROR_INTERRUPTED) /* stray signal */
				continue;
			break;
		}

		if (RTLSDR_CANCELING == dev->async_status) {
			next_status = RTLSDR_INACTIVE;

			if (!dev->xfer)
				break;

			for(i = 0; i < dev->xfer_buf_num; ++i) {
				if (!dev->xfer[i])
					continue;

				if (LIBUSB_TRANSFER_CANCELLED !=
						dev->xfer[i]->status) {
					r = libusb_cancel_transfer(dev->xfer[i]);
					/* handle events after canceling
					 * to allow transfer status to
					 * propagate */
					libusb_handle_events_timeout_completed(dev->ctx,
												 &zerotv, NULL);
					if (r < 0)
						continue;

					next_status = RTLSDR_CANCELING;
				}
			}

			if (dev->dev_lost || RTLSDR_INACTIVE == next_status) {
				/* handle any events that still need to
				 * be handled before exiting after we
				 * just cancelled all transfers */
				libusb_handle_events_timeout_completed(dev->ctx,
											 &zerotv, NULL);
				break;
			}
		}
	}

	_rtlsdr_free_async_buffers(dev);

	dev->async_status = next_status;

	return r;
}

int rtlsdr_cancel_async(rtlsdr_dev_t *dev)
{
	#ifdef _ENABLE_RPC
	if (rtlsdr_rpc_is_enabled())
	{
	  return rtlsdr_rpc_cancel_async(dev);
	}
	#endif

	if (!dev)
		return -1;

	/* if streaming, try to cancel gracefully */
	if (RTLSDR_RUNNING == dev->async_status) {
		dev->async_status = RTLSDR_CANCELING;
		dev->async_cancel = 1;
		return 0;
	}

	/* if called while in pending state, change the state forcefully */
#if 0
	if (RTLSDR_INACTIVE != dev->async_status) {
		dev->async_status = RTLSDR_INACTIVE;
		return 0;
	}
#endif
	return -2;
}

uint32_t rtlsdr_get_tuner_clock(void *dev)
{
	uint32_t tuner_freq;

	if (!dev)
		return 0;

	/* read corrected clock value */
	if (rtlsdr_get_xtal_freq((rtlsdr_dev_t *)dev, NULL, &tuner_freq))
		return 0;

	return tuner_freq;
}

int rtlsdr_i2c_write_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
	if (dev)
		return rtlsdr_i2c_write(((rtlsdr_dev_t *)dev), addr, buf, len);

	return -1;
}

int rtlsdr_i2c_read_fn(void *dev, uint8_t addr, uint8_t *buf, int len)
{
	if (dev)
		return rtlsdr_i2c_read(((rtlsdr_dev_t *)dev), addr, buf, len);

	return -1;
}

/* Infrared (IR) sensor support
 * based on Linux dvb_usb_rtl28xxu drivers/media/usb/dvb-usb-v2/rtl28xxu.h
 * Copyright (C) 2009 Antti Palosaari <crope@iki.fi>
 * Copyright (C) 2011 Antti Palosaari <crope@iki.fi>
 * Copyright (C) 2012 Thomas Mair <thomas.mair86@googlemail.com>
 */

struct rtl28xxu_req {
	uint16_t value;
	uint16_t index;
	uint16_t size;
	uint8_t *data;
};

struct rtl28xxu_reg_val {
	uint16_t reg;
	uint8_t val;
};

struct rtl28xxu_reg_val_mask {
	int block;
	uint16_t reg;
	uint8_t val;
	uint8_t mask;
};

static int rtlsdr_read_regs(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr, uint8_t *data, uint8_t len)
{
	int r;
	uint16_t index = (block << 8);
	if (block == IRB) index = (SYSB << 8) | 0x01;

	r = libusb_control_transfer(dev->devh, CTRL_IN, 0, addr, index, data, len, CTRL_TIMEOUT);

	if (r < 0)
		fprintf(stderr, "%s failed with %d\n", __FUNCTION__, r);

	return r;
}

static int rtlsdr_write_reg_mask(rtlsdr_dev_t *d, int block, uint16_t reg, uint8_t val,
		uint8_t mask)
{
	int ret;
	uint8_t tmp;

	/* no need for read if whole reg is written */
	if (mask != 0xff) {
		tmp = rtlsdr_read_reg(d, block, reg, 1);

		val &= mask;
		tmp &= ~mask;
		val |= tmp;
	}

	return rtlsdr_write_reg(d, block, reg, (uint16_t)val, 1);
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

int rtlsdr_ir_query(rtlsdr_dev_t *d, uint8_t *buf, size_t buf_len)
{
	int ret = -1;
	size_t i, len;
	static const struct rtl28xxu_reg_val_mask refresh_tab[] = {
		{IRB, IR_RX_IF,			   0x03, 0xff},
		{IRB, IR_RX_BUF_CTRL,		 0x80, 0xff},
		{IRB, IR_RX_CTRL,			 0x80, 0xff},
	};

	/* init remote controller */
	if (!d->rc_active) {
		//fprintf(stderr, "initializing remote controller\n");
		static const struct rtl28xxu_reg_val_mask init_tab[] = {
			{USBB, DEMOD_CTL,			 0x00, 0x04},
			{USBB, DEMOD_CTL,			 0x00, 0x08},
			{USBB, USB_CTRL,			  0x20, 0x20},
			{USBB, GPD,				   0x00, 0x08},
			{USBB, GPOE,				  0x08, 0x08},
			{USBB, GPO,				   0x08, 0x08},
			{IRB, IR_MAX_DURATION0,	   0xd0, 0xff},
			{IRB, IR_MAX_DURATION1,	   0x07, 0xff},
			{IRB, IR_IDLE_LEN0,		   0xc0, 0xff},
			{IRB, IR_IDLE_LEN1,		   0x00, 0xff},
			{IRB, IR_GLITCH_LEN,		  0x03, 0xff},
			{IRB, IR_RX_CLK,			  0x09, 0xff},
			{IRB, IR_RX_CFG,			  0x1c, 0xff},
			{IRB, IR_MAX_H_TOL_LEN,	   0x1e, 0xff},
			{IRB, IR_MAX_L_TOL_LEN,	   0x1e, 0xff},
			{IRB, IR_RX_CTRL,			 0x80, 0xff},
		};

		for (i = 0; i < ARRAY_SIZE(init_tab); i++) {
			ret = rtlsdr_write_reg_mask(d, init_tab[i].block, init_tab[i].reg,
					init_tab[i].val, init_tab[i].mask);
			if (ret < 0) {
				fprintf(stderr, "write %zd reg %d %.4x %.2x %.2x failed\n", i, init_tab[i].block,
						init_tab[i].reg, init_tab[i].val, init_tab[i].mask);
				goto err;
			}
		}

		d->rc_active = 1;
		//fprintf(stderr, "rc active\n");
	}
	// TODO: option to ir disable

	buf[0] = rtlsdr_read_reg(d, IRB, IR_RX_IF, 1);

	if (buf[0] != 0x83) {
		if (buf[0] == 0 || // no IR signal
			// also observed: 0x82, 0x81 - with lengths 1, 5, 0.. unknown, sometimes occurs at edges
			// "IR not ready"? causes a -7 timeout if we read
			buf[0] == 0x82 || buf[0] == 0x81) {
			// graceful exit
		} else {
			fprintf(stderr, "read IR_RX_IF unexpected: %.2x\n", buf[0]);
		}

		ret = 0;
		goto exit;
	}

	buf[0] = rtlsdr_read_reg(d, IRB, IR_RX_BC, 1);

	len = buf[0];
	//fprintf(stderr, "read IR_RX_BC len=%d\n", len);

	if (len > buf_len) {
		//fprintf(stderr, "read IR_RX_BC too large for buffer, %lu > %lu\n", buf_len, buf_len);
		goto exit;
	}

	/* read raw code from hw */
	ret = rtlsdr_read_regs(d, IRB, IR_RX_BUF, buf, len);
	if (ret < 0)
		goto err;

	/* let hw receive new code */
	for (i = 0; i < ARRAY_SIZE(refresh_tab); i++) {
		ret = rtlsdr_write_reg_mask(d, refresh_tab[i].block, refresh_tab[i].reg,
				refresh_tab[i].val, refresh_tab[i].mask);
		if (ret < 0)
			goto err;
	}

	// On success return length
	ret = len;

exit:
	return ret;
err:
	printf("failed=%d\n", ret);
	return ret;
}

int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on)
{
	if (!dev)
		return -1;

	rtlsdr_set_gpio_output(dev, 0);
	rtlsdr_set_gpio_bit(dev, 0, on);
	reactivate_softagc(dev, SOFTSTATE_RESET);

	return 0;
}


const char * rtlsdr_get_opt_help(int longInfo)
{
	if ( longInfo )
		return
		"\t[-O\tset RTL options string seperated with ':' ]\n"
		"\t\tverbose:f=<freqHz>:bw=<bw_in_kHz>:bc=<if_in_Hz>\n"
		"\t\tagc=<tuner_gain_mode>:agcv=<>:gain=<tenth_dB>:dagc=<rtl_agc>\n"
		"\t\tds=<direct_sampling_mode>:T=<bias_tee>\n"
#ifdef WITH_UDP_SERVER
		"\t\tport=<udp_port default with 1>\n"
#endif
		;
	else
		return
		"\t[-O\tset RTL options string seperated with ':' ]\n";
}

int rtlsdr_set_opt_string(rtlsdr_dev_t *dev, const char *opts, int verbose)
{
	char * optStr, * optPart;
	int retAll = 0;

	if (!dev)
		return -1;

	/* set some defaults */
	dev->softagc.deadTimeMs = 100;
	dev->softagc.scanTimeMs = 100;

	optStr = strdup(opts);
	if (!optStr)
		return -1;

	optPart = strtok(optStr, ":,");
	while (optPart)
	{
		int ret = 0;
		if (!strcmp(optPart, "verbose")) {
			dev->verbose = 1;
		}
		else if (!strncmp(optPart, "f=", 2)) {
			uint32_t freq = (uint32_t)atol(optPart + 2);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed frequency %u\n", (unsigned)freq);
			ret = rtlsdr_set_center_freq(dev, freq);
		}
		else if (!strncmp(optPart, "bw=", 3)) {
			uint32_t bw = (uint32_t)( atol(optPart +3) * 1000 );
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed bandwidth %u\n", (unsigned)bw);
			ret = rtlsdr_set_tuner_bandwidth(dev, bw);
		}
		else if (!strncmp(optPart, "bc=", 3)) {
			int32_t if_band_center_freq = (int32_t)(atoi(optPart +3));
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed band center %d\n", (int)if_band_center_freq);
			ret = rtlsdr_set_tuner_band_center(dev, if_band_center_freq );
		}
		else if (!strncmp(optPart, "agc=", 4)) {
			int manual = 1 - atoi(optPart +4);	/* invert logic */
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed tuner gain mode, manual=%d\n", manual);
			ret = rtlsdr_set_tuner_gain_mode(dev, manual);
		}
		else if (!strncmp(optPart, "gain=", 5)) {
			int gain = atoi(optPart +5);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed tuner gain = %d /10 dB\n", gain);
			ret = rtlsdr_set_tuner_gain(dev, gain);
		}
		else if (!strncmp(optPart, "agcv=", 5)) {
			int agcv = atoi(optPart +5);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed tuner agc variant = %d\n", agcv);
			ret = rtlsdr_set_tuner_agc_mode(dev, agcv);
		}
		else if (!strncmp(optPart, "dagc=", 5)) {
			int on = atoi(optPart +5);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed rtl/digital gain mode %d\n", on);
			ret = rtlsdr_set_agc_mode(dev, on);
		}
		else if (!strncmp(optPart, "ds=", 3)) {
			int on = atoi(optPart +3);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed direct sampling mode %d\n", on);
			ret = rtlsdr_set_direct_sampling(dev, on);
		}
		else if (!strncmp(optPart, "t=", 2) || !strncmp(optPart, "T=", 2)) {
			int on = atoi(optPart +2);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed bias tee %d\n", on);
			ret = rtlsdr_set_bias_tee(dev, on);
		}
		else if (!strncmp(optPart, "softagc=", 8)) {
			int on = atoi(optPart +8);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed soft agc mode %d\n", on);
			dev->softagc.softAgcMode = on;
			dev->softagc.agcState = on ? SOFTSTATE_INIT : SOFTSTATE_OFF;
		}
		else if (!strncmp(optPart, "softscantime=", 13)) {
			float d = atof(optPart +13);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed soft agc scan time %f ms\n", d);
			dev->softagc.scanTimeMs = d;
		}
		else if (!strncmp(optPart, "softdeadtime=", 13)) {
			float d = atof(optPart +13);
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed soft agc dead time %f ms\n", d);
			dev->softagc.deadTimeMs = d;
		}
#ifdef WITH_UDP_SERVER
		else if (!strncmp(optPart, "port=", 5)) {
			int udpPortNo = atoi(optPart +5);
			if ( udpPortNo == 1 )
				udpPortNo = 32323;
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): UDP control server port\n", udpPortNo);
			dev->udpPortNo = udpPortNo;
		}
#endif
		else {
			if (verbose)
				fprintf(stderr, "rtlsdr_set_opt_string(): parsed unknown option '%s'\n", optPart);
			ret = -1;  /* unknown option */
		}
		if (verbose)
			fprintf(stderr, "  application of option returned %d\n", ret);
		if (ret < 0)
			retAll = ret;
		optPart = strtok(NULL, ":,");
	}

	if ( dev->softagc.agcState != SOFTSTATE_OFF )
		softagc_init(dev);

#ifdef WITH_UDP_SERVER
	if (dev->udpPortNo && dev->srv_started == 0 && dev->tuner_type==RTLSDR_TUNER_R820T) {
		dev->handled = 1;
		dev->saved_27 = dev->tuner->get_i2c_register(dev,27);		/* highest/lowest corner for LPNF and LPF */
		// signal(SIGPIPE, SIG_IGN);
		if(pthread_create(&dev->srv_thread, NULL, srv_server, dev)) {
			fprintf(stderr, "Error creating thread\n");
		}
		else {
			dev->srv_started = 1;
			fprintf(stderr, "UDP server started on port %u\n", dev->udpPortNo);
		}
	}
#endif

	free(optStr);
	return retAll;
}

