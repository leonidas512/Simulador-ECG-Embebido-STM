#include "sim.h"

/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Leo Osio
 */

/*
 * Inclusión única de las plantillas en Flash (.rodata).
 * Asegúrate de cerrar plantillas_ecg.h con #endif al final del archivo.
 */
#include "plantillas_ecg.h"

#define DOS_PI          6.283185307f
#define FS_HZ           ((float)FRECUENCIA_MUESTREO_HZ)
#define VENTANA_BORDE   20.0f  // Muestras de suavizado en extremos de plantilla

/* Segmento ST (en muestras de plantilla contadas desde el pico R) */
#define ST_INICIO       28.0f  // Justo después del QRS
#define ST_SUBIDA       12.0f  // Rampa suave de subida
#define ST_FIN          70.0f  // Fin de la meseta
#define ST_BAJADA       40.0f  // Rampa suave de bajada (se funde con la onda T)
#define GANANCIA_LATIDO_NORMAL  1.5f



/* Ventana suave [0..1] del desnivel ST en función de las muestras desde el pico R */
static inline float VentanaST(float r)
{
    float u;
    if (r <= ST_INICIO || r >= (ST_FIN + ST_BAJADA)) return 0.0f;
    if (r < (ST_INICIO + ST_SUBIDA)) {
        u = (r - ST_INICIO) * (1.0f / ST_SUBIDA);
        return u * u * (3.0f - 2.0f * u);
    }
    if (r < ST_FIN) return 1.0f;
    u = (r - ST_FIN) * (1.0f / ST_BAJADA);
    return 1.0f - u * u * (3.0f - 2.0f * u);
}

/* Puntero maestro a los bancos de plantillas definidos en plantillas_ecg.h */
static const int16_t* const* const BANCOS_PATOLOGIAS[N_PATOLOGIAS_TOTALES] = {
    LATIDOS_NORMAL,
    LATIDOS_BLOQUEO_RAMA_IZQ_LBBB,
    LATIDOS_BLOQUEO_RAMA_DER_RBBB,
    LATIDOS_PVC,
    LATIDOS_APB_AURICULAR
};

/* ==========================================================================
 * 0. NORMALIZACIÓN DE AMPLITUD DE LAS PLANTILLAS
 *    Las plantillas de una misma clase vienen con alturas de R muy distintas
 *    (hasta 2.5x entre ellas). Al elegir y mezclar plantillas al azar, eso hacía
 *    que la amplitud cambiara mucho de un latido a otro. Aquí se calcula, una
 *    sola vez, una ganancia por plantilla que lleva su R a la altura media de
 *    su clase: cada clase conserva su amplitud típica (p. ej. PVC > APB) pero
 *    todas las plantillas de la clase quedan con la misma altura de R.
 * ========================================================================== */
#define R_MINIMO_VALIDO   20.0f   /* si la R medida es menor, no se normaliza esa plantilla */

static float s_ganancia_norm[N_PATOLOGIAS_TOTALES][N_PLANTILLAS_POR_CLASE];

/* Altura de R sobre la línea base (misma corrección de base que usa Sim_Step) */
static float AlturaR(const int16_t *t)
{
    /* Se cambia 255.0f por la longitud real menos 1 (511.0f) */
    float pendiente = (float)(t[LONGITUD_PLANTILLA - 1] - t[0]) * (1.0f / (float)(LONGITUD_PLANTILLA - 1));
    float base = (float)t[0] + pendiente * (float)PRE_R_MUESTRAS;
    return (float)t[PRE_R_MUESTRAS] - base;
}

static void NormalizarPlantillas(void)
{
    for (uint8_t p = 0; p < N_PATOLOGIAS_TOTALES; p++) {
        float suma = 0.0f;
        uint8_t n = 0;
        float r[N_PLANTILLAS_POR_CLASE];

        for (uint8_t i = 0; i < N_PLANTILLAS_POR_CLASE; i++) {
            r[i] = AlturaR(BANCOS_PATOLOGIAS[p][i]);
            if (r[i] > R_MINIMO_VALIDO) { suma += r[i]; n++; }
        }
        float objetivo = (n > 0u) ? (suma / (float)n) : 1.0f;
        float factor_clase = (p == (uint8_t)PATOLOGIA_NORMAL) ? GANANCIA_LATIDO_NORMAL : 1.0f;

        for (uint8_t i = 0; i < N_PLANTILLAS_POR_CLASE; i++) {
            float ganancia_base = (r[i] > R_MINIMO_VALIDO) ? (objetivo / r[i]) : 1.0f;
            s_ganancia_norm[p][i] = ganancia_base * factor_clase;
        }
    }
}

/* ==========================================================================
 * 1. SISTEMA DE SEMILLAS Y PRNG ULTRA-LIGERO (SplitMix32 + Xorshift32)
 * ========================================================================== */

/* Blanqueador de semilla inicial: dispersa semillas cercanas (ej. 1, 2, 3) */
static uint32_t SplitMix32_Init(uint32_t x)
{
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    x = x ^ (x >> 16);
    return (x == 0u) ? 0xA5A5A5A5u : x; // Xorshift nunca debe recibir 0
}

/* PRNG principal: 4 bytes de estado, ~4 ciclos de reloj en Cortex-M4 */
static inline uint32_t PRNG_NextU32(SimuladorECG_t *sim)
{
    uint32_t x = sim->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sim->rng_state = x;
    return x;
}

/* Flotante uniforme en [0.0f, 1.0f] */
static inline float PRNG_Uniform01(SimuladorECG_t *sim)
{
    return (float)(PRNG_NextU32(sim) >> 8) * (1.0f / 16777216.0f);
}

/* Aproximación Gaussiana N(0,1) por Irwin-Hall usando los 4 bytes de un solo U32 */
static inline float PRNG_Gaussian(SimuladorECG_t *sim)
{
    uint32_t r = PRNG_NextU32(sim);
    int32_t suma = (int32_t)(r & 0xFFu) +
                   (int32_t)((r >> 8)  & 0xFFu) +
                   (int32_t)((r >> 16) & 0xFFu) +
                   (int32_t)((r >> 24) & 0xFFu);
    /* Media de 4 bytes = 4 * 127.5 = 510. Desviación estándar ~ 147.8 */
    return (float)(suma - 510) * (1.0f / 147.8f);
}

/* ==========================================================================
 * 2. APROXIMACIÓN TRIGONOMÉTRICA RÁPIDA (Cortex-M4F sin tablas pesadas)
 * ========================================================================== */

/* Aproximación parabólica de sin(x) para x en [0, 2*PI] con corrección de precisión */
static inline float Fast_Sin(float x)
{
    if (x > 3.14159265f) {
        x -= DOS_PI;
    }
    /* Fórmula de Bhaskara / parábola con refinamiento de cuarto orden */
    float y = 1.27323954f * x - 0.405284735f * x * ((x < 0.0f) ? -x : x);
    return 0.225f * (y * ((y < 0.0f) ? -y : y) - y) + y;
}

/* ==========================================================================
 * 3. PLANIFICADOR BIOMÉDICO DE LATIDOS (Markov + HRV + Bazett)
 * ========================================================================== */

static void PlanificarSiguienteLatido(SimuladorECG_t *sim)
{
    /* 1. Selección Markoviana de patología / ectopia */
    sim->patologia_actual = sim->patologia_base;
    uint8_t es_ectopico = 0;

    if (!sim->pausa_compensatoria && sim->prob_ectopia_pct > 0) {
        uint32_t sorteo = PRNG_NextU32(sim) % 100u;
        if (sorteo < sim->prob_ectopia_pct) {
            /* Alterna entre extrasístole ventricular (PVC) y auricular (APB) */
            sim->patologia_actual = (sorteo & 1u) ? PATOLOGIA_PVC : PATOLOGIA_APB_AURICULAR;
            es_ectopico = 1;
        }
    }

    /* 2. Selección estocástica de 2 plantillas distintas para interpolación morfológica */
    uint32_t r = PRNG_NextU32(sim);
    sim->idx_plantilla_a = (uint8_t)(r % N_PLANTILLAS_POR_CLASE);
    sim->idx_plantilla_b = (uint8_t)((sim->idx_plantilla_a + 1u + ((r >> 8) % (N_PLANTILLAS_POR_CLASE - 1u))) % N_PLANTILLAS_POR_CLASE);

    /* Peso de mezcla baricéntrica en [0.15, 0.85] (antes salía en [0.40, 1.15]: con peso > 1
       la plantilla A se restaba y deformaba el latido) y micro-variación de amplitud (+/- 1.5%) */
    sim->peso_mezcla   = 0.15f + 0.70f * PRNG_Uniform01(sim);
    sim->escala_latido = 0.985f + 0.030f * PRNG_Uniform01(sim);

    /* 3. Cálculo del intervalo R-R basal en muestras */
    uint16_t bpm = sim->frecuencia_bpm;
    if (bpm < 30u)  bpm = 30u;
    if (bpm > 220u) bpm = 220u;
    float rr_medio = (60.0f * FS_HZ) / (float)bpm;

    /* 4. Modulación autonómica del R-R, expresada en MILISEGUNDOS de desfase:
     *    - HRV: variabilidad_rr_ms = desfase máximo (pico) respecto al R-R medio.
     *           Mezcla 60% onda de Mayer (LF) + 40% proceso AR(1) aleatorio.
     *    - RSA: rsa_ms = amplitud de la arritmia sinusal respiratoria (0 = desactivada).
     *           Sigue la fase respiratoria: al inhalar el R-R se acorta (sube la FC)
     *           y al exhalar se alarga (baja la FC). */
    const float ms_a_muestras = FS_HZ * 0.001f;
    float dev_hrv = (float)sim->variabilidad_rr_ms * ms_a_muestras;
    float dev_rsa = (float)sim->rsa_ms * ms_a_muestras;

    sim->hrv_ar1_estado = 0.82f * sim->hrv_ar1_estado + 0.18f * PRNG_Gaussian(sim);

    /* El AR(1) tiene desviación estándar ~0.31: se escala (x1.05) para que 3 sigma ~ 1 y se acota a [-1, 1] */
    float ar1_n = sim->hrv_ar1_estado * 1.05f;
    if (ar1_n >  1.0f) ar1_n =  1.0f;
    if (ar1_n < -1.0f) ar1_n = -1.0f;

    float desfase_hrv = dev_hrv * (0.6f * Fast_Sin(sim->fase_mayer_lf) + 0.4f * ar1_n);
    float desfase_rsa = -dev_rsa * Fast_Sin(sim->fase_resp_hf);   /* seno > 0 = inhalacion */

    /* El desfase total no puede pasar del 40% del R-R medio (evita latidos absurdos a FC altas) */
    float desfase = desfase_hrv + desfase_rsa;
    float limite  = 0.40f * rr_medio;
    if (desfase >  limite) desfase =  limite;
    if (desfase < -limite) desfase = -limite;

    float rr_calculado = rr_medio + desfase;

    /* 5. Dinámica de acoplamiento prematuro y pausa compensatoria post-ectópica */
    if (es_ectopico) {
        rr_calculado *= 0.68f;             // Latido prematuro (acorta el R-R al 68%)
        sim->pausa_compensatoria = 1u;     // El siguiente latido tendrá pausa compensatoria
    } else if (sim->pausa_compensatoria) {
        rr_calculado *= 1.32f;             // Pausa compensatoria fisiológica (132%)
        sim->pausa_compensatoria = 0u;
    }

    if (rr_calculado < 120.0f) rr_calculado = 120.0f; // Límite físico (250 BPM máx instantáneo)
    sim->rr_objetivo_muestras = (uint32_t)rr_calculado;
    sim->muestra_en_rr = 0u;

    /* 6. Compresión temporal de plantilla (Ley de Bazett) para altas frecuencias */
    if (rr_calculado < (float)LONGITUD_PLANTILLA) {
        sim->paso_plantilla = (float)LONGITUD_PLANTILLA / rr_calculado;
        sim->inicio_plantilla_muestras = 0u;
    } else {
        /* Compresión suave sub-lineal en frecuencias normales/altas y segmento TP isoeléctrico */
        sim->paso_plantilla = (rr_calculado < 380.0f) ? (380.0f / rr_calculado) * 0.85f : 1.0f;
        if (sim->paso_plantilla < 1.0f) sim->paso_plantilla = 1.0f;

        uint32_t duracion_efectiva = (uint32_t)((float)LONGITUD_PLANTILLA / sim->paso_plantilla);
        /* Centrar el complejo dejando un segmento TP isoeléctrico al inicio del ciclo */
        sim->inicio_plantilla_muestras = (sim->rr_objetivo_muestras > duracion_efectiva) ?
                                         ((sim->rr_objetivo_muestras - duracion_efectiva) >> 1) : 0u;
    }

    sim->fase_plantilla = 0.0f;
    sim->en_complejo_activo = 0u;
}

/* ==========================================================================
 * 4. FUNCIONES PÚBLICAS DE INICIALIZACIÓN Y CONTROL
 * ========================================================================== */

void Sim_SetSemilla(SimuladorECG_t *sim, uint32_t nueva_semilla)
{
    sim->semilla_usuario      = nueva_semilla;
    sim->rng_state            = SplitMix32_Init(nueva_semilla);
    sim->hrv_ar1_estado       = 0.0f;
    sim->fase_mayer_lf        = 0.0f;
    sim->fase_resp_hf         = 0.0f;
    sim->fase_60hz            = 0.0f;
    sim->emg_z1               = 0.0f;
    sim->emg_z2               = 0.0f;
    sim->pausa_compensatoria  = 0u;
    sim->flag_pico_r          = 0u;

    PlanificarSiguienteLatido(sim);
}

void Sim_Init(SimuladorECG_t *sim, uint32_t semilla_inicial)
{
    sim->frecuencia_bpm       = 72u;
    sim->amplitud_pct         = 100u;
    sim->ruido_60hz_pct       = 0u;
    sim->variabilidad_rr_ms  = 20u;   /* ms de desfase pico (HRV) */
    sim->rsa_ms               = 10u;   /* ms de amplitud RSA (0 = desactivada) */
    sim->modulacion_resp_pct  = 20u;
    sim->ruido_emg_pct        = 0u;
    sim->desnivel_st_uv       = 0;
    sim->frec_resp_mhz        = 250u;
    sim->prob_ectopia_pct     = 0u;
    sim->pausa                = 0u;
    sim->patologia_base       = PATOLOGIA_NORMAL;
    sim->ultima_muestra_adc   = 0;
    sim->ultimo_pixel_y       = 120u;

    NormalizarPlantillas();   /* una sola vez, antes de generar latidos */
    Sim_SetSemilla(sim, semilla_inicial);
}

void Sim_SetPatologia(SimuladorECG_t *sim, PatologiaECG_t patologia)
{
    if (patologia < N_PATOLOGIAS_TOTALES) {
        sim->patologia_base = patologia;
    }
}

/* ==========================================================================
 * 5. MOTOR DE SÍNTESIS PROCEDURAL PASO A PASO (500 Hz)
 * ========================================================================== */

int16_t Sim_Step(SimuladorECG_t *sim)
{
    if (sim->pausa) {
        return sim->ultima_muestra_adc;
    }

    sim->flag_pico_r = 0u;

    /* 1. Avanzar osciladores fisiológicos continuos (Mayer 0.1 Hz, Resp configurable, Red 60 Hz) */
    sim->fase_mayer_lf += DOS_PI * (0.10f / FS_HZ);
    if (sim->fase_mayer_lf >= DOS_PI) sim->fase_mayer_lf -= DOS_PI;

    sim->fase_resp_hf  += DOS_PI * (((float)sim->frec_resp_mhz * 0.001f) / FS_HZ);
    if (sim->fase_resp_hf >= DOS_PI)  sim->fase_resp_hf  -= DOS_PI;

    sim->fase_60hz     += DOS_PI * (60.0f / FS_HZ);
    if (sim->fase_60hz >= DOS_PI)     sim->fase_60hz     -= DOS_PI;

    /* 2. Evaluar si estamos dentro de la ventana morfológica P-QRS-T o en diástole TP */
    float morfologia_mv = 0.0f;
    float ventana_st    = 0.0f;   // peso del desnivel ST en esta muestra

    if (sim->muestra_en_rr >= sim->inicio_plantilla_muestras) {
        if (sim->fase_plantilla < (float)(LONGITUD_PLANTILLA - 1)) {
            sim->en_complejo_activo = 1u;

            uint16_t idx = (uint16_t)sim->fase_plantilla;
            float frac   = sim->fase_plantilla - (float)idx;

            const int16_t *tpl_a = BANCOS_PATOLOGIAS[sim->patologia_actual][sim->idx_plantilla_a];
            const int16_t *tpl_b = BANCOS_PATOLOGIAS[sim->patologia_actual][sim->idx_plantilla_b];

            /* Corrección de línea base (DC offset) restando el extremo de cada plantilla */
            /* Corrección de línea base (DC offset) restando el extremo con la pendiente correcta */
                        float pendiente_base = 1.0f / (float)(LONGITUD_PLANTILLA - 1);
                        float base_a = (float)tpl_a[0] + ((float)(tpl_a[LONGITUD_PLANTILLA - 1] - tpl_a[0]) * ((float)idx * pendiente_base));
                        float base_b = (float)tpl_b[0] + ((float)(tpl_b[LONGITUD_PLANTILLA - 1] - tpl_b[0]) * ((float)idx * pendiente_base));

                        /* Protección contra lectura fuera de límites (Out-of-Bounds) en idx + 1 */
                        int16_t sig_a = (idx < LONGITUD_PLANTILLA - 1) ? tpl_a[idx + 1] : tpl_a[idx];
                        int16_t sig_b = (idx < LONGITUD_PLANTILLA - 1) ? tpl_b[idx + 1] : tpl_b[idx];

                        /* Interpolación lineal sub-muestra segura */
                        const float *gan = s_ganancia_norm[sim->patologia_actual];
                        float val_a = (((float)tpl_a[idx] + frac * (float)(sig_a - tpl_a[idx])) - base_a) * gan[sim->idx_plantilla_a];
                        float val_b = (((float)tpl_b[idx] + frac * (float)(sig_b - tpl_b[idx])) - base_b) * gan[sim->idx_plantilla_b];
            /* Mezcla baricéntrica entre ambas plantillas */
            float mezcla = (1.0f - sim->peso_mezcla) * val_a + (sim->peso_mezcla * val_b);

            /* Ventana de suavizado en bordes para garantizar continuidad C1 con la línea isoeléctrica */
            float env = 1.0f;
            if (sim->fase_plantilla < VENTANA_BORDE) {
                env = sim->fase_plantilla * (1.0f / VENTANA_BORDE);
            } else if (sim->fase_plantilla > ((float)(LONGITUD_PLANTILLA - 1) - VENTANA_BORDE)) {
                env = ((float)(LONGITUD_PLANTILLA - 1) - sim->fase_plantilla) * (1.0f / VENTANA_BORDE);
            }

            morfologia_mv = mezcla * env * sim->escala_latido;
            ventana_st    = VentanaST(sim->fase_plantilla - (float)PRE_R_MUESTRAS);

            /* Detectar cruce por el pico R (PRE_R_MUESTRAS = 90) */
            if ((idx <= PRE_R_MUESTRAS) && ((uint16_t)(sim->fase_plantilla + sim->paso_plantilla) > PRE_R_MUESTRAS)) {
                sim->flag_pico_r = 1u;
            }

            sim->fase_plantilla += sim->paso_plantilla;
        } else {
            sim->en_complejo_activo = 0u;
        }
    }

    /* 3. Modulación Cardiorrespiratoria (Amplitud del eje eléctrico + Baseline Wander) */
    float senal_resp = Fast_Sin(sim->fase_resp_hf);
    float mod_resp_norm = (float)sim->modulacion_resp_pct * 0.01f;

    /* La respiración modula +/- 12% la amplitud del QRS y añade deriva isoeléctrica lenta */
    morfologia_mv *= (1.0f + 0.12f * mod_resp_norm * senal_resp);
    float baseline_wander = 22.0f * mod_resp_norm * senal_resp;

    /* Aplicar escalado global de amplitud configurado por el usuario */
    float senal_total = (morfologia_mv * ((float)sim->amplitud_pct * 0.01f)) + baseline_wander;

    /* Desnivel del segmento ST: valor absoluto en uV (no depende de la amplitud del latido) */
    if (sim->desnivel_st_uv != 0) {
        senal_total += ventana_st * ((float)sim->desnivel_st_uv * ((float)GANANCIA_DIGITAL * 0.001f));
    }

    /* 4. Artefacto de Red Eléctrica (60 Hz) */
    if (sim->ruido_60hz_pct > 0u) {
        float amp_60hz = (float)sim->ruido_60hz_pct * 0.45f; // Hasta ~45 unidades (0.22 mV)
        senal_total += amp_60hz * Fast_Sin(sim->fase_60hz);
    }

    /* 5. Artefacto Electromiográfico EMG (Filtro IIR Biquad Pasa-Banda 20-150 Hz @ Fs=500 Hz) */
    if (sim->ruido_emg_pct > 0u) {
        float x = PRNG_Gaussian(sim);
        /* Coeficientes Biquad Butterworth Pasa-Banda 20-150 Hz normalizados para Fs = 500 Hz */
        const float b0 =  0.4208f, b1 = 0.0f,    b2 = -0.4208f;
        const float a1 = -0.4435f, a2 = 0.1584f;

        float y = b0 * x + sim->emg_z1;
        sim->emg_z1 = b1 * x - a1 * y + sim->emg_z2;
        sim->emg_z2 = b2 * x - a2 * y;

        senal_total += y * ((float)sim->ruido_emg_pct * 0.55f);
    }

    /* 6. Avanzar ciclo R-R y planificar nuevo latido al finalizar la diástole */
    sim->muestra_en_rr++;
    if (sim->muestra_en_rr >= sim->rr_objetivo_muestras) {
        PlanificarSiguienteLatido(sim);
    }

    /* 7. Saturación segura a int16_t */
    if (senal_total > 32767.0f)  senal_total = 32767.0f;
    if (senal_total < -32768.0f) senal_total = -32768.0f;

    sim->ultima_muestra_adc = (int16_t)senal_total;
    return sim->ultima_muestra_adc;
}

/* Utilidad para mapear la señal directamente a la coordenada vertical del TFT ILI9341 */
uint8_t Sim_MapearAPantallaY(int16_t muestra_digital, uint16_t y_centro, uint16_t alto_util)
{
    /* Con GANANCIA_DIGITAL = 200, el rango dinámico típico es [-300, +450] */
    int32_t y = (int32_t)y_centro - ((int32_t)muestra_digital * (int32_t)alto_util) / 800;
    if (y < 0)   y = 0;
    if (y > 239) y = 239; // Límite vertical típico ILI9341 en modo landscape (320x240)
    return (uint8_t)y;
}
