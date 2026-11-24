#ifndef ENCODER_H
#define ENCODER_H

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

#include <stdint.h>

/* ---------------- Configuracion ---------------- */

// Transiciones de cuadratura por cada "clic" (detent) del encoder:
//   4 = la mayoria de encoders (KY-040, EC11)
//   2 = encoders con detent cada medio ciclo
#define ENCODER_PASOS_POR_DETENT   4

// Pon 1 si al girar en sentido horario el menu se mueve al reves
#define ENCODER_INVERTIR           0

// 1 = el boton conecta el pin a GND al pulsarlo (con pull-up). Lo normal.
#define ENCODER_BOTON_ACTIVO_BAJO  1

// Milisegundos que debe mantenerse estable el boton (antirrebote)
#define ENCODER_BOTON_DEBOUNCE_MS  20

/* ---------------- API ---------------- */

void    Encoder_Init(void);

// Llamar exactamente cada 1 ms (por ejemplo desde SysTick_Handler)
void    Encoder_Tick(void);

// Clics acumulados desde la ultima llamada (+ horario, - antihorario). Se borra al leer.
int8_t  Encoder_GetDelta(void);

// Devuelve 1 una sola vez por cada pulsacion del boton. Se borra al leer.
uint8_t Encoder_GetBoton(void);

// --- Lecturas que NO borran nada (para depuracion) ---
int32_t Encoder_GetPosicion(void);          // clics totales desde el arranque (+ horario)
uint8_t Encoder_BotonPresionado(void);      // 1 mientras el boton esta mantenido

// --- Pantalla de prueba en la TFT ---
void    Encoder_DebugInit(void);            // la proxima llamada limpia y repinta todo
void    Encoder_DibujarDebug(void);         // llamar en el while(1); solo repinta lo que cambia

#endif /* ENCODER_H */
