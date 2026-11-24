#include "TFT_ILI9341.h"
#include "FONTS.h"

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

void TFT_WriteCommand(uint8_t cmd)
{
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET);

    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}


void TFT_WriteData(uint8_t *data, uint16_t size)
{
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);

    HAL_SPI_Transmit(&hspi1, data, size, HAL_MAX_DELAY);

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

void TFT_Init(void)
{
    uint8_t data[5];

    // 1. Reset de hardware estricto
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(200); // Darle tiempo a la memoria interna

    // 2. Reset por software
    TFT_WriteCommand(0x01);
    HAL_Delay(1000); // 1 segundo completo para que reinicie los registros

    // 3. Power Control A
    TFT_WriteCommand(0xCB);
    data[0]=0x39; data[1]=0x2C; data[2]=0x00; data[3]=0x34; data[4]=0x02;
    TFT_WriteData(data, 5);

    // 4. Power Control B
    TFT_WriteCommand(0xCF);
    data[0]=0x00; data[1]=0xC1; data[2]=0x30;
    TFT_WriteData(data, 3);

    // 5. Driver timing control A
    TFT_WriteCommand(0xE8);
    data[0]=0x85; data[1]=0x00; data[2]=0x78;
    TFT_WriteData(data, 3);

    // 6. Power on sequence control
    TFT_WriteCommand(0xED);
    data[0]=0x64; data[1]=0x03; data[2]=0x12; data[3]=0x81;
    TFT_WriteData(data, 4);

    // 7. Pump ratio control
    TFT_WriteCommand(0xF7);
    data[0]=0x20;
    TFT_WriteData(data, 1);

    // 8. Power Control 1 & 2
    TFT_WriteCommand(0xC0); data[0]=0x23; TFT_WriteData(data, 1);
    TFT_WriteCommand(0xC1); data[0]=0x10; TFT_WriteData(data, 1);

    // 9. VCOM Control 1 & 2
    TFT_WriteCommand(0xC5); data[0]=0x3E; data[1]=0x28; TFT_WriteData(data, 2);
    TFT_WriteCommand(0xC7); data[0]=0x86; TFT_WriteData(data, 1);

    // 10. Memory Access Control (Color BGR)
    TFT_WriteCommand(0x36); data[0]=0x48; TFT_WriteData(data, 1);

    // 11. Pixel Format Set (16 bits RGB565)
    TFT_WriteCommand(0x3A); data[0]=0x55; TFT_WriteData(data, 1);

    // 12. Salir de Sleep y Encender
    TFT_WriteCommand(0x11);
    HAL_Delay(150);
    TFT_WriteCommand(0x29);
    HAL_Delay(50);
}
// Cambiamos 'int' por 'uint16_t' para máxima seguridad en los bits
void TFT_FillColor(uint16_t color)
{
    uint8_t data[4];

    // Configurar Columnas
    TFT_WriteCommand(0x2B);
    data[0] = 0; data[1] = 0; data[2] = 0; data[3] = 0xEF;
    TFT_WriteData(data, 4);

    // Configurar Filas
    TFT_WriteCommand(0x2A);
    data[0] = 0; data[1] = 0; data[2] = 0x01; data[3] = 0x3F;
    TFT_WriteData(data, 4);

    // Comenzar escritura
    TFT_WriteCommand(0x2C);

    // Pines de control
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);

    // Creamos un buffer para enviar 256 píxeles (512 bytes) a la vez en lugar de 1 por 1
    uint8_t buffer[512];
    for(uint16_t i = 0; i < 512; i += 2)
    {
        buffer[i]   = color >> 8;       // Parte alta del color
        buffer[i+1] = color & 0xFF;     // Parte baja del color
    }

    // 76800 píxeles totales / 256 píxeles por bloque = 300 transmisiones
    // Redujimos el ruido y los parones del reloj de 76,800 a solo 300.
    for(uint32_t i = 0; i < 300; i++)
    {
        HAL_SPI_Transmit(&hspi1, buffer, 512, HAL_MAX_DELAY);
    }

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

void TFT_DrawRectangle(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    // Evitar dibujar fuera de la pantalla (resolución 320x240)
    if((x >= 320) || (y >= 240)) return;
    if((x + width) > 320) width = 320 - x;
    if((y + height) > 240) height = 240 - y;

    uint8_t data[4];

    // Configurar ventana X (Columnas)
    TFT_WriteCommand(0x2A);
    data[0] = x >> 8;
    data[1] = x & 0xFF;
    data[2] = (x + width - 1) >> 8;
    data[3] = (x + width - 1) & 0xFF;
    TFT_WriteData(data, 4);

    // Configurar ventana Y (Filas)
    TFT_WriteCommand(0x2B);
    data[0] = y >> 8;
    data[1] = y & 0xFF;
    data[2] = (y + height - 1) >> 8;
    data[3] = (y + height - 1) & 0xFF;
    TFT_WriteData(data, 4);

    // Comando para iniciar escritura de memoria
    TFT_WriteCommand(0x2C);

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);

    // Preparamos el buffer optimizado de 256 píxeles
    uint8_t buffer[512];
    for(uint16_t i = 0; i < 512; i += 2)
    {
        buffer[i] = color >> 8;
        buffer[i+1] = color & 0xFF;
    }

    uint32_t totalPixels = width * height;
    uint32_t blocks = totalPixels / 256;
    uint32_t remainder = totalPixels % 256;

    // Enviar bloques completos
    for(uint32_t i = 0; i < blocks; i++)
    {
        HAL_SPI_Transmit(&hspi1, buffer, 512, HAL_MAX_DELAY);
    }

    // Enviar los píxeles sobrantes
    if(remainder > 0)
    {
        HAL_SPI_Transmit(&hspi1, buffer, remainder * 2, HAL_MAX_DELAY);
    }

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

// Dibuja un solo píxel usando la misma lógica del rectángulo
void TFT_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    TFT_DrawRectangle(x, y, 1, 1, color);
}

// Dibuja un carácter individual de 8x16 píxeles
#define FONT_COUNT (sizeof(font) / sizeof(font[0]))

// Dibuja directamente por índice del arreglo (ideal para probar glifos custom)
void TFT_DrawGlyph(uint16_t x, uint16_t y, uint16_t index, uint16_t color, uint16_t bg_color)
{
    if ((x + 8 > 320) || (y + 16 > 240)) return;
    if (index >= FONT_COUNT) return;

    uint8_t data[4];

    TFT_WriteCommand(0x2A);
    data[0] = x >> 8; data[1] = x & 0xFF;
    data[2] = (x+7) >> 8; data[3] = (x+7) & 0xFF;
    TFT_WriteData(data, 4);

    TFT_WriteCommand(0x2B);
    data[0] = y >> 8; data[1] = y & 0xFF;
    data[2] = (y+15) >> 8; data[3] = (y+15) & 0xFF;
    TFT_WriteData(data, 4);

    TFT_WriteCommand(0x2C);

    uint8_t buffer[256];
    uint16_t buf_idx = 0;

    for (uint8_t row = 0; row < 16; row++)
    {
        for (uint8_t col = 0; col < 8; col++)
        {
            uint16_t px = (font[index][col] & (1 << row)) ? color : bg_color;
            buffer[buf_idx++] = px >> 8;
            buffer[buf_idx++] = px & 0xFF;
        }
    }

    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET);
    HAL_SPI_Transmit(&hspi1, buffer, 256, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
}

void TFT_DrawChar(uint16_t x, uint16_t y, uint8_t c, uint16_t color, uint16_t bg_color)
{
    if (c < 32) return;
    TFT_DrawGlyph(x, y, c - 32, color, bg_color);
}

// Imprime una cadena de texto completa
void TFT_DrawText(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color)
{

    while (*str)
    {
        TFT_DrawChar(x, y, *str, color, bg_color);
        x += 8; // Avanzar 6 píxeles a la derecha para la siguiente letra

        // Salto de línea automático si el texto llega al borde derecho
        if (x + 8 > 320)
        {
            x = 0;
            y += 16;
        }
        str++;
    }
}
