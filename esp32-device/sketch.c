
#include <SD_MMC.h>
#include <sd_defines.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"

const char* ssid = "Livebox6-1AA0";
const char* password = "KKMR4TkycAd9";
const char* serverURL = "http://192.168.1.15:3000/upload-audio";  // URL du back-end
const int ledPin = 2;                                            // Pin 13 pour la LED
const int sdCsPin = 13;  // Broche CS du slot SD intégré
WiFiClient client;
HTTPClient http;
//SPIClass spiSD(HSPI);
// #if CONFIG_SPIRAM_SUPPORT
//   heap_caps_malloc_extmem_enable(512); // Utilise PSRAM pour les allocations >512B
// #endif

void setup() {
  // Ajoutez en début de setup()
  gpio_set_drive_capability(GPIO_NUM_26, GPIO_DRIVE_CAP_3); // BCK
  //gpio_set_pull_mode(GPIO_NUM_33, GPIO_PULLUP_ONLY); // DATA
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  WiFi.mode(WIFI_OFF);
  btStop();
  // Initialisation carte SD avec vérification
  //gpio_set_pull_mode(GPIO_NUM_15, GPIO_PULLUP_ONLY); // DATA0
  //gpio_set_pull_mode(GPIO_NUM_2, GPIO_PULLUP_ONLY);  // CMD
  pinMode(13, INPUT_PULLUP); // CS
  int sd_retry = 0;
  if (!SD_MMC.begin("/sdcard", true)) { // Mode 1-bit (meilleure compatibilité)
    Serial.println("Erreur initialisation SD");
    ESP.restart();
  }
  pinMode(25, OUTPUT);
  digitalWrite(25, LOW); // L/R à la masse pour canal gauche
  // Vérification type de carte
  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE) {
    Serial.println("Aucune carte détectée");
    ESP.restart();
  }
  connectToWiFi();
  delay(500);  // Délai de stabilisation
  Serial.println("SD OK");
  Serial.printf("Taille carte : %llu MB\n", SD_MMC.cardSize()/(1024*1024));
  setupI2S();
}

void loop() {
  // MODIFIER LA GESTION DES ERREURS
  String audioFilePath = captureAudio();
  if(audioFilePath.length() > 0) {
    if(WiFi.status() == WL_CONNECTED) {
      sendAudioToServer(audioFilePath);
    } else {
      Serial.println("Erreur WiFi : Pas de connexion");
      SD_MMC.remove(audioFilePath); // Nettoyage
    }
  }
  delay(10000);
}

void connectToWiFi() {
  Serial.print("Connexion à Wi-Fi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("Connecté !");
  delay(2000);  // Ajoute un délai pour observer la sortie
}

void setupI2S() {
// Configurez-la comme entrée si nécessaire
  pinMode(25, INPUT); 
  // MODIFIER LA CONFIGURATION I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM), // Ajouter PDM
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
    .dma_buf_count = 4,  // Réduire pour économiser mémoire
    .dma_buf_len = 512,  // Augmenter la taille
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,  // Pas utilisé
    .bck_io_num = 26,    // SCK
    .ws_io_num = 32,      // WS
    .data_out_num = I2S_PIN_NO_CHANGE, // Pas de sortie
    .data_in_num = 33     // SD
  };
  gpio_set_pull_mode(GPIO_NUM_33, GPIO_PULLUP_ONLY); // Pull-up sur la ligne de données
  
  // Installer le driver
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if(err != ESP_OK) {
    Serial.printf("Erreur installation I2S: 0x%x\n", err);
    ESP.restart();
  }

  // Configuration des broches
  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if(err != ESP_OK) {
    Serial.printf("Erreur configuration pins: 0x%x\n", err);
    ESP.restart();
  }
  i2s_set_pdm_rx_down_sample(I2S_NUM_0, I2S_PDM_DSR_8S); // Seulement si mode PDM activé
  // Configuration supplémentaire
  gpio_pullup_en(GPIO_NUM_33);  // Pull-up sur SD
  gpio_set_drive_capability(GPIO_NUM_26, GPIO_DRIVE_CAP_3); // Drive fort pour SCK
  i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
}

void writeWavHeader(File file) {
  if (!file) return;

  // Paramètres audio
  const uint32_t sampleRate = 16000;
  const uint16_t numChannels = 1;
  const uint16_t bitsPerSample = 16;

  // En-tête RIFF
  file.write((const uint8_t*)"RIFF", 4);
  const uint32_t chunkSize = 0; // À remplir plus tard
  file.write((const uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);

  // Sous-chunk fmt
  file.write((const uint8_t*)"fmt ", 4);
  const uint32_t subchunk1Size = 16;
  file.write((const uint8_t*)&subchunk1Size, 4);
  const uint16_t audioFormat = 1; // PCM
  file.write((const uint8_t*)&audioFormat, 2);
  file.write((const uint8_t*)&numChannels, 2);
  file.write((const uint8_t*)&sampleRate, 4);
  const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  file.write((const uint8_t*)&byteRate, 4);
  const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  file.write((const uint8_t*)&blockAlign, 2);
  file.write((const uint8_t*)&bitsPerSample, 2);

  // Sous-chunk data
  file.write((const uint8_t*)"data", 4);
  const uint32_t subchunk2Size = 0; // À remplir plus tard
  file.write((const uint8_t*)&subchunk2Size, 4);
}

String captureAudio() {
  String filename = "/audio_" + String(millis()) + ".wav"; // Nom unique
  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.printf("Erreur création : %s\n", filename.c_str());
    return "";
  }

  writeWavHeader(file);
  file.seek(44); // Passer l'en-tête WAV

  int32_t buffer[256];  
  int16_t samples[256];  
  size_t bytesRead;
  unsigned long startTime = millis();  

  Serial.println("Début enregistrement...");

  while (millis() - startTime < 10000) { // Boucle pendant 10 sec
    esp_err_t err = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
      Serial.printf("Erreur lecture I2S: 0x%x\n", err);
      break;
    }

    size_t sampleCount = bytesRead / sizeof(int32_t);
    for (int i = 0; i < sampleCount; i++) {
      samples[i] = buffer[i] >> 16;  // Convertir 32-bit → 16-bit
    }

    file.write((uint8_t*)samples, sampleCount * sizeof(int16_t));
  }

  Serial.println("Fin enregistrement !");
  file.close();
  return filename;
}


void sendAudioToServer(String path) {
  Serial.printf("Envoi du fichier : %s\n", path.c_str());

  File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    Serial.println("Erreur ouverture fichier !");
    return;
  }

  char header[4];
  file.readBytes(header, 4);
  if (strncmp(header, "RIFF", 4) != 0) {
    Serial.println("Fichier WAV corrompu, envoi annulé.");
    file.close();
    return;
  }
  file.seek(0);

  Serial.printf("Taille du fichier : %d octets\n", file.size());

  http.begin(client, serverURL);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("Content-Length", String(file.size()));
  
  int httpCode = http.sendRequest("POST", &file, file.size());
  Serial.printf("Réponse serveur: %d\n", httpCode);

  file.close();
  http.end();

  if (httpCode == 200) {
    Serial.println("Envoi réussi !");
    SD_MMC.remove(path);
  } else {
    Serial.println("Échec de l'envoi !");
  }
}
