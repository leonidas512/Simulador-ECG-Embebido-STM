#ifndef SIM_H
#define SIM_H

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

#include <stdint.h>

/*
 * Nota importante: plantillas_ecg.h define arreglos globales sin 'static'.
 * Para evitar errores de múltiple definición en el linker, plantillas_ecg.h
 * solo se incluye dentro de sim.c, y aquí replicamos únicamente las constantes
 * públicas y el enum si no se ha incluido previamente.
 */
#ifndef PLANTILLAS_ECG_H
#define GANANCIA_DIGITAL 200
#define FRECUENCIA_MUESTREO_HZ 500
#define PRE_R_MUESTRAS 180
#define LONGITUD_PLANTILLA 512
#define N_PLANTILLAS_POR_CLASE 20
#define N_PATOLOGIAS_TOTALES 5

typedef enum {
    PATOLOGIA_NORMAL = 0,
    PATOLOGIA_BLOQUEO_RAMA_IZQ_LBBB = 1,
    PATOLOGIA_BLOQUEO_RAMA_DER_RBBB = 2,
    PATOLOGIA_PVC = 3,
    PATOLOGIA_APB_AURICULAR = 4,
} PatologiaECG_t;
#endif

typedef struct {
    /* --- Parámetros configurables desde el Menú --- */
    uint16_t       frecuencia_bpm;        // 30 a 220 BPM (Defecto: 70)
    uint8_t        amplitud_pct;          // 10 a 200 %   (Defecto: 100)
    uint8_t        ruido_60hz_pct;        // 0 a 100 %    (Defecto: 0)
    uint8_t        variabilidad_rr_pct;   // 0 a 100 %    (Defecto: 25)
    uint8_t        modulacion_resp_pct;   // 0 a 100 %    (Defecto: 20)
    uint8_t        ruido_emg_pct;         // 0 a 100 %    (Defecto: 0)
    uint8_t        prob_ectopia_pct;      // 0 a 100 % probabilidad de PVC/APB esporádico
    int16_t        desnivel_st_uv;        // -500 a +500 uV desnivel del segmento ST (Defecto: 0)
    uint16_t       frec_resp_mhz;         // 150 a 400 mHz frecuencia respiratoria (Defecto: 250)
    uint32_t       semilla_usuario;       // Semilla base para reproducibilidad
    uint8_t        pausa;                 // 0 = Activo, 1 = Pausado
    PatologiaECG_t patologia_base;        // Patología principal seleccionada

    /* --- Estado interno del PRNG (4 bytes) --- */
    uint32_t       rng_state;             // Estado Xorshift32

    /* --- Estado cinemático y morfológico del latido actual --- */
    PatologiaECG_t patologia_actual;
    uint8_t        idx_plantilla_a;       // Índice de plantilla primaria (0..4)
    uint8_t        idx_plantilla_b;       // Índice de plantilla secundaria (0..4)
    float          peso_mezcla;           // Factor baricéntrico [0.0, 1.0]
    float          escala_latido;         // Micro-variación de amplitud latido a latido
    float          fase_plantilla;        // Posición sub-muestra dentro de [0, 255]
    float          paso_plantilla;        // Velocidad de lectura (ajuste tipo Bazett)
    uint8_t        en_complejo_activo;    // 1 = leyendo plantilla, 0 = segmento isoeléctrico TP
    uint8_t        pausa_compensatoria;   // 1 = el próximo latido aplica pausa post-ectópica

    /* --- Temporización R-R y moduladores autónomos (LF / HF / AR1) --- */
    uint32_t       muestra_en_rr;         // Contador de muestras en el ciclo R-R actual
    uint32_t       rr_objetivo_muestras;  // Duración total del ciclo R-R actual en muestras
    uint32_t       inicio_plantilla_muestras; // Muestra donde inicia la onda P del latido
    float          hrv_ar1_estado;        // Memoria del proceso estocástico AR(1)
    float          fase_mayer_lf;         // Fase de ondas de Mayer (~0.10 Hz)
    float          fase_resp_hf;          // Fase cardiorrespiratoria (~0.25 Hz)
    float          fase_60hz;             // Fase del interferente de red (60 Hz)

    /* --- Estado del filtro IIR Biquad Pasa-Banda (20-150 Hz) para EMG --- */
    float          emg_z1;
    float          emg_z2;

    /* --- Salidas recientes --- */
    int16_t        ultima_muestra_adc;    // Valor listo para DAC / PWM / Telemetría
    uint8_t        ultimo_pixel_y;        // Coordenada Y escalada para TFT ILI9341
    uint8_t        flag_pico_r;           // Se pone a 1 exactamente en el pico R

    int32_t variabilidad_rr_ms;
    int32_t rsa_ms ;
} SimuladorECG_t;

/* API Pública */
void    Sim_Init(SimuladorECG_t *sim, uint32_t semilla_inicial);
void    Sim_SetSemilla(SimuladorECG_t *sim, uint32_t nueva_semilla);
void    Sim_SetPatologia(SimuladorECG_t *sim, PatologiaECG_t patologia);
int16_t Sim_Step(SimuladorECG_t *sim);
uint8_t Sim_MapearAPantallaY(int16_t muestra_digital, uint16_t y_centro, uint16_t alto_util);

#endif /* SIM_H */
