#include "encoder.h"
#include "main.h"   // HAL + macros ENCODER_DT_*, ENCODER_CLK_*, BOTON_* generados por CubeMX
#include "TFT_ILI9341.h"
#include <stdio.h>

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

#if ENCODER_INVERTIR
#define SENTIDO (+1)
#else
#define SENTIDO (-1)
#endif

/*
 * Decodificador de cuadratura por tabla de transiciones.
 * Estado = (CLK << 1) | DT. Indice de la tabla = (estado_previo << 2) | estado_actual.
 * Secuencia en un sentido: 00 -> 01 -> 11 -> 10 -> 00
 * Las transiciones invalidas (rebotes, saltos de 2 bits) valen 0 y se ignoran.
 */
static const int8_t tabla_transicion[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0
};

static volatile int16_t delta_acum   = 0;
static volatile uint8_t boton_flag   = 0;

// Solo para lectura/depuracion (nunca se borran)
static volatile int32_t  posicion         = 0;
static volatile int8_t   ultimo_sentido   = 0;   // +1 / -1 / 0 (aun no se ha movido)
static volatile uint16_t contador_botones = 0;
static uint8_t           debug_forzar     = 1;

static uint8_t estado_prev  = 0;
static int8_t  acum_pasos   = 0;

static uint8_t boton_estable = 0;   // 1 = pulsado (ya filtrado)
static uint8_t boton_cnt     = 0;

static inline uint8_t LeerEstadoEncoder(void)
{
    uint8_t clk = (HAL_GPIO_ReadPin(ENCODER_CLK_GPIO_Port, ENCODER_CLK_Pin) == GPIO_PIN_SET);
    uint8_t dt  = (HAL_GPIO_ReadPin(ENCODER_DT_GPIO_Port,  ENCODER_DT_Pin)  == GPIO_PIN_SET);
    return (uint8_t)((clk << 1) | dt);
}

static inline uint8_t LeerBotonCrudo(void)
{
    GPIO_PinState nivel = HAL_GPIO_ReadPin(BOTON_GPIO_Port, BOTON_Pin);
#if ENCODER_BOTON_ACTIVO_BAJO
    return (nivel == GPIO_PIN_RESET);
#else
    return (nivel == GPIO_PIN_SET);
#endif
}

void Encoder_Init(void)
{
    estado_prev   = LeerEstadoEncoder();
    acum_pasos    = 0;
    delta_acum    = 0;
    boton_flag    = 0;
    boton_estable = LeerBotonCrudo();   // si arranca pulsado no genera un evento falso
    boton_cnt     = 0;
}

void Encoder_Tick(void)
{
    /* ---- Rotacion ---- */
    uint8_t estado = LeerEstadoEncoder();

    if (estado != estado_prev) {
        acum_pasos += tabla_transicion[(estado_prev << 2) | estado];
        estado_prev = estado;

        if (acum_pasos >= ENCODER_PASOS_POR_DETENT) {
            delta_acum      += SENTIDO;
            posicion        += SENTIDO;
            ultimo_sentido   = SENTIDO;
            acum_pasos       = 0;
        } else if (acum_pasos <= -ENCODER_PASOS_POR_DETENT) {
            delta_acum      -= SENTIDO;
            posicion        -= SENTIDO;
            ultimo_sentido   = -SENTIDO;
            acum_pasos       = 0;
        }
#if ENCODER_PASOS_POR_DETENT == 4
        else if (estado == 0x03) {
            acum_pasos = 0;   // volvio al reposo sin completar un ciclo: era un rebote
        }
#endif
    }

    /* ---- Boton con antirrebote (el nivel debe mantenerse N ms) ---- */
    uint8_t crudo = LeerBotonCrudo();

    if (crudo != boton_estable) {
        if (++boton_cnt >= ENCODER_BOTON_DEBOUNCE_MS) {
            boton_estable = crudo;
            boton_cnt = 0;
            if (boton_estable) {                 // solo en el flanco de pulsacion
                boton_flag = 1;
                contador_botones++;
            }
        }
    } else {
        boton_cnt = 0;
    }
}

int8_t Encoder_GetDelta(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    int16_t d = delta_acum;
    delta_acum = 0;
    __set_PRIMASK(primask);

    if (d > 127)  d = 127;
    if (d < -127) d = -127;
    return (int8_t)d;
}

uint8_t Encoder_GetBoton(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint8_t b = boton_flag;
    boton_flag = 0;
    __set_PRIMASK(primask);
    return b;
}

int32_t Encoder_GetPosicion(void)
{
    return posicion;   // lectura de 32 bits alineada: atomica en Cortex-M
}

uint8_t Encoder_BotonPresionado(void)
{
    return boton_estable;
}

/* ------------------------------------------------------------------ */
/*  Pantalla de prueba                                                 */
/*  No consume Encoder_GetDelta() ni Encoder_GetBoton(), asi que puede */
/*  usarse junto con el menu.                                          */
/* ------------------------------------------------------------------ */
void Encoder_DebugInit(void)
{
    debug_forzar = 1;
}

void Encoder_DibujarDebug(void)
{
    static int32_t  pos_d;
    static int8_t   sent_d;
    static uint8_t  clk_d, dt_d, btn_d;
    static uint16_t cnt_d;

    char txt[32];

    uint8_t  estado = LeerEstadoEncoder();
    uint8_t  clk    = (estado >> 1) & 1;
    uint8_t  dt     = estado & 1;
    int32_t  pos    = posicion;
    int8_t   sent   = ultimo_sentido;
    uint8_t  btn    = boton_estable;
    uint16_t cnt    = contador_botones;

    uint8_t forzar = debug_forzar;
    if (forzar) {
        TFT_FillColor(TFT_BLACK);
        TFT_DrawText(100, 20, "TEST DE ENCODER", TFT_CYAN, TFT_BLACK);
        debug_forzar = 0;
    }

    if (forzar || pos != pos_d) {
        snprintf(txt, sizeof(txt), "Posicion    : %-8ld", (long)pos);
        TFT_DrawText(20, 60, txt, TFT_WHITE, TFT_BLACK);
        pos_d = pos;
    }

    if (forzar || sent != sent_d) {
        const char *s = (sent > 0) ? "HORARIO" : (sent < 0) ? "ANTIHORARIO" : "--";
        snprintf(txt, sizeof(txt), "Sentido     : %-12s", s);
        TFT_DrawText(20, 80, txt, TFT_WHITE, TFT_BLACK);
        sent_d = sent;
    }

    if (forzar || clk != clk_d || dt != dt_d) {
        snprintf(txt, sizeof(txt), "CLK: %u   DT: %u  ", (unsigned)clk, (unsigned)dt);
        TFT_DrawText(20, 100, txt, TFT_WHITE, TFT_BLACK);
        clk_d = clk;
        dt_d  = dt;
    }

    if (forzar || btn != btn_d) {
        snprintf(txt, sizeof(txt), "Boton       : %-10s", btn ? "PRESIONADO" : "suelto");
        // Presionado se muestra en colores invertidos para verlo de un vistazo
        TFT_DrawText(20, 120, txt,
                     btn ? TFT_BLACK : TFT_WHITE,
                     btn ? TFT_WHITE : TFT_BLACK);
        btn_d = btn;
    }

    if (forzar || cnt != cnt_d) {
        snprintf(txt, sizeof(txt), "Pulsaciones : %-5u", (unsigned)cnt);
        TFT_DrawText(20, 140, txt, TFT_WHITE, TFT_BLACK);
        cnt_d = cnt;
    }
}
