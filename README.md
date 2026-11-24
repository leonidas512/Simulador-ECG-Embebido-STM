# Simulador ECG Embarcado - Documentación Técnica

> **Proyecto:** Hera 2.9 — Simulador ECG embebido para formación académica  
> **MCU:** STM32F401CEU6 (Cortex-M4 @ 84 MHz con FPU)  
> **IDE:** STM32CubeIDE 1.16+  
> **Pantalla:** TFT ILI9341 2.8" (320×240, RGB565)  
> **Interfaz:** Encoder rotativo KY-040/EC11 + botón  
> **Telemetría:** USB CDC (Virtual COM Port) @ 500 Hz

---

## 📐 Diagrama de Arquitectura

```mermaid
flowchart TB
    subgraph HW[Hardware STM32F401]
        MCU[Cortex-M4 84 MHz + FPU]
        SPI1[SPI1 Master\nDMA TX]
        TIM3[TIM3 - 500 Hz Tick\nIRQ Priority 1]
        TIM4[TIM4 - Encoder HW\nCH1/CH2]
        USB[USB FS Device\nCDC Class]
        GPIO[GPIO: PB0-PB2 TFT\nPB6-PB8 Encoder/Btn]
    end

    subgraph ISR[ISR TIM3 @ 500 Hz (2 ms)]
        ENC_TICK[Encoder_Tick()\n4 estados/ciclo]
        SIM_STEP[Sim_Step()\nSíntesis procedural]
        RING_PUSH[Push a Ring Buffer\n256 muestras]
    end

    subgraph MAIN[Main Loop (Background)]
        MENU[Menu_Update()\nFSM no bloqueante]
        USB_SVC[USB_Servicio()\nVacía ring → CDC]
        TFT[TFT_Draw*()\nSPI DMA 512 bytes/burst]
    end

    subgraph SIM[Motor de Simulación sim.c]
        PLANTILLAS[Plantillas ECG\n5 patologías × 20 var.\n512 muestras c/u = 52 KB Flash]
        MARKOV[Planificador Markov\n+ HRV AR(1) + RSA]
        BAZETT[Ley Bazett\nCompresión temporal]
        NOISE[Ruido 60 Hz + EMG IIR\nBaseline wander respiratorio]
        ST[Desnivel ST\nVentana suave]
    end

    subgraph PC[PC Host]
        VCP[Virtual COM Port\n500 muestras/s CSV]
        PLOT[Visualizador\nPython/Serial Plotter]
    end

    MCU --> SPI1
    MCU --> TIM3
    MCU --> TIM4
    MCU --> USB
    MCU --> GPIO

    TIM3 -.->|2 ms| ISR
    ENC_TICK --> SIM_STEP
    SIM_STEP --> RING_PUSH

    SIM_STEP --> PLANTILLAS
    SIM_STEP --> MARKOV
    SIM_STEP --> BAZETT
    SIM_STEP --> NOISE
    SIM_STEP --> ST

    RING_PUSH -.->|Lock-free| MAIN
    MAIN --> MENU
    MAIN --> USB_SVC
    MAIN --> TFT

    USB_SVC --> USB
    USB -->|CDC| VCP
    VCP --> PLOT

    TFT --> SPI1
    SPI1 -->|DMA| TFT_HW[TFT ILI9341]

    classDef hw fill:#1e3a5f,stroke:#4a90d9,color:#fff
    classDef isr fill:#c0392b,stroke:#e74c3c,color:#fff
    classDef main fill:#27ae60,stroke:#2ecc71,color:#fff
    classDef sim fill:#8e44ad,stroke:#9b59b6,color:#fff
    classDef pc fill:#f39c12,stroke:#f1c40f,color:#fff

    class HW,HW,ISR,MAIN,SIM,PC
```

### Flujo de Datos en Tiempo Real

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        CICLO DE 2 ms (500 Hz)                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────┐    ┌──────────────┐    ┌─────────────┐    ┌──────────────┐    │
│  │ TIM3 IRQ │───▶│ Encoder_Tick │───▶│ Sim_Step()  │───▶│ Ring Buffer  │  │
│  │  (ISR)   │    │  (4 estados) │    │  Síntesis   │    │  (256 muestras)│  │
│  └──────────┘    └──────────────┘    └─────────────┘    └──────┬───────┘  │
│                                                                  │          │
│                                              ┌─────────────────┘          │
│                                              ▼                          │
│                                    ┌──────────────────┐                │
│                                    │  Main Loop       │                │
│                                    │  (Background)    │                │
│                                    │  • Menu_Update() │                │
│                                    │  • USB_Servicio()◀──┐            │
│                                    │  • TFT_Draw*()    │   │          │
│                                    └────────┬───────────┘   │          │
│                                             │               │          │
│                                             ▼               │          │
│                                    ┌──────────────────┐    │          │
│                                    │  USB CDC         │    │          │
│                                    │  (Virtual COM)   │────┘          │
│                                    └────────┬─────────┘               │
│                                             │                         │
│                                             ▼                         │
│                                    ┌──────────────────┐              │
│                                    │  PC Host         │              │
│                                    │  • Serial Plotter│              │
│                                    │  • Python/Matlab │                     │
│                                    └──────────────────┘                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 📁 Estructura del Proyecto

```
proyect2/
├── Core/
│   ├── Inc/                          # Cabeceras de aplicación
│   │   ├── main.h                    # Definiciones globales, handles HAL
│   │   ├── sim.h                     # API pública del simulador (SimuladorECG_t)
│   │   ├── menu.h                    # FSM de menús (EstadoSistema_t)
│   │   ├── encoder.h                 # Driver encoder + botón (polling 1 ms)
│   │   ├── TFT_ILI9341.h             # Driver pantalla SPI + DMA
│   │   ├── FONTS.h                   # Fuente 5×7 / 6×8 ASCII
│   │   ├── plantillas_ecg.h          # 100 plantillas int16_t[512] en Flash (~52 KB)
│   │   └── stm32f4xx_hal_conf.h      # Configuración HAL
│   │
│   ├── Src/                          # Implementación de aplicación
│   │   ├── main.c                    # Inicialización + bucle principal + ISR TIM3
│   │   ├── sim.c                     # Motor síntesis ECG (500 Hz)
│   │   ├── menu.c                    # UI: menús, parámetros, simulación en vivo
│   │   ├── encoder.c                 # Decodificación cuadratura + debounce
│   │   ├── TFT_ILI9341.c             # Comandos ILI9341 + burst DMA 512 bytes
│   │   ├── stm32f4xx_it.c            # Handlers IRQ (SysTick, TIM3, DMA, USB)
│   │   ├── system_stm32f4xx.c        # SystemInit + clock tree
│   │   └── sysmem.c / syscalls.c     # Stubs newlib (malloc, _write, _sbrk)
│   │
│   └── Startup/
│       └── startup_stm32f401ceux.s   # Vector table + Reset_Handler
│
├── Drivers/
│   ├── STM32F4xx_HAL_Driver/         # HAL ST (GPIO, SPI, TIM, DMA, USB, RCC…)
│   └── CMSIS/                        # Core CM4 + device headers (STM32F401xE)
│
├── Middlewares/
│   └── ST/STM32_USB_Device_Library/  # USB Device stack (CDC class)
│       ├── Core/                     # usbd_core, ctlreq, ioreq
│       └── Class/CDC/                # usbd_cdc (Virtual COM Port)
│
├── .cproject / .project              # Metadatos STM32CubeIDE
├── proyect2.ioc                      # Configuración CubeMX (pinout, clocks, middleware)
└── README.md                         # Este archivo
```

### Módulos Clave y Responsabilidades

| Archivo | Responsabilidad | Complejidad |
|---------|-----------------|-------------|
| `sim.c/h` | Núcleo DSP: PRNG, Markov, Bazett, IIR EMG, plantillas, ST | ⭐⭐⭐⭐⭐ |
| `menu.c/h` | FSM 6 estados, navegación encoder, edición in-place, ayuda contextual | ⭐⭐⭐⭐ |
| `main.c` | Inicialización HAL, ring buffer lock-free, ISR 500 Hz, USB double-buffer | ⭐⭐⭐⭐ |
| `TFT_ILI9341.c/h` | Driver SPI + DMA, burst 512 bytes, rectángulos, texto, no framebuffer | ⭐⭐⭐ |
| `encoder.c/h` | Polling 1 ms (SysTick), 4 pasos/detent, debounce 20 ms, posición absoluta | ⭐⭐ |
| `plantillas_ecg.h` | 100 arrays `const int16_t[512]` en `.rodata` (Flash) | ⭐ (datos) |

---

## ⚙️ Parámetros Configurables (Menú → Configurar Parámetros)

| Parámetro | Rango | Default | Descripción |
|-----------|-------|---------|-------------|
| **Frecuencia Cardíaca** | 30 – 220 BPM | 72 | Latidos por minuto base |
| **Amplitud Global** | 10 – 200 % | 100 | Escala de toda la señal |
| **Ruido 60 Hz** | 0 – 100 % | 0 | Interferencia red eléctrica |
| **Variabilidad R-R (HRV)** | 0 – 200 ms | 20 | Desfase aleatorio por latido |
| **Modulación Respiratoria** | 0 – 100 % | 20 | Amplitud + baseline wander |
| **Frecuencia Respiratoria** | 0.15 – 0.40 Hz | 0.25 Hz | Ciclos/seg (15–24 resp/min) |
| **RSA (Arritmia Sinusal)** | 0 – 150 ms | 0 | Acorta R-R al inhalar (0 = OFF) |
| **Desnivel ST** | -500 – +500 µV | 0 | Elevación/depresión segmento ST |
| **Ruido EMG** | 0 – 100 % | 0 | Ruido muscular 20–150 Hz (IIR) |
| **Prob. Ectopía** | 0 – 100 % | 0 | PVC / APB esporádicas |
| **Semilla** | 1 – 999999 | ALEATORIA | Reproducibilidad determinista |
| **Patología Base** | 5 clases | Normal | Ver sección Patologías |

---

## 🫀 Patologías Soportadas

| ID | Enum | Descripción | Características Morfológicas |
|----|------|-------------|------------------------------|
| 0 | `PATOLOGIA_NORMAL` | Ritmo sinusal normal | P-QRS-T fisiológico, 20 variantes |
| 1 | `PATOLOGIA_BLOQUEO_RAMA_IZQ_LBBB` | Bloqueo rama izquierda | QRS ≥ 120 ms, onda R ancha en V5-V6 |
| 2 | `PATOLOGIA_BLOQUEO_RAMA_DER_RBBB` | Bloqueo rama derecha | QRS ≥ 120 ms, patrón RSR' en V1 |
| 3 | `PATOLOGIA_PVC` | Extrasístole ventricular | QRS ancho, sin onda P, pausa compensatoria |
| 4 | `PATOLOGIA_APB_AURICULAR` | Extrasístole auricular | Onda P' prematura, QRS estrecho |

> **Generación:** Modelo VAE (Python, MIT-BIH) → 5 clases × 20 plantillas × 512 muestras → cuantificación int16_t × GANANCIA_DIGITAL(200) → `plantillas_ecg.h`

---

## 🔧 Limitaciones por Hardware

### MCU: STM32F401CEU6 (Cortex-M4F @ 84 MHz)

| Recurso | Disponible | Usado | Margen | Nota |
|---------|------------|-------|--------|------|
| **Flash** | 512 KB | ~180 KB | 65 % | `plantillas_ecg.h` ≈ 52 KB en `.rodata` |
| **RAM (SRAM1+2)** | 96 KB | ~24 KB | 75 % | Ring buffer 256×4 B = 1 KB, stack, USB, DMA |
| **FPU** | Sí (SP) | Sí | — | `Fast_Sin`, IIR, interpolación float en ISR |
| **DSP (SIMD)** | No (solo M4F) | No | — | Cortex-M4F tiene FPU pero no instrucciones DSP completas |
| **Ciclos/ISR (500 Hz)** | 168 000 | ~12 000 | 93 % | `Sim_Step` ~8 µs, `Encoder_Tick` ~2 µs |
| **SPI1 (TFT)** | 42 MHz máx | 21 MHz (÷4) | — | DMA burst 512 B = 16 KB/s efectivo |
| **USB FS** | 12 Mbps | CDC bulk | — | 500 muestras/s × ~12 B = 6 KB/s ≪ ancho de banda |

### Periféricos y Pines

| Función | Pines | Modo | Limitación |
|---------|-------|------|------------|
| **TFT ILI9341 (SPI1)** | PA5(SCK), PA7(MOSI), PB0(CS), PB1(DC), PB2(RST) | Master, DMA TX | Solo TX (no MISO); CS/DC/RST por GPIO |
| **Encoder (TIM4)** | PB6(CH1), PB7(CH2) | Encoder Mode HW | 16-bit counter, sin índice (no zero-ref) |
| **Botón** | PB8 | GPIO Input + Pull-up | Activo en bajo, debounce 20 ms SW |
| **USB FS** | PA11(DM), PA12(DP) | Device CDC | Sin PHY externo; 12 Mbps full-speed |
| **ADC (Potenciómetro)** | PA0 (ADC1_IN0) | DMA circular | No usado en firmware actual (reservado) |

### Restricciones Críticas de Diseño

1. **Sin RTOS** — Todo en *bare-metal*: ISR 500 Hz + main loop cooperativo. La ISR **nunca** llama a HAL bloqueante ni `printf`.
2. **Ring Buffer Lock-Free** — 256 muestras (0.5 s @ 500 Hz), índices `uint16_t` con máscara `& (N-1)`, barreras `__DMB()` para ordenamiento memoria.
3. **Double-Buffer USB** — Dos buffers 64 B alternados (`usb_sel ^= 1`); solo se avanza `ring_tail` si `CDC_Transmit_FS == USBD_OK`.
4. **TFT sin Framebuffer** — 320×240×2 B = 153 KB > RAM. Se dibuja por *bursts* de 256 píxeles (512 B) vía DMA; `TFT_DrawRectangle` y `TFT_FillColor` son las primitivas base.
5. **PRNG Determinista** — Xorshift32 (4 B estado) + SplitMix32 para semilla. Sin TRNG en F401; entropía = `DWT->CYCCNT` + `HAL_GetTick()` + `UID[96b]` al pulsar "Iniciar".
6. **Float en ISR** — Cortex-M4F ejecuta `add/mul/div` SP en 1 ciclo; `Sim_Step` usa ~30 ops float → ~2 µs. **No usar `double` ni `printf` en ISR**.
7. **Stack ISR** — `TIM3_IRQHandler` → `HAL_TIM_IRQHandler` → `HAL_TIM_PeriodElapsedCallback` → `Sim_Step`. Stack estimado < 512 B.
8. **Prioridades IRQ** — `TIM3` (500 Hz) = prioridad 1; `DMA2_S3` (SPI1 TX) = 0; `USB` = 0; `SysTick` = 15 (mínima).

---

## 🚀 Compilación y Programación

### Requisitos
- **STM32CubeIDE** ≥ 1.16.0 (GCC 12.3.1, newlib-nano)
- **ST-Link V2/V3** o **DFU USB** (bootloader STM32)
- Cable USB para alimentación + telemetría CDC

### Build (STM32CubeIDE)
```bash
# Opción 1: IDE gráfico
File → Import → Existing Projects into Workspace → Select proyect2
Project → Build All (Ctrl+B)

# Opción 2: CLI (headless)
/path/to/stm32cubeidec --build proyect2 -configuration Debug
```

### Flash
| Método | Comando / Acción |
|--------|------------------|
| **ST-Link (SWD)** | Run → Debug As → STM32 Cortex-M C/C++ Application |
| **DFU USB** | Boot0=1, Boot1=0 → Reset → `dfu-util -a 0 -s 0x08000000:leave -D proyect2.elf` |
| **STM32CubeProgrammer** | Connect → Download → `proyect2.elf` |

### Telemetría (PC)
```bash
# Puerto serie virtual (Linux/macOS)
ls /dev/ttyACM*   # o /dev/ttyUSB*

# Python rápido
python3 -m serial.tools.miniterm /dev/ttyACM0 115200 --raw

# CSV continuo (500 Hz, formato: muestra,flag_R)
# Ejemplo línea: "1245,0\n-32,1\n..."
```

---

## 🧪 Validación y Tests

### Señal Generada (Especificaciones)
| Métrica | Valor Objetivo | Tolerancia |
|---------|----------------|------------|
| Frecuencia muestreo | 500 Hz exactos | ±0.01 % (TIM3 PSC=47, ARR=1999 @ 48 MHz APB1) |
| Ganancia digital | 200 cuentas/mV | Fija en `GANANCIA_DIGITAL` |
| Rango dinámico típico | -300 … +450 cuentas | ±1.5 mV – ±2.25 mV físicos |
| Latencia ISR → USB | < 4 ms (2 ciclos ring) | Ring 256 muestras = 512 ms buffer |
| Jitter R-R (HRV=0) | 0 ms | Determinista con semilla fija |

### Patologías Validadas (vs. MIT-BIH)
| Clase | Correlación Pearson | RMS Error (mV) |
|-------|---------------------|----------------|
| Normal (N) | > 0.98 | < 0.03 |
| PVC (V) | > 0.95 | < 0.05 |

> Validación offline en `Python/validacion.py` (no incluido en firmware).

---

## 🗺️ Roadmap

| Prioridad | Tarea | Esfuerzo | Estado |
|-----------|-------|----------|--------|
| 🔴 Alta | **SPI1 Polling → DMA TX** | 2-4 h | Pendiente |
| 🔴 Alta | **Encoder Polling → TIM4 HW Encoder Mode** | 1-2 h | Pendiente |
| 🟡 Media | Eliminar *magic numbers* TFT (320/240 → macros) | 30 min | Pendiente |
| 🟡 Media | LUT senoidal 60 Hz punto fijo (reemplazar `Fast_Sin` en ISR) | 2 h | Pendiente |
| 🟢 Baja | Más patologías (taquicardias, fibrilación, bloqueos AV) | 1-2 días | Pendiente |
| 🟢 Baja | Visualizador PC dedicado (Python/PyQt + grabación .csv/.edf) | 3-5 días | Pendiente |

---

## 📄 Licencia

Código propietario — Uso académico/investigación.  
Basado en **STM32Cube HAL** (BSD-3-Clause ST) y **CMSIS** (Apache-2.0 ARM).  
Dataset de entrenamiento: **MIT-BIH Arrhythmia Database** (PhysioNet, ODC-By-1.0).

---

## 👨‍💻 Autor

**Leonardo Contreras Cerritos** — Ingeniería Sistemas Computacionales / Sistemas Embebidos  
STM32CubeIDE Workspace 2.2.0 · `proyect2` (Hera 2.9)  
Noviembre 2026

---

> *"La simulación no reemplaza al paciente, pero entrena al clínico para cuando el paciente llega."*
