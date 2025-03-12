# ESP32 Wake Word Detection System with Audio Streaming

A voice assistant system that activates upon hearing the wake word "Hey Jarvis" and streams audio to a backend server for further processing.

## Features

- **Voice Activity Detection (VAD)**: Automatically detects when someone is speaking
- **Wake Word Detection**: Listens for "Hey Jarvis" activation phrase
- **Audio Streaming**: Sends microphone data to a server via WebSocket when activated
- **Energy Efficient**: Only processes audio when speech is detected
- **Configurable Parameters**: Easily adjust sensitivity and timing settings

## Hardware Requirements

- ESP32 Wrover development board
- I2S microphone (INMP441 or similar)
- Power source (USB or battery)
- WiFi network connection

## Wiring

Connect the I2S microphone to the ESP32 with the following pins:

- **Word Select (WS/LRCLK)**: GPIO 15
- **Bit Clock (SCK/BCLK)**: GPIO 14
- **Data (SD/DOUT)**: GPIO 32
- **VCC**: 3.3V
- **GND**: Ground

## Software Dependencies

- [Arduino IDE](https://www.arduino.cc/en/software) (or PlatformIO)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [WebSocketsClient library](https://github.com/Links2004/arduinoWebSockets)
- Edge Impulse wake word model (included in `wake_word_inferencing.h`)

## Installation

1. Install the Arduino IDE and ESP32 board support
2. Install the WebSocketsClient library via the Arduino Library Manager
3. Clone or download this repository
4. Open the sketch in Arduino IDE
5. Install the wake word model by going to "sketch" -> "include library" -> "add .zip library" -> select "ei-wake-word-arduino-1.0.13.zip" under esp32-device/sketch/data
6. Configure your WiFi and WebSocket settings (see Configuration section)
7. Connect your ESP32 via USB
8. Select the correct board (ESP32 Wrover Module) and port
9. Upload the sketch

## Configuration

Edit these parameters in the sketch to match your environment:

### WiFi and WebSocket Settings

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* websocket_server_host = "YOUR_SERVER_IP";
const uint16_t websocket_server_port = 5000;
const char* websocket_path = "/ws";
```

### Detection Parameters

```cpp
#define HEY_JARVIS_THRESHOLD 0.1    // Confidence threshold for wake word
#define STREAMING_DURATION 60000    // Duration to stream audio (in ms)
```

### Voice Activity Detection Parameters

```cpp
#define VAD_VOLUME_THRESHOLD 60     // Audio volume to trigger recording
#define VAD_SILENCE_THRESHOLD 15    // Volume to consider as silence
#define VAD_MIN_DURATION 100        // Minimum recording duration (ms)
#define VAD_MAX_DURATION 1500       // Maximum recording duration (ms)
#define VAD_SILENCE_DURATION 500    // Silence duration to end recording (ms)
```

## Operation

1. **Initial Setup**: The system connects to WiFi and initializes the I2S microphone
2. **LISTENING Mode**: Continuously monitors audio levels to detect speech
3. **RECORDING Mode**: When speech is detected, records audio until silence
4. **INFERENCING Mode**: Analyzes recording to detect the wake word
5. **STREAMING Mode**: If wake word detected, streams audio for 60 seconds

The system provides status updates via Serial Monitor (115200 baud).

## Server Setup

You'll need a WebSocket server that can receive binary audio data. The server should:

1. Accept WebSocket connections at the configured endpoint
2. Process 16-bit PCM audio at 16kHz sample rate
3. Implement your desired voice assistant functionality

## Customization

- **Microphone Gain**: Adjust the scaling factor in `recordAudioSegment()` (currently 8x)
- **I2S Configuration**: Modify sample rate and buffer settings in the i2s_config struct

## Troubleshooting

- **No Voice Detection**: Adjust `VAD_VOLUME_THRESHOLD` to match your microphone sensitivity
- **False Wake Word Triggers**: Increase `HEY_JARVIS_THRESHOLD` for higher confidence requirement
- **WiFi Connection Issues**: Check credentials and ESP32 WiFi signal strength
- **WebSocket Errors**: Verify server is running and accessible on your network

## License

This project is under no License.
