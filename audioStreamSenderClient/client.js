const WebSocket = require('ws');
const recorder = require('node-record-lpcm16');

const ws = new WebSocket('ws://localhost:8080');

ws.on('open', () => {
    const recording = recorder.record({
        sampleRate: 16000,
        channels: 1,
        threshold: 0,
        silence: '10.0', // Désactive la détection de silence
        recorder: 'sox', // Utilise Sox pour plus de stabilité
        endOnSilence: false
      });
    
    recording.stream().on('data', (chunk) => {
        ws.send(chunk);
    });
});