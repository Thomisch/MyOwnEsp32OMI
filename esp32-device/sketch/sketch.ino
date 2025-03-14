/* Wake Word Detection ("hey_jarvis") with Audio Streaming
 * Features:
 * 1. Voice Activity Detection (VAD) to trigger recording
 * 2. Wake word inference only on active speech segments
 * 3. Audio streaming to backend when wake word detected
 * 4. Send inference audio to backend for troubleshooting
 * 5. LED indicator for wake word detection and streaming mode
 * 6. On-demand WebSocket connections only during streaming
 */

// If your target is limited in memory remove this macro to save 10K RAM
#define EIDSP_QUANTIZE_FILTERBANK 0

/* Includes ---------------------------------------------------------------- */
#include <wake_word_inferencing.h>
#include "driver/i2s.h"
#include <WiFi.h>
#include <WebSocketsClient.h>

// WiFi and WebSocket Configuration
const char* ssid = "MIWIFI_E444";
const char* password = "246J9F4K";
const char* websocket_server_host = "192.168.1.141";
const uint16_t websocket_server_port = 8080;
const char* websocket_path = "/";

WebSocketsClient webSocket;
bool websocketConnected = false;

// Detection parameters - removing fixed threshold, will use highest confidence approach
#define STREAMING_DURATION 60000    // Duration to stream audio after detection (1 min)

#define CONFIDENCE_THRESHOLD 0.9    // Minimum confidence required for wake word detection

// Voice Activity Detection parameters
#define VAD_VOLUME_THRESHOLD 60    // Audio volume threshold to consider activity
#define VAD_SILENCE_THRESHOLD 15   // Volume to consider as silence
#define VAD_MIN_DURATION 100        // Minimum duration of sound to trigger recording (ms)
#define VAD_MAX_DURATION 1500       // Maximum recording duration (ms)
#define VAD_SILENCE_DURATION 500    // Duration of silence to end recording (ms)

// Streaming silence detection parameters
#define STREAMING_CALIBRATION_MS 1500    // Time to calibrate background noise (ms)
#define STREAMING_SILENCE_RATIO 0.3      // Ratio of the average to consider as silence (30%)
#define STREAMING_MIN_SILENCE_THRESHOLD 15  // Minimum silence threshold regardless of environment
#define STREAMING_SILENCE_DURATION 5000  // Duration of silence to end streaming (5 seconds)

// I2S pins
#define I2S_MIC_WS   15  // Word Select (LRCLK)
#define I2S_MIC_SCK  14  // Bit Clock (BCLK)
#define I2S_MIC_SD   32  // Data input

// I2S driver handling
i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = -1
};

i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = -1,
    .data_in_num = I2S_MIC_SD
};

// Operation modes and state
enum OperationMode {
  LISTEN_MODE,       // Listening for voice activity
  RECORDING_MODE,    // Recording active speech segment
  INFERENCING_MODE,  // Running wake word detection
  STREAMING_MODE     // Streaming audio to server
};

OperationMode currentMode = LISTEN_MODE;
unsigned long streamingStartTime = 0;
unsigned long recordingStartTime = 0;
unsigned long lastSoundTime = 0;

// Buffer for inference
typedef struct {
    int16_t *buffer;
    uint32_t buf_count;
    uint32_t n_samples;
} inference_t;

static inference_t inference;
static signed short sampleBuffer[512]; // Temporary buffer for I2S reads

// Debug heartbeat
unsigned long lastHeartbeat = 0;

// Audio message types (for WebSocket communication)
#define MSG_TYPE_INFERENCE_START "INFERENCE_AUDIO_START"
#define MSG_TYPE_INFERENCE_END "INFERENCE_AUDIO_END"
#define MSG_TYPE_STREAM_START "STREAM_AUDIO_START"
#define MSG_TYPE_STREAM_END "STREAM_AUDIO_END"

// LED Configuration
#define LED_PIN 2  // Using the built-in LED on most ESP32 boards
#define LED_BLINK_INTERVAL 300  // Blink interval in milliseconds for streaming mode

/**
 * Setup WiFi only (no WebSocket initialization)
 */
void setupWiFi() {
    // Initialize WiFi
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection failed! Continuing anyway...");
    }
}

/**
 * Initialize WebSocket connection (called before streaming)
 */
bool setupWebSocket() {
    Serial.println("Initializing WebSocket connection...");
    
    // Configure WebSocket
    webSocket.begin(websocket_server_host, websocket_server_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    
    // Wait for connection
    int timeout = 0;
    while (!webSocket.isConnected() && timeout < 10) {
        webSocket.loop();
        delay(300);
        timeout++;
    }
    
    websocketConnected = webSocket.isConnected();
    
    if (websocketConnected) {
        Serial.println("WebSocket connected successfully");
    } else {
        Serial.println("WebSocket connection failed");
    }
    
    return websocketConnected;
}

/**
 * Close WebSocket connection
 * Should only be called from streamAudioToServer when streaming is complete
 */
void closeWebSocket() {
    if (websocketConnected) {
        Serial.println("Closing WebSocket connection after streaming");
        webSocket.disconnect();
        websocketConnected = false;
    }
}

/**
 * Initialize I2S driver
 */
bool setupI2S() {
    Serial.println("Initializing I2S...");

    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        Serial.printf("Failed to install I2S driver: %d\n", ret);
        return false;
    }

    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) {
        Serial.printf("Failed to set I2S pins: %d\n", ret);
        return false;
    }

    ret = i2s_zero_dma_buffer(I2S_NUM_0);
    if (ret != ESP_OK) {
        Serial.printf("Failed to zero I2S DMA buffer: %d\n", ret);
        return false;
    }

    Serial.println("I2S initialized successfully");
    return true;
}

/**
 * WebSocket event handler
 */
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.println("WebSocket connected");
            websocketConnected = true;
            break;
        case WStype_DISCONNECTED:
            Serial.println("WebSocket disconnected");
            websocketConnected = false;
            break;
        case WStype_ERROR:
            Serial.printf("WebSocket error: %s\n", payload);
            break;
        default:
            break;
    }
}

/**
 * Initialize inference buffer
 */
bool setupInference() {
    Serial.println("Allocating inference buffer...");

    inference.buffer = (int16_t *)malloc(EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t));
    if(inference.buffer == NULL) {
        Serial.println("Failed to allocate inference buffer");
        return false;
    }

    inference.buf_count = 0;
    inference.n_samples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;

    Serial.println("Inference buffer allocated");
    return true;
}

/**
 * Calculate average volume level of audio buffer
 */
int calculateVolume(int16_t* buffer, size_t length) {
    long sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += abs(buffer[i]);
    }
    return sum / length;
}

/**
 * Listen for voice activity
 * Returns true if voice activity is detected
 */
bool listenForVoiceActivity() {
    size_t bytes_read = 0;

    // Read from I2S
    esp_err_t ret = i2s_read(I2S_NUM_0, (char *)sampleBuffer, sizeof(sampleBuffer), &bytes_read, 100);
    if (ret != ESP_OK || bytes_read == 0) {
        return false;
    }

    // Calculate volume
    int samples_read = bytes_read / 2;  // 16-bit samples = 2 bytes
    int volume = calculateVolume(sampleBuffer, samples_read);

    // Print volume level occasionally
    static unsigned long last_volume_print = 0;
    if (millis() - last_volume_print > 1000) {
        last_volume_print = millis();
        Serial.printf("Current volume level: %d (threshold: %d)\n", volume, VAD_VOLUME_THRESHOLD);
    }

    // Check if volume exceeds threshold
    if (volume > VAD_VOLUME_THRESHOLD) {
        Serial.printf("Voice activity detected! Volume: %d\n", volume);
        return true;
    }

    return false;
}

/**
 * Record audio segment when voice activity is detected
 * Returns true when recording is complete
 */
bool recordAudioSegment() {
    size_t bytes_read = 0;
    static bool printedStart = false;

    // If just started recording
    if (!printedStart) {
        Serial.println("Started recording audio segment");
        printedStart = true;
    }

    // Read from I2S
    esp_err_t ret = i2s_read(I2S_NUM_0, (char *)sampleBuffer, sizeof(sampleBuffer), &bytes_read, 100);
    if (ret != ESP_OK || bytes_read == 0) {
        return false;
    }

    // Calculate volume
    int samples_read = bytes_read / 2;  // 16-bit samples = 2 bytes
    int volume = calculateVolume(sampleBuffer, samples_read);

    // Check if we still have space in the buffer
    if (inference.buf_count + samples_read <= inference.n_samples) {
        // Copy samples to inference buffer with scaling
        for (int i = 0; i < samples_read; i++) {
            inference.buffer[inference.buf_count++] = sampleBuffer[i] * 8; // Scale the data
        }
    }

    // Keep track of when we last heard sound above silence threshold
    if (volume > VAD_SILENCE_THRESHOLD) {
        lastSoundTime = millis();
    }

    // Check if recording should end
    bool recordingComplete = false;

    // 1. If max duration reached
    if (millis() - recordingStartTime >= VAD_MAX_DURATION) {
        Serial.println("Recording complete: max duration reached");
        recordingComplete = true;
    }
    // 2. If silence detected for long enough
    else if (millis() - lastSoundTime >= VAD_SILENCE_DURATION) {
        Serial.println("Recording complete: silence detected");
        recordingComplete = true;
    }
    // 3. If buffer is full
    else if (inference.buf_count >= inference.n_samples) {
        Serial.println("Recording complete: buffer full");
        recordingComplete = true;
    }

    // Reset state if recording is complete
    if (recordingComplete) {
        printedStart = false;
        Serial.printf("Recorded %d samples\n", inference.buf_count);
    }

    return recordingComplete;
}

/**
 * Store inference audio (for future backend sending if wake word detected)
 * Not actually sending over WebSocket anymore - just keeping for reference
 */
void storeInferenceAudio() {
    Serial.println("Storing inference audio for possible troubleshooting");
    
    // We're not sending the inference audio over WebSocket anymore
    // Just keeping the function for future enhancements if needed
    
    Serial.printf("Stored %d samples for potential analysis\n", inference.buf_count);
}

/**
 * Process inference and detect wake word
 * Returns true if wake word is detected
 */
bool runWakeWordInference() {
    Serial.println("Running wake word inference...");

    // Store inference audio but don't send it (removed WebSocket connection)
    storeInferenceAudio();

    // If buffer has too few samples, pad it or reject it
    if (inference.buf_count < inference.n_samples / 2) {
        Serial.println("Too few samples for inference, ignoring");
        return false;
    } else if (inference.buf_count < inference.n_samples) {
        // Pad with zeros if we don't have enough samples
        Serial.printf("Padding buffer from %d to %d samples\n",
                     inference.buf_count, inference.n_samples);
        memset(&inference.buffer[inference.buf_count], 0,
              (inference.n_samples - inference.buf_count) * sizeof(int16_t));
    }

    // Prepare signal for inferencing
    signal_t signal;
    signal.total_length = inference.n_samples;
    signal.get_data = [](size_t offset, size_t length, float *out_ptr) -> int {
        numpy::int16_to_float(&inference.buffer[offset], out_ptr, length);
        return 0;
    };

    // Run the classifier
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);

    if (res != EI_IMPULSE_OK) {
        Serial.printf("ERR: Failed to run classifier (%d)\n", res);
        return false;
    }

    // Instead of using a fixed threshold, find the label with highest confidence
    bool detected = false;
    float highest_confidence = 0.0;
    String highest_label = "";

    // Find highest confidence label
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        float confidence = result.classification[ix].value;
        String label = result.classification[ix].label;

        // Print all predictions
        Serial.print("    ");
        Serial.print(label);
        Serial.print(": ");
        Serial.println(confidence, 4);

        // Check if this is the highest confidence so far
        if (confidence > highest_confidence) {
            highest_confidence = confidence;
            highest_label = label;
        }
    }

    // Check if the highest confidence label is "hey_jarvis"
    if (highest_label.equalsIgnoreCase("hey_jarvis") && highest_confidence >= CONFIDENCE_THRESHOLD) {
        Serial.println("\n------------------------------------");
        Serial.println("HEY JARVIS DETECTED!");
        Serial.print("Highest confidence: ");
        Serial.println(highest_confidence, 4);
        Serial.println("------------------------------------\n");

        // Turn on LED to indicate wake word detection
        digitalWrite(LED_PIN, HIGH);

        detected = true;
    } else {
        Serial.println("\n------------------------------------");
        Serial.print("Highest confidence label: ");
        Serial.print(highest_label);
        Serial.print(" (");
        Serial.print(highest_confidence, 4);
        Serial.println(")");
        Serial.println("Not wake word, ignoring");
        Serial.println("------------------------------------\n");
    }

    return detected;
}

/**
 * Stream audio to WebSocket server
 * This is the ONLY function that should establish and close WebSocket connections
 */
void streamAudioToServer() {
    Serial.println("Starting audio streaming...");
    
    // This is the ONLY place we should connect to WebSocket
    // Connect to WebSocket for streaming
    if (!setupWebSocket()) {
        Serial.println("Failed to connect WebSocket for streaming, aborting");
        return;
    }

    // Send start marker
    webSocket.sendTXT(MSG_TYPE_STREAM_START);
    delay(50);

    // Set start time
    streamingStartTime = millis();
    int packets_sent = 0;
    unsigned long last_status = 0;
    unsigned long last_blink = 0; // For LED blinking
    
    // Silence detection variables
    unsigned long lastSoundAboveThreshold = millis();
    int currentVolume = 0;
    bool silenceEndedStreaming = false;
    
    // Dynamic threshold variables
    bool calibrationDone = false;
    unsigned long calibrationStartTime = millis();
    int volumeSum = 0;
    int volumeCount = 0;
    int maxVolume = 0;
    int avgVolume = 0;
    int dynamicSilenceThreshold = STREAMING_MIN_SILENCE_THRESHOLD;
    int talkingVolumes[20] = {0}; // Keep track of last 20 volumes during talking
    int talkingVolumesIndex = 0;
    int talkingVolumesCount = 0;
    int talkingAvgVolume = 0;

    // We'll use the same buffer size for simplicity
    int16_t streamBuffer[512];
    size_t bytes_read = 0;

    while (millis() - streamingStartTime < STREAMING_DURATION) {
        // Heartbeat messages
        if (millis() - last_status > 2000) {
            last_status = millis();
            Serial.printf("Streaming... packets: %d, remaining: %d sec, volume: %d, threshold: %d\n",
                         packets_sent, 
                         (STREAMING_DURATION - (millis() - streamingStartTime)) / 1000,
                         currentVolume,
                         dynamicSilenceThreshold);
        }

        // Blink LED while streaming
        if (millis() - last_blink > LED_BLINK_INTERVAL) {
            last_blink = millis();
            digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Toggle LED state
        }

        // Handle WebSocket connection
        webSocket.loop();

        // Read audio from I2S
        esp_err_t ret = i2s_read(I2S_NUM_0, (char*)streamBuffer, sizeof(streamBuffer), &bytes_read, 100);
        if (ret != ESP_OK || bytes_read == 0) {
            delay(10);
            continue;
        }

        int samples_read = bytes_read / 2;  // 16-bit samples = 2 bytes
        
        // Calculate volume of current buffer for silence detection
        currentVolume = calculateVolume(streamBuffer, samples_read);
        
        // During calibration phase, collect volume statistics
        if (!calibrationDone) {
            volumeSum += currentVolume;
            volumeCount++;
            
            if (currentVolume > maxVolume) {
                maxVolume = currentVolume;
            }
            
            // End calibration after specified time
            if (millis() - calibrationStartTime > STREAMING_CALIBRATION_MS) {
                calibrationDone = true;
                avgVolume = volumeSum / volumeCount;
                
                // Set initial dynamic threshold based on calibration
                // Use either a percentage of max volume or a minimum value
                dynamicSilenceThreshold = max(
                    STREAMING_MIN_SILENCE_THRESHOLD,
                    (int)(avgVolume * 1.5)  // Start with 150% of average background noise
                );
                
                Serial.printf("Calibration complete - Avg: %d, Max: %d, Initial threshold: %d\n", 
                             avgVolume, maxVolume, dynamicSilenceThreshold);
            }
        } else {
            // After calibration, continue to adapt the threshold based on talking volumes
            
            // If current volume is significantly above the threshold, consider it talking
            if (currentVolume > dynamicSilenceThreshold * 2) {
                // Add to talking volumes circular buffer
                talkingVolumes[talkingVolumesIndex] = currentVolume;
                talkingVolumesIndex = (talkingVolumesIndex + 1) % 20;
                if (talkingVolumesCount < 20) {
                    talkingVolumesCount++;
                }
                
                // Calculate average of talking volumes
                int talkingVolumeSum = 0;
                for (int i = 0; i < talkingVolumesCount; i++) {
                    talkingVolumeSum += talkingVolumes[i];
                }
                talkingAvgVolume = talkingVolumeSum / talkingVolumesCount;
                
                // Update dynamic threshold based on talking volume
                // Use a percentage of the average talking volume
                dynamicSilenceThreshold = max(
                    STREAMING_MIN_SILENCE_THRESHOLD,
                    (int)(talkingAvgVolume * STREAMING_SILENCE_RATIO)
                );
            }
        }
        
        // Check if volume is above the dynamic silence threshold
        if (currentVolume > dynamicSilenceThreshold) {
            lastSoundAboveThreshold = millis();
        }
        
        // Check if we've been silent for too long
        if (calibrationDone && millis() - lastSoundAboveThreshold > STREAMING_SILENCE_DURATION) {
            Serial.printf("Ending streaming due to %d ms of silence (volume: %d, threshold: %d)\n", 
                STREAMING_SILENCE_DURATION, currentVolume, dynamicSilenceThreshold);
            silenceEndedStreaming = true;
            break;
        }

        // Convert samples to 8-bit format for sending
        uint8_t payload[samples_read * 2];
        for (int i = 0; i < samples_read; i++) {
            payload[i * 2]     = streamBuffer[i] & 0xFF;
            payload[i * 2 + 1] = (streamBuffer[i] >> 8) & 0xFF;
        }

        // Send the binary audio data over WebSocket
        if (webSocket.isConnected()) {
            webSocket.sendBIN(payload, samples_read * 2);
            packets_sent++;
        } else {
            Serial.println("WebSocket disconnected during streaming!");
            // Try to reconnect
            if (setupWebSocket()) {
                Serial.println("WebSocket reconnected");
            } else {
                Serial.println("WebSocket reconnection failed, stopping streaming");
                break;
            }
        }

        // Small delay to prevent overwhelming the system
        delay(10);
    }

    // Send end marker
    webSocket.sendTXT(MSG_TYPE_STREAM_END);
    delay(50);

    if (silenceEndedStreaming) {
        Serial.println("Audio streaming ended early due to silence detection");
    } else {
        Serial.println("Audio streaming complete (timed out)");
    }
    Serial.printf("Total packets sent: %d\n", packets_sent);
    if (talkingVolumesCount > 0) {
        Serial.printf("Final values - Avg talking volume: %d, Silence threshold: %d\n", 
                     talkingAvgVolume, dynamicSilenceThreshold);
    }

    // Turn off LED when streaming is complete
    digitalWrite(LED_PIN, LOW);
    
    // Close WebSocket connection when done streaming
    // This is the ONLY place we should disconnect from WebSocket
    closeWebSocket();
}

void setup() {
    // Initialize serial
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n---------------------------------------------");
    Serial.println("Wake Word Detection with On-demand WebSocket");
    Serial.println("---------------------------------------------");

    // Setup LED pin
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);  // Start with LED off

    // Setup WiFi only (no WebSocket yet)
    setupWiFi();

    // Initialize I2S
    if (!setupI2S()) {
        Serial.println("Failed to initialize I2S! Halting.");
        while (1) { delay(1000); } // Halt execution
    }

    // Initialize inference buffer
    if (!setupInference()) {
        Serial.println("Failed to initialize inference! Halting.");
        while (1) { delay(1000); } // Halt execution
    }

    Serial.println("Setup complete - listening for voice activity...");
    Serial.println("Voice activity parameters:");
    Serial.printf("  Volume threshold: %d\n", VAD_VOLUME_THRESHOLD);
    Serial.printf("  Silence threshold: %d\n", VAD_SILENCE_THRESHOLD);
    Serial.printf("  Min duration: %d ms\n", VAD_MIN_DURATION);
    Serial.printf("  Max duration: %d ms\n", VAD_MAX_DURATION);
    Serial.printf("  Silence duration: %d ms\n", VAD_SILENCE_DURATION);
    Serial.printf("  Using highest confidence approach (threshold: %.2f)\n", CONFIDENCE_THRESHOLD);
    Serial.println("Streaming parameters:");
    Serial.printf("  Max streaming duration: %d ms\n", STREAMING_DURATION);
    Serial.printf("  Calibration time: %d ms\n", STREAMING_CALIBRATION_MS);
    Serial.printf("  Silence ratio: %.1f\n", STREAMING_SILENCE_RATIO);
    Serial.printf("  Min silence threshold: %d\n", STREAMING_MIN_SILENCE_THRESHOLD);
    Serial.printf("  Silence end duration: %d ms\n", STREAMING_SILENCE_DURATION);
    Serial.println("WebSocket will only connect when needed for streaming");
}

void loop() {
    // Print heartbeat occasionally
    if (millis() - lastHeartbeat > 10000) {
        lastHeartbeat = millis();
        Serial.printf("System running, mode: %s, Wifi: %s, WebSocket: %s\n",
                     currentMode == LISTEN_MODE ? "LISTENING" :
                     currentMode == RECORDING_MODE ? "RECORDING" :
                     currentMode == INFERENCING_MODE ? "INFERENCING" : "STREAMING",
                     WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED",
                     websocketConnected ? "CONNECTED" : "DISCONNECTED");
    }

    // Handle WebSocket loop only if connected
    if (websocketConnected) {
        webSocket.loop();
    }

    // State machine based on current mode
    switch (currentMode) {
        // Listening for voice activity
        case LISTEN_MODE:
            digitalWrite(LED_PIN, LOW); // Ensure LED is off
            if (listenForVoiceActivity()) {
                // Voice activity detected, start recording
                recordingStartTime = millis();
                lastSoundTime = millis();
                inference.buf_count = 0; // Reset buffer
                currentMode = RECORDING_MODE;
            }
            break;

        // Recording audio segment
        case RECORDING_MODE:
            // Check if we've been recording for minimum duration
            if (millis() - recordingStartTime < VAD_MIN_DURATION) {
                // Keep recording
                recordAudioSegment();
            } else if (recordAudioSegment()) {
                // Recording complete, run inference
                currentMode = INFERENCING_MODE;
            }
            break;

        // Running wake word inference
        case INFERENCING_MODE:
            if (runWakeWordInference()) {
                // Wake word detected, start streaming
                currentMode = STREAMING_MODE;
            } else {
                // Wake word not detected, go back to listening
                digitalWrite(LED_PIN, LOW); // Ensure LED is off
                currentMode = LISTEN_MODE;
            }
            break;

        // Streaming audio to server
        case STREAMING_MODE:
            streamAudioToServer();
            currentMode = LISTEN_MODE; // Return to listening when done
            break;
    }

    // Small delay to prevent overwhelming the CPU
    delay(1);
}