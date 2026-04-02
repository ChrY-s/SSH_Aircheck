#include <WiFi.h>
#include "FS.h"
#include "SPIFFS.h"

// Credenziali WiFi
const char* ssid = "Galaxy A34 5G 9655";
const char* password = "ffg88w3sgnu3sn9";

WiFiServer server(80);

// Funzione pagina HTML di risposta
// Funzione per inviare la pagina HTML principale
void serveHtmlPage(WiFiClient client) {
  File file = SPIFFS.open("/test.txt", "r");


  Serial.println(file.size());


  // Controllo se esiste il file
  if (file) {
    // Header HTTP
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html");
    client.println();

    while(file.available()) {
      client.print(file.read());
      Serial.println("Printing...");
    }
    
    Serial.println("\nDone");

    file.close();  // Chiudi il file HTML
  }
  else {
    // Header HTTP
    client.println("HTTP/1.1 404 Not Fount");
    client.println();
    client.println("File non trovato");
  }
}
// Funzione che dà solo i dati di risposta
// Funzione per restituire i dati (esempio temperatura)
void serveData(WiFiClient client) {
  String data = String(random(20, 30));  // Simulazione di un dato (es. temperatura)
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/plain");
  client.println();
  client.println(data);  // Restituisci solo il dato (temperatura)
}



void setup() {
  Serial.begin(115200);

  // Connessione al WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connesso al WiFi!");

  Serial.println(WiFi.localIP());

  // Monta SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Errore di montaggio SPIFFS");
    return;
  }

  // Avvia server
  server.begin();
}

void loop() {
  WiFiClient client = server.available();

  if (client) {
    String request = client.readStringUntil('\n');
    Serial.println(request);

    // Differenzia tipi di richieste
    // Se la richiesta è per il file HTML principale
    if (request.startsWith("GET / ")) {
      serveHtmlPage(client);
    }
    // Se la richiesta è per i dati (es. "/data")
    else if (request.startsWith("GET /data")) {
      serveData(client);  // Restituisce i dati in formato JSON o plain text
    }

    delay(1);
    client.stop();
  }
}

