#ifndef TFT_ILI9341_H
#define TFT_ILI9341_H

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

#include "stm32f4xx_hal.h"
#include "main.h"

// Referencia externa al SPI configurado en main.c
extern SPI_HandleTypeDef hspi1;

// Definiciones de colores RGB565
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
#define TFT_YELLOW  0xFFE0
#define TFT_CYAN    0x07FF
#define TFT_MAGENTA 0xF81F

// Dimensiones de la pantalla
#define TFT_WIDTH  320
#define TFT_HEIGHT  240

// Prototipos de funciones
void TFT_WriteCommand(uint8_t cmd);
void TFT_WriteData(uint8_t *data, uint16_t size);
void TFT_Init(void);
void TFT_FillColor(uint16_t color);
void TFT_DrawRectangle(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color);
void TFT_DrawChar(uint16_t x, uint16_t y, uint8_t c, uint16_t color, uint16_t bg_color);
void TFT_DrawText(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color);
void TFT_DrawGlyph(uint16_t x, uint16_t y, uint16_t index, uint16_t color, uint16_t bg_color);

#endif /* TFT_ILI9341_H */
