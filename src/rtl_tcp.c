/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include "getopt/getopt.h"
#define usleep(x) Sleep(x/1000)
#endif

#ifdef NEED_PTHREADS_WORKARROUND
#define HAVE_STRUCT_TIMESPEC
#endif
#include <pthread.h>

#include <rtl-sdr.h>
#include <rtl_app_ver.h>
#include <rtl_tcp.h>
#include "convenience/convenience.h"
#include "convenience/rtl_convenience.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")

typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#endif

#include "controlThread.h"

static ctrl_thread_data_t ctrldata;

static SOCKET s;

static pthread_t tcp_worker_thread;
static pthread_t command_thread;
static pthread_cond_t exit_cond;
static pthread_mutex_t exit_cond_lock;

static pthread_mutex_t ll_mutex;
static pthread_cond_t cond;

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

static rtlsdr_dev_t *dev = NULL;

static int verbosity = 0;
static uint32_t bandwidth = 0;

static int enable_biastee = 0;
static int global_numq = 0;
static struct llist *ll_buffers = 0;
static int llbuf_num = 500;

static volatile int do_exit = 0;

void usage(void)
{
	fprintf(stderr,
		"rtl_tcp, an I/Q spectrum server for RTL2832 based SDR-receivers\n"
		"rtl_tcp version %d.%d %s (%s)\n"
		"rtl-sdr library %d.%d %s\n\n",
		APP_VER_MAJOR, APP_VER_MINOR, APP_VER_ID, __DATE__,
		rtlsdr_get_version() >>16, rtlsdr_get_version() & 0xFFFF,
		rtlsdr_get_ver_id() );
	fprintf(stderr, "Usage:\trtl_tcp [-a listen address]\n"
		"\t[-p control listen port (default: 1234)]\n"
		"\t[-r response listen port: 0 = off; 1 (=default) for On at control listen port +1; or port]\n"
		"\t[-I infrared sensor listen port (default: 0=none)]\n"
		"\t[-W infrared sensor query wait interval usec (default: 10000)]\n"
		"\t[-f frequency to tune to [Hz]]\n"
		"\t[-g gain in dB (default: 0 for auto)]\n"
		"\t[-s samplerate in Hz (default: 2048000 Hz)]\n"
		"\t[-b number of buffers (default: 15, set by library)]\n"
		"\t[-l length of single buffer in units of 512 samples (default: 32 was 256)]\n"
		"\t[-n max number of linked list buffers to keep (default: 500)]\n"
		"\t[-w rtlsdr tuner bandwidth [Hz] (for R820T/2 and E4000 tuners)]\n"
		"\t[-d device index or :<serial number> (default: 0)]\n"
		"\t[-P ppm_error (default: 0)]\n"
		"%s"
		"\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n"

		"\t[-D direct_sampling_mode (default: 0, 1 = I, 2 = Q, 3 = I below threshold, 4 = Q below threshold)]\n"
		"\t[-D direct_sampling_threshold_frequency (default: 0 use tuner specific frequency threshold for 3 and 4)]\n"
		"\t[-N no dithering (default: use dithering)]\n" 
		"\t[-v increase verbosity (default: 0)]\n"
		, rtlsdr_get_opt_help(1) );
	exit(1);
}

#ifdef _WIN32
int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
#ifdef _MSC_VER
		tmp -= 11644473600000000Ui64;
#else
		tmp -= 11644473600000000ULL;
#endif
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	rtlsdr_cancel_async(dev);
	do_exit = 1;
}
#endif

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	if(!do_exit) {
		struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
		rpt->data = (char*)malloc(len);
		memcpy(rpt->data, buf, len);
		rpt->len = len;
		rpt->next = NULL;

		pthread_mutex_lock(&ll_mutex);

		if (ll_buffers == NULL) {
			ll_buffers = rpt;
		} else {
			struct llist *cur = ll_buffers;
			int num_queued = 0;

			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}

			if(llbuf_num && llbuf_num == num_queued-2){
				struct llist *curelem;

				free(ll_buffers->data);
				curelem = ll_buffers->next;
				free(ll_buffers);
				ll_buffers = curelem;
			}

			cur->next = rpt;

			if ( verbosity )
			{
				if (num_queued > global_numq)
					printf("ll+, now %d\n", num_queued);
				else if (num_queued < global_numq)
					printf("ll-, now %d\n", num_queued);
			}

			global_numq = num_queued;
		}
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&ll_mutex);
	}
}

static void *tcp_worker(void *arg)
{
	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	fd_set writefds;
	int r = 0;

	while(1) {
		if(do_exit)
			pthread_exit(0);

		pthread_mutex_lock(&ll_mutex);
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec+1;
		ts.tv_nsec = tp.tv_usec * 1000;
		r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
		if(r == ETIMEDOUT) {
			pthread_mutex_unlock(&ll_mutex);
			printf("worker cond timeout\n");
			sighandler(0);
			pthread_exit(NULL);
		}

		curelem = ll_buffers;
		ll_buffers = 0;
		pthread_mutex_unlock(&ll_mutex);

		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(s, &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(s+1, NULL, &writefds, NULL, &tv);
				if(r) {
					bytessent = send(s,  &curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(bytessent == SOCKET_ERROR || do_exit) {
						printf("worker socket bye\n");
						sighandler(0);
						pthread_exit(NULL);
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	}
}

static int set_gain_by_index(rtlsdr_dev_t *_dev, unsigned int index)
{
	int res = 0;
	int* gains;
	int count = rtlsdr_get_tuner_gains(_dev, NULL);

	if (count > 0 && index < (unsigned int)count) {
		gains = malloc(sizeof(int) * count);
		count = rtlsdr_get_tuner_gains(_dev, gains);

		if (verbosity)
			printf("set tuner gain to %.1f dB\n", gains[index] / 10.0);
		res = rtlsdr_set_tuner_gain(_dev, gains[index]);
		if (res < 0)
		printf("  setting tuner gain index failed\n");

		free(gains);
	}
	else
	{
		printf("set tuner gain index to %u\n", index);
		printf("  error setting tuner gain index failed: valid range: 0 .. %d\n", count-1);
	}

	return res;
}

static void check_tuner_pll(rtlsdr_dev_t *dev, int *tuner_unsupported, int *last_lock_report)
{
	int r = rtlsdr_is_tuner_PLL_locked(dev);
	/* printf("performed lock check:\n"); */
	if (r == 1) {
		if (*last_lock_report != r)
			printf("tuner PLL is unlocked!\n");
		*last_lock_report = r;
	}
	else if (r == 0) {
		if (*last_lock_report != r)
			printf("tuner PLL is locked.\n");
		*last_lock_report = r;
	}
	else if (r == -2) {
		printf("error at PLL-locked check: tuner not supported! No further tests.\n");
		*tuner_unsupported = 1;
	}
	else if (r < 0)
		printf("error checking tuner PLL!\n");
	else
		printf("unknown error at tuner PLL check!\n");
	fflush(stdout);
}




#ifdef _WIN32
#define __attribute__(x)
#pragma pack(push, 1)
#endif
struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));
#ifdef _WIN32
#pragma pack(pop)
#endif
static void *command_worker(void *arg)
{
	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	unsigned tuner_check_timeout = 0;
	int last_lock_report = -1;
	int tuner_unsupported = 0;
	int r = 0;
	uint32_t freqhi = 0;
	uint64_t tmp64;
	uint32_t tmp;
	int32_t itmp;
	int32_t if_band_center_freq;
	int iitmp;

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(s+1, &readfds, NULL, NULL, &tv);
			if(r) {
				received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
				left -= received;
				/* printf("received %d bytes\n", received); */
			}
			else if (!tuner_unsupported)
			{
				/* timeout: nothing happend */
				++tuner_check_timeout;
				if (tuner_check_timeout >= 3)
				{
					/* automatic check every 3 seconds */
					check_tuner_pll(dev, &tuner_unsupported, &last_lock_report);
					tuner_check_timeout = 0;
				}
				fflush(stdout);
			}
			if(received == SOCKET_ERROR || do_exit) {
				printf("comm recv bye\n");
				sighandler(0);
				pthread_exit(NULL);
			}
		}
		switch(cmd.cmd) {
		case SET_DITHERING: 
			tmp = ntohl(cmd.param);
			if (!tmp) {
				printf("Disabling dithering...  \n");
				r = rtlsdr_set_dithering(dev, tmp);
				if (r) {
					printf("failure\n");
				} else {
					printf("success\n");
				}
			}
			break;
		case SET_FREQUENCY:
			tmp = ntohl(cmd.param);
			if (!freqhi)
			{
				printf("set freq %f MHz\n", tmp * 1E-6);
				r = rtlsdr_set_center_freq(dev, tmp);
				if (r < 0) {
					printf("  error setting frequency!\n");
					last_lock_report = -1;
				}
			}
			else
			{
				tmp64 = ( ((uint64_t)freqhi) << 32 ) | (uint64_t)tmp;
				printf("set freq64 %f MHz\n", tmp64 * 1E-6);
				r = rtlsdr_set_center_freq64(dev, tmp64);
				if (r < 0) {
					printf("  error setting frequency!\n");
					last_lock_report = -1;
				}
			}
			freqhi = 0;
			break;
		case SET_FREQ_HI32:
			freqhi = ntohl(cmd.param);
			break;
		case SET_SAMPLE_RATE:
			tmp = ntohl(cmd.param);
			printf("set sample rate %u\n", tmp);
			r = rtlsdr_set_sample_rate(dev, tmp);
			if (r < 0)
				printf("  error setting sample rate! sample rate is %u\n", rtlsdr_get_sample_rate(dev));
			break;
		case SET_GAIN_MODE:
			tmp = ntohl(cmd.param);
			printf("set gain mode %u (=%s)\n", tmp, tmp?"manual":"automatic");
			r = rtlsdr_set_tuner_gain_mode(dev, tmp);
			if (r < 0)
				printf("  error setting gain mode!\n");
			break;
		case SET_GAIN:
			tmp = ntohl(cmd.param);
			printf("set manual tuner gain %.1f dB\n", tmp/10.0);
			r = rtlsdr_set_tuner_gain(dev, tmp);
			if (r < 0)
				printf("  error setting tuner gain!\n");
			break;
		case SET_FREQUENCY_CORRECTION:
			itmp = ntohl(cmd.param);
			printf("set freq correction %d ppm\n", itmp);
			r = rtlsdr_set_freq_correction(dev, itmp);
			if (r < 0) {
				printf("  error setting frequency correction!\n");
				last_lock_report = -1;
			}
			break;
		case SET_IF_STAGE:
			tmp = ntohl(cmd.param);
			printf("set if stage %d gain %.1f dB\n", tmp >> 16, ((short)(tmp & 0xffff))/10.0);
			r = rtlsdr_set_tuner_if_gain(dev, tmp >> 16, (short)(tmp & 0xffff));
			if (r < 0)
				printf("  error setting gain for stage!\n");
			break;
		case SET_TEST_MODE:
			tmp = ntohl(cmd.param);
			printf("set test mode %d (=%s)\n", tmp, tmp?"active":"inactive");
			r = rtlsdr_set_testmode(dev, tmp);
			if (r < 0)
				printf("  error setting test mode!\n");
			break;
		case SET_AGC_MODE:
			tmp = ntohl(cmd.param);
			printf("set rtl2832's digital agc mode %d (=%s)\n", tmp, tmp?"enabled":"disabled");
			r = rtlsdr_set_agc_mode(dev, tmp);
			if (r < 0)
				printf("  error setting digital agc mode!\n");
			break;
		case SET_DIRECT_SAMPLING:
			tmp = ntohl(cmd.param);
			printf("set direct sampling %u (=%s)\n", tmp, (!tmp) ? "disabled": (tmp==1)?"pin I-ADC": (tmp==2)? "pin Q-ADC":"unknown!");
			r = rtlsdr_set_direct_sampling(dev, tmp);
			if (r < 0)
				printf("  error setting direct sampling!\n");
			break;
		case SET_OFFSET_TUNING:
			itmp = ntohl(cmd.param);
			printf("set offset tuning %d\n", itmp);
			r = rtlsdr_set_offset_tuning(dev, itmp);
			if (r < 0) {
				printf("  error setting offset tuning!\n");
				last_lock_report = -1;
			}
			break;
		case SET_RTL_CRYSTAL:
			printf("set rtl xtal frequency %d\n", ntohl(cmd.param));
			r = rtlsdr_set_xtal_freq(dev, ntohl(cmd.param), 0);
			if (r < 0)
				printf("  error setting rtl xtal frequency!\n");
			break;
		case SET_TUNER_CRYSTAL:
			printf("set tuner xtal %d\n", ntohl(cmd.param));
			r = rtlsdr_set_xtal_freq(dev, 0, ntohl(cmd.param));
			if (r < 0)
				printf("  error setting tuner xtal frequency!\n");
			break;
		case SET_TUNER_GAIN_BY_INDEX:
			tmp = ntohl(cmd.param);
			printf("set tuner gain by index %u\n", tmp);
			set_gain_by_index(dev, tmp);
			break;
		case SET_BIAS_TEE:
			tmp = ntohl(cmd.param);
			printf("set bias T %u (%s)\n", tmp, tmp?"on":"off");
			r = rtlsdr_set_bias_tee(dev, tmp);
			if (r < 0)
				printf("  error setting bias tee!\n");
			break;
		case SET_TUNER_BANDWIDTH:
			bandwidth = ntohl(cmd.param);
			printf("set tuner bandwidth to %i Hz\n", bandwidth);
			verbose_set_bandwidth(dev, bandwidth);
			break;
		case SET_I2C_TUNER_REGISTER:
			tmp = ntohl(cmd.param);
			printf("set i2c register x%03X to x%03X with mask x%02X\n", (tmp >> 20) & 0xfff, tmp & 0xfff, (tmp >> 12) & 0xff );
			r = rtlsdr_set_tuner_i2c_register(dev, (tmp >> 20) & 0xfff, (tmp >> 12) & 0xff, tmp & 0xfff);
			if (r < 0)
				printf("  error setting i2c register!\n");
			break;
		case SET_I2C_TUNER_OVERRIDE:
			tmp = ntohl(cmd.param);
			printf("set i2c override register x%03X to x%03X with mask x%02X\n", (tmp >> 20) & 0xfff, tmp & 0xfff, (tmp >> 12) & 0xff );
			r = rtlsdr_set_tuner_i2c_override(dev, (tmp >> 20) & 0xfff, (tmp >> 12) & 0xff, tmp & 0xfff);
			if (r < 0)
				printf("  error setting i2c register!\n");
			break;
		case UDP_TERMINATE:
			printf("comm recv bye\n");
			sighandler(0);
			pthread_exit(NULL);
			break;
		case SET_TUNER_BW_IF_CENTER:
			if_band_center_freq = ntohl(cmd.param);
			printf("set tuner band to IF frequency %i Hz from center\n", if_band_center_freq);
			r = rtlsdr_set_tuner_band_center(dev, if_band_center_freq );
			if (r < 0)
				printf("  error setting tuner band's IF center frequency!\n");
			break;
		case SET_TUNER_IF_MODE:
			itmp = ntohl(cmd.param);
			printf("set tuner IF mode to %i: ", itmp);
			if (!itmp)
				printf("automatic gain of VGA controlled from RTL2832\n");
			else if (-2500 <= itmp && itmp <= 2500)
				printf("VGA nearest to %.1f dB)\n", itmp/10.0);
			else if (10000 <=itmp && itmp <= 10015)
				printf("VGA gain idx %d\n", itmp - 10000);
			else if (10016 <= itmp && itmp <= 10031)
				printf("VGA gain idx %d - but with automatic gain of VGA controlled from RTL2832\n", itmp-10016);
			else
				printf("unknown!\n");
			r = rtlsdr_set_tuner_if_mode(dev, itmp);
			if (r < 0)
				printf("  error setting tuner IF mode!\n");
			break;
		case SET_SIDEBAND:
			tmp = ntohl(cmd.param);
			if(tmp)
				tmp = 1;
			printf("set tuner sideband %d: %s sideband\n", tmp, (tmp ? "upper" : "lower") );
			r = rtlsdr_set_tuner_sideband(dev, tmp);
			if (r < 0) {
				printf("  error setting tuner sideband!\n");
				last_lock_report = -1;
			}
			break;
		case REPORT_I2C_REGS:
			tmp = ntohl(cmd.param);
			if(tmp)
				tmp = 1;
			ctrldata.report_i2c = tmp;  /* (de)activate reporting */
			break;
		case GPIO_SET_OUTPUT_MODE:	/* rtlsdr_set_gpio_output() */
			itmp = ntohl(cmd.param);
			if ( 0 <= itmp && itmp < 8 )
			{
				printf("set gpio pin %d to output\n", itmp);
				r = rtlsdr_set_gpio_output(dev, (uint8_t)itmp);
				if (r < 0)
					printf("  error setting gpio pin to output mode!\n");
			}
			else
				printf("set gpio pin %d to output: error: pin has to be in 0 .. 7\n", itmp);
			break;
		case GPIO_SET_INPUT_MODE:	/* rtlsdr_set_gpio_input() */
			itmp = ntohl(cmd.param);
			if ( 0 <= itmp && itmp < 8 )
			{
				printf("set gpio pin %d to input\n", itmp);
				r = rtlsdr_set_gpio_input(dev, (uint8_t)itmp);
				if (r < 0)
					printf("  error setting gpio pin to input mode!\n");
			}
			else
				printf("set gpio pin %d to input: error: pin has to be in 0 .. 7\n", itmp);
			break;
		case GPIO_GET_IO_STATUS:	/* rtlsdr_set_gpio_status() */
			r = rtlsdr_set_gpio_status(dev, &iitmp );
			if (r < 0)
				printf("error at requesting gpio io status!\n");
			else
				printf("request for gpio io status: 0x%02x = %d%d%d%d %d%d%d%d for bits 7 .. 0\n",
					iitmp & 0xff,
					(iitmp >>7) & 1, (iitmp >>6) & 1, (iitmp >>5) & 1, (iitmp >>4) & 1,
					(iitmp >>3) & 1, (iitmp >>2) & 1, (iitmp >>1) & 1, iitmp & 1 );
			break;
		case GPIO_WRITE_PIN:		/* rtlsdr_set_gpio_bit() */
			itmp = ntohl(cmd.param);
			if ( 0 <= ((itmp >> 16) & 0xffff) && ((itmp >> 16) & 0xffff) < 8 )
			{
				printf("write %d to gpio %d\n", itmp & 0xffff, (itmp >> 16) & 0xffff);
				rtlsdr_set_gpio_output(dev, (uint8_t)((itmp >> 16) & 0xffff));
				rtlsdr_set_gpio_bit(dev, (uint8_t)((itmp >> 16) & 0xffff), itmp & 0xffff);
			}
			else
				printf("write %d to gpio %d: error: pin has to be in 0 .. 7\n", itmp & 0xffff, (itmp >> 16) & 0xffff);
			break;
		case GPIO_READ_PIN:
			itmp = ntohl(cmd.param);
			if ( 0 <= itmp && itmp < 8 )
			{
				r = rtlsdr_get_gpio_bit(dev, itmp, &iitmp);
				if (r < 0)
					printf("  error reading gpio pin!\n");
				else
					printf("read gpio pin %d: %d\n", itmp, iitmp);
			}
			else
				printf("read gpio pin %d out of range: pin has to be in 0 .. 7\n", itmp);
			break;
		case GPIO_GET_BYTE:
			r = rtlsdr_get_gpio_byte(dev, &iitmp);
			if (r < 0)
				printf("error reading gpio byte!\n");
			else
				printf("read gpio byte: 0x%02x = %d%d%d%d %d%d%d%d for bits 7 .. 0\n",
					iitmp & 0xff,
					(iitmp >>7) & 1, (iitmp >>6) & 1, (iitmp >>5) & 1, (iitmp >>4) & 1,
					(iitmp >>3) & 1, (iitmp >>2) & 1, (iitmp >>1) & 1, iitmp & 1 );
			break;
		case IS_TUNER_PLL_LOCKED:
			itmp = -1; /* always print lock status */
			check_tuner_pll(dev, &tuner_unsupported, &itmp);
			if (itmp != -1)
				last_lock_report = itmp;
			tuner_check_timeout = 0;
			break;
		default:
			printf("unknown command 0x%02x\n", cmd.cmd);
			break;
		}
		cmd.cmd = 0xff;
	}
}

struct ir_thread_data
{
	rtlsdr_dev_t *dev;
	int port;
	int wait;
	char *addr;
};

void *ir_thread_fn(void *arg)
{
	int r = 1;
	struct linger ling = {1,0};
	SOCKET listensocket;
	SOCKET irsocket;
	struct sockaddr_in local, remote;
	socklen_t rlen;
	uint8_t buf[128];
	int ret = 0, len;

	struct ir_thread_data *data = (struct ir_thread_data *)arg;

	rtlsdr_dev_t *dev = data->dev;
	int port = data->port;
	int wait = data->wait;
	char *addr = data->addr;


	memset(&local,0,sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = inet_addr(addr);

	listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	r = 1;
	setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
	bind(listensocket,(struct sockaddr *)&local,sizeof(local));


	while(1) {
		printf("listening on IR port %d...\n", port);
		listen(listensocket,1);

		irsocket = accept(listensocket,(struct sockaddr *)&remote, &rlen);
		setsockopt(irsocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		printf("IR client accepted!\n");

		while(1) {
		    ret = rtlsdr_ir_query(dev, buf, sizeof(buf));
		    if (ret < 0) {
			printf("rtlsdr_ir_query error %d\n", ret);
			break;
		    }

		    len = ret;

		    ret = send(irsocket, buf, len, 0);
		    if (ret != len){
			printf("incomplete write to ir client: %d != %d\n", ret,len);
			break;
		    }

		    usleep(wait);
		}

		closesocket(irsocket);
	}

	return 0;
}

int main(int argc, char **argv)
{
	int r, opt, i;
	char* addr = "127.0.0.1";
	int port = 1234;
	int port_ir = 0;
	int wait_ir = 10000;
	pthread_t thread_ir;
	pthread_t thread_ctrl; /* -cs- for periodically reading the register values */
	int port_resp = 1;
	int report_i2c = 0;
	int do_exit_thrd_ctrl = 0;
	int dithering = 1; 

	uint64_t frequency = 100000000;
	uint32_t samp_rate = 2048000;
	enum rtlsdr_ds_mode ds_mode = RTLSDR_DS_IQ;
	uint32_t ds_temp, ds_threshold = 0;
	struct sockaddr_in local, remote;
	uint32_t buf_num = 0;
	/* buf_len:
	 * must be multiple of 512 - else it will be overwritten
	 * in rtlsdr_read_async() in librtlsdr.c with DEFAULT_BUF_LENGTH (= 16*32 *512 = 512 *512)
	 *
	 * -> 512*512 -> 1048 ms @ 250 kS  or  81.92 ms @ 3.2 MS (internal default)
	 * ->  32*512 ->   65 ms @ 250 kS  or   5.12 ms @ 3.2 MS (new default)
	 *
	 * usual soundcard as reference:
	 *   512 samples @ 48 kHz ~= 10.6 ms
	 *   512 samples @  8 kHz  = 64 ms
	 */
	uint32_t buf_len = 32 * 512;
	const char * rtlOpts = NULL;
	int dev_index = 0;
	int dev_given = 0;
	int gain = 0;
	int ppm_error = 0;
	struct llist *curelem,*prev;
	pthread_attr_t attr;
	void *status;
	struct timeval tv = {1,0};
	struct linger ling = {1,0};
	SOCKET listensocket;
	socklen_t rlen;
	fd_set readfds;
	u_long blockmode = 1;
	dongle_info_t dongle_info;
	int gains[100];
	const char * opt_str = NULL;
#ifdef _WIN32
	WSADATA wsd;
	i = WSAStartup(MAKEWORD(2,2), &wsd);
#else
	struct sigaction sigact, sigign;
#endif

	opt_str = "a:p:f:g:s:b:n:d:N:P:O:TI:W:l:w:D:vr:";
	while ((opt = getopt(argc, argv, opt_str)) != -1) {
		switch (opt) {
		case 'd':
			if(optarg[0] == ':') {
				char dev_arg[8];
				int device_count, rc;
				char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
				
				device_count = rtlsdr_get_device_count();

				fprintf(stderr, "Found %d device(s):\n", device_count);
				for (i = 0; i < device_count; i++) {
					memset(&vendor[0],  0, 256 * sizeof(char));
					memset(&product[0], 0, 256 * sizeof(char));
					memset(&serial[0],  0, 256 * sizeof(char));
					rc = rtlsdr_get_device_usb_strings(i, vendor, product, serial);
					if ( rc == 0 )
						fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
					else
						fprintf(stderr, "  %d:  %s\n", i, "Failed to query data");
				}
				fprintf(stderr, "\n");

				memcpy(dev_arg, &optarg[1], 8);
				dev_index = rtlsdr_get_index_by_serial(dev_arg);

				if(dev_index < 0) {
					fprintf(stderr, "Could not find RTL-SDR by serial number\n");
					exit(1);
				}
				else {
					fprintf(stderr, "Using device %d: %s\n",
						dev_index, rtlsdr_get_device_name((uint32_t)dev_index));
				}
			}
			else {
				dev_index = verbose_device_search(optarg);
			}
			dev_given = 1;
			break;
		case 'f':
			frequency = (uint64_t)( atofs(optarg) + 0.5 );
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			samp_rate = (uint32_t)atofs(optarg);
			break;
		case 'a':
			addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'r':
			port_resp = atoi(optarg);
			report_i2c = 0;
			break;
		case 'I':
			port_ir = atoi(optarg);
			break;
		case 'W':
			wait_ir = atoi(optarg);
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		case 'l':
			buf_len = 512 * atoi(optarg);
			break;
		case 'n':
			llbuf_num = atoi(optarg);
			break;
		case 'P':
			ppm_error = atoi(optarg);
			break;
		case 'O':
			rtlOpts = optarg;
			break;
		case 'T':
			enable_biastee = 1;
			break;
		case 'N': 
			dithering = 0;
			break;
		case 'w':
			bandwidth = (uint32_t)atofs(optarg);
			break;
		case 'v':
			++verbosity;
			break;
		case 'D':
			ds_temp = (uint32_t)( atofs(optarg) + 0.5 );
			if (ds_temp <= RTLSDR_DS_Q_BELOW)
				ds_mode = (enum rtlsdr_ds_mode)ds_temp;
			else
				ds_threshold = ds_temp;
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (verbosity)
		fprintf(stderr, "verbosity set to %d\n", verbosity);

	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}

	if (dev_index < 0) {
	    exit(1);
	}

	rtlsdr_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
	fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}

#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif
	/* Disable dither */
	if (!dithering) {
		fprintf(stderr, "Disabling dithering...  ");
		r = rtlsdr_set_dithering(dev, dithering);
		if (r) {
			fprintf(stderr, "failure\n");
		} else {
			fprintf(stderr, "success\n");
		}
	}

	/* Set the tuner error */
	verbose_ppm_set(dev, ppm_error);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	if (rtlOpts) {
		rtlsdr_set_opt_string(dev, rtlOpts, verbosity);
	}

	/* Set direct sampling with threshold */
	rtlsdr_set_ds_mode(dev, ds_mode, ds_threshold);

	/* Set the frequency */
	r = rtlsdr_set_center_freq64(dev, frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %f MHz.\n", frequency * 1E-6);

	if (0 == gain) {
		 /* Enable automatic gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
	} else {
		/* Enable manual gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

		/* Set the tuner gain */
		r = rtlsdr_set_tuner_gain(dev, gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", gain/10.0);
	}
	verbose_set_bandwidth(dev, bandwidth);

	rtlsdr_set_bias_tee(dev, enable_biastee);
	if (enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");

	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&exit_cond, NULL);

	if (port_ir) {
		struct ir_thread_data data = {.dev = dev, .port = port_ir, .wait = wait_ir, .addr = addr};

		pthread_create(&thread_ir, NULL, &ir_thread_fn, (void *)(&data));
	}

#if 0
	fprintf(stderr, "enabling Response channel with I2C reporting\n");
	port_resp = 1;
	report_i2c = 1;
#endif
	if ( port_resp == 1 )
		port_resp = port + 1;
	ctrldata.port = port_resp;
	ctrldata.dev = dev;
	ctrldata.addr = addr;
	ctrldata.wait = 500000; /* = 0.5 sec */
	ctrldata.report_i2c = report_i2c;
	ctrldata.pDoExit = &do_exit_thrd_ctrl;
	if ( port_resp ) {
		fprintf(stderr, "activating Response channel on port %d with %s I2C reporting\n"
			, port_resp, (report_i2c ? "active" : "inactive") );
		pthread_create(&thread_ctrl, NULL, &ctrl_thread_fn, &ctrldata);
	}

	memset(&local,0,sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = inet_addr(addr);

	listensocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	r = 1;
	setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));
	bind(listensocket,(struct sockaddr *)&local,sizeof(local));

#ifdef _WIN32
	ioctlsocket(listensocket, FIONBIO, &blockmode);
#else
	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
#endif

	while(1) {
		printf("listening...\n");
		printf("Use the device argument 'rtl_tcp=%s:%d' in OsmoSDR "
		       "(gr-osmosdr) source\n"
		       "to receive samples in GRC and control "
		       "rtl_tcp parameters (frequency, gain, ...).\n",
		       addr, port);
		listen(listensocket,1);

		while(1) {
			FD_ZERO(&readfds);
			FD_SET(listensocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(listensocket+1, &readfds, NULL, NULL, &tv);
			if(do_exit) {
				goto out;
			} else if(r) {
				rlen = sizeof(remote);
				s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
				break;
			}
		}

		setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		printf("client accepted!\n");

		memset(&dongle_info, 0, sizeof(dongle_info));
		memcpy(&dongle_info.magic, "RTL0", 4);

		r = rtlsdr_get_tuner_type(dev);
		if (r >= 0)
			dongle_info.tuner_type = htonl(r);

		r = rtlsdr_get_tuner_gains(dev, gains);
		if (r >= 0)
			dongle_info.tuner_gain_count = htonl(r);
		if (verbosity)
		{
			fprintf(stderr, "Supported gain values (%d): ", r);
			for (i = 0; i < r; i++)
				fprintf(stderr, "%.1f ", gains[i] / 10.0);
			fprintf(stderr, "\n");
		}

		r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
		if (sizeof(dongle_info) != r)
			printf("failed to send dongle information\n");

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);

		r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, buf_num, buf_len);

		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);

		closesocket(s);

		printf("all threads dead..\n");
		curelem = ll_buffers;
		ll_buffers = 0;

		while(curelem != 0) {
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}

		do_exit = 0;
		global_numq = 0;
	}

out:
	rtlsdr_close(dev);
	closesocket(listensocket);
	/* if (port_ir) pthread_join(thread_ir, &status); */

	if ( port_resp ) {
		do_exit_thrd_ctrl = 1;
		pthread_join(thread_ctrl, &status);
	}

	closesocket(s);
#ifdef _WIN32
	WSACleanup();
#endif
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
