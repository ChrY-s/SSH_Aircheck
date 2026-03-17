#include <Arduino.h>
#include <Adafruit_AHTX0.h>

// Definisco i pin a cui i sensori sono collegati
#define MQ7 32                  // Rilevatore CO (Monossido di carbonio)
#define MQ4 33                  // Rilevatore Metano
#define VOUT 25                 // Tensione per il sensore MQ7

// Resistenza di riferimento
#define RL 1000                 // 1 kOhm

// Tensione di riferimento
#define V 3.3

// Sensore umidità
Adafruit_AHTX0 aht;

// Struttura dati per recupero Resistenze di riferimento e controllo sulla funzione
struct calibInit {
  int control_flag;
  float data;
};

// Valori di riferimento per i sensori
float Ro4, Ro7;

// Funzione di preriscaldamento per il sensore MQ7
void preheat(void){
  Serial.println("Riscaldamento sensore MQ7...");

  // Do tensione a 3.3 V per 60s
  dacWrite(VOUT, 255);
  delay(60000);

  // Do tensione di campionamento a 1.5 V per 90s
  dacWrite(VOUT, 115);
  delay(90000);
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

void setup() {
  // Inizilizzazioni
  Serial.begin(115200);         // Seriale per stampe e controlli in-line
  pinMode(MQ7, INPUT);          // Sensore CO
  pinMode(MQ4, INPUT);          // Sensore Met
  pinMode(VOUT, OUTPUT);        // Tensione MQ7

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
}

void loop() {
}
