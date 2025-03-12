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

const app = express();
const wss = new WebSocket.Server({ port: 8080 });
const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });
const WEBHOOK_URL = process.env.WEBHOOK_URL; // Ajouter l'URL du webhook dans un .env

// Configuration audio
const SAMPLE_RATE = 16000;
const CHUNK_DURATION = 5; // secondes
let audioBuffer = Buffer.alloc(0);
let processing = false;


// Créer un répertoire temporaire dédié
const TEMP_DIR = path.join(os.tmpdir(), 'temp-audio');
fs.mkdirSync(TEMP_DIR, { recursive: true });

// Ajouter cette fonction de nettoyage automatique
function cleanOldFiles() {
    const MAX_AGE = 3600 * 1000; // 1 heure en millisecondes
    fs.readdir(TEMP_DIR, (err, files) => {
        if (err) return;
        
        files.forEach(file => {
            const filePath = path.join(TEMP_DIR, file);
            const stats = fs.statSync(filePath);
            
            if (Date.now() - stats.birthtimeMs > MAX_AGE) {
                fs.unlinkSync(filePath);
                console.log(`Fichier nettoyé: ${file}`);
            }
        });
    });
}


wss.on('connection', (ws) => {
    console.log('Client WebSocket connecté');

    ws.on('message', (message) => {
        if (message instanceof Buffer) {
            audioBuffer = Buffer.concat([audioBuffer, message]);
        }
    });

    ws.on('close', () => {
        console.log('Client WebSocket déconnecté');
        audioBuffer = Buffer.alloc(0);
    });
});

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
// Traitement périodique du buffer audio
// Modifier la partie du traitement périodique comme suit :
setInterval(async () => {
  if (audioBuffer.length === 0 || processing) return;
  
  processing = true;
  
  try {
    // Calculer la taille minimale requise pour 5 secondes (16kHz * 2 bytes * 5s)
    const minSize = 16000 * 2 * CHUNK_DURATION;
    if (audioBuffer.length < minSize) return;

    const filename = `audio-${Date.now()}.wav`;
    const tempFile = path.join(TEMP_DIR, filename);
    
    // Extraire le chunk et conserver le reste
    const currentChunk = audioBuffer.slice(0, minSize);
    audioBuffer = audioBuffer.slice(minSize); // Conserve les données non traitées

    // Conversion FFmpeg améliorée
    await new Promise((resolve, reject) => {
      ffmpeg()
        .input(Readable.from(currentChunk))
        .inputOptions([
          '-f s16le', // Format d'entrée brut
          '-ar 16000', // Fréquence d'échantillonnage
          '-ac 1'     // Mono
        ])
        .output(tempFile)
        .outputOptions([
          '-acodec pcm_s16le', // Codec PCM
          '-ar 16000',         // Même fréquence
          '-ac 1'              // Mono
        ])
        .on('end', resolve)
        .on('error', reject)
        .run();
    });

    // Vérifier la taille du fichier généré
    const stats = fs.statSync(tempFile);
    if (stats.size < 44) { // Taille minimale d'un WAV vide + header
      throw new Error('Fichier WAV invalide');
    }

    const transcription = await openai.audio.transcriptions.create({
      file: fs.createReadStream(tempFile),
      model: "whisper-1",
      response_format: "text"
    });
    sendToWebhook(transcription);
    console.log('SendToWebhook:', transcription);

  } catch (error) {
    console.error('Erreur:', error.message);
    // Supprimer le fichier problématique
    if(fs.existsSync(tempFile)) fs.unlinkSync(tempFile);
  } finally {
    processing = false;
  }
}, CHUNK_DURATION * 1000);

// Nettoyage toutes les heures
setInterval(cleanOldFiles, 3600 * 1000);

app.listen(3000, () => {
    console.log('Serveur HTTP en écoute sur le port 3000');
});