/*  VTV - Joystick ESP-NOW -> Relés  (ESP32-C3 Super Mini)
    Un SOLO sketch para TX (mando) o RX (plataforma): cambiar ROLE_TX.

    TX (mando)
      - Envía al cambiar la dirección Y además cada 50 ms (keepalive).
      - Autocalibra el centro al encender (dejar el joystick suelto al prender).
      - Histéresis en zona muerta y en cambio de eje.

    RX (plataforma)
      - Salidas a relé con nivel activo configurable.
      - Relés en OFF antes de pinMode (sin "clic" al arrancar).
      - Enclavamiento + tiempo muerto al cambiar de dirección.
      - Failsafe: >200 ms sin paquetes => STOP; exige joystick al centro para rearmar.
      - Paquete con firma (MAGIC) y filtro opcional por MAC del mando.

    Diagnóstico
      - Línea de ESTADO por Serial cada 2 s (siempre se ve algo).
      - LED azul de la placa (GPIO8):
          TX: parpadeo corto cada 1 s = vivo | fijo = joystick fuera del centro
          RX: parpadeo lento = SIN señal del mando | fijo = recibiendo

    Arduino IDE -> Herramientas:
      - Placa: "ESP32C3 Dev Module" (o "Nologo ESP32C3 Super Mini")
      - USB CDC On Boot: "Enabled"   <-- sin esto el Serial no sale por USB
*/

#include <WiFi.h>
#include <esp_now.h>

// ================== CAMBIAR EN CADA PLACA ==================
#define ROLE_TX 0   // 1 = TX (mando) | 0 = RX (plataforma)
// ===========================================================

// ------------------ Protocolo ------------------
static const uint16_t MAGIC = 0x5654;   // "VT" - descarta paquetes ESP-NOW ajenos

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t  dirBits;   // 0 = STOP | uno solo de: bit0=F bit1=B bit2=L bit3=R
  uint8_t  seq;
} Packet;

static const uint8_t BIT_F = 1 << 0;
static const uint8_t BIT_B = 1 << 1;
static const uint8_t BIT_L = 1 << 2;
static const uint8_t BIT_R = 1 << 3;

static uint8_t BCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ------------------ Diagnóstico ------------------
static const int      LED_PIN        = 8;      // LED azul del Super Mini
static const bool     LED_ACTIVE_LOW = true;   // en el Super Mini prende con LOW
static const uint32_t STATUS_MS      = 2000;   // cada cuánto imprime ESTADO

// ------------------ TX: joystick ------------------
static const int JOY_X_PIN = 0;   // GPIO0 (A0)
static const int JOY_Y_PIN = 1;   // GPIO1 (A1)

static const bool AUTO_CAL         = true;
static const int  CENTER_X_DEFAULT = 3470;  // lo que habías medido a mano
static const int  CENTER_Y_DEFAULT = 3650;
static const int  CAL_MAX_DIFF     = 400;   // si la autocal se aleja más que esto, usa los default

static const int  TH_ON       = 300;  // desvío del centro para ACTIVAR una dirección
static const int  TH_OFF      = 200;  // desvío para SOLTAR (histéresis)
static const int  AXIS_MARGIN = 150;  // margen para cambiar de eje en diagonal

static const uint32_t KEEPALIVE_MS = 50;

// ------------------ RX: relés ------------------
static const int OUT_F = 2;
static const int OUT_B = 3;
static const int OUT_L = 4;
static const int OUT_R = 5;

// true  = módulo de relé activo en LOW (lo más común)
// false = LEDs de prueba / módulo activo en HIGH
static const bool RELAY_ACTIVE_LOW = true;

static const uint32_t FAILSAFE_MS  = 200;  // sin paquetes => STOP
static const uint32_t DEAD_TIME_MS = 200;  // pausa al INVERTIR en el mismo eje (F <-> B, L <-> R)
// true  = la pausa solo se aplica al invertir en el mismo eje; cambiar de eje (F -> R) es inmediato
// false = pausa en cualquier cambio de dirección (más conservador)
static const bool DEAD_TIME_ONLY_REVERSE = true;

// Filtro por MAC del mando (copiar la "TX MAC:" que imprime el mando al arrancar)
static const bool    FILTER_MAC        = false;
static const uint8_t ALLOWED_TX_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ================================================================

static const char* dirToText(uint8_t b) {
  switch (b) {
    case 0:     return "S";
    case BIT_F: return "F";
    case BIT_B: return "B";
    case BIT_L: return "L";
    case BIT_R: return "R";
    default:    return "?";
  }
}

static void ledSet(bool on) {
  digitalWrite(LED_PIN, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

static void ledInit() {
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
}

// Espera hasta 3 s a que abras el monitor serie (para no perder los mensajes de arranque)
static void serialInit() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Si el USB está enchufado pero el monitor cerrado, print() no debe frenar el loop
  Serial.setTxTimeoutMs(0);
#endif
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) delay(10);
  delay(200);
  Serial.println();
}

static void printLocalMac(const char* label) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("%s %02X:%02X:%02X:%02X:%02X:%02X\n",
                label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#if ROLE_TX
// ============================ TX ============================

static uint8_t  seqTx    = 0;
static int      centerX  = CENTER_X_DEFAULT;
static int      centerY  = CENTER_Y_DEFAULT;
static uint32_t sentOk   = 0;
static uint32_t sentErr  = 0;

static int readAvg(int pin, int n) {
  long s = 0;
  for (int i = 0; i < n; i++) s += analogRead(pin);
  return (int)(s / n);
}

static void calibrateCenter() {
  if (!AUTO_CAL) {
    Serial.printf("Centro fijo: X=%d Y=%d\n", centerX, centerY);
    return;
  }
  delay(100);
  int cx = readAvg(JOY_X_PIN, 64);
  int cy = readAvg(JOY_Y_PIN, 64);

  if (abs(cx - CENTER_X_DEFAULT) > CAL_MAX_DIFF || abs(cy - CENTER_Y_DEFAULT) > CAL_MAX_DIFF) {
    Serial.printf("AVISO: autocal fuera de rango (X=%d Y=%d). ¿Joystick movido o desconectado?\n", cx, cy);
    Serial.printf("Uso centro por defecto: X=%d Y=%d\n", centerX, centerY);
    return;
  }
  centerX = cx;
  centerY = cy;
  Serial.printf("Centro calibrado: X=%d Y=%d\n", centerX, centerY);
}

static uint8_t makeDirFromJoystick(int x, int y) {
  static uint8_t cur = 0;

  int ex = x - centerX;
  int ey = y - centerY;
  int ax = abs(ex);
  int ay = abs(ey);

  // Zona muerta con histéresis
  int th = (cur == 0) ? TH_ON : TH_OFF;
  if (ax < th && ay < th) {
    cur = 0;
    return 0;  // STOP
  }

  // Eje dominante, con histéresis para no saltar de eje en diagonal
  bool useX;
  if (cur & (BIT_L | BIT_R))      useX = (ax + AXIS_MARGIN >= ay);
  else if (cur & (BIT_F | BIT_B)) useX = (ax > ay + AXIS_MARGIN);
  else                            useX = (ax >= ay);

  if (useX) cur = (ex > 0) ? BIT_R : BIT_L;
  else      cur = (ey > 0) ? BIT_F : BIT_B;

  return cur;
}

void setup() {
  ledInit();
  serialInit();
  Serial.println("=== MANDO (TX) ===");

  analogReadResolution(12);
  pinMode(JOY_X_PIN, INPUT);
  pinMode(JOY_Y_PIN, INPUT);
  calibrateCenter();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // sin ahorro de energía del radio: menos latencia y pérdidas
  delay(200);
  printLocalMac("TX MAC:");

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init FAILED");
    while (true) { ledSet(true); delay(100); ledSet(false); delay(100); }  // parpadeo rápido = error
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.channel = 0;   // canal actual
  peer.encrypt = false;
  Serial.printf("add_peer=%d\n", (int)esp_now_add_peer(&peer));
  Serial.println("TX listo");
}

void loop() {
  int x = readAvg(JOY_X_PIN, 4);
  int y = readAvg(JOY_Y_PIN, 4);
  uint8_t dir = makeDirFromJoystick(x, y);

  static uint8_t  prevDir    = 0xFF;
  static uint32_t lastSend   = 0;
  static uint32_t lastStatus = 0;
  uint32_t now = millis();
  bool changed = (dir != prevDir);

  // Enviar al cambiar Y cada KEEPALIVE_MS (para que el RX no caiga en failsafe)
  if (changed || (now - lastSend) >= KEEPALIVE_MS) {
    Packet p{MAGIC, dir, seqTx++};
    esp_err_t r = esp_now_send(BCAST_ADDR, (uint8_t*)&p, sizeof(p));
    if (r == ESP_OK) sentOk++; else sentErr++;

    if (changed) {
      Serial.printf("TX x=%d y=%d  dir=%s  seq=%u  send=%d\n",
                    x, y, dirToText(dir), p.seq, (int)r);
    }

    prevDir  = dir;
    lastSend = now;
  }

  // Estado periódico
  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("[ESTADO TX] x=%d y=%d (centro %d/%d)  dir=%s  enviados=%lu  errores=%lu\n",
                  x, y, centerX, centerY, dirToText(dir),
                  (unsigned long)sentOk, (unsigned long)sentErr);
  }

  // LED: fijo si hay dirección, si no parpadeo corto cada 1 s
  ledSet(dir != 0 || (now % 1000) < 50);

  delay(10);
}

#else
// ============================ RX ============================

volatile uint8_t  rxDir    = 0;
volatile uint8_t  rxSeq    = 0;
volatile uint32_t rxMs     = 0;
volatile bool     rxNew    = false;
volatile uint8_t  rxMac[6] = {0};
volatile uint32_t rxCount  = 0;   // paquetes válidos
volatile uint32_t rxOther  = 0;   // paquetes ESP-NOW ajenos / descartados

static uint8_t  outDir     = 0;     // lo que está aplicado en los relés
static uint32_t offSinceMs = 0;     // desde cuándo está todo apagado
static bool     armed      = false; // requiere STOP (centro) antes de mover

static const int OUT_PINS[4] = {OUT_F, OUT_B, OUT_L, OUT_R};

static void writeRelay(int pin, bool on) {
  digitalWrite(pin, (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// ¿Pasar de 'from' a 'to' necesita tiempo muerto?
static bool needsDeadTime(uint8_t from, uint8_t to) {
  if (from == 0 || from == to) return false;           // misma dirección o nada antes
  if (!DEAD_TIME_ONLY_REVERSE) return true;            // modo conservador: siempre
  uint8_t both = from | to;
  return both == (BIT_F | BIT_B) || both == (BIT_L | BIT_R);  // inversión en el mismo eje
}

static void setOutputs(uint8_t b) {
  // Primero apagar todo, después prender la que corresponda (nunca dos a la vez)
  for (int i = 0; i < 4; i++) writeRelay(OUT_PINS[i], false);
  if (b == BIT_F) writeRelay(OUT_F, true);
  if (b == BIT_B) writeRelay(OUT_B, true);
  if (b == BIT_L) writeRelay(OUT_L, true);
  if (b == BIT_R) writeRelay(OUT_R, true);
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != (int)sizeof(Packet)) { rxOther++; return; }

  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != MAGIC) { rxOther++; return; }
  if (FILTER_MAC && memcmp(info->src_addr, ALLOWED_TX_MAC, 6) != 0) { rxOther++; return; }

  uint8_t d = p.dirBits;
  if (d != 0 && d != BIT_F && d != BIT_B && d != BIT_L && d != BIT_R) d = 0;  // inválido => STOP

  rxDir = d;
  rxSeq = p.seq;
  memcpy((void*)rxMac, info->src_addr, 6);
  rxMs  = millis();
  rxNew = true;
  rxCount++;
  // Los relés se manejan en loop() (enclavamiento + tiempo muerto)
}

void setup() {
  // Relés en OFF antes y después de pinMode: evita que "clickeen" al arrancar
  for (int i = 0; i < 4; i++) {
    writeRelay(OUT_PINS[i], false);
    pinMode(OUT_PINS[i], OUTPUT);
    writeRelay(OUT_PINS[i], false);
  }

  ledInit();
  serialInit();
  Serial.println("=== PLATAFORMA (RX) ===");
  Serial.printf("Relés activos en %s\n", RELAY_ACTIVE_LOW ? "LOW" : "HIGH");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // CLAVE en RX: con ahorro de energía pierde paquetes y salta el failsafe
  delay(200);
  printLocalMac("RX MAC:");

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init FAILED");
    while (true) { ledSet(true); delay(100); ledSet(false); delay(100); }  // parpadeo rápido = error
  }
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("RX listo (esperando mando)...");
}

void loop() {
  // Leer rxMs ANTES de millis() para que (now - lastRx) nunca dé negativo
  uint32_t lastRx = rxMs;
  uint8_t  dir    = rxDir;
  uint32_t now    = millis();
  bool link = (lastRx != 0) && (now - lastRx <= FAILSAFE_MS);

  if (rxNew) {
    rxNew = false;
    static uint8_t lastPrinted = 0xFF;
    if (dir != lastPrinted) {   // imprimir solo cambios (el keepalive llega cada 50 ms)
      lastPrinted = dir;
      Serial.printf("RX from %02X:%02X:%02X:%02X:%02X:%02X  seq=%u  dir=%s\n",
                    rxMac[0], rxMac[1], rxMac[2], rxMac[3], rxMac[4], rxMac[5],
                    (unsigned)rxSeq, dirToText(dir));
    }
  }

  // ---- Failsafe ----
  static bool linkPrev = false;
  if (!link) {
    if (linkPrev) Serial.println("FAILSAFE: sin paquetes -> STOP (soltar joystick para rearmar)");
    armed = false;
  } else if (!linkPrev) {
    Serial.println("Señal del mando OK");
  }
  linkPrev = link;

  // ---- Qué debería estar activo ----
  uint8_t target = 0;
  if (link) {
    if (dir == 0) {
      if (!armed) { armed = true; Serial.println("ARMADO"); }
    } else if (armed) {
      target = dir;
    } else {
      static uint32_t lastWarn = 0;
      if (now - lastWarn > 1000) {
        lastWarn = now;
        Serial.println("Bloqueado: llevar joystick al centro para armar");
      }
    }
  }

  // ---- Enclavamiento + tiempo muerto ----
  static uint8_t lastActive = 0;   // última dirección que estuvo activa
  if (target != outDir) {
    if (outDir != 0) {
      // Primero cortar lo que está activo
      setOutputs(0);
      outDir = 0;
      offSinceMs = now;
      Serial.println("OUT -> S");
    } else if (!needsDeadTime(lastActive, target) || (now - offSinceMs) >= DEAD_TIME_MS) {
      // Inversión en el mismo eje: espera DEAD_TIME_MS. Resto: inmediato
      setOutputs(target);
      outDir = target;
      lastActive = target;
      Serial.printf("OUT -> %s\n", dirToText(outDir));
    }
  }

  // ---- Estado periódico ----
  static uint32_t lastStatus = 0;
  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    if (lastRx == 0) {
      Serial.printf("[ESTADO RX] nunca recibió del mando  ajenos=%lu\n", (unsigned long)rxOther);
    } else {
      Serial.printf("[ESTADO RX] señal=%s  armado=%s  out=%s  paquetes=%lu  ajenos=%lu  ultimo hace %lu ms\n",
                    link ? "OK" : "NO", armed ? "SI" : "NO", dirToText(outDir),
                    (unsigned long)rxCount, (unsigned long)rxOther,
                    (unsigned long)(now - lastRx));
    }
  }

  // LED: fijo = recibiendo | parpadeo lento = sin señal
  ledSet(link || (now % 1000) < 500);

  delay(5);
}

#endif
