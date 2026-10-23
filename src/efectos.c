/*
 * efectos.c - efectos de imagen de Camara VS sobre Direct3D 8: blur de tribunas estilo Virtua
 * Striker, anclado a la cancha (sirve igual para estadios del juego y de Kitserver).
 *
 * Como funciona
 *   El juego dibuja el mundo en una textura de escena y despues el HUD con vertices ya
 *   transformados (FVF XYZRHW). Justo antes del primer dibujo del HUD:
 *   1. Se achica la escena a 1/2, 1/4 y 1/8 (4 muestras sumadas por paso = blur de caja).
 *   2. Mascara por metros reales: cajas anidadas sobre la cancha (105 x 68 m, centro en el origen)
 *      dibujadas contra el depth del juego. Con paridad en el stencil se sabe, por pixel, si la
 *      superficie visible esta adentro de cada caja; el stencil termina contando en cuantas cajas
 *      esta. Funciona con vertex shaders (no hace falta leer el Z, que D3D8 no deja).
 *   3. Por cada anillo se mezcla la escena desenfocada con mas fuerza cuanto mas afuera.
 *   El HUD se dibuja despues, nitido.
 *
 *   Ganchos: el device se pide a Kitserver (kload.dll); sus funciones se enganchan con hotpatch
 *   en la d3d8.dll de Windows o cambiando el vtable en dgVoodoo (ver efectos_iniciar()). Reset y
 *   CreateTexture ya los engancha Kitserver: se sigue su cadena hasta la funcion real.
 *
 *   Diagnostico (diagnostico=1 en el ini): con la tecla de captura se anota un cuadro completo.
 *
 * No hay d3d8.h en los SDK actuales: se declaran solo los indices y constantes que se usan.
 * Ejes del juego: x a lo largo, y vertical (negativo = arriba), z a lo ancho. La fisica usa 256
 * unidades por metro, pero el mundo se dibuja a 1/5 de esa escala: 51,2 unidades por metro.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camara_vs.h"

extern char g_log[MAX_PATH];

#define M 51.2f   /* unidades de dibujo por metro */

/* ---------------------------------------------------------------- D3D8 minimo */

typedef struct { UINT formato, tipo, uso, pool, tam, multisample, ancho, alto; } DescSuperficie8;

/* IDirect3DDevice8 */
enum {
    DEV_GetDeviceCaps = 7, DEV_Reset = 14, DEV_GetBackBuffer = 16, DEV_CreateTexture = 20,
    DEV_CopyRects = 28, DEV_SetRenderTarget = 31, DEV_GetRenderTarget = 32,
    DEV_GetDepthStencilSurface = 33, DEV_BeginScene = 34, DEV_EndScene = 35, DEV_Clear = 36,
    DEV_SetTransform = 37, DEV_SetViewport = 40, DEV_SetRenderState = 50, DEV_ApplyStateBlock = 54,
    DEV_CaptureStateBlock = 55, DEV_DeleteStateBlock = 56, DEV_CreateStateBlock = 57,
    DEV_SetTexture = 61, DEV_SetTextureStageState = 63, DEV_DrawPrimitive = 70,
    DEV_DrawIndexedPrimitive = 71, DEV_DrawPrimitiveUP = 72, DEV_DrawIndexedPrimitiveUP = 73,
    DEV_SetVertexShader = 76, DEV_SetVertexShaderConstant = 79, DEV_SetStreamSource = 83,
    DEV_CreatePixelShader = 87, DEV_SetPixelShader = 88, DEV_DeletePixelShader = 90,
    DEV_SetPixelShaderConstant = 91,
    DEV_N = 97
};
/* IDirect3DSurface8 / IDirect3DTexture8 */
enum { OBJ_Release = 2, SUP_GetContainer = 7, SUP_GetDesc = 8, TEX_GetSurfaceLevel = 15, TEX_LockRect = 16,
       TEX_UnlockRect = 17 };

#define VT(obj) (*(void ***)(obj))
#define LLAMAR(tipo, obj, idx) ((tipo)VT(obj)[idx])

typedef ULONG   (WINAPI *Release_t)(void *);
typedef HRESULT (WINAPI *GetDeviceCaps_t)(void *, DWORD *);
typedef HRESULT (WINAPI *GetBackBuffer_t)(void *, UINT, UINT, void **);
typedef HRESULT (WINAPI *GetDesc_t)(void *, DescSuperficie8 *);
typedef HRESULT (WINAPI *GetContainer_t)(void *, const GUID *, void **);
typedef HRESULT (WINAPI *GetSurfaceLevel_t)(void *, UINT, void **);
typedef struct { INT pitch; void *bits; } RectBloqueado8;
typedef HRESULT (WINAPI *LockRect_t)(void *, UINT, RectBloqueado8 *, const RECT *, DWORD);
typedef HRESULT (WINAPI *UnlockRect_t)(void *, UINT);
typedef HRESULT (WINAPI *Reset_t)(void *, void *);
typedef HRESULT (WINAPI *CreateTexture_t)(void *, UINT, UINT, UINT, DWORD, UINT, UINT, void **);
typedef HRESULT (WINAPI *CopyRects_t)(void *, void *, const RECT *, UINT, void *, const POINT *);
typedef HRESULT (WINAPI *SetRenderTarget_t)(void *, void *, void *);
typedef HRESULT (WINAPI *GetSurface_t)(void *, void **);
typedef HRESULT (WINAPI *BeginScene_t)(void *);
typedef HRESULT (WINAPI *EndScene_t)(void *);
typedef HRESULT (WINAPI *Clear_t)(void *, DWORD, const void *, DWORD, DWORD, float, DWORD);
typedef HRESULT (WINAPI *SetTransform_t)(void *, DWORD, const float *);
typedef HRESULT (WINAPI *SetViewport_t)(void *, const DWORD *);
typedef HRESULT (WINAPI *SetRenderState_t)(void *, DWORD, DWORD);
typedef HRESULT (WINAPI *StateBlock_t)(void *, DWORD);
typedef HRESULT (WINAPI *CreateStateBlock_t)(void *, DWORD, DWORD *);
typedef HRESULT (WINAPI *SetTexture_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *SetTextureStageState_t)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *DrawPrimitive_t)(void *, DWORD, UINT, UINT);
typedef HRESULT (WINAPI *DrawIndexedPrimitive_t)(void *, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (WINAPI *DrawPrimitiveUP_t)(void *, DWORD, UINT, const void *, UINT);
typedef HRESULT (WINAPI *DrawIndexedPrimitiveUP_t)(void *, DWORD, UINT, UINT, UINT, const void *, DWORD, const void *, UINT);
typedef HRESULT (WINAPI *SetVertexShader_t)(void *, DWORD);
typedef HRESULT (WINAPI *SetVertexShaderConstant_t)(void *, DWORD, const void *, DWORD);
typedef HRESULT (WINAPI *SetStreamSource_t)(void *, UINT, void *, UINT);
typedef HRESULT (WINAPI *SetPixelShader_t)(void *, DWORD);
typedef HRESULT (WINAPI *CreatePixelShader_t)(void *, const DWORD *, DWORD *);
typedef HRESULT (WINAPI *DeletePixelShader_t)(void *, DWORD);
typedef HRESULT (WINAPI *SetPixelShaderConstant_t)(void *, DWORD, const void *, DWORD);

static const GUID IID_Textura8 = {0xe4cdd575, 0x2866, 0x4f01, {0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58}};

/* Render states */
#define RS_ZENABLE 7
#define RS_FILLMODE 8
#define RS_ZWRITEENABLE 14
#define RS_ALPHATESTENABLE 15
#define RS_SRCBLEND 19
#define RS_DESTBLEND 20
#define RS_CULLMODE 22
#define RS_ZFUNC 23
#define RS_ALPHABLENDENABLE 27
#define RS_FOGENABLE 28
#define RS_FOGCOLOR 34
#define RS_STENCILENABLE 52
#define RS_STENCILFAIL 53
#define RS_STENCILZFAIL 54
#define RS_STENCILPASS 55
#define RS_STENCILFUNC 56
#define RS_STENCILREF 57
#define RS_STENCILMASK 58
#define RS_STENCILWRITEMASK 59
#define RS_TEXTUREFACTOR 60
#define RS_CLIPPING 136
#define RS_LIGHTING 137
#define RS_CLIPPLANEENABLE 152
#define RS_COLORWRITEENABLE 168
#define RS_BLENDOP 171
/* Texture stage states */
#define TSS_COLOROP 1
#define TSS_COLORARG1 2
#define TSS_COLORARG2 3
#define TSS_ALPHAOP 4
#define TSS_ALPHAARG1 5
#define TSS_ALPHAARG2 6
#define TSS_TEXCOORDINDEX 11
#define TSS_ADDRESSU 13
#define TSS_ADDRESSV 14
#define TSS_MAGFILTER 16
#define TSS_MINFILTER 17
#define TSS_MIPFILTER 18
#define TSS_TEXTURETRANSFORMFLAGS 24
/* Valores */
#define TOP_DISABLE 1
#define TOP_SELECTARG1 2
#define TOP_MODULATE 4
#define TA_DIFFUSE 0
#define TA_TEXTURE 2
#define TA_TFACTOR 3
#define BLEND_ZERO 1
#define BLEND_ONE 2
#define BLEND_SRCALPHA 5
#define BLEND_INVSRCALPHA 6
#define CMP_EQUAL 3
#define CMP_LESSEQUAL 4
#define CMP_ALWAYS 8
#define SOP_KEEP 1
#define SOP_ZERO 2
#define SOP_INVERT 6
#define SOP_INCR 7
#define CULL_NONE 1
#define TS_VIEW 2
#define TS_PROJECTION 3
#define TS_WORLD 256
#define FVF_XYZ 0x002
#define FVF_XYZRHW 0x004
#define FVF_TEX1 0x100
#define PT_TRIANGLELIST 4
#define PT_TRIANGLESTRIP 5
#define CLEAR_TARGET 1
#define CLEAR_STENCIL 4
#define USAGE_RENDERTARGET 1
#define POOL_DEFAULT 0
#define POOL_MANAGED 1
#define SBT_ALL 1

/* ---------------------------------------------------------------- ajustes */

#define N_CAJAS 6
static struct {
    int activo, bruma, color, ver_mascara, diagnostico, guia_fija;
    float saturacion, contraste, rgb[3];      /* color grading */
    int t_activar, t_captura, t_guia;
    float fuerza, bruma_fuerza;
    DWORD bruma_color;                        /* 0 = el color de niebla del juego */
    /* medio ancho de cada caja del lado lejano y del lado de la camara, y medio largo, en m */
    float lejos[N_CAJAS], cerca[N_CAJAS], fondo[N_CAJAS];
    float alfa[N_CAJAS], bruma_alfa[N_CAJAS]; /* mezcla por anillo, de adentro hacia afuera */
} E;
static char g_ini[MAX_PATH];

/* Anillos por defecto (lineas: laterales a 34 m, de gol a 52,5 m). Del lado lejano el blur
 * arranca tenue 7 m adentro de la cancha; del lado de la camara recien en la linea, para no
 * desenfocar a los jugadores cercanos; en los fondos desde la linea de gol (la red apenas). */
static const float LEJOS[N_CAJAS]  = {27.0f, 30.0f, 34.0f, 39.0f, 45.0f, 52.0f};
static const float CERCA[N_CAJAS]  = {34.0f, 38.0f, 42.0f, 47.0f, 53.0f, 60.0f};
static const float FONDO[N_CAJAS]  = {52.5f, 56.0f, 60.0f, 65.0f, 71.0f, 78.0f};
static const float ALFA[N_CAJAS]   = {0.15f, 0.30f, 0.45f, 0.60f, 0.80f, 1.00f};
/* Bruma: solo los anillos de afuera (tribunas altas), muy suave. */
static const float BRUMA[N_CAJAS]  = {0.0f, 0.0f, 0.05f, 0.10f, 0.16f, 0.24f};
#define ALTO_CAJA_M 80.0f

static void leer_lista(const char *clave, float *v, const float *def) {
    char s[256], d[256] = "", *p;
    int i;
    for (i = 0; i < N_CAJAS; i++) sprintf(d + strlen(d), "%s%g", i ? " " : "", def[i]);
    GetPrivateProfileStringA("efectos", clave, d, s, sizeof s, g_ini);
    p = s;
    for (i = 0; i < N_CAJAS; i++) {
        char *fin;
        v[i] = (float)strtod(p, &fin);
        if (fin == p || v[i] < 0.0f) v[i] = def[i];
        p = fin;
    }
}

static void leer_ajustes(void) {
    char s[64];
    E.activo = GetPrivateProfileIntA("efectos", "blur", 1, g_ini);
    E.bruma = GetPrivateProfileIntA("efectos", "bruma", 1, g_ini);
    E.color = GetPrivateProfileIntA("efectos", "color", 1, g_ini);
    GetPrivateProfileStringA("efectos", "color_saturacion", "1.12", s, sizeof s, g_ini);
    E.saturacion = (float)atof(s);
    GetPrivateProfileStringA("efectos", "color_contraste", "1.08", s, sizeof s, g_ini);
    E.contraste = (float)atof(s);
    GetPrivateProfileStringA("efectos", "color_rgb", "1 1 1", s, sizeof s, g_ini);
    if (sscanf(s, "%f %f %f", &E.rgb[0], &E.rgb[1], &E.rgb[2]) != 3) E.rgb[0] = E.rgb[1] = E.rgb[2] = 1.0f;
    E.ver_mascara = GetPrivateProfileIntA("efectos", "ver_mascara", 0, g_ini);
    E.diagnostico = GetPrivateProfileIntA("efectos", "diagnostico", 0, g_ini);
    GetPrivateProfileStringA("efectos", "fuerza", "1", s, sizeof s, g_ini);
    E.fuerza = (float)atof(s);
    GetPrivateProfileStringA("efectos", "bruma_fuerza", "1", s, sizeof s, g_ini);
    E.bruma_fuerza = (float)atof(s);
    GetPrivateProfileStringA("efectos", "bruma_color", "auto", s, sizeof s, g_ini);
    E.bruma_color = strncmp(s, "auto", 4) == 0 ? 0 : (DWORD)strtoul(s, NULL, 16) & 0x00FFFFFF;
    GetPrivateProfileStringA("efectos", "activar", "0x71", s, sizeof s, g_ini);
    E.t_activar = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "captura", "0x7B", s, sizeof s, g_ini);
    E.t_captura = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "guia", "0x70", s, sizeof s, g_ini);
    E.t_guia = (int)strtol(s, NULL, 0);
    E.guia_fija = GetPrivateProfileIntA("efectos", "guia_fija", 0, g_ini);
    leer_lista("lado_lejano", E.lejos, LEJOS);
    leer_lista("lado_camara", E.cerca, CERCA);
    leer_lista("fondos", E.fondo, FONDO);
    leer_lista("mezcla", E.alfa, ALFA);
    leer_lista("bruma_mezcla", E.bruma_alfa, BRUMA);
}

/* ---------------------------------------------------------------- estado */

static void *g_orig[DEV_N];         /* funciones originales */
static void *g_destino[DEV_N];      /* a donde salta cada stub: gancho u original */
static volatile LONG g_cuenta[DEV_N];
static BYTE *g_stubs;
static CreateTexture_t g_crear_textura;   /* CreateTexture real de d3d8.dll */

/* Estado que se sigue del juego durante el cuadro */
static struct {
    DWORD vs;                 /* vertex shader o FVF actual */
    void *rt;                 /* render target actual */
    void *rt_mundo;           /* render target donde se dibujo el mundo */
    int mundo;                /* dibujos del mundo en este cuadro */
    int hecho;                /* el efecto ya se aplico en este cuadro */
    int en_efecto;            /* nuestros propios dibujos: los ganchos no hacen nada */
    int vista_ok, proy_ok;
    float vista[16], proy[16];
    DWORD z, zw, ab, at, fog, luz, ps;
    DWORD niebla;             /* ultimo color de niebla que puso el juego (0 = ninguno) */
    void *tex0;
    UINT stride;
} J;

/* Recursos propios (pool DEFAULT: se sueltan antes de cada Reset) */
static struct {
    void *tex[3], *sup[3];    /* escena a 1/2, 1/4 y 1/8 */
    void *copia, *copia_sup;  /* copia de la escena si el render target no es textura */
    UINT ancho, alto;
    DWORD bloque;             /* state block para dejar todo como estaba */
    int fallo;
} R;

/* Guia en pantalla: textura MANAGED (sobrevive a los Reset) con el panel pintado por guia.c */
#define GUIA_W 600
#define GUIA_H 256
#define GUIA_S 4000                 /* ms que se muestra despues de tocar una tecla */
static struct { void *tex; DWORD bloque, pintado_ms; DWORD px[GUIA_W * GUIA_H]; } G;

/* Diagnostico */
static struct { int capturando, cuadro, dibujos; FILE *f; } C;

static void cap(const char *fmt, ...) {
    va_list ap;
    if (!C.f) return;
    va_start(ap, fmt); vfprintf(C.f, fmt, ap); va_end(ap);
    fputc('\n', C.f);
}

static int juego_en_foco(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static int flanco(int vk, int *antes) {
    int ahora = vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0, r = ahora && !*antes;
    *antes = ahora;
    return r;
}

/* Dibujo del HUD: FVF con vertices ya transformados (los handles de shader son impares). */
static int es_hud(DWORD vs) { return !(vs & 1) && (vs & 0x00E) == FVF_XYZRHW; }

/* ---------------------------------------------------------------- recursos */

static void soltar(void **p) {
    if (*p) { LLAMAR(Release_t, *p, OBJ_Release)(*p); *p = NULL; }
}

/* Color grading: pixel shader 1.4 armado a mano (no hay ensamblador de D3D8). Las constantes de
 * ps 1.x se recortan a [-1,1]: los factores van a la mitad y se usa el modificador _x2.
 *   texld r0, t0 ; dp3 r1.rgb, r0, c0 (luma) ; sub_x2 r2.rgb, r0, r1 ; mad r0.rgb, r2, c1, r1
 *   sub r0.rgb, r0, c2 ; mad_x2 r0.rgb, r0, c3, c5 ; mul_x2 r0.rgb, r0, c4
 *   c0 = luma, c1 = saturacion/2, c2 = 0.5, c3 = contraste/2, c4 = rgb/2, c5 = 0.25 */
#define PS_REG(tipo, n) (0x80000000u | ((DWORD)(tipo) << 28) | (n))
#define PS_DST(n, x2) (PS_REG(0, n) | 0x00070000u | ((x2) ? 0x01000000u : 0))   /* temporal .rgb */
#define PS_DST_TODO(n) (PS_REG(0, n) | 0x000F0000u)
#define PS_SRC(tipo, n) (PS_REG(tipo, n) | 0x00E40000u)
#define R_ 0
#define C_ 2
#define T_ 3
static const DWORD PS_COLOR[] = {
    0xFFFF0104,
    66, PS_DST_TODO(0), PS_SRC(T_, 0),
    8, PS_DST(1, 0), PS_SRC(R_, 0), PS_SRC(C_, 0),
    3, PS_DST(2, 1), PS_SRC(R_, 0), PS_SRC(R_, 1),
    4, PS_DST(0, 0), PS_SRC(R_, 2), PS_SRC(C_, 1), PS_SRC(R_, 1),
    3, PS_DST(0, 0), PS_SRC(R_, 0), PS_SRC(C_, 2),
    4, PS_DST(0, 1), PS_SRC(R_, 0), PS_SRC(C_, 3), PS_SRC(C_, 5),
    5, PS_DST(0, 1), PS_SRC(R_, 0), PS_SRC(C_, 4),
    0x0000FFFF};
static DWORD g_ps_color;
static int g_ps_fallo;

static void liberar_recursos(void *dev) {
    int i;
    if (g_ps_color && dev) LLAMAR(DeletePixelShader_t, dev, DEV_DeletePixelShader)(dev, g_ps_color);
    g_ps_color = 0;
    if (G.bloque && dev) LLAMAR(StateBlock_t, dev, DEV_DeleteStateBlock)(dev, G.bloque);
    G.bloque = 0;
    for (i = 0; i < 3; i++) { soltar(&R.sup[i]); soltar(&R.tex[i]); }
    soltar(&R.copia_sup); soltar(&R.copia);
    if (R.bloque && dev) LLAMAR(StateBlock_t, dev, DEV_DeleteStateBlock)(dev, R.bloque);
    R.bloque = 0; R.ancho = R.alto = 0;
}

static int textura_rt(void *dev, UINT w, UINT h, UINT formato, void **tex, void **sup) {
    if (FAILED(g_crear_textura(dev, w, h, 1, USAGE_RENDERTARGET, formato, POOL_DEFAULT, tex)) || !*tex) return 0;
    if (FAILED(LLAMAR(GetSurfaceLevel_t, *tex, TEX_GetSurfaceLevel)(*tex, 0, sup))) { soltar(tex); return 0; }
    return 1;
}

static int preparar_recursos(void *dev, UINT w, UINT h, UINT formato) {
    int i;
    if (R.ancho == w && R.alto == h && R.tex[0]) return 1;
    liberar_recursos(dev);
    for (i = 0; i < 3; i++)
        if (!textura_rt(dev, w >> (i + 1), h >> (i + 1), formato, &R.tex[i], &R.sup[i])) {
            anotar("efectos: no pude crear las texturas del blur (%ux%u formato %u)", w, h, formato);
            liberar_recursos(dev); return 0;
        }
    if (FAILED(LLAMAR(CreateStateBlock_t, dev, DEV_CreateStateBlock)(dev, SBT_ALL, &R.bloque))) {
        anotar("efectos: no pude crear el state block");
        liberar_recursos(dev); return 0;
    }
    R.ancho = w; R.alto = h;
    anotar("efectos: blur listo para %ux%u", w, h);
    return 1;
}

/* ---------------------------------------------------------------- dibujo propio */

#define RS(r, v) LLAMAR(SetRenderState_t, dev, DEV_SetRenderState)(dev, r, v)
#define TSS(e, t, v) LLAMAR(SetTextureStageState_t, dev, DEV_SetTextureStageState)(dev, e, t, v)

typedef struct { float x, y, z, rhw, u, v; } VerticeQuad;

/* Rectangulo de pantalla completa en el render target actual (w x h), con la textura dada
 * corrida (du, dv) en coordenadas de textura. */
static void quad(void *dev, UINT w, UINT h, float du, float dv) {
    VerticeQuad q[4];
    float x0 = -0.5f, y0 = -0.5f, x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
    q[0] = (VerticeQuad){x0, y0, 0, 1, 0 + du, 0 + dv};
    q[1] = (VerticeQuad){x1, y0, 0, 1, 1 + du, 0 + dv};
    q[2] = (VerticeQuad){x0, y1, 0, 1, 0 + du, 1 + dv};
    q[3] = (VerticeQuad){x1, y1, 0, 1, 1 + du, 1 + dv};
    LLAMAR(DrawPrimitiveUP_t, dev, DEV_DrawPrimitiveUP)(dev, PT_TRIANGLESTRIP, 2, q, sizeof(VerticeQuad));
}

/* Rectangulo (x, y, w, h) de pantalla con la textura de (0,0) a (u1,v1). */
static void quad_en(void *dev, float x, float y, float w, float h, float u1, float v1) {
    VerticeQuad q[4];
    x -= 0.5f; y -= 0.5f;
    q[0] = (VerticeQuad){x, y, 0, 1, 0, 0};
    q[1] = (VerticeQuad){x + w, y, 0, 1, u1, 0};
    q[2] = (VerticeQuad){x, y + h, 0, 1, 0, v1};
    q[3] = (VerticeQuad){x + w, y + h, 0, 1, u1, v1};
    LLAMAR(DrawPrimitiveUP_t, dev, DEV_DrawPrimitiveUP)(dev, PT_TRIANGLESTRIP, 2, q, sizeof(VerticeQuad));
}

/* Estados comunes de nuestros dibujos: sin luces, niebla, shaders ni texturas extra. */
static void estados_base(void *dev) {
    int e;
    LLAMAR(SetPixelShader_t, dev, DEV_SetPixelShader)(dev, 0);
    RS(RS_LIGHTING, FALSE); RS(RS_FOGENABLE, FALSE); RS(RS_ALPHATESTENABLE, FALSE);
    RS(RS_CULLMODE, CULL_NONE); RS(RS_FILLMODE, 3); RS(RS_CLIPPING, TRUE); RS(RS_CLIPPLANEENABLE, 0);
    RS(RS_ZWRITEENABLE, FALSE); RS(RS_COLORWRITEENABLE, 0xF); RS(RS_BLENDOP, 1);
    for (e = 1; e < 8; e++) {
        TSS(e, TSS_COLOROP, TOP_DISABLE); TSS(e, TSS_ALPHAOP, TOP_DISABLE);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, e, NULL);
    }
    TSS(0, TSS_TEXCOORDINDEX, 0); TSS(0, TSS_TEXTURETRANSFORMFLAGS, 0);
    TSS(0, TSS_ADDRESSU, 3); TSS(0, TSS_ADDRESSV, 3);   /* clamp */
    TSS(0, TSS_MAGFILTER, 2); TSS(0, TSS_MINFILTER, 2); TSS(0, TSS_MIPFILTER, 0);
}

/* Achica src (sw x sh) sobre el render target actual (w x h): 4 muestras bilineales en las
 * esquinas de cada texel destino, sumadas a 1/4 cada una (blur de caja de 4x4 texels fuente). */
static void achicar(void *dev, void *src, UINT sw, UINT sh, UINT w, UINT h) {
    float du = 1.0f / sw, dv = 1.0f / sh;
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_TARGET, 0, 1.0f, 0);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, src);
    TSS(0, TSS_COLOROP, TOP_MODULATE); TSS(0, TSS_COLORARG1, TA_TEXTURE); TSS(0, TSS_COLORARG2, TA_TFACTOR);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
    RS(RS_TEXTUREFACTOR, 0x40404040);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_ONE); RS(RS_DESTBLEND, BLEND_ONE);
    quad(dev, w, h, -du, -dv); quad(dev, w, h, du, -dv);
    quad(dev, w, h, -du, dv);  quad(dev, w, h, du, dv);
}

/* Caja sobre la cancha: medio largo fx, a lo ancho de z0 a z1 (en m); de 1 m bajo el piso hasta
 * ALTO_CAJA_M. Triangulos sueltos: se dibuja sin culling, el sentido no importa. */
static void caja(void *dev, float fx, float z0, float z1) {
    static const BYTE caras[36] = {
        0,1,2, 1,3,2,  4,6,5, 5,6,7,  0,4,1, 1,4,5,  2,3,6, 3,7,6,  0,2,4, 2,6,4,  1,5,3, 3,5,7};
    float v[8][3], t[36][3];
    int i;
    for (i = 0; i < 8; i++) {
        v[i][0] = (i & 1 ? fx : -fx) * M;
        v[i][1] = (i & 4 ? -ALTO_CAJA_M : 1.0f) * M;
        v[i][2] = (i & 2 ? z1 : z0) * M;
    }
    for (i = 0; i < 36; i++) memcpy(t[i], v[caras[i]], sizeof t[i]);
    LLAMAR(DrawPrimitiveUP_t, dev, DEV_DrawPrimitiveUP)(dev, PT_TRIANGLELIST, 12, t, 12);
}

/* Posicion de la camara en el mundo a partir de la matriz de vista (filas: p * V). */
static void camara_de_vista(const float *m, float *c) {
    int j;
    for (j = 0; j < 3; j++) c[j] = -(m[12] * m[j * 4] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
}

static const DWORD COLOR_MASCARA[N_CAJAS + 1] = {
    0xFFFF0000, 0xFFFF8000, 0xFFFFFF00, 0xFF00FF00, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF};

static void aplicar_blur(void *dev) {
    void *rt = NULL, *z = NULL, *escena = NULL;
    DescSuperficie8 ds;
    float cam[3];
    static const float identidad[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int i, n, lado;

    if (FAILED(LLAMAR(GetSurface_t, dev, DEV_GetRenderTarget)(dev, &rt)) || !rt) return;
    LLAMAR(GetSurface_t, dev, DEV_GetDepthStencilSurface)(dev, &z);
    if (!z || FAILED(LLAMAR(GetDesc_t, rt, SUP_GetDesc)(rt, &ds)) || !preparar_recursos(dev, ds.ancho, ds.alto, ds.formato)) {
        soltar(&rt); soltar(&z); R.fallo = 1; return;
    }
    /* La escena: el render target del juego si es una textura; si no, una copia. */
    if (FAILED(LLAMAR(GetContainer_t, rt, SUP_GetContainer)(rt, &IID_Textura8, &escena)) || !escena) {
        escena = NULL;
        if (!R.copia && !textura_rt(dev, ds.ancho, ds.alto, ds.formato, &R.copia, &R.copia_sup)) {
            soltar(&rt); soltar(&z); R.fallo = 1; return;
        }
        LLAMAR(CopyRects_t, dev, DEV_CopyRects)(dev, rt, NULL, 0, R.copia_sup, NULL);
    }

    LLAMAR(StateBlock_t, dev, DEV_CaptureStateBlock)(dev, R.bloque);
    estados_base(dev);
    LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
    RS(RS_ZENABLE, FALSE); RS(RS_STENCILENABLE, FALSE);

    /* 0. Color grading sutil (saturacion, contraste, ganancia RGB) sobre el mundo, desde una copia */
    if (E.color && !E.ver_mascara && !g_ps_fallo) {
        if (!g_ps_color && FAILED(LLAMAR(CreatePixelShader_t, dev, DEV_CreatePixelShader)(dev, PS_COLOR, &g_ps_color))) {
            g_ps_color = 0; g_ps_fallo = 1;
            anotar("efectos: el device no acepto el pixel shader del color: color apagado");
        }
        if (g_ps_color && (R.copia || textura_rt(dev, ds.ancho, ds.alto, ds.formato, &R.copia, &R.copia_sup)) &&
            SUCCEEDED(LLAMAR(CopyRects_t, dev, DEV_CopyRects)(dev, rt, NULL, 0, R.copia_sup, NULL))) {
            float c[6][4] = {{0.299f, 0.587f, 0.114f, 0}, {0}, {0.5f, 0.5f, 0.5f, 0.5f}, {0},
                             {E.rgb[0] * 0.5f, E.rgb[1] * 0.5f, E.rgb[2] * 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f, 0.25f}};
            for (i = 0; i < 4; i++) { c[1][i] = E.saturacion * 0.5f; c[3][i] = E.contraste * 0.5f; }
            LLAMAR(SetPixelShaderConstant_t, dev, DEV_SetPixelShaderConstant)(dev, 0, c, 6);
            LLAMAR(SetPixelShader_t, dev, DEV_SetPixelShader)(dev, g_ps_color);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, R.copia);
            RS(RS_ALPHABLENDENABLE, FALSE);
            quad(dev, ds.ancho, ds.alto, 0, 0);
            LLAMAR(SetPixelShader_t, dev, DEV_SetPixelShader)(dev, 0);
            /* si el blur parte de la copia, que parta de la escena ya corregida */
            if (!escena) LLAMAR(CopyRects_t, dev, DEV_CopyRects)(dev, rt, NULL, 0, R.copia_sup, NULL);
        }
    }

    /* 1. Escena a 1/2, 1/4 y 1/8 (solo si hay blur) */
    for (i = 0; i < 3 && E.activo; i++) {
        void *src = i ? R.tex[i - 1] : (escena ? escena : R.copia);
        UINT sw = ds.ancho >> i, sh = ds.alto >> i;
        LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.sup[i], NULL);
        achicar(dev, src, sw, sh, sw >> 1, sh >> 1);
    }
    LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);

    /* 2. Mascara: el stencil cuenta en cuantas cajas esta la superficie visible. Por caja, el
     * bit 7 cambia con cada cara que queda delante de la superficie: con la camara afuera, impar
     * = adentro; con la camara adentro, par = adentro. */
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_STENCIL, 0, 1.0f, 0);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_DIFFUSE);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_DIFFUSE);
    RS(RS_STENCILENABLE, TRUE); RS(RS_STENCILFAIL, SOP_KEEP); RS(RS_STENCILZFAIL, SOP_KEEP);
    RS(RS_COLORWRITEENABLE, 0); RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_ZERO); RS(RS_DESTBLEND, BLEND_ONE);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_WORLD, identidad);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_VIEW, J.vista);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_PROJECTION, J.proy);
    camara_de_vista(J.vista, cam);
    cap("blur: rt %ux%u fmt %u escena=%s camara (%.0f, %.0f, %.0f) = (%.1f, %.1f, %.1f) m", ds.ancho, ds.alto, ds.formato,
        escena ? "textura" : "copia", cam[0], cam[1], cam[2], cam[0] / M, cam[1] / M, cam[2] / M);
    cap("blur: vista [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", J.vista[0], J.vista[1], J.vista[2], J.vista[3],
        J.vista[4], J.vista[5], J.vista[6], J.vista[7], J.vista[8], J.vista[9], J.vista[10], J.vista[11],
        J.vista[12], J.vista[13], J.vista[14], J.vista[15]);
    cap("blur: proy [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", J.proy[0], J.proy[1], J.proy[2], J.proy[3],
        J.proy[4], J.proy[5], J.proy[6], J.proy[7], J.proy[8], J.proy[9], J.proy[10], J.proy[11],
        J.proy[12], J.proy[13], J.proy[14], J.proy[15]);
    lado = cam[2] >= 0.0f ? 1 : -1;           /* lado de la cancha donde esta la camara */
    for (i = 0; i < N_CAJAS; i++) {
        float fx = E.fondo[i];
        float z0 = lado > 0 ? -E.lejos[i] : -E.cerca[i], z1 = lado > 0 ? E.cerca[i] : E.lejos[i];
        int adentro = cam[0] > -fx * M && cam[0] < fx * M && cam[2] > z0 * M && cam[2] < z1 * M &&
                      cam[1] < 1.0f * M && cam[1] > -ALTO_CAJA_M * M;
        /* caras delante de la superficie: invertir el bit 7 */
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZ);
        RS(RS_ZENABLE, TRUE); RS(RS_ZFUNC, CMP_LESSEQUAL);
        RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_INVERT); RS(RS_STENCILWRITEMASK, 0x80);
        caja(dev, fx, z0, z1);
        /* sumar 1 al contador (bits 0-6) donde la superficie quedo adentro */
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
        RS(RS_ZENABLE, FALSE);
        RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILREF, adentro ? 0x00 : 0x80); RS(RS_STENCILMASK, 0x80);
        RS(RS_STENCILPASS, SOP_INCR); RS(RS_STENCILWRITEMASK, 0x7F);
        quad(dev, ds.ancho, ds.alto, 0, 0);
        /* limpiar el bit 7 */
        RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_ZERO); RS(RS_STENCILWRITEMASK, 0x80);
        quad(dev, ds.ancho, ds.alto, 0, 0);
    }

    /* 3. Mezcla por anillo: contador n = cajas que contienen la superficie (N_CAJAS = nitido). */
    RS(RS_COLORWRITEENABLE, 0xF);
    RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILMASK, 0x7F); RS(RS_STENCILPASS, SOP_KEEP); RS(RS_STENCILWRITEMASK, 0);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
    for (n = 0; n < N_CAJAS && (E.activo || E.ver_mascara); n++) {
        int anillo = N_CAJAS - 1 - n;              /* 0 = el de adentro */
        float a = E.alfa[anillo] * E.fuerza;
        DWORD fa = (DWORD)((a < 0 ? 0 : a > 1 ? 1 : a) * 255.0f + 0.5f);
        RS(RS_STENCILREF, n);
        if (E.ver_mascara) {
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
            TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
            TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
            RS(RS_TEXTUREFACTOR, (COLOR_MASCARA[anillo] & 0x00FFFFFF) | 0x80000000);
        } else {
            /* anillos de adentro: 1/2 (blur suave); los de afuera: 1/8 (blur fuerte) */
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, R.tex[anillo < 2 ? 0 : anillo < 4 ? 1 : 2]);
            TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TEXTURE);
            TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
            RS(RS_TEXTUREFACTOR, fa << 24);
        }
        quad(dev, ds.ancho, ds.alto, 0, 0);
    }
    /* 4. Bruma: color de aire sobre los anillos de afuera; toma el color de niebla del juego
     * (cambia con el estadio y la hora) si lo puso. */
    for (n = 0; n < N_CAJAS && E.bruma && !E.ver_mascara; n++) {
        int anillo = N_CAJAS - 1 - n;
        float a = E.bruma_alfa[anillo] * E.bruma_fuerza;
        DWORD fa = (DWORD)((a < 0 ? 0 : a > 1 ? 1 : a) * 255.0f + 0.5f);
        DWORD color = E.bruma_color ? E.bruma_color : J.niebla ? J.niebla & 0x00FFFFFF : 0x00B4C3D2;
        if (!fa) continue;
        RS(RS_STENCILREF, n);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
        RS(RS_TEXTUREFACTOR, (fa << 24) | color);
        quad(dev, ds.ancho, ds.alto, 0, 0);
    }
    if (E.ver_mascara) {   /* lo que queda nitido, en violeta */
        RS(RS_STENCILREF, N_CAJAS);
        RS(RS_TEXTUREFACTOR, (COLOR_MASCARA[N_CAJAS] & 0x00FFFFFF) | 0x50000000);
        quad(dev, ds.ancho, ds.alto, 0, 0);
    }
    /* el stencil queda limpio para el resto del cuadro */
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_STENCIL, 0, 1.0f, 0);

    LLAMAR(StateBlock_t, dev, DEV_ApplyStateBlock)(dev, R.bloque);
    LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
    soltar(&escena); soltar(&rt); soltar(&z);
}

/* Guia encima de todo (al final de la escena): se muestra unos segundos despues de cada tecla de
 * la camara o del blur, o siempre si se fijo con la tecla de la guia. Va arriba al centro, entre
 * los dos recuadros del marcador, para no tapar paneles del juego. */
static void dibujar_guia(void *dev) {
    DWORD ahora = GetTickCount();
    void *rt = NULL;
    DescSuperficie8 ds;
    float w, h;
    int y;
    if (!E.guia_fija && (!g_guia.tecla_ms || ahora - g_guia.tecla_ms > GUIA_S)) return;
    if (!G.tex && FAILED(g_crear_textura(dev, 1024, GUIA_H, 1, 0, 21 /* A8R8G8B8 */, POOL_MANAGED, &G.tex))) {
        G.tex = NULL; return;
    }
    if (!G.bloque && FAILED(LLAMAR(CreateStateBlock_t, dev, DEV_CreateStateBlock)(dev, SBT_ALL, &G.bloque))) {
        G.bloque = 0; return;
    }
    if (ahora - G.pintado_ms > 100) {
        RectBloqueado8 lr;
        guia_pintar(G.px, GUIA_W, GUIA_H, &g_guia);
        if (SUCCEEDED(LLAMAR(LockRect_t, G.tex, TEX_LockRect)(G.tex, 0, &lr, NULL, 0))) {
            for (y = 0; y < GUIA_H; y++) memcpy((BYTE *)lr.bits + y * lr.pitch, G.px + y * GUIA_W, GUIA_W * 4);
            LLAMAR(UnlockRect_t, G.tex, TEX_UnlockRect)(G.tex, 0);
        }
        G.pintado_ms = ahora;
    }
    if (FAILED(LLAMAR(GetSurface_t, dev, DEV_GetRenderTarget)(dev, &rt)) || !rt) return;
    if (FAILED(LLAMAR(GetDesc_t, rt, SUP_GetDesc)(rt, &ds))) { soltar(&rt); return; }
    soltar(&rt);
    LLAMAR(StateBlock_t, dev, DEV_CaptureStateBlock)(dev, G.bloque);
    estados_base(dev);
    LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
    RS(RS_ZENABLE, FALSE); RS(RS_STENCILENABLE, FALSE);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, G.tex);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TEXTURE);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TEXTURE);
    w = ds.ancho * 0.38f; h = w * GUIA_H / GUIA_W;
    quad_en(dev, (ds.ancho - w) * 0.5f, ds.alto * 0.02f, w, h, (float)GUIA_W / 1024.0f, 1.0f);
    LLAMAR(StateBlock_t, dev, DEV_ApplyStateBlock)(dev, G.bloque);
}

/* Antes de cada dibujo del juego: el primer dibujo del HUD despues del mundo dispara el efecto. */
static void antes_de_dibujar(void *dev) {
    if (J.en_efecto) return;
    if (!es_hud(J.vs)) {
        J.mundo++;
        J.rt_mundo = J.rt;
        return;
    }
    if (J.hecho || !(E.activo || E.bruma || E.color || E.ver_mascara) || R.fallo || J.mundo < 50 || J.rt != J.rt_mundo || !J.vista_ok || !J.proy_ok) return;
    J.hecho = 1;
    J.en_efecto = 1;
    __try { aplicar_blur(dev); }
    __except (EXCEPTION_EXECUTE_HANDLER) { R.fallo = 1; anotar("efectos: excepcion en el blur, queda apagado"); }
    J.en_efecto = 0;
}

/* ---------------------------------------------------------------- ganchos */

static void dibujo_diag(const char *que, DWORD prim, UINT a, UINT b, UINT c) {
    C.dibujos++;
    cap("%-5s prim=%u %u %u %u | vs=%08lx ps=%08lx z=%lu zw=%lu ab=%lu at=%lu fog=%lu luz=%lu tex0=%p rt=%p stride=%u",
        que, prim, a, b, c, J.vs, J.ps, J.z, J.zw, J.ab, J.at, J.fog, J.luz, J.tex0, J.rt, J.stride);
}

static HRESULT WINAPI h_BeginScene(void *d) {
    static int a_activar, a_captura, a_guia;
    if (!J.en_efecto) {
        J.mundo = 0; J.hecho = 0; J.vista_ok = J.proy_ok = 0; J.rt_mundo = NULL;
        if (juego_en_foco() && flanco(E.t_guia, &a_guia)) {
            E.guia_fija = !E.guia_fija;
            WritePrivateProfileStringA("efectos", "guia_fija", E.guia_fija ? "1" : "0", g_ini);
            g_guia.tecla_ms = GetTickCount();
        }
        if (juego_en_foco() && flanco(E.t_activar, &a_activar)) {
            g_guia.tecla_ms = GetTickCount();
            int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && !shift) {
                E.bruma = !E.bruma;
                WritePrivateProfileStringA("efectos", "bruma", E.bruma ? "1" : "0", g_ini);
            } else if (shift && !ctrl) {
                E.color = !E.color;
                WritePrivateProfileStringA("efectos", "color", E.color ? "1" : "0", g_ini);
            } else if (shift && ctrl) {
                E.ver_mascara = !E.ver_mascara;
                WritePrivateProfileStringA("efectos", "ver_mascara", E.ver_mascara ? "1" : "0", g_ini);
            } else {
                E.activo = !E.activo;
                WritePrivateProfileStringA("efectos", "blur", E.activo ? "1" : "0", g_ini);
            }
        }
        g_guia.blur_activo = E.activo; g_guia.bruma_activa = E.bruma; g_guia.color_activo = E.color && !g_ps_fallo; g_guia.t_blur = E.t_activar; g_guia.t_guia = E.t_guia;
        C.cuadro++;
        if (E.diagnostico && !C.capturando && flanco(E.t_captura, &a_captura)) {
            char ruta[MAX_PATH];
            strcpy(ruta, g_log);
            strcpy(strrchr(ruta, '.'), "_cuadro.txt");
            C.f = fopen(ruta, "w");
            C.capturando = C.f != NULL; C.dibujos = 0;
            memset((void *)g_cuenta, 0, sizeof g_cuenta);
            cap("== cuadro %d", C.cuadro);
        }
        cap("BeginScene");
    }
    return ((BeginScene_t)g_orig[DEV_BeginScene])(d);
}

static HRESULT WINAPI h_EndScene(void *d) {
    if (!J.en_efecto) {
        J.en_efecto = 1;
        __try { dibujar_guia(d); }
        __except (EXCEPTION_EXECUTE_HANDLER) { E.guia_fija = 0; g_guia.tecla_ms = 0; }
        J.en_efecto = 0;
    }
    if (C.capturando && !J.en_efecto) {
        int i;
        cap("EndScene: %d dibujos. Llamadas por funcion del device:", C.dibujos);
        for (i = 0; i < DEV_N; i++) if (g_cuenta[i]) cap("  [%d] %ld", i, g_cuenta[i]);
        fclose(C.f); C.f = NULL; C.capturando = 0;
        anotar("efectos: cuadro capturado (%d dibujos)", C.dibujos);
    }
    return ((EndScene_t)g_orig[DEV_EndScene])(d);
}

static HRESULT WINAPI h_Reset(void *d, void *pp) {
    liberar_recursos(d);
    R.fallo = 0;
    return ((Reset_t)g_orig[DEV_Reset])(d, pp);
}

static HRESULT WINAPI h_SetRenderTarget(void *d, void *rt, void *z) {
    if (!J.en_efecto) { J.rt = rt; cap("SetRenderTarget rt=%p z=%p", rt, z); }
    return ((SetRenderTarget_t)g_orig[DEV_SetRenderTarget])(d, rt, z);
}

static HRESULT WINAPI h_Clear(void *d, DWORD n, const void *r, DWORD flags, DWORD color, float z, DWORD st) {
    if (!J.en_efecto) cap("Clear flags=%lx color=%08lx z=%g", flags, color, z);
    return ((Clear_t)g_orig[DEV_Clear])(d, n, r, flags, color, z, st);
}

static HRESULT WINAPI h_SetTransform(void *d, DWORD tipo, const float *m) {
    if (!J.en_efecto) {
        /* La primera vista y la primera proyeccion de perspectiva del cuadro son las de la camara. */
        if (tipo == TS_VIEW && !J.vista_ok) { memcpy(J.vista, m, sizeof J.vista); J.vista_ok = 1; }
        if (tipo == TS_PROJECTION && !J.proy_ok && m[11] != 0.0f) { memcpy(J.proy, m, sizeof J.proy); J.proy_ok = 1; }
        if (C.capturando && (tipo == TS_VIEW || tipo == TS_PROJECTION))
            cap("SetTransform %lu [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", tipo,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    return ((SetTransform_t)g_orig[DEV_SetTransform])(d, tipo, m);
}

static HRESULT WINAPI h_SetRenderState(void *d, DWORD rs, DWORD v) {
    if (!J.en_efecto) switch (rs) {
    case RS_ZENABLE:
        if (C.capturando && v != J.z) cap("  Z %lu -> %lu (tras %d dibujos)", J.z, v, C.dibujos);
        J.z = v; break;
    case RS_ZWRITEENABLE: J.zw = v; break;
    case RS_ALPHABLENDENABLE: J.ab = v; break;
    case RS_ALPHATESTENABLE: J.at = v; break;
    case RS_FOGENABLE: J.fog = v; break;
    case RS_FOGCOLOR: J.niebla = v; break;
    case RS_LIGHTING: J.luz = v; break;
    }
    return ((SetRenderState_t)g_orig[DEV_SetRenderState])(d, rs, v);
}

static HRESULT WINAPI h_SetTexture(void *d, DWORD etapa, void *t) {
    if (etapa == 0 && !J.en_efecto) J.tex0 = t;
    return ((SetTexture_t)g_orig[DEV_SetTexture])(d, etapa, t);
}

static HRESULT WINAPI h_DrawPrimitive(void *d, DWORD p, UINT ini, UINT n) {
    antes_de_dibujar(d);
    if (C.capturando && !J.en_efecto) dibujo_diag("DP", p, ini, n, 0);
    return ((DrawPrimitive_t)g_orig[DEV_DrawPrimitive])(d, p, ini, n);
}

static HRESULT WINAPI h_DrawIndexedPrimitive(void *d, DWORD p, UINT minimo, UINT nv, UINT ini, UINT n) {
    antes_de_dibujar(d);
    if (C.capturando && !J.en_efecto) dibujo_diag("DIP", p, nv, ini, n);
    return ((DrawIndexedPrimitive_t)g_orig[DEV_DrawIndexedPrimitive])(d, p, minimo, nv, ini, n);
}

static HRESULT WINAPI h_DrawPrimitiveUP(void *d, DWORD p, UINT n, const void *datos, UINT stride) {
    antes_de_dibujar(d);
    if (C.capturando && !J.en_efecto) { J.stride = stride; dibujo_diag("DPUP", p, n, 0, 0); }
    return ((DrawPrimitiveUP_t)g_orig[DEV_DrawPrimitiveUP])(d, p, n, datos, stride);
}

static HRESULT WINAPI h_DrawIndexedPrimitiveUP(void *d, DWORD p, UINT minimo, UINT nv, UINT n, const void *idx,
                                               DWORD fmt, const void *datos, UINT stride) {
    antes_de_dibujar(d);
    if (C.capturando && !J.en_efecto) { J.stride = stride; dibujo_diag("DIPUP", p, nv, n, 0); }
    return ((DrawIndexedPrimitiveUP_t)g_orig[DEV_DrawIndexedPrimitiveUP])(d, p, minimo, nv, n, idx, fmt, datos, stride);
}

static HRESULT WINAPI h_SetVertexShader(void *d, DWORD vs) {
    if (!J.en_efecto) J.vs = vs;
    return ((SetVertexShader_t)g_orig[DEV_SetVertexShader])(d, vs);
}

static HRESULT WINAPI h_SetVertexShaderConstant(void *d, DWORD reg, const void *datos, DWORD n) {
    if (C.capturando && !J.en_efecto && reg < 8) {
        const float *f = (const float *)datos;
        if (n >= 4)
            cap("  VSC c%lu x%lu [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", reg, n,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        else cap("  VSC c%lu x%lu [%g %g %g %g]", reg, n, f[0], f[1], f[2], f[3]);
    }
    return ((SetVertexShaderConstant_t)g_orig[DEV_SetVertexShaderConstant])(d, reg, datos, n);
}

static HRESULT WINAPI h_SetStreamSource(void *d, UINT n, void *vb, UINT stride) {
    if (n == 0 && !J.en_efecto) J.stride = stride;
    return ((SetStreamSource_t)g_orig[DEV_SetStreamSource])(d, n, vb, stride);
}

static HRESULT WINAPI h_SetPixelShader(void *d, DWORD ps) {
    if (!J.en_efecto) J.ps = ps;
    return ((SetPixelShader_t)g_orig[DEV_SetPixelShader])(d, ps);
}

static const struct { int idx; void *fn; } GANCHOS[] = {
    {DEV_Reset, h_Reset}, {DEV_SetRenderTarget, h_SetRenderTarget},
    {DEV_BeginScene, h_BeginScene}, {DEV_EndScene, h_EndScene}, {DEV_Clear, h_Clear},
    {DEV_SetTransform, h_SetTransform}, {DEV_SetRenderState, h_SetRenderState}, {DEV_SetTexture, h_SetTexture},
    {DEV_DrawPrimitive, h_DrawPrimitive}, {DEV_DrawIndexedPrimitive, h_DrawIndexedPrimitive},
    {DEV_DrawPrimitiveUP, h_DrawPrimitiveUP}, {DEV_DrawIndexedPrimitiveUP, h_DrawIndexedPrimitiveUP},
    {DEV_SetVertexShader, h_SetVertexShader}, {DEV_SetVertexShaderConstant, h_SetVertexShaderConstant},
    {DEV_SetStreamSource, h_SetStreamSource},
    {DEV_SetPixelShader, h_SetPixelShader},
};
#define N_GANCHOS (sizeof(GANCHOS) / sizeof(GANCHOS[0]))

/* ---------------------------------------------------------------- arranque */

/* Kitserver (kload.dll) guarda el device del juego: se espera a que exista.
 * Crear un device propio no sirve: Kitserver engancha CreateDevice y lo tomaria por el del juego. */
static void *device_del_juego(void) {
    typedef void *(*GetActiveDevice_t)(void);
    HMODULE kload = GetModuleHandleA("kload.dll");
    GetActiveDevice_t activo = kload ? (GetActiveDevice_t)GetProcAddress(kload, "GetActiveDevice") : NULL;
    void *dev = NULL;
    DWORD caps[128];
    int intento;
    if (!activo) { anotar("efectos: no encontre kload.dll (hace falta Kitserver)"); return NULL; }
    for (intento = 0; intento < 360 && !(dev = activo()); intento++) Sleep(500);
    if (!dev) return NULL;
    memset(caps, 0, sizeof caps);
    if (SUCCEEDED(LLAMAR(GetDeviceCaps_t, dev, DEV_GetDeviceCaps)(dev, caps)))
        anotar("efectos: device %p, VS %lx PS %lx, texturas simultaneas %lu", dev, caps[49], caps[51], caps[38]);
    return dev;
}

static BYTE *g_d3d8, *g_d3d8_fin;
#define EN_D3D8(p) ((BYTE *)(p) >= g_d3d8 && (BYTE *)(p) < g_d3d8_fin)

static int hotpatcheable(const BYTE *fn) {
    static const BYTE cc[5] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC}, nop[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    return fn[0] == 0x8B && fn[1] == 0xFF && (memcmp(fn - 5, cc, 5) == 0 || memcmp(fn - 5, nop, 5) == 0);
}

/* Hotpatch: las funciones de d3d8.dll empiezan con mov edi,edi y tienen 5 bytes de relleno
 * antes. Se pone un jmp largo al gancho en el relleno y un jmp corto hacia atras en lugar del
 * mov edi,edi; la original sigue en fn+2. Sobrevive a los Reset y a cualquier copia del vtable. */
static int hotpatch(BYTE *fn, void *gancho) {
    DWORD prot;
    if (!hotpatcheable(fn)) return 0;
    VirtualProtect(fn - 5, 7, PAGE_EXECUTE_READWRITE, &prot);
    fn[-5] = 0xE9;
    *(LONG *)(fn - 4) = (LONG)((BYTE *)gancho - fn);
    *(volatile WORD *)fn = 0xF9EB;   /* jmp short fn-5 */
    VirtualProtect(fn - 5, 7, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), fn - 5, 7);
    return 1;
}

/* Sigue la cadena de ganchos de Kitserver desde una entrada del vtable hasta la funcion real de
 * d3d8.dll: busca call/jmp [puntero] (FF 15 / FF 25) en el gancho y entra en lo que apunten.
 * En *ranura queda el puntero (de un gancho ajeno) que lleva a la funcion real, o NULL si la
 * entrada ya apuntaba a d3d8.dll. */
static BYTE *original_en_d3d8(BYTE *fn, BYTE ***ranura, int prof) {
    int i;
    if (EN_D3D8(fn)) return fn;
    if (prof > 3) return NULL;
    for (i = 0; i < 0x400; i++) {
        __try {
            if (fn[i] == 0xFF && (fn[i + 1] == 0x15 || fn[i + 1] == 0x25)) {
                BYTE **pp = *(BYTE ***)(fn + i + 2), *p = *pp, *r;
                MEMORY_BASIC_INFORMATION mi;
                if (VirtualQuery(p, &mi, sizeof mi) && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                    (r = original_en_d3d8(p, ranura, prof + 1)) != NULL) {
                    if (EN_D3D8(p)) *ranura = pp;
                    return r;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return NULL;
}

static void poner_puntero(void **donde, void *valor) {
    DWORD prot;
    VirtualProtect(donde, sizeof(void *), PAGE_READWRITE, &prot);
    InterlockedExchangePointer(donde, valor);
    VirtualProtect(donde, sizeof(void *), prot, &prot);
}

/* Engancha cada funcion del device segun la d3d8.dll que haya:
 *   - d3d8.dll de Windows: hotpatch (el vtable vive en el device y se rearma en cada Reset);
 *   - dgVoodoo y otras sin prologo de hotpatch: el vtable es fijo en la dll, se cambia la entrada;
 *   - Reset, que ya engancha Kitserver: hotpatch de la real, o el puntero donde Kitserver guarda
 *     la original (cambiar su entrada del vtable haria ping-pong con sus re-enganches). */
int efectos_iniciar(const char *ini) {
    void *dev, **vt;
    IMAGE_NT_HEADERS *nt;
    BYTE *reset, *crear, **ranura_reset = NULL, **ranura_crear = NULL;
    int i, n_hot = 0, n_vt = 0;
    unsigned k;
    char lista[512] = "";
    strcpy(g_ini, ini);
    leer_ajustes();
    if (!(dev = device_del_juego())) { anotar("efectos: no consegui el device de D3D8"); return 0; }
    if (!(g_d3d8 = (BYTE *)GetModuleHandleA("d3d8.dll"))) return 0;
    nt = (IMAGE_NT_HEADERS *)(g_d3d8 + ((IMAGE_DOS_HEADER *)g_d3d8)->e_lfanew);
    g_d3d8_fin = g_d3d8 + nt->OptionalHeader.SizeOfImage;
    vt = VT(dev);
    crear = original_en_d3d8((BYTE *)vt[DEV_CreateTexture], &ranura_crear, 0);
    reset = original_en_d3d8((BYTE *)vt[DEV_Reset], &ranura_reset, 0);
    if (!crear || !reset) {
        anotar("efectos: no encontre CreateTexture/Reset reales de d3d8.dll (%p %p): blur apagado", crear, reset);
        return 0;
    }
    if (!hotpatcheable(reset) && !ranura_reset && !EN_D3D8(vt[DEV_Reset])) {
        anotar("efectos: no puedo enganchar Reset: blur apagado");
        return 0;
    }
    g_crear_textura = (CreateTexture_t)crear;
    g_stubs = VirtualAlloc(NULL, DEV_N * 12, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) return 0;
    for (i = 0; i < DEV_N; i++) {
        BYTE *st = g_stubs + i * 12, *fn = i == DEV_Reset ? reset : (BYTE *)vt[i];
        void *gancho = NULL;
        for (k = 0; k < N_GANCHOS; k++) if (GANCHOS[k].idx == i) gancho = GANCHOS[k].fn;
        st[0] = 0xFF; st[1] = 0x05; *(volatile LONG **)(st + 2) = &g_cuenta[i];
        st[6] = 0xFF; st[7] = 0x25; *(void ***)(st + 8) = &g_destino[i];
        /* antes de activar el salto, el stub ya tiene que llevar a la original */
        if (EN_D3D8(fn) && hotpatcheable(fn)) {
            g_orig[i] = fn + 2;
            g_destino[i] = gancho ? gancho : fn + 2;
            if (hotpatch(fn, st)) { n_hot++; continue; }
        }
        if (i == DEV_Reset && ranura_reset) {
            g_orig[i] = fn;
            g_destino[i] = gancho;
            poner_puntero((void **)ranura_reset, st);
            n_vt++; continue;
        }
        if (EN_D3D8(vt[i]) && (gancho || E.diagnostico)) {
            g_orig[i] = vt[i];
            g_destino[i] = gancho ? gancho : vt[i];
            poner_puntero(&vt[i], st);
            n_vt++; continue;
        }
        if (gancho && strlen(lista) < sizeof lista - 8) sprintf(lista + strlen(lista), " %d", i);
    }
    g_guia.blur_ok = 1;
    anotar("efectos: %d funciones con hotpatch, %d por puntero; ganchos sin poner:%s", n_hot, n_vt,
           lista[0] ? lista : " ninguno");
    anotar("efectos: blur %s (tecla 0x%02X; Ctrl = bruma, Shift = color, Ctrl+Shift = mascara); guia en pantalla: tecla 0x%02X",
           E.activo ? "activo" : "apagado", E.t_activar, E.t_guia);
    return 1;
}
