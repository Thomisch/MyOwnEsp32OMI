# Étape 1 : Utilisation d'une image Node.js
FROM node:18

# Installer ffmpeg
RUN apt-get update && apt-get install -y ffmpeg

# Définir le répertoire de travail
WORKDIR /app

# Copier les fichiers du projet
COPY package.json package-lock.json ./
RUN npm install

# Copier le reste du projet
COPY . .

# Exposer les ports
EXPOSE 3000
EXPOSE 8080

# Démarrer l'application
CMD ["node", "index.js"]
