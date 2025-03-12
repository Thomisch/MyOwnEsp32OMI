const express = require('express');
const WebSocket = require('ws');
const ffmpeg = require('fluent-ffmpeg');
const { Readable } = require('stream');
const axios = require('axios');
const { OpenAI } = require('openai');
const fs = require('fs');
require("dotenv").config();
const os = require('os');
const path = require('path');

const HOST = '0.0.0.0';
const app = express();
const wss = new WebSocket.Server({ port: 8080, host: HOST });
const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });
const WEBHOOK_URL = process.env.WEBHOOK_URL;

// Configuration audio
const SAMPLE_RATE = 16000;
const CHUNK_DURATION = 5; // secondes
const TEMP_DIR = path.join(os.tmpdir(), 'temp-audio');
fs.mkdirSync(TEMP_DIR, { recursive: true });

// Structure pour gérer les clients
const clients = new Map();

// Fonction de nettoyage
function cleanOldFiles() {
    const MAX_AGE = 3600 * 1000;
    fs.readdir(TEMP_DIR, (err, files) => {
        if (err) return;
        files.forEach(file => {
            const filePath = path.join(TEMP_DIR, file);
            const stats = fs.statSync(filePath);
            if (Date.now() - stats.birthtimeMs > MAX_AGE) {
                fs.unlinkSync(filePath);
            }
        });
    });
}

wss.on('connection', (ws) => {
    const clientId = Symbol();
    console.log('Nouvelle connexion WebSocket');

    const clientState = {
        audioBuffer: Buffer.alloc(0),
        processing: false,
        transcriptions: [],
        tempFiles: []
    };
    clients.set(clientId, clientState);

    ws.on('message', (message) => {
        if (message instanceof Buffer) {
            const state = clients.get(clientId);
            state.audioBuffer = Buffer.concat([state.audioBuffer, message]);
        }
    });

    ws.on('close', async () => {
        console.log('Connexion fermée, finalisation...');
        const state = clients.get(clientId);
        
        try {
            // Traitement final du buffer restant
            if (state.audioBuffer.length > 0) {
                await processClientAudio(clientId);
            }
            
            // Envoi de la conversation complète
            if (state.transcriptions.length > 0) {
                const fullConversation = state.transcriptions.join('\n');
                await sendToWebhook(fullConversation);
                console.log('Conversation envoyée:', fullConversation);
            }
        } catch (err) {
            console.error('Erreur lors de la fermeture:', err);
        } finally {
            // Nettoyage
            state.tempFiles.forEach(file => fs.existsSync(file) && fs.unlinkSync(file));
            clients.delete(clientId);
        }
    });
});

async function processClientAudio(clientId) {
    const state = clients.get(clientId);
    if (!state || state.processing) return;

    state.processing = true;
    try {
        const minSize = 16000 * 2 * CHUNK_DURATION;
        while (state.audioBuffer.length >= minSize) {
            const filename = `audio-${Date.now()}-${clientId.toString()}.wav`;
            const tempFile = path.join(TEMP_DIR, filename);
            const currentChunk = state.audioBuffer.slice(0, minSize);
            state.audioBuffer = state.audioBuffer.slice(minSize);

            await new Promise((resolve, reject) => {
                ffmpeg()
                    .input(Readable.from(currentChunk))
                    .inputOptions(['-f s16le', '-ar 16000', '-ac 1'])
                    .output(tempFile)
                    .outputOptions(['-acodec pcm_s16le', '-ar 16000', '-ac 1'])
                    .on('end', resolve)
                    .on('error', reject)
                    .run();
            });

            const transcription = await openai.audio.transcriptions.create({
                file: fs.createReadStream(tempFile),
                model: "whisper-1",
                response_format: "text"
            });

            state.transcriptions.push(transcription);
            state.tempFiles.push(tempFile);
        }
    } catch (error) {
        console.error('Erreur de traitement:', error);
    } finally {
        state.processing = false;
    }
}

// Traitement périodique pour chaque client
setInterval(() => {
    clients.forEach((_, clientId) => processClientAudio(clientId));
}, CHUNK_DURATION * 1000);

// Webhook et serveur Express (inchangés)
async function sendToWebhook(transcription) {
  let data = JSON.stringify({
    "message": transcription
  });
  
  let config = {
    method: 'post',
    maxBodyLength: Infinity,
    url: 'https://n8n.stealz.moe/webhook/a79df37a-fe72-40a1-b734-b5e80997a2e2',
    headers: { 
      'content-type': 'application/json'
    },
    data : data
  };
  axios.request(config)
  .then((response) => {
    console.log(JSON.stringify(response.data));
  })
  .catch((error) => {
    console.log(error);
  });
}

setInterval(cleanOldFiles, 3600 * 1000);
app.listen(3000, HOST, () => console.log('Serveur HTTP sur port 3000'));