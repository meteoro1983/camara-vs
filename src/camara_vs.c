/*
 * camara_vs.c - Camara VS: camara de juego estilo Virtua Striker para Winning Eleven 9 y
 * Winning Eleven 9 Liveware Evolution (PC). Modulo para Kitserver 5 (kload.cfg: DLL.N).
 *
 * Como funciona
 *   La camara del juego vive en un struct (ojo, objetivo, zoom, plano lejano). El director de
 *   camaras del juego la suaviza a partir de lo que hay en ese struct, asi que no se puede
 *   escribir ahi: el juego partiria de nuestra camara en el cuadro siguiente (realimentacion).
 *   En cambio se envuelve la funcion que arma la matriz de vista para dibujar:
 *     al entrar -> se guarda la camara del juego, se calcula y se pone la nuestra
 *     al salir  -> se devuelve la del juego
 *   El juego nunca ve nuestra camara; solo la pantalla.
 *
 *   La camara sigue a la pelota desde un riel detras de la linea de costado (como VS4), con un
 *   leve giro lateral cerca de los arcos que se acomoda de frente mientras el mismo jugador
 *   conserva la pelota (steadycam). Laterales en tercera persona; con la pelota en manos del
 *   arquero y en los saques de arco queda la camara del juego. Repeticiones y festejos tambien.
 *
 *   El juego decide que partes del estadio dibujar con SU camara (apaga el cielo si mira hacia
 *   abajo y oculta tribunas segun donde esta): mientras la camara VS esta activa se anula esa
 *   decision para que no se vea negro arriba de las tribunas.
 *
 * Todo se busca por firmas (WE9 y WE9LE comparten el motor), nada de direcciones fijas.
 * Ejes del juego: x a lo largo de la cancha, y vertical (negativo = arriba), z a lo ancho;
 * unas 256 unidades por metro.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "1.0.0"
#define M 256.0f

/* ---------------------------------------------------------------- firmas */

typedef struct {
    const char *nombre;
    const char *patron;   /* bytes en hex, ?? = comodin */
    int off;              /* desde el inicio del patron hasta el dato que se lee */
} Firma;

/* Funcion que arma la vista: push ebp; mov ebp,esp; and esp,-16; sub esp,60h;
 * test byte [flags],8; jnz ...; call CalcDir; fld [ojo] */
static const Firma F_VISTA = {"vista",
    "55 8B EC 83 E4 F0 83 EC 60 F6 05 ?? ?? ?? ?? 08 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? D9 05", 0};
#define VISTA_OFF_FLAGS 11
#define VISTA_OFF_OJO   29
/* Puntero global al objeto pelota (posicion en +0x10, la del cuadro anterior en +0x1C). */
static const Firma F_PELOTA = {"pelota", "8B 0D ?? ?? ?? ?? 33 DB 88 1D", 2};
/* Fase del partido: 1 en juego, 2 preparar pelota parada, 3/9 laterales y saques, 6 falta. */
static const Firma F_FASE = {"fase", "2B CE 8B D1 57 C1 E9 02 33 C0 8D BE ?? ?? ?? ?? F3 AB", 12};
/* Ultimo jugador que toco la pelota: 0-10 local, 11-21 visitante (0 y 11 = arqueros). */
static const Firma F_TOQUE = {"ultimo toque", "8A 56 07 88 15 ?? ?? ?? ?? 8A 46 08 A2", 5};
/* Final de la funcion de visibilidad del estadio: mascara de tribunas ocultas (esi) y la
 * llamada que apaga el cielo cuando la camara del juego mira hacia abajo. */
static const Firma F_ESTADIO = {"estadio",
    "BE C0 00 00 00 E8 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 03 83 CE 0C "
    "E8 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0 F6 C4 41 75 03 83 CE 03 56 E8 ?? ?? ?? ?? "
    "83 C4 04 E8 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 05 E8", 0};

/* Parches del estadio, relativos al inicio de F_ESTADIO. */
typedef struct { int off; int n; BYTE nuevo[2]; BYTE original[2]; } Parche;
static Parche g_parches[] = {
    {0x01, 1, {0x00}},        /* mov esi,0xC0 -> mov esi,0 */
    {0x19, 1, {0x00}},        /* or esi,0xC   -> or esi,0  */
    {0x2E, 1, {0x00}},        /* or esi,3     -> or esi,0  */
    {0x4A, 2, {0xEB, 0x03}},  /* call (apaga el cielo) -> jmp corto por encima */
};
#define N_PARCHES (sizeof(g_parches) / sizeof(g_parches[0]))

static BYTE *g_texto; static SIZE_T g_texto_tam;

static int parsear(const char *p, BYTE *bytes, BYTE *mascara) {
    int n = 0;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (p[0] == '?') { bytes[n] = 0; mascara[n] = 0; }
        else { bytes[n] = (BYTE)strtoul((char[3]){p[0], p[1], 0}, NULL, 16); mascara[n] = 1; }
        n++; p += 2;
    }
    return n;
}

/* Devuelve la direccion del patron si aparece UNA sola vez en el codigo del exe. */
static BYTE *buscar(const Firma *f, int *coincidencias) {
    BYTE b[128], m[128];
    int n = parsear(f->patron, b, m), c = 0;
    BYTE *hallado = NULL;
    SIZE_T i; int k;
    for (i = 0; i + n <= g_texto_tam; i++) {
        if (g_texto[i] != b[0]) continue;
        for (k = 1; k < n; k++)
            if (m[k] && g_texto[i + k] != b[k]) break;
        if (k == n) { c++; hallado = g_texto + i; }
    }
    if (coincidencias) *coincidencias = c;
    return c == 1 ? hallado : NULL;
}

static DWORD leer_dato(const Firma *f, int *ok) {
    int c; BYTE *p = buscar(f, &c);
    *ok = p != NULL;
    return p ? *(DWORD *)(p + f->off) : 0;
}

/* ---------------------------------------------------------------- registro y ajustes */

static char g_ini[MAX_PATH], g_log[MAX_PATH];

static void anotar(const char *fmt, ...) {
    FILE *f = fopen(g_log, "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

typedef struct {
    float altura, distancia, mira, retraso, adelanto, giro, centrado, zoom;
    int activa;
    int t_activar, t_altura_menos, t_altura_mas, t_dist_menos, t_dist_mas,
        t_zoom_abrir, t_zoom_cerrar, t_retraso;
} Ajustes;
static Ajustes A;

static float ini_f(const char *clave, float def) {
    char s[64], d[64];
    sprintf(d, "%g", def);
    GetPrivateProfileStringA("camara", clave, d, s, sizeof s, g_ini);
    return (float)atof(s);
}
static int ini_tecla(const char *clave, int def) {
    char s[64], d[64];
    sprintf(d, "0x%02X", def);
    GetPrivateProfileStringA("teclas", clave, d, s, sizeof s, g_ini);
    return (int)strtol(s, NULL, 0);
}
static void ini_guardar(const char *clave, float v) {
    char s[64];
    sprintf(s, "%g", v);
    WritePrivateProfileStringA("camara", clave, s, g_ini);
}

static void leer_ajustes(void) {
    A.altura    = ini_f("altura", 11.0f);     /* m sobre el piso */
    A.distancia = ini_f("distancia", 10.0f);  /* m detras de la linea de costado */
    A.mira      = ini_f("mira", 0.0f);        /* m por delante de la pelota hacia el lado lejano */
    A.retraso   = ini_f("retraso", 0.35f);    /* s de retraso del travelling */
    A.adelanto  = ini_f("adelanto", 0.25f);   /* s que se adelanta el objetivo segun la velocidad */
    A.giro      = ini_f("giro", 0.35f);       /* giro lateral maximo cerca de los arcos */
    A.centrado  = ini_f("centrado", 2.5f);    /* s de posesion para acomodarse de frente */
    A.zoom      = ini_f("zoom", 470.0f);      /* foco: menos = mas abierto */
    A.activa    = GetPrivateProfileIntA("camara", "activa", 1, g_ini);
    A.t_activar      = ini_tecla("activar", VK_F9);
    A.t_altura_menos = ini_tecla("altura_menos", VK_F7);
    A.t_altura_mas   = ini_tecla("altura_mas", VK_F8);
    A.t_dist_menos   = ini_tecla("distancia_menos", VK_F10);
    A.t_dist_mas     = ini_tecla("distancia_mas", VK_F11);
    A.t_zoom_abrir   = ini_tecla("zoom_abrir", VK_F3);
    A.t_zoom_cerrar  = ini_tecla("zoom_cerrar", VK_F4);
    A.t_retraso      = ini_tecla("retraso", VK_F6);
}

/* ---------------------------------------------------------------- estado del juego */

static BYTE *g_vista;            /* funcion que arma la vista */
static volatile BYTE *g_flags;   /* bit 0 = direccion ya calculada */
static float *g_ojo;             /* ojo x,y,z; objetivo en +0x10; zoom en -0x18; plano lejano en -0x1C */
static DWORD *g_pelota;          /* puntero al objeto pelota */
static BYTE *g_fase, *g_toque;
static BYTE *g_estadio;          /* NULL si no se reconocio la funcion de visibilidad */

#define MEDIO_LARGO_M 50.0f      /* la pelota anda por x = +-49 m cerca de los arcos */
#define MEDIO_ANCHO_M 34.0f      /* y por z = +-34 m en las lineas de costado */
#define DIST_REF_M    45.0f      /* distancia ojo-pelota para la que vale el zoom tal cual */
#define SEGUIR_ANCHO  0.6f       /* fraccion del movimiento a lo ancho que sigue el riel */
#define PLANO_LEJANO  60000.0f   /* el juego usa 16000 (~62 m): las tomas bajas miran mas lejos */
#define ZOOM_LATERAL  650.0f
#define MIRA_TECHO_M  8.0f       /* en los globos la toma no termina mirando el cielo */

enum { JUEGO, LATERAL, ARQUERO };

static int fase_con_camara(int f) { return f == 1 || f == 2 || f == 3 || f == 6 || f == 9; }
static int fase_saque(int f) { return f == 3 || f == 9; }
static int es_arquero(int d) { return d == 0 || d == 11; }

/* Camara del juego guardada al entrar a la funcion de vista, y la nuestra. */
static float g_juego[8], g_nuevo[8];
static int g_puesta;             /* nuestra camara esta en el struct ahora */
static int g_estadio_puesto = -1;

static struct {
    int iniciado, modo_ant, dueno_ant;
    float s_obj[3], s_ojo[3], giro, posesion, en_manos_t;
    LARGE_INTEGER antes;
} S;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static int tecla(int vk) {
    static BYTE abajo[256];
    int ahora = (GetAsyncKeyState(vk) & 0x8000) != 0, flanco;
    if (vk <= 0 || vk > 255) return 0;
    flanco = ahora && !abajo[vk];
    abajo[vk] = (BYTE)ahora;
    return flanco;
}

static int juego_en_foco(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void escribir_codigo(BYTE *va, const BYTE *datos, int n) {
    DWORD viejo;
    VirtualProtect(va, n, PAGE_EXECUTE_READWRITE, &viejo);
    if (n == 2 && ((UINT_PTR)va & 1) == 0) *(volatile WORD *)va = *(const WORD *)datos;
    else memcpy(va, datos, n);
    VirtualProtect(va, n, viejo, &viejo);
    FlushInstructionCache(GetCurrentProcess(), va, n);
}

static void poner_estadio(int completo) {
    unsigned i;
    if (!g_estadio || completo == g_estadio_puesto) return;
    g_estadio_puesto = completo;
    for (i = 0; i < N_PARCHES; i++)
        escribir_codigo(g_estadio + g_parches[i].off,
                        completo ? g_parches[i].nuevo : g_parches[i].original, g_parches[i].n);
}

static void teclas(void) {
    if (!juego_en_foco()) return;
    if (tecla(A.t_activar)) {
        A.activa = !A.activa; S.iniciado = 0;
        WritePrivateProfileStringA("camara", "activa", A.activa ? "1" : "0", g_ini);
    }
    if (tecla(A.t_altura_menos)) { A.altura = A.altura > 2.0f ? A.altura - 1.0f : 1.0f; ini_guardar("altura", A.altura); }
    if (tecla(A.t_altura_mas))   { A.altura += 1.0f; ini_guardar("altura", A.altura); }
    if (tecla(A.t_dist_menos))   { A.distancia = A.distancia > 6.0f ? A.distancia - 2.0f : 4.0f; ini_guardar("distancia", A.distancia); }
    if (tecla(A.t_dist_mas))     { A.distancia += 2.0f; ini_guardar("distancia", A.distancia); }
    if (tecla(A.t_zoom_abrir))   { A.zoom = A.zoom * 0.9f > 200.0f ? A.zoom * 0.9f : 200.0f; ini_guardar("zoom", A.zoom); }
    if (tecla(A.t_zoom_cerrar))  { A.zoom = A.zoom / 0.9f < 4000.0f ? A.zoom / 0.9f : 4000.0f; ini_guardar("zoom", A.zoom); }
    if (tecla(A.t_retraso)) {
        static const float r[] = {0.2f, 0.35f, 0.6f, 1.0f};
        int i, sig = 1;
        for (i = 0; i < 4; i++) if (fabsf(A.retraso - r[i]) < 0.01f) sig = (i + 1) % 4;
        A.retraso = r[sig]; ini_guardar("retraso", A.retraso);
    }
}

/* Calcula nuestra camara a partir de la del juego (g_juego) y la pelota. Devuelve 1 si va. */
static int calcular(void) {
    float pel[3], ant[3], vel[3], q_obj[3], q_ojo[3], mira[3];
    float dt, rapidez, retraso, k, lado, lado_cancha, relajado, q_giro, riel, sep, dist, zoom;
    int fase, dueno, modo, en_manos, i;
    LARGE_INTEGER ahora, frec;
    BYTE *b = (BYTE *)(UINT_PTR)*g_pelota;

    QueryPerformanceCounter(&ahora); QueryPerformanceFrequency(&frec);
    dt = S.antes.QuadPart ? (float)(ahora.QuadPart - S.antes.QuadPart) / (float)frec.QuadPart : 0.016f;
    dt = clampf(dt, 1e-4f, 0.1f);
    S.antes = ahora;
    teclas();

    memcpy(pel, g_juego + 3, sizeof pel); memcpy(ant, pel, sizeof ant);
    if (b) {
        const float *p = (const float *)(b + 0x10);
        int ok = 1;
        for (i = 0; i < 6; i++) if (!_finite(p[i])) ok = 0;
        if (ok && fabsf(p[0]) < 40000.0f && fabsf(p[2]) < 40000.0f) {
            memcpy(pel, p, sizeof pel); memcpy(ant, p + 3, sizeof ant);
        }
    }
    for (i = 0; i < 3; i++) vel[i] = (pel[i] - ant[i]) * 60.0f;   /* unidades por segundo */
    rapidez = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]) / M;      /* m/s en el plano */
    fase = *g_fase;
    dueno = *g_toque;

    /* Tomas: lateral en tercera persona; con el arquero (pelota en sus manos o saque de arco)
     * va la camara del juego. */
    if (fase_saque(fase) && fabsf(pel[2]) > (MEDIO_ANCHO_M - 4) * M && fabsf(pel[0]) < (MEDIO_LARGO_M - 3) * M)
        modo = LATERAL;
    else if (fase_saque(fase) && fabsf(pel[0]) > (MEDIO_LARGO_M - 8) * M && fabsf(pel[2]) < (MEDIO_ANCHO_M - 4) * M)
        modo = ARQUERO;
    else if (S.en_manos_t > 0.4f || (S.modo_ant == ARQUERO && rapidez < 3.0f))
        modo = ARQUERO;
    else
        modo = JUEGO;
    /* Arquero con la pelota en las manos: la toco un arquero, cerca de su arco, casi quieta y
     * a la altura de las manos. */
    en_manos = es_arquero(dueno) && fabsf(pel[0]) > (MEDIO_LARGO_M - 18) * M && rapidez < 0.6f && pel[1] < -0.6f * M;
    S.en_manos_t = en_manos ? S.en_manos_t + dt : 0.0f;
    S.modo_ant = modo;

    q_obj[0] = pel[0] + vel[0] * A.adelanto;
    q_obj[1] = 0.5f * pel[1] - 0.5f * M;
    q_obj[2] = pel[2] + vel[2] * A.adelanto;
    if (modo == LATERAL) {
        float afuera = pel[2] > 0 ? 1.0f : -1.0f;
        q_obj[0] = pel[0]; q_obj[1] = -1.2f * M; q_obj[2] = pel[2] - afuera * 15.0f * M;
    }
    if (!S.iniciado) memcpy(S.s_obj, q_obj, sizeof q_obj);
    retraso = modo == JUEGO ? A.retraso : 0.45f;
    k = 1.0f - expf(-dt / (retraso > 0.01f ? retraso : 0.01f));
    for (i = 0; i < 3; i++) S.s_obj[i] += (q_obj[i] - S.s_obj[i]) * k;

    /* Ojo: del lado de la transmision del juego, siguiendo a la pelota a lo largo de la cancha.
     * Giro lateral (steadycam): cerca de un arco el ojo se corre hacia el centro; mientras el
     * mismo jugador conserva la pelota se acomoda de frente. */
    lado = g_juego[2] >= g_juego[5] ? 1.0f : -1.0f;
    if (dueno != S.dueno_ant) { S.dueno_ant = dueno; S.posesion = 0.0f; }
    S.posesion += dt;
    lado_cancha = clampf(S.s_obj[0] / (MEDIO_LARGO_M * M), -1.0f, 1.0f);
    relajado = expf(-S.posesion / (A.centrado > 0.1f ? A.centrado : 0.1f));
    q_giro = A.giro * lado_cancha * (0.25f + 0.75f * relajado);
    S.giro += (q_giro - S.giro) * (1.0f - expf(-dt / 1.2f));
    /* Riel detras de la linea de costado que acompana a medias el ancho (como VS4). */
    riel = lado * (MEDIO_ANCHO_M + A.distancia) * M + SEGUIR_ANCHO * S.s_obj[2];
    sep = fabsf(riel - S.s_obj[2]);
    q_ojo[0] = S.s_obj[0] - S.giro * sep; q_ojo[1] = -A.altura * M; q_ojo[2] = riel;
    mira[0] = S.s_obj[0]; mira[1] = S.s_obj[1]; mira[2] = S.s_obj[2] - lado * A.mira * M;
    {
        float techo = -(MIRA_TECHO_M < 0.5f * A.altura ? MIRA_TECHO_M : 0.5f * A.altura) * M;
        if (mira[1] < techo) mira[1] = techo;
    }
    if (modo == LATERAL) {
        /* A la altura del hombro del que saca, mirando hacia adentro de la cancha. */
        float afuera = pel[2] > 0 ? 1.0f : -1.0f;
        q_ojo[0] = pel[0]; q_ojo[1] = -2.5f * M; q_ojo[2] = pel[2] + afuera * 6.0f * M;
        memcpy(mira, S.s_obj, sizeof mira);
    }
    if (!S.iniciado) memcpy(S.s_ojo, q_ojo, sizeof q_ojo);
    S.iniciado = 1;
    k = 1.0f - expf(-dt / (retraso * 1.4f > 0.01f ? retraso * 1.4f : 0.01f));
    for (i = 0; i < 3; i++) S.s_ojo[i] += (q_ojo[i] - S.s_ojo[i]) * k;

    /* Zoom compensado por la distancia real: jugadores de tamano parecido en toda la cancha. */
    dist = sqrtf((S.s_ojo[0] - mira[0]) * (S.s_ojo[0] - mira[0]) + (S.s_ojo[1] - mira[1]) * (S.s_ojo[1] - mira[1]) +
                 (S.s_ojo[2] - mira[2]) * (S.s_ojo[2] - mira[2])) / (M * DIST_REF_M);
    zoom = modo == LATERAL ? ZOOM_LATERAL : A.zoom * clampf(dist, 0.6f, 1.8f);
    memcpy(g_nuevo, S.s_ojo, 12); memcpy(g_nuevo + 3, mira, 12);
    g_nuevo[6] = zoom; g_nuevo[7] = PLANO_LEJANO;

    /* Solo con la pelota en juego o parada; repeticiones y festejos quedan del juego. */
    return A.activa && b && fase_con_camara(fase) && modo != ARQUERO;
}

/* Struct de camara <-> buffer: ojo xyz, objetivo xyz, zoom, plano lejano. */
static void struct_a(float *d) {
    memcpy(d, g_ojo, 12); memcpy(d + 3, g_ojo + 4, 12);
    d[6] = g_ojo[-6]; d[7] = g_ojo[-7];
}
static void a_struct(const float *s) {
    memcpy(g_ojo, s, 12); memcpy(g_ojo + 4, s + 3, 12);
    g_ojo[-6] = s[6]; g_ojo[-7] = s[7];
    *g_flags &= (BYTE)~1;   /* que recalcule la direccion */
}

static void __cdecl al_entrar(void) {
    int va;
    __try {
        struct_a(g_juego);
        va = calcular();
        poner_estadio(va);
        if (va) a_struct(g_nuevo);
        g_puesta = va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_puesta = 0;
    }
}

static void __cdecl al_salir(void) {
    if (g_puesta) { a_struct(g_juego); g_puesta = 0; }
}

/* ---------------------------------------------------------------- enganche */

static DWORD g_ret;          /* direccion de retorno original (la funcion no es reentrante) */
static BYTE *g_trampolin;    /* 6 bytes robados + jmp al resto de la funcion */
static BYTE g_robados[8];

static __declspec(naked) void gancho(void) {
    __asm {
        pushad
        pushfd
        call al_entrar
        popfd
        popad
        pop g_ret             ; la funcion vuelve aca (mismos argumentos en la pila)
        call g_trampolin
        pushad
        pushfd
        call al_salir
        popfd
        popad
        jmp g_ret
    }
}

static int enganchar(void) {
    static const BYTE inicio[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    LONGLONG viejo, nuevo;
    BYTE *n = (BYTE *)&nuevo;
    DWORD prot;
    if (memcmp(g_vista, inicio, 6) != 0 || ((UINT_PTR)g_vista & 7) != 0) return 0;
    g_trampolin = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampolin) return 0;
    memcpy(g_trampolin, inicio, 6);
    g_trampolin[6] = 0xE9;
    *(LONG *)(g_trampolin + 7) = (LONG)((g_vista + 6) - (g_trampolin + 11));
    /* jmp gancho + nop en los primeros 6 bytes, escrito de una sola vez (8 bytes alineados). */
    viejo = *(volatile LONGLONG *)g_vista;
    memcpy(g_robados, &viejo, 8);
    nuevo = viejo;
    n[0] = 0xE9;
    *(LONG *)(n + 1) = (LONG)((BYTE *)gancho - (g_vista + 5));
    n[5] = 0x90;
    VirtualProtect(g_vista, 8, PAGE_EXECUTE_READWRITE, &prot);
    InterlockedCompareExchange64((volatile LONGLONG *)g_vista, nuevo, viejo);
    VirtualProtect(g_vista, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), g_vista, 8);
    return 1;
}

static void desenganchar(void) {
    DWORD prot;
    if (!g_trampolin) return;
    VirtualProtect(g_vista, 8, PAGE_EXECUTE_READWRITE, &prot);
    InterlockedExchange64((volatile LONGLONG *)g_vista, *(LONGLONG *)g_robados);
    VirtualProtect(g_vista, 8, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), g_vista, 8);
    poner_estadio(0);
}

/* ---------------------------------------------------------------- arranque */

static int texto_del_exe(void) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    g_texto = base + s->VirtualAddress;
    g_texto_tam = s->Misc.VirtualSize;
    return g_texto_tam > 0x10000;
}

static DWORD WINAPI iniciar(LPVOID nada) {
    int intento, c, ok1, ok2, ok3;
    unsigned i;
    BYTE *v = NULL;
    (void)nada;
    anotar("Camara VS %s", VERSION);
    if (!texto_del_exe()) { anotar("no encontre el codigo del exe"); return 0; }
    /* El codigo puede estar todavia protegido al cargar el modulo: se espera hasta 3 minutos. */
    for (intento = 0; intento < 360 && !(v = buscar(&F_VISTA, &c)); intento++) Sleep(500);
    if (!v) { anotar("firma de la vista: %d coincidencias (1 esperada) -> exe no soportado", c); return 0; }
    g_vista = v;
    g_flags = (BYTE *)(UINT_PTR)*(DWORD *)(v + VISTA_OFF_FLAGS);
    g_ojo = (float *)(UINT_PTR)*(DWORD *)(v + VISTA_OFF_OJO);
    g_pelota = (DWORD *)(UINT_PTR)leer_dato(&F_PELOTA, &ok1);
    g_fase = (BYTE *)(UINT_PTR)leer_dato(&F_FASE, &ok2);
    g_toque = (BYTE *)(UINT_PTR)leer_dato(&F_TOQUE, &ok3);
    anotar("vista %p, flags %p, ojo %p, pelota %p, fase %p, ultimo toque %p",
         g_vista, g_flags, g_ojo, g_pelota, g_fase, g_toque);
    if (!ok1 || !ok2 || !ok3) { anotar("faltan firmas de datos -> exe no soportado"); return 0; }
    g_estadio = buscar(&F_ESTADIO, &c);
    if (g_estadio) {
        for (i = 0; i < N_PARCHES; i++) memcpy(g_parches[i].original, g_estadio + g_parches[i].off, g_parches[i].n);
        anotar("estadio %p: cielo y tribunas completas con la camara VS", g_estadio);
    } else {
        anotar("no reconozco la funcion de visibilidad del estadio (%d): puede verse negro arriba", c);
    }
    S.dueno_ant = -1; S.modo_ant = JUEGO;
    if (!enganchar()) { anotar("no se pudo enganchar la funcion de vista"); return 0; }
    anotar("listo. %s = activar/desactivar (ahora %s)", "F9", A.activa ? "activa" : "apagada");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD motivo, LPVOID reservado) {
    if (motivo == DLL_PROCESS_ATTACH) {
        char *p;
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_ini, MAX_PATH);
        strcpy(g_log, g_ini);
        if ((p = strrchr(g_ini, '.')) != NULL) strcpy(p, ".ini");
        if ((p = strrchr(g_log, '.')) != NULL) strcpy(p, ".log");
        DeleteFileA(g_log);
        leer_ajustes();
        CloseHandle(CreateThread(NULL, 0, iniciar, NULL, 0, NULL));
    } else if (motivo == DLL_PROCESS_DETACH && reservado == NULL) {
        desenganchar();
    }
    return TRUE;
}
