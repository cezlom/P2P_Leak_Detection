#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <math.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>

/* === SIMIG v2.3 - HIGH PRECISION OVERSAMPLING ===
   Placa: ESP32-S3 Dev Module
   AWS IoT Core: MQTT/TLS 8883 + X.509
   + PID local (atuador via PWM) + buzzer de alarme (< 3 bar)
*/

// =======================
// OBJETOS
// =======================
Preferences prefs;
WiFiClientSecure net;          // TLS
PubSubClient client(net);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// =======================
// REDE (WiFi)
// =======================
const char* WIFI_SSID = "Machado";
const char* WIFI_PASS = "22394729Cezar@";

// =======================
// AWS IoT Core (AJUSTE O ENDPOINT)
// =======================
const char* AWS_IOT_ENDPOINT = "SEU_ENDPOINT_AWS_IOT"; // ex: xxxxx-ats.iot.sa-east-1.amazonaws.com
const int   AWS_IOT_PORT     = 8883;

const char* MQTT_CLIENT_ID   = "ESP_S3_UTI_05";
const char* MQTT_TOPIC       = "hospital/uti_a/leito_05/dados";
const char* STATUS_TOPIC     = "hospital/status";

// Certificados no SPIFFS (faça upload em /data/certs/)
const char* AWS_CA_FILE   = "/certs/AmazonRootCA1.pem";
const char* AWS_CERT_FILE = "/certs/device.pem.crt";
const char* AWS_KEY_FILE  = "/certs/private.pem.key";

// Mantém os PEM em memória (WiFiClientSecure usa os ponteiros)
static String g_ca, g_cert, g_key;

// =======================
// PINOS (MANTIDOS DO SEU CÓDIGO)
// =======================
#define PIN_NTC1      1
#define PIN_NTC2      2
#define PIN_FLUXO_1   4
#define PIN_FLUXO_2   5
#define PIN_PRESS_IN  6
#define PIN_PRESS_OUT 7
#define PIN_BUZZER    9   // beep do teclado (se você quiser manter)
#define PIN_SDA       16
#define PIN_SCL       17

// --- NOVOS PINOS (ESCOLHA GPIOs LIVRES NO SEU BOARD) ---
#define PIN_BUZZER_ALARM  13  // buzzer/alarme em pino livre
#define PIN_ACTUATOR_PWM  14  // saída PWM do atuador (válvula/driver)

// =======================
// TECLADO
// =======================
byte rowPins[4] = {18, 19, 20, 21};
byte colPins[3] = {10, 11, 12};
char keys[4][3] = {{'1','2','3'},{'4','5','6'},{'7','8','9'},{'*','0','#'}};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, 4, 3);

// =======================
// VARIÁVEIS DE CALIBRAÇÃO
// =======================
float offsets[4] = {0.0, 0.0, 0.0, 0.0};
float escalas[4] = {1.0, 1.0, 1.0, 1.0};
const float pontosRefFluxo[4] = {3.0, 6.0, 9.0, 12.0};

// =======================
// UI / TIMERS
// =======================
int modoExibicao = 0;
int paginaAuto = 1;
unsigned long tempoTrocaPagina = 0;

unsigned long lastPublish = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;

// Para não “moer” 1024x leituras a cada loop:
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL_MS    = 200;
const unsigned long PUBLISH_INTERVAL_MS = 5000;

// Leituras atuais
float f1=0, f2=0, p1=0, p2=0, t1=0, t2=0;

// =======================
// PID LOCAL + ALARME
// =======================
// Controle usando p1 (pressão IN) em bar
const float SP_PRESS_BAR    = 3.50;   // setpoint do PID (ajuste conforme sua planta)
const float ALARM_ON_BAR    = 3.00;   // dispara alarme se cair abaixo
const float ALARM_OFF_BAR   = 3.10;   // desliga quando recuperar acima (histerese)
const unsigned long ALARM_DELAY_MS = 1000; // precisa ficar abaixo por 1s

// Intervalo do PID (combina com READ_INTERVAL_MS)
const unsigned long PID_INTERVAL_MS = 200;

// Ganhos PID (iniciais — ajuste fino obrigatório)
float Kp = 1200.0f;
float Ki = 150.0f;
float Kd = 0.0f;

// Estado PID
float pidIntegral = 0.0f;
float pidPrevError = 0.0f;
unsigned long lastPid = 0;

// PWM (LEDC) no ESP32
const int PWM_CH   = 0;
const int PWM_FREQ = 2000;   // 2 kHz
const int PWM_RES  = 12;     // 0..4095
const int PWM_MAX  = (1 << PWM_RES) - 1;

// Alarme
bool alarmActive = false;
unsigned long alarmBelowSince = 0;

// =======================
// UTIL: FS + NTP + TLS
// =======================
static bool readFileToString(fs::FS& fs, const char* path, String& out) {
  File f = fs.open(path, "r");
  if (!f) return false;
  out = f.readString();
  f.close();
  out.trim();
  return out.length() > 0;
}

static bool syncTime(uint32_t timeoutMs = 20000) {
  // UTC suficiente para validação TLS
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    if (getLocalTime(&timeinfo, 1000)) return true;
    delay(50);
  }
  return false;
}

static bool loadAwsCertsFromSpiffs() {
  if (!SPIFFS.begin(true)) return false;

  if (!readFileToString(SPIFFS, AWS_CA_FILE, g_ca))     return false;
  if (!readFileToString(SPIFFS, AWS_CERT_FILE, g_cert)) return false;
  if (!readFileToString(SPIFFS, AWS_KEY_FILE, g_key))   return false;

  net.setCACert(g_ca.c_str());
  net.setCertificate(g_cert.c_str());
  net.setPrivateKey(g_key.c_str());

  net.setHandshakeTimeout(15);
  return true;
}

// =======================
// LEITURA (OVERSAMPLING)
// =======================
float lerRaw(int pino) {
  unsigned long soma = 0;
  for(int i=0; i<1024; i++) {
    soma += analogRead(pino);
    delayMicroseconds(2);
  }
  float rawMedia = (float)soma / 1024.0f;
  return rawMedia * (3.3f / 4095.0f);
}

float lerSensor(int pino, int idx) {
  float tensaoLida = lerRaw(pino);
  float tensaoEfetiva = tensaoLida - offsets[idx];
  if(tensaoEfetiva < 0.001f) tensaoEfetiva = 0.0f;

  if(idx == 0 || idx == 1) {
    // FLUXO: raiz quadrada
    return escalas[idx] * sqrt(tensaoEfetiva);
  } else {
    // PRESSÃO: linear
    return escalas[idx] * tensaoEfetiva;
  }
}

float lerNTC(int pino) {
  int raw = analogRead(pino);
  if (raw <= 0 || raw >= 4095) return 0;
  float resistencia = 10000.0f * ((4095.0f / (float)raw) - 1.0f);
  float tempK = 1.0f / (1.0f / (25.0f + 273.15f) + log(resistencia / 10000.0f) / 3950.0f);
  return tempK - 273.15f;
}

// =======================
// PERSISTÊNCIA
// =======================
void carregarCalibracao() {
  prefs.begin("calib", true);
  for(int i=0; i<4; i++) {
    char kE[10], kO[10];
    sprintf(kE, "e%d", i); sprintf(kO, "o%d", i);
    escalas[i] = prefs.getFloat(kE, 0.0f);
    offsets[i] = prefs.getFloat(kO, 0.0f);
    if(escalas[i] == 0) escalas[i] = 1.0f;
  }
  prefs.end();
}

void salvarCalib(int idx, float esc, float off) {
  prefs.begin("calib", false);
  char kE[10], kO[10];
  sprintf(kE, "e%d", idx); sprintf(kO, "o%d", idx);
  prefs.putFloat(kE, esc);
  prefs.putFloat(kO, off);
  prefs.end();
  escalas[idx] = esc;
  offsets[idx] = off;
}

// =======================
// MENU DE CALIBRAÇÃO
// =======================
void menuCalibracao() {
  Serial.println("\n=== CALIBRAÇÃO DE ALTA PRECISÃO ===");
  Serial.println("0: Fluxo 1");
  Serial.println("1: Fluxo 2");
  Serial.println("2: Pressao IN");
  Serial.println("3: Pressao OUT");

  while(!Serial.available());
  int idx = Serial.parseInt();
  int pino = (idx==0)?PIN_FLUXO_1:(idx==1)?PIN_FLUXO_2:(idx==2)?PIN_PRESS_IN:PIN_PRESS_OUT;

  float vZero = 0;
  Serial.println("\n>>> PASSO 1: CALIBRAR ZERO");
  Serial.println("Deixe em repouso e envie 'c'...");

  while(true) {
      if(Serial.available() && Serial.read() == 'c') break;
      Serial.printf("Lendo Zero: %.4f V\r", lerRaw(pino)); delay(100);
  }
  vZero = lerRaw(pino);
  Serial.printf("\nZERO SALVO: %.4f V\n", vZero);

  if(idx == 0 || idx == 1) {
    float leituras[4];
    for(int i=0; i<4; i++) {
        Serial.printf("\nAplique %.1f L/min e envie 'c'...\n", pontosRefFluxo[i]);
        while(true) {
          if(Serial.available() && Serial.read() == 'c') break;
          float atual = lerRaw(pino);
          Serial.printf("Raw: %.4f V | Delta: %.4f V\r", atual, atual - vZero);
          delay(100);
        }
        leituras[i] = lerRaw(pino);
    }

    float soma = 0; int n=0;
    for(int i=0; i<4; i++) {
       float deltaV = leituras[i] - vZero;
       if(deltaV > 0.002f) {
           soma += pontosRefFluxo[i] / sqrt(deltaV);
           n++;
       }
    }

    if(n > 0) {
        float novaEscala = soma/n;
        salvarCalib(idx, novaEscala, vZero);
        Serial.printf("\nSUCESSO! Zero: %.4f, Escala: %.4f\n", vZero, novaEscala);
    } else {
        Serial.println("\nERRO CRÍTICO: Sinal muito baixo (< 2mV).");
        Serial.println("Tentando salvar calibração de emergência...");
        float maxDelta = leituras[3] - vZero;
        if(maxDelta > 0.0005f) {
           float escEmergencia = pontosRefFluxo[3] / sqrt(maxDelta);
           salvarCalib(idx, escEmergencia, vZero);
           Serial.printf("Calibração Forçada Salva: %.4f (Pode ser imprecisa)\n", escEmergencia);
        }
    }

  } else {
    float vMax;
    Serial.println("\n>>> PASSO 2: APLIQUE 5 BAR");
    Serial.println("Envie 'c' quando estabilizar...");

    while(true) {
        if(Serial.available() && Serial.read() == 'c') break;
        Serial.printf("Lendo: %.4f V\r", lerRaw(pino)); delay(100);
    }
    vMax = lerRaw(pino);

    float deltaV = vMax - vZero;
    if(deltaV > 0.05f) {
        float novaEscala = 5.0f / deltaV;
        salvarCalib(idx, novaEscala, vZero);
        Serial.printf("\nPRESSAO OK! Zero: %.4f, Escala: %.4f\n", vZero, novaEscala);
    } else {
        Serial.println("ERRO: Sensor de pressão não respondeu.");
    }
  }
}

// =======================
// DISPLAY
// =======================
void mostrarTela(int pg, float f1, float f2, float p1, float p2, float t1, float t2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);

  display.printf("WiFi:%s MQTT:%s %s",
                (WiFi.status()==WL_CONNECTED?"OK":"X"),
                (client.connected()?"OK":"X"),
                (modoExibicao==0?"A":"M"));

  if (alarmActive) {
    display.setCursor(100, 0);
    display.print("!");
  }

  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setTextSize(2);
  if (pg == 1) {
    display.setCursor(0, 15); display.print("FLUXO L/m");
    display.setCursor(0, 35); display.printf("1:%.1f", f1);
    display.setCursor(0, 52); display.printf("2:%.1f", f2);
  } else if (pg == 2) {
    display.setCursor(0, 15); display.print("PRESSAO Bar");
    display.setCursor(0, 35); display.printf("I:%.2f", p1);
    display.setCursor(0, 52); display.printf("O:%.2f", p2);
  } else if (pg == 3) {
    display.setCursor(0, 15); display.print("TEMP C");
    display.setCursor(0, 35); display.printf("1:%.1f", t1);
    display.setCursor(0, 52); display.printf("2:%.1f", t2);
  }
  display.display();
}

// =======================
// ALARME + PID
// =======================
static void updateAlarm(float pressBar) {
  unsigned long now = millis();

  if (!alarmActive) {
    if (pressBar < ALARM_ON_BAR) {
      if (alarmBelowSince == 0) alarmBelowSince = now;
      if (now - alarmBelowSince >= ALARM_DELAY_MS) {
        alarmActive = true;
        alarmBelowSince = 0;
      }
    } else {
      alarmBelowSince = 0;
    }
  } else {
    if (pressBar > ALARM_OFF_BAR) {
      alarmActive = false;
    }
  }

  // Buzzer pulsado quando em alarme
  if (alarmActive) {
    digitalWrite(PIN_BUZZER_ALARM, (now / 300) % 2); // alterna a cada 300ms
  } else {
    digitalWrite(PIN_BUZZER_ALARM, LOW);
  }
}

static void updatePid(float pressBar) {
  unsigned long now = millis();
  if (now - lastPid < PID_INTERVAL_MS) return;

  float dt = (lastPid == 0) ? (PID_INTERVAL_MS / 1000.0f) : ((now - lastPid) / 1000.0f);
  lastPid = now;

  // erro = setpoint - medição
  float error = SP_PRESS_BAR - pressBar;

  // Integral com anti-windup
  pidIntegral += error * dt;
  const float I_LIMIT = 5.0f; // bar*s (ajuste)
  if (pidIntegral > I_LIMIT) pidIntegral = I_LIMIT;
  if (pidIntegral < -I_LIMIT) pidIntegral = -I_LIMIT;

  float derivative = (dt > 0) ? ((error - pidPrevError) / dt) : 0.0f;
  pidPrevError = error;

  float u = (Kp * error) + (Ki * pidIntegral) + (Kd * derivative);

  // Saída PWM limitada (0..PWM_MAX)
  if (u < 0) u = 0;
  if (u > PWM_MAX) u = PWM_MAX;

  ledcWrite(PWM_CH, (uint32_t)u);
}

// =======================
// MQTT AWS IoT
// =======================
static void mqttConnectAws() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) return;

  // LWT retained: online/offline
  if (client.connect(MQTT_CLIENT_ID, STATUS_TOPIC, 0, true, "offline")) {
    client.publish(STATUS_TOPIC, "online", true);
  }
}

// =======================
// SETUP
// =======================
void setup() {
  Serial.begin(115200);
  delay(1500);

  carregarCalibracao();

  pinMode(PIN_BUZZER, OUTPUT);        // beep do teclado
  pinMode(PIN_BUZZER_ALARM, OUTPUT);  // alarme dedicado
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_BUZZER_ALARM, LOW);

  // PWM do atuador (válvula/driver)
  ledcSetup(PWM_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(PIN_ACTUATOR_PWM, PWM_CH);
  ledcWrite(PWM_CH, 0);

  Wire.begin(PIN_SDA, PIN_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Fail");

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10,20); display.println("SIMIG v2.3");
  display.setCursor(10,40); display.println("AWS+PID+ALM");
  display.display();

  // ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);

  // tenta conectar rapidamente no boot (sem travar infinito)
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 12000) {
    delay(250);
  }

  // TLS: sincroniza tempo (ideal com WiFi conectado) e carrega certs
  bool timeOk = syncTime();
  if (!timeOk) Serial.println("WARN: NTP nao sincronizou (TLS pode falhar).");

  if (!loadAwsCertsFromSpiffs()) {
    Serial.println("ERRO: nao consegui carregar certificados do SPIFFS.");
  }

  client.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
  client.setBufferSize(2048);
  client.setKeepAlive(60);

  Serial.println(">>> Digite 'CAL' para Calibrar");
}

// =======================
// LOOP
// =======================
void loop() {
  // Comandos
  if(Serial.available()) {
    String c = Serial.readStringUntil('\n'); c.trim();
    if(c == "CAL") menuCalibracao();
  }

  // Teclado
  char key = keypad.getKey();
  if (key) {
    // beep do teclado (não interfere no alarme dedicado)
    digitalWrite(PIN_BUZZER, HIGH); delay(50); digitalWrite(PIN_BUZZER, LOW);
    if(key >= '0' && key <= '3') modoExibicao = key - '0';
  }

  // Reconexão WiFi com backoff
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttempt > 5000) {
      lastWifiAttempt = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  // MQTT: tenta conectar com backoff
  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    if (millis() - lastMqttAttempt > 5000) {
      lastMqttAttempt = millis();
      mqttConnectAws();
    }
  }

  // Mantém MQTT vivo
  client.loop();

  // Leitura dos sensores em intervalo
  if (millis() - lastRead > READ_INTERVAL_MS) {
    lastRead = millis();

    f1 = lerSensor(PIN_FLUXO_1, 0);
    f2 = lerSensor(PIN_FLUXO_2, 1);
    p1 = lerSensor(PIN_PRESS_IN, 2);
    p2 = lerSensor(PIN_PRESS_OUT, 3);
    t1 = lerNTC(PIN_NTC1);
    t2 = lerNTC(PIN_NTC2);

    // Alarme e PID rodam localmente (independente da rede)
    updateAlarm(p1);  // buzzer se p1 < 3.0 bar
    updatePid(p1);    // PWM do atuador para manter SP_PRESS_BAR
  }

  // Display
  if (modoExibicao == 0) {
    if (millis() - tempoTrocaPagina > 3000) {
      paginaAuto = (paginaAuto % 3) + 1;
      tempoTrocaPagina = millis();
    }
    mostrarTela(paginaAuto, f1, f2, p1, p2, t1, t2);
  } else {
    mostrarTela(modoExibicao, f1, f2, p1, p2, t1, t2);
  }

  // Publica a cada 5s
  if (client.connected() && (millis() - lastPublish > PUBLISH_INTERVAL_MS)) {
    lastPublish = millis();

    StaticJsonDocument<384> doc;
    doc["sensor_id"] = MQTT_CLIENT_ID;
    doc["f_in"]  = f1; doc["f_out"] = f2; //- fluxo
    doc["p_in"]  = p1; doc["p_out"] = p2; //- pressao
    doc["t1"]    = t1; doc["t2"]    = t2; //- temperatura

    // extras úteis p/ observabilidade
    doc["alarm"] = alarmActive ? 1 : 0;
    doc["sp_bar"] = SP_PRESS_BAR;

    char buf[768];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    client.publish(MQTT_TOPIC, buf, n);
  }
}
