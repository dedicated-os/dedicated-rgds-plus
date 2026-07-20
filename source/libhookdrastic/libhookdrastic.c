#define _GNU_SOURCE // for RTLD_NEXT
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <dirent.h>
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

#define CEIL_TO(x, n) (((x) + (n) - 1) / (n) * (n))
#define NUMBER_OF(items) (sizeof(items) / sizeof((items)[0]))

#define SDCARD_PATH		"/mnt/mmc"
#define GAMES_PATH 		SDCARD_PATH "/games"
#define BIOS_PATH		SDCARD_PATH "/bios"
#define MICS_PATH		SDCARD_PATH "/mics"
#define ARCHIVE_PATH	GAMES_PATH "/archive"
#define SYSTEM_PATH 	SDCARD_PATH "/system"
#define ASSETS_PATH		SYSTEM_PATH "/assets"
#define USERDATA_PATH	SDCARD_PATH "/userdata"

#define BAT_PATH 		"/sys/class/power_supply/cw2015-battery/"
#define USB_PATH 		"/sys/class/power_supply/usb/"
#define CPU_PATH		"/sys/devices/system/cpu/cpu0/cpufreq/"
#define LID_PATH		"/sys/class/anbernic_misc/hallkey" // 3:open,2:closed

#define FREQ_MENU		"408000"
#define FREQ_GAME		"1800000"

#define AUTO_SLOT 0

#define MAX_LINE 1024
#define MAX_PATH 512
#define MAX_FILE 256

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
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

// --------------------------------------------

typedef enum {
	OSD_NONE,
	OSD_VOLUME,
	OSD_BRIGHTNESS,
} OSDMode;

typedef enum {
	MODE_MENU,
	MODE_ARCHIVE,
} MenuMode;

typedef enum {
	SCALE_NATIVE,
	SCALE_ASPECT,

	SCALE_COUNT
} ScaleMode;
typedef enum {
	SHADER_NEAREST,
	SHADER_SHARP,
	SHADER_LINEAR,

	SHADER_COUNT
} ScaleShader;

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
		XGL_Texture *drastic_menu;
		XGL_Texture *icons;
		XGL_Shader *shaders[SHADER_COUNT];
	} xgl;
	
	SDL_Rect rects[SCREEN_COUNT];
	
	struct {
		SDL_Renderer* renderers[SCREEN_COUNT];
		SDL_Texture* screens[SCREEN_COUNT];
		SDL_Texture* drastic_menu;
		int claimed[SCREEN_COUNT];
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
	int fast_forward;
} app;

static int osd_at;

static int raw_x,raw_y; // touch screen fix

// --------------------------------------------
// support
// --------------------------------------------

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

	int drm;
	struct {
		uint32_t connector;
		uint32_t dpms;
	} dsi[SCREEN_COUNT];
	int dsi_count;
	
	snd_mixer_t *mix;
	snd_mixer_elem_t *dac;
} raw;

static void raw_init(void) {
	raw.bri0 = open("/sys/class/backlight/backlight1/brightness", O_WRONLY | O_CLOEXEC);
	raw.bri1 = open("/sys/class/backlight/backlight/brightness", O_WRONLY | O_CLOEXEC);
	raw.led0 = open("/sys/class/anbernic_misc/lowpwr_led", O_WRONLY | O_CLOEXEC);
	raw.led1 = open("/sys/class/anbernic_misc/work_led", O_WRONLY | O_CLOEXEC);
	
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
	
	// I hate audio code
	snd_mixer_open(&raw.mix, 0);
	snd_mixer_attach(raw.mix, "hw:0");
	snd_mixer_selem_register(raw.mix, NULL, NULL);
	snd_mixer_load(raw.mix);

	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "DAC");
	raw.dac = snd_mixer_find_selem(raw.mix, sid);
}
static void raw_quit(void) {
	close(raw.bri0);
	close(raw.bri1);
	close(raw.led0);
	close(raw.led1);
	close(raw.drm);
	snd_mixer_close(raw.mix);
}

static void raw_vol(int value) {
	snd_mixer_selem_set_playback_volume_all(raw.dac, value);
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

// --------------------------------------------
// settings
// --------------------------------------------

static struct {
	int version;
	int volume;		// 0-20
	int brightness;	// 0 - 10
	int scale;
	int shader;
	int unused[3];
	char game[MAX_PATH];
} settings = {
	.version = 1,
	.volume = 6,
	.brightness = 3,
	.scale = SCALE_ASPECT,
	.shader = SHADER_SHARP,
};

#define SETTINGS_PATH USERDATA_PATH "/settings.bin"
static void Settings_load(void) {
	raw_vol(0);
	
	if (access(SETTINGS_PATH, F_OK)!=0) return;
	
	FILE* file = fopen(SETTINGS_PATH, "rb");
	if (!file) return;
	fread(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_save(void) {
	FILE* file = fopen(SETTINGS_PATH, "wb");
	if (!file) return;
	fwrite(&settings, sizeof(settings), 1, file);
	fclose(file);
}
static void Settings_setVolume(int value) {
	static const uint8_t raw[21] = {
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
	
	raw_vol(raw[value]);
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
static int (*real_SDL_RenderSetLogicalSize)(SDL_Renderer*, int, int) = NULL;
static SDL_Renderer* (*real_SDL_CreateRenderer)(SDL_Window*, int, Uint32) = NULL;
static int (*real_SDL_RenderClear)(SDL_Renderer *) = NULL;
static void (*real_SDL_RenderPresent)(SDL_Renderer*) = NULL;
static SDL_Texture* (*real_SDL_CreateTexture)(SDL_Renderer*, Uint32, int, int, int) = NULL;
static int (*real_SDL_LockTexture)(SDL_Texture * texture, const SDL_Rect * rect, void **pixels, int *pitch) = NULL;
static void (*real_SDL_UnlockTexture)(SDL_Texture * texture) = NULL;
static int (*real_SDL_UpdateTexture)(SDL_Texture * texture, const SDL_Rect * rect, const void *pixels, int pitch) = NULL;
static void (*real_SDL_DestroyTexture)(SDL_Texture *) = NULL;
static int (*real_SDL_RenderCopy)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) = NULL;
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

// --------------------------------------------
// logging
// --------------------------------------------

#define LOG(...) printf(__VA_ARGS__); printf("\n"); fflush(stdout)
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
typedef void (*drastic_audio_revert_t)(void *);

static inline void* drastic_var_system(void) {
	return PTR_AT(app.base + 0x15ff30); // follow arg in Cutter
}
static void drastic_audio_pause(int flag) {
	// not sure this is necessary but
	// it feels like good hygiene?
	void* sys = drastic_var_system();
	void* audio = (uint8_t *)sys + 0x1587000;
	
	if (flag) {
		drastic_audio_pause_t drastic_audio_pause = GET_PFN(app.base + 0x0008caa0);
		drastic_audio_pause(audio);
	}
	else {
		drastic_audio_unpause_t drastic_audio_unpause = GET_PFN(app.base + 0x0008caf0);
		drastic_audio_unpause(audio);
		// TODO: this seems to cause hangs
		// drastic_audio_revert_t drastic_audio_revert = GET_PFN(app.base + 0x000886a0);
		// drastic_audio_revert(audio);
	}
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

	drastic_load_nds_t d_load_nds = GET_PFN(app.base + 0x0006fd30); // nm ./drastic | grep load_nds
	drastic_reset_system_t d_reset_system = GET_PFN(app.base + 0x0000fd50); // nm ./drastic | grep reset_system

	void* sys = drastic_var_system();
	d_load_nds((uint8_t *)sys + 800, path);
	d_reset_system(sys);
	
	jmp_buf *env = (jmp_buf *)((uint8_t *)sys + 0x3b2a840); // from main in Cutter
	longjmp(*env, 1); // no return
}
static void drastic_load_state(int slot) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_load_state_t d_load_state = GET_PFN(app.base + 0x000746f0); // nm ./drastic | grep load_state
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/states/%s.st%i", app.game_name, slot);

	d_load_state(sys, path, NULL,NULL,0);
}
static void drastic_save_state(int slot) {
	drastic_await_save();

	void* sys = drastic_var_system();	
	drastic_save_state_t d_save_state = GET_PFN(app.base + 0x00074da0); // nm ./drastic | grep save_state
	
	char name[MAX_FILE];
	sprintf(name, "%s.st%i", app.game_name, slot);
	
	d_save_state(sys, USERDATA_PATH "/states/", name, NULL,NULL);
}
static void drastic_quit(void) {
	drastic_await_save();
	drastic_quit_t d_quit = GET_PFN(app.base + 0x0000e8d0); // nm ./drastic | grep quit
	void* sys = drastic_var_system();	
	d_quit(sys);
}

static int compareNatural(const char *a, const char *b) {
	while (*a && *b) {
		// ensure "Game 10.ext" sorts after "Game 2.ext"
		if (isdigit(*a) && isdigit(*b)) {
			char *ea, *eb;
			long na = strtol(a, &ea, 10);
			long nb = strtol(b, &eb, 10);
			
			if (na!=nb) return (na<nb) ? -1 : 1;

			a = ea;
			b = eb;
		} else {
			int ca = tolower(*a);
			int cb = tolower(*b);
			
			if (ca!=cb) {
				// special case: treat '.' as less than ' '
				// ensure "Game.ext" sorts before "Game 2.ext"
				if (ca=='.' && cb==' ') return -1;
				if (ca==' ' && cb=='.') return  1;
				return ca - cb;
			}
			
			a++;
			b++;
		}
	}
	return tolower(*a) - tolower(*b);
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
		['A']['Y'] = -6,
		['Y']['A'] = -6,
		['T']['-'] = -6,
		['-']['T'] = -6,
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
		['T']['-'] = -4,
		['-']['T'] = -4,
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
		['T']['-'] = -3,
		['-']['T'] = -3,
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
		['T']['-'] = -2,
		['-']['T'] = -2,
	},
};

void Font_init(Font* font) {
	char path[MAX_FILE];
	sprintf(path, "%s/%s", ASSETS_PATH, font->name);
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
void Font_quit(Font* font) {
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

void Font_getTextSize(Font* font, const char* text, int* out_width, int* out_height) {
	__Font_drawText(font, text, out_width, out_height, NULL);
}

SDL_Surface* Font_drawText(Font* font, const char* text) {
	SDL_Surface* surface = NULL;
	__Font_drawText(font, text, NULL, NULL, &surface);
	return surface;
}

typedef void (*Font_renderFunc)(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color);

void __Font_renderText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color, int s) {	
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

void Font_renderText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 0);
}
void Font_shadowText(XGL_Renderer* renderer, Font* font, const char* text, int x, int y, SDL_Color color) {
	__Font_renderText(renderer, font, text, x,y, color, 1);
}

// --------------------------------------------

void Fonts_init(void) {
	Font_init(font36);
	Font_init(font24);
	Font_init(font18);
	Font_init(font16);
}
void Fonts_quit(void) {
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
static void App_load(void);
static void App_save(void);
static void App_reset(void);
static void App_extra(void);
static void App_sync(int force);
static void App_bye(void);

// --------------------------------------------
// device features
// --------------------------------------------
#define AWAKE_TIMEOUT (30 * 1000) // thirty seconds
#define SLEEP_TIMEOUT (2 * 60 * 1000) // two minutes
#define WAKE_DEFER 250 // quarter of a second
#define POWER_TIMEOUT 1000 // one second
static int awake_at = 0;
static int power_at = 0;
static void Device_mute(int mute) {
	if (mute) raw_vol(0);
	else Settings_setVolume(settings.volume);
}
static void Device_sleep(void) {
	raw_vol(0);
	raw_bri(0);
	raw_led(1);
	raw_scr(0);

	Settings_save();
	drastic_audio_pause(1);
	
	App_save();
	// drastic_await_save(); // not necessary, drastic_quit already calls this
	
	int was_closed = getInt(app.lid)==2;
	
	#define POWEROFF_TIMEOUT 2 * 60 * 1000 // two minutes
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
	App_save();
	unlink("/tmp/exec_loop");
	raw_bri(0);
	raw_led(1);
	raw_scr(0);
	drastic_quit();
}
static int Device_tick(void) {
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
			app.osd = OSD_BRIGHTNESS;
		}
		else {
			if (settings.volume<20) {
				Settings_setVolume(settings.volume+1);
			}
			app.osd = OSD_VOLUME;
		}
		osd_at = SDL_GetTicks();
		dirty = 1;
	}
	else if (Pad_justRepeated(PAD_MINUS)) {
		if (Pad_isPressed(PAD_MENU)) {
			if (settings.brightness>0) {
				Settings_setBrightness(settings.brightness-1);
			}
			app.osd = OSD_BRIGHTNESS;
		}
		else {
			if (settings.volume>0) {
				Settings_setVolume(settings.volume-1);
			}
			app.osd = OSD_VOLUME;
		}
		osd_at = SDL_GetTicks();
		dirty = 1;
	}
	
	Pad_nextFrame();
	return dirty;
}
static int Device_handleEvent(SDL_Event* event) {
	if (Pad_justPressed(PAD_CLOSE)) {
		Device_sleep();
		return 1;
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
			settings.scale = (settings.scale + 1) % SCALE_COUNT;
			App_sync(1);
			menu_combo = 1;
			return 1;
		}
		else if (Pad_justPressed(PAD_R2)) {
			Pad_consume(PAD_R2);
			settings.shader = (settings.shader + 1) % SHADER_COUNT;
			App_sync(1);
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
	SNAP_SAVE = 0,
	SNAP_CURRENT,
	SNAP_RESET,
};

static void App_screenshot(int game, int screen, int snap) {
	if (!app.xgl.screens[screen]) return;
	XGL_Texture* texture = app.xgl.screens[screen];
	
	char game_name[MAX_FILE];
	if (snap==SNAP_CURRENT) strcpy(game_name, "current");
	else strcpy(game_name, app.items[game].name);
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/screenshots/%s-%i.bmp", game_name, screen);
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
	if (snap==SNAP_CURRENT) strcpy(game_name, "current");
	else strcpy(game_name, app.items[game].name);
	char *dot = strrchr(game_name, '.');
	if (dot && dot!=game_name) *dot = '\0';
	
	char path[MAX_PATH];
	sprintf(path, USERDATA_PATH "/screenshots/%s-%i.bmp", game_name, screen);
	// SDL_Log("game_name: %s path: %s", game_name, path);
	if (snap==SNAP_RESET || !exists(path)) sprintf(path, ASSETS_PATH "/screenshot-%i.png", screen);

	SDL_Log("preview: %s (snap: %i)", path, snap);
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
static int App_capture(const char *name) {
	char path[MAX_PATH];
	sprintf(path, "/tmp/capture-%s.bmp", name);
	SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_WIDTH, SCREEN_HEIGHT*2, 32, SDL_PIXELFORMAT_ARGB8888);
	XGL_RenderReadPixels(app.xgl.renderers[0], NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels, tmp->pitch);
	XGL_RenderReadPixels(app.xgl.renderers[1], NULL, SDL_PIXELFORMAT_ARGB8888, tmp->pixels + (SCREEN_HEIGHT*tmp->pitch), tmp->pitch);
	SDL_SaveBMP(tmp, path);
	SDL_FreeSurface(tmp);
	app.capture = 0;
}
static void App_sync(int force) {
	if (force) app.synced = 0;
	if (app.synced) return;	
	
	// TODO: no-op here?
}
static void App_load(void) {
	drastic_load_state(AUTO_SLOT);
}
static void App_save(void) {
	App_screenshot(app.current, 0, SNAP_SAVE);
	App_screenshot(app.current, 1, SNAP_SAVE);
	drastic_save_state(AUTO_SLOT);
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
	int x = 571;
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
static void App_OSD(XGL_Renderer *renderer, char* label, int value, int max) {
	int nh = 28;
	int x,y,w,h;
	Font_getTextSize(font16, label, &w, &h);
	w = 252;
	h = 64;
	x = (SCREEN_WIDTH - w) / 2;
	y = (SCREEN_HEIGHT - h) - 32;
	
	AA_rect(renderer, x,y,w,h, 0, TRIAD_ALPHA(BLACK_TRIAD,0x60));

	Font_shadowText(renderer, font16, label, x+8, y+8, LIGHT_COLOR);
	
	int nw = max==10?20:8;
	int no = max==10?24:12;
	for (int i=0; i<max; i++) {
		int nx = x + 8;
		int ny = y + 28;
		AA_rect(renderer, nx+i*no+2,ny+2,nw,nh, 0, BLACK_COLOR);
		if (i<value) {
			AA_rect(renderer, nx+i*no,ny,nw,nh, 0, WHITE_COLOR);
		}
	}
}
static void App_bye(void) {
	App_extra();
	
	int battery = getInt(app.bat);
	int is_charging = getInt(app.usb);
	const char* lines[] = {
		"Saving &",
		"shutting",
		"down...",
	};
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];
		XGL_SetRenderShader(renderer, app.xgl.shaders[SHADER_NEAREST]);

		XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
		XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
		XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
		
		if (i==0) {
			App_battery(renderer, battery,is_charging,1);	
			Font_shadowText(renderer, font24, "Dedicated OS", 20,20, LIGHT_COLOR);
			for (int j=0; j<3; j++) {
				Font_shadowText(renderer, font36, lines[j], 20, 56+(j*36), WHITE_COLOR);
			}
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
		XGL_SetRenderShader(renderer, app.xgl.shaders[SHADER_NEAREST]);

		XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
		XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
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
	sprintf(game_path, "%s/%s", item->hidden ? ARCHIVE_PATH : GAMES_PATH, settings.game);
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
	raw_init();
	
	Settings_load();
	
	putString(CPU_PATH "scaling_governor", "userspace");
	putString(CPU_PATH "scaling_setspeed", FREQ_GAME);
	
	char* dirs[] = {
		GAMES_PATH,
		ARCHIVE_PATH,
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
		
		SDL_Renderer *sdl_renderer = real_SDL_CreateRenderer(window, -1, 0);
		SDL_Texture *sdl_screen = real_SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DS_WIDTH, DS_HEIGHT);
		
		app.dummy.renderers[i] = sdl_renderer;
		app.dummy.screens[i] = sdl_screen;
	}
	
	XGL_Renderer *renderer = app.xgl.renderers[0];	
	app.xgl.shaders[SHADER_NEAREST] = XGL_GetDefaultTextureShader(renderer);
	app.xgl.shaders[SHADER_SHARP] = XGL_CreateSharpShader(renderer);
	app.xgl.shaders[SHADER_LINEAR] = XGL_CreateLinearShader(renderer);
	app.xgl.drastic_menu = XGL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, XGL_TEXTUREACCESS_STREAMING, 800,480);
	app.dummy.drastic_menu = real_SDL_CreateTexture(app.dummy.renderers[0], SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, 800,480);
		
	SDL_Log("video driver: %s", SDL_GetCurrentVideoDriver());

	app.bat = open(BAT_PATH "capacity", O_RDONLY);
	app.usb = open(USB_PATH "online", O_RDONLY);
	app.lid = open(LID_PATH, O_RDONLY);
	
	raw_led(0);
}
static void App_quit(void) {
	SDL_Log("App_quit");
	
	free(app.items);
	
	if (app.xgl.icons) 		XGL_DestroyTexture(app.xgl.icons);
	if (app.xgl.overlay) 	XGL_DestroyTexture(app.xgl.overlay);
	if (app.xgl.previews[0]) XGL_DestroyTexture(app.xgl.previews[0]);
	if (app.xgl.previews[1]) XGL_DestroyTexture(app.xgl.previews[1]);
	
	close(app.bat);
	close(app.usb);
	close(app.lid);
	
	Fonts_quit();
	
	SDL_DestroyTexture(app.dummy.drastic_menu);
	XGL_DestroyTexture(app.xgl.drastic_menu);
	for (int i=0; i<SHADER_COUNT; i++) {
		XGL_DestroyShader(app.xgl.shaders[i]);
	}
	
	// drastic never destroys renderers or windows so this is safe
	for (int i=0; i<SCREEN_COUNT; i++) {
		SDL_DestroyTexture(app.dummy.screens[i]);
		SDL_DestroyRenderer(app.dummy.renderers[i]);
		XGL_DestroyTexture(app.xgl.screens[i]);
		XGL_DestroyRenderer(app.xgl.renderers[i]);
		SDL_GL_DeleteContext(app.xgl.contexts[i]);
		SDL_DestroyWindow(app.xgl.windows[i]);
	}
	
	XGL_Quit();
	
	Settings_save();
	raw_quit();
}

static void App_bootlogo(void) {
	int bootlogo = 1;
	int dirty = 1;
	SDL_Event event;
	
	const char *hints[] = {
		"Scale", "MENU+L2",
		"Lid",	"L2",
		"Mic",	"R2",
		"Sharpness", "MENU+R2",
		"Brightness", "MENU+VOLUME",
		"Fast Forward", "HOME/BACK",
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
		
		if (Pad_justPressed(PAD_B)) {
			bootlogo = 0;
			break;
		}
		
		if (Device_autosleep()) dirty = 1;
		if (Device_tick()) dirty = 1;
		
		if (dirty) {
			dirty = 0;
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_Renderer *renderer = app.xgl.renderers[i];
				XGL_SetRenderShader(renderer, app.xgl.shaders[SHADER_NEAREST]);

				XGL_SetRenderDrawColor(renderer, MID_TRIAD,0xff);
				XGL_RenderFillRect(renderer, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT});
				XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
			
				// top screen
				if (i==0) {
					Font_shadowText(renderer, font24, "Dedicated OS", 20,20, LIGHT_COLOR);
					Font_shadowText(renderer, font36, "Nintendo DS", 20, 56+(i*36), WHITE_COLOR);
				
				}
				// bottom screen
				else {
					int lw = 0;
					int rw = 0;
					
					// layout pass
					for (int i = 0; hints[i] != NULL; i += 2) {
						const char *hint = hints[i];
						const char *trigger = hints[i + 1];
						
						int w,h;
						Font_getTextSize(font16, hint, &w, &h);
						if (i<6 && w>lw) lw = w; // left
						else if (i>=6 && w>rw) rw = w; // right
						
						Font_getTextSize(font24, trigger, &w, &h);
						if (i<6 && w>lw) lw = w; // left
						else if (i>=6 && w>rw) rw = w; // right
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
						
						int j = i % 6;
						if (i==6) {
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
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d UTC'; hwclock --utc -w", dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
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
		
		if (Pad_justPressed(PAD_B)) {
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
			
			SDL_Rect rect = {0,0,DS_WIDTH*2,DS_HEIGHT*2};
			rect.x = (SCREEN_WIDTH - rect.w) / 2;
			rect.y = (SCREEN_HEIGHT - rect.h);
			
			int scale = settings.scale==SCALE_ASPECT;
			XGL_Shader *default_shader = app.xgl.shaders[SHADER_NEAREST];
			XGL_Shader *screen_shader = app.xgl.shaders[settings.shader];
			
			int i = 1;
			XGL_Renderer *renderer = app.xgl.renderers[i];
			
			XGL_SetRenderShader(renderer, scale ? screen_shader : default_shader);
    		XGL_RenderCopy(renderer, app.xgl.previews[i], NULL, scale ? NULL : &rect);
			
			XGL_SetRenderShader(renderer, default_shader);
			XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
			
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
					font_renderer(renderer, font24, c, ox+(20-ow)/2,oy, color);
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
			XGL_RenderPresent(renderer);
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

		uint32_t* d = tmp->pixels;
		int total = w * h;
		for (int i=0; i<total; i++,d++) {
			*d = (64 + ((total - i) * 160 / total)) << 24; // 87.5% to 25%
		}

		app.xgl.overlay = XGL_CreateTextureFromSurface(app.xgl.renderers[0], tmp);
		SDL_FreeSurface(tmp);
	}
	
	if (!app.xgl.previews[0]) {
		app.xgl.previews[0] = XGL_CreateTexture(app.xgl.renderers[0], SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DS_WIDTH,DS_HEIGHT);
		app.xgl.previews[1] = XGL_CreateTexture(app.xgl.renderers[1], SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DS_WIDTH,DS_HEIGHT);
	}
	
	if (!app.xgl.icons) {
		app.xgl.icons = XGL_LoadTexture(app.xgl.renderers[0], ASSETS_PATH "/icons.png");
	}
	
}
static void App_menu(void) {
	SDL_Log("enter menu");
	app.menu = 1;
	Device_mute(1);
	putString(CPU_PATH "scaling_setspeed", FREQ_MENU);
	
	static int last_battery = 0;
	static int was_charging = 0;
	
	int current = app.current;
	App_screenshot(current, 0, SNAP_CURRENT);
	App_screenshot(current, 1, SNAP_CURRENT);
	
	App_extra();
	
	char* save_items[] = {
		"SAVE",
		"LOAD",
		"ARCHIVE",
		"RESET",
	};
	char* load_items[] = {
		"LOAD",
		"ARCHIVE",
	};
	
	Pad_reset();
	Device_poke();

	int selected = 0;
	int dirty = 1;
	int top = 0;
	int rows = 15;
	int in_menu = 1;
	SDL_Event event;
	int mode = MODE_MENU;
	int last_osd = app.osd;
	while (in_menu) {
		
		if (osd_at+1000<SDL_GetTicks()) app.osd = OSD_NONE;
		if (last_osd!=app.osd) {
			last_osd = app.osd;
			dirty = 1;
		}
		
		while (in_menu && Pad_pollEvent(&event)) {
			if (Device_handleEvent(&event)) {
				dirty = 1;
				continue;
			}
		}
		
		if (Pad_justReleased(PAD_MENU)) {
			in_menu = 0;
		}
		
		if (mode==MODE_MENU) {
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
			
			if (Pad_justPressed(PAD_SELECT)) {
				App_datetime();
				dirty = 1;
			}
			if (Pad_justPressed(PAD_START)) {
				// TODO: tmp
				App_bootlogo();
				dirty = 1;
			}
			
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
						App_save();
					}
					else if (selected==1) { // LOAD
						App_load();
					}
					else if (selected==2) { // ARCHIVE
						mode = MODE_ARCHIVE;
						selected = current;
						dirty = 1;
					}
					else if (selected==3) { // RESET
						App_reset();
					}
					if (mode==MODE_MENU) in_menu = 0;
				}
				else {
					if (selected==0) { // LOAD
						App_save();
						App_set(current);
						Settings_save();
					
						drastic_audio_pause(0);
						puts("[LOAD] resume requested");
						loader.state = LOADER_REQUESTED;
						loader.after = LOADER_RESUME;
					
						in_menu = 0;
					}
					else if (selected==1) { // ARCHIVE
						mode = MODE_ARCHIVE;
						selected = current;
						dirty = 1;
					}
				}
			}
		}
		else if (mode==MODE_ARCHIVE) {
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
			
			if (Pad_justPressed(PAD_B)) {
				mode = MODE_MENU;
				dirty = 1;
				// reusing unused selected to store current
				// upon entering archive so we can restore
				// when existing, selected will always
				// revert to ARCHIVE
				current = selected;
				selected = current==app.current ? 2 : 1;
			}
			
			if (Pad_justPressed(PAD_A)) {
				Entry* item = &app.items[current];
				
				char shown[MAX_FILE];
				sprintf(shown, "%s/%s", GAMES_PATH, item->name);
				char hidden[MAX_FILE];
				sprintf(hidden, "%s/%s", ARCHIVE_PATH, item->name);
				
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
		}
		
		// TODO: should these be running every frame?
		// TODO: need to duplicate logic in App_render()
		int is_charging = getInt(app.usb);
		if (is_charging!=was_charging) {
			was_charging = is_charging;
			dirty = 1;
		}
		
		int battery = getInt(app.bat);
		if (battery!=last_battery) {
			last_battery = battery;
			dirty = 1;
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
			if (mode==MODE_MENU) {
				if (selected<0) selected += count;
				selected %= count;
			}
			
			int snap = selected==3 ? SNAP_RESET : (current==app.current && selected==0 ? SNAP_CURRENT : SNAP_SAVE);
			App_preview(current,0, snap);
			App_preview(current,1, snap);
			
			SDL_Rect rect = {0,0,DS_WIDTH*2,DS_HEIGHT*2};
			rect.x = (SCREEN_WIDTH - rect.w) / 2;
			rect.y = (SCREEN_HEIGHT - rect.h);
			
			int scale = settings.scale==SCALE_ASPECT;
			XGL_Shader *default_shader = app.xgl.shaders[SHADER_NEAREST];
			XGL_Shader *screen_shader = app.xgl.shaders[settings.shader];
			
			// game name
			char name[MAX_FILE];
			App_getDisplayName(app.items[current].name, name);
			
			for (int i=0; i<SCREEN_COUNT; i++) {
				XGL_Renderer *renderer = app.xgl.renderers[i];
				if (i && mode==MODE_ARCHIVE); // don't draw bottom screen in archive mode
				else {
					XGL_SetRenderShader(renderer, scale ? screen_shader : default_shader);
		    		XGL_RenderCopy(renderer, app.xgl.previews[i], NULL, scale ? NULL : &rect);
				}
	    
				XGL_SetRenderShader(renderer, default_shader);
				XGL_RenderCopy(renderer, app.xgl.overlay, &(SDL_Rect){0,i*SCREEN_HEIGHT,1,SCREEN_HEIGHT}, NULL);
				
				// top screen
				if (i==0) {
					// system
					Font_shadowText(renderer, font24, "Nintendo DS", 20,20, LIGHT_COLOR);
					
					// game name
					#define MAX_LINES 8
					char* lines[MAX_LINES];
					int splits[MAX_LINES] = {0};
					int line_count = App_wrap(font36, name, MAX_LINES, lines, splits);
					SDL_Color color = WHITE_COLOR;
					for (int i=0; i<line_count; i++) {
						if (splits[i]) color = LIGHT_COLOR;
						Font_shadowText(renderer, font36, lines[i], 20, 56+(i*36), color);
						free(lines[i]);
					}
					
					// battery
					App_battery(renderer, battery,is_charging,1);
					
					// osd
					if (app.osd==OSD_VOLUME) App_OSD(renderer, "VOLUME", settings.volume, 20);
					else if (app.osd==OSD_BRIGHTNESS) App_OSD(renderer, "BRIGHTNESS", settings.brightness, 10);
				}
				// bottom screen
				else {
					int x,y,w,h;
					if (mode==MODE_MENU) {
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
					}
					else if (mode==MODE_ARCHIVE) {
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
				
						x = 0;
						y = 0;
						h = 32;
				
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
					
							char fit[MAX_FILE];
							App_getDisplayName(item->name, name);
							App_trunc(font18, name, SCREEN_WIDTH-40, fit);
					
							XGL_RenderCopy(renderer, app.xgl.icons, &(SDL_Rect){item->hidden?24:0,0,24,24}, &(SDL_Rect){x+4,oy+4,24,24});
							Font_renderText(renderer, font18, fit,	x+32,oy+9, c);
						}
					}
				}
				
				rect.y = 0;
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
	if (!app.fast_forward && loader.state==LOADER_IDLE) Device_mute(0);
	app.menu = 0;
	SDL_Log("exit menu");
}

static int render_drastic_menu = 0;
static void App_render(void) {
	if (render_drastic_menu) {
		render_drastic_menu = 0;
		XGL_Renderer *renderer = app.xgl.renderers[0];
		XGL_SetRenderShader(renderer, XGL_GetDefaultTextureShader(renderer));
		XGL_RenderCopy(renderer, app.xgl.drastic_menu, &(SDL_Rect){0,0,SCREEN_WIDTH,SCREEN_HEIGHT}, NULL);
		XGL_RenderPresent(renderer);
		return;
	}
	
	SDL_Rect rect = {0,0,DS_WIDTH*2,DS_HEIGHT*2};
	rect.x = (SCREEN_WIDTH - rect.w) / 2;
	rect.y = (SCREEN_HEIGHT - rect.h);
	
	int scale = settings.scale==SCALE_ASPECT;
	XGL_Shader *default_shader = app.xgl.shaders[SHADER_NEAREST];
	XGL_Shader *screen_shader = app.xgl.shaders[settings.shader];
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];
		XGL_SetRenderShader(renderer, scale ? screen_shader : default_shader);
	    XGL_RenderCopy(renderer, app.xgl.screens[i], NULL, scale ? NULL : &rect);
		XGL_SetRenderShader(renderer, XGL_GetDefaultTextureShader(renderer));
		
		if (i==0) {
			if (app.osd==OSD_VOLUME) App_OSD(renderer, "VOLUME", settings.volume, 20);
			else if (app.osd==OSD_BRIGHTNESS) App_OSD(renderer, "BRIGHTNESS", settings.brightness, 10);
		
			if (app.fast_forward) {
				if (!app.xgl.icons) app.xgl.icons = XGL_LoadTexture(renderer, ASSETS_PATH "/icons.png");
				XGL_RenderCopy(renderer, app.xgl.icons, &(SDL_Rect){48,0,24,24}, &(SDL_Rect){4,4,24,24});
			}
		
		}
		rect.y = 0;
	}
	
	if (app.capture) App_capture("game");
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_RenderPresent(app.xgl.renderers[i]);
	}
	
	if (osd_at+1000<SDL_GetTicks()) app.osd = OSD_NONE;
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
	return app.dummy.renderers[0]; // created in App_init
}
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) {
	return 1; // complete render takeover
}
int SDL_RenderClear(SDL_Renderer* renderer) {
	return 1; // complete render takeover
}
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture  *texture, const SDL_Rect *srcrect, const SDL_Rect *dstrect) {
	if (texture==app.dummy.drastic_menu) render_drastic_menu = 1;
	return 1; // complete render takeover
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
				drastic_load_state(AUTO_SLOT);
			}
			else puts("[LOAD] perform reset");
			Device_mute(0);
		}
		
		return;
	}
	
	App_render(); // complete render takeover

	Device_tick();
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int type, int w, int h) {
	// SDL_Log("SDL_CreateTexture(%s, %i,%i)", SDL_GetPixelFormatName(format), w,h);
	SDL_Texture *texture = NULL;
	if (type==SDL_TEXTUREACCESS_STREAMING && w==DS_WIDTH && h==DS_HEIGHT) {
		if (!app.dummy.claimed[0]) {
			app.dummy.claimed[0] = 1;
			texture = app.dummy.screens[0];
		}
		else if (!app.dummy.claimed[1]) {
			app.dummy.claimed[1] = 1;
			texture = app.dummy.screens[1];
		}
	}
	else {
		if (w==800 && h==480) {
			texture = app.dummy.drastic_menu;
		}
	}
	if (!texture) {
		texture = real_SDL_CreateTexture(renderer, format, type, w, h);
	}
	return texture;
}
int SDL_LockTexture(SDL_Texture * texture, const SDL_Rect * rect, void **pixels, int *pitch) {
	if (texture==app.dummy.screens[0]) {
		return XGL_LockTexture(app.xgl.screens[0], rect, pixels, pitch);
	}
	else if (texture==app.dummy.screens[1]) {
		return XGL_LockTexture(app.xgl.screens[1], rect, pixels, pitch);
	}
	return real_SDL_LockTexture(texture, rect, pixels, pitch); // doesn't matter
}
void SDL_UnlockTexture(SDL_Texture * texture) {
	if (texture==app.dummy.screens[0]) {
		return XGL_UnlockTexture(app.xgl.screens[0]);
	}
	else if (texture==app.dummy.screens[1]) {
		return XGL_UnlockTexture(app.xgl.screens[1]);
	}
	return real_SDL_UnlockTexture(texture); // doesn't matter
}
int SDL_UpdateTexture(SDL_Texture * texture, const SDL_Rect * rect, const void *pixels, int pitch) {
	if (texture==app.dummy.drastic_menu) {
		return XGL_UpdateTexture(app.xgl.drastic_menu, rect, pixels, pitch);
	}
	return real_SDL_UpdateTexture(texture, rect, pixels, pitch); // doesn't matter
}
void SDL_DestroyTexture(SDL_Texture *texture) {
	// if (texture==app.dummy.screens[0]) app.dummy.screens[0] = NULL;
	// else if (texture==app.dummy.screens[1]) app.dummy.screens[1] = NULL;
	// else if (texture==app.dummy.drastic_menu) app.dummy.drastic_menu = NULL;
	// real_SDL_DestroyTexture(texture);
	// no-op now
}

int SDL_PollEvent(SDL_Event* event) {
	while (1) {
		int result = Pad_pollEvent(event);
		if (!result) return result;
		
		if (Device_handleEvent(event)) continue;
		
		if (Pad_justPressed(PAD_MENU)) {
			continue;
		}
		
		if (Pad_justReleased(PAD_MENU)) {
			App_menu();
			continue;
		}

		if (Pad_justPressed(PAD_BACK)) {
			Pad_consume(PAD_BACK);
			app.fast_forward = !app.fast_forward;
			if (app.fast_forward) Device_mute(1);
			else Device_mute(0);
		}
		
		// LOG_event(event);
		switch (event->type) {
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_FINGERMOTION: {
				// TODO: maybe this has something to do with the dummy 2d renderer?
				int x = raw_x / 2; // drastic is expecting SCREEN_WIDTH/2?
				int y = raw_y; // but just SCREEN_HEIGHT?
				if (settings.scale == SCALE_NATIVE) {
					int sw = SCREEN_WIDTH / 2; // drastic is thinking in half-widths?
					int sh = SCREEN_HEIGHT;

				    int w = DS_WIDTH; // drastic is thinking in half-widths?
				    int h = DS_HEIGHT * 2;
					
				    int ox = (sw - w) / 2; // centered
				    int oy = 0;
					
				    x = ((x - ox) * sw)  / w;
				    y = ((y - oy) * sh) / h;

				    if (x < 0) x = 0;
				    else if (x >= sw) x = sw - 1;

				    if (y < 0) y = 0;
				    else if (y >= sh) y = sh - 1;
				}
				event->tfinger.x = x;
				event->tfinger.y = y;
			} break;
		}
		
		return result;
	}
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) {
	raw_vol(0);
	int result = real_SDL_OpenAudio(desired, obtained);
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
	if (fmt && strcmp(fmt, "%s%cbackup%c%s.dsv")==0) return real__snprintf_chk(s, maxlen, flag, slen, USERDATA_PATH "/saves/%s.sram", app.game_name);

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
		
		if (bios && strncmp(bios, "nds_",4)==0) return real__sprintf_chk(s, flag, slen, BIOS_PATH "/%s", bios);
	}
	if (fmt && strcmp(fmt, "%s%cmicrophone%c%s.wav")==0) {
		va_list ap;
		va_start(ap, fmt);
		va_arg(ap, const char*); va_arg(ap, int); va_arg(ap, int); // skip %s %c %c
		const char* name = va_arg(ap, const char*);	// %s
		va_end(ap);
		
		char path[MAX_PATH];
		sprintf(path, MICS_PATH "/%s.wav", name);
		if (name && exists(path)) return real__sprintf_chk(s, flag, slen, MICS_PATH "/%s.wav", name);
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
	
	// hook SDL functions
	real_SDL_Init = dlsym(RTLD_NEXT, "SDL_Init");
	
	real_SDL_CreateWindow = dlsym(RTLD_NEXT, "SDL_CreateWindow");
	real_SDL_SetWindowSize = dlsym(RTLD_NEXT, "SDL_SetWindowSize");
	
	real_SDL_CreateRenderer = dlsym(RTLD_NEXT, "SDL_CreateRenderer");
	real_SDL_RenderSetLogicalSize = dlsym(RTLD_NEXT, "SDL_RenderSetLogicalSize");
	real_SDL_RenderClear = dlsym(RTLD_NEXT, "SDL_RenderClear");
	real_SDL_RenderCopy = dlsym(RTLD_NEXT, "SDL_RenderCopy");
	real_SDL_RenderPresent = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	
	real_SDL_CreateTexture = dlsym(RTLD_NEXT, "SDL_CreateTexture");
	real_SDL_LockTexture = dlsym(RTLD_NEXT, "SDL_LockTexture");
	real_SDL_UnlockTexture = dlsym(RTLD_NEXT, "SDL_UnlockTexture");
	real_SDL_UpdateTexture = dlsym(RTLD_NEXT, "SDL_UpdateTexture");
	real_SDL_DestroyTexture = dlsym(RTLD_NEXT, "SDL_DestroyTexture");
	
	real_SDL_PollEvent = dlsym(RTLD_NEXT, "SDL_PollEvent");
	
	real_SDL_OpenAudio = dlsym(RTLD_NEXT, "SDL_OpenAudio");
	real_SDL_Delay = dlsym(RTLD_NEXT, "SDL_Delay");
	
	app.base = find_exe_base();
	
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
	hook();
	drastic_main = main;
	return real__libc_start_main(override_args, argc, ubp_av, init, fini, rtld_fini, stack_end);
}
