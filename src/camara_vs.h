/*
 * camara_vs.h - estado compartido entre la camara (camara_vs.c), los efectos (efectos.c) y la
 * guia en pantalla (guia.c).
 */
#ifndef CAMARA_VS_H
#define CAMARA_VS_H

#include <windows.h>

#define CAMARA_VS_VERSION "1.2.1"

enum { GUIA_JUEGO, GUIA_LATERAL, GUIA_ARQUERO };

typedef struct {
    /* camara */
    int camara_ok;            /* se encontraron las firmas de la camara */
    int camara_activa;        /* F9 */
    int en_uso;               /* la camara VS esta puesta en este momento (no la del juego) */
    int modo, tiro;           /* tiro: 1 = disparo, 2 = centro o pelotazo */
    float altura, distancia, zoom, retraso, giro;   /* ajustes (m, m, foco, s, 0..1) */
    float ojo[3], mira[3], pelota[3];        /* en metros: x largo, y vertical (negativo arriba), z ancho */
    int t_activar, t_altura_menos, t_altura_mas, t_dist_menos, t_dist_mas,
        t_zoom_abrir, t_zoom_cerrar, t_retraso, t_estandar, t_giro_menos, t_giro_mas;
    /* efectos */
    int blur_ok, blur_activo, bruma_activa, color_activo, t_blur, t_bruma, t_color, t_guia;
    /* GetTickCount de la ultima tecla: la guia se muestra unos segundos */
    DWORD tecla_ms;
} EstadoGuia;

extern EstadoGuia g_guia;

void anotar(const char *fmt, ...);
int efectos_iniciar(const char *ini);
/* Pinta la guia en px (w x h, 32 bits BGRA, de arriba hacia abajo). */
void guia_pintar(DWORD *px, int w, int h, const EstadoGuia *e);

#endif
