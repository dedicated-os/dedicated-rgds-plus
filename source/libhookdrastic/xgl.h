#ifndef XGL_H
#define XGL_H

#include <SDL2/SDL.h>

typedef enum XGL_TextureAccess {
	XGL_TEXTUREACCESS_STATIC,
	XGL_TEXTUREACCESS_STREAMING
} XGL_TextureAccess;

typedef struct XGL_Renderer XGL_Renderer;
typedef struct XGL_Shader XGL_Shader;
typedef struct XGL_Texture XGL_Texture;

int XGL_Init(void);
void XGL_Quit(void);

XGL_Renderer *XGL_CreateRenderer(SDL_Window *window, SDL_GLContext context);
void XGL_DestroyRenderer(XGL_Renderer *renderer);
int XGL_GetRendererOutputSize(XGL_Renderer *renderer, int *w, int *h);

XGL_Texture *XGL_CreateTexture(XGL_Renderer *renderer, Uint32 format, int access, int w, int h);
XGL_Texture *XGL_CreateTextureFromSurface(XGL_Renderer *renderer, SDL_Surface *surface);
int XGL_QueryTexture(XGL_Texture *texture, Uint32 *format, int *access, int *w, int *h);
void XGL_DestroyTexture(XGL_Texture *texture);
int XGL_SetTextureBlendMode(XGL_Texture *texture, SDL_BlendMode blend_mode);
int XGL_SetTextureColorMod(XGL_Texture *texture, Uint8 r, Uint8 g, Uint8 b);
int XGL_SetTextureAlphaMod(XGL_Texture *texture, Uint8 alpha);
int XGL_UpdateTexture(XGL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
int XGL_LockTexture(XGL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch);
void XGL_UnlockTexture(XGL_Texture *texture);

XGL_Shader *XGL_CreateShader(XGL_Renderer *renderer, const char *vertex_source, const char *fragment_source);
XGL_Shader *XGL_CreateLinearShader(XGL_Renderer *renderer);
XGL_Shader *XGL_CreateSharpShader(XGL_Renderer *renderer);
XGL_Shader *XGL_CreateGridShader(XGL_Renderer *renderer);
void XGL_DestroyShader(XGL_Shader *shader);
XGL_Shader *XGL_GetDefaultTextureShader(XGL_Renderer *renderer);
int XGL_SetRenderShader(XGL_Renderer *renderer, XGL_Shader *shader);

int XGL_SetRenderDrawColor(XGL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int XGL_RenderClear(XGL_Renderer *renderer);
int XGL_RenderFillRect(XGL_Renderer *renderer, const SDL_Rect *rect);
int XGL_RenderFillRects(XGL_Renderer *renderer, const SDL_Rect *rects, int count);
int XGL_RenderCopy(XGL_Renderer *renderer, XGL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dst);
int XGL_RenderReadPixels(XGL_Renderer *renderer, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch);
void XGL_RenderPresent(XGL_Renderer *renderer);

#endif
