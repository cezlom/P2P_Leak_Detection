// ==========================================================================
// BIBLIOTECAS
// ==========================================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <AdafruitIO_WiFi.h>

// ==========================================================================
// CONSTANTES E DEFINIÇÕES
// ==========================================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SENSOR_PIN A0
#define CONFIG_FILE "/config.json"
#define SERIAL_TIMEOUT 30000
#define PUBLISH_INTERVAL 5000

// ==========================================================================
// VARIÁVEIS GLOBAIS
// ==========================================================================
struct Config {
  char wifi_ssid[34] = "Cayque e Camila 2G";
  char wifi_password[64] = "MINHA#princesa";
  char aio_username[40] = "Cayque_1";
  char aio_key[40] = "aio_FGxB84KBqIDn6Dw2xPapvAFAPNTW";
  char feed_name[60] = "pressao";
};

Config config;
bool onlineMode = false;
unsigned long previousMillis = 0;

// Objetos de hardware
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Objetos Adafruit IO
AdafruitIO_WiFi io(config.aio_username, config.aio_key, config.wifi_ssid, config.wifi_password);
AdafruitIO_Feed *press1 = nullptr;

// ==========================================================================
// FUNÇÕES DE MANIPULAÇÃO DE CONFIGURAÇÃO
// ==========================================================================
bool saveConfiguration() {
  StaticJsonDocument<512> json;
  json["wifi_ssid"] = config.wifi_ssid;
  json["wifi_password"] = config.wifi_password;
  json["aio_username"] = config.aio_username;
  json["aio_key"] = config.aio_key;
  json["feed_name"] = config.feed_name;

  File configFile = LittleFS.open(CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("Falha ao abrir arquivo para escrita");
    return false;
  }

  serializeJson(json, configFile);
  configFile.close();
  Serial.println("Configuração salva com sucesso");
  return true;
}

bool loadConfiguration() {
  if (!LittleFS.begin()) {
    Serial.println("Falha ao montar LittleFS");
    return false;
  }

  if (!LittleFS.exists(CONFIG_FILE)) {
    Serial.println("Arquivo de configuração não encontrado, criando novo");
    return saveConfiguration();
  }

  File configFile = LittleFS.open(CONFIG_FILE, "r");
  if (!configFile) {
    Serial.println("Falha ao abrir arquivo para leitura");
    return false;
  }

  StaticJsonDocument<512> json;
  DeserializationError error = deserializeJson(json, configFile);
  if (error) {
    Serial.println("Erro ao ler arquivo JSON");
    configFile.close();
    return false;
  }

  strlcpy(config.wifi_ssid, json["wifi_ssid"] | "Cayque e Camila 2G", sizeof(config.wifi_ssid));
  strlcpy(config.wifi_password, json["wifi_password"] | "MINHA#princesa", sizeof(config.wifi_password));
  strlcpy(config.aio_username, json["aio_username"] | "Cayque_1", sizeof(config.aio_username));
  strlcpy(config.aio_key, json["aio_key"] | "aio_FGxB84KBqIDn6Dw2xPapvAFAPNTW", sizeof(config.aio_key));
  strlcpy(config.feed_name, json["feed_name"] | "pressao", sizeof(config.feed_name));

  configFile.close();
  Serial.println("Configuração carregada com sucesso");
  return true;
}

// ==========================================================================
// FUNÇÕES DE CONECTIVIDADE
// ==========================================================================
void connectToAdafruitIO() {
  Serial.print("Conectando ao Adafruit IO... ");
  
  // Inicializa o feed
  press1 = io.feed(config.feed_name);
  
  io.connect();
  
  // Espera pela conexão
  while(io.status() < AIO_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  
  Serial.println();
  Serial.println(io.statusText());
}

void setupOTA() {
  ArduinoOTA.setHostname("sensor-pressao");
  ArduinoOTA.onStart([]() { Serial.println("OTA iniciando..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA finalizado."); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progresso: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Erro OTA [%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR: Serial.println("Autenticação falhou"); break;
      case OTA_BEGIN_ERROR: Serial.println("Falha ao iniciar"); break;
      case OTA_CONNECT_ERROR: Serial.println("Falha na conexão"); break;
      case OTA_RECEIVE_ERROR: Serial.println("Falha na recepção"); break;
      case OTA_END_ERROR: Serial.println("Falha ao finalizar"); break;
    }
  });
  ArduinoOTA.begin();
}

// ==========================================================================
// FUNÇÕES DE INTERFACE SERIAL
// ==========================================================================
String readSerialString() {
  while (Serial.available() > 0) Serial.read(); // Limpa buffer
  unsigned long startTime = millis();
  
  while (Serial.available() == 0) {
    if (millis() - startTime > SERIAL_TIMEOUT) {
      Serial.println("\nTimeout - retornando vazio");
      return "";
    }
    delay(10);
  }
  
  String content = Serial.readStringUntil('\n');
  content.trim();
  return content;
}

void showCurrentConfig() {
  Serial.println("\nConfiguração Atual:");
  Serial.println("-------------------");
  Serial.print("SSID WiFi: ");
  Serial.println(config.wifi_ssid);
  Serial.print("Senha WiFi: ");
  Serial.println("*****");
  Serial.print("Usuário Adafruit IO: ");
  Serial.println(config.aio_username);
  Serial.print("Chave Adafruit IO: ");
  Serial.println("*****");
  Serial.print("Nome do Feed: ");
  Serial.println(config.feed_name);
  Serial.println("-------------------\n");
}

void runConfigMenu() {
  showCurrentConfig();
  
  Serial.println("Deixe em branco para manter o valor atual");
  
  // SSID
  Serial.print("Novo SSID [");
  Serial.print(config.wifi_ssid);
  Serial.print("]: ");
  String newSsid = readSerialString();
  if (newSsid.length() > 0) {
    strlcpy(config.wifi_ssid, newSsid.c_str(), sizeof(config.wifi_ssid));
  }

  // Senha WiFi
  Serial.print("Nova Senha WiFi [*****]: ");
  String newPassword = readSerialString();
  if (newPassword.length() > 0) {
    strlcpy(config.wifi_password, newPassword.c_str(), sizeof(config.wifi_password));
  }

  // Usuário Adafruit
  Serial.print("Novo Usuário Adafruit [");
  Serial.print(config.aio_username);
  Serial.print("]: ");
  String newUser = readSerialString();
  if (newUser.length() > 0) {
    strlcpy(config.aio_username, newUser.c_str(), sizeof(config.aio_username));
  }

  // Chave Adafruit
  Serial.print("Nova Chave Adafruit [*****]: ");
  String newKey = readSerialString();
  if (newKey.length() > 0) {
    strlcpy(config.aio_key, newKey.c_str(), sizeof(config.aio_key));
  }

  // Nome do Feed
  Serial.print("Novo Nome do Feed [");
  Serial.print(config.feed_name);
  Serial.print("]: ");
  String newFeed = readSerialString();
  if (newFeed.length() > 0) {
    strlcpy(config.feed_name, newFeed.c_str(), sizeof(config.feed_name));
  }

  if (saveConfiguration()) {
    Serial.println("\nConfiguração salva com sucesso!");
    showCurrentConfig();
    Serial.println("Reinicialize o dispositivo para aplicar as mudanças");
  } else {
    Serial.println("\nErro ao salvar configuração!");
  }

  while (true) delay(1000); // Trava para forçar reinicialização
}

// ==========================================================================
// FUNÇÕES PRINCIPAIS
// ==========================================================================
void setupDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha ao iniciar display OLED"));
    while (true);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Iniciando...");
  display.setCursor(0, 17);
  display.println("Conectando Wi-Fi...");
  display.display();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nIniciando dispositivo...");
  
  setupDisplay();
  
  if (!loadConfiguration()) {
    Serial.println("Usando configuração padrão");
  }
  
  // Conecta ao WiFi e Adafruit IO
  if (WiFi.begin(config.wifi_ssid, config.wifi_password)) {
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      onlineMode = true;
      Serial.println("\nWiFi conectado!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      
      setupOTA();
      connectToAdafruitIO();

     display.clearDisplay();
     display.setTextSize(1);
     display.setCursor(0, 0);
     display.print("Conectado");
     display.setCursor(0, 28);
     display.setTextSize(4);
     display.print("Hello");
     display.display();
     delay(500);
      
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("MODO ONLINE");
      display.setCursor(0, 20);
      display.print("IP: ");
      display.println(WiFi.localIP());
      display.display();
      delay(1000);
    }
  }
  
  if (!onlineMode) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MODO OFFLINE");
    display.display();
  }
  
  Serial.println("\nPronto. Envie 'menu' para configurar");
}

void loop() {
  // Verifica comandos serial
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.equalsIgnoreCase("menu")) {
      runConfigMenu();
    } else if (cmd.equalsIgnoreCase("status")) {
      showCurrentConfig();
    }
  }

  // Mantém conexões ativas
  if (onlineMode) {
    ArduinoOTA.handle();
    io.run(); // Mantém a conexão MQTT ativa
    
    if (io.status() != AIO_CONNECTED) {
      onlineMode = false;
      Serial.println("Desconectado do Adafruit IO");
    }
  }

  // Publica dados periodicamente
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= PUBLISH_INTERVAL) {
    previousMillis = currentMillis;
    
    int leituraADC = analogRead(SENSOR_PIN);
    float tensao = leituraADC * (1.0 / 1023.0);
    float pressao_kgfcm2 = (tensao - 0.059) * 8.1301;
    if (pressao_kgfcm2 < 0) pressao_kgfcm2 = 0;

    // Exibe no display
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Pressao:");
    display.setCursor(78, 0);
    display.println("kgf/cm2");
    display.setCursor(80, 56);
    display.print(onlineMode ? "Online" : "Offline");
    
    display.setTextSize(4);
    display.setCursor(10, 18);
    display.print(pressao_kgfcm2, 2);
    display.display();

    // Log serial
    Serial.print("Pressão: ");
    Serial.print(pressao_kgfcm2, 2);
    Serial.println(" kgf/cm²");

    // Publica no Adafruit IO se estiver online
    if (onlineMode && press1) {
      if (!press1->save(pressao_kgfcm2)) {
        Serial.println("Falha ao publicar dados!");
      } else {
        Serial.println("Dados publicados com sucesso!");
      }
    }
  }
}
