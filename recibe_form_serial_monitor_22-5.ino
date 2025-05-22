#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <time.h>

const char* WIFI_SSID  = "sofi";
const char* WIFI_PASS  = "dominga2";
const char* MQTT_HOST  = "54.80.230.215";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "mqttUser";
const char* MQTT_PASS = "mqttPass";
const char* MQTT_TOPIC = "rfid/ingreso";

#define SS_PIN  5
#define RST_PIN 27
MFRC522 rfid(SS_PIN, RST_PIN);

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

std::vector<String> tarjetasDentro;
unsigned long lastUIDTime = 0;
bool tarjetaLeida = false;
String esp32Id;
String topicDisplay;
std::vector<String> consultas;
bool mostrarConsultas = false;
int indiceConsulta = 0;
unsigned long lastConsultaScroll = 0;
const unsigned long intervaloScroll = 3000;

bool estaDentro(const String& uid) {
  for (const String& t : tarjetasDentro) if (t == uid) return true;
  return false;
}
void agregarTarjeta(const String& uid) { tarjetasDentro.push_back(uid); }
void quitarTarjeta(const String& uid) {
  for (auto it = tarjetasDentro.begin(); it != tarjetasDentro.end(); ++it)
    if (*it == uid) { tarjetasDentro.erase(it); break; }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String nombre = "";
  for (unsigned int i = 0; i < length; i++) nombre += (char)payload[i];
  nombre.trim();

  if (nombre.length() == 0) return;

  Serial.println("Consulta recibida: " + nombre);

  consultas.push_back(nombre);
  mostrarConsultas = true;
  indiceConsulta = consultas.size() - 1;
  lastConsultaScroll = millis();

  Serial.println("Total consultas: " + String(consultas.size()));
  Serial.println("Última: " + nombre);
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.print("\nIP: "); Serial.println(WiFi.localIP());
}

void conectarMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando a MQTT… ");
    if (mqttClient.connect("ESP32-RFID", MQTT_USER, MQTT_PASS)) {
      Serial.println("OK");
      mqttClient.subscribe(topicDisplay.c_str());
    } else {
      Serial.print("Error, rc="); Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  char idBuf[13];
  sprintf(idBuf, "%012llX", (unsigned long long)ESP.getEfuseMac());
  esp32Id = String(idBuf);
  Serial.print("ESP32 ID: "); Serial.println(esp32Id);

  topicDisplay = "esp32/" + esp32Id + "/display";

  conectarWiFi();
  configTime(0, 0, "pool.ntp.org");

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(callback);

  Wire.begin(21, 22);
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  Serial.println("Escanea tarjeta");
}

void loop() {
  if (!mqttClient.connected()) conectarMQTT();
  mqttClient.loop();

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uid += (rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    const char* tipo;
    if (estaDentro(uid)) { quitarTarjeta(uid); tipo = "salida"; }
    else                { agregarTarjeta(uid); tipo = "entrada"; }

    StaticJsonDocument<128> doc;
    doc["uid"]   = uid;
    doc["tipo"]  = tipo;
    doc["ts"]    = (uint32_t)(time(nullptr) - 3 * 3600);
    doc["esp32"] = esp32Id;
    char payload[128];
    serializeJson(doc, payload);

    mqttClient.publish(MQTT_TOPIC, payload);

    String pref = (String(tipo) == "entrada") ? "Entra: " : "Sale: ";
    Serial.println("Ultimo evento:");
    Serial.println((pref + uid));

    tarjetaLeida = true;
    lastUIDTime  = millis();

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  if (tarjetaLeida && millis() - lastUIDTime > 5000) {
    tarjetaLeida = false;

    if (mostrarConsultas && !consultas.empty()) {
      indiceConsulta = 0;
      lastConsultaScroll = millis();

      Serial.println("Total consultas: " + String(consultas.size()));
      Serial.println(String(indiceConsulta + 1) + ": " + consultas[indiceConsulta]);
    } else {
      Serial.println("Escanea tarjeta");
    }
  }

  if (tarjetasDentro.empty() && !consultas.empty()) {
    Serial.println("Todos los profesores salieron. Limpiando consultas.");
    consultas.clear();
    mostrarConsultas = false;
    Serial.println("Escanea tarjeta");
  }

  if (!tarjetaLeida && mostrarConsultas && consultas.size() > 0 &&
      millis() - lastConsultaScroll > intervaloScroll) {
    indiceConsulta = (indiceConsulta + 1) % consultas.size();
    lastConsultaScroll = millis();

    Serial.println("Total consultas: " + String(consultas.size()));
    Serial.println(String(indiceConsulta + 1) + ": " + consultas[indiceConsulta]);
  }

  if (millis() - lastUIDTime > 2000 && !tarjetaLeida) rfid.PCD_Init();
}

