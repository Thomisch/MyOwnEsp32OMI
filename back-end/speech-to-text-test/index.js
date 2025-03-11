const OpenAI = require("openai");
const fs = require("fs");
const mic = require("mic");
require("dotenv").config();

const openai = new OpenAI({ apiKey: process.env.OPENAI_API_KEY });

async function transcribeAudio(audioBuffer) {
  try {
    if (!audioBuffer || audioBuffer.length === 0) {
      console.error("❌ Aucune donnée audio enregistrée !");
      return;
    }

    const filePath = "temp_audio.wav";
    fs.writeFileSync(filePath, audioBuffer); // Sauvegarde temporaire

    const response = await openai.audio.transcriptions.create({
      file: fs.createReadStream(filePath),
      model: "whisper-1",
      language: "fr",
    });

    console.log("✅ Texte transcrit:", response.text);
    fs.unlinkSync(filePath); // Supprime le fichier après transcription
  } catch (error) {
    console.error("❌ Erreur lors de la transcription:", error);
  }
}

async function recordAndTranscribe() {
  console.log("🎤 Enregistrement en cours... Parlez et appuyez sur Entrée pour arrêter !");

  const micInstance = mic({
    rate: "16000",
    channels: "1",
    fileType: "wav",
  });

  const micInputStream = micInstance.getAudioStream();
  let audioChunks = [];

  micInputStream.on("data", (data) => {
    audioChunks.push(data);
  });

  micInputStream.on("error", (err) => {
    console.error("❌ Erreur micro:", err);
  });

  micInputStream.on("end", async () => {
    console.log("🔴 Arrêt de l'enregistrement, traitement en cours...");
    
    const audioBuffer = Buffer.concat(audioChunks);
    
    if (audioBuffer.length === 0) {
      console.error("❌ Aucun son détecté, vérifiez votre micro !");
      return;
    }

    console.log(`🔍 Taille du fichier audio capturé : ${audioBuffer.length} bytes`);
    await transcribeAudio(audioBuffer);
  });

  micInstance.start();

  // ✅ Attendre que l'utilisateur appuie sur Entrée pour arrêter
  process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.on("data", () => {
    console.log("🛑 Arrêt manuel détecté !");
    micInstance.stop();
    process.stdin.setRawMode(false);
    process.stdin.pause();
  });
}

recordAndTranscribe();
