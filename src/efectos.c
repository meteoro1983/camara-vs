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
#include <math.h>
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
    DEV_CreateRenderTarget = 25, DEV_CopyRects = 28, DEV_SetRenderTarget = 31, DEV_GetRenderTarget = 32,
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
typedef HRESULT (WINAPI *CreateRenderTarget_t)(void *, UINT, UINT, UINT, UINT, BOOL, void **);
typedef HRESULT (WINAPI *SupLock_t)(void *, RectBloqueado8 *, const RECT *, DWORD);
typedef HRESULT (WINAPI *SupUnlock_t)(void *);
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
#define TA_CURRENT 1
#define TA_TEXTURE 2
#define TA_TFACTOR 3
#define BLEND_ZERO 1
#define BLEND_ONE 2
#define BLEND_SRCALPHA 5
#define BLEND_INVSRCALPHA 6
#define CMP_LESS 2
#define CMP_EQUAL 3
#define CMP_GREATER 5
#define CMP_ALWAYS 8
#define SOP_KEEP 1
#define SOP_ZERO 2
#define SOP_REPLACE 3
#define SOP_INVERT 6
#define SOP_INCR 7
#define CULL_NONE 1
#define TS_VIEW 2
#define TS_PROJECTION 3
#define TS_WORLD 256
#define FVF_XYZ 0x002
#define FVF_XYZRHW 0x004
#define FVF_TEX1 0x100
#define FVF_DIFFUSE 0x040
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
    int activo, bruma, color, ver_mascara, diagnostico, guia_fija, publico;
    float saturacion, contraste, rgb[3];      /* color grading */
    int t_activar, t_captura, t_guia, t_bruma, t_color, t_mascara, t_cancha;
    int aniso, cancha;                        /* cancha: bit 0 = franjas de cesped, bit 1 = sombra */
    float nitidez, vineta, sombra_linea, sombra_fuerza, sombra_penumbra, franjas_fuerza;
    int franjas_n, luces, cielo;            /* luces: bloom + destellos de noche; cielo: tinte del cielo */
    float bloom, destellos;
    float fuerza, bruma_fuerza;
    DWORD bruma_color;                        /* 0 = el color de niebla del juego */
    /* medio ancho de cada caja del lado lejano y del lado de la camara, y medio largo, en m */
    /* [0] = preset 1, [1] = preset 2 (F2 / Ctrl+F2 una segunda vez) */
    float lejos[2][N_CAJAS], cerca[2][N_CAJAS], fondo[2][N_CAJAS];
    float alfa[2][N_CAJAS], bruma_alfa[2][N_CAJAS]; /* mezcla por anillo, de adentro hacia afuera */
    float nivel[2][N_CAJAS];                  /* blur por anillo: 1 = escena a 1/2, 2 = 1/4, 3 = 1/8 */
} E;
static char g_ini[MAX_PATH];

/* Anillos por defecto (lineas: laterales a 34 m, de gol a 52,5 m). Del lado lejano el blur
 * arranca tenue 11 m adentro de la cancha; del lado de la camara 2,5 m adentro; en los fondos
 * 1 m antes de la linea de gol (la red apenas). */
static const float LEJOS[N_CAJAS]  = {23.0f, 27.0f, 31.0f, 37.0f, 44.0f, 52.0f};
static const float CERCA[N_CAJAS]  = {31.5f, 35.0f, 40.0f, 46.0f, 53.0f, 60.0f};
/* Detras de los arcos los anillos se abren mas: con la camara a la espalda del arquero la
 * tribuna de enfrente (60-80 m) queda con blur medio y no con el mas espeso. */
static const float FONDO[N_CAJAS]  = {51.5f, 57.0f, 64.0f, 76.0f, 92.0f, 115.0f};
/* La escena puede ser mucho mas grande que la pantalla (4K en WE9LE) y el blur a 1/2 casi no se
 * ve: adentro de la cancha va a 1/4 con poca mezcla; las tribunas a 1/4 y las de afuera a 1/8. */
static const float ALFA[N_CAJAS]   = {0.30f, 0.40f, 0.45f, 0.60f, 0.80f, 1.00f};
static const float NIVEL[N_CAJAS]  = {2.0f, 2.0f, 2.0f, 2.0f, 3.0f, 3.0f};
/* Bruma: solo los anillos de afuera (tribunas altas), muy suave. */
static const float BRUMA[N_CAJAS]  = {0.0f, 0.0f, 0.05f, 0.10f, 0.16f, 0.24f};
/* Preset 2: el blur entra mas en la cancha (lado lejano desde la linea del area grande, a 20 m del
 * centro; lado de la camara 3,75 m adentro; fondos 2 m antes de la linea de gol), un poco mas
 * fuerte en general pero con la primera banda mas suave. Bruma mas baja y menos sutil. */
static const float LEJOS2[N_CAJAS] = {20.0f, 25.0f, 31.0f, 37.0f, 44.0f, 52.0f};
static const float CERCA2[N_CAJAS] = {30.25f, 34.0f, 39.0f, 45.0f, 52.0f, 60.0f};
static const float FONDO2[N_CAJAS] = {50.5f, 56.0f, 63.0f, 75.0f, 91.0f, 114.0f};
static const float ALFA2[N_CAJAS]  = {0.35f, 0.60f, 0.75f, 0.90f, 1.00f, 1.00f};
static const float NIVEL2[N_CAJAS] = {2.0f, 2.0f, 3.0f, 3.0f, 3.0f, 3.0f};
static const float BRUMA2[N_CAJAS] = {0.04f, 0.12f, 0.22f, 0.32f, 0.42f, 0.52f};
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
    E.publico = GetPrivateProfileIntA("efectos", "publico_colores", 1, g_ini);
    E.color = GetPrivateProfileIntA("efectos", "color", 1, g_ini);
    GetPrivateProfileStringA("efectos", "color_saturacion", "1.12", s, sizeof s, g_ini);
    E.saturacion = (float)atof(s);
    GetPrivateProfileStringA("efectos", "color_contraste", "1.15", s, sizeof s, g_ini);
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
    GetPrivateProfileStringA("efectos", "tecla_bruma", "0", s, sizeof s, g_ini);   /* Insert */
    E.t_bruma = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "tecla_color", "0", s, sizeof s, g_ini);   /* Inicio */
    E.t_color = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "tecla_cancha", "0", s, sizeof s, g_ini);  /* Fin */
    E.t_cancha = (int)strtol(s, NULL, 0);
    E.aniso = GetPrivateProfileIntA("efectos", "anisotropico", 8, g_ini);
    E.luces = GetPrivateProfileIntA("efectos", "luces_noche", 1, g_ini);
    E.cielo = GetPrivateProfileIntA("efectos", "cielo", 1, g_ini);
    GetPrivateProfileStringA("efectos", "bloom", "0.35", s, sizeof s, g_ini); E.bloom = (float)atof(s);
    GetPrivateProfileStringA("efectos", "destellos", "0.6", s, sizeof s, g_ini); E.destellos = (float)atof(s);
    E.cancha = GetPrivateProfileIntA("efectos", "cancha", 7, g_ini) & 7;
    E.franjas_n = GetPrivateProfileIntA("efectos", "franjas", 18, g_ini);
    if (E.franjas_n < 2) E.franjas_n = 2;
    if (E.franjas_n > 40) E.franjas_n = 40;
    GetPrivateProfileStringA("efectos", "nitidez", "0.35", s, sizeof s, g_ini); E.nitidez = (float)atof(s);
    GetPrivateProfileStringA("efectos", "vineta", "0.18", s, sizeof s, g_ini); E.vineta = (float)atof(s);
    GetPrivateProfileStringA("efectos", "sombra_linea", "12", s, sizeof s, g_ini); E.sombra_linea = (float)atof(s);
    GetPrivateProfileStringA("efectos", "sombra_fuerza", "0.25", s, sizeof s, g_ini); E.sombra_fuerza = (float)atof(s);
    GetPrivateProfileStringA("efectos", "sombra_penumbra", "2.5", s, sizeof s, g_ini); E.sombra_penumbra = (float)atof(s);
    GetPrivateProfileStringA("efectos", "franjas_fuerza", "0.07", s, sizeof s, g_ini); E.franjas_fuerza = (float)atof(s);
    GetPrivateProfileStringA("efectos", "tecla_mascara", "0", s, sizeof s, g_ini); /* Supr */
    E.t_mascara = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "captura", "0x7B", s, sizeof s, g_ini);
    E.t_captura = (int)strtol(s, NULL, 0);
    GetPrivateProfileStringA("efectos", "guia", "0x70", s, sizeof s, g_ini);
    E.t_guia = (int)strtol(s, NULL, 0);
    E.guia_fija = GetPrivateProfileIntA("efectos", "guia_fija", 0, g_ini);
    leer_lista("lado_lejano", E.lejos[0], LEJOS);
    leer_lista("lado_camara", E.cerca[0], CERCA);
    leer_lista("fondos", E.fondo[0], FONDO);
    leer_lista("mezcla", E.alfa[0], ALFA);
    leer_lista("nivel", E.nivel[0], NIVEL);
    leer_lista("bruma_mezcla", E.bruma_alfa[0], BRUMA);
    leer_lista("lado_lejano_2", E.lejos[1], LEJOS2);
    leer_lista("lado_camara_2", E.cerca[1], CERCA2);
    leer_lista("fondos_2", E.fondo[1], FONDO2);
    leer_lista("mezcla_2", E.alfa[1], ALFA2);
    leer_lista("nivel_2", E.nivel[1], NIVEL2);
    leer_lista("bruma_mezcla_2", E.bruma_alfa[1], BRUMA2);
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
    void *masc, *masc_sup;    /* mascara de pesos a resolucion completa (se pinta con el stencil) */
    void *lectura;            /* render target bloqueable a 1/8: se lee para dia/noche y reflectores */
    void *nit, *nit_sup;      /* imagen final a 1/2 para la nitidez (despues del blur y la bruma) */
    void *mtex[8], *msup[8];  /* la mascara achicada a la mitad hasta ~150 lineas: queda suave */
    int n_masc;
    void *copia, *copia_sup;  /* copia de la escena si el render target no es textura */
    UINT ancho, alto;
    DWORD bloque;             /* state block para dejar todo como estaba */
    int fallo;
} R;

/* Guia en pantalla: textura MANAGED (sobrevive a los Reset) con el panel pintado por guia.c */
#define GUIA_W 600
#define GUIA_H 292
#define GUIA_TEX_H 512
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

/* Pixel shaders 1.4 armados a mano (no hay ensamblador de D3D8). Las constantes de ps 1.x se
 * recortan a [-1,1]: los factores van a la mitad y se usa el modificador _x2.
 *   c0 = luma, c1 = saturacion/2, c2 = 0.5, c3 = contraste/2, c5 = 0.25, c6 = nitidez */
#define PS_REG(tipo, n) (0x80000000u | ((DWORD)(tipo) << 28) | (n))
#define PS_DST(n, x2) (PS_REG(0, n) | 0x00070000u | ((x2) ? 0x01000000u : 0))   /* temporal .rgb */
#define PS_DST_TODO(n) (PS_REG(0, n) | 0x000F0000u)
#define PS_SRC(tipo, n) (PS_REG(tipo, n) | 0x00E40000u)
#define R_ 0
#define C_ 2
#define T_ 3
/* Pase final (1.3): nitidez + color + vineta en dos fases de ps 1.4.
 *   texld r0 (escena), r1 (escena a 1/2), r2 (vineta)
 *   sub r1, r0, r1 ; mad r0, r1, c6, r0          nitidez: r0 + k (r0 - blur)
 *   dp3/sub_x2/mad/sub/mad_x2                    color: saturacion y contraste
 *   mul_x2 r0, r0, r2                             vineta x ganancia RGB / 2 (horneadas en la textura)
 * Todo en una fase: ps 1.4 admite 8 instrucciones aritmeticas. */
static const DWORD PS_FINAL[] = {
    0xFFFF0104,
    66, PS_DST_TODO(0), PS_SRC(T_, 0),
    66, PS_DST_TODO(1), PS_SRC(T_, 0),
    66, PS_DST_TODO(2), PS_SRC(T_, 0),
    3, PS_DST(1, 0), PS_SRC(R_, 0), PS_SRC(R_, 1),
    4, PS_DST(0, 0), PS_SRC(R_, 1), PS_SRC(C_, 6), PS_SRC(R_, 0),
    8, PS_DST(1, 0), PS_SRC(R_, 0), PS_SRC(C_, 0),
    3, PS_DST(3, 1), PS_SRC(R_, 0), PS_SRC(R_, 1),
    4, PS_DST(0, 0), PS_SRC(R_, 3), PS_SRC(C_, 1), PS_SRC(R_, 1),
    3, PS_DST(0, 0), PS_SRC(R_, 0), PS_SRC(C_, 2),
    4, PS_DST(0, 1), PS_SRC(R_, 0), PS_SRC(C_, 3), PS_SRC(C_, 5),
    5, PS_DST(0, 1), PS_SRC(R_, 0), PS_SRC(R_, 2),
    0x0000FFFF};
static DWORD g_ps_final;
static int g_final_fallo;
static void *g_vineta;                         /* textura MANAGED de la vineta */
static float g_vineta_hecha[4] = {-1, -1, -1, -1};
static DWORD g_max_aniso = 1;



static void liberar_recursos(void *dev) {
    int i;
    if (g_ps_final && dev) LLAMAR(DeletePixelShader_t, dev, DEV_DeletePixelShader)(dev, g_ps_final);
    g_ps_final = 0;
    if (G.bloque && dev) LLAMAR(StateBlock_t, dev, DEV_DeleteStateBlock)(dev, G.bloque);
    G.bloque = 0;
    for (i = 0; i < 3; i++) { soltar(&R.sup[i]); soltar(&R.tex[i]); }
    for (i = 0; i < R.n_masc; i++) { soltar(&R.msup[i]); soltar(&R.mtex[i]); }
    soltar(&R.masc_sup); soltar(&R.masc); R.n_masc = 0; soltar(&R.lectura); soltar(&R.nit_sup); soltar(&R.nit);
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
    if (!textura_rt(dev, w, h, 21 /* A8R8G8B8 */, &R.masc, &R.masc_sup)) {
        anotar("efectos: no pude crear la mascara del blur");
        liberar_recursos(dev); return 0;
    }
    {
        UINT mw = w, mh = h;
        while (mh > 160 && R.n_masc < 8) {
            mw >>= 1; mh >>= 1;
            if (!textura_rt(dev, mw, mh, 21, &R.mtex[R.n_masc], &R.msup[R.n_masc])) {
                anotar("efectos: no pude crear la mascara achicada");
                liberar_recursos(dev); return 0;
            }
            R.n_masc++;
        }
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

/* Rectangulo de pantalla completa a la profundidad z (0 = cerca, 1 = lejos). */
static void quad_z(void *dev, UINT w, UINT h, float z) {
    VerticeQuad q[4];
    float x0 = -0.5f, y0 = -0.5f, x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
    q[0] = (VerticeQuad){x0, y0, z, 1, 0, 0};
    q[1] = (VerticeQuad){x1, y0, z, 1, 1, 0};
    q[2] = (VerticeQuad){x0, y1, z, 1, 0, 1};
    q[3] = (VerticeQuad){x1, y1, z, 1, 1, 1};
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
 * esquinas de cada texel destino, sumadas a 1/4 cada una (blur de caja de 4x4 texels fuente).
 * Promedia tambien el alfa (la mascara de pesos va en el alfa). */
static void achicar(void *dev, void *src, UINT sw, UINT sh, UINT w, UINT h) {
    float du = 1.0f / sw, dv = 1.0f / sh;
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_TARGET, 0, 1.0f, 0);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, src);
    TSS(0, TSS_COLOROP, TOP_MODULATE); TSS(0, TSS_COLORARG1, TA_TEXTURE); TSS(0, TSS_COLORARG2, TA_TFACTOR);
    TSS(0, TSS_ALPHAOP, TOP_MODULATE); TSS(0, TSS_ALPHAARG1, TA_TEXTURE); TSS(0, TSS_ALPHAARG2, TA_TFACTOR);
    RS(RS_TEXTUREFACTOR, 0x40404040);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_ONE); RS(RS_DESTBLEND, BLEND_ONE);
    quad(dev, w, h, -du, -dv); quad(dev, w, h, du, -dv);
    quad(dev, w, h, -du, dv);  quad(dev, w, h, du, dv);
}

/* Pesos por anillo (0 = nitido .. 1) -> mascara suave. Se pintan con el stencil (contador de
 * cajas) en el alfa de la mascara a resolucion completa y se achica hasta ~150 lineas: asi los
 * anillos se funden sin escalones donde su borde corta los sprites del publico. Deja puesto el
 * render target de la mascara; devuelve la textura final. */
static void *mascara_suave(void *dev, void *z, UINT w, UINT h, const float *peso) {
    void *src = R.masc;
    int n, i;
    LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.masc_sup, z);
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_TARGET, 0, 1.0f, 0);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
    RS(RS_ZENABLE, FALSE); RS(RS_ALPHABLENDENABLE, FALSE);
    RS(RS_STENCILENABLE, TRUE); RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILMASK, 0x7F);
    RS(RS_STENCILPASS, SOP_KEEP); RS(RS_STENCILFAIL, SOP_KEEP); RS(RS_STENCILZFAIL, SOP_KEEP); RS(RS_STENCILWRITEMASK, 0);
    for (n = 0; n < N_CAJAS; n++) {
        float a = peso[N_CAJAS - 1 - n];           /* contador n -> anillo N-1-n */
        DWORD fa = (DWORD)((a < 0 ? 0 : a > 1 ? 1 : a) * 255.0f + 0.5f);
        if (!fa) continue;
        RS(RS_STENCILREF, n);
        RS(RS_TEXTUREFACTOR, fa * 0x01010101u);
        quad(dev, w, h, 0, 0);
    }
    RS(RS_STENCILENABLE, FALSE);
    for (i = 0; i < R.n_masc; i++) {
        LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.msup[i], NULL);
        achicar(dev, src, w, h, w >> 1, h >> 1);
        src = R.mtex[i]; w >>= 1; h >>= 1;
    }
    return src;
}

/* Mezcla sobre el render target actual (w x h) la imagen desenfocada (o un color, si imagen es
 * NULL) con el alfa de la mascara. */
static void mezclar(void *dev, UINT w, UINT h, void *imagen, DWORD color, void *mascara) {
    RS(RS_ZENABLE, FALSE); RS(RS_STENCILENABLE, FALSE);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
    if (imagen) {
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, imagen);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TEXTURE);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TEXTURE);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 1, mascara);
        TSS(1, TSS_COLOROP, TOP_SELECTARG1); TSS(1, TSS_COLORARG1, TA_CURRENT);
        TSS(1, TSS_ALPHAOP, TOP_SELECTARG1); TSS(1, TSS_ALPHAARG1, TA_TEXTURE);
        TSS(1, TSS_TEXCOORDINDEX, 0); TSS(1, TSS_TEXTURETRANSFORMFLAGS, 0);
        TSS(1, TSS_ADDRESSU, 3); TSS(1, TSS_ADDRESSV, 3);
        TSS(1, TSS_MAGFILTER, 2); TSS(1, TSS_MINFILTER, 2); TSS(1, TSS_MIPFILTER, 0);
    } else {
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, mascara);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TEXTURE);
        RS(RS_TEXTUREFACTOR, color);
    }
    quad(dev, w, h, 0, 0);
    TSS(1, TSS_COLOROP, TOP_DISABLE); TSS(1, TSS_ALPHAOP, TOP_DISABLE);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 1, NULL);
}

/* Caja sobre la cancha: medio largo fx, a lo ancho de z0 a z1 (en m); de 1 m bajo el piso hasta
 * ALTO_CAJA_M. Triangulos sueltos: se dibuja sin culling, el sentido no importa. */
static void caja2(void *dev, float x0, float x1, float z0, float z1, float alto) {
    static const BYTE caras[36] = {
        0,1,2, 1,3,2,  4,6,5, 5,6,7,  0,4,1, 1,4,5,  2,3,6, 3,7,6,  0,2,4, 2,6,4,  1,5,3, 3,5,7};
    float v[8][3], t[36][3];
    int i;
    for (i = 0; i < 8; i++) {
        v[i][0] = (i & 1 ? x1 : x0) * M;
        v[i][1] = (i & 4 ? -alto : 1.0f) * M;
        v[i][2] = (i & 2 ? z1 : z0) * M;
    }
    for (i = 0; i < 36; i++) memcpy(t[i], v[caras[i]], sizeof t[i]);
    LLAMAR(DrawPrimitiveUP_t, dev, DEV_DrawPrimitiveUP)(dev, PT_TRIANGLELIST, 12, t, 12);
}

/* Stencil = en cuantas de las cajas {x0, x1, z0, z1, alto} (m) esta la superficie visible (ver
 * aplicar_blur). Donde el juego no escribio profundidad queda el valor "cielo". */
static void contar_cajas(void *dev, UINT w, UINT h, int n, const float (*c)[5], int cielo) {
    static const float identidad[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int i;
    LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_STENCIL, 0, 1.0f, 0);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_DIFFUSE);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_DIFFUSE);
    RS(RS_STENCILENABLE, TRUE); RS(RS_STENCILFAIL, SOP_KEEP); RS(RS_STENCILZFAIL, SOP_KEEP);
    RS(RS_COLORWRITEENABLE, 0); RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_ZERO); RS(RS_DESTBLEND, BLEND_ONE);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_WORLD, identidad);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_VIEW, J.vista);
    LLAMAR(SetTransform_t, dev, DEV_SetTransform)(dev, TS_PROJECTION, J.proy);
    for (i = 0; i < n; i++) {
        /* caras detras de la superficie: invertir el bit 7 */
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZ);
        RS(RS_ZENABLE, TRUE); RS(RS_ZFUNC, CMP_GREATER);
        RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_INVERT); RS(RS_STENCILWRITEMASK, 0x80);
        caja2(dev, c[i][0], c[i][1], c[i][2], c[i][3], c[i][4]);
        /* sumar 1 al contador (bits 0-6) donde la superficie quedo adentro */
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
        RS(RS_ZENABLE, FALSE);
        RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILREF, 0x80); RS(RS_STENCILMASK, 0x80);
        RS(RS_STENCILPASS, SOP_INCR); RS(RS_STENCILWRITEMASK, 0x7F);
        quad(dev, w, h, 0, 0);
        /* limpiar el bit 7 */
        RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_ZERO); RS(RS_STENCILWRITEMASK, 0x80);
        quad(dev, w, h, 0, 0);
    }
    RS(RS_ZENABLE, TRUE); RS(RS_ZFUNC, CMP_LESS);
    RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_REPLACE); RS(RS_STENCILREF, cielo); RS(RS_STENCILWRITEMASK, 0x7F);
    quad_z(dev, w, h, 0.99999f);
    RS(RS_ZENABLE, FALSE); RS(RS_COLORWRITEENABLE, 0xF);
}

/* Vineta: textura 64x64 (gris) que multiplica la imagen: 1 en el centro, 1 - fuerza en las esquinas. */
static void *textura_vineta(void *dev) {
    if (g_vineta && g_vineta_hecha[0] == E.vineta && g_vineta_hecha[1] == E.rgb[0] &&
        g_vineta_hecha[2] == E.rgb[1] && g_vineta_hecha[3] == E.rgb[2]) return g_vineta;
    if (!g_vineta && FAILED(g_crear_textura(dev, 64, 64, 1, 0, 21, POOL_MANAGED, &g_vineta))) { g_vineta = NULL; return NULL; }
    {
        RectBloqueado8 lr; int x, y;
        if (FAILED(LLAMAR(LockRect_t, g_vineta, TEX_LockRect)(g_vineta, 0, &lr, NULL, 0))) return NULL;
        for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
            float dx = (x + 0.5f) / 32.0f - 1.0f, dy = (y + 0.5f) / 32.0f - 1.0f;
            float r = sqrtf(dx * dx * 0.8f + dy * dy) / 1.25f, t = r < 0.45f ? 0.0f : (r - 0.45f) / 0.55f;
            float v = 1.0f - E.vineta * (t > 1 ? 1 : t * t * (3 - 2 * t));
            DWORD c = 0xFF000000u;
            int j;
            for (j = 0; j < 3; j++) {                       /* x ganancia RGB / 2 (el shader hace _x2) */
                float g = v * E.rgb[2 - j] * 0.5f * 255.0f + 0.5f;
                c |= (DWORD)(g > 255 ? 255 : g) << (8 * j);
            }
            ((DWORD *)((BYTE *)lr.bits + y * lr.pitch))[x] = c;
        }
        LLAMAR(UnlockRect_t, g_vineta, TEX_UnlockRect)(g_vineta, 0);
    }
    g_vineta_hecha[0] = E.vineta; g_vineta_hecha[1] = E.rgb[0]; g_vineta_hecha[2] = E.rgb[1]; g_vineta_hecha[3] = E.rgb[2];
    return g_vineta;
}

/* Posicion de la camara en el mundo a partir de la matriz de vista (filas: p * V). */
static void camara_de_vista(const float *m, float *c) {
    int j;
    for (j = 0; j < 3; j++) c[j] = -(m[12] * m[j * 4] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
}

/* Dia / noche y reflectores, a partir de la escena a 1/8 leida cada LEER_CADA cuadros. */
#define LEER_CADA 20
#define MAX_LUCES 16
static int g_noche, g_cuadro_lectura;
static struct { float x, y, f; } g_luces[MAX_LUCES];
static int g_n_luces;
static void *g_destello;                       /* textura MANAGED de la estrella */

static void leer_escena(void *dev, UINT w8, UINT h8) {
    RectBloqueado8 lr;
    UINT x, y, n = 0;
    double suma = 0;
    if (!R.lectura && FAILED(LLAMAR(CreateRenderTarget_t, dev, DEV_CreateRenderTarget)(dev, w8, h8, 21, 0, TRUE, &R.lectura))) {
        R.lectura = NULL; return;
    }
    LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.lectura, NULL);
    LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, R.tex[2]);
    TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TEXTURE);
    TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TEXTURE);
    RS(RS_ALPHABLENDENABLE, FALSE);
    quad(dev, w8, h8, 0, 0);
    if (FAILED(((SupLock_t)VT(R.lectura)[9])(R.lectura, &lr, NULL, 0x10 /* READONLY */))) return;
    /* brillo medio del tercio de arriba (cielo / tribunas altas): de noche es muy oscuro */
    for (y = 0; y < h8 / 3; y++) {
        const BYTE *p = (const BYTE *)lr.bits + y * lr.pitch;
        for (x = 0; x < w8; x++, p += 4) { suma += 0.299 * p[2] + 0.587 * p[1] + 0.114 * p[0]; n++; }
    }
    if (n) {
        double m = suma / n;
        int antes = g_noche;
        if (m < 40) g_noche = 1; else if (m > 55) g_noche = 0;     /* con histeresis */
        if (g_noche != antes) anotar("efectos: %s (brillo medio arriba %.0f)", g_noche ? "noche" : "dia", m);
    }
    /* reflectores: maximos locales muy brillantes */
    g_n_luces = 0;
    if (g_noche) {
        for (y = 2; y + 2 < h8 * 2 / 3 && g_n_luces < MAX_LUCES; y++) {
            for (x = 2; x + 2 < w8 && g_n_luces < MAX_LUCES; x++) {
                const BYTE *p = (const BYTE *)lr.bits + y * lr.pitch + 4 * x;
                int l = (p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2])), dx, dy, maximo = 1;
                if (l < 235) continue;
                for (dy = -2; dy <= 2 && maximo; dy++) for (dx = -2; dx <= 2; dx++) {
                    const BYTE *q = (const BYTE *)lr.bits + (y + dy) * lr.pitch + 4 * (x + dx);
                    int lq = (q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]));
                    if ((dx || dy) && (lq > l || (lq == l && (dy < 0 || (dy == 0 && dx < 0))))) { maximo = 0; break; }
                }
                if (maximo) {
                    g_luces[g_n_luces].x = (x + 0.5f) * 8; g_luces[g_n_luces].y = (y + 0.5f) * 8;
                    g_luces[g_n_luces].f = (l - 235) / 20.0f; g_n_luces++;
                }
            }
        }
    }
    ((SupUnlock_t)VT(R.lectura)[10])(R.lectura);
}

/* Estrella de los reflectores (estilo arcade): brillo central + 4 rayos largos y 4 cortos. */
static void *textura_destello(void *dev) {
    RectBloqueado8 lr; int x, y;
    if (g_destello) return g_destello;
    if (FAILED(g_crear_textura(dev, 128, 128, 1, 0, 21, POOL_MANAGED, &g_destello))) { g_destello = NULL; return NULL; }
    if (FAILED(LLAMAR(LockRect_t, g_destello, TEX_LockRect)(g_destello, 0, &lr, NULL, 0))) return g_destello;
    for (y = 0; y < 128; y++) for (x = 0; x < 128; x++) {
        float dx = (x - 63.5f) / 64.0f, dy = (y - 63.5f) / 64.0f, r = sqrtf(dx * dx + dy * dy);
        float g = expf(-r * 7.0f) * 1.2f;
        float d1 = fabsf(dx) < fabsf(dy) ? fabsf(dx) : fabsf(dy);                    /* rayos en cruz */
        float d2 = fabsf(dx - dy) < fabsf(dx + dy) ? fabsf(dx - dy) : fabsf(dx + dy);  /* rayos en diagonal */
        float v = g + expf(-d1 * 60.0f) * (1 - r) * 0.9f + expf(-d2 * 70.0f) * (1 - r * 1.6f > 0 ? 1 - r * 1.6f : 0) * 0.4f;
        BYTE b = (BYTE)((v > 1 ? 1 : v) * 255.0f);
        ((DWORD *)((BYTE *)lr.bits + y * lr.pitch))[x] = 0xFF000000u | b * 0x010101u;
    }
    LLAMAR(UnlockRect_t, g_destello, TEX_UnlockRect)(g_destello, 0);
    return g_destello;
}

typedef struct { float x, y, z, rhw; DWORD c; } VerticeColor;

static const DWORD COLOR_MASCARA[N_CAJAS + 1] = {
    0xFFFF0000, 0xFFFF8000, 0xFFFFFF00, 0xFF00FF00, 0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF};

static void aplicar_blur(void *dev) {
    void *rt = NULL, *z = NULL, *escena = NULL;
    DescSuperficie8 ds;
    float cam[3];
    static const float identidad[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int i, n, lado, pb = E.activo > 1;

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

    /* 1. Escena a 1/2, 1/4 y 1/8 (blur; la de 1/2 tambien para la nitidez) */
    for (i = 0; i < 3; i++) {
        void *src = i ? R.tex[i - 1] : (escena ? escena : R.copia);
        UINT sw = ds.ancho >> i, sh = ds.alto >> i;
        LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.sup[i], NULL);
        achicar(dev, src, sw, sh, sw >> 1, sh >> 1);
    }
    if (++g_cuadro_lectura >= LEER_CADA) { g_cuadro_lectura = 0; leer_escena(dev, ds.ancho >> 3, ds.alto >> 3); }
    LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);

    /* 2. Mascara: el stencil cuenta en cuantas cajas esta la superficie visible. Por caja, el
     * bit 7 cambia con cada cara que queda DETRAS de la superficie: si el punto esta adentro de la
     * caja hay exactamente una (la de salida del rayo), este donde este la camara; afuera, 0 o 2.
     * No depende de saber si la camara esta adentro de la caja. */
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
    {
        float c[N_CAJAS][5];
        for (i = 0; i < N_CAJAS; i++) {
            c[i][0] = -E.fondo[pb][i]; c[i][1] = E.fondo[pb][i];
            c[i][2] = lado > 0 ? -E.lejos[pb][i] : -E.cerca[pb][i];
            c[i][3] = lado > 0 ? E.cerca[pb][i] : E.lejos[pb][i];
            c[i][4] = ALTO_CAJA_M;
        }
        /* donde no hay profundidad (cielo, suelo sin Z de algunos estadios) queda nitido */
        contar_cajas(dev, ds.ancho, ds.alto, N_CAJAS, (const float (*)[5])c, N_CAJAS);
    }

    /* 3. Mezcla. Contador n = cajas que contienen la superficie (N_CAJAS = nitido). Por cada ancho
     * de blur y para la bruma, los pesos por anillo pasan por una mascara suave. */
    RS(RS_COLORWRITEENABLE, 0xF);
    if (!E.ver_mascara) {
        float peso[N_CAJAS];
        void *masc;
        int nv, hay;
        for (nv = 1; nv <= 3 && E.activo; nv++) {
            for (i = 0, hay = 0; i < N_CAJAS; i++) {
                int k = (int)(E.nivel[pb][i] + 0.5f);
                k = k < 1 ? 1 : k > 3 ? 3 : k;
                peso[i] = k == nv ? E.alfa[pb][i] * E.fuerza : 0.0f;
                if (peso[i] > 0.0f) hay = 1;
            }
            if (!hay) continue;
            masc = mascara_suave(dev, z, ds.ancho, ds.alto, peso);
            LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
            mezclar(dev, ds.ancho, ds.alto, R.tex[nv - 1], 0, masc);
        }
        /* Bruma: color de aire sobre los anillos de afuera; toma el color de niebla del juego
         * (cambia con el estadio y la hora) si lo puso. */
        if (E.bruma) {
            DWORD color = E.bruma_color ? E.bruma_color : J.niebla ? J.niebla & 0x00FFFFFF : 0x00B4C3D2;
            for (i = 0, hay = 0; i < N_CAJAS; i++) {
                peso[i] = E.bruma_alfa[E.bruma > 1][i] * E.bruma_fuerza;
                if (peso[i] > 0.0f) hay = 1;
            }
            if (hay) {
                masc = mascara_suave(dev, z, ds.ancho, ds.alto, peso);
                LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
                mezclar(dev, ds.ancho, ds.alto, NULL, color, masc);
            }
        }
    }
    /* 4. Cancha (1.3): sombra del estadio sobre el lado lejano (3 cajas = penumbra suave) y franjas
     * de corte del cesped (cajas finas a ras del piso). Ancladas en metros, sirven en cualquier estadio. */
    if (!E.ver_mascara && (E.cancha & 2) && E.sombra_fuerza > 0 && !g_noche) {   /* de noche no hay sombra */
        float c[3][5], peso[N_CAJAS] = {0};
        int k;
        void *masc;
        for (k = 0; k < 3; k++) {
            float borde = E.sombra_linea + (k - 1) * E.sombra_penumbra;   /* m desde el centro hacia el lado lejano */
            c[k][0] = -120; c[k][1] = 120; c[k][4] = ALTO_CAJA_M;
            if (lado > 0) { c[k][2] = -150; c[k][3] = -borde; } else { c[k][2] = borde; c[k][3] = 150; }
            peso[N_CAJAS - 1 - (k + 1)] = E.sombra_fuerza * (k + 1) / 3.0f;
        }
        contar_cajas(dev, ds.ancho, ds.alto, 3, (const float (*)[5])c, 0);
        masc = mascara_suave(dev, z, ds.ancho, ds.alto, peso);
        LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
        mezclar(dev, ds.ancho, ds.alto, NULL, 0x000A0E16, masc);
    }
    if (!E.ver_mascara && (E.cancha & 1) && E.franjas_fuerza > 0) {
        float c[20][5], ancho = 105.0f / E.franjas_n;
        int k, n_c = 0;
        DWORD a = (DWORD)((E.franjas_fuerza > 1 ? 1 : E.franjas_fuerza) * 255.0f + 0.5f);
        for (k = 0; k < E.franjas_n && n_c < 20; k += 2, n_c++) {
            c[n_c][0] = -52.5f + k * ancho; c[n_c][1] = c[n_c][0] + ancho;
            c[n_c][2] = -34.0f; c[n_c][3] = 34.0f; c[n_c][4] = 0.15f;   /* solo el pasto */
        }
        contar_cajas(dev, ds.ancho, ds.alto, n_c, (const float (*)[5])c, 0);
        RS(RS_STENCILENABLE, TRUE); RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILREF, 1); RS(RS_STENCILMASK, 0x7F);
        RS(RS_STENCILPASS, SOP_KEEP); RS(RS_STENCILWRITEMASK, 0);
        RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
        RS(RS_TEXTUREFACTOR, a << 24);
        quad(dev, ds.ancho, ds.alto, 0, 0);
        RS(RS_STENCILENABLE, FALSE);
    }

    /* Perimetro: oscurecido de contacto a ras del piso alrededor de la cancha (base de vallas y
     * tribunas). Banda entre cajas: adentro de la cancha (3 cajas) nada, afuera mas oscuro. */
    if (!E.ver_mascara && (E.cancha & 4)) {
        float c[3][5], peso[N_CAJAS] = {0};
        int k;
        void *masc;
        for (k = 0; k < 3; k++) {
            float m = 3.0f + (2 - k) * 4.0f;       /* k=0 la mas grande */
            c[k][0] = -(52.5f + m); c[k][1] = 52.5f + m; c[k][2] = -(34.0f + m); c[k][3] = 34.0f + m; c[k][4] = 2.5f;
        }
        peso[N_CAJAS - 1 - 1] = 0.30f; peso[N_CAJAS - 1 - 2] = 0.15f;   /* 1 caja = lo mas afuera */
        contar_cajas(dev, ds.ancho, ds.alto, 3, (const float (*)[5])c, 0);
        masc = mascara_suave(dev, z, ds.ancho, ds.alto, peso);
        LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
        mezclar(dev, ds.ancho, ds.alto, NULL, 0x00000000, masc);
    }
    /* Cielo: degradado suave donde no hay profundidad (mas azul de dia, mas profundo de noche) */
    if (!E.ver_mascara && E.cielo && E.color) {
        VerticeColor q[4];
        DWORD arriba = g_noche ? 0x70040814u : 0x50183A80u, abajo = arriba & 0x00FFFFFF;
        float x1 = (float)ds.ancho - 0.5f, y1 = (float)ds.alto * 0.7f;
        LLAMAR(Clear_t, dev, DEV_Clear)(dev, 0, NULL, CLEAR_STENCIL, 0, 1.0f, 0);
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
        RS(RS_STENCILENABLE, TRUE); RS(RS_COLORWRITEENABLE, 0);
        RS(RS_ZENABLE, TRUE); RS(RS_ZFUNC, CMP_LESS);
        RS(RS_STENCILFUNC, CMP_ALWAYS); RS(RS_STENCILPASS, SOP_REPLACE); RS(RS_STENCILFAIL, SOP_KEEP); RS(RS_STENCILZFAIL, SOP_KEEP);
        RS(RS_STENCILREF, 1); RS(RS_STENCILWRITEMASK, 0x7F);
        quad_z(dev, ds.ancho, ds.alto, 0.99999f);
        RS(RS_ZENABLE, FALSE); RS(RS_COLORWRITEENABLE, 0xF);
        RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILMASK, 0x7F); RS(RS_STENCILPASS, SOP_KEEP); RS(RS_STENCILWRITEMASK, 0);
        RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_DIFFUSE);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_DIFFUSE);
        q[0] = (VerticeColor){-0.5f, -0.5f, 0, 1, arriba}; q[1] = (VerticeColor){x1, -0.5f, 0, 1, arriba};
        q[2] = (VerticeColor){-0.5f, y1, 0, 1, abajo};    q[3] = (VerticeColor){x1, y1, 0, 1, abajo};
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_DIFFUSE);
        LLAMAR(DrawPrimitiveUP_t, dev, DEV_DrawPrimitiveUP)(dev, PT_TRIANGLESTRIP, 2, q, sizeof(VerticeColor));
        LLAMAR(SetVertexShader_t, dev, DEV_SetVertexShader)(dev, FVF_XYZRHW | FVF_TEX1);
        RS(RS_STENCILENABLE, FALSE);
    }
    /* Luces de noche: bloom sutil (lo que pasa del umbral en la escena a 1/8, sumado) y destellos
     * sobre los reflectores detectados. */
    if (!E.ver_mascara && E.luces && g_noche) {
        RS(RS_STENCILENABLE, FALSE); RS(RS_ZENABLE, FALSE);
        RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_ONE);
        if (E.bloom > 0) {
            DWORD a = (DWORD)((E.bloom > 1 ? 1 : E.bloom) * 255.0f);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, R.tex[2]);
            TSS(0, TSS_COLOROP, 10 /* SUBTRACT */); TSS(0, TSS_COLORARG1, TA_TEXTURE); TSS(0, TSS_COLORARG2, TA_TFACTOR);
            TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
            RS(RS_TEXTUREFACTOR, (a << 24) | 0x00A0A0A0);      /* umbral ~63 % */
            quad(dev, ds.ancho, ds.alto, 0, 0);
        }
        if (E.destellos > 0 && g_n_luces && textura_destello(dev)) {
            int k;
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, g_destello);
            TSS(0, TSS_COLOROP, TOP_MODULATE); TSS(0, TSS_COLORARG1, TA_TEXTURE); TSS(0, TSS_COLORARG2, TA_TFACTOR);
            TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
            RS(RS_SRCBLEND, BLEND_ONE); RS(RS_DESTBLEND, BLEND_ONE);
            for (k = 0; k < g_n_luces; k++) {
                float f = g_luces[k].f > 1 ? 1 : g_luces[k].f, tam = ds.alto * (0.05f + 0.07f * f);
                DWORD v = (DWORD)(E.destellos * (0.5f + 0.5f * f) * 255.0f);
                if (v > 255) v = 255;
                RS(RS_TEXTUREFACTOR, 0xFF000000u | (v << 16) | ((v * 240 / 255) << 8) | (v * 215 / 255));
                quad_en(dev, g_luces[k].x * ds.ancho / (8.0f * (ds.ancho >> 3)) - tam * 0.5f,
                        g_luces[k].y * ds.alto / (8.0f * (ds.alto >> 3)) - tam * 0.5f, tam, tam, 1.0f, 1.0f);
            }
        }
        RS(RS_ALPHABLENDENABLE, FALSE);
    }

    /* 5. Pase final: nitidez + color + vineta (pixel shader 1.4), desde una copia de la escena */
    if (E.color && !E.ver_mascara && !g_final_fallo) {
        void *vin = textura_vineta(dev);
        if (!g_ps_final && FAILED(LLAMAR(CreatePixelShader_t, dev, DEV_CreatePixelShader)(dev, PS_FINAL, &g_ps_final))) {
            g_ps_final = 0; g_final_fallo = 1;
            anotar("efectos: el device no acepto el pixel shader final (nitidez/color/vineta): apagado");
        }
        if (g_ps_final && vin && (R.copia || textura_rt(dev, ds.ancho, ds.alto, ds.formato, &R.copia, &R.copia_sup)) &&
            (R.nit || textura_rt(dev, ds.ancho >> 1, ds.alto >> 1, ds.formato, &R.nit, &R.nit_sup)) &&
            SUCCEEDED(LLAMAR(CopyRects_t, dev, DEV_CopyRects)(dev, rt, NULL, 0, R.copia_sup, NULL))) {
            float c[7][4] = {{0.299f, 0.587f, 0.114f, 0}, {0}, {0.5f, 0.5f, 0.5f, 0.5f}, {0},
                             {E.rgb[0] * 0.5f, E.rgb[1] * 0.5f, E.rgb[2] * 0.5f, 0.5f}, {0.25f, 0.25f, 0.25f, 0.25f}, {0}};
            int e;
            float k = E.nitidez < 0 ? 0 : E.nitidez > 1 ? 1 : E.nitidez;
            for (i = 0; i < 4; i++) { c[1][i] = E.saturacion * 0.5f; c[3][i] = E.contraste * 0.5f; c[6][i] = k; }
            RS(RS_STENCILENABLE, FALSE);
            /* la referencia de la nitidez sale de la imagen final: en las tribunas ya desenfocadas no
             * hay detalle que realzar (con la escena original quedaba un velo gris) */
            LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, R.nit_sup, NULL);
            achicar(dev, R.copia, ds.ancho, ds.alto, ds.ancho >> 1, ds.alto >> 1);
            LLAMAR(SetRenderTarget_t, dev, DEV_SetRenderTarget)(dev, rt, z);
            RS(RS_ALPHABLENDENABLE, FALSE);
            LLAMAR(SetPixelShaderConstant_t, dev, DEV_SetPixelShaderConstant)(dev, 0, c, 7);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, R.copia);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 1, R.nit);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 2, vin);
            for (e = 0; e < 3; e++) {
                TSS(e, TSS_TEXCOORDINDEX, 0); TSS(e, TSS_TEXTURETRANSFORMFLAGS, 0);
                TSS(e, TSS_ADDRESSU, 3); TSS(e, TSS_ADDRESSV, 3);
                TSS(e, TSS_MAGFILTER, 2); TSS(e, TSS_MINFILTER, 2); TSS(e, TSS_MIPFILTER, 0);
            }
            LLAMAR(SetPixelShader_t, dev, DEV_SetPixelShader)(dev, g_ps_final);
            quad(dev, ds.ancho, ds.alto, 0, 0);
            LLAMAR(SetPixelShader_t, dev, DEV_SetPixelShader)(dev, 0);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 1, NULL);
            LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 2, NULL);
        }
    }

    /* Mascara a la vista (para ajustar medidas): cada anillo de un color, directo del stencil. */
    RS(RS_STENCILENABLE, TRUE);
    RS(RS_STENCILFUNC, CMP_EQUAL); RS(RS_STENCILMASK, 0x7F); RS(RS_STENCILPASS, SOP_KEEP); RS(RS_STENCILWRITEMASK, 0);
    RS(RS_ALPHABLENDENABLE, TRUE); RS(RS_SRCBLEND, BLEND_SRCALPHA); RS(RS_DESTBLEND, BLEND_INVSRCALPHA);
    for (n = 0; n < N_CAJAS && E.ver_mascara; n++) {
        int anillo = N_CAJAS - 1 - n;              /* 0 = el de adentro */
        RS(RS_STENCILREF, n);
        LLAMAR(SetTexture_t, dev, DEV_SetTexture)(dev, 0, NULL);
        TSS(0, TSS_COLOROP, TOP_SELECTARG1); TSS(0, TSS_COLORARG1, TA_TFACTOR);
        TSS(0, TSS_ALPHAOP, TOP_SELECTARG1); TSS(0, TSS_ALPHAARG1, TA_TFACTOR);
        RS(RS_TEXTUREFACTOR, (COLOR_MASCARA[anillo] & 0x00FFFFFF) | 0x80000000);
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
    if (!E.guia_fija && !g_guia.menu_abierto && (!g_guia.tecla_ms || ahora - g_guia.tecla_ms > GUIA_S)) return;
    if (!G.tex && FAILED(g_crear_textura(dev, 1024, GUIA_TEX_H, 1, 0, 21 /* A8R8G8B8 */, POOL_MANAGED, &G.tex))) {
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
    quad_en(dev, (ds.ancho - w) * 0.5f, ds.alto * 0.02f, w, h, (float)GUIA_W / 1024.0f, (float)GUIA_H / GUIA_TEX_H);
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
    if (J.hecho || !(E.activo || E.bruma || E.color || E.cancha || E.luces || E.ver_mascara) || R.fallo || J.mundo < 50 || J.rt != J.rt_mundo || !J.vista_ok || !J.proy_ok) return;
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

/* Menu de efectos: blur, bruma, color y nitidez, cancha, luces de noche, anisotropico, mascara. */
#define MENU_N 7
static void guardar_int(const char *clave, int v) {
    char s[16]; sprintf(s, "%d", v); WritePrivateProfileStringA("efectos", clave, s, g_ini);
}
static void menu_cambiar(int item, int d) {
    static const int ciclo_cancha[3] = {0, 5, 7};
    int i;
    switch (item) {
    case 0: E.activo = (E.activo + 3 + d) % 3; guardar_int("blur", E.activo); break;
    case 1: E.bruma = (E.bruma + 3 + d) % 3; guardar_int("bruma", E.bruma); break;
    case 2: E.color = !E.color; guardar_int("color", E.color); break;
    case 3:
        for (i = 0; i < 3 && ciclo_cancha[i] != (E.cancha & 7); i++) {}
        E.cancha = ciclo_cancha[((i < 3 ? i : 0) + 3 + d) % 3]; guardar_int("cancha", E.cancha); break;
    case 4: E.luces = !E.luces; guardar_int("luces_noche", E.luces); break;
    case 5: E.aniso = E.aniso > 1 ? 0 : 8; guardar_int("anisotropico", E.aniso); break;
    case 6: E.ver_mascara = !E.ver_mascara; guardar_int("ver_mascara", E.ver_mascara); break;
    }
}

static HRESULT WINAPI h_BeginScene(void *d) {
    static int a_activar, a_captura, a_guia, a_bruma, a_color, a_mascara, a_cancha;
    if (!J.en_efecto) {
        J.mundo = 0; J.hecho = 0; J.vista_ok = J.proy_ok = 0; J.rt_mundo = NULL;
        if (juego_en_foco() && flanco(E.t_guia, &a_guia)) {
            E.guia_fija = !E.guia_fija;
            WritePrivateProfileStringA("efectos", "guia_fija", E.guia_fija ? "1" : "0", g_ini);
            g_guia.tecla_ms = 0;   /* al soltarla se oculta en el acto */
        }
        /* F2 abre/cierra el menu de efectos; adentro, las teclas de altura y zoom de la camara
         * eligen (F3/F4) y cambian (F7/F8) el efecto. */
        if (juego_en_foco()) {
            char v[4];
            static int a_arr, a_aba, a_men, a_mas;
            if (flanco(E.t_activar, &a_activar)) {
                g_guia.menu_abierto = !g_guia.menu_abierto; g_guia.tecla_ms = GetTickCount();
            }
            if (g_guia.menu_abierto) {
                int arr = flanco(g_guia.t_zoom_abrir, &a_arr), aba = flanco(g_guia.t_zoom_cerrar, &a_aba);
                int d = flanco(g_guia.t_altura_mas, &a_mas) - flanco(g_guia.t_altura_menos, &a_men);
                if (arr) g_guia.menu_item = (g_guia.menu_item + MENU_N - 1) % MENU_N;
                if (aba) g_guia.menu_item = (g_guia.menu_item + 1) % MENU_N;
                if (d) menu_cambiar(g_guia.menu_item, d);
            }
            if (flanco(E.t_bruma, &a_bruma)) {
                E.bruma = (E.bruma + 1) % 3; g_guia.tecla_ms = GetTickCount();
                WritePrivateProfileStringA("efectos", "bruma", (sprintf(v, "%d", E.bruma), v), g_ini);
            }
            if (flanco(E.t_color, &a_color)) {
                E.color = !E.color; g_guia.tecla_ms = GetTickCount();
                WritePrivateProfileStringA("efectos", "color", E.color ? "1" : "0", g_ini);
            }
            if (flanco(E.t_cancha, &a_cancha)) {
                /* todo -> franjas + perimetro -> nada -> todo */
                E.cancha = (E.cancha & 7) == 7 ? 5 : (E.cancha & 7) == 0 ? 7 : 0; g_guia.tecla_ms = GetTickCount();
                WritePrivateProfileStringA("efectos", "cancha", (sprintf(v, "%d", E.cancha), v), g_ini);
            }
            if (flanco(E.t_mascara, &a_mascara)) {
                E.ver_mascara = !E.ver_mascara;
                WritePrivateProfileStringA("efectos", "ver_mascara", E.ver_mascara ? "1" : "0", g_ini);
            }
        }
        g_guia.blur_activo = E.activo; g_guia.bruma_activa = E.bruma; g_guia.color_activo = E.color && !g_final_fallo; g_guia.t_blur = E.t_activar; g_guia.t_bruma = E.t_bruma; g_guia.t_color = E.t_color; g_guia.t_guia = E.t_guia;
        g_guia.cancha = E.cancha; g_guia.t_cancha = E.t_cancha; g_guia.noche = g_noche; g_guia.luces = E.luces;
        g_guia.aniso = E.aniso; g_guia.mascara = E.ver_mascara;
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

/* Filtrado anisotropico: donde el juego pide filtro lineal al achicar, se pide anisotropico. */
static HRESULT WINAPI h_SetTextureStageState(void *d, DWORD etapa, DWORD tipo, DWORD v) {
    if (!J.en_efecto && E.aniso > 1 && g_max_aniso > 1 && tipo == TSS_MINFILTER && v == 2 && etapa < 4) {
        ((SetTextureStageState_t)g_orig[DEV_SetTextureStageState])(d, etapa, 21 /* MAXANISOTROPY */,
            (DWORD)E.aniso < g_max_aniso ? (DWORD)E.aniso : g_max_aniso);
        v = 3;                                              /* D3DTEXF_ANISOTROPIC */
    }
    return ((SetTextureStageState_t)g_orig[DEV_SetTextureStageState])(d, etapa, tipo, v);
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

/* ---------------------------------------------------------------- texturas del publico
 * Etapa de estudio: se registran las texturas que crea el juego del tamano del publico (1024x256
 * nuestro, 256x64 original) y como las escribe (LockRect/UnlockRect), para reemplazar despues sus
 * pixeles por los colores verdaderos (el juego tine una parte de la paleta al convertirla). */
static BYTE *g_d3d8, *g_d3d8_fin;
#define EN_D3D8(p) ((BYTE *)(p) >= g_d3d8 && (BYTE *)(p) < g_d3d8_fin)
static int hotpatcheable(const BYTE *fn);
static int hotpatch(BYTE *fn, void *gancho);
static void poner_puntero(void **donde, void *valor);
static BYTE *original_en_d3d8(BYTE *fn, BYTE ***ranura, int prof);
typedef HRESULT (WINAPI *TexLock_t)(void *, UINT, RectBloqueado8 *, const RECT *, DWORD);
typedef HRESULT (WINAPI *TexUnlock_t)(void *, UINT);
static TexLock_t g_tex_lock;
static TexUnlock_t g_tex_unlock;
#define MAX_PUB 4096
static struct { void *tex; UINT w, h, fmt; int lock_n; RectBloqueado8 lr; } P[MAX_PUB];
static int g_n_pub, g_creadas;

/* Recoloreo del publico: el juego arma sus propias paletas por tribuna a partir de la posicion de
 * cada indice (rampas de ropa) y tine todo. publico_vs.dat (de publico-vs/armar_publico.py) trae
 * los indices de cada cuadro y la paleta verdadera: se reconoce el cuadro (los pixeles con el mismo
 * indice tienen el mismo color escrito) y se reescribe con los colores verdaderos, conservando solo
 * el brillo de la tribuna. */
typedef struct { DWORD id; BYTE pal[256][3]; const BYTE *idx; } CuadroPub;
static CuadroPub *g_cuadros; static int g_n_cuadros; static UINT g_pub_w, g_pub_h;
static BYTE *g_pub_dat;

static void cargar_publico(void) {
    char ruta[MAX_PATH]; FILE *f; long tam; DWORD nf, k, c;
    BYTE *p;
    strcpy(ruta, g_ini); strcpy(strrchr(ruta, '\\') + 1, "publico_vs.dat");
    if (!(f = fopen(ruta, "rb"))) return;
    fseek(f, 0, SEEK_END); tam = ftell(f); fseek(f, 0, SEEK_SET);
    g_pub_dat = malloc(tam);
    if (!g_pub_dat || fread(g_pub_dat, 1, tam, f) != (size_t)tam || memcmp(g_pub_dat, "PVS1", 4)) {
        fclose(f); free(g_pub_dat); g_pub_dat = NULL; return;
    }
    fclose(f);
    p = g_pub_dat + 4; nf = *(DWORD *)p; p += 4;
    g_cuadros = calloc(64 * nf + 1, sizeof(CuadroPub));
    for (k = 0; k < nf; k++) {
        DWORD id = ((DWORD *)p)[0], n = ((DWORD *)p)[1];
        g_pub_w = ((DWORD *)p)[2]; g_pub_h = ((DWORD *)p)[3]; p += 16;
        for (c = 0; c < n && g_n_cuadros < 64 * (int)nf; c++) {
            CuadroPub *q = &g_cuadros[g_n_cuadros++];
            q->id = id; memcpy(q->pal, p, 768); q->idx = p + 768 + c * g_pub_w * g_pub_h;
        }
        p += 768 + n * g_pub_w * g_pub_h;
    }
    anotar("publico: %d cuadros de %ux%u para recolorear", g_n_cuadros, g_pub_w, g_pub_h);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y;
}

static void recolorear(BYTE *bits, int pitch) {
    const UINT W = g_pub_w, H = g_pub_h, PASO = 8;
    static double s1[256][3], s2[256][3], cnt[256];
    static float ratios[(1024 / 8) * (256 / 8)];
    int c, mejor = -1, nr = 0; double mejor_var = 1e30, ratio;
    UINT x, y;
    for (c = 0; c < g_n_cuadros; c++) {
        const BYTE *ix = g_cuadros[c].idx;
        int malos = 0, total = 0, j, n = 0; double var = 0;
        memset(s1, 0, sizeof s1); memset(s2, 0, sizeof s2); memset(cnt, 0, sizeof cnt);
        for (y = 0; y < H; y += PASO) for (x = 0; x < W; x += PASO) {
            const BYTE *px = bits + y * pitch + 4 * x; BYTE i = ix[y * W + x];
            int op = px[3] > 0; total++;
            if (op != (i != 255)) { malos++; continue; }
            if (!op) continue;
            for (j = 0; j < 3; j++) { s1[i][j] += px[2 - j]; s2[i][j] += (double)px[2 - j] * px[2 - j]; }
            cnt[i]++; n++;
        }
        if (malos * 100 > total * 3 || !n) continue;
        for (j = 0; j < 256; j++) if (cnt[j])
            var += s2[j][0] - s1[j][0] * s1[j][0] / cnt[j] + s2[j][1] - s1[j][1] * s1[j][1] / cnt[j] + s2[j][2] - s1[j][2] * s1[j][2] / cnt[j];
        var /= n;
        if (var < mejor_var) { mejor_var = var; mejor = c; }
    }
    if (mejor < 0 || mejor_var > 60) return;            /* no es una textura de nuestro publico */
    {
        const CuadroPub *q = &g_cuadros[mejor];
        for (y = 0; y < H; y += PASO) for (x = 0; x < W; x += PASO) {
            const BYTE *px = bits + y * pitch + 4 * x; BYTE i = q->idx[y * W + x];
            float lo, lw;
            if (!px[3] || i == 255) continue;
            lo = 0.299f * q->pal[i][0] + 0.587f * q->pal[i][1] + 0.114f * q->pal[i][2];
            lw = 0.299f * px[2] + 0.587f * px[1] + 0.114f * px[0];
            if (lo > 8) ratios[nr++] = lw / lo;
        }
        qsort(ratios, nr, sizeof(float), cmp_float);
        ratio = nr ? ratios[nr / 2] : 1.0;
        if (ratio < 0.6) ratio = 0.6; if (ratio > 1.15) ratio = 1.15;
        for (y = 0; y < H; y++) {
            BYTE *px = bits + y * pitch; const BYTE *ix = q->idx + y * W;
            for (x = 0; x < W; x++, px += 4) {
                BYTE i = ix[x]; double v;
                if (!px[3] || i == 255) continue;
                v = q->pal[i][2] * ratio; px[0] = (BYTE)(v > 255 ? 255 : v);
                v = q->pal[i][1] * ratio; px[1] = (BYTE)(v > 255 ? 255 : v);
                v = q->pal[i][0] * ratio; px[2] = (BYTE)(v > 255 ? 255 : v);
            }
        }
        if (E.diagnostico) anotar("publico: recoloreado cuadro %d de %lu (varianza %.1f, brillo %.2f)",
                                  mejor % 33, q->id, mejor_var, ratio);
    }
}

static int es_publico(UINT w, UINT h) { return (w == 1024 && h == 256) || (w == 256 && h == 64) || (w == 2048 && h == 256); }

static int buscar_pub(void *t) {
    int i;
    for (i = 0; i < g_n_pub; i++) if (P[i].tex == t) return i;
    return -1;
}

static HRESULT WINAPI h_TexLock(void *t, UINT nivel, RectBloqueado8 *lr, const RECT *r, DWORD flags) {
    HRESULT hr = g_tex_lock(t, nivel, lr, r, flags);
    int i;
    if (!J.en_efecto && SUCCEEDED(hr) && nivel == 0 && (i = buscar_pub(t)) >= 0) {
        P[i].lock_n++; P[i].lr = *lr;
        if (E.diagnostico && P[i].lock_n <= 2) anotar("publico: Lock tex %p (%ux%u fmt %u) flags %lx pitch %d", t, P[i].w, P[i].h, P[i].fmt, flags, lr->pitch);
    }
    return hr;
}

static HRESULT WINAPI h_TexUnlock(void *t, UINT nivel) {
    int i;
    if (E.diagnostico && !J.en_efecto && nivel == 0 && (i = buscar_pub(t)) >= 0 && P[i].lock_n == 1) {
        /* volcado de estudio: tex_<n>.raw (BGRA 1024x256) junto al log */
        static int volcadas;
        if (volcadas < 200) {
            char ruta[MAX_PATH]; FILE *f; UINT y;
            strcpy(ruta, g_log); strcpy(strrchr(ruta, '\\') + 1, "publico_volcado");
            CreateDirectoryA(ruta, NULL);
            sprintf(ruta + strlen(ruta), "\\tex_%03d.raw", i);
            if ((f = fopen(ruta, "wb")) != NULL) {
                for (y = 0; y < P[i].h; y++) fwrite((const BYTE *)P[i].lr.bits + y * P[i].lr.pitch, 4, P[i].w, f);
                fclose(f); volcadas++;
            }
        }
    }
    if (E.publico && g_n_cuadros && !J.en_efecto && nivel == 0 && (i = buscar_pub(t)) >= 0 && P[i].lock_n == 1 &&
        P[i].w == g_pub_w && P[i].h == g_pub_h && P[i].fmt == 21) {
        __try { recolorear(P[i].lr.bits, P[i].lr.pitch); }
        __except (EXCEPTION_EXECUTE_HANDLER) { anotar("publico: excepcion al recolorear"); E.publico = 0; }
    }
    if (E.diagnostico && !J.en_efecto && nivel == 0 && (i = buscar_pub(t)) >= 0 && P[i].lock_n <= 2) {
        const DWORD *px = (const DWORD *)P[i].lr.bits;
        UINT y = P[i].h * 3 / 4, x = P[i].w / 4;
        anotar("publico: Unlock tex %p primeros pixeles %08lx %08lx %08lx %08lx (fila %u: %08lx)",
               t, px[0], px[1], px[2], px[3], y, *(const DWORD *)((const BYTE *)P[i].lr.bits + y * P[i].lr.pitch + 4 * x));
    }
    return g_tex_unlock(t, nivel);
}

static HRESULT WINAPI h_CreateTexture(void *d, UINT w, UINT h, UINT niveles, DWORD uso, UINT fmt, UINT pool, void **tex) {
    HRESULT hr = ((CreateTexture_t)g_orig[DEV_CreateTexture])(d, w, h, niveles, uso, fmt, pool, tex);
    if (J.en_efecto) return hr;
    g_creadas++;
    if (SUCCEEDED(hr) && *tex) {
        /* el juego libera texturas y crea otras en la misma direccion: la lista se actualiza siempre */
        int i = buscar_pub(*tex);
        if (i < 0 && es_publico(w, h) && g_n_pub < MAX_PUB) i = g_n_pub++;
        if (i >= 0) {
            if (es_publico(w, h)) { P[i].tex = *tex; P[i].w = w; P[i].h = h; P[i].fmt = fmt; P[i].lock_n = 0; }
            else P[i].tex = NULL;
        }
        if (E.diagnostico && es_publico(w, h))
            anotar("publico: CreateTexture %ux%u niveles %u fmt %u pool %u tex %p (total creadas %d)", w, h, niveles, fmt, pool, *tex, g_creadas);
    }
    if (SUCCEEDED(hr) && *tex && !g_tex_lock) {          /* primera textura: enganchar Lock/Unlock */
        BYTE **r1 = NULL, **r2 = NULL;
        /* si Kitserver ya los engancho, se sigue su cadena hasta las funciones reales de d3d8 */
        BYTE *lk = original_en_d3d8((BYTE *)VT(*tex)[TEX_LockRect], &r1, 0);
        BYTE *ul = original_en_d3d8((BYTE *)VT(*tex)[TEX_UnlockRect], &r2, 0);
        void **tvt = VT(*tex);
        if (lk && ul && hotpatcheable(lk) && hotpatcheable(ul)) {
            g_tex_lock = (TexLock_t)(lk + 2); g_tex_unlock = (TexUnlock_t)(ul + 2);
            hotpatch(lk, h_TexLock); hotpatch(ul, h_TexUnlock);
            anotar("publico: Lock/Unlock de texturas enganchados (hotpatch)");
        } else if (lk && ul) {
            /* dgVoodoo: vtable de texturas fijo en la dll, o puntero donde Kitserver guarda la real */
            g_tex_lock = (TexLock_t)lk; g_tex_unlock = (TexUnlock_t)ul;
            poner_puntero(r1 ? (void **)r1 : &tvt[TEX_LockRect], h_TexLock);
            poner_puntero(r2 ? (void **)r2 : &tvt[TEX_UnlockRect], h_TexUnlock);
            anotar("publico: Lock/Unlock de texturas enganchados (puntero)");
        } else { g_tex_lock = (TexLock_t)1; anotar("publico: no pude enganchar Lock/Unlock de texturas"); }
    }
    return hr;
}

static const struct { int idx; void *fn; } GANCHOS[] = {
    {DEV_Reset, h_Reset}, {DEV_CreateTexture, h_CreateTexture}, {DEV_SetRenderTarget, h_SetRenderTarget},
    {DEV_BeginScene, h_BeginScene}, {DEV_EndScene, h_EndScene}, {DEV_Clear, h_Clear},
    {DEV_SetTransform, h_SetTransform}, {DEV_SetRenderState, h_SetRenderState}, {DEV_SetTexture, h_SetTexture},
    {DEV_DrawPrimitive, h_DrawPrimitive}, {DEV_DrawIndexedPrimitive, h_DrawIndexedPrimitive},
    {DEV_DrawPrimitiveUP, h_DrawPrimitiveUP}, {DEV_DrawIndexedPrimitiveUP, h_DrawIndexedPrimitiveUP},
    {DEV_SetVertexShader, h_SetVertexShader}, {DEV_SetTextureStageState, h_SetTextureStageState}, {DEV_SetVertexShaderConstant, h_SetVertexShaderConstant},
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
        anotar("efectos: device %p, VS %lx PS %lx, texturas simultaneas %lu, anisotropico max %lu", dev, caps[49], caps[51], caps[38], caps[27]);
    g_max_aniso = caps[27];
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
    if (E.publico) cargar_publico();
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
        BYTE *st = g_stubs + i * 12, *fn = i == DEV_Reset ? reset : i == DEV_CreateTexture ? crear : (BYTE *)vt[i];
        void *gancho = NULL;
        for (k = 0; k < N_GANCHOS; k++) if (GANCHOS[k].idx == i) gancho = GANCHOS[k].fn;
        if (gancho && !EN_D3D8(fn)) {            /* ya enganchada por Kitserver: ir a la real */
            BYTE **r = NULL, *real = original_en_d3d8(fn, &r, 0);
            if (real && hotpatcheable(real)) fn = real;
        }
        st[0] = 0xFF; st[1] = 0x05; *(volatile LONG **)(st + 2) = &g_cuenta[i];
        st[6] = 0xFF; st[7] = 0x25; *(void ***)(st + 8) = &g_destino[i];
        /* antes de activar el salto, el stub ya tiene que llevar a la original */
        if (EN_D3D8(fn) && hotpatcheable(fn)) {
            g_orig[i] = fn + 2;
            g_destino[i] = gancho ? gancho : fn + 2;
            if (hotpatch(fn, st)) { n_hot++; continue; }
        }
        if (i == DEV_CreateTexture && ranura_crear) {   /* dgVoodoo: Kitserver guarda la real ahi */
            g_orig[i] = fn;
            g_destino[i] = gancho;
            poner_puntero((void **)ranura_crear, st);
            n_vt++; continue;
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
    /* nuestras texturas propias no pasan por el gancho */
    if (g_orig[DEV_CreateTexture]) g_crear_textura = (CreateTexture_t)g_orig[DEV_CreateTexture];
    g_guia.blur_ok = 1;
    anotar("efectos: %d funciones con hotpatch, %d por puntero; ganchos sin poner:%s", n_hot, n_vt,
           lista[0] ? lista : " ninguno");
    anotar("efectos: blur %s (tecla 0x%02X; bruma, color y mascara con sus teclas); guia en pantalla: tecla 0x%02X",
           E.activo ? "activo" : "apagado", E.t_activar, E.t_guia);
    return 1;
}
