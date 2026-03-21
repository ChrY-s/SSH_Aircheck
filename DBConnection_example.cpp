#include <WiFiMulti.h>
WiFiMulti wifiMulti;
#define DEVICE "ESP32"
  
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
  
// WiFi SSID
#define WIFI_SSID "Galaxy A34 5G 9655"
// WiFi password
#define WIFI_PASSWORD "ffg88w3sgnu3sn9"
  
// Informazioni per connettersi al DB
#define INFLUXDB_URL "https://us-east-1-1.aws.cloud2.influxdata.com"
#define INFLUXDB_TOKEN "hBoaIv0FPOXc8Ig1xFqf1DPFubWpS3wUoqH_4HsQlniWELqkwDrIXzUXMyU_NixqpxV0ieoWNJA2qr7HSiNOcg=="
#define INFLUXDB_ORG "4bae2f56f0b03886"
#define INFLUXDB_BUCKET "AircheckSSH"
  
// Time zone
#define TZ_INFO "CET"
  
// Client InfluxDB
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
  
// Dato da scrivere
Point sensor("wifi_status");
  
void setup() {
  Serial.begin(115200);
  
  // Setup wifi
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Connecting to wifi");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  
  // Accurate time is necessary for certificate validation and writing in batches
  // We use the NTP servers in your area as provided by: https://www.pool.ntp.org/zone/
  // Syncing progress and the time will be printed to Serial.
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");
  
  
  // Check server connection
  if (client.validateConnection()) {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(client.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
  }



  // -------- WRITING -------- 

  // Metadati necessari per l'identificazione del dato
  sensor.addTag("device", DEVICE);
  sensor.addTag("SSID", WiFi.SSID());
}

  void loop() {
    // Tolgo i dati vecchi ma resta l'identificativo
    sensor.clearFields();
  
    // Metto il valore misurato in una colonna del dato
    // Report RSSI of currently connected network
    sensor.addField("rssi", WiFi.RSSI());
  
    // Print what are we exactly writing
    Serial.print("Writing: ");
    Serial.println(sensor.toLineProtocol());
  
    // Check WiFi connection and reconnect if needed
    if (wifiMulti.run() != WL_CONNECTED) {
      Serial.println("Wifi connection lost");
    }
  
    // Write point
    if (!client.writePoint(sensor)) {
      Serial.print("InfluxDB write failed: ");
      Serial.println(client.getLastErrorMessage());
    }
  
    // Delay di scrittura
    Serial.println("Waiting 1 second");
    delay(1000);
}
  
