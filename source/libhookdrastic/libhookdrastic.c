#define _GNU_SOURCE // for RTLD_NEXT
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_opengles2.h>
#include "xgl.h"

// --------------------------------------------
// XGL extensions
// --------------------------------------------

XGL_Texture *XGL_LoadTexture(XGL_Renderer *renderer, const char *path) {
	SDL_Surface *tmp = IMG_Load(path);
	if (!tmp) {
	    SDL_SetError("IMG_Load %s failed: %s", path, IMG_GetError());
	    return NULL;
	}
	XGL_Texture *texture = XGL_CreateTextureFromSurface(renderer, tmp);
	SDL_FreeSurface(tmp);
	return texture;
}

// --------------------------------------------

// yeesh
#define ATOMIC_GET(what) __atomic_load_n(&(what), __ATOMIC_ACQUIRE)
#define ATOMIC_GET_RELAXED(what) __atomic_load_n(&(what), __ATOMIC_RELAXED)
#define ATOMIC_SET(what, value) __atomic_store_n(&(what), (value), __ATOMIC_RELEASE)

// --------------------------------------------

#define CEIL_TO(x, n) (((x) + (n) - 1) / (n) * (n))
#define NUMBER_OF(items) (sizeof(items) / sizeof((items)[0]))
#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

#define MAX_LINE 1024
#define MAX_PATH 512
#define MAX_FILE 256

static struct {
	char root[MAX_PATH];
	char games[MAX_PATH];
	char bios[MAX_PATH];
	char archive[MAX_PATH];
	char system[MAX_PATH];
	char assets[MAX_PATH];
	char userdata[MAX_PATH];
	char settings[MAX_PATH];
} paths;

#define Path_digit(n) ((n) ? "1" : "0")
#define Path_concat(path, ...) Path_concat_implementation(path, __VA_ARGS__, NULL)
static char *Path_concat_implementation(char *path, ...) {
	va_list ap;
	const char *s;

	path[0] = '\0';

	va_start(ap, path);
	while ((s = va_arg(ap, const char *))) {
		strcat(path, s);
	}
	va_end(ap);

	return path;
}
static void Paths_init(void) {
	Path_concat(paths.root, getenv("SDCARD_PATH"));
	Path_concat(paths.games, paths.root, "/games");
	Path_concat(paths.bios, paths.root, "/bios");
	Path_concat(paths.archive, paths.games, "/archive");
	Path_concat(paths.system, paths.root, "/system");
	Path_concat(paths.assets, paths.system, "/assets");
	Path_concat(paths.userdata, paths.root, "/userdata");
	Path_concat(paths.settings, paths.userdata, "/settings.bin");
}

#define BAT_PATH 		"/sys/class/power_supply/cw2015-battery/"
#define USB_PATH 		"/sys/class/power_supply/usb/"
#define CPU_PATH		"/sys/devices/system/cpu/cpu0/cpufreq/"
#define LID_PATH		"/sys/class/anbernic_misc/hallkey" // 3:open,2:closed

#define FREQ_MENU		"408000"
#define FREQ_GAME		"1800000"

enum {
	AUTO_STATE = 0,
	USER_STATE,
};

// #define SCREEN_WIDTH  640
// #define SCREEN_HEIGHT 480
#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768
#define SCREEN_COUNT 2 // duh

#define DS_WIDTH 256
#define DS_HEIGHT 192

// --------------------------------------------

#define BLACK_TRIAD 	0x00,0x00,0x00
#define WHITE_TRIAD 	0xff,0xff,0xff
#define LIGHT_TRIAD 	0x99,0x99,0x99
#define DARK_TRIAD 		0x33,0x33,0x33
#define MID_TRIAD 		0x44,0x44,0x44
#define RED_TRIAD		0xff,0x33,0x33
#define GREEN_TRIAD		0x33,0xcc,0x33
#define YELLOW_TRIAD	0xff,0xcc,0x00

#define TRIAD_ALPHA(t,a) (SDL_Color){t,a}

#define BLACK_COLOR		TRIAD_ALPHA(BLACK_TRIAD,0xff)
#define WHITE_COLOR		TRIAD_ALPHA(WHITE_TRIAD,0xff)
#define LIGHT_COLOR		TRIAD_ALPHA(LIGHT_TRIAD,0xff)
#define DARK_COLOR		TRIAD_ALPHA(DARK_TRIAD,0xff)
#define RED_COLOR		TRIAD_ALPHA(RED_TRIAD,0xff)
#define GREEN_COLOR		TRIAD_ALPHA(GREEN_TRIAD,0xff)
#define YELLOW_COLOR	TRIAD_ALPHA(YELLOW_TRIAD,0xff)

// --------------------------------------------

#define HAT_NEUTRAL	0
#define HAT_UP 		(1 << 0)
#define HAT_RIGHT	(1 << 1)
#define HAT_DOWN	(1 << 2)
#define HAT_LEFT	(1 << 3)

#define JOY_X		3
#define JOY_B		1
#define JOY_Y		2
#define JOY_A		0

#define JOY_START	7
#define JOY_SELECT	6
#define JOY_MENU	8 // and 13!?

#define JOY_L1		4
#define JOY_R1		5
#define JOY_L2		10
#define JOY_R2		11
#define JOY_L3		9
#define JOY_R3		12

#define JOY_PLUS	16
#define JOY_MINUS	15

#define AXIS_LX 	0
#define AXIS_LY 	1
#define AXIS_RX 	2
#define AXIS_RY 	3

#define SCAN_CLOSE	73
#define SCAN_OPEN	76

#define SCAN_BACK	14
#define SCAN_POWER	102

#define KEY_LID		'l'


// --------------------------------------------

typedef struct {
	XGL_Texture *texture;
	int claimed;
} DummyTexture;

typedef struct {
	SDL_Window *window;
} DummyRenderer;

// --------------------------------------------

typedef enum {
	OSD_NONE,
	OSD_VOLUME,
	OSD_BRIGHTNESS,
} OSDMode;

typedef struct {
	char name[MAX_FILE];
	int hidden;
} Entry;

static struct {
	uintptr_t base;
	
	struct {
		SDL_Window *windows[SCREEN_COUNT];
		SDL_GLContext contexts[SCREEN_COUNT];
		XGL_Renderer *renderers[SCREEN_COUNT];
		XGL_Texture *screens[SCREEN_COUNT];
		XGL_Texture *previews[SCREEN_COUNT];
		XGL_Texture *overlay;
		XGL_Texture *icons;
		XGL_Texture *meters;
		XGL_Texture *notches;
		XGL_Texture *grid;
	} xgl;
	
	SDL_Rect rects[SCREEN_COUNT];
	
	struct {
		DummyRenderer renderers[SCREEN_COUNT];
		DummyTexture screens[SCREEN_COUNT];
		DummyTexture ignored;
	} dummy;
	
	int count;
	int current;
	int capacity;
	Entry* items;
	
	char game_path[MAX_PATH];
	char game_name[MAX_FILE];
	
	// TODO: why aren't these part of raw?
	int bat;
	int usb;
	int lid;
	
	int synced;
	int menu;
	int osd;
	int capture;
	
	uint64_t menu_us; // elapsed time in menu
} app;

static struct {
	int current;
	int snap;
	int preview_current;
	int preview_snap;
	int battery;
	int charging;
	Uint32 status_at;
} ui;

static int osd_at;

static int raw_x,raw_y; // touch screen fix

// --------------------------------------------
// support
// --------------------------------------------

static void dmesg(const char *msg) {
	char buffer[MAX_LINE];
	snprintf(buffer, sizeof(buffer), "<6>dedicated: %s\n", msg);
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd<0) return;
	write(fd, buffer, strlen(buffer));
	close(fd);
}

static inline void touch(const char *path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (fd >= 0) close(fd);
}
static inline int getInt(int f) {
	if (f<0) return 0;

	char b[32];
	int n = pread(f, b, sizeof(b) - 1, 0);
	if (n<=0) return 0;

	b[n] = '\0';
	return atoi(b);
}
static inline void setInt(int f, int value) {
    char buffer[16];
    int len = sprintf(buffer, "%d\n", value);
	lseek(f, 0, SEEK_SET);
    write(f, buffer, len);
}
static inline void getString(void) {
	
}
static inline void putString(const char* path, const char* value) {
	int f = open(path, O_WRONLY);
	if (f<0) return;
	write(f, value, strlen(value));
	close(f);
}
static inline void putInt(const char* path, int value) {
	char buffer[16];
	sprintf(buffer, "%d", value);
	putString(path, buffer);
}
static inline int exists(const char* path) {
	return access(path, F_OK)==0;
}

// --------------------------------------------
// raw HMI controls
// --------------------------------------------

static struct {
	int bri0;
	int bri1;
	int led0;
	int led1;
	int jck;

	int drm;
	struct {
		uint32_t connector;
		uint32_t dpms;
	} dsi[SCREEN_COUNT];
	int dsi_count;
	
	int jack;
	struct {
		snd_mixer_t *mix;
		snd_mixer_elem_t *dac;
	} hp;
	struct {
		snd_mixer_t *mix;
		snd_mixer_elem_t *dev0;
		snd_mixer_elem_t *dev1;
		snd_mixer_elem_t *swi0;
		snd_mixer_elem_t *swi1;
	} spk;
} raw;

static void raw_mixer_init(snd_mixer_t **mix, const char *card, const char *const *names, snd_mixer_elem_t **elems, int count) {
	// I hate audio code
	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	for (int waiting=0;; waiting=1) {
		int err, i=0;
		if ((err=snd_mixer_open(mix, 0))>=0 &&
			(err=snd_mixer_attach(*mix, card))>=0 &&
			(err=snd_mixer_selem_register(*mix, NULL, NULL))>=0 &&
			(err=snd_mixer_load(*mix))>=0) {
			for (; i<count; i++) {
				snd_mixer_selem_id_set_name(sid, names[i]);
				if (!(elems[i]=snd_mixer_find_selem(*mix, sid))) break;
			}
			if (i==count) {
				if (waiting) {
					char msg[MAX_LINE];
					sprintf(msg, "mixer %s ready", card);
					dmesg(msg);
				}
				return;
			}
		}
		if (!waiting) {
			char msg[MAX_LINE];
			sprintf(msg, "waiting for mixer %s: %s", card, err<0 ? snd_strerror(err) : names[i]);
			dmesg(msg);
		}
		if (*mix) snd_mixer_close(*mix);
		*mix = NULL;
		usleep(20000);
	}
}
static void raw_poll(void);
static void raw_audio_init(void) {
	if (raw.spk.swi1) return;

	snd_mixer_elem_t *hp[1], *spk[4];
	const char *hp_names[] = {
		"DAC"
	};
	const char *spk_names[] = {
		"aw_dev_0_rx_volume",
		"aw_dev_1_rx_volume",
		"aw_dev_0_switch",
		"aw_dev_1_switch"
	};
	raw_mixer_init(&raw.hp.mix, "hw:0", hp_names, hp, 1);
	raw.hp.dac = hp[0];
	raw_mixer_init(&raw.spk.mix, "hw:1", spk_names, spk, 4);
	raw.spk.dev0 = spk[0];
	raw.spk.dev1 = spk[1];
	raw.spk.swi0 = spk[2];
	raw.spk.swi1 = spk[3];
	
	raw.jack = -1;
	raw_poll();
}

static void raw_init(void) {
	raw.bri0 = open("/sys/class/backlight/backlight1/brightness", O_WRONLY | O_CLOEXEC);
	raw.bri1 = open("/sys/class/backlight/backlight/brightness", O_WRONLY | O_CLOEXEC);
	raw.led0 = open("/sys/class/anbernic_misc/lowpwr_led", O_WRONLY | O_CLOEXEC);
	raw.led1 = open("/sys/class/anbernic_misc/work_led", O_WRONLY | O_CLOEXEC);
	raw.jck = open("/sys/class/anbernic_misc/spk_state", O_RDONLY | O_CLOEXEC);
	
	// I hate drm code
	raw.drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	raw.dsi_count = 0;
	if (raw.drm >= 0) {
		drmModeRes *res = drmModeGetResources(raw.drm);
		if (res) {
			for (int i=0; i<res->count_connectors && raw.dsi_count<SCREEN_COUNT; i++) {
				drmModeConnector *conn = drmModeGetConnector(raw.drm, res->connectors[i]);
				if (!conn) continue;

				if (conn->connection==DRM_MODE_CONNECTED &&
					conn->connector_type==DRM_MODE_CONNECTOR_DSI) {
					drmModeObjectProperties *props = drmModeObjectGetProperties(
						raw.drm,
						conn->connector_id,
						DRM_MODE_OBJECT_CONNECTOR
					);

					if (props) {
						for (uint32_t j=0; j<props->count_props; j++) {
							drmModePropertyRes *prop = drmModeGetProperty(raw.drm, props->props[j]);
							if (!prop) continue;

							if (strcmp(prop->name, "DPMS")==0) {
								int index = raw.dsi_count++;
								raw.dsi[index].connector = conn->connector_id;
								raw.dsi[index].dpms = prop->prop_id;
								drmModeFreeProperty(prop);
								break;
							}

							drmModeFreeProperty(prop);
						}

						drmModeFreeObjectProperties(props);
					}
				}

				drmModeFreeConnector(conn);
			}

			drmModeFreeResources(res);
		}
	}
	
}
static void raw_quit(void) {
	close(raw.bri0);
	close(raw.bri1);
	close(raw.led0);
	close(raw.led1);
	close(raw.drm);
	close(raw.jck);
	if (raw.hp.mix) snd_mixer_close(raw.hp.mix);
	if (raw.spk.mix) snd_mixer_close(raw.spk.mix);
}

static void raw_spk(int enable) {
    if (!raw.spk.swi1) return;
	
    snd_mixer_selem_set_enum_item(raw.spk.swi0, SND_MIXER_SCHN_MONO, enable ? 1 : 0);
    snd_mixer_selem_set_enum_item(raw.spk.swi1, SND_MIXER_SCHN_MONO, enable ? 1 : 0);
}
static void raw_poll(void) {
    if (!raw.spk.swi1) return;
	
    static uint32_t next = 0;

    uint32_t now = SDL_GetTicks();
    if (now < next) return;
    next = now + 250;

    int jack = getInt(raw.jck) == 1;
    if (jack == raw.jack) return;

    raw.jack = jack;
    raw_spk(!jack);
}

static void raw_vol(int value) { // 0-20, not really raw anymore
	if (!raw.spk.swi1) return;
	
	static const uint8_t hp_raw[21] = {
		  0, // mute
		128, // 0
	  	135, // 7
	  	141, // 6
	  	148, // 7
	  	154, // 6
	  	161, // 7
	  	167, // 6
	  	174, // 7
	  	180, // 6
	  	187, // 7
	  	193, // 6
	  	200, // 7
	  	206, // 6
	  	213, // 7
	  	219, // 6
	  	226, // 7
	  	232, // 6
	  	239, // 7
	  	245, // 6
	  	252, // 7
	};
	
	static const uint16_t spk_raw[21] = {
	    1023, // mute
	     512, // 512
		 480, // 32
		 448, // 32
		 424, // 24
		 392, // 32
	     360, // 32
		 336, // 24
		 304, // 32
		 280, // 24
		 256, // 24
	     224, // 24
		 200, // 24
		 176, // 24
		 152, // 24
		 128, // 24
	     104, // 24
		  80, // 24 
		  56, // 24
		  32, // 24
		   0, // 32
	};
	
	snd_mixer_selem_set_playback_volume_all(raw.hp.dac, hp_raw[value]);
	snd_mixer_selem_set_playback_volume_all(raw.spk.dev0, spk_raw[value]);
	snd_mixer_selem_set_playback_volume_all(raw.spk.dev1, spk_raw[value]);
}
static void raw_bri(int value) {
	setInt(raw.bri0, value);
	setInt(raw.bri1, value);
}
static void raw_led(int enable) {
	setInt(raw.led0, 0);
	setInt(raw.led1, enable);
	// #define SUPPRESS_PATH "/tmp/suppress-leds"
	// if (enable) unlink(SUPPRESS_PATH);
	// else touch(SUPPRESS_PATH);
	// #undef SUPPRESS_PATH
}

static void raw_scr(int enable) {
	uint64_t dpms = enable ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
	for (int i=0; i<raw.dsi_count; i++) {
		drmModeObjectSetProperty(
			raw.drm,
			raw.dsi[i].connector,
			DRM_MODE_OBJECT_CONNECTOR,
			raw.dsi[i].dpms,
			dpms
		);
	}
}

static void raw_ip(char *out) {
	out[0] = '\0';

	unsetenv("LD_PRELOAD");
	FILE *fp = popen("ip -4 route get 1.1.1.1 | awk '{for (i=1;i<=NF;i++) if ($i==\"src\") print $(i+1)}'", "r");
	if (!fp) return;
	if (!fgets(out, 64, fp)) out[0] = '\0';
	
	pclose(fp);
	if (!out[0]) return;
	
	char *nl = strchr(out, '\n');
	if (nl) *nl = '\0';
}

// --------------------------------------------
// settings
// --------------------------------------------

static struct {
	int version;
	int volume;		// 0-20
	int brightness;	// 0 - 10
	int grid;
	int unused[4];
	char game[MAX_PATH];
} settings = {
	.version = 1,
	.volume = 10,
	.brightness = 5,
	.grid = 1,
};

static void Settings_load(void) {
	if (access(paths.settings, F_OK)!=0) return;
	
	FILE* file = fopen(paths.settings, "rb");
	if (!file) return;
	fread(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_save(void) {
	FILE* file = fopen(paths.settings, "wb");
	if (!file) return;
	fwrite(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_setVolume(int value) {
	raw_vol(value);
	settings.volume = value;
}
static void Settings_setBrightness(int value) {
	static const uint8_t raw[11] = {
		  1, // 0
		  3, // 
		  5, // 
		  8, // 
		 16, // 
		 40, // 
		 64, // 
		 80, // 
		128, // 
		192, // 
		255, // 
	};
	
	raw_bri(raw[value]);
	settings.brightness = value;
}

// --------------------------------------------
// real function handles
// --------------------------------------------

static int (*real_SDL_Init)(Uint32) = NULL;
static SDL_Window* (*real_SDL_CreateWindow)(const char*, int, int, int, int, Uint32) = NULL;
static void (*real_SDL_SetWindowSize)(SDL_Window *, int, int) = NULL;
static int (*real_SDL_OpenAudio)(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) = NULL;
static int (*real_SDL_PollEvent)(SDL_Event*) = NULL;
static void (*real_SDL_Delay)(uint32_t) = NULL;

static int (*real__libc_start_main)(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) = NULL;
static int (*real__sprintf_chk)(char *s, int flag, size_t slen, const char *fmt, ...) = NULL;
static int (*real__snprintf_chk)(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) = NULL;
static void (*real_exit)(int) __attribute__((noreturn)) = NULL;
static void (*real__exit)(int) __attribute__((noreturn)) = NULL;
static int (*real_system)(const char *) = NULL;
static int (*real_puts)(const char *__s) = NULL;
static int (*real__printf_chk)(int flag, const char *fmt, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static int (*real_gettimeofday)(struct timeval *tv, void *tz) = NULL;

// --------------------------------------------
// logging
// --------------------------------------------

static inline void LOG_event(SDL_Event* event) {
	switch (event->type) {
		case SDL_QUIT:
			printf("SDL_PollEvent: QUIT\n"); 
			break;
		
		case SDL_KEYDOWN:
			printf("SDL_PollEvent: KEYDOWN (scancode=%d sym=%d)\n", event->key.keysym.scancode, event->key.keysym.sym);
			break;
		case SDL_KEYUP:
			printf("SDL_PollEvent: KEYUP (scancode=%d sym=%d)\n", event->key.keysym.scancode, event->key.keysym.sym);
			break;
		
		case SDL_MOUSEBUTTONDOWN:
			printf("SDL_PollEvent: MOUSEBUTTONDOWN (button=%d)\n", event->button.button);
			break;
		case SDL_MOUSEBUTTONUP:
			printf("SDL_PollEvent: MOUSEBUTTONUP (button=%d)\n", event->button.button);
			break;
		case SDL_MOUSEMOTION:
			printf("SDL_PollEvent: MOUSEMOTION (x=%d y=%d)\n", event->motion.x, event->motion.y);
			break;
			
		case SDL_FINGERDOWN:
			printf("SDL_PollEvent: FINGERDOWN (fingerId=%lld touchId=%lld x=%f y=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.pressure);
			break;
		case SDL_FINGERUP:
			printf("SDL_PollEvent: FINGERUP (fingerId=%lld touchId=%lld x=%f y=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.pressure);
			break;
		case SDL_FINGERMOTION:
			printf("SDL_PollEvent: FINGERMOTION (fingerId=%lld touchId=%lld x=%f y=%f dx=%f dy=%f pressure=%f)\n", (long long)event->tfinger.fingerId, (long long)event->tfinger.touchId, event->tfinger.x, event->tfinger.y, event->tfinger.dx, event->tfinger.dy, event->tfinger.pressure);
			break;

		case SDL_JOYAXISMOTION:
			// printf("SDL_PollEvent: JOYAXISMOTION which=%d axis=%d value=%d\n", event->jaxis.which, event->jaxis.axis, event->jaxis.value);
			break;
		case SDL_JOYBUTTONDOWN:
			printf("SDL_PollEvent: JOYBUTTONDOWN which=%d button=%d\n", event->jbutton.which, event->jbutton.button);
			break;
		case SDL_JOYBUTTONUP:
			printf("SDL_PollEvent: JOYBUTTONUP which=%d button=%d\n", event->jbutton.which, event->jbutton.button);
			break;
		case SDL_JOYHATMOTION:
			printf("SDL_PollEvent: JOYHATMOTION which=%d hat=%d value=%d\n", event->jhat.which, event->jhat.hat, event->jhat.value);
			break;

		case SDL_JOYDEVICEADDED:
			printf("SDL_PollEvent: JOYDEVICEADDED which=%d\n", event->jdevice.which);
			break;
		case SDL_JOYDEVICEREMOVED:
			printf("SDL_PollEvent: JOYDEVICEREMOVED which=%d\n", event->jdevice.which);
			break;

		case SDL_CONTROLLERAXISMOTION:
			printf("SDL_PollEvent: CONTROLLERAXISMOTION which=%d axis=%d value=%d\n", event->caxis.which, event->caxis.axis, event->caxis.value);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			printf("SDL_PollEvent: CONTROLLERBUTTONDOWN which=%d button=%d\n", event->cbutton.which, event->cbutton.button);
			break;
		case SDL_CONTROLLERBUTTONUP:
			printf("SDL_PollEvent: CONTROLLERBUTTONUP which=%d button=%d\n", event->cbutton.which, event->cbutton.button);
			break;

		case SDL_CONTROLLERDEVICEADDED:
			printf("SDL_PollEvent: CONTROLLERDEVICEADDED which=%d\n", event->cdevice.which);
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			printf("SDL_PollEvent: CONTROLLERDEVICEREMOVED which=%d\n", event->cdevice.which);
			break;
		
		default:
			printf("SDL_PollEvent: type=%d\n", event->type);
			break;
	}
	fflush(stdout);
}
static inline void LOG_rect(const char *label, const SDL_Rect *r) {
	if (r) printf("%s[x=%d y=%d w=%d h=%d] ", label, r->x, r->y, r->w, r->h);
	else   printf("%sNULL ", label);
}
static inline void LOG_render(SDL_Renderer *renderer, SDL_Texture  *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
	float sx=1.0f, sy=1.0f;
	int lw=0, lh=0, outw=0, outh=0;
	SDL_Rect vp; vp.x=vp.y=vp.w=vp.h=0;
	SDL_bool integer = SDL_FALSE;

	SDL_RenderGetScale(renderer, &sx, &sy);				  // 1,1 unless set
	SDL_RenderGetLogicalSize(renderer, &lw, &lh);			// 0,0 if none
	integer = SDL_RenderGetIntegerScale(renderer);		   // SDL_TRUE if forced integer scale
	SDL_RenderGetViewport(renderer, &vp);					// defaults to full output
	SDL_GetRendererOutputSize(renderer, &outw, &outh);	   // e.g. 1024x768

	printf("SDL_RenderCopy: renderer=%p texture=%p ", (void*)renderer, (void*)texture);
	LOG_rect("src=", srcrect);
	LOG_rect("dst=", dstrect);
	printf(" | scale=%.3fx%.3f logical=%dx%d integer=%d viewport=[%d,%d %dx%d] output=%dx%d\n", sx, sy, lw, lh, integer ? 1 : 0, vp.x, vp.y, vp.w, vp.h, outw, outh);
	fflush(stdout);
}
static inline void LOG_texture(SDL_Texture* texture, const SDL_Rect* rect, int pitch) {
	Uint32 fmt = 0; int access = 0, tw = 0, th = 0;
	SDL_QueryTexture(texture, &fmt, &access, &tw, &th);

	int rw = rect ? rect->w : tw;
	int rh = rect ? rect->h : th;
	int bpp = SDL_BYTESPERPIXEL(fmt);
	long long approx = (long long)rh * pitch;
	
	printf("Texture %p ", (void*)texture);
	LOG_rect("rect=", rect);
	printf("pitch=%d format=%s tex=%dx%d bytes=%lld\n", pitch, SDL_GetPixelFormatName(fmt), tw, th, approx);
	fflush(stdout);
}
static const char* get_access_name(int access) {
	switch (access) {
		case SDL_TEXTUREACCESS_STATIC:   return "STATIC";
		case SDL_TEXTUREACCESS_STREAMING:return "STREAMING";
		case SDL_TEXTUREACCESS_TARGET:   return "TARGET";
		default:						 return "UNKNOWN";
	}
}
static void hexdump(const void *ptr, size_t len) {
	const unsigned char *p = (const unsigned char *)ptr;
	for (size_t i = 0; i<len; i += 16) {
		printf("%08zx  ", i);
		for (size_t j = 0; j<16; ++j) {
			if (i + j<len) printf("%02X ", p[i + j]);
			else			 printf("   ");
		}
		printf(" ");
		for (size_t j = 0; j<16 && i + j<len; ++j) {
			unsigned char c = p[i + j];
			printf("%c", (c>=32 && c<127) ? c : '.');
		}
		printf("\n");
	}
	fflush(stdout);
}

// --------------------------------------------
// audio
// --------------------------------------------

static struct {
	void (*callback)(void *, Uint8 *, int);
	void *userdata;
	int mute;
} audio;

static void Audio_callback(void *userdata, Uint8 *stream, int len) {
	(void)userdata;

	if (app.menu) {
		SDL_memset(stream, 0, len);
		return;
	}

	audio.callback(audio.userdata, stream, len);
	if (audio.mute) SDL_memset(stream, 0, len);
}

// --------------------------------------------
// microphone support
// --------------------------------------------

#define MIC_SAMPLE_RATE		32768u
#define MIC_SAMPLE_COUNT	(1u << 15) // 1 second
#define MIC_SAMPLE_MASK		(MIC_SAMPLE_COUNT - 1)
#define MIC_SAMPLE_SIZE		256
#define MIC_SAMPLE_DRIFT	2048
#define DRASTIC_CLOCK_HZ	67027964ull // from drastic's spu_get_microphone_sample()

static struct {
	snd_pcm_t *pcm;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	int running;
	int active;

	int16_t ring[MIC_SAMPLE_COUNT];
	uint64_t write_sample;

	double dc;
	double gain;

	struct {
		int valid;
		uint64_t timestamp;
		uint64_t start_sample;
	} session;
} mic = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
	.gain = 2.0,
};

static void Mic_recordSample(int16_t sample) {
	uint64_t start_sample = ATOMIC_GET_RELAXED(mic.write_sample);
	mic.ring[start_sample & MIC_SAMPLE_MASK] = sample;
	ATOMIC_SET(mic.write_sample, start_sample + 1);
}
static int32_t Mic_getSample(uint64_t timestamp) {
	if (!ATOMIC_GET(mic.active)) return 0;
	
	uint64_t write_sample = ATOMIC_GET(mic.write_sample);
	if (write_sample == 0) return 0;

	if (!mic.session.valid) {
		mic.session.valid = 1;
		mic.session.timestamp = timestamp;
		mic.session.start_sample = write_sample > MIC_SAMPLE_SIZE ? write_sample - MIC_SAMPLE_SIZE : 0;
	}

	uint64_t elapsed = timestamp > mic.session.timestamp ? timestamp - mic.session.timestamp : 0;
	uint64_t offset = elapsed * MIC_SAMPLE_RATE / DRASTIC_CLOCK_HZ;
	uint64_t start_sample = mic.session.start_sample + offset;
	uint64_t target_sample = write_sample > MIC_SAMPLE_SIZE ? write_sample - MIC_SAMPLE_SIZE : 0;
	
	// reanchor after drift
	if (start_sample >= write_sample) {
		mic.session.timestamp = timestamp;
		mic.session.start_sample = target_sample;
		start_sample = target_sample;
	}
	else if (write_sample - start_sample > MIC_SAMPLE_DRIFT) {
		mic.session.timestamp = timestamp;
		mic.session.start_sample = target_sample;
		start_sample = target_sample;
	}

	return mic.ring[start_sample & MIC_SAMPLE_MASK];
}
static void* Mic_thread(void* arg) {
	int16_t buf[64];
	int running = 1;
	while (running) {
		pthread_mutex_lock(&mic.lock);
		while (mic.running && !mic.active) {
			if (mic.pcm) {
				snd_pcm_drop(mic.pcm);
				snd_pcm_prepare(mic.pcm);
			}
			pthread_cond_wait(&mic.cond, &mic.lock);
		}
		running = mic.running;
		pthread_mutex_unlock(&mic.lock);

		if (!running) break;

		snd_pcm_sframes_t n = snd_pcm_readi(mic.pcm, buf, NUMBER_OF(buf));
		
		if (n == -EAGAIN) {
			usleep(1000);
			continue;
		}
		
		if (n < 0) {
			int err = snd_pcm_recover(mic.pcm, (int)n, 1);
			if (err < 0) usleep(5000);
			continue;
		}
		
		if (n == 0) {
			usleep(1000);
			continue;
		}

		for (snd_pcm_sframes_t i=0; i<n; i++) {
			double v = buf[i];
			mic.dc += (v - mic.dc) * 0.005;

			double y = (v - mic.dc) * mic.gain;
			if (y > 32767.0) y = 32767.0;
			if (y < -32768.0) y = -32768.0;

			Mic_recordSample((int16_t)y);
		}
	}
	return NULL;
}

static void Mic_init(void) {
	if (mic.running) return;
	
	snd_pcm_open(&mic.pcm, "plughw:0,0", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	snd_pcm_set_params(mic.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, MIC_SAMPLE_RATE, 1, 10000);
	
	pthread_mutex_lock(&mic.lock);
	ATOMIC_SET(mic.active, 1);
	mic.running = 1;
	pthread_create(&mic.thread, NULL, Mic_thread, NULL);
	pthread_mutex_unlock(&mic.lock);
}
static void Mic_quit(void) {
	pthread_mutex_lock(&mic.lock);
	if (!mic.running) {
		pthread_mutex_unlock(&mic.lock);
		return;
	}

	ATOMIC_SET(mic.active, 0);
	mic.running = 0;
	pthread_cond_signal(&mic.cond);
	pthread_mutex_unlock(&mic.lock);
	pthread_join(mic.thread, NULL);
	snd_pcm_close(mic.pcm);
	mic.pcm = NULL;
	ATOMIC_SET(mic.write_sample, 0);
	mic.session.valid = 0;
	mic.dc = 0;
}

static void Mic_pause(int pause) {
	pthread_mutex_lock(&mic.lock);
	if (mic.running) {
		ATOMIC_SET(mic.active, pause ? 0 : 1);
		mic.session.valid = 0;

		if (pause) {
			mic.dc = 0;
		}
		else {
			mic.dc = 0;
			ATOMIC_SET(mic.write_sample, 0);
		}

		pthread_cond_signal(&mic.cond);
	}
	pthread_mutex_unlock(&mic.lock);
}

// --------------------------------------------
// loading
// --------------------------------------------

enum {
	LOADER_IDLE = 0,
	LOADER_REQUESTED,
	LOADER_AWAITING,
	LOADER_STARTED,
	LOADER_COMPLETE,
};
enum {
	LOADER_RESET = 0,
	LOADER_RESUME,
};

static struct {
	int state;
	int after;
} loader = {LOADER_AWAITING,LOADER_RESUME};

// --------------------------------------------
// drastic shims
// --------------------------------------------

#define PTR_AT(ptr) (void*)(*(uintptr_t *)(ptr))
#define GET_PFN(ptr) (void*)((ptr))

typedef int (*drastic_main_t)(int,char**,char**);
typedef void (*drastic_quit_t)(void*);
typedef void (*drastic_reset_system_t)(void*);
typedef int32_t (*drastic_load_state_t)(void *, const char *, uint16_t *, uint16_t *, uint32_t);
typedef int32_t (*drastic_save_state_t)(void *, const char *, char *, uint16_t *, uint16_t *);
typedef int32_t (*drastic_load_nds_t)(void *, const char *);
typedef uint8_t (*drastic_audio_pause_t)(void *);
typedef void (*drastic_audio_unpause_t)(void *);
typedef void (*drastic_audio_revert_t)(void *, uint8_t);
typedef void (*drastic_audio_reset_buffer_t)(void *);

static inline void* drastic_var_system(void) {
	return PTR_AT(app.base + 0x15ff30); // follow arg in Cutter
}
static void drastic_resync(void) {
	uint8_t *sys = drastic_var_system();
	*(volatile uint8_t *)(sys + 0x3b2a9a3) = 1;
}
static uint8_t drastic_audio_pause_state;
static void drastic_audio_pause(int flag) {
	void* audio = (uint8_t *)drastic_var_system() + 0x1587000;
	
	if (flag) {
		drastic_audio_pause_t pause = GET_PFN(app.base + 0x0008caa0);
		drastic_audio_pause_state = pause(audio);
	}
	else {
		drastic_audio_revert_t revert = GET_PFN(app.base + 0x000886a0);
		revert(audio, drastic_audio_pause_state);
	}
}
static void drastic_audio_reset_buffer(void) {
	void* sys = drastic_var_system();
	void* audio = (uint8_t *)sys + 0x1587000;
	drastic_audio_reset_buffer_t reset_buffer = GET_PFN(app.base + 0x00088620);
	reset_buffer(audio);
}

static int drastic_fast_forward(void) {
	uint8_t *sys = drastic_var_system();
	return *(volatile uint32_t *)(sys + 0x00085a04) != 0;
}

static inline int drastic_is_saving(void) {
	const volatile uint32_t *busy = (const volatile uint32_t *)(app.base + 0x3ec27c);
	return __atomic_load_n(busy, __ATOMIC_ACQUIRE)!=0;
}
static void drastic_await_save(void) {
	while (drastic_is_saving()) real_SDL_Delay(1);
	sync();
}

static void drastic_load_nds_and_jump(const char* path) {
	drastic_await_save();

	Mic_quit();
	
	drastic_load_nds_t d_load_nds = GET_PFN(app.base + 0x0006fd30); // nm ./drastic | grep load_nds
	drastic_reset_system_t d_reset_system = GET_PFN(app.base + 0x0000fd50); // nm ./drastic | grep reset_system

	void* sys = drastic_var_system();
	d_load_nds((uint8_t *)sys + 800, path);
	d_reset_system(sys);
	
	jmp_buf *env = (jmp_buf *)((uint8_t *)sys + 0x3b2a840); // from main in Cutter
	longjmp(*env, 1); // no return
}
static void drastic_load_state(const char *name) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_load_state_t d_load_state = GET_PFN(app.base + 0x000746f0); // nm ./drastic | grep load_state
	
	char path[MAX_PATH];
	Path_concat(path, paths.userdata, "/states/", name);
	d_load_state(sys, path, NULL,NULL,0);
}
static void drastic_save_state(const char *name) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_save_state_t d_save_state = GET_PFN(app.base + 0x00074da0); // nm ./drastic | grep save_state
		
	char path[MAX_PATH];
	Path_concat(path, paths.userdata, "/states/");
	d_save_state(sys, path, (char *)name, NULL,NULL);
}
static void drastic_quit(void) {
	drastic_await_save();
	drastic_quit_t d_quit = GET_PFN(app.base + 0x0000e8d0); // nm ./drastic | grep quit
	void* sys = drastic_var_system();	
	d_quit(sys);
}

static int32_t drastic_spu_get_microphone_sample(void *spu, uint64_t timestamp) {
	if (audio.mute) return 0;
	Mic_init();
	return Mic_getSample(timestamp);
}

// --------------------------------------------

typedef struct {
	int valid;
	unsigned long long value;
	const char *end;
} NumberString;

static NumberString getNumberString(const char *s, const char *start) {
	NumberString n = {0, 0, s};

	// Arabic numeral
	if (isdigit((unsigned char)*s)) {
		n.valid = 1;

		do {
			n.value = n.value * 10 + (*s - '0');
			s++;
		} while (isdigit((unsigned char)*s));

		n.end = s;
		return n;
	}

	// Roman numerals must begin a space-separated token
	if (s!=start && s[-1]!=' ') return n;

	const char *p = s;
	int prev = 0;

	while (*p && *p!=' ' && *p!='.') {
		int v;

		switch (toupper((unsigned char)*p)) {
			case 'I': v = 1;  break;
			case 'V': v = 5;  break;
			case 'X': v = 10; break;
			default: return n;
		}

		if (v>prev) n.value += v - 2*prev;
		else n.value += v;

		prev = v;
		p++;
	}

	if (p==s) return n;

	n.valid = 1;
	n.end = p;
	return n;
}

static int compareNatural(const char *a, const char *b) {
	const char *startA = a;
	const char *startB = b;

	while (*a && *b) {
		NumberString na = getNumberString(a, startA);
		NumberString nb = getNumberString(b, startB);

		if (na.valid || nb.valid) {
			if (na.valid && nb.valid) {
				if (na.value!=nb.value)
					return na.value<nb.value ? -1 : 1;

				a = na.end;
				b = nb.end;
				continue;
			}

			// NumberString sort where digits normally would,
			// after punctuation/space, before letters
			if (na.valid)
				return '0'<tolower((unsigned char)*b) ? -1 : 1;
			else
				return tolower((unsigned char)*a)<'0' ? -1 : 1;
		}

		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);

		if (ca!=cb) {
			// ensure "Game.ext" sorts before "Game 2.ext"
			if (ca=='.' && cb==' ') return -1;
			if (ca==' ' && cb=='.') return  1;
			return ca - cb;
		}

		a++;
		b++;
	}

	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

// --------------------------------------------
// custom fonts
// --------------------------------------------

typedef struct {
	uint8_t map[128];
	uint8_t char_widths[128];
	int8_t kern_pairs[128][128];

	SDL_Surface* bitmap;
	const char* name;
	const char* charset;
	int8_t offset_x;
	int8_t offset_y;
	int8_t tracking;
	uint8_t tile_width;
	uint8_t tile_height;
	uint8_t tiles_wide;
	uint8_t tiles_high;
	uint8_t missing;

	uint8_t char_width;
	uint8_t low_width;
} Font;

Font* font36 = &(Font){
	.name = "font-dedicated-36.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 36,
	.tile_height = 36,
	.offset_x = -3,
	.tracking = 6,
	.char_width = 30,
	.low_width = 24,
	.char_widths = {
		['I'] = 6,
		['1'] = 18,
		['.'] = 6,
		[','] = 6,
		['-'] = 24,
		['+'] = 24,
		['\''] = 6,
		['!'] = 6,
		[':'] = 6,
		['&'] = 36,
		[' '] = 12,
	},
	.kern_pairs = {
		['A']['T'] = -6,
		['T']['A'] = -6,
		['A']['V'] = -6,
		['V']['A'] = -6,
		['A']['W'] = -3,
		['W']['A'] = -3,
		['A']['Y'] = -3,
		['Y']['A'] = -3,
		['T']['-'] = -6,
		['-']['T'] = -6,
		['L']['Y'] = -6,
		['L']['T'] = -6,
	},
};

Font* font24 = &(Font){
	.name = "font-dedicated-24.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 24,
	.tile_height = 24,
	.offset_x = -2,
	.tracking = 4,
	.char_width = 20,
	.char_widths = {
		['I'] = 4,
		['1'] = 12,
		['.'] = 4,
		[','] = 4,
		['-'] = 16,
		['\''] = 4,
		['!'] = 4,
		[':'] = 4,
		[' '] = 8,
		['%'] = 21,
	},
	.kern_pairs = {
		['A']['T'] = -4,
		['T']['A'] = -4,
		['A']['V'] = -4,
		['V']['A'] = -4,
		['A']['W'] = -2,
		['W']['A'] = -2,
		['A']['Y'] = -2,
		['Y']['A'] = -2,
		['T']['-'] = -4,
		['-']['T'] = -4,
		['L']['Y'] = -4,
		['L']['T'] = -4,
	},
};

Font* font18 = &(Font){
	.name = "font-dedicated-18.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 18,
	.tile_height = 18,
	.offset_x = -1,
	.tracking = 3,
	.char_width = 15,
	.char_widths = {
		['I'] = 3,
		['1'] = 9,
		['.'] = 3,
		[','] = 3,
		['-'] = 12,
		['\''] = 3,
		['!'] = 3,
		[':'] = 3,
		[' '] = 6,
	},
	.kern_pairs = {
		['A']['T'] = -3,
		['T']['A'] = -3,
		['A']['V'] = -3,
		['V']['A'] = -3,
		['A']['W'] = -1,
		['W']['A'] = -1,
		['A']['Y'] = -1,
		['Y']['A'] = -1,
		['T']['-'] = -3,
		['-']['T'] = -3,
		['L']['Y'] = -3,
		['L']['T'] = -3,
	},
};

Font* font16 = &(Font){
	.name = "font-dedicated-16.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%,: ",
	.tile_width = 16,
	.tile_height = 16,
	.offset_x = -1,
	.tracking = 4,
	.char_width = 12,
	.char_widths = {
		['I'] = 2,
		['1'] = 8,
		['.'] = 2,
		[','] = 2,
		['-'] = 10,
		['\''] = 2,
		['!'] = 2,
		[':'] = 2,
		[' '] = 4,
	},
	.kern_pairs = {
		['A']['T'] = -2,
		['T']['A'] = -2,
		['A']['V'] = -2,
		['V']['A'] = -2,
		['A']['W'] = -1,
		['W']['A'] = -1,
		['A']['Y'] = -1,
		['Y']['A'] = -1,
		['T']['-'] = -2,
		['-']['T'] = -2,
		['L']['Y'] = -2,
		['L']['T'] = -2,
	},
};

static void Font_init(Font* font) {
	char path[MAX_PATH];
	Path_concat(path, paths.assets, "/", font->name);
	font->bitmap = IMG_Load(path);
	SDL_SetSurfaceBlendMode(font->bitmap, SDL_BLENDMODE_BLEND);
	
	font->tiles_wide = font->bitmap->w / font->tile_width;
	font->tiles_high = font->bitmap->h / font->tile_height;
	font->missing = (font->tiles_wide * font->tiles_high) - 1;
	
	memset(font->map, font->missing, sizeof(font->map));
	for (uint8_t i=0; font->charset[i]; i++) {
		unsigned char c = font->charset[i];
		font->map[c] = (uint8_t)i;
		if (!font->char_widths[c]) {
			if (font->low_width && c>='a' && c<='z') {
				font->char_widths[c] = font->low_width;
			}
			else {
				font->char_widths[c] = font->char_width;
			}
		}
	}
}
static void Font_quit(Font* font) {
	SDL_FreeSurface(font->bitmap);
}

static void __Font_drawChar(Font* font, SDL_Surface* dst, unsigned char c, int x, int y) {
	int i = c<128 ? font->map[c] : font->missing;
	int tx = i % font->tiles_wide;
	int ty = i / font->tiles_wide;
	int tw = font->tile_width;
	int th = font->tile_height;
	SDL_BlitSurface(font->bitmap, &(SDL_Rect){tx*tw,ty*th,tw,th}, dst, &(SDL_Rect){x,y,tw,th});
}

static void __Font_drawText(Font* font, const char* text, int* out_width, int* out_height, SDL_Surface** out_surface) {
	SDL_Surface* dst = NULL;
	if (out_surface) {
		int w = 0;
		int h = 0;
		__Font_drawText(font, text, &w, &h, NULL);
		dst = SDL_CreateRGBSurfaceWithFormat(0, w,h, 32, SDL_PIXELFORMAT_RGBA32);
		SDL_FillRect(dst, NULL, 0);
	}
	
	const char* tmp = text;
	int ow = 0;
	int oh = font->tile_height;
	while (*tmp) {
		unsigned char c = *tmp++;
		unsigned char n = *tmp;
		if (!font->char_widths[c]) c = toupper(c);
		if (n && !font->char_widths[n]) n = toupper(n);
		
		if (out_surface) __Font_drawChar(font, dst, c, ow,0);

		ow += font->char_widths[c] ? font->char_widths[c] : font->char_width;
		if (n) {
			ow += font->tracking;
			if (font->kern_pairs[c][n]) {
				ow += font->kern_pairs[c][n];
			}
			else {
				ow += font->kern_pairs[0][n];
			}
		}
	}
	ow -= font->offset_x * 2; // reverse pad?
	
	if (out_width) *out_width = ow;
	if (out_height) *out_height = oh;
	if (out_surface) *out_surface = dst;
}

static void Font_getTextSize(Font* font, const char* text, int* out_width, int* out_height) {
	__Font_drawText(font, text, out_width, out_height, NULL);
}

static SDL_Surface* Font_drawText(Font* font, const char* text) {
	SDL_Surface* surface = NULL;
	__Font_drawText(font, text, NULL, NULL, &surface);
	return surface;
}

typedef void (*Font_renderFunc)(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color);

static void __Font_renderText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color, int s) {	
	x += font->offset_x;
	y += font->offset_y;

	SDL_Surface* surface = Font_drawText(font, text);
	XGL_Texture* texture = XGL_CreateTextureFromSurface(renderer, surface);
	XGL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	
	if (s) {
		XGL_SetTextureColorMod(texture, 0,0,0);
		XGL_RenderCopy(renderer, texture, NULL, &(SDL_Rect){x+2,y+2,surface->w,surface->h});
	}
	
	XGL_SetTextureColorMod(texture, color.r, color.g, color.b);
	XGL_RenderCopy(renderer, texture, NULL, &(SDL_Rect){x,y,surface->w,surface->h});

	XGL_DestroyTexture(texture);
	SDL_FreeSurface(surface);
}

static void Font_renderText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 0);
}
static void Font_shadowText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 1);
}

// --------------------------------------------

static void Fonts_init(void) {
	Font_init(font36);
	Font_init(font24);
	Font_init(font18);
	Font_init(font16);
}
static void Fonts_quit(void) {
	Font_quit(font36);
	Font_quit(font24);
	Font_quit(font18);
	Font_quit(font16);
}

// --------------------------------------------
// input abstraction
// --------------------------------------------

#define PAD_REPEAT_TIMEOUT	300
#define PAD_REPEAT_INTERVAL 100
#define PAD_DEADZONE 12000

typedef enum {
	PAD_ID_NONE = -1,
	
	PAD_ID_UP,
	PAD_ID_DOWN,
	PAD_ID_LEFT,
	PAD_ID_RIGHT,
	
	PAD_ID_A,
	PAD_ID_B,
	PAD_ID_X,
	PAD_ID_Y,
	
	PAD_ID_START,
	PAD_ID_SELECT,
	
	PAD_ID_L1,
	PAD_ID_R1,
	PAD_ID_L2,
	PAD_ID_R2,
	PAD_ID_L3,
	PAD_ID_R3,
	
	PAD_ID_MENU,
	PAD_ID_BACK,
	PAD_ID_PLUS,
	PAD_ID_MINUS,
	PAD_ID_OPEN,
	PAD_ID_CLOSE,
	PAD_ID_POWER,
	
	PAD_ID_COUNT,
} PadButtonId;
typedef enum {
	PAD_NONE 		= 0,
	PAD_UP			= 1u << PAD_ID_UP,
	PAD_DOWN		= 1u << PAD_ID_DOWN,
	PAD_LEFT		= 1u << PAD_ID_LEFT,
	PAD_RIGHT		= 1u << PAD_ID_RIGHT,
	
	PAD_A			= 1u << PAD_ID_A,
	PAD_B			= 1u << PAD_ID_B,
	PAD_X			= 1u << PAD_ID_X,
	PAD_Y			= 1u << PAD_ID_Y,
	
	PAD_START		= 1u << PAD_ID_START,
	PAD_SELECT		= 1u << PAD_ID_SELECT,
	
	PAD_L1			= 1u << PAD_ID_L1,
	PAD_R1			= 1u << PAD_ID_R1,
	PAD_L2			= 1u << PAD_ID_L2,
	PAD_R2			= 1u << PAD_ID_R2,
	PAD_L3			= 1u << PAD_ID_L3,
	PAD_R3			= 1u << PAD_ID_R3,
	
	PAD_MENU		= 1u << PAD_ID_MENU,
	PAD_BACK		= 1u << PAD_ID_BACK,
	PAD_PLUS		= 1u << PAD_ID_PLUS,
	PAD_MINUS		= 1u << PAD_ID_MINUS,
	PAD_OPEN		= 1u << PAD_ID_OPEN,
	PAD_CLOSE		= 1u << PAD_ID_CLOSE,
	PAD_POWER		= 1u << PAD_ID_POWER,
} PadButton;

static struct {
	uint32_t is_pressed;
	uint32_t just_pressed;
	uint32_t just_released;
	uint32_t just_repeated;
	uint32_t repeat_at[PAD_ID_COUNT];
} pad;

static inline int Pad_anyJustPressed(void)	{ return pad.just_pressed!=PAD_NONE; }
static inline int Pad_anyPressed(void)		{ return pad.is_pressed!=PAD_NONE; }
static inline int Pad_anyJustReleased(void)	{ return pad.just_released!=PAD_NONE; }

static inline int Pad_justPressed(PadButton btn)	{ return pad.just_pressed & btn; }
static inline int Pad_isPressed(PadButton btn)	{ return pad.is_pressed & btn; }
static inline int Pad_justReleased(PadButton btn)	{ return pad.just_released & btn; }
static inline int Pad_justRepeated(PadButton btn)	{ return pad.just_repeated & btn; }

static int Pad_fakeButtonEvent(SDL_Event* event, int btn, int press) {
	SDL_memset(event, 0, sizeof(*event));
	event->type = event->jbutton.type = press ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
	event->jbutton.which  = 0;
	event->jbutton.button = btn;
	event->jbutton.state  = press ? SDL_PRESSED : SDL_RELEASED;
	event->jbutton.timestamp = SDL_GetTicks();
	return 1;
}
static int Pad_fakeHatEvent(SDL_Event* event, int value) {
	SDL_memset(event, 0, sizeof(*event));

	event->type = SDL_JOYHATMOTION;
	event->jhat.type = SDL_JOYHATMOTION;
	event->jhat.which = 0;
	event->jhat.hat = 0;
	event->jhat.value = value;
	event->jhat.timestamp = SDL_GetTicks();

	return 1;
}

static void Pad_reset(void) {
	pad.is_pressed = PAD_NONE;
	pad.just_pressed = PAD_NONE;
	pad.just_released = PAD_NONE;
	pad.just_repeated = PAD_NONE;
}
static void Pad_consume(PadButton btn) {
	// pad.is_pressed is intentionally omitted
	pad.just_pressed  &= ~btn;
	pad.just_released &= ~btn;
	pad.just_repeated &= ~btn;
}
static int Pad_nextFrame(void) {
	pad.just_pressed  = PAD_NONE;
	pad.just_released = PAD_NONE;
	pad.just_repeated = PAD_NONE;
	
	uint32_t tick = SDL_GetTicks();
	for (int i=0; i<PAD_ID_COUNT; i++) {
		uint32_t mask = 1 << i;
		if ((pad.is_pressed & mask) && (tick>=pad.repeat_at[i])) {
			pad.just_repeated |= mask;
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}
}
static int Pad_pollEvent(SDL_Event* event) {
	static int ax = 0;
	static int ay = 0;
	static int last_pad = HAT_NEUTRAL;
	
	int result = real_SDL_PollEvent(event);
	if (!result) return result;
	
	// dupe left stick to dpad
	if (event->type==SDL_JOYAXISMOTION) {
		int lx = ax;
		int ly = ay;
		int pad = last_pad;

		if (event->jaxis.axis==AXIS_LX) ax = event->jaxis.value;
		else if (event->jaxis.axis==AXIS_LY) ay = event->jaxis.value;

		if (ax!=lx) {
			if (ax>PAD_DEADZONE) {
				pad |= HAT_RIGHT;
				pad &= ~HAT_LEFT;
			}
			else if (ax<-PAD_DEADZONE) {
				pad |= HAT_LEFT;
				pad &= ~HAT_RIGHT;
			}
			else {
				pad &= ~(HAT_LEFT | HAT_RIGHT);
			}
		}
		if (ay!=ly) {
			if (ay>PAD_DEADZONE) {
				pad |= HAT_DOWN;
				pad &= ~HAT_UP;
			}
			else if (ay<-PAD_DEADZONE) {
				pad |= HAT_UP;
				pad &= ~HAT_DOWN;
			}
			else {
				pad &= ~(HAT_UP | HAT_DOWN);
			}
		}

		if (pad!=last_pad) {
			last_pad = pad;
			Pad_fakeHatEvent(event, pad);
		}
	}
	
	uint32_t tick = SDL_GetTicks();
	int pressed = 0;
	PadButtonId id = PAD_ID_NONE;
	
	if (event->type==SDL_KEYDOWN || event->type==SDL_KEYUP) {
		uint8_t scan = event->key.keysym.scancode;
		pressed = event->type==SDL_KEYDOWN;
			 if (scan==SCAN_CLOSE)	id = PAD_ID_CLOSE;
		else if (scan==SCAN_OPEN)	id = PAD_ID_OPEN;
		else if (scan==SCAN_BACK)	id = PAD_ID_BACK;
		else if (scan==SCAN_POWER)	id = PAD_ID_POWER;
	}
	else if (event->type==SDL_JOYBUTTONDOWN || event->type==SDL_JOYBUTTONUP) {
		uint8_t joy = event->jbutton.button;
		pressed = event->type==SDL_JOYBUTTONDOWN;
			 if (joy==JOY_X)		id = PAD_ID_X;
		else if (joy==JOY_B)		id = PAD_ID_B;
		else if (joy==JOY_Y)		id = PAD_ID_Y;
		else if (joy==JOY_A)		id = PAD_ID_A;
		else if (joy==JOY_START)	id = PAD_ID_START;
		else if (joy==JOY_SELECT)	id = PAD_ID_SELECT;
		else if (joy==JOY_MENU)		id = PAD_ID_MENU;
		else if (joy==JOY_L1)		id = PAD_ID_L1;
		else if (joy==JOY_R1)		id = PAD_ID_R1;
		else if (joy==JOY_L2)		id = PAD_ID_L2;
		else if (joy==JOY_R2)		id = PAD_ID_R2;
		else if (joy==JOY_L3)		id = PAD_ID_L3;
		else if (joy==JOY_R3)		id = PAD_ID_R3;
		else if (joy==JOY_PLUS)		id = PAD_ID_PLUS;
		else if (joy==JOY_MINUS)	id = PAD_ID_MINUS;
	}
	else if (event->type==SDL_JOYHATMOTION) {
		int hat = event->jhat.value;
		uint32_t dpad = PAD_NONE;
		if (hat & SDL_HAT_UP)		dpad |= PAD_UP;
		if (hat & SDL_HAT_DOWN)		dpad |= PAD_DOWN;
		if (hat & SDL_HAT_LEFT)		dpad |= PAD_LEFT;
		if (hat & SDL_HAT_RIGHT)	dpad |= PAD_RIGHT;

		for (id=PAD_ID_UP; id<=PAD_ID_RIGHT; id++) {
			uint32_t mask = 1u << id;

			uint32_t was_pressed = pad.is_pressed & mask;
			uint32_t is_pressed  = dpad & mask;

			if (!is_pressed && was_pressed) {
				pad.is_pressed		&= ~mask; // unset
				pad.just_repeated	&= ~mask; // unset
				pad.just_released	|= mask;  // set
			}
			else if (is_pressed && !was_pressed) {
				pad.just_pressed	|= mask; // set
				pad.just_repeated	|= mask; // set
				pad.is_pressed		|= mask; // set
				pad.repeat_at[id]	= tick + PAD_REPEAT_TIMEOUT;
			}
		}
		
		return result;
	}
	
	if (id==PAD_ID_NONE) return result;
	
	uint32_t mask = 1u << id;
	if (!pressed) {
		pad.is_pressed		&= ~mask; // unset
		pad.just_repeated	&= ~mask; // unset
		pad.just_released	|= mask;  // set
	}
	else if ((pad.is_pressed & mask)==PAD_NONE) {
		pad.just_pressed	|= mask; // set
		pad.just_repeated	|= mask; // set
		pad.is_pressed		|= mask; // set
		pad.repeat_at[id]	= tick + PAD_REPEAT_TIMEOUT;
	}
	
	return result;
}

// --------------------------------------------
// app declarations
// --------------------------------------------
static void App_init(void);
static void App_quit(void);
static void App_load(int state);
static void App_save(int state);
static void App_reset(void);
static void App_extra(void);
static void App_bye(void);
static int App_status(void);
static void TimeZone_quit(void);

// --------------------------------------------
// on-screen display
// --------------------------------------------

static void OSD_set(OSDMode osd) {
	app.osd = osd;
	osd_at = SDL_GetTicks();
}
static int OSD_update(void) {
	if (app.osd==OSD_NONE) return 0;
	if (osd_at+1000>=SDL_GetTicks()) return 0;
	app.osd = OSD_NONE;
	return 1;
}
static void OSD_draw(XGL_Renderer *renderer, char* label, int value, int max) {
	if (!app.xgl.meters || !app.xgl.notches) return;
	
	int x,y,w,h;
	w = 252;
	h = 64;
	x = (SCREEN_WIDTH - w) / 2;
	y = (SCREEN_HEIGHT - h) - 20;
	
	int oy = max==10 ? 0 : h;
	XGL_RenderCopy(renderer, app.xgl.meters, &(SDL_Rect){0,oy,w,h}, &(SDL_Rect){x,y,w,h});
	
	Font_shadowText(renderer, font16, label, x+8, y+8, app.menu ? LIGHT_COLOR : WHITE_COLOR);
	
	if (value<=0) return;
	
	oy = max==10 ? 0 : 30;
	int no = max==10 ? 24 : 12;
	int nw = value * no;
	int nh = 30;
	XGL_RenderCopy(renderer, app.xgl.notches, &(SDL_Rect){0,oy,nw,nh}, &(SDL_Rect){x+8,y+28,nw,nh});
}
static void OSD_render(XGL_Renderer *renderer) {
	if (app.osd==OSD_VOLUME) OSD_draw(renderer, "VOLUME", settings.volume, 20);
	else if (app.osd==OSD_BRIGHTNESS) OSD_draw(renderer, "BRIGHTNESS", settings.brightness, 10);
}

// --------------------------------------------
// device features
// --------------------------------------------
#define AWAKE_TIMEOUT (30 * 1000) // thirty seconds
#define POWEROFF_TIMEOUT 2 * 60 * 1000 // two minutes
#define POWER_TIMEOUT 1000 // one second
static int awake_at = 0;
static int power_at = 0;
static int idling = 0;
static int deidle = 0;
static int idle_at = 0;

static int hangs_on_lid(void) {
	uint32_t game_code = *(volatile uint32_t *)((uint8_t *)drastic_var_system() + 0xc58);
	switch (game_code) {
		case 0x454a5242: // BRJE Radiant Historia US
		case 0x45484942: // BIHE Monster Tale US
		case 0x4a4b3242: // B2KJ Ni no Kuni JP
			return 1;
	}
	return 0;
}

static void Device_mute(int mute) {
	if (mute) raw_vol(0);
	else Settings_setVolume(settings.volume);
	
	if (raw.jack) raw_spk(0);
	else raw_spk(!mute);
}

static void emitKeyEvent(int key, int press) {
	SDL_Event event = {0};
	
	event.type = event.key.type = press ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.state = press ? SDL_PRESSED : SDL_RELEASED;
	event.key.repeat = 0;
	event.key.keysym.sym = key;
	event.key.timestamp = SDL_GetTicks();
	
	SDL_PushEvent(&event);
}

static void Device_idle(void) {
	// enter
	emitKeyEvent(KEY_LID, 1);
	
	raw_bri(0);
	raw_led(1);
	// raw_scr(0); // causes audio lag

	Settings_save();
	App_save(AUTO_STATE);

	idle_at = SDL_GetTicks();
	idling = 1;
	deidle = 0;
}
static void Device_idling(void) {
	// exit
	if (Pad_justPressed(PAD_OPEN)) {
		emitKeyEvent(KEY_LID, 0);
		deidle = 4;
	}
	
	if (Pad_justPressed(PAD_L1) || Pad_justPressed(PAD_R1)) {
		idle_at = SDL_GetTicks();
	}
	
	// a few frames after exit (basically till drastic/game draws again)
	if (deidle>0) {
		deidle -= 1;
		if (deidle==0) {
			// raw_scr(1); // causes audio lag
			raw_led(0);
			Settings_setBrightness(settings.brightness);
			idling = 0;
			return;
		}
	}
	
	// every frame while idling
	if (SDL_GetTicks()>=idle_at+POWEROFF_TIMEOUT) {
		int is_charging = getInt(app.usb);
		if (is_charging) {
			idle_at = SDL_GetTicks(); // keep on
		}
		else {
			raw_vol(0);
			drastic_audio_pause(1);
			unlink("/tmp/exec_loop");
			drastic_quit();
		}
	}
}
static void Device_sleep(void) {
	raw_vol(0);
	raw_bri(0);
	raw_led(1);
	raw_scr(0);

	Settings_save();
	drastic_audio_pause(1);
	
	App_save(AUTO_STATE);
	// drastic_await_save(); // not necessary, drastic_quit already calls this
	
	int was_closed = getInt(app.lid)==2;
	
	int slept_at = SDL_GetTicks();
	int asleep = 1;
	SDL_Event event;
	while (asleep) {
		while (asleep && real_SDL_PollEvent(&event)) {
			if (event.type==SDL_KEYUP) {
				int scan = event.key.keysym.scancode;
				if (scan==SCAN_OPEN || (scan==SCAN_POWER && getInt(app.lid)!=2)) {
					asleep = 0;
					break;
				}
			}
		}
		
		int is_closed = getInt(app.lid)==2;
		if (is_closed) was_closed = 1;
		
		if (!is_closed && was_closed) {
			asleep = 0;
			break;
		}
		
		real_SDL_Delay(200);
	
		if (SDL_GetTicks()>=slept_at+POWEROFF_TIMEOUT) {
			int is_charging = getInt(app.usb);
			if (is_charging) {
				slept_at = SDL_GetTicks(); // keep on
			}
			else {
				unlink("/tmp/exec_loop");
				drastic_quit();
			}
		}
	}

	drastic_audio_pause(0);
	drastic_resync();
	
	raw_scr(1);
	raw_led(0);
	Settings_setBrightness(settings.brightness);
	Settings_setVolume(settings.volume);

	Pad_reset();
}
static void Device_poke(void) {
	awake_at = SDL_GetTicks();
}
static int Device_autosleep(void) {
	int dirty = 0;
	return 0; // disabled
	
	if (Pad_anyPressed() || Pad_anyJustPressed() || Pad_anyJustReleased()) {
		Device_poke();
		return dirty;
	}
	
	if (SDL_GetTicks() >= awake_at+AWAKE_TIMEOUT) {
		int is_charging = getInt(app.usb);
		if (!is_charging) {
			Device_sleep();
			dirty = 1; // force redraw on wake
		}
		Device_poke();
	}
	return dirty;
}
static void Device_poweroff(void) {
	App_bye();
	App_bye();
	sleep(2);
	
	raw_vol(0);
	App_save(AUTO_STATE);
	unlink("/tmp/exec_loop");
	raw_bri(0);
	raw_led(1);
	raw_scr(0);
	raw_spk(0);
	drastic_quit();
}
static int Device_tick(void) {
	raw_poll();
	
	if (idling) Device_idling();
	
	int dirty = 0;
	// must be called outside of Device_handleEvent because
	// the power key doesn't fire repeat events so otherwise
	// nothing triggers this check
	if (power_at && SDL_GetTicks()-power_at>=POWER_TIMEOUT) {
		Device_poweroff();
	}
	
	// same for our repeating volume buttons
	if (Pad_justRepeated(PAD_PLUS)) {
		if (Pad_isPressed(PAD_MENU)) {
			if (settings.brightness<10) {
				Settings_setBrightness(settings.brightness+1);
			}
			OSD_set(OSD_BRIGHTNESS);
		}
		else {
			if (settings.volume<20) {
				Settings_setVolume(settings.volume+1);
			}
			OSD_set(OSD_VOLUME);
		}
		dirty = 1;
	}
	else if (Pad_justRepeated(PAD_MINUS)) {
		if (Pad_isPressed(PAD_MENU)) {
			if (settings.brightness>0) {
				Settings_setBrightness(settings.brightness-1);
			}
			OSD_set(OSD_BRIGHTNESS);
		}
		else {
			if (settings.volume>0) {
				Settings_setVolume(settings.volume-1);
			}
			OSD_set(OSD_VOLUME);
		}
		dirty = 1;
	}
	
	if (OSD_update()) dirty = 1;
	if (App_status()) dirty = 1;
	
	static int last_ff = 0;
	int ff = drastic_fast_forward();
	if (ff != last_ff) {
		last_ff = ff;
		audio.mute = ff;
	}
	
	Pad_nextFrame();
	return dirty;
}
static int Device_handleEvent(SDL_Event* event) {
	if (Pad_justPressed(PAD_CLOSE)) {
		if (app.menu || hangs_on_lid()) {
			Device_sleep();
			return 1;
		}
		else {
			Device_idle();
			return 0;
		}
	}
	
	if (Pad_justPressed(PAD_POWER)) {
		power_at = SDL_GetTicks();
		return 1;
	}
	else if (Pad_justReleased(PAD_POWER)) {
		power_at = 0;
		Device_sleep();
		return 1;
	}
	
	static int menu_combo = 0;
	if (Pad_justPressed(PAD_MENU)) {
		menu_combo = 0;
	}
	
	if (Pad_isPressed(PAD_MENU)) {
		if (Pad_justPressed(PAD_L2)) {
			Pad_consume(PAD_L2);
			settings.grid = !settings.grid;
			menu_combo = 1;
			return 1;
		}
		
		if (Pad_justPressed(PAD_START)) {
			Pad_consume(PAD_START);
			app.capture = 1;
			menu_combo = 1;
			return 1;
		}
	}
	
	// NOTE: moved Pad_justRepeated() to Device_tick()
	// otherwise they only fire on press and not on repeat
	// because Device_handleEvent() is only called when
	// SDL fires an event
	if (Pad_justPressed(PAD_PLUS)) {
		if (Pad_isPressed(PAD_MENU)) {
			if (settings.brightness<10) {
				menu_combo = 1;
			}
		}
		return 1;
	}
	else if (Pad_justPressed(PAD_MINUS)) {
		if (Pad_isPressed(PAD_MENU)) {
			if (settings.brightness>0) {
				menu_combo = 1;
			}
		}
		return 1;
	}
	
	if (Pad_justReleased(PAD_MENU)) {
		if (menu_combo) Pad_consume(PAD_MENU);
		return menu_combo;
	}
	
	return 0;
}

// --------------------------------------------
// app logic
// --------------------------------------------

enum {
	SNAP_USER = 0,
	SNAP_CURRENT,
	SNAP_RESET,
	SNAP_AUTO,
};

static void App_snapName(int game, int snap, char *game_name) {
	if (snap==SNAP_CURRENT) strcpy(game_name, "current");
	else if (snap==SNAP_USER) strcpy(game_name, "session");
	else strcpy(game_name, app.items[game].name);
	
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
}
static void App_snapPath(char *path, const char *game_name, int screen, int snap) {
	Path_concat(path, paths.userdata, "/screenshots/", game_name, "-", Path_digit(screen), ".bmp");
}
static void App_stateName(char *name, int state) {
	if (state==USER_STATE) strcpy(name, "session.st0");
	else Path_concat(name, app.game_name, ".st0");
}
static void App_statePath(char *path, const char *name) {
	Path_concat(path, paths.userdata, "/states/", name);
}
static void App_copy(const char *src, const char *dst) {
	FILE *in = fopen(src, "rb");
	if (!in) return;
	
	FILE *out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return;
	}
	
	char buf[4096];
	size_t n;
	while ((n=fread(buf, 1, sizeof(buf), in))>0) {
		fwrite(buf, 1, n, out);
	}
	
	fclose(out);
	fclose(in);
}
static void App_screenshot(int game, int screen, int snap) {
	if (!app.xgl.screens[screen]) return;
	XGL_Texture* texture = app.xgl.screens[screen];
	
	char game_name[MAX_FILE];
	App_snapName(game, snap, game_name);
	
	char path[MAX_PATH];
	App_snapPath(path, game_name, screen, snap);
	// SDL_Log("screenshot: %s", path);
	
	void *pixels = NULL;
	int pitch = 0;

	int w,h,access;
	Uint32 fmt;
	XGL_QueryTexture(texture, &fmt, &access, &w, &h);
	XGL_LockTexture(texture, NULL, &pixels, &pitch);

	const int depth = SDL_BITSPERPIXEL(fmt);
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, depth, pitch, fmt);
	if (surface) {
		SDL_SaveBMP(surface, path);
		SDL_FreeSurface(surface);
	}

	XGL_UnlockTexture(texture);
}
static void App_preview(int game, int screen, int snap) {
	if (!app.xgl.previews[screen]) return;
	XGL_Texture* texture = app.xgl.previews[screen];
	
	char game_name[MAX_FILE];
	App_snapName(game, snap, game_name);
	
	char path[MAX_PATH];
	App_snapPath(path, game_name, screen, snap);
	// SDL_Log("game_name: %s path: %s", game_name, path);
	if (snap==SNAP_RESET || !exists(path)) Path_concat(path, paths.assets, "/screenshot-", Path_digit(screen), ".png");

	// SDL_Log("preview: %s (snap: %i)", path, snap);
	SDL_Surface* tmp = IMG_Load(path);

	int w, h;
	Uint32 format;
	XGL_QueryTexture(texture, &format, NULL, &w, &h);

	if (format==tmp->format->format) {
		XGL_UpdateTexture(texture, NULL, tmp->pixels, tmp->pitch);
	} else {
		void *dst;
		int dst_pitch;
		if (XGL_LockTexture(texture, NULL, &dst, &dst_pitch)==0) {
			SDL_ConvertPixels(w, h, tmp->format->format, tmp->pixels, tmp->pitch, format, dst, dst_pitch);
			XGL_UnlockTexture(texture);
		}
	}
	
	SDL_FreeSurface(tmp);
}
static void App_capture(const char *name) {
	char path[MAX_PATH];
	sprintf(path, "/tmp/capture-%s.bmp", name);
	SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_WIDTH, SCREEN_HEIGHT*2, 32, SDL_PIXELFORMAT_ARGB8888);
	XGL_RenderReadPixels(app.xgl.renderers[0], NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels, tmp->pitch);
	XGL_RenderReadPixels(app.xgl.renderers[1], NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels + (SCREEN_HEIGHT*tmp->pitch), tmp->pitch);
	SDL_SaveBMP(tmp, path);
	SDL_FreeSurface(tmp);
	app.capture = 0;
}
static int App_session(void) {
	char name[MAX_FILE];
	char path[MAX_PATH];
	
	char src[MAX_PATH];
	char dst[MAX_PATH];
	
	// delete stale session state
	App_stateName(name, USER_STATE);
	App_statePath(dst, name);
	unlink(dst);
	
	char user_name[MAX_FILE];
	char auto_name[MAX_FILE];
	App_snapName(app.current, SNAP_USER, user_name);
	App_snapName(app.current, SNAP_AUTO, auto_name);

	for (int screen=0; screen<SCREEN_COUNT; screen++) {
		App_snapPath(dst, user_name, screen, SNAP_USER);
		unlink(dst);
	}
	
	App_stateName(name, AUTO_STATE);
	App_statePath(path, name);
	if (!exists(path)) return 0;
	
	// load persistent state into session
	App_statePath(src, name);
	App_stateName(name, USER_STATE);
	App_statePath(dst, name);
	App_copy(src, dst);
	
	for (int screen=0; screen<SCREEN_COUNT; screen++) {
		App_snapPath(src, auto_name, screen, SNAP_AUTO);
		App_snapPath(dst, user_name, screen, SNAP_USER);
		App_copy(src, dst);
	}

	return 1;
}
static void App_load(int state) {
	char name[MAX_FILE];
	char path[MAX_PATH];

	if (state==AUTO_STATE) {
		if (!App_session()) return;

		App_stateName(name, USER_STATE);
		App_statePath(path, name);
	}
	else {
		App_stateName(name, state);
		App_statePath(path, name);
	}
	
	if (!exists(path)) return;
	drastic_load_state(name);
}
static void App_save(int state) {
	int snap = state==AUTO_STATE ? SNAP_AUTO : SNAP_USER;
	char name[MAX_FILE];
	App_stateName(name, state);
	App_screenshot(app.current, 0, snap);
	App_screenshot(app.current, 1, snap);
	drastic_save_state(name);
}
static void App_reset(void) {
	drastic_audio_pause(0);
	puts("[LOAD] reset requested");
	loader.state = LOADER_REQUESTED;
	loader.after = LOADER_RESET;
}

static void App_getDisplayName(const char* in_name, char* out_name) {
	char* tmp;
	char work_name[MAX_FILE];
	strcpy(work_name, in_name);
	strcpy(out_name, in_name);
	
	// extract just the filename if necessary
	tmp = strrchr(work_name, '/');
	if (tmp) strcpy(out_name, tmp+1);
	
	// remove extension(s), eg. .p8.png
	while ((tmp = strrchr(out_name, '.'))!=NULL) {
		int len = strlen(tmp);
		if (len>2 && len<=5) {
			if (tmp[1]==' ') tmp[1] = '\0'; // excempt . followed by whitespace
			else tmp[0] = '\0'; // 1-4 letter extension plus dot (was 1-3, extended for .doom files)
		}
		else break;
	}
	
	// remove trailing parens (round and square)
	strcpy(work_name, out_name);
	while ((tmp=strrchr(out_name, '('))!=NULL || (tmp=strrchr(out_name, '['))!=NULL) {
		if (tmp==out_name) break;
		tmp[0] = '\0';
		tmp = out_name;
	}
	
	// make sure we haven't nuked the entire name
	if (out_name[0]=='\0') strcpy(out_name, work_name);
	
	// remove trailing whitespace
	tmp = out_name + strlen(out_name) - 1;
	while(tmp>out_name && isspace((unsigned char)*tmp)) tmp--;
	tmp[1] = '\0';
}
static int App_wrap(Font* font, char* text, int max_lines, char** lines, int* splits) {
	int line_count = 0;

	char line[1024] = {0};
	char word[256] = {0};
	char* p = text;
	int line_width = 0;
	
	while (*p) {
		char* start = p;
	
		// get next word
		while (*p && !isspace(*p)) p++;
		int word_len = p - start;
		strncpy(word, start, word_len);
		word[word_len] = '\0';
		
		// wrap on hyphen
		if (strcmp(word, "-")==0) {
			if (strlen(line)>0 && line_count<max_lines) {
				lines[line_count++] = strdup(line);
				splits[line_count] = 1;
				line[0] = '\0';
			}
			while (isspace(*p)) p++;
			continue;
		}
	
		// append to line
		char test_line[1024] = {0};
		if (strlen(line)>0) sprintf(test_line,"%s %s", line, word);
		else sprintf(test_line,"%s", word);
	
		Font_getTextSize(font, test_line, &line_width, NULL);
		// line_width += font->offset_x * 2;
		
		int ow = 20 + 20;
		if (line_width<=SCREEN_WIDTH-ow) {
			strcpy(line, test_line);
		}
		else {
			if (line_count<max_lines) {
				lines[line_count++] = strdup(line);
			}
			else {
				line[0] = '\0';
				break;
			}
			strcpy(line, word);
		}
	
		// advance to next word
		while (isspace(*p)) p++;
	}

	// add trailing line
	if (strlen(line)>0 && line_count<max_lines) {
		lines[line_count++] = strdup(line);
	}

	// prevent single-word orphan
	if (line_count>=2 && !splits[line_count-1]) {
		char* last = lines[line_count-1];
		
		if (!strchr(last, ' ')) {
			char* prev = lines[line_count-2];
			char* space = strrchr(prev, ' ');
			
			if (space) {
				char tmp[1024];
				sprintf(tmp, "%s %s", space+1, last);
				
				*space = '\0';
				
				free(lines[line_count-1]);
				lines[line_count-1] = strdup(tmp);
			}
		}
	}

	return line_count;
}
static void App_trunc(Font* font, const char* text, int max_width, char* out_text) {
	int fw = 0;
	Font_getTextSize(font, text, &fw, NULL);
	
	if (fw<=max_width) {
		strcpy(out_text, text);
		return;
	}
	
	const char* sep = "...";
	int slen = strlen(sep);
	int sw = 0;
	Font_getTextSize(font, sep, &sw, NULL);
	// width(head) + width(sep) + width(tail) != width(head + sep + tail)
	// because it doesn't account for tracking/kerning against sep and head/tail
	// so we have to fudge it here or out_text could be wider than max_width
	sw += font->tracking * 2;
	
	fw = max_width - sw;
	int hw = fw / 2;
	int len = strlen(text);
	
	char tmp[MAX_FILE];
	int head = 0;
	int tail = 0;

	#define WORDS_ONLY 1 // looks much better to me
	
	// head
	for (int i=0; i<len; i++) {
		if (WORDS_ONLY && i!=0 && text[i]!=' ') continue;
		
		memcpy(tmp, text, i);
		tmp[i] = '\0';
		int w = 0;
		Font_getTextSize(font, tmp, &w, NULL);
		if (w>hw) {
			if (head) break; // found at least one word
			if (w>fw) {
				int j = i;
				while (j>0) {
					j -= 1;
					memcpy(tmp,text,j);
					tmp[j] = '\0';
					Font_getTextSize(font, tmp, &w, NULL);
					if (w<=fw) {
						head = j;
						break;
					}
				}
				break;
			}
			// else continue searching for a word break past the halfway
		}
		head = i;
	}
	
	// trim trailing space or dash
	while (WORDS_ONLY && head>0 && (text[head-1]==' ' || text[head-1]=='-')) head -= 1;
	
	memcpy(tmp, text, head);
	tmp[head] = '\0';
	int w = 0;
	Font_getTextSize(font, tmp, &w, NULL);

	hw = max_width - sw - w;
	if (hw<0) hw = 0;
	
	// tail
	for (int i=1; i<len-head; i++) {
		int start = len - i;
		if (WORDS_ONLY && start>0 && text[start-1]!=' ') continue;
		
		const char* trail = text + start;
		int w = 0;
		Font_getTextSize(font, trail, &w, NULL);
		if (w>hw) break;
		tail = i;
	}
	
	while (WORDS_ONLY && tail>0 && (text[len-tail]==' ' || text[len-tail]=='-')) tail -= 1;
	
	int i = 0;
	memcpy(out_text+i, text, head);
	i += head;
	memcpy(out_text+i, sep, slen);
	i += slen;
	if (tail) {
		memcpy(out_text+i, text+(len-tail), tail);
		i += tail;
	}
	out_text[i] = '\0';
}

static void AA_rect(XGL_Renderer *renderer, int x, int y, int w, int h, int s, SDL_Color c) {
	if (s==0) {
		// body
		XGL_SetRenderDrawColor(renderer, c.r,c.g,c.b,c.a);
		const SDL_Rect rects[] = {
			{x + 0, y+1,   1, h-2}, // left
			{x + 1, y+0, w-2, h  }, // middle
			{x+w-1, y+1,   1, h-2}, // right
		};
		XGL_RenderFillRects(renderer, rects, NUMBER_OF(rects));
	}
	else {
		// outline
		XGL_SetRenderDrawColor(renderer, c.r,c.g,c.b,c.a);
		const SDL_Rect rects[] = {
			{x + 0, y + 1,   1, h-2}, // left
			{x + 1, y + 0, s-1, h  }, // left
			{x + s, y + 0, w-8, s  }, // top
			{x + s, y+h-s, w-8, s  }, // bottom
			{x+w-s, y + 0, s-1, h  }, // right
			{x+w-1, y + 1,   1, h-2}, // right
		};
		XGL_RenderFillRects(renderer, rects, NUMBER_OF(rects));
	}
}
static void AA_bolt(XGL_Renderer *renderer, int x, int y, SDL_Color c) {
	const SDL_Rect rects[] = {
		// top left
		{x+ 6,y+ 0, 6,2},
		{x+ 5,y+ 2, 6,2},
		{x+ 4,y+ 4, 6,2},
		{x+ 3,y+ 6, 6,2},
		// middle
		{x+ 9,y+ 7,11,1},
		{x+ 2,y+ 8,17,2},
		{x+ 1,y+10,17,2},
		{x+ 0,y+12,11,1},
		// bottom right
		{x+11,y+12, 6,2},
		{x+10,y+14, 6,2},
		{x+ 9,y+16, 6,2},
		{x+ 8,y+18, 6,2},
	};
	XGL_SetRenderDrawColor(renderer, c.r,c.g,c.b,c.a);
	XGL_RenderFillRects(renderer, rects, NUMBER_OF(rects));
}
static void AA_bat(XGL_Renderer *renderer, int x, int y, int battery, SDL_Color c) {
	AA_rect(renderer, x,y,48,28, 4, c);
	AA_rect(renderer, x+47,y+8,5,12, 0, c);
	
	int w = CEIL_TO(battery,20) * 30 / 100;
	if (w>0) AA_rect(renderer, x+8,y+8,w+2,12, 0, c);
}
static void App_battery(XGL_Renderer *renderer, int battery, int is_charging, int shadowed) {
	int x = SCREEN_WIDTH - 69;
	int y = 17;
	
	SDL_Color c = LIGHT_COLOR;

	if (shadowed) AA_bat(renderer, x+2,y+2, battery, BLACK_COLOR);
	AA_bat(renderer, x,y, battery, c);

	if (is_charging && battery<100) {
		y += 4;
		
		char percent[8];
		sprintf(percent, "%i%%", battery);
		int w = 0;
		Font_getTextSize(font24, percent, &w, NULL);
		
		x -= w + 4;
		Font_shadowText(renderer, font24, percent, x,y, c);

		x -= 24;
		AA_bolt(renderer, x+2,y+2,BLACK_COLOR);
		AA_bolt(renderer, x,y,c);
	}
}

static int App_status(void) {
	if (!app.menu) return 0;
	
	Uint32 now = SDL_GetTicks();
	if (ui.status_at && now<ui.status_at+1000) return 0;
	ui.status_at = now;
	
	int dirty = 0;
	int charging = getInt(app.usb);
	if (charging!=ui.charging) {
		ui.charging = charging;
		dirty = 1;
	}
	
	int battery = getInt(app.bat);
	if (battery!=ui.battery) {
		ui.battery = battery;
		dirty = 1;
	}
	return dirty;
}

static void App_focus(int current, int snap) {
	ui.current = current;
	ui.snap = snap;
	
	if (ui.preview_current==current && ui.preview_snap==snap) return;
	ui.preview_current = current;
	ui.preview_snap = snap;
	
	App_preview(current,0, snap);
	App_preview(current,1, snap);
}

static void App_grid(XGL_Renderer *renderer) {
	if (settings.grid) XGL_RenderRepeat(renderer, app.xgl.grid, NULL);
}

static void App_background(int screen) {
	XGL_Renderer *renderer = app.xgl.renderers[screen];
	XGL_RenderCopy(renderer, app.xgl.previews[screen], NULL, NULL);
	App_grid(renderer);
	XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,screen*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
}

static void App_top(int hud) {
	XGL_Renderer *renderer = app.xgl.renderers[0];
	App_background(0);
	
	if (hud) {
		Font_shadowText(renderer, font24, "Nintendo DS", 20,20, LIGHT_COLOR);
		
		char name[MAX_FILE];
		App_getDisplayName(app.items[ui.current].name, name);
		
		#define MAX_LINES 8
		char* lines[MAX_LINES];
		int splits[MAX_LINES] = {0};
		int line_count = App_wrap(font36, name, MAX_LINES, lines, splits);
		SDL_Color color = WHITE_COLOR;
		for (int i=0; i<line_count; i++) {
			if (splits[i]) color = LIGHT_COLOR;
			Font_shadowText(renderer, font36, lines[i], 20, 56+(i*39), color);
			free(lines[i]);
		}
	}
	
	App_battery(renderer, ui.battery,ui.charging,1);
	OSD_render(renderer);
}

static void App_bottom(void) {
	App_background(1);
}

static void App_bye(void) {
	App_extra();
	
	int battery = getInt(app.bat);
	int is_charging = getInt(app.usb);
	// const char* lines[] = {
	// 	"Saving &",
	// 	"shutting",
	// 	"down...",
	// };
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];

		XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
		XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
		App_grid(renderer);
		XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
		
		if (i==0) {
			App_battery(renderer, battery,is_charging,1);	
			Font_shadowText(renderer, font24, "Dedicated OS", 20,20, LIGHT_COLOR);
			Font_shadowText(renderer, font36, "Saving & shutting down...", 20, 20+36, WHITE_COLOR);
			
			// for (int j=0; j<3; j++) {
			// 	Font_shadowText(renderer, font36, lines[j], 20, 56+(j*36), WHITE_COLOR);
			// }
		}

		XGL_RenderPresent(renderer);
	}
}
static void App_empty(void) {
	SDL_Log("App_empty");

	App_init();
	App_extra();
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];

		XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
		XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
		App_grid(renderer);
		XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
		
		if (i==0) {
			Font_shadowText(renderer, font24, "Dedicated OS", 20,20, LIGHT_COLOR);
			Font_shadowText(renderer, font36, "No games found", 20, 56, WHITE_COLOR);
		}

		XGL_RenderPresent(renderer);
	}
	
	sleep(5);
	unlink("/tmp/exec_loop");
	App_quit();
}

static int App_sort(const void* a, const void* b) {
	const Entry *i = (const Entry *)a;
	const Entry *j = (const Entry *)b;
	return compareNatural(i->name, j->name);
}
static void App_set(int i) {
	if (!app.count) App_empty();
	
	if (i>=app.count) i -= app.count;
	if (i<0) i += app.count;
	app.current = i;
	
	Entry* item = &app.items[app.current];
	strcpy(settings.game, item->name);
	
	static char game_path[MAX_PATH];
	sprintf(game_path, "%s/%s", item->hidden ? paths.archive : paths.games, settings.game);
	strcpy(app.game_path, game_path);
	
	strcpy(app.game_name, settings.game);
	char *dot = strrchr(app.game_name, '.');
	if (dot && dot!=app.game_name) *dot = '\0';
	
	SDL_Log("settings.game: %s", settings.game);
	SDL_Log("app.game_path: %s", app.game_path);
	SDL_Log("app.game_name: %s", app.game_name);
}
static int App_next(int start, int dir) {
	int i = start;
	int count = app.count;
	for (int _=0; _<count; _++) {
		i += dir;
		if (i>=count) i -= count;
		else if (i<0) i += count;
		if (!app.items[i].hidden || i==app.current) return i;
	}
	return start;
}

static void App_boot(void) {
	SDL_Log("App_boot");
	
	// before even drastic loads
	// this is drastic setup logic
	Paths_init();
	raw_init();
	
	Settings_load();
	
	putString(CPU_PATH "scaling_governor", "userspace");
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	
	char* dirs[] = {
		paths.games,
		paths.archive,
	};
	
	app.count = 0;
	app.current = 0;
	app.capacity = 16;
	app.items = malloc(sizeof(Entry) * app.capacity);

	// get games
	for (int i=0; i<NUMBER_OF(dirs); i++) {
		DIR* dir = opendir(dirs[i]);
		if (dir) {
			struct dirent* entry;
			while ((entry=readdir(dir))!=NULL) {
				if (entry->d_name[0]=='.') continue;
				if (entry->d_type==DT_DIR) continue;
			
				if (app.count>=app.capacity) {
					app.capacity *= 2;
					app.items = realloc(app.items, sizeof(Entry) * app.capacity);
				}
				Entry* item = &app.items[app.count++];
				item->hidden = i;
				snprintf(item->name, MAX_FILE, "%s", entry->d_name);
			}
			closedir(dir);
		}
	}
	
	if (app.count) qsort(app.items, app.count, sizeof(Entry), App_sort);
	
	// get index of last played game
	if (*settings.game) {
		for (int i=0; i<app.count; i++) {
			if (strcmp(app.items[i].name, settings.game)==0) {
				app.current = i;
				break;
			}
		}
	}
	
	// if there is no current and the first game is hidden
	if (app.count>0 && app.items[app.current].hidden) {
		// try to find a non-hidden game
		app.current = App_next(app.current, 1);
	}
	
	App_set(app.current);
}
static void App_init(void) {
	SDL_Log("App_init");
	
	// after SDL_Init
	// this is rendering setup logic
	
	XGL_Init();
	Fonts_init();
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		SDL_Window *window = real_SDL_CreateWindow(NULL, i*SCREEN_WIDTH,0, SCREEN_WIDTH,SCREEN_HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL);
		SDL_GLContext context = SDL_GL_CreateContext(window);
		if (i == 0) {
			SDL_GL_MakeCurrent(window, context);
			SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
		}
		XGL_Renderer *renderer = XGL_CreateRenderer(window, context);
		XGL_Texture *screen = XGL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, XGL_TEXTUREACCESS_STREAMING, DS_WIDTH, DS_HEIGHT);

		app.xgl.windows[i] = window;
		app.xgl.contexts[i] = context;
		app.xgl.renderers[i] = renderer;
		app.xgl.screens[i] = screen;
		
		app.dummy.renderers[i] = (DummyRenderer){.window = window};
		app.dummy.screens[i] = (DummyTexture){.texture = screen};
	}
	
	XGL_Renderer *renderer = app.xgl.renderers[0];	
	
	char path[MAX_PATH];
	app.xgl.meters = XGL_LoadTexture(renderer, Path_concat(path, paths.assets, "/meters.png"));
	app.xgl.notches = XGL_LoadTexture(renderer, Path_concat(path, paths.assets, "/notches.png"));

	app.xgl.grid = XGL_LoadTexture(renderer, Path_concat(path, paths.assets, "/grid.png"));
	
	SDL_Log("video driver: %s", SDL_GetCurrentVideoDriver());

	app.bat = open(BAT_PATH "capacity", O_RDONLY);
	app.usb = open(USB_PATH "online", O_RDONLY);
	app.lid = open(LID_PATH, O_RDONLY);
	
	ui.preview_current = -1;
	ui.preview_snap = -1;
	
	raw_led(0);
}
static void App_quit(void) {
	SDL_Log("App_quit");
	
	free(app.items);
	
	XGL_DestroyTexture(app.xgl.meters);
	XGL_DestroyTexture(app.xgl.notches);
	XGL_DestroyTexture(app.xgl.grid);
	
	if (app.xgl.icons) 		XGL_DestroyTexture(app.xgl.icons);
	if (app.xgl.overlay) 	XGL_DestroyTexture(app.xgl.overlay);
	if (app.xgl.previews[0]) XGL_DestroyTexture(app.xgl.previews[0]);
	if (app.xgl.previews[1]) XGL_DestroyTexture(app.xgl.previews[1]);
	
	close(app.bat);
	close(app.usb);
	close(app.lid);
	
	TimeZone_quit();
	Fonts_quit();
	
	// drastic never destroys renderers or windows so this is safe
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_DestroyTexture(app.xgl.screens[i]);
		XGL_DestroyRenderer(app.xgl.renderers[i]);
		SDL_GL_DeleteContext(app.xgl.contexts[i]);
		SDL_DestroyWindow(app.xgl.windows[i]);
	}
	
	XGL_Quit();
	
	Mic_quit();

	Settings_save();
	raw_quit();
}

static void App_bootlogo(void) {
	int bootlogo = 1;
	int dirty = 1;
	SDL_Event event;
	
	const char *hints[] = {
		// "Scale", "MENU+L2",
		"Lid",	"L2",
		"Mic",	"R2",
		// "Sharpness", "MENU+R2",
		"Brightness", "MENU+VOLUME",
		"Fast Forward", "BACK",
		NULL,
	};
	
	Pad_reset();
	Device_poke();
	
	while (bootlogo) {
		while (bootlogo && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justPressed(PAD_B) || Pad_justPressed(PAD_BACK)) {
			bootlogo = 0;
			break;
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_Renderer *renderer = app.xgl.renderers[i];
	
				XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
				XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
				XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
				
				// top screen
				if (i==0) {
					Font_shadowText(renderer, font24, "Dedicated OS", 20,20, LIGHT_COLOR);
					Font_shadowText(renderer, font36, "Nintendo DS", 20, 56+(i*36), WHITE_COLOR);
					OSD_render(renderer);
				}
				// bottom screen
				else {
					int lw = 0;
					int rw = 0;
					
					int s = 4;
						
					// layout pass
					for (int i = 0; hints[i] != NULL; i += 2) {
						const char *hint = hints[i];
						const char *trigger = hints[i + 1];
						
						int w,h;
						Font_getTextSize(font16, hint, &w, &h);
						if (i<s && w>lw) lw = w; // left
						else if (i>=s && w>rw) rw = w; // right
						
						Font_getTextSize(font24, trigger, &w, &h);
						if (i<s && w>lw) lw = w; // left
						else if (i>=s && w>rw) rw = w; // right
					}
					
					int pad = (SCREEN_WIDTH - lw - rw) / 3;
					int ox = pad;
					int oy = (SCREEN_HEIGHT - 72 - 72 - 40) / 2;
					
					int x = ox;
					int y = oy;
					
					// render pass
					for (int i = 0; hints[i] != NULL; i += 2) {
						const char *hint = hints[i];
						const char *trigger = hints[i + 1];
						
						int j = i % s;
						if (i==s) {
							x = ox + lw + pad;
							y = oy;
						}
						
						Font_shadowText(renderer, font16, hint, x,y, LIGHT_COLOR);
						Font_shadowText(renderer, font24, trigger, x,y+20, WHITE_COLOR);
						
						y += 72;
					}
				}
			}
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	Pad_reset();
}

// --------------------------------------------

typedef struct {
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int ampm;
} DateTime;

static DateTime DateTime_get(void) {
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	return (DateTime){
		.year = tm.tm_year + 1900,
		.month = tm.tm_mon + 1,
		.day = tm.tm_mday,
		.hour = tm.tm_hour,
		.minute = tm.tm_min,
		.second = tm.tm_sec,
		.ampm = tm.tm_hour >= 12,
	};
}
static void DateTime_set(DateTime *dt) {
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock -w", dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
	system(cmd);
}

static int days_in_month(int year, int month) {
	static const uint8_t days[] = {
		31, 28, 31, 30, 31, 30,
		31, 31, 30, 31, 30, 31
	};

	if (month == 2) {
		int leap = (year % 4 == 0 && year % 100 != 0) ||
		           (year % 400 == 0);
		return 28 + leap;
	}

	return days[month - 1];
}

enum {
	FIELD_YEAR = 0,
	FIELD_MONTH,
	FIELD_DAY,
	FIELD_HOUR,
	FIELD_MINUTE,
	// FIELD_SECOND,
	FIELD_AMPM,
};

static void App_datetime(void) {
	int datetime = 1;
	int dirty = 1;
	SDL_Event event;
	
	DateTime dt = DateTime_get();
	int columns[] = {
		dt.year,
		dt.month,
		dt.day,
		dt.hour,
		dt.minute,
		// dt.second,
		dt.ampm,
	};
	char separators[] = {
		'/',
		'/',
		' ',
		':',
		// ':',
		0,
		0,
	};
	int count = NUMBER_OF(columns);
	
	int w = 8;
	for (int i=0; i<count; i++) {
		// column
		if (i==FIELD_YEAR) w += 20 * 4 + 4 * 3;
		else w += 20 * 2 + 4 * 1;
		w += 8;
		
		// separator
		if (i==FIELD_AMPM) break;
		
		int sep = separators[i];
			 if (sep==':') w += 4;
		else if (sep==' ') w += 12;
		else if (sep=='/') w += 20;
		w += 8;
	}
	int h = 8 + 20 + 8;
	int x = (SCREEN_WIDTH - w) / 2;
	int y = (SCREEN_HEIGHT - h) / 2;
	
	Pad_reset();
	Device_poke();
	
	int selected = FIELD_DAY;
	while (datetime) {
		while (datetime && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justPressed(PAD_B) || Pad_justPressed(PAD_BACK)) {
			datetime = 0;
			break;
		}
		
		if (Pad_justRepeated(PAD_LEFT)) {
			dirty = 1;
			selected -= 1;
			if (selected<0) selected += count;
		}
		else if (Pad_justRepeated(PAD_RIGHT)) {
			dirty = 1;
			selected += 1;
			if (selected>=count) selected -= count;
		}
		
		int validate = 0;
		if (Pad_justRepeated(PAD_UP)) {
			dirty = 1;
			columns[selected] += 1;
			if (selected==FIELD_AMPM) columns[FIELD_HOUR] += 12;
			validate = 1;
		}
		else if (Pad_justRepeated(PAD_DOWN)) {
			dirty = 1;
			columns[selected] -= 1;
			if (selected==FIELD_AMPM) columns[FIELD_HOUR] -= 12;
			validate = 1;
		}
		
		if (validate) {
			int year	= columns[FIELD_YEAR];
			int month	= columns[FIELD_MONTH];
			int day		= columns[FIELD_DAY];
			int hour	= columns[FIELD_HOUR];
			int minute	= columns[FIELD_MINUTE];
			// int second	= columns[FIELD_SECOND];
			int ampm	= columns[FIELD_AMPM];
			
			if (year < 1970) year = 1970;
			else if (year > 2100) year = 2100;

			if (month < 1) month = 12;
			else if (month > 12) month = 1;

			int max_day = days_in_month(year, month);

			if (day < 1) day = max_day;
			else if (day > max_day) day = 1;

			if (hour < 0) hour += 24;
			else if (hour >= 24) hour -= 24;
			
			ampm = hour >= 12;

			if (minute < 0) minute = 59;
			else if (minute > 59) minute = 0;

			// if (second < 0) second = 59;
			// else if (second > 59) second = 0;
			
			columns[FIELD_YEAR]		= year;
			columns[FIELD_MONTH]	= month;
			columns[FIELD_DAY]		= day;
			columns[FIELD_HOUR]		= hour;
			columns[FIELD_MINUTE]	= minute;
			// columns[FIELD_SECOND]	= second;
			columns[FIELD_AMPM]		= ampm;

			dt.year = year;
			dt.month = month;
			dt.day = day;
			dt.hour = hour;
			dt.minute = minute;
			// dt.second = second;
			dt.ampm = ampm;
		}
		
		if (Pad_justPressed(PAD_A)) {
			DateTime_set(&dt);
			datetime = 0;
			break;
		}

		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			
			App_top(0);
			App_bottom();
			
			XGL_Renderer *renderer = app.xgl.renderers[1];
			XGL_SetRenderDrawColor(renderer, BLACK_TRIAD,0x80);
			XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
			
			char *lines[] = {
				"Save in-game then",
				"reset to apply",
			};
			int line_count = NUMBER_OF(lines);
			for (int i=0; i<line_count; i++) {
				int ow;
				Font_getTextSize(font18, lines[i], &ow, NULL);
				Font_shadowText(renderer, font18, lines[i], (SCREEN_WIDTH-ow)/2, y + 8 + h + (i * 18), LIGHT_COLOR);
			}
			
			AA_rect(renderer, x,y,w,h, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));
			int ox = x + 8;
			int oy = y + 8;
			for (int j=0; j<count; j++) {
				Font_renderFunc font_renderer = Font_shadowText;
				SDL_Color color = WHITE_COLOR;
				
				if (j==selected) {
					int ow = 20 * 2 + 4 * 1;
					if (j==FIELD_YEAR) ow = 20 * 4 + 4 * 3;
					ow += 8;
					AA_rect(renderer, ox-2,oy-2, ow,28, 0, TRIAD_ALPHA(BLACK_TRIAD,0xff));
					AA_rect(renderer, ox-4,oy-4, ow,28, 0, TRIAD_ALPHA(WHITE_TRIAD,0xff));
					font_renderer = Font_renderText;
					color = DARK_COLOR;
				}
				
				char *format = "%02i";
				if (j==FIELD_YEAR) format = "%04i";
				
				int value = columns[j];
				if (j==FIELD_HOUR) {
					if (value==0) value = 12;
					if (value>12) value -= 12;
				}
				
				char tmp[8];
				if (j==FIELD_AMPM) sprintf(tmp, "%s", value ? "PM" : "AM");
				else sprintf(tmp, format, value);
				
				int len = strlen(tmp);
				for (int k=0; k<len; k++) {
					char c[2];
					sprintf(c, "%c", tmp[k]);
					int ow;
					Font_getTextSize(font24, c, &ow, NULL);
					int tx = ox + (20 - ow) / 2 - font24->offset_x;
					font_renderer(renderer, font24, c, tx, oy, color);
					ox += 20;
					ox += 4;
				}
				
				if (j==FIELD_AMPM) break;
				
				ox += 4;
				value = separators[j];
				if (value) {
					sprintf(tmp, "%c", value);
					Font_shadowText(renderer, font24, tmp, ox,oy, WHITE_COLOR);
				}
					 if (value==':') ox += 4;
				else if (value==' ') ox += 12;
				else if (value=='/') ox += 20;
				ox += 8;
			}
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	Pad_reset();
}

// --------------------------------------------

#define TZ_MAX_DEPTH 8

static struct {
	int count;
	int current;
	int capacity;
	int selected;
	int max_width;
	SDL_Rect rect;
	char (*items)[MAX_FILE];
} tz;

static int TimeZone_sort(const void *a, const void *b) {
	return strcmp(a,b);
}
static int TimeZone_split(const char *name, char parts[TZ_MAX_DEPTH][MAX_FILE]) {
	int count = 0;
	char tmp[MAX_FILE];
	strcpy(tmp, name);

	char *part = tmp;
	while (part && count<TZ_MAX_DEPTH) {
		char *slash = strchr(part, '/');
		if (slash) *slash++ = '\0';
		strcpy(parts[count++], part);
		part = slash;
	}
	return count;
}
static int TimeZone_width(const char *name) {
	char parts[TZ_MAX_DEPTH][MAX_FILE];
	int count = TimeZone_split(name, parts);

	int slash_width;
	Font_getTextSize(font24, "/", &slash_width, NULL);

	int w = 8;
	for (int i=0; i<count; i++) {
		char label[MAX_FILE];
		strcpy(label, parts[i]);
		for (char *c=label; *c; c++) {
			if (*c == '_') *c = ' ';
		}

		int ow;
		Font_getTextSize(font24, label, &ow, NULL);
		w += ow;

		if (i<count-1) {
			w += 8 + slash_width + 8;
		}
	}
	w += 8;
	return w;
}
static int TimeZone_matches(char parts[TZ_MAX_DEPTH][MAX_FILE], int count, char prefix[TZ_MAX_DEPTH][MAX_FILE], int prefix_count) {
	if (count < prefix_count) return 0;
	for (int i=0; i<prefix_count; i++) {
		if (strcmp(parts[i], prefix[i]) != 0) return 0;
	}
	return 1;
}
static void TimeZone_init(void) {
	if (!tz.items) {
		tz.capacity = 512;
		tz.items = malloc(sizeof(*tz.items) * tz.capacity);
		if (!tz.items) return;

		char line[MAX_LINE];
		FILE *file = fopen("/usr/share/zoneinfo/zone1970.tab", "r");
		if (file) {
			while (fgets(line, sizeof(line), file)) {
				if (line[0] == '#' || line[0] == '\n') continue;

				char *country = line;
				char *coords = strchr(country, '\t');
				if (!coords) continue;
				*coords++ = '\0';

				char *name = strchr(coords, '\t');
				if (!name) continue;
				*name++ = '\0';

				char *comment = strchr(name, '\t');
				if (comment) *comment = '\0';

				char *newline = strchr(name, '\n');
				if (newline) *newline = '\0';

				if (!name[0]) continue;

				if (tz.count >= tz.capacity) {
					tz.capacity *= 2;
					char (*items)[MAX_FILE] = realloc(tz.items, sizeof(*tz.items) * tz.capacity);
					if (!items) break;
					tz.items = items;
				}
			
				strcpy(tz.items[tz.count++], name);
			}
			fclose(file);
		}
	
		qsort(tz.items, tz.count, sizeof(*tz.items), TimeZone_sort);
	
		tz.max_width = 0;
		for (int i=0; i<tz.count; i++) {
			int w = TimeZone_width(tz.items[i]);
			if (w>tz.max_width) tz.max_width = w;
		}
	}

	char path[MAX_PATH];
	int len = readlink("/etc/localtime", path, sizeof(path) - 1);
	if (len > 0) {
		path[len] = '\0';

		char *name = path;
		char *prefix = "/usr/share/zoneinfo/";
		if (strncmp(name, prefix, strlen(prefix)) == 0) name += strlen(prefix);

		for (int i=0; i<tz.count; i++) {
			if (strcmp(tz.items[i], name) == 0) {
				tz.current = i;
				break;
			}
		}
	}

	if (tz.current >= tz.count) tz.current = 0;

	char parts[TZ_MAX_DEPTH][MAX_FILE];
	int count = tz.count ? TimeZone_split(tz.items[tz.current], parts) : 0;
	if (tz.selected >= count) tz.selected = count - 1;
	if (tz.selected < 0) tz.selected = 0;

	int w = tz.max_width;
	int h = 8 + 20 + 8;
	int x = (SCREEN_WIDTH - w) / 2;
	int y = (SCREEN_HEIGHT - h) / 2;
	tz.rect = (SDL_Rect){x,y,w,h};
}
static void TimeZone_quit(void) {
	free(tz.items);
	tz.items = NULL;
	tz.count = 0;
	tz.capacity = 0;
}
static void TimeZone_set(void) {
	if (!tz.count) return;
	DateTime datetime = DateTime_get();

	char path[MAX_PATH];
	sprintf(path, "/usr/share/zoneinfo/%s", tz.items[tz.current]);

	unlink("/etc/localtime");
	symlink(path, "/etc/localtime");
	tzset();

	DateTime_set(&datetime);
}
static int TimeZone_update(void) {
	int dirty = 0;
	if (!tz.count) return 0;

	char parts[TZ_MAX_DEPTH][MAX_FILE];
	int count = TimeZone_split(tz.items[tz.current], parts);
	if (!count) return 0;

	if (tz.selected >= count) tz.selected = count - 1;
	if (tz.selected < 0) tz.selected = 0;

	if (Pad_justRepeated(PAD_LEFT)) {
		dirty = 1;
		tz.selected -= 1;
		if (tz.selected<0) tz.selected += count;
	}
	else if (Pad_justRepeated(PAD_RIGHT)) {
		dirty = 1;
		tz.selected += 1;
		if (tz.selected>=count) tz.selected -= count;
	}

	int dir = 0;
	if (Pad_justRepeated(PAD_UP)) dir = -1;
	else if (Pad_justRepeated(PAD_DOWN)) dir = 1;

	if (dir) {
		char first[MAX_FILE] = "";
		char previous[MAX_FILE] = "";
		char next[MAX_FILE] = "";
		char last[MAX_FILE] = "";
		char seen[MAX_FILE] = "";
		int found = 0;

		for (int i=0; i<tz.count; i++) {
			char item_parts[TZ_MAX_DEPTH][MAX_FILE];
			int item_count = TimeZone_split(tz.items[i], item_parts);
			if (item_count <= tz.selected) continue;
			if (!TimeZone_matches(item_parts, item_count, parts, tz.selected)) continue;
			if (strcmp(seen, item_parts[tz.selected]) == 0) continue;

			strcpy(seen, item_parts[tz.selected]);
			if (!first[0]) strcpy(first, seen);

			if (found && !next[0]) strcpy(next, seen);
			if (strcmp(seen, parts[tz.selected]) == 0) found = 1;
			if (!found) strcpy(previous, seen);
			strcpy(last, seen);
		}

		char selected[MAX_FILE];
		if (dir > 0) strcpy(selected, next[0] ? next : first);
		else strcpy(selected, previous[0] ? previous : last);

		for (int i=0; i<tz.count; i++) {
			char item_parts[TZ_MAX_DEPTH][MAX_FILE];
			int item_count = TimeZone_split(tz.items[i], item_parts);
			if (item_count <= tz.selected) continue;
			if (!TimeZone_matches(item_parts, item_count, parts, tz.selected)) continue;
			if (strcmp(item_parts[tz.selected], selected) != 0) continue;

			tz.current = i;
			count = TimeZone_split(tz.items[tz.current], parts);
			if (tz.selected >= count) tz.selected = count - 1;
			dirty = 1;
			break;
		}
	}
	
	return dirty;
}
static void TimeZone_render(void) {
	if (!tz.count) return;

	XGL_Renderer *renderer = app.xgl.renderers[1];
	int x = tz.rect.x;
	int y = tz.rect.y;
	int w = tz.rect.w;
	int h = tz.rect.h;

	AA_rect(renderer, x,y,w,h, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));

	char parts[TZ_MAX_DEPTH][MAX_FILE];
	int count = TimeZone_split(tz.items[tz.current], parts);

	int slash_width;
	Font_getTextSize(font24, "/", &slash_width, NULL);
	
	char *labels[] = {
		"REGION",
		"AREA",
		"LOCATION",
	};
	
	int ox = x + 8;
	int oy = y + 8;
	for (int i=0; i<count; i++) {
		char label[MAX_FILE];
		strcpy(label, parts[i]);
		for (char *c=label; *c; c++) {
			if (*c == '_') *c = ' ';
		}
		
		int j = i==0 ? 0 : (i==count-1 ? 2 : 1); 
		Font_shadowText(renderer, font18, labels[j], ox - font24->offset_x,oy-28, LIGHT_COLOR);
		
		int ow;
		Font_getTextSize(font24, label, &ow, NULL);

		Font_renderFunc font_renderer = Font_shadowText;
		SDL_Color color = WHITE_COLOR;
		if (i == tz.selected) {
			AA_rect(renderer, ox-2,oy-2, ow+8,28, 0, BLACK_COLOR);
			AA_rect(renderer, ox-4,oy-4, ow+8,28, 0, WHITE_COLOR);
			font_renderer = Font_renderText;
			color = DARK_COLOR;
		}

		font_renderer(renderer, font24, label, ox - font24->offset_x,oy, color);
		ox += ow;

		if (i<count-1) {
			ox += 8;
			Font_shadowText(renderer, font24, "/", ox - font24->offset_x,oy, WHITE_COLOR);
			ox += slash_width + 8;
		}
	}
}

static void App_timezone(void) {
	int timezone = 1;
	int dirty = 1;
	SDL_Event event;
	
	TimeZone_init();
	Pad_reset();
	Device_poke();
	
	while (timezone) {
		while (timezone && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (TimeZone_update()) dirty = 1;
		
		if (Pad_justPressed(PAD_A)) {
			TimeZone_set();
			timezone = 0;
			break;
		}
		
		if (Pad_justPressed(PAD_B) || Pad_justPressed(PAD_BACK)) {
			timezone = 0;
			break;
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			
			App_top(0);
			App_bottom();
			TimeZone_render();
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	Pad_reset();
}

// --------------------------------------------

static void App_extra(void) {
	SDL_Surface* tmp;
	if (!app.xgl.overlay) {
		SDL_Log("create overlay");
		int w = 1; // we can just stretch horizontally on the GPU
		int h = SCREEN_HEIGHT * 2;
		tmp = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		
		int o = -16; // brighten (for shaders)
		uint32_t* d = tmp->pixels;
		int total = w * h;
		for (int i=0; i<total; i++,d++) {
			*d = ((64 + o) + ((total - i) * (160 + o) / total)) << 24; // 87.5% to 25%
		}

		app.xgl.overlay = XGL_CreateTextureFromSurface(app.xgl.renderers[0], tmp);
		SDL_FreeSurface(tmp);
	}
	
	if (!app.xgl.previews[0]) {
		app.xgl.previews[0] = XGL_CreateTexture(app.xgl.renderers[0], SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DS_WIDTH,DS_HEIGHT);
		app.xgl.previews[1] = XGL_CreateTexture(app.xgl.renderers[1], SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DS_WIDTH,DS_HEIGHT);
	}
	
	if (!app.xgl.icons) {
		char path[MAX_PATH];
		app.xgl.icons = XGL_LoadTexture(app.xgl.renderers[0], Path_concat(path, paths.assets, "/icons.png"));
	}
	
}

static void App_archive(int current) {
	int archive = 1;
	int dirty = 1;
	int top = 0;
	int rows = 19;
	SDL_Event event;
	
	Pad_reset();
	Device_poke();
	
	while (archive) {
		while (archive && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justRepeated(PAD_UP)) {
			current -= 1;
			if (current<0) current += app.count;
			dirty = 1;
		}
		else if (Pad_justRepeated(PAD_DOWN)) {
			current += 1;
			if (current>=app.count) current -= app.count;
			dirty = 1;
		}
		
		if (Pad_justPressed(PAD_RIGHT)) {
			if (current==app.count-1) current = 0;
			else {
				current += rows;
				if (current>=app.count) current = app.count-1;
			}
			dirty = 1;
		}
		else if (Pad_justPressed(PAD_LEFT)) {
			if (current==0) current = app.count-1;
			else {
				current -= rows;
				if (current<0) current = 0;
			}
			dirty = 1;
		}
		
		if (Pad_justPressed(PAD_B) || Pad_justPressed(PAD_BACK)) {
			archive = 0;
			break;
		}
		
		if (Pad_justPressed(PAD_A)) {
			Entry* item = &app.items[current];
			
			char shown[MAX_PATH];
			sprintf(shown, "%s/%s", paths.games, item->name);
			char hidden[MAX_PATH];
			sprintf(hidden, "%s/%s", paths.archive, item->name);
			
			char *dst,*src;
			if (item->hidden) {
				src = hidden;
				dst = shown;
			}
			else {
				src = shown;
				dst = hidden;
			}
			
			if (!exists(dst) && rename(src, dst)==0) {
				item->hidden = !item->hidden;
				dirty = 1;
			}
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			
			int snap = current==app.current ? SNAP_CURRENT : SNAP_AUTO;
			App_focus(current, snap);
			App_top(1);
			
			XGL_Renderer *renderer = app.xgl.renderers[1];
			XGL_SetRenderDrawColor(renderer, BLACK_TRIAD,0xff);
			XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
			
			// calculate viewport
			if (app.count<=rows) {
				top = 0;
			}
			else {
				int max = app.count - rows;
				int bottom = top + rows - 1;
				if (current<top) top = current;
				else if (current>bottom) top = current - (rows - 1);
				if (top<0) top = 0;
				if (top>max) top = max;
			}
			int end = top + rows;
			if (end>app.count) end = app.count;
	
			int x = 0;
			int y = 0;
			int h = 40;
	
			// draw viewport
			for (int i=top; i<end; i++) {
				int row = i - top;
				int oy = y + row * h;
		
				Entry* item = &app.items[i];
				SDL_Color c = item->hidden ? LIGHT_COLOR : WHITE_COLOR;
				if (i==current) {
					AA_rect(renderer, x,oy,SCREEN_WIDTH,h, 0, c);
					c = BLACK_COLOR;
				}
		
				char name[MAX_FILE];
				char fit[MAX_FILE];
				App_getDisplayName(item->name, name);
				App_trunc(font24, name, SCREEN_WIDTH-40, fit);
		
				XGL_RenderCopy(renderer, app.xgl.icons, &(SDL_Rect){item->hidden?24:0,0,24,24}, &(SDL_Rect){x+8,oy+8,24,24});
				Font_renderText(renderer, font24, fit,	x+40,oy+10, c);
			}
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	Pad_reset();
}

static void App_system(void) {
	int current = ui.current;
	int snap = ui.snap;
	
	char* items[] = {
		"ARCHIVE",
		"DATETIME",
		"TIMEZONE",
	};
	
	int selected = 0;
	int dirty = 1;
	int system = 1;
	SDL_Event event;
	
	Pad_reset();
	Device_poke();
	
	char ip[64];
	raw_ip(ip);
	
	while (system) {
		while (system && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justPressed(PAD_B) || Pad_justPressed(PAD_BACK)) {
			system = 0;
			break;
		}
		
		if (Pad_justPressed(PAD_UP)) {
			selected -= 1;
			dirty = 1;
		}
		else if (Pad_justPressed(PAD_DOWN)) {
			selected += 1;
			dirty = 1;
		}
		
		if (Pad_justPressed(PAD_A)) {
			if (selected==0) {
				App_archive(ui.current);
				App_focus(current, snap);
			}
			else if (selected==1) {
				App_datetime();
			}
			else if (selected==2) {
				App_timezone();
			}
			dirty = 1;
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			
			int count = NUMBER_OF(items);
			if (selected<0) selected += count;
			selected %= count;
			
			App_top(0);
			App_bottom();
			
			XGL_Renderer *renderer = app.xgl.renderers[1];
			int x,y,w,h;
			
			y = 0;
			h = SCREEN_HEIGHT;
			
			int mw = 0;
			for (int i=0; i<count; i++) {
				Font_getTextSize(font24, items[i], &w, NULL);
				if (w>mw) mw = w;
			}
			mw += font24->offset_x;
			
			w = 8 + mw + 8;
			x = (SCREEN_WIDTH - w) / 2;
			
			int oh = ((count-1) * 40) + 36;
			y += (h - oh) / 2;
			h = oh;
			oh = 40;
			
			AA_rect(renderer, x-8,y-8,8+w+8,8+h+8, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));
			
			for (int i=0; i<count; i++) {
				Font_renderFunc font_renderer = Font_shadowText;
				SDL_Color color = WHITE_COLOR;
				
				if (i==selected) {
					AA_rect(renderer, x+2,y+(i*oh)+2,w,36, 0, TRIAD_ALPHA(BLACK_TRIAD,0xff));
					AA_rect(renderer, x,y+(i*oh),w,36, 0, TRIAD_ALPHA(WHITE_TRIAD,0xff));
					font_renderer = Font_renderText;
					color = DARK_COLOR;
				}
				
				font_renderer(renderer, font24, items[i], x+8, y+8+(i*oh), color);
			}
			
			if (ip[0]) {
				Font_getTextSize(font18, ip, &w, NULL);
				h = 15;
				x = (SCREEN_WIDTH - w) / 2;
				y = SCREEN_HEIGHT - (8+h+8) - 20;
				AA_rect(renderer, x-8,y-8,8+w+8,8+h+8, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));
				Font_shadowText(renderer, font18, ip, x,y, WHITE_COLOR);
			}
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	Pad_reset();
}

static uint64_t time_us(void) {
	struct timeval tv;
	real_gettimeofday(&tv, NULL);
	return ((uint64_t)tv.tv_sec * 1000000ULL) + (uint64_t)tv.tv_usec;
}

static void App_menu(void) {
	uint64_t open_us = time_us();
	app.menu = 1;
	
	SDL_Log("enter menu");
	Mic_pause(1);
	Device_mute(1);
	putString(CPU_PATH "scaling_setspeed", FREQ_MENU);
	
	int current = app.current;
	App_screenshot(current, 0, SNAP_CURRENT);
	App_screenshot(current, 1, SNAP_CURRENT);
	
	App_extra();
	ui.preview_current = -1;
	ui.preview_snap = -1;
	ui.status_at = 0;
	App_focus(current, SNAP_CURRENT);
	
	char* save_items[] = {
		"SAVE",
		"LOAD",
		"RESET",
	};
	char* load_items[] = {
		"LOAD",
		"RESET",
	};
	
	Pad_reset();
	Device_poke();

	int selected = 0;
	int dirty = 1;
	int in_menu = 1;
	SDL_Event event;
	while (in_menu) {
		while (in_menu && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justReleased(PAD_MENU)) {
			App_system();
			dirty = 1;
		}
		if (Pad_justPressed(PAD_BACK)) {
			in_menu = 0;
		}
		
		if (Pad_justPressed(PAD_UP)) {
			selected -= 1;
			dirty = 1;
		}
		else if (Pad_justPressed(PAD_DOWN)) {
			selected += 1;
			dirty = 1;
		}
		
		if (Pad_justPressed(PAD_RIGHT)) {
			selected = 0;
			current = App_next(current, +1);
			dirty = 1;
		}
		else if (Pad_justPressed(PAD_LEFT)) {
			selected = 0;
			current = App_next(current, -1);
			dirty = 1;
		}
		
		// TODO: tmp
		// if (Pad_justPressed(PAD_START)) {
		// 	App_bootlogo();
		// 	dirty = 1;
		// }
		
		if (Pad_justPressed(PAD_B)) {
			if (current!=app.current) {
				current = app.current;
				selected = 0;
				dirty = 1;
			}
			else {
				in_menu = 0;
			}
		}
		if (Pad_justPressed(PAD_A)) {
			if (current==app.current) {
				if (selected==0) { // SAVE
					App_save(USER_STATE);
				}
				else if (selected==1) { // LOAD
					App_load(USER_STATE);
				}
				else if (selected==2) { // RESET
					App_reset();
				}
				in_menu = 0;
			}
			else {
				if (selected==0) { // LOAD
					App_save(AUTO_STATE);
					App_set(current);
					Settings_save();
				
					drastic_audio_pause(0);
					puts("[LOAD] resume requested");
					loader.state = LOADER_REQUESTED;
					loader.after = LOADER_RESUME;
				
					in_menu = 0;
				}
				else if (selected==1) { // RESET
					App_save(AUTO_STATE);
					App_set(current);
					Settings_save();

					App_session(); // switch session to incoming game
					App_reset();
				
					in_menu = 0;
				}
			}
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;

		// render
		if (dirty) {
			dirty = 0;
			
			char** items;
			int count;
			if (current==app.current) {
				items = save_items;
				count = NUMBER_OF(save_items);
			}
			else {
				items = load_items;
				count = NUMBER_OF(load_items);
			}
			
			// TODO: BAD this is update logic in render block
			if (selected<0) selected += count;
			selected %= count;
			
			int reset_selected = current==app.current ? selected==2 : selected==1;
			int snap = reset_selected ? SNAP_RESET : (current==app.current ? (selected==0 ? SNAP_CURRENT : SNAP_USER) : SNAP_AUTO);
			App_focus(current, snap);
			App_top(1);
			
			XGL_Renderer *renderer = app.xgl.renderers[1];
			int x,y,w,h;
			App_bottom();
			
			// center in bottom "screen"
			y = 0;
			h = SCREEN_HEIGHT;

			int mw = 0;
			for (int i=0; i<count; i++) {
				Font_getTextSize(font24, items[i], &w, NULL);
				if (w>mw) mw = w;
			}
			mw += font24->offset_x;
			
			w = 8 + mw + 8;
			x = (SCREEN_WIDTH - w) / 2;

			int oh = ((count-1) * 40) + 36;
			y += (h - oh) / 2;
			h = oh;
			oh = 40;

			AA_rect(renderer, x-8,y-8,8+w+8,8+h+8, 0, TRIAD_ALPHA(BLACK_TRIAD,0x40));

			for (int i=0; i<count; i++) {
				Font_renderFunc font_renderer = Font_shadowText;
				SDL_Color color = WHITE_COLOR;

				if (i==selected) {
					AA_rect(renderer, x+2,y+(i*oh)+2,w,36, 0, TRIAD_ALPHA(BLACK_TRIAD,0xff));
					AA_rect(renderer, x,y+(i*oh),w,36, 0, TRIAD_ALPHA(WHITE_TRIAD,0xff));
					font_renderer = Font_renderText;
					color = DARK_COLOR;
				}

				font_renderer(renderer, font24, items[i], x+8, y+8+(i*oh), color);
			}
			
			if (app.capture) App_capture("menu");
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_RenderPresent(app.xgl.renderers[i]);
			}
		}
		else {
			real_SDL_Delay(16);
		}
	}
	
	Pad_reset();
	
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	if (loader.state==LOADER_IDLE) {
		Device_mute(0);
		Mic_pause(0);
		drastic_resync();
	}
	SDL_Log("exit menu");

	app.menu_us += time_us() - open_us;
	app.menu = 0;
}

static int rendered_first_frame = 0;
static void App_render(void) {
	if (idling) {
		real_SDL_Delay(1); // drastic paces itself
		return;
	}
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];
		XGL_RenderCopy(renderer, app.xgl.screens[i], NULL, NULL);
		App_grid(renderer);
		
		if (i==0) {
			OSD_render(renderer);

			if (audio.mute) {
				if (!app.xgl.icons) {
					char path[MAX_PATH];
					app.xgl.icons = XGL_LoadTexture(renderer, Path_concat(path, paths.assets, "/icons.png"));
				}
				XGL_RenderCopy(renderer, app.xgl.icons, &(SDL_Rect){48,0,24,24}, &(SDL_Rect){4,4,24,24});
			}
		}
	}

	if (app.capture) App_capture("game");
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_RenderPresent(app.xgl.renderers[i]);
	}
	
	if (!rendered_first_frame) {
		dmesg("rendered_first_frame");
		rendered_first_frame = 1;
	}
}

// --------------------------------------------
// hook SDL
// --------------------------------------------

int SDL_Init(Uint32 flags) {
	Settings_setBrightness(settings.brightness);
	int result = real_SDL_Init(flags);
	App_init();
	return result;
}
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	return app.xgl.windows[0]; // created in App_init
}
void SDL_SetWindowSize(SDL_Window* window, int w, int h) {
	// window size is always 640x480
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags) {
	for (int i=0; i<SCREEN_COUNT; i++) {
		if (window == app.dummy.renderers[i].window) return (SDL_Renderer *)&app.dummy.renderers[i];
	}
	return NULL;
}
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) {
	return 0; // complete render takeover
}
void SDL_RenderGetLogicalSize(SDL_Renderer *renderer, int *w, int *h) {
	// unused
	if (w) *w = 0;
	if (h) *h = 0;
}
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	return 0; // complete render takeover
}
int SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode mode) {
	return 0; // complete render takeover
}
int SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode mode) {
	return 0; // complete render takeover
}
int SDL_RenderClear(SDL_Renderer* renderer) {
	return 0; // complete render takeover
}
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture  *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
	return 0; // complete render takeover
}
void SDL_RenderPresent(SDL_Renderer *_) {
	if (loader.state!=LOADER_IDLE) {
		if (loader.state==LOADER_REQUESTED) {
			puts("[LOAD] requesting load...");
			loader.state = LOADER_AWAITING;
			drastic_load_nds_and_jump(app.game_path);
			return;
		}
		
		if (loader.state==LOADER_COMPLETE) {
			puts("[LOAD] load complete");
			loader.state = LOADER_IDLE;
			if (loader.after==LOADER_RESUME) {
				puts("[LOAD] perform resume");
				loader.after = LOADER_RESET;
				App_load(AUTO_STATE);
			}
			else puts("[LOAD] perform reset");
			Device_mute(0);
		}
		
		return;
	}
	
	App_render(); // complete render takeover

	if (Pad_justReleased(PAD_MENU)) {
		App_menu();
		return;
	}

	// no before-drastic-frame hook so call this 
	// as late as possible so it's ready at the 
	// beginning of the next frame
	Device_tick();
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int type, int w, int h) {
	if (type==SDL_TEXTUREACCESS_STREAMING && w==DS_WIDTH && h==DS_HEIGHT) {
		for (int i=0; i<SCREEN_COUNT; i++) {
			DummyTexture *dummy = &app.dummy.screens[i];
			if (!dummy->claimed) {
				dummy->claimed = 1;
				return (SDL_Texture *)dummy;
			}
		}
	}
	return (SDL_Texture *)&app.dummy.ignored;
}
int SDL_LockTexture(SDL_Texture * texture, const SDL_Rect * rect, void **pixels, int *pitch) {
	DummyTexture *dummy = (DummyTexture *)texture;
	if (!dummy || !dummy->texture) return SDL_SetError("invalid texture lock");
	return XGL_LockTexture(dummy->texture, rect, pixels, pitch);
}
void SDL_UnlockTexture(SDL_Texture * texture) {
	DummyTexture *dummy = (DummyTexture *)texture;
	if (dummy && dummy->texture) XGL_UnlockTexture(dummy->texture);
}
int SDL_UpdateTexture(SDL_Texture * texture, const SDL_Rect * rect, const void *pixels, int pitch) {
	return 0;
}
void SDL_DestroyTexture(SDL_Texture *texture) {
	DummyTexture *dummy = (DummyTexture *)texture;
	if (dummy) dummy->claimed = 0;
}

int SDL_PollEvent(SDL_Event* event) {
	while (1) {
		int result = Pad_pollEvent(event);
		if (!result) return result;
		
		if (Device_handleEvent(event)) continue;
		
		if (Pad_justPressed(PAD_MENU)) continue;
		
		// LOG_event(event);
		switch (event->type) {
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_FINGERMOTION: {
				// TODO: maybe this has something to do with the dummy 2d renderer?
				int x = raw_x / 2; // drastic is expecting SCREEN_WIDTH/2?
				int y = raw_y; // but just SCREEN_HEIGHT?
				event->tfinger.x = x;
				event->tfinger.y = y;
			} break;
		}
		
		return result;
	}
}
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
	dmesg("SDL_OpenAudio");
	raw_audio_init();
	raw_vol(0);
	
	SDL_AudioSpec hijacked = *desired;
	audio.callback = desired->callback;
	audio.userdata = desired->userdata;
	hijacked.callback = Audio_callback;
	hijacked.userdata = NULL;
	// hijacked.samples = 256;
	
	int result = real_SDL_OpenAudio(&hijacked, obtained);
	Settings_setVolume(settings.volume); // must be done here (or later)
	return result;
}
void SDL_Delay(uint32_t ms) {
	if (ms>16) {
		SDL_Log("excessive SDL_Delay(%i)!", ms);
		ms = 16;
	}
	real_SDL_Delay(ms);
}

// --------------------------------------------
// logging and string formatting hooks
// --------------------------------------------

// #define DISABLE_LOGGING

int __printf_chk(int flag, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	
	// listen for loading events
	if (loader.state==LOADER_AWAITING && strncmp(fmt, "Gamecard title", 14)==0) {
		puts("[LOAD] load started");
		loader.state = LOADER_STARTED;
	}
	else if (loader.state==LOADER_STARTED && strncmp(fmt, "Remapping DTCM", 14)==0) {
		puts("[LOAD] load completed");
		loader.state = LOADER_COMPLETE;
	}

	// silence spam
	if (strncmp(fmt, "vf ticks", 8)==0 || strncmp(fmt, "ticks_delta", 11)==0) {
		va_end(ap);
		return 0;
	}
	
#ifdef DISABLE_LOGGING
	va_end(ap);
	return 0;
#endif
	
	char tick_fmt[1024];
	snprintf(tick_fmt, sizeof tick_fmt, "[%i] %s", SDL_GetTicks(), fmt);

	int r = __vprintf_chk(flag, tick_fmt, ap);
	va_end(ap);
	return r;

	return 0;
}
int puts (const char *msg) {
#ifdef DISABLE_LOGGING
	return 0;
#endif

	char tick_msg[1024];
	snprintf(tick_msg, sizeof tick_msg, "[%i] %s", SDL_GetTicks(), msg);
	return real_puts(tick_msg);
}

__attribute__((noreturn))
void exit(int status) {
	App_quit();
	real_exit(status);
}
__attribute__((noreturn))
void _exit(int status) {
	App_quit();
	real__exit(status);
}
int system(const char *command) {
	unsetenv("LD_PRELOAD");
	return real_system(command);
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
	// hooked to repath save data
	if (fmt && strcmp(fmt, "%s%cbackup%c%s.dsv")==0) return real__snprintf_chk(s, maxlen, flag, slen, "%s/%s/%s.sram", paths.userdata, "saves", app.game_name);

	va_list ap;
	va_start(ap, fmt);
	int r = __vsnprintf_chk(s, maxlen, flag, slen, fmt, ap);
	va_end(ap);
	return r;
}
int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...) {
	// hooked to repath official bios (but not clean room bios)
	if (fmt && strcmp(fmt, "%s%csystem%c%s")==0)  {
		va_list ap;
		va_start(ap, fmt);
		va_arg(ap, const char*); va_arg(ap, int); va_arg(ap, int); // skip %s %c %c
		const char* bios = va_arg(ap, const char*);	// %s
		va_end(ap);
		
		if (bios && strncmp(bios, "nds_",4)==0) return real__sprintf_chk(s, flag, slen, "%s/%s", paths.bios, bios);
	}

	va_list ap;
	va_start(ap, fmt);
	int r = __vsprintf_chk(s, flag, slen, fmt, ap);
	va_end(ap);
	return r;
}

// --------------------------------------------

static int ts_fd = -1;

int open64(const char *pathname, int flags, ...) {
	mode_t mode = 0;
	int fd;

	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);

		fd = real_open64(pathname, flags, mode);
	}
	else {
		fd = real_open64(pathname, flags);
	}
	
	if (ts_fd == -1 && pathname && strstr(pathname, "/dev/input/event1")) {
		ts_fd = fd;
		SDL_Log("ts_fd %i", ts_fd);
	}
	
	return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
	ssize_t n = real_read(fd, buf, count);

	if (n > 0 && fd == ts_fd) {
		struct input_event *ev = buf;
		int num = n / sizeof(*ev);

		for (int i = 0; i < num; i++) {
			if (ev[i].type == EV_ABS) {
				if (ev[i].code == ABS_MT_POSITION_X) raw_x = ev[i].value;
				if (ev[i].code == ABS_MT_POSITION_Y) raw_y = ev[i].value;
			}
		}
	}

	return n;
}

int gettimeofday(struct timeval *tv, void *tz) {
	int r = real_gettimeofday(tv, tz);
	if (r == 0 && tv) {
		uint64_t us = tv->tv_sec * 1000000ULL + tv->tv_usec;
		us -= app.menu_us;
		tv->tv_sec = us / 1000000ULL;
		tv->tv_usec = us % 1000000ULL;
	}
	return r;
}

// --------------------------------------------
// hijack main to modify args
// --------------------------------------------

static int pick_main(struct dl_phdr_info *i, size_t s, void *out) {
	(void)s;
	if (!i->dlpi_name || !i->dlpi_name[0]) {
		*(uintptr_t *)out = (uintptr_t)i->dlpi_addr;
		return 1;
	}
	return 0;
}
static inline uintptr_t find_exe_base(void) {
	uintptr_t base = 0;
	dl_iterate_phdr(pick_main, &base);
	return base;
}
static void patch_absolute_jump(void *target, void *replacement) {
    uint32_t patch[4];

    patch[0] = 0x58000050;
    patch[1] = 0xd61f0200;
    memcpy(&patch[2], &replacement, sizeof(replacement));

    size_t page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~(page_size - 1);

    mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache((char *)target, (char *)target + sizeof(patch));
    mprotect((void *)page, page_size, PROT_READ | PROT_EXEC);
}

static void resolve_real(void) {
	// hook glibc? functions
	real__libc_start_main = dlsym(RTLD_NEXT, "__libc_start_main");
	real__sprintf_chk = dlsym(RTLD_NEXT, "__sprintf_chk");
	real__snprintf_chk = dlsym(RTLD_NEXT, "__snprintf_chk");
	real_exit  = dlsym(RTLD_NEXT, "exit");
	real__exit = dlsym(RTLD_NEXT, "_exit");
	real_system = dlsym(RTLD_NEXT, "system");
	real_puts = dlsym(RTLD_NEXT, "puts");
	real__printf_chk = dlsym(RTLD_NEXT, "__printf_chk");
	
	real_open64 = dlsym(RTLD_NEXT, "open64");
	real_read = dlsym(RTLD_NEXT, "read");
	real_gettimeofday = dlsym(RTLD_NEXT, "gettimeofday");
	
	// hook SDL functions
	real_SDL_Init = dlsym(RTLD_NEXT, "SDL_Init");
	
	real_SDL_CreateWindow = dlsym(RTLD_NEXT, "SDL_CreateWindow");
	real_SDL_SetWindowSize = dlsym(RTLD_NEXT, "SDL_SetWindowSize");
	
	real_SDL_PollEvent = dlsym(RTLD_NEXT, "SDL_PollEvent");
	
	real_SDL_OpenAudio = dlsym(RTLD_NEXT, "SDL_OpenAudio");
	real_SDL_Delay = dlsym(RTLD_NEXT, "SDL_Delay");
	
	app.base = find_exe_base();
	
	// patch drastic functions
	void *target = (uint8_t *)app.base + 0x6c7a0;
	patch_absolute_jump(target, drastic_spu_get_microphone_sample);
	
	printf("resolved real function pointers\n"); fflush(stdout);
}

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static inline void hook(void) {
	pthread_once(&resolve_once, resolve_real);
}

static drastic_main_t drastic_main = NULL;
static int override_args(int argc, char **argv, char **envp) {
	hook();
	App_boot();
	char *new_argv[] = { argv[0], app.game_path, NULL };
	
	puts("[LOAD] resume requested");
	return drastic_main(2, new_argv, envp);
}

int __libc_start_main(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
	dmesg("__libc_start_main");
	hook();
	drastic_main = main;
	return real__libc_start_main(override_args, argc, ubp_av, init, fini, rtld_fini, stack_end);
}
