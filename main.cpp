#include <Arduino.h>
#include <Adafruit_AHTX0.h>
#include <WiFiMulti.h>
#include <WiFi.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include "FS.h"

// ------------ DEFINZIONI -------------
// Definisco i pin a cui i sensori sono collegati
#define MQ7 32                  // Rilevatore CO (Monossido di carbonio)
#define MQ4 33                  // Rilevatore Metano

// Pin collegati alla ventola
#define motor1Pin1 27
#define motor1Pin2 26
#define enable1Pin 33

// Velocità ventola
// 255 = 100%
#define SLOW 80
#define MEDIUM 150
#define FAST 255

// Resistenza di riferimento
#define RL 1000                 // 1 kOhm

// Tensione di riferimento
#define V 3.3

// Microcontrollore
#define DEVICE "ESP32"

// Credenziali WiFi
#define WIFI_SSID "Galaxy A34 5G 9655"      // SSID
#define WIFI_PASSWORD "ffg88w3sgnu3sn9"     // Password

//#define WIFI_SSID "TIM-83699574"                 // SSID
//#define WIFI_PASSWORD "VA89zqccfKWwdeCtXd6N"     // Password

// Informazioni per connettersi al DB
#define INFLUXDB_URL "https://us-east-1-1.aws.cloud2.influxdata.com"
#define INFLUXDB_TOKEN "hBoaIv0FPOXc8Ig1xFqf1DPFubWpS3wUoqH_4HsQlniWELqkwDrIXzUXMyU_NixqpxV0ieoWNJA2qr7HSiNOcg=="
#define INFLUXDB_ORG "4bae2f56f0b03886"
#define INFLUXDB_BUCKET "AircheckSSH"

// Time zone
#define TZ_INFO "CET"

// Porta del server HTTP
#define PORT 80

// --------- VARIABILI GLOBALI -----------
float current_Methane;                  // valore relativo di Metano
float current_CO;                       // valore relativo di CO
float current_RH;                       // valore assoluto di umidità

float Ro4, Ro7;                         // valori di riferimento dei sensori

bool sensorData_available = false;      // mi dice se ci sono dei dati nuovi che posso scrivere nel DB

// Dato da scrivere
Point sensor("sensor_data");

// Proprietà PWM
const int freq = 30000;
const int pwmChannel = 0;
const int resolution = 8;
int dutyCycle = 200;

// ------------ DEVICE -------------
// Sensore umidità
Adafruit_AHTX0 aht;

// Task Handle
TaskHandle_t read_handle;       // Lettura da sensori
TaskHandle_t writeDB_handle;    // Scrittura su DB
TaskHandle_t serverWEB_handle;  // Server WEB
TaskHandle_t fan_handle;        // Ventola

// WIFI
WiFiMulti wifiMulti;                // Dispositivo wifi multi-device
AsyncWebServer server(PORT);        // Server per HTTP a porta 80

// Client InfluxDB
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);

// Struttura dati per recupero Resistenze di riferimento e controllo sulla funzione
struct calibInit {
  int control_flag;
  float data;
};


// -------------- FUNZIONI ----------------
// Funzione di preriscaldamento per il sensore MQ7
void preheat(void){
  Serial.println("Riscaldamento sensore MQ7...");

  // Do tensione a 5 V per 60s
  delay(60000);
  Serial.println("Riscaldamento completato");
}

// Funzione di calibrazione dei sensori del gas
// 0 = successo
// 1 = errore nella selezione del sensore
calibInit calib_gas(int sensor) {
  Serial.print("Calibrazione sensore ");

  // Dati di risposta
  calibInit status;
  status.control_flag = 0;
  status.data = 0;

  // Fattore aria pulita
  float CLEAN_AIR_FACTOR;

  // Differenzio il sensore che calibro
  if (sensor == MQ7) {
    Serial.println("MQ7...");

    // Fattore di calibrazione in aria pulita
    CLEAN_AIR_FACTOR = 27;
    // Per rilevare R0 corretto devo fare un ciclo di preriscaldamento
    preheat();
  }
  else if (sensor == MQ4) {
    Serial.println("MQ4...");

    // Fattore di calibrazione in aria pulita
    CLEAN_AIR_FACTOR = 4.4;
  }
  else {
    status.control_flag = 1;
    return status;
  };

  // Valori di Rs per la media
  float Rs_mean = 0;

  for (int i = 0; i < 50; i++){
    // Leggo il valore di resistenza relativo 
    int adc = analogRead(sensor);

    // Calcolo la tensione
    float voltage = adc * (V / 4095);

    // Calcolo RS e lo metto nella somma
    float Rs = RL * ((V - voltage)/voltage);
    Rs_mean += Rs;
  }

  // Calcolo il valore medio di Rs e calcolo la R0 (Di riferimento)
  float R0 = Rs_mean/CLEAN_AIR_FACTOR;
  status.data = R0;

  Serial.println("Calibrazione completata");
  return status;
}

// Funzioni di lettura 
// METANO
float readMethane(void) {
    // Leggo il valore di resistenza relativo 
    int adc = analogRead(MQ4);

    // Calcolo la tensione
    float voltage = adc * (V / 4095);

    // Calcolo RS 
    float Rs = RL * ((V - voltage)/voltage);

    // Calcolo il rapporto tra Rs e R0 e lo restituisco
    return Rs/Ro4;
}
// CO
float readCO(void) {
    // Leggo il valore di resistenza relativo 
    int adc = analogRead(MQ7);

    // Calcolo la tensione
    float voltage = adc * (V / 4095);

    // Calcolo RS 
    float Rs = RL * ((V - voltage)/voltage);

    // Calcolo il rapporto tra Rs e R0 e lo restituisco
    return Rs/Ro7;
}
// UMIDITA
float readRH(void) {
  // Dove scriverò l'umidità e tempratura
  sensors_event_t humidity, temp;

  // Prendo i valori
  aht.getEvent(&humidity, &temp);

  // Restituisco l'umidità
  return humidity.relative_humidity;
}

// Funzioni pagina HTML di risposta
// Funzioni per restituire i singoli dati
// Restituiscono i dati in formato Stringa per spedirli alla pagina WEB
// Metano
String serveData_Meth() {
  String data = String(current_Methane);
  return data;      // Restituisci solo il dato
}
// CO
String serveData_CO() {
  String data = String(current_CO);
  return data;      // Restituisci solo il dato
}
// Umidità
String serveData_RH() {
  String data = String(current_RH);
  return data;      // Restituisci solo il dato
}
// Quzlità dell'aria in percentuale
String calculateQuality() {
  int air_quality;

  // Calcolo la qualità dell'aria moltiplicando i valori di metano e CO, restituisco un intero
  air_quality = current_CO * current_Methane * 100;

  return (String)air_quality;
}

// Sostituzione dato placeholder nell'HTML
String processor(const String& var){
  Serial.println(var);

  if (var == "METHANE"){
    return serveData_Meth();
  }
  else if (var == "CO"){
    return serveData_CO();
  }
  else if (var == "HUMIDITY"){
    return serveData_RH();
  }
  else if (var == "QUALITY") {
    return calculateQuality();
  }

  return String();
}

// Definizione funzioni task
// Lettura da sensori
void readSensors (void *parameters) {
  // Legge sempre dai sensori
  for (;;) {

    // Controllo su seriale
    // Serial.println("Reading...");

    // Leggo i parametri che mi interessano
    current_CO = readCO();
    current_Methane = readMethane();
    current_RH = readRH();

    // I dati sono disponibili per la scrittura
    sensorData_available = true;
    
    // Leggo circa ogni 2 secondi
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}
// Scrittura su DB
void writeSensorData (void *parameters) {
  // Scrivo in continuazione 
  for (;;) {
    // Scrivo solo se nuovi dati sono disponibili inserirli nel DB
    // Potrei perdermi un dato ogni tanto se la scrittura ci mette piu del tempo di lettura dei dati, 
    // ma non è rilevante in quanto rilevo ogni secondo e importa l'andamento, non il singolo dato
    if (sensorData_available) {
      sensorData_available = false;

      // Tolgo i dati vecchi ma resta l'identificativo
      sensor.clearFields();

      // Metto il valore misurato in una colonna del dato
      // Riporta livelli di Metano, CO e Umidità in un certo istante di tempo
      sensor.addField("methane", current_Methane);
      sensor.addField("co", current_CO);
      sensor.addField("humidity", current_RH);

      // Stampo su seriale per controllo
      // Serial.print("Writing: ");
      // Serial.println(sensor.toLineProtocol());

      // Mi riconnetto al DB se mi sono disconnesso per scriverci i dati
      if (wifiMulti.run() != WL_CONNECTED) {
        Serial.println("Wifi connection lost");
      }

      // Scrivo sul DB
      if (!client.writePoint(sensor)) {
        Serial.print("InfluxDB write failed: ");
        Serial.println(client.getLastErrorMessage());
      }
    }

    // Permetto al server di accettare richieste
    vTaskDelay(1000);
  }
}
// Attivazione ventola
void startWind (void *parameters) {
  // Vedo la qualità dell'aria e attivo la ventola a seconda di quanto la qualità è bassa
  int air_quality = calculateQuality().toInt();
  // velocità ventola
  int fan_speed;  

  // Attività:
  // 75 < Q < 100    -->    velocità lenta
  // 50 < Q < 75     -->    velocità media
  // 0  < Q < 50     -->    velocità veloce
  if ( air_quality >= 75 ) fan_speed = SLOW;
  else if ( air_quality >= 50 ) fan_speed = MEDIUM;
  else fan_speed = FAST;

  // Attivo la ventola
  digitalWrite(motor1Pin1, LOW);
  digitalWrite(motor1Pin2, HIGH);
  ledcWrite(pwmChannel, fan_speed);

  // La ventola gira per 10 secondi prima di controllare di nuovo la qualità dell'aria e modificare
  // eventualmente la velocità della ventola
  vTaskDelay(10000);
}

void setup() {
  // Inizilizzazioni
  Serial.begin(115200);         // Seriale per stampe e controlli in-line
  pinMode(MQ7, INPUT);          // Sensore CO
  pinMode(MQ4, INPUT);          // Sensore Met

  // Avvio sensori
  aht.begin();                  // Sensore AHT20 per umidità
  // Eseguo la calibrazione dei sensori
  calibInit init_4 = calib_gas(MQ4);
  calibInit init_7 = calib_gas(MQ7);

  // Se imposto i valori di riferimento a negativo in caso di errore posso individuare che c'è stato un problema nella
  // calibrazione dei sensori
  if (init_4.control_flag == 0) Ro4 = init_4.data; 
  else Ro4 = -1;

  if (init_7.control_flag == 0) Ro7 = init_7.data; 
  else Ro7 = -1;

  // ---------------- VENTOLA ---------------
  // Imposto i pin come output
  pinMode(motor1Pin1, OUTPUT);
  pinMode(motor1Pin2, OUTPUT);
  ledcSetup(pwmChannel, freq, resolution);
  // Configuro PWM
  ledcAttachPin(enable1Pin, pwmChannel);


  // ------------------ WIFI ------------------
  // Setup wifi
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Connecting to wifi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.println();
  

  // --------------- DB ----------------
  // Prendo il tempo preciso, necessario per scrivere nel DB Influx
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  // Controllo la connessione al DB
  if (client.validateConnection()) {
      Serial.print("Connected to InfluxDB: ");
      Serial.println(client.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
  }

  // Metadati necessari per l'identificazione del dato
  sensor.addTag("device", DEVICE);
  sensor.addTag("SSID", WiFi.SSID());

  // ------------- HTTP ---------------
  // Inizializzo LittleFS
  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // Root page 
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", String(), false, processor);
  });

  // Richieste dei singoli dati
  server.on("/co", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", serveData_CO().c_str());
  });
  
  server.on("/humidity", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", serveData_RH().c_str());
  });
  
  server.on("/methane", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", serveData_Meth().c_str());
  });

  server.on("/quality", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", calculateQuality().c_str());
  });

  // Pagine di dettagli
  server.on("/details_humidity", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/humidity.html", String(), false, processor);
  });

  server.on("/details_methane", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/methane.html", String(), false, processor);
  });

  server.on("/details_co", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/co.html", String(), false, processor);
  });


  // Avvia server
  server.begin();
  Serial.print("Server available at address: ");
  Serial.println(WiFi.localIP());

  // --------------- TASK -----------------
  // Creazione Task
  // Task di lettura dai sensori
  xTaskCreatePinnedToCore (
    readSensors,                // Funzione da eseguire
    "readSensors",              // Nome task
    10000,                      // Stack size
    NULL,                       // Parametri della funzione da eseguire
    2,                          // Priorità
    &read_handle,               // Task handle
    1                           // Core d'appoggio
  );

  // Task di scrittura su DB
  xTaskCreatePinnedToCore (
    writeSensorData,
    "writeSensorData",
    10000,
    NULL,
    1,
    &writeDB_handle,
    1
  );

  // Task di attivazione ventola
  xTaskCreatePinnedToCore (
    startWind,
    "startWind",
    10000,
    NULL,
    2,
    &fan_handle,
    1
  );
}

void loop() {
}
