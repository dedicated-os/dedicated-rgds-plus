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

#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_opengles2.h>
#include "xgl.h"

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

#define BAT_PATH 		"/sys/class/power_supply/battery/"
#define USB_PATH 		"/sys/class/power_supply/usb/"
#define CPU_PATH		"/sys/devices/system/cpu/cpu0/cpufreq/"

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
#define LIGHT_TRIAD 	0xbb,0xbb,0xbb
#define DARK_TRIAD 		0x33,0x33,0x33
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
#define JOY_MENU	13

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

#define SCAN_BACK	270
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
		XGL_Texture *preview[SCREEN_COUNT];
		XGL_Texture *drastic_menu;
		XGL_Texture *icons;
		XGL_Shader *shader;
	} xgl;
	
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
	
	int bat;
	int usb;
	
	int synced;
	int menu;
	int osd;
	int capture;
	int fast_forward;
} app;

static int osd_at;

static int raw_x,raw_y;


// --------------------------------------------
// raw HMI controls
// --------------------------------------------

#define BL1_PATH "/sys/class/backlight/backlight1/brightness"
#define BL2_PATH "/sys/class/backlight/backlight/brightness"

static void raw_vol(int value) {
	char cmd[256];
	sprintf(cmd, "tinymix set 14 %i %i", value, value);
	system(cmd);
}
static void raw_bri(int value) {
	char cmd[256];

	sprintf(cmd, "echo %i > %s", value, BL1_PATH);
	system(cmd);

	sprintf(cmd, "echo %i > %s", value, BL2_PATH);
	system(cmd);
}
static void raw_led(int enable) {
	char cmd[256];
	sprintf(cmd, "led %s", enable ? "on" : "off");
	system(cmd);
}

// --------------------------------------------
// settings
// --------------------------------------------

static struct {
	int version;
	int volume;		// 0-20
	int brightness;	// 0 - 10
	char game[MAX_PATH];
} settings = {
	.version = 0,
	.volume = 6,
	.brightness = 3,
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
	char cmd[MAX_FILE];
	static const uint8_t raw[21] = {
		  0, // mute
		120, // 0
		131, // 11
		138, // 7
		143, // 5
		147, // 4
		151, // 4
		154, // 3
		157, // 3
		160, // 3
		162, // 2
		164, // 2
		166, // 2
		168, // 2
		170, // 2
		172, // 2
		174, // 2
		176, // 2
		178, // 2
		179, // 2
		180, // 1
	};
	
	raw_vol(raw[value]);
	settings.volume = value;
}
static void Settings_setBrightness(int value) {
	char cmd[MAX_FILE];
	static const uint8_t raw[11] = {
		  1, // 0
		  8, // 8
		 16, // 8
		 32, // 16
		 48, // 16
		 72, // 24
		 96, // 24
		128, // 32
		160, // 32
		192, // 32
		255, // 64
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

// --------------------------------------------
// support
// --------------------------------------------

static inline int getInt(int f) {
	if (f<0) return 0;

	char b[32];
	int n = pread(f, b, sizeof(b) - 1, 0);
	if (n<=0) return 0;

	b[n] = '\0';
	return atoi(b);
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
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%, ",
	.tile_width = 36,
	.tile_height = 36,
	.offset_x = -2,
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
		['&'] = 36,
		[' '] = 12,
	},
	.kern_pairs = {
		['A']['T'] = -6,
		['T']['A'] = -6,
		['A']['V'] = -6,
		['V']['A'] = -6,
		['A']['Y'] = -6,
		['Y']['A'] = -6,
		['T']['-'] = -6,
		['-']['T'] = -6,
	},
};

Font* font24 = &(Font){
	.name = "font-dedicated-24.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%, ",
	.tile_width = 24,
	.tile_height = 24,
	.offset_x = -1,
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
		[' '] = 8,
		['%'] = 21,
	},
	.kern_pairs = {
		['A']['T'] = -4,
		['T']['A'] = -4,
		['A']['V'] = -4,
		['V']['A'] = -4,
		['T']['-'] = -4,
		['-']['T'] = -4,
	},
};

Font* font18 = &(Font){
	.name = "font-dedicated-18.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%, ",
	.tile_width = 18,
	.tile_height = 18,
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
		[' '] = 6,
	},
	.kern_pairs = {
		['A']['T'] = -3,
		['T']['A'] = -3,
		['A']['V'] = -3,
		['V']['A'] = -3,
		['T']['-'] = -3,
		['-']['T'] = -3,
	},
};

Font* font16 = &(Font){
	.name = "font-dedicated-16.png",
	.charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-'!?&0123456789/$+%, ",
	.tile_width = 16,
	.tile_height = 16,
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
		[' '] = 4,
	},
	.kern_pairs = {
		['A']['T'] = -2,
		['T']['A'] = -2,
		['A']['V'] = -2,
		['V']['A'] = -2,
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
	ow -= font->offset_x; // reverse pad?
	
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
// button repeater
// --------------------------------------------

#define DEADZONE 12000

static int Repeater_fakeHatEvent(SDL_Event* event, int value) {
	SDL_memset(event, 0, sizeof(*event));

	event->type = SDL_JOYHATMOTION;
	event->jhat.type = SDL_JOYHATMOTION;
	event->jhat.which = 0;
	event->jhat.hat = 0;
	event->jhat.value = value;
	event->jhat.timestamp = SDL_GetTicks();

	return 1;
}

static int Repeater_pollEvent(SDL_Event* event) {
	static int ax = 0;
	static int ay = 0;
	static int last_pad = HAT_NEUTRAL;
	
	int result = real_SDL_PollEvent(event);
	if (!result) return result;
	
	if (event->type==SDL_JOYAXISMOTION) {
		int lx = ax;
		int ly = ay;
		int pad = last_pad;

		if (event->jaxis.axis==AXIS_LX) ax = event->jaxis.value;
		else if (event->jaxis.axis==AXIS_LY) ay = event->jaxis.value;

		if (ax!=lx) {
			if (ax>DEADZONE) {
				pad |= HAT_RIGHT;
				pad &= ~HAT_LEFT;
			}
			else if (ax<-DEADZONE) {
				pad |= HAT_LEFT;
				pad &= ~HAT_RIGHT;
			}
			else {
				pad &= ~(HAT_LEFT | HAT_RIGHT);
			}
		}
		if (ay!=ly) {
			if (ay>DEADZONE) {
				pad |= HAT_DOWN;
				pad &= ~HAT_UP;
			}
			else if (ay<-DEADZONE) {
				pad |= HAT_UP;
				pad &= ~HAT_DOWN;
			}
			else {
				pad &= ~(HAT_UP | HAT_DOWN);
			}
		}

		if (pad!=last_pad) {
			last_pad = pad;
			Repeater_fakeHatEvent(event, pad);
		}
	}
	
	return result;
}

// --------------------------------------------
// device features
// --------------------------------------------
static void App_save(void);

static void Device_mute(int mute) {
	if (mute) raw_vol(0);
	else Settings_setVolume(settings.volume);
}
static void Device_sleep(void) {
	raw_vol(0);
	raw_bri(0);
	raw_led(1);
	system("screen off");
	
	Settings_save();
	drastic_audio_pause(1);
	
	App_save();
	drastic_await_save();
	
	#define POWEROFF_TIMEOUT 2 * 60 * 1000 // two minutes
	int slept_at = SDL_GetTicks();
	int asleep = 1;
	SDL_Event event;
	while (asleep) {
		while (asleep && real_SDL_PollEvent(&event)) {
			if (event.type==SDL_KEYUP) {
				if (event.key.keysym.scancode==SCAN_OPEN) {
					asleep = 0;
					break;
				}
			}
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
	
	system("screen on");
	Settings_setBrightness(settings.brightness);
	Settings_setVolume(settings.volume);
	raw_led(0);
}
static void Device_poweroff(void) {
	raw_vol(0);
	App_save();
	unlink("/tmp/exec_loop");
	raw_bri(0);
	raw_led(1);
	system("screen off");
	drastic_quit();
}
#define SLEEP_TIMEOUT (2 * 60 * 1000) // two minutes
#define POWER_TIMEOUT 1000 // one second
static int power_at = 0;
static void Device_tick(void) {
	// must be called outside of Device_handleEvent because
	// the power key doesn't fire repeat events so otherwise
	// nothing triggers this check
	if (power_at && SDL_GetTicks()-power_at>=POWER_TIMEOUT) {
		Device_poweroff();
	}
}
static int Device_handleEvent(SDL_Event* event) {
	if (event->type==SDL_KEYDOWN) {
		if (event->key.keysym.scancode==SCAN_CLOSE) {
			Device_sleep();
			return 1;
		}
		else if (event->key.keysym.scancode==SCAN_POWER && event->key.repeat==0) {
			power_at = SDL_GetTicks();
			return 1;
		}
	}
	else if (event->type==SDL_KEYUP) {
		if (event->key.keysym.scancode==SCAN_POWER) {
			power_at = 0;
			return 1;
		}
	}
	
	return 0;
}

// --------------------------------------------
// custom menu
// --------------------------------------------
static void App_quit(void);

static void App_load(void) {
	drastic_load_state(AUTO_SLOT);
}
static void App_save(void) {
	// App_screenshot(app.current, 0, SNAP_SAVE);
	// App_screenshot(app.current, 1, SNAP_SAVE);
	drastic_save_state(AUTO_SLOT);
}
static void App_reset(void) {
	drastic_audio_pause(0);
	puts("[LOAD] reset requested");
	loader.state = LOADER_REQUESTED;
	loader.after = LOADER_RESET;
}

static int App_sort(const void* a, const void* b) {
	const Entry *i = (const Entry *)a;
	const Entry *j = (const Entry *)b;
	return compareNatural(i->name, j->name);
}
static void App_empty(void) {
	// TODO: 
	
	sleep(5);
	App_quit();
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

static void App_preload(void) {
	SDL_Log("App_preload");
	
	// before even drastic loads
	// this is drastic setup logic
	
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
		SDL_GLContext *context = SDL_GL_CreateContext(window);
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
	app.xgl.shader = XGL_CreateSharpShader(renderer); // XGL_CreateSharpShader_Original(renderer);
	app.xgl.drastic_menu = XGL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, XGL_TEXTUREACCESS_STREAMING, 800,480);
	app.dummy.drastic_menu = real_SDL_CreateTexture(app.dummy.renderers[0], SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, 800,480);
		

	SDL_Log("video driver: %s", SDL_GetCurrentVideoDriver());

	app.bat = open(BAT_PATH "capacity", O_RDONLY);
	app.usb = open(USB_PATH "online", O_RDONLY);
}
static void App_quit(void) {
	SDL_Log("App_quit");
	
	free(app.items);
	
	if (app.xgl.icons) 		XGL_DestroyTexture(app.xgl.icons);
	if (app.xgl.preview[0]) XGL_DestroyTexture(app.xgl.preview[0]);
	if (app.xgl.preview[1]) XGL_DestroyTexture(app.xgl.preview[1]);
	
	close(app.bat);
	close(app.usb);
	
	Fonts_quit();
	
	SDL_DestroyTexture(app.dummy.drastic_menu);
	XGL_DestroyTexture(app.xgl.drastic_menu);
	XGL_DestroyShader(app.xgl.shader);
	
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
}

static int tmp_toggle = 0;
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
	
	int s = 2;
	SDL_Rect rect = {0,0,DS_WIDTH*s,DS_HEIGHT*s};
	rect.x = (SCREEN_WIDTH - rect.w) / 2;
	rect.y = (SCREEN_HEIGHT - rect.h);
	
	for (int i=0; i<SCREEN_COUNT; i++) {
		XGL_Renderer *renderer = app.xgl.renderers[i];
		XGL_SetRenderShader(renderer, tmp_toggle ? XGL_GetDefaultTextureShader(renderer) : app.xgl.shader);
	    XGL_RenderCopy(renderer, app.xgl.screens[i], NULL, tmp_toggle ? &rect : NULL);
	    
		// TODO: tmp
		// Font_shadowText(renderer, font36, "HELLO WORLD", 16,16, WHITE_COLOR);
		
		XGL_RenderPresent(renderer);
		rect.y = 0;
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
			// Device_mute(0); // TODO: 
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
		int result = Repeater_pollEvent(event);
		if (!result) return result;
		
		if (Device_handleEvent(event)) continue;
	
		// LOG_event(event);
		switch (event->type) {
			case SDL_KEYUP: {
				// TODO: tmp poweroff
				if (event->key.keysym.scancode==SCAN_POWER) {
					unlink("/tmp/exec_loop");
					drastic_quit();
					// tmp_toggle = !tmp_toggle;
				}
			} break;
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_FINGERMOTION: {
				event->tfinger.x = raw_x / 2; // ???
				event->tfinger.y = raw_y;
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
	App_preload();
	char *new_argv[] = { argv[0], app.game_path, NULL };
	
	puts("[LOAD] resume requested");
	return drastic_main(2, new_argv, envp);
}

int __libc_start_main(int (*main)(int,char**,char**), int argc, char **ubp_av, void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
	hook();
	drastic_main = main;
	return real__libc_start_main(override_args, argc, ubp_av, init, fini, rtld_fini, stack_end);
}
