/*
 * guia.c - guia en pantalla de Camara VS: esquema de la cancha visto desde arriba con la camara,
 * hacia donde mira y la pelota, barra de altura, y las teclas con sus valores (camara y blur).
 * Se pinta con GDI en un bitmap; efectos.c lo sube a una textura y lo dibuja encima del HUD.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "camara_vs.h"

#define FONDO      RGB(16, 20, 28)
#define LINEA      RGB(225, 230, 235)
#define CESPED     RGB(34, 92, 46)
#define ACENTO     RGB(255, 196, 0)
#define APAGADO    RGB(120, 128, 140)
#define ENCENDIDO  RGB(90, 220, 120)

static void nombre_tecla(int vk, char *s) {
    if (vk >= VK_F1 && vk <= VK_F24) sprintf(s, "F%d", vk - VK_F1 + 1);
    else if (vk == VK_INSERT) strcpy(s, "Insert");
    else if (vk == VK_HOME) strcpy(s, "Inicio");
    else if (vk == VK_END) strcpy(s, "Fin");
    else if (vk == VK_DELETE) strcpy(s, "Supr");
    else if (vk == VK_PRIOR) strcpy(s, "ReP");
    else if (vk == VK_NEXT) strcpy(s, "AvP");
    else if (vk > 0) sprintf(s, "0x%02X", vk);
    else strcpy(s, "-");
}

static void par_teclas(int a, int b, char *s) {
    char x[16], y[16];
    nombre_tecla(a, x); nombre_tecla(b, y);
    sprintf(s, "%s/%s", x, y);
}

static void texto(HDC dc, int x, int y, COLORREF c, const char *s) {
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

/* Una fila: tecla a la izquierda (en acento), descripcion y valor. */
static void fila(HDC dc, int x, int y, const char *tecla, const char *que, const char *valor, COLORREF cv) {
    texto(dc, x, y, ACENTO, tecla);
    texto(dc, x + 88, y, LINEA, que);
    texto(dc, x + 204, y, cv, valor);
}

/* Esquema de la cancha (vista desde arriba) en el rectangulo r. */
static void esquema(HDC dc, RECT r, const EstadoGuia *e) {
    /* metros visibles: la cancha y el riel de la camara detras de un costado */
    float mx = 62.0f, mz = 34.0f + (e->distancia > 4 ? e->distancia : 4) + 6.0f;
    float ex = (r.right - r.left) / (2 * mx), ez = (r.bottom - r.top) / (2 * mz), esc = ex < ez ? ex : ez;
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    HPEN linea = CreatePen(PS_SOLID, 2, LINEA), fina = CreatePen(PS_SOLID, 1, LINEA);
    HPEN acento = CreatePen(PS_SOLID, 2, e->en_uso ? ACENTO : APAGADO);
    HBRUSH cesped = CreateSolidBrush(CESPED), punto = CreateSolidBrush(e->en_uso ? ACENTO : APAGADO);
    HBRUSH blanco = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ viejo_pen = SelectObject(dc, linea), viejo_br = SelectObject(dc, cesped);
    /* x de la cancha -> horizontal; z -> vertical (z positivo abajo, del lado de la transmision) */
#define PX(x) (cx + (int)((x) * esc))
#define PY(z) (cy + (int)((z) * esc))
    Rectangle(dc, PX(-52.5f), PY(-34.0f), PX(52.5f), PY(34.0f));
    SelectObject(dc, fina);
    MoveToEx(dc, PX(0), PY(-34.0f), NULL); LineTo(dc, PX(0), PY(34.0f));
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, PX(-9.15f), PY(-9.15f), PX(9.15f), PY(9.15f));
    Rectangle(dc, PX(-52.5f), PY(-20.16f), PX(-36.0f), PY(20.16f));
    Rectangle(dc, PX(36.0f), PY(-20.16f), PX(52.5f), PY(20.16f));
    /* camara: lineas hacia el punto de mira y el ojo */
    {
        float ox = e->ojo[0], oz = e->ojo[2], mxp = e->mira[0], mzp = e->mira[2];
        float dx = mxp - ox, dz = mzp - oz, d = sqrtf(dx * dx + dz * dz), a = 0.35f, px, pz;
        if (d < 1.0f) d = 1.0f;
        /* cono aproximado: dos rayos a +-a rad del eje */
        SelectObject(dc, acento);
        px = ox + (dx * cosf(a) - dz * sinf(a)) * 1.4f; pz = oz + (dx * sinf(a) + dz * cosf(a)) * 1.4f;
        MoveToEx(dc, PX(ox), PY(oz), NULL); LineTo(dc, PX(px), PY(pz));
        px = ox + (dx * cosf(-a) - dz * sinf(-a)) * 1.4f; pz = oz + (dx * sinf(-a) + dz * cosf(-a)) * 1.4f;
        MoveToEx(dc, PX(ox), PY(oz), NULL); LineTo(dc, PX(px), PY(pz));
        SelectObject(dc, fina);
        MoveToEx(dc, PX(ox), PY(oz), NULL); LineTo(dc, PX(mxp), PY(mzp));
        SelectObject(dc, punto);
        Ellipse(dc, PX(ox) - 5, PY(oz) - 5, PX(ox) + 6, PY(oz) + 6);
    }
    /* pelota */
    SelectObject(dc, blanco);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, PX(e->pelota[0]) - 3, PY(e->pelota[2]) - 3, PX(e->pelota[0]) + 4, PY(e->pelota[2]) + 4);
#undef PX
#undef PY
    SelectObject(dc, viejo_pen); SelectObject(dc, viejo_br);
    DeleteObject(linea); DeleteObject(fina); DeleteObject(acento);
    DeleteObject(cesped); DeleteObject(punto); DeleteObject(blanco);
}

/* Barra de altura de la camara (0-30 m) con la marca actual. */
static void barra_altura(HDC dc, RECT r, const EstadoGuia *e) {
    HBRUSH fondo = CreateSolidBrush(RGB(40, 46, 58)), marca = CreateSolidBrush(e->en_uso ? ACENTO : APAGADO);
    float h = -e->ojo[1];
    int y;
    char s[16];
    if (h < 0) h = 0;
    if (h > 30) h = 30;
    FillRect(dc, &r, fondo);
    y = r.bottom - (int)((r.bottom - r.top) * h / 30.0f);
    {
        RECT m = {r.left - 3, y - 2, r.right + 3, y + 3};
        FillRect(dc, &m, marca);
    }
    sprintf(s, "%.0f m", -e->ojo[1]);
    texto(dc, r.left - 6, r.bottom + 2, LINEA, s);
    DeleteObject(fondo); DeleteObject(marca);
}

void guia_pintar(DWORD *px, int w, int h, const EstadoGuia *e) {
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp;
    HFONT chica, grande, vieja;
    RECT todo = {0, 0, w, h};
    char s[64], k[32];
    int x = 286, y = 36, paso = 19, i;
    HBRUSH fondo;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   /* de arriba hacia abajo */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { DeleteDC(dc); return; }
    SelectObject(dc, bmp);
    fondo = CreateSolidBrush(FONDO);
    FillRect(dc, &todo, fondo);
    DeleteObject(fondo);
    SetBkMode(dc, TRANSPARENT);
    chica = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    grande = CreateFontA(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");

    vieja = SelectObject(dc, grande);
    texto(dc, 14, 8, ACENTO, "CAMARA VS");
    texto(dc, 150, 10, APAGADO, CAMARA_VS_VERSION);
    SelectObject(dc, chica);
    if (e->camara_ok) {
        RECT r = {10, 44, 236, 214}, b = {252, 50, 259, 204};
        esquema(dc, r, e);
        barra_altura(dc, b, e);
        texto(dc, 14, 226, APAGADO, !e->camara_activa ? "camara del juego" :
              !e->en_uso ? "camara del juego (repeticion / saque)" :
              e->modo == GUIA_LATERAL ? "lateral en tercera persona" : e->tiro == 1 ? "disparo: zoom abierto" : e->tiro ? "centro / pelotazo: zoom abierto" : "siguiendo la pelota");
    } else texto(dc, 14, 60, APAGADO, "camara no disponible en este exe");

    nombre_tecla(e->t_activar, k);
    fila(dc, x, y, k, "Camara VS", e->camara_activa ? "ACTIVA" : "APAGADA", e->camara_activa ? ENCENDIDO : APAGADO);
    y += paso;
    nombre_tecla(e->t_blur, k);
    fila(dc, x, y, k, "Blur tribunas", !e->blur_ok ? "NO DISPONIBLE" : e->blur_activo == 2 ? "PRESET 2" : e->blur_activo ? "PRESET 1" : "NO",
         e->blur_ok && e->blur_activo ? ENCENDIDO : APAGADO);
    y += paso;
    nombre_tecla(e->t_bruma, s);
    fila(dc, x, y, s, "Bruma", !e->blur_ok ? "NO DISPONIBLE" : e->bruma_activa == 2 ? "PRESET 2" : e->bruma_activa ? "PRESET 1" : "NO",
         e->blur_ok && e->bruma_activa ? ENCENDIDO : APAGADO);
    y += paso;
    nombre_tecla(e->t_color, s);
    fila(dc, x, y, s, "Color/nitidez", !e->blur_ok ? "NO DISPONIBLE" : e->color_activo ? "SI" : "NO",
         e->blur_ok && e->color_activo ? ENCENDIDO : APAGADO);
    y += paso;
    {
        char v[48] = "";
        int c = e->cancha & 7;
        if (c == 7) strcpy(v, e->noche ? "TODO (noche)" : "TODO");
        else if (c == 5) strcpy(v, "SIN SOMBRA");
        else if (c) sprintf(v, "%s%s%s", c & 1 ? "F" : "", c & 2 ? "S" : "", c & 4 ? "B" : "");
        nombre_tecla(e->t_cancha, s);
        fila(dc, x, y, s, "Cancha", !e->blur_ok ? "NO DISPONIBLE" : v[0] ? v : "NO",
             e->blur_ok && e->cancha ? ENCENDIDO : APAGADO);
    }
    y += paso;
    fila(dc, x, y, "auto", "Dia / noche", !e->blur_ok ? "-" : e->noche ? (e->luces ? "NOCHE + LUCES" : "NOCHE") : "DIA",
         e->blur_ok && e->noche && e->luces ? ENCENDIDO : LINEA);
    y += paso + 4;
    par_teclas(e->t_altura_menos, e->t_altura_mas, k); sprintf(s, "%.0f m", e->altura);
    fila(dc, x, y, k, "Altura", s, LINEA); y += paso;
    par_teclas(e->t_dist_menos, e->t_dist_mas, k); sprintf(s, "%.0f m", e->distancia);
    fila(dc, x, y, k, "Distancia", s, LINEA); y += paso;
    par_teclas(e->t_zoom_abrir, e->t_zoom_cerrar, k); sprintf(s, "%.0f", e->zoom);
    fila(dc, x, y, k, "Zoom", s, LINEA); y += paso;
    par_teclas(e->t_giro_menos, e->t_giro_mas, k); sprintf(s, "%.2f", e->giro);
    fila(dc, x, y, k, "Giro arco", s, LINEA); y += paso;
    nombre_tecla(e->t_retraso, k); sprintf(s, "%.2f s", e->retraso);
    fila(dc, x, y, k, "Retraso", s, LINEA); y += paso;
    nombre_tecla(e->t_estandar, k);
    fila(dc, x, y, k, "Estandar", "", LINEA); y += paso + 6;
    nombre_tecla(e->t_guia, k);
    fila(dc, x, y, k, "Fijar guia", "", APAGADO);
    SelectObject(dc, vieja);
    DeleteObject(chica); DeleteObject(grande);

    GdiFlush();
    /* GDI no escribe alfa: panel semitransparente parejo */
    for (i = 0; i < w * h; i++) px[i] = (((DWORD *)bits)[i] & 0x00FFFFFF) | 0xD8000000;
    DeleteObject(bmp);
    DeleteDC(dc);
}
