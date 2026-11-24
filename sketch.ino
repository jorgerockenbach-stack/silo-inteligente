#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <time.h>

#include "certificado.h"
#include "secrets.h"

// COMPONENTES
#define DHTPIN 15
#define DHTTYPE DHT22
#define LED_PIN 2

#define UMIDADE_LIGAR 70.0
#define UMIDADE_DESLIGAR 65.0

// TOPICOS MQTT
#define TOP_TELEMETRIA "sis1a/silo/graos/telemetria"
#define TOP_ESTADO "sis1a/silo/graos/estado"
#define TOP_COMANDO "sis1a/silo/graos/comando"
#define TOP_ALERTA "sis1a/silo/graos/alerta"
#define TOP_ACK "sis1a/silo/graos/ack"

DHT dht(DHTPIN, DHTTYPE);
WiFiClientSecure tls;
PubSubClient mqtt(tls);

float temperatura = 0;
float umidade = 0;

bool ventilador = false;
bool sensorValido = false;
bool modoAuto = true;
bool alertaEnviado = false;

unsigned long ultimaLeitura = 0;
unsigned long ultimaPublicacao = 0;
unsigned long ultimaTentativa = 0;
unsigned long ultimoAlerta = 0;

const unsigned long INTERVALO_LEITURA = 2000;
const unsigned long INTERVALO_ENVIO = 5000;
const unsigned long COOLDOWN = 60000;

SemaphoreHandle_t mutexDados;

// CONTROLE LOCAL
void acionarVentilador(bool estado) {
  ventilador = estado;
  digitalWrite(LED_PIN, estado ? HIGH : LOW);
}

void aplicarRegra() {
  if (!sensorValido) {
    acionarVentilador(false);
    return;
  }

  if (modoAuto) {
    if (umidade >= UMIDADE_LIGAR) {
      acionarVentilador(true);
    }
    else if (umidade <= UMIDADE_DESLIGAR) {
      acionarVentilador(false);
    }
  }
}

// PUBLICAR ESTADO
void publicarEstado() {
  StaticJsonDocument<256> doc;

  xSemaphoreTake(mutexDados, portMAX_DELAY);
  doc["dispositivo"] = "silo-01";
  doc["ventilador"] = ventilador;
  doc["modo"] = modoAuto ? "AUTO" : "MANUAL";
  doc["sensor_ok"] = sensorValido;
  xSemaphoreGive(mutexDados);

  char mensagem[256];
  serializeJson(doc, mensagem);

  mqtt.publish(TOP_ESTADO, mensagem, true);
}

// PUBLICAR TELEMETRIA
void publicarTelemetria() {
  StaticJsonDocument<256> doc;

  xSemaphoreTake(mutexDados, portMAX_DELAY);

  doc["dispositivo"] = "silo-01";

  if (sensorValido) {
    doc["temp_c"] = temperatura;
    doc["umid_pct"] = umidade;
  } else {
    doc["temp_c"] = nullptr;
    doc["umid_pct"] = nullptr;
  }

  doc["ventilador"] = ventilador;
  doc["sensor_ok"] = sensorValido;
  doc["modo"] = modoAuto ? "AUTO" : "MANUAL";

  xSemaphoreGive(mutexDados);

  doc["ts"] = (long)time(nullptr);

  char mensagem[256];
  serializeJson(doc, mensagem);

  if (mqtt.publish(TOP_TELEMETRIA, mensagem)) {
    Serial.print("MQTT enviado: ");
    Serial.println(mensagem);
  }

  publicarEstado();
}

// ALERTAS COM COOLDOWN
void verificarAlerta() {
  unsigned long agora = millis();

  StaticJsonDocument<256> doc;

  xSemaphoreTake(mutexDados, portMAX_DELAY);

  bool falha = !sensorValido;
  bool umidadeAlta = sensorValido &&
                     umidade >= UMIDADE_LIGAR;

  if (!falha && !umidadeAlta) {
    xSemaphoreGive(mutexDados);
    return;
  }

  doc["dispositivo"] = "silo-01";
  doc["alerta"] = falha
    ? "FALHA_SENSOR"
    : "UMIDADE_ELEVADA";

  if (sensorValido) {
    doc["umid_pct"] = umidade;
  }

  xSemaphoreGive(mutexDados);

  if (alertaEnviado &&
      agora - ultimoAlerta < COOLDOWN) {
    return;
  }

  doc["ts"] = (long)time(nullptr);

  char mensagem[256];
  serializeJson(doc, mensagem);

  if (mqtt.publish(TOP_ALERTA, mensagem)) {
    ultimoAlerta = agora;
    alertaEnviado = true;
    Serial.println("ALERTA MQTT ENVIADO");
  }
}

// RECEBER COMANDOS
void receberComando(char* topic,
                    byte* payload,
                    unsigned int length) {

  if (strcmp(topic, TOP_COMANDO) != 0) {
    return;
  }

  StaticJsonDocument<256> doc;

  if (deserializeJson(doc, payload, length)) {
    Serial.println("Comando JSON invalido");
    return;
  }

  const char* id = doc["id"] | "";
  const char* comando = doc["comando"] | "";
  long ts = doc["ts"] | 0L;
  long agora = (long)time(nullptr);

  bool executado = false;
  const char* motivo = "";

  xSemaphoreTake(mutexDados, portMAX_DELAY);

  if (strlen(id) == 0 ||
      ts <= 0 || agora < ts ||
      agora - ts > 30) {

    motivo = "COMANDO_INVALIDO_OU_EXPIRADO";

  } else if (strcmp(comando, "AUTO") == 0) {

    modoAuto = true;
    aplicarRegra();
    executado = true;

  } else if (!sensorValido) {

    motivo = "FALHA_SENSOR";

  } else if (strcmp(comando, "LIGAR") == 0) {

    modoAuto = false;
    acionarVentilador(true);
    executado = true;

  } else if (strcmp(comando, "DESLIGAR") == 0) {

    modoAuto = false;
    acionarVentilador(false);
    executado = true;

  } else {

    motivo = "COMANDO_DESCONHECIDO";
  }

  StaticJsonDocument<256> ack;

  ack["id"] = id;
  ack["comando"] = comando;
  ack["executado"] = executado;
  ack["ventilador"] = ventilador;
  ack["motivo"] = motivo;

  xSemaphoreGive(mutexDados);

  char resposta[256];
  serializeJson(ack, resposta);

  mqtt.publish(TOP_ACK, resposta);

  publicarEstado();

  Serial.print("ACK: ");
  Serial.println(resposta);
}

// TAREFA DE REDE SEPARADA
void tarefaRede(void* parametro) {

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin("Wokwi-GUEST", "");

while (WiFi.status() != WL_CONNECTED) {
  Serial.print(".");
  delay(500);
}

Serial.println();
Serial.println("Wi-Fi conectado!");
Serial.print("IP: ");
Serial.println(WiFi.localIP());

  Serial.println("Conectando ao Wi-Fi...");

  configTime(0, 0,
             "pool.ntp.org",
             "time.google.com");

  tls.setCACert(root_ca);
  tls.setHandshakeTimeout(30);

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(receberComando);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(15);

  bool wifiAvisado = false;

  for (;;) {

    if (WiFi.status() != WL_CONNECTED) {

      if (mqtt.connected()) {
        mqtt.disconnect();
      }

      wifiAvisado = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!wifiAvisado) {
      Serial.println("Wi-Fi conectado!");
      wifiAvisado = true;
    }

    if (time(nullptr) < 1700000000) {
      Serial.println("Aguardando sincronizacao NTP...");
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    if (!mqtt.connected()) {

      if (millis() - ultimaTentativa >= 5000) {

        ultimaTentativa = millis();

        Serial.println("Conectando MQTT TLS...");

        if (mqtt.connect(
              MQTT_CLIENT,
              FLESPI_TOKEN,
              "")) {

          Serial.println("MQTT TLS CONECTADO!");

          mqtt.subscribe(TOP_COMANDO, 1);
          publicarEstado();

        } else {

          Serial.print("Erro MQTT: ");
          Serial.println(mqtt.state());
        }
      }

    } else {

      mqtt.loop();

      if (millis() - ultimaPublicacao
          >= INTERVALO_ENVIO) {

        ultimaPublicacao = millis();

        publicarTelemetria();
        verificarAlerta();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// INICIALIZACAO
void setup() {

  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  dht.begin();

  mutexDados = xSemaphoreCreateMutex();

  Serial.println("========================");
  Serial.println("   SILO INTELIGENTE");
  Serial.println("========================");

  xTaskCreatePinnedToCore(
    tarefaRede,
    "RedeMQTT",
    12288,
    NULL,
    1,
    NULL,
    0
  );
}

// AUTOMACAO INDEPENDENTE DA INTERNET
void loop() {

  if (millis() - ultimaLeitura
      >= INTERVALO_LEITURA) {

    ultimaLeitura = millis();

    float t = dht.readTemperature();
    float u = dht.readHumidity();

    xSemaphoreTake(mutexDados, portMAX_DELAY);

    temperatura = t;
    umidade = u;

    sensorValido =
      !isnan(t) && !isnan(u) &&
      t >= -40 && t <= 80 &&
      u >= 0 && u <= 100;

    if (!sensorValido) {

      modoAuto = true;
      acionarVentilador(false);
      Serial.println("FALHA NO SENSOR");

    } else {

      aplicarRegra();

      Serial.printf(
        "Temp: %.1f C | Umid: %.1f %% | Fan: %s\n",
        temperatura,
        umidade,
        ventilador ? "LIGADO" : "DESLIGADO"
      );
    }

    xSemaphoreGive(mutexDados);
  }

  delay(20);
}
