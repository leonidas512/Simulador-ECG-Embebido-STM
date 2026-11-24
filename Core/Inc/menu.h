#ifndef MENU_H
#define MENU_H

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

#include <stdint.h>

typedef enum {
    ESTADO_MENU_PRINCIPAL = 0,
    ESTADO_CONFIGURAR_PARAMETROS,
    ESTADO_MENU_PATOLOGIAS,
    ESTADO_SIMULACION,
	ESTADO_SEMILLA,
    ESTADO_MENU_ACERCA_DE
} EstadoSistema_t;

void Menu_Init(void);
void Menu_Update(int8_t encoder_delta, uint8_t boton_presionado);
void DibujarMenuPrincipal(void);
void DibujarMenuParametros(void);
void DibujarMenuPatologias(void);
void DibujarVistaSimulacion(void);
void DibujarMenuAcercaDe(void);

#endif
