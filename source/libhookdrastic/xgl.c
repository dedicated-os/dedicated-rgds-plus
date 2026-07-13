#include "xgl.h"
#include <SDL2/SDL_opengles2.h>

typedef struct XGL_PixelFormat {
	GLenum gl_format;
	GLenum gl_type;
	int bytes_per_pixel;
} XGL_PixelFormat;

struct XGL_Renderer {
	SDL_Window *window;
	SDL_GLContext context;
	int output_w;
	int output_h;
	Uint8 draw_r;
	Uint8 draw_g;
	Uint8 draw_b;
	Uint8 draw_a;
	XGL_Shader *default_shader;
	XGL_Shader *current_shader;
	GLuint white_texture;
	GLuint quad_vbo;
};

struct XGL_Shader {
	XGL_Renderer *owner;
	GLuint program;
	SDL_bool linear_filter;
	GLint attr_position;
	GLint attr_texcoord;
	GLint uniform_view_size;
	GLint uniform_output_size;
	GLint uniform_texture;
	GLint uniform_color;
	GLint uniform_texture_size;
	GLint uniform_input_size;
	GLint uniform_source_pos;
};

struct XGL_Texture {
	XGL_Renderer *owner;
	GLuint id;
	int w;
	int h;
	Uint32 format;
	GLenum gl_format;
	GLenum gl_type;
	int bytes_per_pixel;
	int access;
	SDL_BlendMode blend_mode;
	Uint8 color_r;
	Uint8 color_g;
	Uint8 color_b;
	Uint8 alpha;
	void *lock_pixels;
	int lock_pitch;
	SDL_bool locked;
};

typedef struct XGL_Vertex {
	GLfloat x;
	GLfloat y;
	GLfloat u;
	GLfloat v;
} XGL_Vertex;

int XGL_Init(void) {
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) != 0) {
		return -1;
	}
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) != 0) {
		return -1;
	}
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0) != 0) {
		return -1;
	}
	return 0;
}
void XGL_Quit(void) {
}

static int XGL_SetCurrentRenderer(XGL_Renderer *renderer);

static const char *XGL_default_vertex_shader =
	"attribute vec2 a_position;"
	"attribute vec2 a_texcoord;"
	"uniform vec2 u_view_size;"
	"varying vec2 v_texcoord;"
	"void main() {"
		"vec2 zero_to_one = a_position / u_view_size;"
		"vec2 zero_to_two = zero_to_one * 2.0;"
		"vec2 clip = zero_to_two - 1.0;"
		"gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);"
		"v_texcoord = a_texcoord;"
	"}";

static const char *XGL_default_fragment_shader =
	"precision mediump float;"
	"varying vec2 v_texcoord;"
	"uniform sampler2D u_texture;"
	"uniform vec4 u_color;"
	"void main() {"
		"gl_FragColor = texture2D(u_texture, v_texcoord) * u_color;"
	"}";

// based on: https://github.com/Woohyun-Kang/Sharp-Shimmerless-Shader/blob/main/shaders_glsl/shaders/sharp-shimmerless.glsl
static const char *XGL_sharp_vertex_shader =
	"attribute vec2 a_position;"
	"attribute vec2 a_texcoord;"
	"uniform vec2 u_view_size;"
	"uniform vec2 u_output_size;"
	"uniform vec2 u_texture_size;"
	"uniform vec2 u_input_size;"
	"uniform vec2 u_source_pos;"
	"varying vec2 v_pixel;"
	"varying vec2 v_scale;"
	"varying vec2 v_invscale;"
	"void main() {"
		"vec2 zero_to_one = a_position / u_view_size;"
		"vec2 zero_to_two = zero_to_one * 2.0;"
		"vec2 clip = zero_to_two - 1.0;"
		"vec2 source_texel = a_texcoord * u_texture_size - u_source_pos;"
		"gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);"
		"v_pixel = source_texel * u_output_size / u_input_size;"
		"v_scale = u_output_size / u_input_size;"
		"v_invscale = u_input_size / u_output_size;"
	"}";

static const char *XGL_sharp_fragment_shader =
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
	"precision highp float;\n"
	"#else\n"
	"precision mediump float;\n"
	"#endif\n"
	"varying vec2 v_pixel;"
	"varying vec2 v_scale;"
	"varying vec2 v_invscale;"
	"uniform sampler2D u_texture;"
	"uniform vec4 u_color;"
	"uniform vec2 u_texture_size;"
	"void main() {"
		"vec2 pixel_tl = floor(v_pixel);"
		"vec2 pixel_br = ceil(v_pixel);"
		"vec2 texel_tl = floor(v_invscale * pixel_tl);"
		"vec2 texel_br = floor(v_invscale * pixel_br);"
		"vec2 mod_texel = texel_br + vec2(0.5);"
		"mod_texel -= (vec2(1.0) - step(texel_br, texel_tl)) * (v_scale * texel_br - pixel_tl);"
		"gl_FragColor = texture2D(u_texture, mod_texel / u_texture_size) * u_color;"
	"}";

static SDL_bool XGL_GetPixelFormat(Uint32 sdl_format, XGL_PixelFormat *format) {
	switch (sdl_format) {
		case SDL_PIXELFORMAT_ARGB8888:
			if (format) {
				format->gl_format = GL_BGRA_EXT;
				format->gl_type = GL_UNSIGNED_BYTE;
				format->bytes_per_pixel = 4;
			}
			return SDL_TRUE;
		case SDL_PIXELFORMAT_RGB565:
			if (format) {
				format->gl_format = GL_RGB;
				format->gl_type = GL_UNSIGNED_SHORT_5_6_5;
				format->bytes_per_pixel = 2;
			}
			return SDL_TRUE;
		default:
			return SDL_FALSE;
	}
}

static GLuint XGL_CompileShader(GLenum type, const char *source) {
	GLuint shader;
	GLint compiled;
	char log[512];

	shader = glCreateShader(type);
	if (!shader) {
		SDL_SetError("glCreateShader failed");
		return 0;
	}

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		SDL_SetError("shader compile failed: %s", log);
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static GLuint XGL_CreateProgram(const char *vertex_source, const char *fragment_source) {
	GLuint vertex_shader;
	GLuint fragment_shader;
	GLuint program;
	GLint linked;
	char log[512];

	vertex_shader = XGL_CompileShader(GL_VERTEX_SHADER, vertex_source);
	if (!vertex_shader) {
		return 0;
	}

	fragment_shader = XGL_CompileShader(GL_FRAGMENT_SHADER, fragment_source);
	if (!fragment_shader) {
		glDeleteShader(vertex_shader);
		return 0;
	}

	program = glCreateProgram();
	if (!program) {
		SDL_SetError("glCreateProgram failed");
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);
		return 0;
	}

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	if (!linked) {
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		SDL_SetError("program link failed: %s", log);
		glDeleteProgram(program);
		return 0;
	}

	return program;
}

XGL_Shader *XGL_CreateShader(XGL_Renderer *renderer, const char *vertex_source, const char *fragment_source) {
	XGL_Shader *shader;

	if (!renderer || !vertex_source || !fragment_source) {
		SDL_SetError("XGL_CreateShader: renderer and shader sources are required");
		return NULL;
	}
	if (XGL_SetCurrentRenderer(renderer) != 0) {
		return NULL;
	}

	shader = (XGL_Shader *)SDL_calloc(1, sizeof(*shader));
	if (!shader) {
		SDL_OutOfMemory();
		return NULL;
	}

	shader->owner = renderer;
	shader->program = XGL_CreateProgram(vertex_source, fragment_source);
	if (!shader->program) {
		SDL_free(shader);
		return NULL;
	}

	shader->attr_position = glGetAttribLocation(shader->program, "a_position");
	shader->attr_texcoord = glGetAttribLocation(shader->program, "a_texcoord");
	shader->uniform_view_size = glGetUniformLocation(shader->program, "u_view_size");
	shader->uniform_output_size = glGetUniformLocation(shader->program, "u_output_size");
	shader->uniform_texture = glGetUniformLocation(shader->program, "u_texture");
	shader->uniform_color = glGetUniformLocation(shader->program, "u_color");
	shader->uniform_texture_size = glGetUniformLocation(shader->program, "u_texture_size");
	shader->uniform_input_size = glGetUniformLocation(shader->program, "u_input_size");
	shader->uniform_source_pos = glGetUniformLocation(shader->program, "u_source_pos");
	if (shader->attr_position < 0 || shader->attr_texcoord < 0 || shader->uniform_view_size < 0 || shader->uniform_texture < 0 || shader->uniform_color < 0) {
		SDL_SetError("XGL_CreateShader: shader is missing required inputs");
		glDeleteProgram(shader->program);
		SDL_free(shader);
		return NULL;
	}

	return shader;
}

XGL_Shader *XGL_CreateLinearShader(XGL_Renderer *renderer) {
	XGL_Shader *shader = XGL_CreateShader(renderer, XGL_default_vertex_shader, XGL_default_fragment_shader);
	if (shader) {
		shader->linear_filter = SDL_TRUE;
	}
	return shader;
}

XGL_Shader *XGL_CreateSharpShader(XGL_Renderer *renderer) {
	XGL_Shader *shader = XGL_CreateShader(renderer, XGL_sharp_vertex_shader, XGL_sharp_fragment_shader);
	if (shader) {
		shader->linear_filter = SDL_TRUE;
	}
	return shader;
}

void XGL_DestroyShader(XGL_Shader *shader) {
	if (!shader) {
		return;
	}
	if (shader->owner && XGL_SetCurrentRenderer(shader->owner) == 0 && shader->program) {
		glDeleteProgram(shader->program);
	}
	SDL_free(shader);
}

XGL_Shader *XGL_GetDefaultTextureShader(XGL_Renderer *renderer) {
	if (!renderer) {
		return NULL;
	}
	return renderer->default_shader;
}

int XGL_SetRenderShader(XGL_Renderer *renderer, XGL_Shader *shader) {
	if (!renderer) {
		SDL_SetError("XGL_SetRenderShader: renderer is NULL");
		return -1;
	}
	renderer->current_shader = shader ? shader : renderer->default_shader;
	return 0;
}

static void XGL_ApplyTextureParameters(void) {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static int XGL_CreateWhiteTexture(XGL_Renderer *renderer) {
	const Uint32 white = 0xffffffff;

	glGenTextures(1, &renderer->white_texture);
	if (!renderer->white_texture) {
		SDL_SetError("glGenTextures failed");
		return -1;
	}

	glBindTexture(GL_TEXTURE_2D, renderer->white_texture);
	XGL_ApplyTextureParameters();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);

	return 0;
}

static int XGL_SetCurrentRenderer(XGL_Renderer *renderer) {
	if (!renderer) {
		SDL_SetError("XGL_SetCurrentRenderer: renderer is NULL");
		return -1;
	}
	if (SDL_GL_MakeCurrent(renderer->window, renderer->context) != 0) {
		return -1;
	}
	return 0;
}

static int XGL_UpdateRendererOutputSize(XGL_Renderer *renderer) {
	if (!renderer) {
		SDL_SetError("XGL_UpdateRendererOutputSize: renderer is NULL");
		return -1;
	}
	SDL_GL_GetDrawableSize(renderer->window, &renderer->output_w, &renderer->output_h);
	if (renderer->output_w <= 0 || renderer->output_h <= 0) {
		SDL_SetError("XGL_UpdateRendererOutputSize: invalid drawable size");
		return -1;
	}
	return 0;
}

XGL_Texture *XGL_CreateTexture(XGL_Renderer *renderer, Uint32 format, int access, int w, int h) {
	XGL_PixelFormat pixel_format;
	XGL_Texture *texture;

	if (!renderer) {
		SDL_SetError("XGL_CreateTexture: renderer is NULL");
		return NULL;
	}
	if (access != XGL_TEXTUREACCESS_STATIC && access != XGL_TEXTUREACCESS_STREAMING) {
		SDL_SetError("XGL_CreateTexture: unsupported texture access");
		return NULL;
	}
	if (w <= 0 || h <= 0) {
		SDL_SetError("XGL_CreateTexture: texture dimensions must be positive");
		return NULL;
	}
	if (!XGL_GetPixelFormat(format, &pixel_format)) {
		SDL_SetError("XGL: unsupported pixel format %s", SDL_GetPixelFormatName(format));
		return NULL;
	}
	if (XGL_SetCurrentRenderer(renderer) != 0) {
		return NULL;
	}

	texture = (XGL_Texture *)SDL_calloc(1, sizeof(*texture));
	if (!texture) {
		SDL_OutOfMemory();
		return NULL;
	}

	texture->owner = renderer;
	texture->w = w;
	texture->h = h;
	texture->format = format;
	texture->gl_format = pixel_format.gl_format;
	texture->gl_type = pixel_format.gl_type;
	texture->bytes_per_pixel = pixel_format.bytes_per_pixel;
	texture->access = access;
	texture->blend_mode = SDL_BLENDMODE_NONE;
	texture->color_r = 255;
	texture->color_g = 255;
	texture->color_b = 255;
	texture->alpha = 255;

	glGenTextures(1, &texture->id);
	if (!texture->id) {
		SDL_SetError("glGenTextures failed");
		SDL_free(texture);
		return NULL;
	}

	glBindTexture(GL_TEXTURE_2D, texture->id);
	XGL_ApplyTextureParameters();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, texture->gl_format, texture->w, texture->h, 0, texture->gl_format, texture->gl_type, NULL);

	return texture;
}

int XGL_QueryTexture(XGL_Texture *texture, Uint32 *format, int *access, int *w, int *h) {
	if (!texture) {
		SDL_SetError("XGL_QueryTexture: texture is NULL");
		return -1;
	}
	if (format) {
		*format = texture->format;
	}
	if (access) {
		*access = texture->access;
	}
	if (w) {
		*w = texture->w;
	}
	if (h) {
		*h = texture->h;
	}
	return 0;
}

void XGL_DestroyTexture(XGL_Texture *texture) {
	if (!texture) {
		return;
	}
	if (texture->owner && XGL_SetCurrentRenderer(texture->owner) == 0 && texture->id) {
		glDeleteTextures(1, &texture->id);
	}
	if (texture->lock_pixels) {
		SDL_free(texture->lock_pixels);
	}
	SDL_free(texture);
}

int XGL_SetTextureBlendMode(XGL_Texture *texture, SDL_BlendMode blend_mode) {
	if (!texture) {
		SDL_SetError("XGL_SetTextureBlendMode: texture is NULL");
		return -1;
	}
	if (blend_mode != SDL_BLENDMODE_NONE && blend_mode != SDL_BLENDMODE_BLEND) {
		SDL_SetError("XGL_SetTextureBlendMode: unsupported blend mode");
		return -1;
	}
	texture->blend_mode = blend_mode;
	return 0;
}

int XGL_SetTextureColorMod(XGL_Texture *texture, Uint8 r, Uint8 g, Uint8 b) {
	if (!texture) {
		SDL_SetError("XGL_SetTextureColorMod: texture is NULL");
		return -1;
	}
	texture->color_r = r;
	texture->color_g = g;
	texture->color_b = b;
	return 0;
}

int XGL_SetTextureAlphaMod(XGL_Texture *texture, Uint8 alpha) {
	if (!texture) {
		SDL_SetError("XGL_SetTextureAlphaMod: texture is NULL");
		return -1;
	}
	texture->alpha = alpha;
	return 0;
}

int XGL_UpdateTexture(XGL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
	SDL_Rect upload_rect;
	int tight_pitch;
	const Uint8 *upload_pixels;
	void *packed_pixels = NULL;

	if (!texture || !pixels) {
		SDL_SetError("XGL_UpdateTexture: texture and pixels are required");
		return -1;
	}
	if (!texture->owner) {
		SDL_SetError("XGL_UpdateTexture: texture has no owner renderer");
		return -1;
	}
	if (XGL_SetCurrentRenderer(texture->owner) != 0) {
		return -1;
	}

	if (rect) {
		upload_rect = *rect;
	} else {
		upload_rect.x = 0;
		upload_rect.y = 0;
		upload_rect.w = texture->w;
		upload_rect.h = texture->h;
	}
	if (upload_rect.x < 0 || upload_rect.y < 0 || upload_rect.w <= 0 || upload_rect.h <= 0 || upload_rect.x + upload_rect.w > texture->w || upload_rect.y + upload_rect.h > texture->h) {
		SDL_SetError("XGL_UpdateTexture: invalid update rectangle");
		return -1;
	}

	tight_pitch = upload_rect.w * texture->bytes_per_pixel;
	if (pitch < tight_pitch) {
		SDL_SetError("XGL_UpdateTexture: pitch is too small");
		return -1;
	}

	upload_pixels = (const Uint8 *)pixels;
	if (pitch != tight_pitch) {
		int y;
		Uint8 *dst;

		packed_pixels = SDL_malloc((size_t)tight_pitch * (size_t)upload_rect.h);
		if (!packed_pixels) {
			SDL_OutOfMemory();
			return -1;
		}
		dst = (Uint8 *)packed_pixels;
		for (y = 0; y < upload_rect.h; y++) {
			SDL_memcpy(dst + y * tight_pitch, upload_pixels + y * pitch, (size_t)tight_pitch);
		}
		upload_pixels = (const Uint8 *)packed_pixels;
	}

	glBindTexture(GL_TEXTURE_2D, texture->id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, upload_rect.x, upload_rect.y, upload_rect.w, upload_rect.h, texture->gl_format, texture->gl_type, upload_pixels);

	if (packed_pixels) {
		SDL_free(packed_pixels);
	}
	return 0;
}

XGL_Texture *XGL_CreateTextureFromSurface(XGL_Renderer *renderer, SDL_Surface *surface) {
	Uint32 format;
	SDL_Surface *converted = NULL;
	SDL_Surface *upload_surface;
	XGL_Texture *texture;
	SDL_bool has_alpha;

	if (!renderer || !surface) {
		SDL_SetError("XGL_CreateTextureFromSurface: renderer and surface are required");
		return NULL;
	}
	if (!surface->format) {
		SDL_SetError("XGL_CreateTextureFromSurface: surface has no pixel format");
		return NULL;
	}

	format = surface->format->format;
	has_alpha = SDL_ISPIXELFORMAT_ALPHA(format) ? SDL_TRUE : SDL_FALSE;
	if (!XGL_GetPixelFormat(format, NULL)) {
		format = SDL_PIXELFORMAT_ARGB8888;
		converted = SDL_ConvertSurfaceFormat(surface, format, 0);
		if (!converted) {
			return NULL;
		}
		upload_surface = converted;
	} else {
		upload_surface = surface;
	}

	texture = XGL_CreateTexture(renderer, format, XGL_TEXTUREACCESS_STATIC, upload_surface->w, upload_surface->h);
	if (!texture) {
		if (converted) {
			SDL_FreeSurface(converted);
		}
		return NULL;
	}

	if (XGL_UpdateTexture(texture, NULL, upload_surface->pixels, upload_surface->pitch) != 0) {
		XGL_DestroyTexture(texture);
		if (converted) {
			SDL_FreeSurface(converted);
		}
		return NULL;
	}

	if (converted) {
		SDL_FreeSurface(converted);
	}
	XGL_SetTextureBlendMode(texture, has_alpha ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	return texture;
}

int XGL_LockTexture(XGL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch) {
	size_t lock_size;

	if (!texture || !pixels || !pitch) {
		SDL_SetError("XGL_LockTexture: texture, pixels, and pitch are required");
		return -1;
	}
	if (texture->access != XGL_TEXTUREACCESS_STREAMING) {
		SDL_SetError("XGL_LockTexture: texture is not streaming");
		return -1;
	}
	if (texture->locked) {
		SDL_SetError("XGL_LockTexture: texture is already locked");
		return -1;
	}
	if (rect) {
		SDL_SetError("XGL_LockTexture: partial locks are not supported");
		return -1;
	}

	texture->lock_pitch = texture->w * texture->bytes_per_pixel;
	lock_size = (size_t)texture->lock_pitch * (size_t)texture->h;
	if (!texture->lock_pixels) {
		texture->lock_pixels = SDL_malloc(lock_size);
		if (!texture->lock_pixels) {
			SDL_OutOfMemory();
			return -1;
		}
	}

	texture->locked = SDL_TRUE;
	*pixels = texture->lock_pixels;
	*pitch = texture->lock_pitch;
	return 0;
}

void XGL_UnlockTexture(XGL_Texture *texture) {
	if (!texture || !texture->locked) {
		return;
	}

	XGL_UpdateTexture(texture, NULL, texture->lock_pixels, texture->lock_pitch);
	texture->locked = SDL_FALSE;
}

XGL_Renderer *XGL_CreateRenderer(SDL_Window *window, SDL_GLContext context) {
	XGL_Renderer *renderer;

	if (!window || !context) {
		SDL_SetError("XGL_CreateRenderer: window and context are required");
		return NULL;
	}

	renderer = (XGL_Renderer *)SDL_calloc(1, sizeof(*renderer));
	if (!renderer) {
		SDL_OutOfMemory();
		return NULL;
	}

	renderer->window = window;
	renderer->context = context;
	renderer->draw_r = 0;
	renderer->draw_g = 0;
	renderer->draw_b = 0;
	renderer->draw_a = 255;

	if (XGL_SetCurrentRenderer(renderer) != 0 || XGL_UpdateRendererOutputSize(renderer) != 0) {
		SDL_free(renderer);
		return NULL;
	}

	renderer->default_shader = XGL_CreateShader(renderer, XGL_default_vertex_shader, XGL_default_fragment_shader);
	if (!renderer->default_shader) {
		SDL_free(renderer);
		return NULL;
	}
	renderer->current_shader = renderer->default_shader;

	if (XGL_CreateWhiteTexture(renderer) != 0) {
		XGL_DestroyShader(renderer->default_shader);
		SDL_free(renderer);
		return NULL;
	}

	glGenBuffers(1, &renderer->quad_vbo);
	if (!renderer->quad_vbo) {
		SDL_SetError("glGenBuffers failed");
		glDeleteTextures(1, &renderer->white_texture);
		XGL_DestroyShader(renderer->default_shader);
		SDL_free(renderer);
		return NULL;
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glViewport(0, 0, renderer->output_w, renderer->output_h);

	return renderer;
}

void XGL_DestroyRenderer(XGL_Renderer *renderer) {
	if (!renderer) {
		return;
	}
	if (XGL_SetCurrentRenderer(renderer) == 0) {
		if (renderer->quad_vbo) {
			glDeleteBuffers(1, &renderer->quad_vbo);
		}
		if (renderer->white_texture) {
			glDeleteTextures(1, &renderer->white_texture);
		}
	}
	XGL_DestroyShader(renderer->default_shader);
	SDL_free(renderer);
}

int XGL_GetRendererOutputSize(XGL_Renderer *renderer, int *w, int *h) {
	if (XGL_UpdateRendererOutputSize(renderer) != 0) {
		return -1;
	}
	if (w) {
		*w = renderer->output_w;
	}
	if (h) {
		*h = renderer->output_h;
	}
	return 0;
}

int XGL_SetRenderDrawColor(XGL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	if (!renderer) {
		SDL_SetError("XGL_SetRenderDrawColor: renderer is NULL");
		return -1;
	}
	renderer->draw_r = r;
	renderer->draw_g = g;
	renderer->draw_b = b;
	renderer->draw_a = a;
	return 0;
}

int XGL_RenderClear(XGL_Renderer *renderer) {
	if (XGL_SetCurrentRenderer(renderer) != 0 || XGL_UpdateRendererOutputSize(renderer) != 0) {
		return -1;
	}
	glViewport(0, 0, renderer->output_w, renderer->output_h);
	glClearColor(renderer->draw_r / 255.0f, renderer->draw_g / 255.0f, renderer->draw_b / 255.0f, renderer->draw_a / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

int XGL_RenderFillRects(XGL_Renderer *renderer, const SDL_Rect *rects, int count) {
	int i;
	XGL_Shader *shader;

	if (count < 0) {
		SDL_SetError("XGL_RenderFillRects: count must be >= 0");
		return -1;
	}
	if (count == 0) {
		return 0;
	}
	if (!rects) {
		SDL_SetError("XGL_RenderFillRects: rects is NULL");
		return -1;
	}
	if (XGL_SetCurrentRenderer(renderer) != 0 || XGL_UpdateRendererOutputSize(renderer) != 0) {
		return -1;
	}

	shader = renderer->current_shader ? renderer->current_shader : renderer->default_shader;
	glViewport(0, 0, renderer->output_w, renderer->output_h);
	glUseProgram(shader->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, renderer->white_texture);
	glUniform1i(shader->uniform_texture, 0);
	glUniform2f(shader->uniform_view_size, (GLfloat)renderer->output_w, (GLfloat)renderer->output_h);
	if (shader->uniform_output_size >= 0) {
		glUniform2f(shader->uniform_output_size, (GLfloat)renderer->output_w, (GLfloat)renderer->output_h);
	}
	glUniform4f(shader->uniform_color, renderer->draw_r / 255.0f, renderer->draw_g / 255.0f, renderer->draw_b / 255.0f, renderer->draw_a / 255.0f);
	if (shader->uniform_texture_size >= 0) {
		glUniform2f(shader->uniform_texture_size, 1.0f, 1.0f);
	}
	if (shader->uniform_input_size >= 0) {
		glUniform2f(shader->uniform_input_size, 1.0f, 1.0f);
	}
	if (shader->uniform_source_pos >= 0) {
		glUniform2f(shader->uniform_source_pos, 0.0f, 0.0f);
	}

	if (renderer->draw_a < 255) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}

	glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vbo);
	glEnableVertexAttribArray(shader->attr_position);
	glEnableVertexAttribArray(shader->attr_texcoord);
	glVertexAttribPointer(shader->attr_position, 2, GL_FLOAT, GL_FALSE, sizeof(XGL_Vertex), (const GLvoid *)0);
	glVertexAttribPointer(shader->attr_texcoord, 2, GL_FLOAT, GL_FALSE, sizeof(XGL_Vertex), (const GLvoid *)(2 * sizeof(GLfloat)));

	for (i = 0; i < count; i++) {
		GLfloat x0 = (GLfloat)rects[i].x;
		GLfloat y0 = (GLfloat)rects[i].y;
		GLfloat x1 = (GLfloat)(rects[i].x + rects[i].w);
		GLfloat y1 = (GLfloat)(rects[i].y + rects[i].h);
		XGL_Vertex vertices[6] = {
			{ x0, y0, 0.0f, 0.0f },
			{ x1, y0, 1.0f, 0.0f },
			{ x0, y1, 0.0f, 1.0f },
			{ x1, y0, 1.0f, 0.0f },
			{ x1, y1, 1.0f, 1.0f },
			{ x0, y1, 0.0f, 1.0f }
		};

		if (rects[i].w <= 0 || rects[i].h <= 0) {
			continue;
		}

		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	glDisableVertexAttribArray(shader->attr_position);
	glDisableVertexAttribArray(shader->attr_texcoord);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return 0;
}

int XGL_RenderFillRect(XGL_Renderer *renderer, const SDL_Rect *rect) {
	SDL_Rect output_rect;

	if (rect) {
		return XGL_RenderFillRects(renderer, rect, 1);
	}
	if (XGL_GetRendererOutputSize(renderer, &output_rect.w, &output_rect.h) != 0) {
		return -1;
	}
	output_rect.x = 0;
	output_rect.y = 0;
	return XGL_RenderFillRects(renderer, &output_rect, 1);
}

int XGL_RenderCopy(XGL_Renderer *renderer, XGL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dst) {
	SDL_Rect src_rect;
	SDL_Rect dst_rect;
	XGL_Shader *shader;
	GLfloat x0;
	GLfloat y0;
	GLfloat x1;
	GLfloat y1;
	GLfloat u0;
	GLfloat v0;
	GLfloat u1;
	GLfloat v1;
	XGL_Vertex vertices[6];

	if (!texture) {
		SDL_SetError("XGL_RenderCopy: texture is NULL");
		return -1;
	}
	if (XGL_SetCurrentRenderer(renderer) != 0 || XGL_UpdateRendererOutputSize(renderer) != 0) {
		return -1;
	}
	shader = renderer->current_shader ? renderer->current_shader : renderer->default_shader;

	if (src) {
		src_rect = *src;
	} else {
		src_rect.x = 0;
		src_rect.y = 0;
		src_rect.w = texture->w;
		src_rect.h = texture->h;
	}
	if (src_rect.x < 0 || src_rect.y < 0 || src_rect.w <= 0 || src_rect.h <= 0 || src_rect.x + src_rect.w > texture->w || src_rect.y + src_rect.h > texture->h) {
		SDL_SetError("XGL_RenderCopy: invalid source rectangle");
		return -1;
	}

	if (dst) {
		dst_rect = *dst;
	} else {
		dst_rect.x = 0;
		dst_rect.y = 0;
		dst_rect.w = renderer->output_w;
		dst_rect.h = renderer->output_h;
	}
	if (dst_rect.w <= 0 || dst_rect.h <= 0) {
		return 0;
	}

	x0 = (GLfloat)dst_rect.x;
	y0 = (GLfloat)dst_rect.y;
	x1 = (GLfloat)(dst_rect.x + dst_rect.w);
	y1 = (GLfloat)(dst_rect.y + dst_rect.h);
	u0 = (GLfloat)src_rect.x / (GLfloat)texture->w;
	v0 = (GLfloat)src_rect.y / (GLfloat)texture->h;
	u1 = (GLfloat)(src_rect.x + src_rect.w) / (GLfloat)texture->w;
	v1 = (GLfloat)(src_rect.y + src_rect.h) / (GLfloat)texture->h;

	vertices[0] = (XGL_Vertex){ x0, y0, u0, v0 };
	vertices[1] = (XGL_Vertex){ x1, y0, u1, v0 };
	vertices[2] = (XGL_Vertex){ x0, y1, u0, v1 };
	vertices[3] = (XGL_Vertex){ x1, y0, u1, v0 };
	vertices[4] = (XGL_Vertex){ x1, y1, u1, v1 };
	vertices[5] = (XGL_Vertex){ x0, y1, u0, v1 };

	glViewport(0, 0, renderer->output_w, renderer->output_h);
	glUseProgram(shader->program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture->id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, shader->linear_filter ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, shader->linear_filter ? GL_LINEAR : GL_NEAREST);
	glUniform1i(shader->uniform_texture, 0);
	glUniform2f(shader->uniform_view_size, (GLfloat)renderer->output_w, (GLfloat)renderer->output_h);
	if (shader->uniform_output_size >= 0) {
		glUniform2f(shader->uniform_output_size, (GLfloat)dst_rect.w, (GLfloat)dst_rect.h);
	}
	glUniform4f(shader->uniform_color, texture->color_r / 255.0f, texture->color_g / 255.0f, texture->color_b / 255.0f, texture->alpha / 255.0f);
	if (shader->uniform_texture_size >= 0) {
		glUniform2f(shader->uniform_texture_size, (GLfloat)texture->w, (GLfloat)texture->h);
	}
	if (shader->uniform_input_size >= 0) {
		glUniform2f(shader->uniform_input_size, (GLfloat)src_rect.w, (GLfloat)src_rect.h);
	}
	if (shader->uniform_source_pos >= 0) {
		glUniform2f(shader->uniform_source_pos, (GLfloat)src_rect.x, (GLfloat)src_rect.y);
	}

	if (texture->blend_mode == SDL_BLENDMODE_BLEND || texture->alpha < 255) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}

	glBindBuffer(GL_ARRAY_BUFFER, renderer->quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
	glEnableVertexAttribArray(shader->attr_position);
	glEnableVertexAttribArray(shader->attr_texcoord);
	glVertexAttribPointer(shader->attr_position, 2, GL_FLOAT, GL_FALSE, sizeof(XGL_Vertex), (const GLvoid *)0);
	glVertexAttribPointer(shader->attr_texcoord, 2, GL_FLOAT, GL_FALSE, sizeof(XGL_Vertex), (const GLvoid *)(2 * sizeof(GLfloat)));
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(shader->attr_position);
	glDisableVertexAttribArray(shader->attr_texcoord);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return 0;
}

int XGL_RenderReadPixels(XGL_Renderer *renderer, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch) {
	SDL_Rect read_rect;
	SDL_PixelFormat *dst_format;
	Uint8 *read_pixels;
	int read_pitch;
	int min_pitch;
	int gl_y;
	int y;

	if (!pixels) {
		SDL_SetError("XGL_RenderReadPixels: pixels is NULL");
		return -1;
	}
	if (XGL_SetCurrentRenderer(renderer) != 0 || XGL_UpdateRendererOutputSize(renderer) != 0) {
		return -1;
	}

	if (rect) {
		read_rect = *rect;
	} else {
		read_rect.x = 0;
		read_rect.y = 0;
		read_rect.w = renderer->output_w;
		read_rect.h = renderer->output_h;
	}
	if (read_rect.x < 0 || read_rect.y < 0 || read_rect.w <= 0 || read_rect.h <= 0 || read_rect.x + read_rect.w > renderer->output_w || read_rect.y + read_rect.h > renderer->output_h) {
		SDL_SetError("XGL_RenderReadPixels: invalid read rectangle");
		return -1;
	}

	dst_format = SDL_AllocFormat(format);
	if (!dst_format) {
		return -1;
	}
	min_pitch = read_rect.w * dst_format->BytesPerPixel;
	SDL_FreeFormat(dst_format);
	if (pitch < min_pitch) {
		SDL_SetError("XGL_RenderReadPixels: pitch is too small");
		return -1;
	}

	read_pitch = read_rect.w * 4;
	read_pixels = (Uint8 *)SDL_malloc((size_t)read_pitch * (size_t)read_rect.h);
	if (!read_pixels) {
		SDL_OutOfMemory();
		return -1;
	}

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	gl_y = renderer->output_h - read_rect.y - read_rect.h;
	glReadPixels(read_rect.x, gl_y, read_rect.w, read_rect.h, GL_RGBA, GL_UNSIGNED_BYTE, read_pixels);

	for (y = 0; y < read_rect.h; y++) {
		Uint8 *src_row = read_pixels + (size_t)(read_rect.h - 1 - y) * (size_t)read_pitch;
		Uint8 *dst_row = (Uint8 *)pixels + (size_t)y * (size_t)pitch;
		if (SDL_ConvertPixels(read_rect.w, 1, SDL_PIXELFORMAT_RGBA32, src_row, read_pitch, format, dst_row, pitch) != 0) {
			SDL_free(read_pixels);
			return -1;
		}
	}

	SDL_free(read_pixels);
	return 0;
}

void XGL_RenderPresent(XGL_Renderer *renderer) {
	if (XGL_SetCurrentRenderer(renderer) == 0) {
		SDL_GL_SwapWindow(renderer->window);
	}
}
