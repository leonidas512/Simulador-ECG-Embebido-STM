#include "menu.h"
#include "main.h"      /* HAL_GetTick(), SysTick, DWT */
#include "FONTS.h"
#include "TFT_ILI9341.h"
#include "sim.h"
#include <stdio.h>

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

/* Enlace a la instancia global del simulador definida en main.c */
extern SimuladorECG_t sim_ecg;

/* Control de la simulación en segundo plano (definidas en main.c) */
extern void Sim_Iniciar(uint32_t);
extern void Sim_Detener(void);
extern void Sim_Pausar(uint8_t pausar);   /* 1 = pausar, 0 = reanudar */


/* Variables de estado y navegación del menú */
static EstadoSistema_t estado_actual       = ESTADO_MENU_PRINCIPAL;
static int8_t          opcion_seleccionada = 0;
static int8_t          ultima_opcion       = -1;
static uint8_t         modo_edicion        = 0; // 0 = Navegando lista, 1 = Modificando valor/dígito
static uint8_t         en_simulacion       = 0; // 1 = simulación corriendo en segundo plano

/* Semilla: aleatoria en cada inicio (por defecto) o fija elegida a mano */
static uint8_t         semilla_manual      = 0;      // 0 = aleatoria, 1 = manual
static uint32_t        semilla_fija        = 12345u; // valor usado cuando semilla_manual = 1

/* Constantes de configuración de listas */
#define NUM_OPCIONES_PRINCIPAL   4
#define NUM_OPCIONES_PATOLOGIAS  6
#define MAX_OPCIONES_VISIBLES    5

/* Valores especiales de ultima_opcion (además de -1 = pantalla nueva):
     -2 = repintar la pantalla completa
     -3 = repintar solo el renglón que se está editando (rápido) */
#define REPINTAR_TODO      (-2)
#define REPINTAR_RENGLON   (-3)

/* Pantalla de semilla (candado de combinación) */
#define NUM_DIGITOS_SEMILLA      6      /* 000001 - 999999 */
#define SLOT_ALEATORIA           6
#define SLOT_GUARDAR             7
#define SLOT_CANCELAR            8
#define NUM_SLOTS_SEMILLA        9

static uint8_t digitos_semilla[NUM_DIGITOS_SEMILLA];

/* Índices de la lista de parámetros (el orden define el orden en pantalla) */
enum {
    PAR_PAUSA = 0,       /* Pausar / reanudar la simulación (solo activo al simular) */
    PAR_FC,              /* Frecuencia cardiaca            */
    PAR_AMPLITUD,        /* Amplitud global                */
    PAR_RUIDO_60HZ,      /* Ruido de red 60 Hz             */
    PAR_VARIABILIDAD_RR, /* Variabilidad R-R (HRV), en ms  */
    PAR_MOD_RESP,        /* Modulación cardiorrespiratoria */
    PAR_FREC_RESP,       /* Frecuencia respiratoria        */
    PAR_RSA,             /* Arritmia sinusal resp. (RSA), ms; 0 = OFF */
    PAR_DESNIVEL_ST,     /* Desnivel del segmento ST       */
    PAR_RUIDO_EMG,       /* Ruido EMG                      */
    PAR_PROB_ECTOPIA,    /* Probabilidad de ectopia        */
    PAR_SEMILLA,         /* Semilla (abre pantalla de candado; bloqueada al simular) */
    PAR_PATOLOGIAS,      /* Ir al menú de patologías sin parar la simulación */
    PAR_VOLVER,          /* Volver al menú principal / detener simulación    */
    NUM_OPCIONES_PARAMETROS
};

/* Valor que se está editando: solo se escribe en sim_ecg al aceptar con el click */
static int32_t valor_pendiente = 0;


static const char* const nombres_patologias[N_PATOLOGIAS_TOTALES] = {
    "Normal (Sinusal)",
    "Bloqueo Rama Izq (LBBB)",
    "Bloqueo Rama Der (RBBB)",
    "Extrasistole Vent (PVC)",
    "Extrasistole Auric (APB)"
};

/* ==========================================================================
 * 0. SEMILLA ALEATORIA
 *    El STM32F401 no tiene generador de números aleatorios por hardware, así
 *    que la entropía sale del instante exacto (en ciclos de reloj) en que el
 *    usuario pulsa "Iniciar", más el ID único del chip. Suficiente para un
 *    simulador (no sirve para criptografía).
 * ========================================================================== */
static uint32_t Mezclar32(uint32_t x)
{
    x ^= x >> 16;  x *= 0x85EBCA6Bu;
    x ^= x >> 13;  x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

static void Entropia_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* habilita el contador de ciclos */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t SemillaAleatoria(void)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFF7A10UL; /* ID único de 96 bits */
    uint32_t e = Mezclar32(DWT->CYCCNT);
    e = Mezclar32(e ^ HAL_GetTick());
    e = Mezclar32(e ^ SysTick->VAL);
    e = Mezclar32(e ^ uid[0]);
    e = Mezclar32(e ^ uid[1] ^ (uid[2] << 1));
    return (e % 999999u) + 1u;                        /* rango 1..999999 */
}

static void SemillaADigitos(uint32_t v)
{
    for (int i = NUM_DIGITOS_SEMILLA - 1; i >= 0; i--) {
        digitos_semilla[i] = (uint8_t)(v % 10u);
        v /= 10u;
    }
}

static uint32_t DigitosASemilla(void)
{
    uint32_t v = 0;
    for (int i = 0; i < NUM_DIGITOS_SEMILLA; i++) v = v * 10u + digitos_semilla[i];
    return v;
}

/* Fija la semilla y arranca la simulación en segundo plano.
   Sim_Iniciar() (main.c) ya aplica la semilla con Sim_SetSemilla(). */
static void IniciarSimulacion(void)
{
    uint32_t s = semilla_manual ? semilla_fija : SemillaAleatoria();
    Sim_Iniciar(s);
    en_simulacion = 1;
}

/* ==========================================================================
 * 1. VISTA: MENÚ PRINCIPAL
 * ========================================================================== */
void DibujarMenuPrincipal(void)
{
    if (opcion_seleccionada == ultima_opcion) return;

    if (ultima_opcion == -1) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawText(80, 20, "SIMULADOR ECG EMBEBIDO", TFT_CYAN, TFT_BLACK);
        TFT_DrawRectangle(20, 35, 280, 2, TFT_CYAN);
    }

    const char* opciones[NUM_OPCIONES_PRINCIPAL] = {
        "1. Iniciar Simulacion          ",
        "2. Configurar Parametros       ",
        "3. Seleccionar Patologia       ",
        "4. Acerca De                   "
    };

    int y_pos = 65;
    for (int i = 0; i < NUM_OPCIONES_PRINCIPAL; i++) {
        uint16_t c_txt   = (opcion_seleccionada == i) ? TFT_BLACK : TFT_WHITE;
        uint16_t c_fondo = (opcion_seleccionada == i) ? TFT_WHITE : TFT_BLACK;
        TFT_DrawText(20, y_pos, opciones[i], c_txt, c_fondo);
        y_pos += 30;
    }

    /* Resumen inferior del estado actual */
    char info[52];
    snprintf(info, sizeof(info), "Actual: %u BPM | %s    ",
             (unsigned)sim_ecg.frecuencia_bpm,
             nombres_patologias[sim_ecg.patologia_base]);
    TFT_DrawText(20, 210, info, TFT_CYAN, TFT_BLACK);

    ultima_opcion = opcion_seleccionada;
}

/* ==========================================================================
 * 2. VISTA: CONFIGURACIÓN DE PARÁMETROS
 *    Click  -> entra a editar el parámetro resaltado (aparece "<<")
 *    Giro   -> cambia el valor mostrado (todavía NO se aplica al simulador)
 *    Click  -> ACEPTA: el valor se escribe en sim_ecg y se vuelve a la lista
 *    La semilla tiene su propia pantalla (ver sección 6).
 * ========================================================================== */
static int32_t Limitar(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Valor actual del parámetro idx dentro del simulador */
static int32_t LeerParametro(int idx)
{
    switch (idx) {
        case PAR_FC:             return (int32_t)sim_ecg.frecuencia_bpm;
        case PAR_AMPLITUD:       return (int32_t)sim_ecg.amplitud_pct;
        case PAR_RUIDO_60HZ:     return (int32_t)sim_ecg.ruido_60hz_pct;
        case PAR_VARIABILIDAD_RR:return (int32_t)sim_ecg.variabilidad_rr_ms;
        case PAR_MOD_RESP:       return (int32_t)sim_ecg.modulacion_resp_pct;
        case PAR_FREC_RESP:      return (int32_t)sim_ecg.frec_resp_mhz;
        case PAR_RSA:            return (int32_t)sim_ecg.rsa_ms;
        case PAR_DESNIVEL_ST:    return (int32_t)sim_ecg.desnivel_st_uv;
        case PAR_RUIDO_EMG:      return (int32_t)sim_ecg.ruido_emg_pct;
        case PAR_PROB_ECTOPIA:   return (int32_t)sim_ecg.prob_ectopia_pct;
        default:                 return 0;
    }
}

/* Rango y paso por detent de cada parámetro (por defecto: porcentaje, de 5 en 5) */
static void LimitesParametro(int idx, int32_t *vmin, int32_t *vmax, int32_t *paso)
{
    *vmin = 0;  *vmax = 100;  *paso = 5;
    switch (idx) {
        case PAR_FC:         *vmin = 30;   *vmax = 220;    *paso = 1;  break; /* BPM  */
        case PAR_AMPLITUD:   *vmin = 10;   *vmax = 200;                break; /* %    */
        case PAR_VARIABILIDAD_RR:*vmin = 0; *vmax = 200;   *paso = 5;  break; /* ms   */
        case PAR_FREC_RESP:  *vmin = 150;  *vmax = 400;    *paso = 10; break; /* mHz  */
        case PAR_RSA:        *vmin = 0;    *vmax = 150;    *paso = 5;  break; /* ms   */
        case PAR_DESNIVEL_ST:*vmin = -500; *vmax = 500;    *paso = 25; break; /* uV   */
        default: break;
    }
}

/* Escribe el valor aceptado en el simulador */
static void EscribirParametro(int idx, int32_t val)
{
    int32_t vmin, vmax, paso;
    LimitesParametro(idx, &vmin, &vmax, &paso);
    val = Limitar(val, vmin, vmax);

    switch (idx) {
        case PAR_FC:              sim_ecg.frecuencia_bpm      = (uint16_t)val; break;
        case PAR_AMPLITUD:        sim_ecg.amplitud_pct        = (uint8_t)val;  break;
        case PAR_RUIDO_60HZ:      sim_ecg.ruido_60hz_pct      = (uint8_t)val;  break;
        case PAR_VARIABILIDAD_RR: sim_ecg.variabilidad_rr_ms  = (uint16_t)val; break;
        case PAR_MOD_RESP:        sim_ecg.modulacion_resp_pct = (uint8_t)val;  break;
        case PAR_FREC_RESP:       sim_ecg.frec_resp_mhz       = (uint16_t)val; break;
        case PAR_RSA:             sim_ecg.rsa_ms              = (uint16_t)val; break;
        case PAR_DESNIVEL_ST:     sim_ecg.desnivel_st_uv      = (int16_t)val;  break;
        case PAR_RUIDO_EMG:       sim_ecg.ruido_emg_pct       = (uint8_t)val;  break;
        case PAR_PROB_ECTOPIA:    sim_ecg.prob_ectopia_pct    = (uint8_t)val;  break;
        default: break;
    }
}

static void FormatearLineaParametro(int idx, char *buf, size_t len, uint8_t editando)
{
    /* Mientras se edita se muestra el valor pendiente; si no, el valor real del simulador */
    int32_t     v   = editando ? valor_pendiente : LeerParametro(idx);
    const char *ind = editando ? "<<" : "  ";

    switch (idx) {
        case PAR_PAUSA:
            if (!en_simulacion)       snprintf(buf, len, "    Pausa (solo al simular)       ");
            else if (sim_ecg.pausa)   snprintf(buf, len, "    REANUDAR simulacion  [PAUSA]  ");
            else                      snprintf(buf, len, "    PAUSAR simulacion             ");
            break;
        case PAR_FC:
            snprintf(buf, len, " 1. Frec. Cardiaca : %3ld BPM %s   ", (long)v, ind);
            break;
        case PAR_AMPLITUD:
            snprintf(buf, len, " 2. Amplitud       : %3ld %%   %s   ", (long)v, ind);
            break;
        case PAR_RUIDO_60HZ:
            snprintf(buf, len, " 3. Ruido 60Hz     : %3ld %%   %s   ", (long)v, ind);
            break;
        case PAR_VARIABILIDAD_RR:
            snprintf(buf, len, " 4. Variab. R-R    : %3ld ms  %s   ", (long)v, ind);
            break;
        case PAR_MOD_RESP:
            snprintf(buf, len, " 5. Mod. Respirat. : %3ld %%   %s   ", (long)v, ind);
            break;
        case PAR_FREC_RESP:
            snprintf(buf, len, " 6. Frec. Respirat.: %ld.%02ld Hz %s   ",
                     (long)(v / 1000), (long)((v % 1000) / 10), ind);
            break;
        case PAR_RSA:
            if (v == 0) snprintf(buf, len, " 7. Arritmia Resp. : OFF     %s   ", ind);
            else        snprintf(buf, len, " 7. Arritmia Resp. : %3ld ms  %s   ", (long)v, ind);
            break;
        case PAR_DESNIVEL_ST:
            snprintf(buf, len, " 8. Desnivel ST    : %+4ld uV %s   ", (long)v, ind);
            break;
        case PAR_RUIDO_EMG:
            snprintf(buf, len, " 9. Ruido EMG      : %3ld %%   %s   ", (long)v, ind);
            break;
        case PAR_PROB_ECTOPIA:
            snprintf(buf, len, "10. Prob. Ectopia  : %3ld %%   %s   ", (long)v, ind);
            break;
        case PAR_SEMILLA:
            if (en_simulacion) {
                /* Semilla en uso, bloqueada: se muestra para poder reproducir la corrida */
                snprintf(buf, len, "11. Semilla        : %06lu [BLOQ]   ",
                         (unsigned long)sim_ecg.semilla_usuario);
            } else if (semilla_manual) {
                snprintf(buf, len, "11. Semilla        : %06lu (manual) ", (unsigned long)semilla_fija);
            } else {
                snprintf(buf, len, "11. Semilla        : ALEATORIA       ");
            }
            break;
        case PAR_PATOLOGIAS:
            snprintf(buf, len, "    Seleccionar Patologia  >>     ");
            break;
        case PAR_VOLVER:
            snprintf(buf, len, en_simulacion ? "    Detener y volver al Menu      "
                                             : "    Volver al Menu Principal      ");
            break;
    }
}

/* Posición de la descripción bajo la lista (ajusta si se encima con la lista o la ayuda) */
#define DESC_Y1   188
#define DESC_Y2   204
#define DESC_ANCHO 36          /* caracteres por línea (relleno para borrar texto previo) */

/* Descripción de 2 líneas (máx. 36 caracteres c/u) de lo que hace cada parámetro */
static void DescripcionParametro(int idx, const char **l1, const char **l2)
{
    *l1 = ""; *l2 = "";
    switch (idx) {
        case PAR_PAUSA:
            if (!en_simulacion) {
                *l1 = "Disponible solo con la simulacion";
                *l2 = "en curso (Iniciar Simulacion).";
            } else if (sim_ecg.pausa) {
                *l1 = "Reanuda desde el mismo punto.";
                *l2 = "No cambia la semilla ni la senal.";
            } else {
                *l1 = "Congela la senal y el envio USB.";
                *l2 = "Puedes seguir editando parametros.";
            }
            break;
        case PAR_FC:
            *l1 = "Latidos por minuto del corazon.";
            *l2 = "Mayor valor = ritmo mas rapido.";
            break;
        case PAR_AMPLITUD:
            *l1 = "Tamano de toda la senal ECG.";
            *l2 = "100% = amplitud normal.";
            break;
        case PAR_RUIDO_60HZ:
            *l1 = "Interferencia de la red electrica";
            *l2 = "(60 Hz) sumada a la senal.";
            break;
        case PAR_VARIABILIDAD_RR:
            *l1 = "Desfase aleatorio de cada latido";
            *l2 = "en ms (+/-). 0 = ritmo regular.";
            break;
        case PAR_MOD_RESP:
            *l1 = "Efecto de la respiracion: la linea";
            *l2 = "base y la amplitud oscilan.";
            break;
        case PAR_FREC_RESP:
            *l1 = "Ritmo de la respiracion simulada.";
            *l2 = "0.25 Hz = 15 respiraciones/min.";
            break;
        case PAR_RSA:
            *l1 = "Sube FC al inhalar, baja al exhalar";
            *l2 = "Amplitud en ms. 0 = desactivada.";
            break;
        case PAR_DESNIVEL_ST:
            *l1 = "Eleva o hunde el segmento ST (uV).";
            *l2 = "Se asocia a isquemia o infarto.";
            break;
        case PAR_RUIDO_EMG:
            *l1 = "Ruido muscular de alta frecuencia";
            *l2 = "que ensucia la senal.";
            break;
        case PAR_PROB_ECTOPIA:
            *l1 = "Probabilidad de latidos ectopicos";
            *l2 = "(extrasistoles) en la senal.";
            break;
        case PAR_SEMILLA:
            *l1 = "Numero que fija la parte aleatoria.";
            *l2 = en_simulacion ? "Bloqueada mientras se simula."
                                : "Misma semilla = misma senal.";
            break;
        case PAR_PATOLOGIAS:
            *l1 = "Elige el tipo de ritmo o latido:";
            *l2 = "normal, bloqueos o extrasistoles.";
            break;
        case PAR_VOLVER:
            *l1 = en_simulacion ? "Detiene la simulacion y regresa"
                                : "Regresa al menu principal.";
            *l2 = en_simulacion ? "al menu principal." : "";
            break;
        default:
            break;
    }
}

static void DibujarDescripcionParametro(int idx)
{
    const char *d1, *d2;
    char buf[DESC_ANCHO + 4];

    DescripcionParametro(idx, &d1, &d2);

    snprintf(buf, sizeof(buf), "%-*s", DESC_ANCHO, d1);
    TFT_DrawText(15, DESC_Y1, buf, 0xFFE0, TFT_BLACK);   /* 0xFFE0 = amarillo RGB565 */
    snprintf(buf, sizeof(buf), "%-*s", DESC_ANCHO, d2);
    TFT_DrawText(15, DESC_Y2, buf, 0xFFE0, TFT_BLACK);
}

void DibujarMenuParametros(void)
{
    if (opcion_seleccionada == ultima_opcion) return;

    static int ventana_inicio = 0;

    /* REPINTAR_RENGLON: solo cambió el valor que se edita; no se toca título,
       ayuda ni descripción (menos escrituras SPI por cada detent del encoder) */
    uint8_t completo = (ultima_opcion != REPINTAR_RENGLON);

    if (ultima_opcion == -1) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawRectangle(15, 30, 290, 2, TFT_CYAN);
    }

    if (completo) {
        /* Título: se repinta para reflejar CURSO / PAUSA (verde / amarillo / cyan) */
        if (!en_simulacion)
            TFT_DrawText(40, 15, "PARAMETROS DE SIMULACION   ", TFT_CYAN, TFT_BLACK);
        else if (sim_ecg.pausa)
            TFT_DrawText(40, 15, "SIMULACION EN PAUSA        ", 0xFFE0, TFT_BLACK);
        else
            TFT_DrawText(40, 15, "SIMULACION EN CURSO (USB)  ", 0x07E0, TFT_BLACK);

        /* La ayuda cambia según se esté navegando o editando (relleno con espacios para borrar) */
        TFT_DrawText(15, 220,
                     modo_edicion ? "Giro: cambiar valor | Click: ACEPTAR  "
                                  : "Giro: moverse | Click: editar        ",
                     TFT_CYAN, TFT_BLACK);
    }

    if (opcion_seleccionada < ventana_inicio) {
        ventana_inicio = opcion_seleccionada;
    } else if (opcion_seleccionada >= ventana_inicio + MAX_OPCIONES_VISIBLES) {
        ventana_inicio = opcion_seleccionada - MAX_OPCIONES_VISIBLES + 1;
    }

    int y_pos = 55;
    char linea[48];

    for (int i = 0; i < MAX_OPCIONES_VISIBLES; i++) {
        int idx = ventana_inicio + i;
        if (idx >= NUM_OPCIONES_PARAMETROS) break;

        uint8_t es_seleccionada = (opcion_seleccionada == idx);
        uint8_t editando_esta   = (es_seleccionada && modo_edicion);

        if (!completo && !es_seleccionada) {   /* renglón sin cambios: no se repinta */
            y_pos += 30;
            continue;
        }

        FormatearLineaParametro(idx, linea, sizeof(linea), editando_esta);

        /* En modo edición resalta en CYAN; en navegación normal resalta en BLANCO */
        uint16_t c_txt   = es_seleccionada ? TFT_BLACK : TFT_WHITE;
        uint16_t c_fondo = editando_esta   ? TFT_CYAN  : (es_seleccionada ? TFT_WHITE : TFT_BLACK);

        TFT_DrawText(15, y_pos, linea, c_txt, c_fondo);
        y_pos += 30;
    }

    /* Descripción del parámetro resaltado */
    if (completo) DibujarDescripcionParametro(opcion_seleccionada);

    ultima_opcion = opcion_seleccionada;
}

/* Ajuste inmediato de BPM desde la vista de simulación en vivo (no pasa por aceptar) */
static void AjustarBPMEnVivo(int8_t delta)
{
    int32_t vmin, vmax, paso;
    LimitesParametro(PAR_FC, &vmin, &vmax, &paso);
    EscribirParametro(PAR_FC, Limitar((int32_t)sim_ecg.frecuencia_bpm + delta, vmin, vmax));
}

/* ==========================================================================
 * 3. VISTA: SELECCIÓN DE PATOLOGÍA CARDÍACA
 * ========================================================================== */
void DibujarMenuPatologias(void)
{
    if (opcion_seleccionada == ultima_opcion) return;

    if (ultima_opcion == -1) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawText(50, 15, "SELECCIONAR PATOLOGIA", TFT_CYAN, TFT_BLACK);
        TFT_DrawRectangle(15, 30, 290, 2, TFT_CYAN);
    }

    char linea[48];
    int y_pos = 50;

    for (int i = 0; i < NUM_OPCIONES_PATOLOGIAS; i++) {
        uint16_t c_txt   = (opcion_seleccionada == i) ? TFT_BLACK : TFT_WHITE;
        uint16_t c_fondo = (opcion_seleccionada == i) ? TFT_WHITE : TFT_BLACK;

        if (i < N_PATOLOGIAS_TOTALES) {
            char marca = (sim_ecg.patologia_base == (PatologiaECG_t)i) ? '*' : ' ';
            snprintf(linea, sizeof(linea), "[%c] %d. %-26s", marca, i + 1, nombres_patologias[i]);
        } else {
            snprintf(linea, sizeof(linea), en_simulacion ? "    6. Volver a Parametros       "
                                                         : "    6. Volver al Menu Principal  ");
        }

        TFT_DrawText(15, y_pos, linea, c_txt, c_fondo);
        y_pos += 26;
    }

    ultima_opcion = opcion_seleccionada;
}

/* ==========================================================================
 * 4. VISTA: SIMULACIÓN EN VIVO (OSCILOSCOPIO + TELEMETRÍA USB)
 *    (ya no se usa: la simulación corre en segundo plano desde la vista de parámetros)
 * ========================================================================== */
#define Y_SUP_GRAF   42
#define Y_INF_GRAF   215
#define Y_CEN_GRAF   135
#define ALTO_GRAF    130

static uint8_t  y_min_hist[320];
static uint8_t  y_max_hist[320];
static uint16_t x_cursor_sim = 0;
static uint8_t  y_prev_sim   = Y_CEN_GRAF;
static uint8_t  sim_vista_lista = 0;

static void DibujarTrazoVertical(uint16_t x, uint8_t y1, uint8_t y2, uint16_t color)
{
    uint8_t min_y = (y1 < y2) ? y1 : y2;
    uint8_t max_y = (y1 > y2) ? y1 : y2;
    TFT_DrawRectangle(x, min_y, 1, (max_y - min_y) + 1, color);
}

static void ActualizarEncabezadoSimulacion(void)
{
    char txt[52];
    snprintf(txt, sizeof(txt), "%3u BPM | %-16s | %s ",
             (unsigned)sim_ecg.frecuencia_bpm,
             nombres_patologias[sim_ecg.patologia_base],
             sim_ecg.pausa ? "PAUSA" : "USB+TFT");
    TFT_DrawText(10, 12, txt, TFT_CYAN, TFT_BLACK);
}

void DibujarVistaSimulacion(void)
{
    if (!sim_vista_lista) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawRectangle(0, 35, 320, 2, TFT_CYAN);
        TFT_DrawRectangle(0, 218, 320, 2, TFT_CYAN);
        TFT_DrawText(10, 225, "Giro: Ajustar BPM | Click: Volver al Menu", TFT_WHITE, TFT_BLACK);

        for (int i = 0; i < 320; i++) {
            y_min_hist[i] = Y_CEN_GRAF;
            y_max_hist[i] = Y_CEN_GRAF;
        }
        x_cursor_sim = 0;
        y_prev_sim   = Y_CEN_GRAF;
        sim_vista_lista = 1;
        ActualizarEncabezadoSimulacion();
    }

    if (sim_ecg.pausa) return;

    /* Trazar la última muestra calculada usando TFT_DrawRectangle optimizado */
    uint8_t y_actual = Sim_MapearAPantallaY(sim_ecg.ultima_muestra_adc, Y_CEN_GRAF, ALTO_GRAF);
    if (y_actual < Y_SUP_GRAF) y_actual = Y_SUP_GRAF;
    if (y_actual > Y_INF_GRAF) y_actual = Y_INF_GRAF;

    uint16_t x_adelante = (x_cursor_sim + 1) % 320;
    DibujarTrazoVertical(x_adelante, y_min_hist[x_adelante], y_max_hist[x_adelante], TFT_BLACK);
    DibujarTrazoVertical(x_cursor_sim, y_min_hist[x_cursor_sim], y_max_hist[x_cursor_sim], TFT_BLACK);

    if (x_cursor_sim == 0) y_prev_sim = y_actual;

    /* 0x07E0 = Verde médico brillante en formato RGB565 */
    DibujarTrazoVertical(x_cursor_sim, y_prev_sim, y_actual, 0x07E0);

    y_min_hist[x_cursor_sim] = (y_prev_sim < y_actual) ? y_prev_sim : y_actual;
    y_max_hist[x_cursor_sim] = (y_prev_sim > y_actual) ? y_prev_sim : y_actual;

    y_prev_sim   = y_actual;
    x_cursor_sim = x_adelante;
}

/* ==========================================================================
 * 5. VISTA: ACERCA DE
 * ========================================================================== */
void DibujarMenuAcercaDe(void)
{
    if (ultima_opcion != -1) return;

    TFT_FillColor(TFT_BLACK);
    TFT_DrawText(70, 20, "ACERCA DEL PROYECTO", TFT_CYAN, TFT_BLACK);
    TFT_DrawRectangle(20, 35, 280, 2, TFT_CYAN);

    TFT_DrawText(20, 60,  "Desarrollo de un simulador ECG", TFT_WHITE, TFT_BLACK);
    TFT_DrawText(20, 75,  "embebido para formacion academica", TFT_WHITE, TFT_BLACK);
    TFT_DrawText(20, 110, "MCU : STM32F401CEU6 (500 Hz)", TFT_CYAN, TFT_BLACK);
    TFT_DrawText(20, 130, "Gen : Procedural Markov + HRV", TFT_CYAN, TFT_BLACK);
    TFT_DrawText(20, 150, "Out : USB CDC + TFT ILI9341", TFT_CYAN, TFT_BLACK);
    TFT_DrawText(20, 205, "[Presiona el boton para volver]", TFT_WHITE, TFT_BLACK);

    ultima_opcion = 0;
}

/* ==========================================================================
 * 6. VISTA: SEMILLA (CANDADO DE COMBINACIÓN)
 *    Giro (navegando) -> mueve el cursor: 6 dígitos, ALEATORIA, GUARDAR, CANCELAR
 *    Click en dígito  -> lo selecciona para editar (caja cyan)
 *    Giro (editando)  -> cambia ese dígito 0-9 (da la vuelta como un candado)
 *    Click (editando) -> confirma el dígito y pasa al siguiente
 * ========================================================================== */
static void DibujarCajaDigito(int i)
{
    uint8_t  sel    = (opcion_seleccionada == i);
    uint8_t  edit   = (sel && modo_edicion);
    uint16_t fondo  = edit ? TFT_CYAN : (sel ? TFT_WHITE : TFT_BLACK);
    uint16_t c_txt  = sel ? TFT_BLACK : TFT_WHITE;
    uint16_t x      = 52 + (uint16_t)i * 36;
    char     d[2]   = { (char)('0' + digitos_semilla[i]), '\0' };

    TFT_DrawRectangle(x,     85, 28, 36, TFT_CYAN);   /* borde  */
    TFT_DrawRectangle(x + 2, 87, 24, 32, fondo);      /* interior */
    TFT_DrawText(x + 10, 98, d, c_txt, fondo);
}

void DibujarPantallaSemilla(void)
{
    if (opcion_seleccionada == ultima_opcion) return;

    if (ultima_opcion == -1) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawText(60, 15, "SEMILLA DEL SIMULADOR", TFT_CYAN, TFT_BLACK);
        TFT_DrawRectangle(15, 30, 290, 2, TFT_CYAN);
    }

    char linea[48];

    snprintf(linea, sizeof(linea), "Modo actual: %-16s", semilla_manual ? "MANUAL" : "ALEATORIA");
    TFT_DrawText(52, 55, linea, TFT_WHITE, TFT_BLACK);

    for (int i = 0; i < NUM_DIGITOS_SEMILLA; i++) DibujarCajaDigito(i);

    static const char* const botones[3] = {
        "  ALEATORIA (cada inicio)",
        "  GUARDAR (semilla manual)",
        "  CANCELAR"
    };
    int y_pos = 140;
    for (int i = 0; i < 3; i++) {
        uint8_t  sel   = (opcion_seleccionada == NUM_DIGITOS_SEMILLA + i);
        uint16_t c_txt = sel ? TFT_BLACK : TFT_WHITE;
        uint16_t fondo = sel ? TFT_WHITE : TFT_BLACK;
        snprintf(linea, sizeof(linea), "%-28s", botones[i]);
        TFT_DrawText(52, y_pos, linea, c_txt, fondo);
        y_pos += 24;
    }

    snprintf(linea, sizeof(linea), "%-38s",
             modo_edicion ? "Giro: cambiar digito | Click: OK"
                          : "Giro: moverse | Click: elegir");
    TFT_DrawText(15, 220, linea, TFT_CYAN, TFT_BLACK);

    ultima_opcion = opcion_seleccionada;
}

static void AbrirPantallaSemilla(void)
{
    SemillaADigitos(semilla_manual ? semilla_fija : (uint32_t)sim_ecg.semilla_usuario);
    estado_actual       = ESTADO_SEMILLA;
    opcion_seleccionada = 0;
    ultima_opcion       = -1;
    modo_edicion        = 0;
}

static void CerrarPantallaSemilla(void)
{
    estado_actual       = ESTADO_CONFIGURAR_PARAMETROS;
    opcion_seleccionada = PAR_SEMILLA;
    ultima_opcion       = -1;
    modo_edicion        = 0;
}

/* ==========================================================================
 * 7. MÁQUINA DE ESTADOS PRINCIPAL Y CONTROLADOR DEL ENCODER
 * ========================================================================== */
void Menu_Init(void)
{
    Entropia_Init();
    estado_actual       = ESTADO_MENU_PRINCIPAL;
    opcion_seleccionada = 0;
    ultima_opcion       = -1;
    modo_edicion        = 0;
    en_simulacion       = 0;
    semilla_manual      = 0;
    sim_vista_lista     = 0;
    TFT_FillColor(TFT_BLACK);
}

void Menu_Update(int8_t encoder_delta, uint8_t boton_presionado)
{
    /* 1. Procesar el giro del encoder según el estado y modo actual */
    if (encoder_delta != 0) {
        if (estado_actual == ESTADO_SEMILLA && modo_edicion) {
            /* Candado: el giro cambia solo el dígito seleccionado, con vuelta 9 <-> 0 */
            int d = (int)digitos_semilla[opcion_seleccionada] + encoder_delta;
            d %= 10;
            if (d < 0) d += 10;
            digitos_semilla[opcion_seleccionada] = (uint8_t)d;
            ultima_opcion = REPINTAR_TODO;
        }
        else if (estado_actual == ESTADO_CONFIGURAR_PARAMETROS && modo_edicion) {
            /* Modo edición: el giro solo cambia el valor pendiente; se aplica al aceptar con el click */
            int32_t vmin, vmax, paso;
            LimitesParametro(opcion_seleccionada, &vmin, &vmax, &paso);
            valor_pendiente = Limitar(valor_pendiente + (int32_t)encoder_delta * paso, vmin, vmax);
            ultima_opcion = REPINTAR_RENGLON; // Repinta solo el renglón editado
        }
        else if (estado_actual == ESTADO_SIMULACION) {
            /* En simulación en vivo, girar el encoder permite ajustar los BPM al vuelo */
            AjustarBPMEnVivo(encoder_delta);
            ActualizarEncabezadoSimulacion();
        }
        else {
            /* Navegación estándar por las listas de opciones */
            int8_t max_opc = NUM_OPCIONES_PRINCIPAL;
            if (estado_actual == ESTADO_CONFIGURAR_PARAMETROS) max_opc = NUM_OPCIONES_PARAMETROS;
            if (estado_actual == ESTADO_MENU_PATOLOGIAS)       max_opc = NUM_OPCIONES_PATOLOGIAS;
            if (estado_actual == ESTADO_SEMILLA)               max_opc = NUM_SLOTS_SEMILLA;

            opcion_seleccionada += encoder_delta;
            if (opcion_seleccionada < 0)        opcion_seleccionada = max_opc - 1;
            if (opcion_seleccionada >= max_opc) opcion_seleccionada = 0;
        }
    }

    /* 2. Procesar la pulsación del botón */
    if (boton_presionado) {
        switch (estado_actual) {
            case ESTADO_MENU_PRINCIPAL:
                if (opcion_seleccionada == 0) {
                    /* Iniciar: fija la semilla (aleatoria o manual), arranca en segundo plano
                       y abre la pantalla de parámetros */
                    IniciarSimulacion();
                    estado_actual       = ESTADO_CONFIGURAR_PARAMETROS;
                    opcion_seleccionada = PAR_PAUSA;   // queda sobre el botón de pausa
                    ultima_opcion       = -1;
                    modo_edicion        = 0;
                } else if (opcion_seleccionada == 1) {
                    estado_actual       = ESTADO_CONFIGURAR_PARAMETROS;
                    opcion_seleccionada = PAR_FC;      // sin simular, la pausa no aplica
                    ultima_opcion       = -1;
                    modo_edicion        = 0;
                } else if (opcion_seleccionada == 2) {
                    estado_actual       = ESTADO_MENU_PATOLOGIAS;
                    opcion_seleccionada = (int8_t)sim_ecg.patologia_base;
                    ultima_opcion       = -1;
                } else if (opcion_seleccionada == 3) {
                    estado_actual = ESTADO_MENU_ACERCA_DE;
                    ultima_opcion = -1;
                }
                break;

            case ESTADO_CONFIGURAR_PARAMETROS:
                if (modo_edicion) {
                    /* 2º click: ACEPTAR el valor pendiente */
                    if (valor_pendiente != LeerParametro(opcion_seleccionada)) {
                        EscribirParametro(opcion_seleccionada, valor_pendiente);
                    }
                    modo_edicion  = 0;
                    ultima_opcion = REPINTAR_TODO;
                } else if (opcion_seleccionada == PAR_PAUSA) {
                    /* Pausar / reanudar (sin efecto si la simulación no está corriendo) */
                    if (en_simulacion) {
                        Sim_Pausar(sim_ecg.pausa ? 0u : 1u);
                        ultima_opcion = REPINTAR_TODO;   // repinta el renglón, el título y la descripción
                    }
                } else if (opcion_seleccionada == PAR_SEMILLA) {
                    /* Con la simulación en curso la semilla está bloqueada: el click no hace nada */
                    if (!en_simulacion) AbrirPantallaSemilla();
                } else if (opcion_seleccionada == PAR_PATOLOGIAS) {
                    /* Ir a patologías sin detener la simulación */
                    estado_actual       = ESTADO_MENU_PATOLOGIAS;
                    opcion_seleccionada = (int8_t)sim_ecg.patologia_base;
                    ultima_opcion       = -1;
                } else if (opcion_seleccionada == PAR_VOLVER) {
                    if (en_simulacion) {
                        /* Detener la simulación y volver al menú, sobre "Iniciar Simulacion" */
                        Sim_Detener();
                        en_simulacion       = 0;
                        opcion_seleccionada = 0;
                    } else {
                        opcion_seleccionada = 1;
                    }
                    estado_actual = ESTADO_MENU_PRINCIPAL;
                    ultima_opcion = -1;
                } else {
                    /* 1er click: editar el parámetro resaltado, partiendo de su valor actual */
                    valor_pendiente = LeerParametro(opcion_seleccionada);
                    modo_edicion    = 1;
                    ultima_opcion   = REPINTAR_TODO; // Refrescar color de la barra y la ayuda
                }
                break;

            case ESTADO_SEMILLA:
                if (opcion_seleccionada < NUM_DIGITOS_SEMILLA) {
                    if (modo_edicion) {
                        /* Confirma el dígito y salta al siguiente (el último se queda) */
                        modo_edicion = 0;
                        if (opcion_seleccionada < NUM_DIGITOS_SEMILLA - 1) opcion_seleccionada++;
                    } else {
                        modo_edicion = 1;
                    }
                    ultima_opcion = REPINTAR_TODO;
                } else if (opcion_seleccionada == SLOT_ALEATORIA) {
                    semilla_manual = 0;           /* se generará una nueva al pulsar Iniciar */
                    CerrarPantallaSemilla();
                } else if (opcion_seleccionada == SLOT_GUARDAR) {
                    uint32_t v = DigitosASemilla();
                    if (v < 1u) v = 1u;           /* 000000 no es válida */
                    semilla_fija   = v;
                    semilla_manual = 1;
                    CerrarPantallaSemilla();
                } else {                          /* SLOT_CANCELAR */
                    CerrarPantallaSemilla();
                }
                break;

            case ESTADO_MENU_PATOLOGIAS:
                if (opcion_seleccionada < N_PATOLOGIAS_TOTALES) {
                    Sim_SetPatologia(&sim_ecg, (PatologiaECG_t)opcion_seleccionada);
                    ultima_opcion = REPINTAR_TODO; // Actualizar el marcador [*]
                } else {
                    /* Opción "Volver": a parámetros si hay simulación, si no al menú principal */
                    if (en_simulacion) {
                        estado_actual       = ESTADO_CONFIGURAR_PARAMETROS;
                        opcion_seleccionada = PAR_PATOLOGIAS;
                        modo_edicion        = 0;
                    } else {
                        estado_actual       = ESTADO_MENU_PRINCIPAL;
                        opcion_seleccionada = 2;
                    }
                    ultima_opcion = -1;
                }
                break;

            case ESTADO_SIMULACION:
                estado_actual       = ESTADO_MENU_PRINCIPAL;
                opcion_seleccionada = 0;
                ultima_opcion       = -1;
                break;

            case ESTADO_MENU_ACERCA_DE:
                estado_actual       = ESTADO_MENU_PRINCIPAL;
                opcion_seleccionada = 3;
                ultima_opcion       = -1;
                break;
        }
    }

    /* 3. Renderizar la pantalla activa */
    switch (estado_actual) {
        case ESTADO_MENU_PRINCIPAL:
            DibujarMenuPrincipal();
            break;
        case ESTADO_CONFIGURAR_PARAMETROS:
            DibujarMenuParametros();
            break;
        case ESTADO_SEMILLA:
            DibujarPantallaSemilla();
            break;
        case ESTADO_MENU_PATOLOGIAS:
            DibujarMenuPatologias();
            break;
        case ESTADO_SIMULACION:
            DibujarVistaSimulacion();
            break;
        case ESTADO_MENU_ACERCA_DE:
            DibujarMenuAcercaDe();
            break;
    }
}
