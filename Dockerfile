# Étape 1 : Utilisation d'une image Node.js
FROM node:18

# Définir le répertoire de travail
WORKDIR /app

# Copier les fichiers du projet
COPY package.json package-lock.json ./
RUN npm install

# Copier le reste du projet
COPY . .

# Exposer le port (ex: 3000)
EXPOSE 3000

# Démarrer l'application
CMD ["node", "index.js"]