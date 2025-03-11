const express = require("express");
const multer = require("multer");
const fs = require("fs");
const OpenAI = require("openai");
require("dotenv").config();

const app = express();
const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

// Configuration de Multer pour accepter les fichiers audio
const storage = multer.memoryStorage();
const upload = multer({ storage: storage });

async function transcribeAudio(audioBuffer) {
  try {
    const filePath = "temp_audio.wav";
    fs.writeFileSync(filePath, audioBuffer); // Sauvegarde temporaire du fichier

    const response = await openai.audio.transcriptions.create({
      file: fs.createReadStream(filePath),
      model: "whisper-1",
      language: "en",
    });

    console.log("✅ Texte transcrit:", response.text);
    fs.unlinkSync(filePath); // Supprime le fichier après transcription
  } catch (error) {
    console.error("❌ Erreur lors de la transcription:", error);
  }
}

// Point d'entrée pour recevoir le fichier audio
app.post("/upload-audio", upload.single("audio"), async (req, res) => {
  if (!req.file) {
    console.error("❌ Aucun fichier reçu !");
    return res.status(400).json({ error: "Aucun fichier audio reçu" });
  }

  // Vérification du type MIME
  if (!req.file.mimetype.includes('wav')) {
    return res.status(400).json({ error: "Format audio invalide (WAV requis)" });
  }

  try {
    const filePath = "temp_audio.wav";
    fs.writeFileSync(filePath, req.file.buffer); // Sauvegarde temporaire du fichier

    const response = await openai.audio.transcriptions.create({
      file: fs.createReadStream(filePath), // Utilisez le fichier sauvegardé
      model: "whisper-1",
      language: "en",
    });

    fs.unlinkSync(filePath); // Supprime le fichier après transcription
    res.status(200).json({ transcription: response.text });
  } catch (error) {
    console.error("Erreur OpenAI:", error);
    res.status(500).json({ error: "Erreur de transcription" });
  }
});

// Démarre le serveur en écoutant sur toutes les interfaces réseau (0.0.0.0)
app.listen(3000, '0.0.0.0', () => {
console.log(`Serveur écoutant sur http://0.0.0.0:3000`);
});