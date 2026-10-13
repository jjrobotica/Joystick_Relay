# Joystick inalámbrico ESP-NOW → Relés

Control inalámbrico con joystick para las plataformas móviles de un taller de **Verificación Técnica Vehicular (VTV)**: las placas que mueven las ruedas del vehículo para revisar holguras de tren delantero y suspensión.

Reemplaza la botonera tradicional por un joystick: el operario empuja hacia donde quiere mover la plataforma y suelta para frenar.

Usa dos **ESP32-C3 Super Mini** que se comunican por **ESP-NOW**, sin router ni WiFi. Un mismo sketch sirve para las dos placas, y se elige el rol con una constante.

```mermaid
flowchart LR
    J[Joystick analógico] -->|ADC X/Y| TX[ESP32-C3<br/>MANDO]
    TX -.->|ESP-NOW broadcast<br/>cada 50 ms| RX[ESP32-C3<br/>PLATAFORMA]
    RX -->|GPIO 2..5| R[Módulo de 4 relés]
    R --> P[Válvulas / contactores<br/>de la plataforma]
```

---

## Características

**Mando (TX)**
- Lee el joystick y lo traduce a **una sola dirección**: `F` (adelante), `B` (atrás), `L` (izquierda), `R` (derecha) o `S` (stop).
- Envía al cambiar la dirección y además **cada 50 ms (keepalive)**, para que el receptor sepa que el mando sigue vivo.
- **Autocalibra el centro** al encender.
- **Histéresis** en la zona muerta y en el cambio de eje: no parpadea entre F y R en diagonal.

**Plataforma (RX)**
- **Enclavamiento:** nunca hay dos salidas activas a la vez.
- **Tiempo muerto** al invertir en el mismo eje (F↔B, L↔R) para proteger válvulas y motores.
- **Failsafe:** si pasan más de 200 ms sin paquetes, frena todo.
- **Rearme:** después de un failsafe o al arrancar, no se mueve hasta que el joystick vuelve al centro.
- Relés en OFF desde el arranque, sin "clic" al encender.
- Descarta paquetes ESP-NOW ajenos (firma `MAGIC`) y opcionalmente filtra por MAC del mando.

**Diagnóstico**
- Línea `[ESTADO]` por Serial cada 2 s.
- LED azul de la placa como indicador de estado, útil sin PC.

---

## Hardware

| Cant. | Componente | Notas |
|---|---|---|
| 2 | ESP32-C3 Super Mini | Una para el mando y otra para la plataforma |
| 1 | Joystick analógico (tipo KY-023) | Alimentado a **3,3 V** |
| 1 | Módulo de 4 relés 5 V con optoacoplador | Idealmente con jumper **JD-VCC** |
| 1 | Fuente 5 V 1–2 A | Para la plataforma |
| — | Capacitor 100–470 µF | Entre 5 V y GND, junto al módulo de relés (recomendado) |

---

## Conexiones

### Mando (TX)

| Joystick | ESP32-C3 |
|---|---|
| VCC | **3V3** |
| GND | GND |
| VRx | GPIO0 |
| VRy | GPIO1 |

> ⚠️ **No alimentes el joystick con 5 V.** En los extremos le llegarían hasta 5 V al pin analógico, y el C3 tolera 3,3 V.

### Plataforma (RX)

| ESP32-C3 | Módulo de relés | Función |
|---|---|---|
| GPIO2 | IN1 | F — Adelante |
| GPIO3 | IN2 | B — Atrás |
| GPIO4 | IN3 | L — Izquierda |
| GPIO5 | IN4 | R — Derecha |
| 3V3 | VCC (lado optos) | Sacar el jumper JD-VCC |
| 5V | JD-VCC (bobinas) | Desde el pin 5V o desde una fuente externa |
| GND | GND | Común |

> ⚠️ **Nunca alimentes las bobinas desde el pin 3V3.** El regulador del Super Mini es chico, y con los picos del radio el C3 se resetea o el USB deja de responder.

**¿Por qué separar VCC y JD-VCC?** Con el módulo entero a 5 V, el pin del ESP en HIGH (3,3 V) no alcanza para apagar del todo el optoacoplador y el relé puede quedar enganchado. Si la lógica del opto va a 3,3 V, HIGH apaga de verdad.

**Hacia la plataforma:** conectá los contactos **NO** de cada relé **en paralelo** con el botón original correspondiente. Así la botonera sigue funcionando.

---

## Software

### Requisitos
- Arduino IDE 2.x
- Core **esp32 de Espressif 3.x** (Gestor de placas → "esp32")

### Configuración del IDE (Herramientas)

| Opción | Valor |
|---|---|
| Placa | **ESP32C3 Dev Module** |
| USB CDC On Boot | **Enabled** ← sin esto no sale nada por el monitor serie |
| Flash Mode | QIO (si no arranca, DIO) |
| Monitor Serie | 115200 baudios |

> En algunas versiones del core, la placa *"Nologo ESP32C3 Super Mini"* muestra la opción **"Enabled" dos veces** en *USB CDC On Boot*. Con *ESP32C3 Dev Module* la opción es clara.

### Carga

1. Abrí `joystick_relay.ino`.
2. **Mando:** dejá `#define ROLE_TX 1`, elegí su puerto COM y subilo.
3. **Plataforma:** cambiá a `#define ROLE_TX 0`, elegí el puerto de la otra placa y subilo.

Las dos placas tienen que tener **la misma versión** del sketch: si el formato del paquete no coincide, el receptor lo descarta como ajeno.

---

## Parámetros configurables

Todos están al principio del sketch.

| Constante | Default | Descripción |
|---|---|---|
| `ROLE_TX` | `1` | `1` = mando, `0` = plataforma |
| `AUTO_CAL` | `true` | Calibra el centro del joystick al encender |
| `CENTER_X_DEFAULT` / `_Y_` | `3470` / `3650` | Centro de respaldo si la autocalibración falla |
| `TH_ON` / `TH_OFF` | `300` / `200` | Desvío del centro para activar / soltar (histéresis) |
| `AXIS_MARGIN` | `150` | Margen para cambiar de eje en diagonal (más bajo = más ágil) |
| `KEEPALIVE_MS` | `50` | Intervalo de envío del mando |
| `RELAY_ACTIVE_LOW` | `true` | `true` = módulo de relés típico; `false` = LEDs de prueba o módulo activo en HIGH |
| `FAILSAFE_MS` | `200` | Tiempo sin paquetes antes de frenar |
| `DEAD_TIME_MS` | `200` | Pausa al invertir dirección |
| `DEAD_TIME_ONLY_REVERSE` | `true` | `true` = pausa solo al invertir en el mismo eje; `false` = en cualquier cambio |
| `FILTER_MAC` | `false` | Aceptar solo el mando con la MAC `ALLOWED_TX_MAC` |

> Si F/B y L/R los mueve **el mismo** actuador (una sola válvula o motor), usá `DEAD_TIME_ONLY_REVERSE = false`.

---

## Protocolo

Paquete de 4 bytes enviado por ESP-NOW en broadcast (`FF:FF:FF:FF:FF:FF`):

```c
typedef struct __attribute__((packed)) {
  uint16_t magic;    // 0x5654 ("VT") - firma del protocolo
  uint8_t  dirBits;  // 0 = STOP | bit0=F bit1=B bit2=L bit3=R (uno solo)
  uint8_t  seq;      // contador 0..255
} Packet;
```

- `magic` es una firma fija, no un identificador único: sirve para descartar paquetes de otros dispositivos ESP-NOW. **No es seguridad.**
- Un `dirBits` con más de un bit encendido se trata como STOP.
- Para que la plataforma obedezca **solo a tu mando**, activá `FILTER_MAC` con la MAC que imprime el mando al arrancar (`TX MAC: ...`).

---

## Diagnóstico

### LED azul de la placa (GPIO8)

| Placa | Patrón | Significado |
|---|---|---|
| Mando | Parpadeo corto cada 1 s | Encendido, joystick en el centro |
| Mando | Fijo | Joystick fuera del centro (enviando dirección) |
| Plataforma | Parpadeo lento | **Sin señal** del mando |
| Plataforma | Fijo | Recibiendo del mando |
| Cualquiera | Parpadeo rápido | Error al iniciar ESP-NOW |

### Monitor serie

Mando:
```
=== MANDO (TX) ===
Centro calibrado: X=3468 Y=3652
TX MAC: 34:85:18:XX:XX:XX
TX x=3470 y=4050  dir=F  seq=12  send=0
[ESTADO TX] x=3471 y=3650 (centro 3468/3652)  dir=S  enviados=812  errores=0
```

Plataforma:
```
=== PLATAFORMA (RX) ===
RX MAC: 34:85:18:YY:YY:YY
Señal del mando OK
ARMADO
OUT -> F
[ESTADO RX] señal=OK  armado=SI  out=F  paquetes=640  ajenos=0  ultimo hace 12 ms
```

---

## Problemas comunes

| Síntoma | Causa probable | Solución |
|---|---|---|
| No aparece nada en el monitor serie | *USB CDC On Boot* deshabilitado | Habilitarlo y volver a subir |
| La PC no reconoce el puerto | Puerto o cable USB | Probar otro puerto (directo, sin hub) y otro cable. Modo descarga: mantener **BOOT** al enchufar |
| El puerto se cae al activar un relé | Bobinas alimentadas desde 3V3, o ruido | Bobinas a 5V (JD-VCC), GND común, capacitor de 100–470 µF |
| El relé engancha pero no suelta | Módulo de opto a 5 V manejado con 3,3 V | Separar VCC (3V3) y JD-VCC (5V) |
| La plataforma frena sola mientras se mantiene el joystick | Pérdida de paquetes | Revisar distancia o obstáculos; ver `FAILSAFE` en el monitor |
| "Bloqueado: llevar joystick al centro" | Rearme después de un failsafe o del arranque | Soltar el joystick |
| Cuenta `ajenos` alta en el receptor | El mando tiene otra versión del sketch | Subir el mismo `.ino` a las dos placas |
| Se mueve hacia el lado opuesto | Orientación del joystick | Intercambiar los relés o los cables X/Y |

---

## ⚠️ Seguridad

- Este sistema **no reemplaza la parada de emergencia**. La seta de emergencia de la instalación tiene que seguir cortando por hardware.
- Probar primero **sin vehículo** sobre la plataforma.
- La lógica de failsafe y rearme reduce riesgos, pero el tablero y las protecciones eléctricas existentes deben mantenerse.

---

## Autor

Juan José Campis — **JJ-Robótica** · Posadas, Misiones, Argentina
