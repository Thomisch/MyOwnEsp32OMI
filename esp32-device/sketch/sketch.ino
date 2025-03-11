#include <WiFi.h>
#include <WebSocketsClient.h>
#include "driver/i2s.h"

const char* ssid = "MIWIFI_E444";
const char* password = "246J9F4K";

// Replace with your backend server's IP, port, and WebSocket path
const char* websocket_server_host = "192.168.1.137";
const uint16_t websocket_server_port = 5000;
const char* websocket_path = "/ws";  // The path on your backend to accept WebSocket connections

WebSocketsClient webSocket;

// I2S microphone pin definitions
#define I2S_MIC_WS   15  // Word Select (LRCLK)
#define I2S_MIC_SCK  14  // Bit Clock (BCLK)
#define I2S_MIC_SD   32  // Data input

#define SAMPLE_RATE 16000
#define BUFFER_SIZE 256  // Number of 32-bit samples per read

// Volume detection parameters
#define VOLUME_THRESHOLD 200      // Adjust this threshold based on your microphone sensitivity
#define MIN_SEND_TIME 500         // Minimum time to send audio after trigger (in ms)
#define SILENCE_TIME_THRESHOLD 1000 // Time of silence before stopping transmission (in ms)

bool isTransmitting = false;
unsigned long lastVolumeTime = 0;
unsigned long transmissionStartTime = 0;

// Initialize I2S for microphone input
void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // We capture 32-bit samples
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,   // Mono channel
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,  // Not used in RX mode
    .data_in_num = I2S_MIC_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// Function to calculate average volume level from audio samples
int calculateVolume(int16_t* samples, int numSamples) {
  long sum = 0;
  for (int i = 0; i < numSamples; i++) {
    sum += abs(samples[i]);
  }
  return sum / numSamples;  // Average absolute amplitude
}

// Callback for WebSocket events (connected/disconnected/errors)
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      Serial.println("WebSocket connected.");
      break;
    case WStype_DISCONNECTED:
      Serial.println("WebSocket disconnected.");
      break;
    case WStype_ERROR:
      Serial.printf("WebSocket error: %s\n", payload);
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 Audio Streaming via WebSocket with Volume Detection...");

  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected.");

  setupI2S();

  // Initialize WebSocket connection
  webSocket.begin(websocket_server_host, websocket_server_port, websocket_path);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  Serial.println("Volume threshold set to: " + String(VOLUME_THRESHOLD));
}

void loop() {
  webSocket.loop();  // Maintain the WebSocket connection

  int32_t i2sBuffer[BUFFER_SIZE];
  size_t bytesRead = 0;
  esp_err_t result = i2s_read(I2S_NUM_0, i2sBuffer, sizeof(i2sBuffer), &bytesRead, 1000);

  if (result == ESP_OK && bytesRead > 0) {
    int samplesRead = bytesRead / 4;  // each sample is 4 bytes in 32-bit mode

    // Convert 32-bit samples to 16-bit (by shifting right 16 bits)
    int16_t samples[samplesRead];
    for (int i = 0; i < samplesRead; i++) {
      samples[i] = i2sBuffer[i] >> 16;
    }

    // Calculate volume
    int volume = calculateVolume(samples, samplesRead);

    // Print volume level occasionally for debugging (every ~500ms)
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 500) {
      //Serial.println("Volume: " + String(volume));
      lastPrint = millis();
    }

    // Check if volume is above threshold
    if (volume > VOLUME_THRESHOLD) {
      lastVolumeTime = millis();

      if (!isTransmitting) {
        Serial.println("Speech detected! Starting transmission...");
        isTransmitting = true;
        transmissionStartTime = millis();
      }
    }

    // Determine if we should be sending audio
    bool shouldSend = false;

    if (isTransmitting) {
      // Keep sending if:
      // 1. We recently detected high volume, or
      // 2. We haven't reached the minimum transmission time yet
      if ((millis() - lastVolumeTime < SILENCE_TIME_THRESHOLD) ||
          (millis() - transmissionStartTime < MIN_SEND_TIME)) {
        shouldSend = true;
      } else {
        Serial.println("End of speech detected. Stopping transmission.");
        isTransmitting = false;
      }
    }

    // Send audio data if we should be transmitting
    if (shouldSend) {
      // Convert samples to 8-bit format for sending
      uint8_t payload[samplesRead * 2];
      for (int i = 0; i < samplesRead; i++) {
        payload[i * 2]     = samples[i] & 0xFF;
        payload[i * 2 + 1] = (samples[i] >> 8) & 0xFF;
      }

      // Send the binary audio data over the WebSocket
      webSocket.sendBIN(payload, samplesRead * 2);
    }
  }

  delay(10);  // Adjust delay as needed to balance throughput and CPU usage
}