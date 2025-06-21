#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <time.h>

// TODO: que cuando un profesor se va, se vayan tmb sus consultas...

// ------------------------- CONFIGURA AQUÍ -----------------------------------
const char* WIFI_SSID  = "sofi";
const char* WIFI_PASS  = "dominga2";
const char* MQTT_HOST  = "54.80.230.215";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "mqttUser";
const char* MQTT_PASS = "mqttPass";
const char* MQTT_TOPIC = "rfid/ingreso";
// -----------------------------------------------------------------------------

#define SS_PIN  5
#define RST_PIN 27
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x3F, 16, 2);

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

std::vector<String> tarjetasDentro;
unsigned long lastUIDTime = 0;
bool tarjetaLeida = false;
String esp32Id;
String topicDisplay;  // tópico donde recibe mensajes para mostrar
std::vector<String> consultas;
bool mostrarConsultas = false;
int indiceConsulta = 0;
unsigned long lastConsultaScroll = 0;
const unsigned long intervaloScroll = 3000; // cada 3 segundos

// ------------------- UTILIDADES RFID -------------------
bool estaDentro(const String& uid) {
  for (const String& t : tarjetasDentro) if (t == uid) return true;
  return false;
}
void agregarTarjeta(const String& uid) { tarjetasDentro.push_back(uid); }
void quitarTarjeta(const String& uid) {
  for (auto it = tarjetasDentro.begin(); it != tarjetasDentro.end(); ++it)
    if (*it == uid) { tarjetasDentro.erase(it); break; }
}

// ------------------- MQTT CALLBACK ---------------------
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

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Consultas: ");
  lcd.print(consultas.size());
  lcd.setCursor(0, 1);
  lcd.print(String(consultas.size()) + ": " + nombre.substring(0, 13));  // ajusta para largo
}


// ------------------- CONEXIONES ------------------------
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.print("\nIP: "); Serial.println(WiFi.localIP());
}

void conectarMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando a MQTT… ");
    String clientId = "ESP32-RFID-" + esp32Id;
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("OK");
      mqttClient.subscribe(topicDisplay.c_str());
    } else {
      Serial.print("Error, rc="); Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

// ------------------- SETUP -----------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial);

  // ID único del ESP32
  char idBuf[13];
  sprintf(idBuf, "%012llX", (unsigned long long)ESP.getEfuseMac());
  esp32Id = String(idBuf);
  Serial.print("ESP32 ID: "); Serial.println(esp32Id);

  // Tópico personalizado de recepción
  topicDisplay = "esp32/" + esp32Id + "/display";

  conectarWiFi();
  configTime(0, 0, "pool.ntp.org");

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(callback);  // ← para recibir mensajes

  Wire.begin(21, 22);
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  lcd.init();
  lcd.backlight();
  lcd.print("Escanea tarjeta");
}

// ------------------- LOOP ------------------------------
void loop() {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Desconectado. Reintentando conexión...");
    conectarMQTT();
  }
  mqttClient.loop();

  // --- LECTURA DE TARJETA ---
  if (rfid.PICC_IsNewCardPresent()) {
    Serial.println("[RFID] Tarjeta detectada.");
    if (rfid.PICC_ReadCardSerial()) {
      Serial.println("[RFID] UID leído correctamente.");
      String uid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        uid += (rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      Serial.println("[RFID] UID: " + uid);

      const char* tipo;
      if (estaDentro(uid)) {
        quitarTarjeta(uid);
        tipo = "salida";
        Serial.println("[LÓGICA] Ya estaba dentro → será salida");
      } else {
        agregarTarjeta(uid);
        tipo = "entrada";
        Serial.println("[LÓGICA] No estaba dentro → será entrada");
      }

      time_t ts = time(nullptr) - 3 * 3600;
      Serial.print("[TIEMPO] Timestamp: ");
      Serial.println((uint32_t)ts);

      StaticJsonDocument<128> doc;
      doc["uid"]   = uid;
      doc["tipo"]  = tipo;
      doc["ts"]    = (uint32_t)ts;
      doc["esp32"] = esp32Id;

      char payload[128];
      serializeJson(doc, payload);
      Serial.print("[MQTT] Payload: ");
      Serial.println(payload);

      // Publicar
      bool success = mqttClient.publish(MQTT_TOPIC, payload);
      Serial.println(success ? "✅ Publicación exitosa" : "❌ Falló publicación MQTT");

      // Asegurar que el mensaje salga realmente
      for (int i = 0; i < 10; i++) {
        mqttClient.loop();
        delay(10);
      }


      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Ultimo evento:");
      lcd.setCursor(0, 1);
      lcd.print(((String(tipo) == "entrada" ? "Entra: " : "Sale: ") + uid).substring(0, 16));

      tarjetaLeida = true;
      lastUIDTime  = millis();

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    } else {
      Serial.println("❌ Error al leer la tarjeta.");
    }
  }

  // --- ESPERA Y RESTAURACIÓN DE CONSULTAS ---
  if (tarjetaLeida && millis() - lastUIDTime > 5000) {
    Serial.println("[LCD] Pasaron 5s desde última tarjeta.");
    tarjetaLeida = false;

    if (mostrarConsultas && !consultas.empty()) {
      Serial.println("[LCD] Mostrando consultas nuevamente.");
      indiceConsulta = 0;
      lastConsultaScroll = millis();

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Consultas: ");
      lcd.print(consultas.size());

      String linea = String(indiceConsulta + 1) + ": " + consultas[indiceConsulta];
      lcd.setCursor(0, 1);
      lcd.print(linea.substring(0, 16));
    } else {
      Serial.println("[LCD] No hay consultas para mostrar.");
      lcd.clear();
      lcd.print("Escanea tarjeta");
    }
  }

  // --- LIMPIAR CONSULTAS SI TODOS SE FUERON ---
  if (tarjetasDentro.empty() && !consultas.empty()) {
    Serial.println("⚠️ No queda nadie adentro. Limpiando consultas.");
    consultas.clear();
    mostrarConsultas = false;
    lcd.clear();
    lcd.print("Escanea tarjeta");
  }

  // --- SCROLL AUTOMÁTICO DE CONSULTAS ---
  if (!tarjetaLeida && mostrarConsultas && consultas.size() > 0 &&
      millis() - lastConsultaScroll > intervaloScroll) {
    indiceConsulta = (indiceConsulta + 1) % consultas.size();
    lastConsultaScroll = millis();

    Serial.println("[SCROLL] Mostrando consulta " + String(indiceConsulta + 1));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Consultas: ");
    lcd.print(consultas.size());

    String linea = String(indiceConsulta + 1) + ": " + consultas[indiceConsulta];
    lcd.setCursor(0, 1);
    lcd.print(linea.substring(0, 16));
  }

  // --- WATCHDOG RFID ---
  if (millis() - lastUIDTime > 2000 && !tarjetaLeida) {
    Serial.println("[RFID] Reiniciando lector por watchdog.");
    rfid.PCD_Init();
  }
}

