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


/*
----------------------------------------------------------------
1 - Controllo automatico e/o manuale della ventola
2 - Partitore di tensione per ridurre uscita 5V a max 3.3V --> r2/(r1 + r2) = 2/3
3 - MQTT per dati 
----------------------------------------------------------------
*/

// ------------ DEFINZIONI -------------
// Definisco i pin a cui i sensori sono collegati
#define MQ7 32                  // Rilevatore CO (Monossido di carbonio)
#define MQ4 33                  // Rilevatore Metano

// Pin collegati alla ventola
#define motor1Pin1 12
#define motor1Pin2 14
#define enable1Pin 13

// Velocità ventola
// 255 = 100%
#define SLOW 80
#define MEDIUM 150
#define FAST 255

// Resistenza di riferimento
#define RL 1000                 // 1 kOhm

// Tensione di riferimento
#define V 5.0

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
SemaphoreHandle_t dataMutex;            // Semaforo per gestire l'accesso alle variabili globali

float current_Methane;                  // valore relativo di Metano
float current_CO;                       // valore relativo di CO
float current_RH;                       // valore assoluto di umidità

float Ro4, Ro7;                         // valori di riferimento dei sensori

bool sensorData_available = false;      // mi dice se ci sono dei dati nuovi che posso scrivere nel DB

bool manual = false;                    // mi dice se sto controllando automaticamente la ventola o no

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
// 2 = sensori scollegati e/o malfunzionanti
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
    // preheat();
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
    // Evito il problema della divisione per 0 se non c'è tensione
    if (voltage <= 0.01) {
      status.control_flag = 2;
      return status;
    }

    // Calcolo RS e lo metto nella somma
    float Rs = RL * ((V - voltage)/voltage);
    Rs_mean += Rs;

    // Aspetto 0.1 secondi per eseguire una nuova misurazione
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  // Calcolo il valore medio di Rs e calcolo la R0 (Di riferimento)
  Rs_mean /= 50.0;
  float R0 = Rs_mean/CLEAN_AIR_FACTOR;
  status.data = R0;

  Serial.println("Calibrazione completata");
  Serial.print("Sensore ");
  Serial.print(sensor);
  Serial.print(": ");
  Serial.println(R0);
  return status;
}

// Funzioni di lettura 
// METANO
float readMethane(void) {
    // Leggo il valore di resistenza relativo 
    int adc = analogRead(MQ4);

    // Calcolo la tensione
    float voltage = adc * (3.3 / 1023);
    // Evito il problema della divisione per 0 se non c'è tensione
    if (voltage == 0)
    return -1;

    // Calcolo RS 
    float Rs = RL * ((3.3 - voltage)/voltage);
    
    // Calcolo il rapporto tra Rs e R0 e lo restituisco
    if (Rs/Ro4 <= 0)
      return -1;
      
    float gas = 1-(Rs/Ro4);
    if (gas < 0) return 0;
    else return gas;
}
// CO
float readCO(void) {
    // Leggo il valore di resistenza relativo 
    int adc = analogRead(MQ7);

    // Calcolo la tensione
    float voltage = adc * (V / 1023);
    // Evito il problema della divisione per 0 se non c'è tensione
    if (voltage == 0)
    return -1;

    // Calcolo RS 
    float Rs = RL * ((V - voltage)/voltage) / V;

    // Calcolo il rapporto tra Rs e R0 e lo restituisco
    return (1-(Rs/Ro7))*100;
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
float serveData_Meth() {
  return current_Methane;      // Restituisci solo il dato
}
// CO
float serveData_CO() {
  return current_CO;      // Restituisci solo il dato
}
// Umidità
float serveData_RH() {
  return current_RH;      // Restituisci solo il dato
}
// Qualità dell'aria in percentuale
int calculateQuality() {
  int air_quality;
  float co;
  float meth;
  float rh;

  // Calcolo la qualità dell'aria moltiplicando i valori di metano e CO, restituisco un intero
  if (current_CO == -1) co = 0;
  else co = 1 - current_CO;
  if (current_Methane == -1) meth = 0;
  else meth = 1 - current_Methane;
  if (current_RH == -1) rh = 0;
  else rh = 1 - current_RH;

  float score;

  int valid = 3;
  if (co == 0) valid--;
  if (rh == 0) valid--;
  if (meth == 0) valid--;

  if (valid == 0) score = -1;
  else score = - (co + meth + rh) / valid;

  return score;
}
// Avvio manuale ventola
// L'avvio avverrà a velocità massima, in quanto l'idea è che se serve un'attivazione manuale si è in stato di emergenza
String manualStart() {
  manual = true;

  return ("OK");
}
// Stop manuale ventola
String manualStop() {
  // Attivo controllo automatico
  manual = false;

  return ("OK");
}

// Sostituzione dato placeholder nell'HTML
String processor(const String& var){
  // Serial.println(var);
  int data;

  if (var == "METHANE"){
    data = serveData_Meth();
  }
  else if (var == "CO"){
    data = serveData_CO();
  }
  else if (var == "HUMIDITY"){
    data = serveData_RH();
  }
  else if (var == "QUALITY") {
    data = calculateQuality();
  }

  return String(data);
}

// Definizione funzioni task
// Lettura da sensori
void readSensors (void *parameters) {
  // Legge sempre dai sensori
  for (;;) {

    // Controllo su seriale
    // Serial.println("Reading...");

    // Leggo i parametri che mi interessano
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    current_CO = readCO();
    current_Methane = readMethane();
    current_RH = readRH();

    // I dati sono disponibili per la scrittura
    sensorData_available = true;
    xSemaphoreGive(dataMutex);
    
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

      // Istantanea dei valori
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      float co = current_CO;
      float methane = current_Methane;
      float rh = current_RH;
      xSemaphoreGive(dataMutex);

      // Tolgo i dati vecchi ma resta l'identificativo
      sensor.clearFields();

      // Metto il valore misurato in una colonna del dato
      // Riporta livelli di Metano, CO e Umidità in un certo istante di tempo
      sensor.addField("methane", methane);
      sensor.addField("co", co);
      sensor.addField("humidity", rh);

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
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
// Attivazione ventola
void startWind (void *parameters) {
  for (;;) {
    // Solo se non avvio il controllo manuale
    if (!manual) {
      // Vedo la qualità dell'aria e attivo la ventola a seconda di quanto la qualità è bassa
      int air_quality = calculateQuality();
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
    }
    else {
      // Avvio manualmente
      digitalWrite(motor1Pin1, HIGH);
      digitalWrite(motor1Pin2, LOW);
      ledcWrite(pwmChannel, 200);
    }

    // La ventola gira per 10 secondi prima di controllare di nuovo la qualità dell'aria e modificare
    // eventualmente la velocità della ventola
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  // Inizilizzazioni
  Serial.begin(115200);         // Seriale per stampe e controlli in-line
  pinMode(MQ7, INPUT);          // Sensore CO
  pinMode(MQ4, INPUT);          // Sensore Met

  // Semaforo per evitare race condition
  dataMutex = xSemaphoreCreateMutex();

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
  WiFi.setSleep(false);
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

  // Route per il file di stile
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });

  // Root page 
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", String(), false, processor);
  });

  // Richieste dei singoli dati
  server.on("/co", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", (String)serveData_CO());
  });
  
  server.on("/humidity", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", (String)serveData_RH());
  });
  
  server.on("/methane", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", (String)serveData_Meth());
  });

  server.on("/quality", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", (String)calculateQuality());
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

  // Controlli manuali
  server.on("/manual_start", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", manualStart());
  });

    server.on("/manual_stop", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", manualStop());
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
    3072,                       // Stack size
    NULL,                       // Parametri della funzione da eseguire
    2,                          // Priorità
    &read_handle,               // Task handle
    1                           // Core d'appoggio
  );
  

  
  // Task di scrittura su DB
  xTaskCreatePinnedToCore (
    writeSensorData,
    "writeSensorData",
    8192,
    NULL,
    1,
    &writeDB_handle,
    1
  );
  
  

  // Task di attivazione ventola
  xTaskCreatePinnedToCore (
    startWind,
    "startWind",
    3072,
    NULL,
    2,
    &fan_handle,
    1
  );
}

void loop() {
}
