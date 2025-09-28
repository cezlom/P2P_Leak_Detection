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

// NOVAS BIBLIOTECAS PARA AWS IOT
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

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
#define AWS_IOT_PORT 8883 // Porta padrão para MQTT seguro

// ==========================================================================
// VARIÁVEIS GLOBAIS E CONFIGURAÇÃO
// ==========================================================================
struct Config {
  char wifi_ssid[34] = "Cayque e Camila 2G";
  char wifi_password[64] = "MINHA#princesa";
  char aio_username[40] = "Cayque_1";
  char aio_key[40] = "aio_FGxB84KBqIDn6Dw2xPapvAFAPNTW";
  char feed_name[60] = "pressao";
  
  // NOVOS CAMPOS PARA AWS IOT
  char aws_iot_endpoint[100] = "SEU_ENDPOINT_AWS_AQUI.amazonaws.com"; // EX: simig.ats.iot.sa-east-1.amazonaws.com
  char aws_mqtt_topic[60] = "dispositivo/pressao";
};

Config config;
bool onlineMode = false;
unsigned long previousMillis = 0;

// Objetos de hardware
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Objetos Adafruit IO
AdafruitIO_WiFi io(config.aio_username, config.aio_key, config.wifi_ssid, config.wifi_password);
AdafruitIO_Feed *press1 = nullptr;

// NOVOS OBJETOS AWS IOT
WiFiClientSecure aws_client;
PubSubClient mqtt_aws(aws_client);


// ==========================================================================
// CERTIFICADOS AWS IOT (O USUÁRIO DEVE PREENCHER COM SEUS DADOS)
// ==========================================================================

// 1. Certificado CA Raiz da Amazon
const char *aws_root_ca =
    "-----BEGIN CERTIFICATE-----\n"
    "COLE_AQUI_O_CONTEÚDO_DO_SEU_CERTIFICADO_CA_RAIZ\n"
    "-----END CERTIFICATE-----\n";

// 2. Certificado do Dispositivo (Cliente)
const char *aws_device_cert =
    "-----BEGIN CERTIFICATE-----\n"
    "COLE_AQUI_O_CONTEÚDO_DO_SEU_CERTIFICADO_DO_DISPOSITIVO\n"
    "-----END CERTIFICATE-----\n";

// 3. Chave Privada do Dispositivo
const char *aws_private_key =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "COLE_AQUI_O_CONTEÚDO_DA_SUA_CHAVE_PRIVADA\n"
    "-----END RSA PRIVATE KEY-----\n";


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

  // NOVOS CAMPOS AWS
  json["aws_iot_endpoint"] = config.aws_iot_endpoint;
  json["aws_mqtt_topic"] = config.aws_mqtt_topic;

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

  // NOVOS CAMPOS AWS
  strlcpy(config.aws_iot_endpoint, json["aws_iot_endpoint"] | "SEU_ENDPOINT_AWS_AQUI.amazonaws.com", sizeof(config.aws_iot_endpoint));
  strlcpy(config.aws_mqtt_topic, json["aws_mqtt_topic"] | "dispositivo/pressao", sizeof(config.aws_mqtt_topic));

  configFile.close();
  Serial.println("Configuração carregada com sucesso");
  return true;
}

// ==========================================================================
// FUNÇÕES DE CONECTIVIDADE
// ==========================================================================

// Função existente para Adafruit IO
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

// NOVA FUNÇÃO PARA AWS IOT
void connectToAWSIoT() {
  Serial.print("Conectando ao AWS IoT Core... ");

  if (strlen(aws_root_ca) < 100) {
    Serial.println("ERRO: Certificados AWS não configurados!");
    return;
  }
  
  // Configura os certificados para o cliente seguro
  aws_client.setTrustAnchors(aws_root_ca);
  aws_client.setClientRSACert(aws_device_cert, aws_private_key);
  
  // Configura o cliente MQTT
  mqtt_aws.setServer(config.aws_iot_endpoint, AWS_IOT_PORT);

  // Tenta conectar
  String clientId = "ESP8266-" + String(micros());
  if (mqtt_aws.connect(clientId.c_str())) {
    Serial.println("Conectado ao AWS IoT!");
  } else {
    Serial.print("Falha na conexão AWS IoT. Código: ");
    Serial.println(mqtt_aws.state());
  }
}

// Função existente para OTA
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
// ... (funções readSerialString e setupDisplay permanecem inalteradas) ...

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
  
  // NOVOS CAMPOS AWS
  Serial.print("Endpoint AWS IoT: ");
  Serial.println(config.aws_iot_endpoint);
  Serial.print("Tópico MQTT AWS: ");
  Serial.println(config.aws_mqtt_topic);
  
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

  // NOVO: Endpoint AWS
  Serial.print("Novo Endpoint AWS IoT [");
  Serial.print(config.aws_iot_endpoint);
  Serial.print("]: ");
  String newEndpoint = readSerialString();
  if (newEndpoint.length() > 0) {
    strlcpy(config.aws_iot_endpoint, newEndpoint.c_str(), sizeof(config.aws_iot_endpoint));
  }

  // NOVO: Tópico AWS
  Serial.print("Novo Tópico MQTT AWS [");
  Serial.print(config.aws_mqtt_topic);
  Serial.print("]: ");
  String newTopic = readSerialString();
  if (newTopic.length() > 0) {
    strlcpy(config.aws_mqtt_topic, newTopic.c_str(), sizeof(config.aws_mqtt_topic));
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
  
  // Conecta ao WiFi
  if (WiFi.begin(config.wifi_ssid, config.wifi_password)) {
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      onlineMode = true; // Indica conectividade geral
      Serial.println("\nWiFi conectado!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      
      setupOTA();
      
      // Conecta ao Adafruit IO
      connectToAdafruitIO();

      // Conecta ao AWS IoT Core
      connectToAWSIoT();

      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.println("MODO ONLINE DUPLO");
      display.setCursor(0, 10);
      display.print("IO: ");
      display.println(io.status() == AIO_CONNECTED ? "OK" : "FALHA");
      display.setCursor(0, 20);
      display.print("AWS: ");
      display.println(mqtt_aws.connected() ? "OK" : "FALHA");
      display.setCursor(0, 30);
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
    
    // Mantém a conexão Adafruit IO ativa (MQTT)
    io.run(); 
    
    // Mantém a conexão AWS IoT ativa (reconexão automática se necessário)
    if (!mqtt_aws.connected() && WiFi.status() == WL_CONNECTED) {
      Serial.println("Tentando reconectar AWS IoT...");
      connectToAWSIoT();
    } else {
      mqtt_aws.loop(); // Processa tráfego MQTT da AWS
    }
    
    // Verifica se perdeu ambas as conexões para evitar erros
    if (io.status() != AIO_CONNECTED && !mqtt_aws.connected()) {
        // onlineMode = false; // Não é estritamente necessário, mas pode ajudar na clareza
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
    // Indica o status de ambas as conexões
    bool aio_ok = io.status() == AIO_CONNECTED;
    bool aws_ok = mqtt_aws.connected();
    
    if (aio_ok && aws_ok) {
        display.print("IO/AWS OK");
    } else if (aio_ok) {
        display.print("IO OK");
    } else if (aws_ok) {
        display.print("AWS OK");
    } else {
        display.print("Offline");
    }
    
    display.setTextSize(4);
    display.setCursor(10, 18);
    display.print(pressao_kgfcm2, 2);
    display.display();

    // Log serial
    Serial.print("Pressão: ");
    Serial.print(pressao_kgfcm2, 2);
    Serial.println(" kgf/cm²");

    // =======================================================================
    // PUBLICAÇÃO 1: Adafruit IO
    // =======================================================================
    if (onlineMode && press1 && io.status() == AIO_CONNECTED) {
      if (!press1->save(pressao_kgfcm2)) {
        Serial.println("Falha ao publicar dados no Adafruit IO!");
      } else {
        Serial.println("Dados publicados com sucesso no Adafruit IO!");
      }
    }

    // =======================================================================
    // PUBLICAÇÃO 2: AWS IoT Core
    // =======================================================================
    if (onlineMode && mqtt_aws.connected()) {
      // Cria um JSON payload
      String payload = "{\"pressao\": " + String(pressao_kgfcm2, 2) + "}";
      
      if (mqtt_aws.publish(config.aws_mqtt_topic, payload.c_str())) {
        Serial.println("Dados publicados com sucesso no AWS IoT!");
      } else {
        Serial.print("Falha ao publicar dados no AWS IoT. Código: ");
        Serial.println(mqtt_aws.state());
      }
    }
  }
}
