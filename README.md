# 🚦 Carrefour Intelligent à base d'Arduino

> **TP Systèmes Embarqués — INGC2 / MIAGE2**  
> Conception et réalisation d'un système intelligent de gestion de carrefour connecté  
> Plateforme : **Arduino Uno R3** | Interface : **Port Série (PC)** + **LCD I2C 16×2**

---

## 📋 Table des matières

- [Présentation](#présentation)
- [Matériel requis](#matériel-requis)
- [Câblage](#câblage)
- [Architecture logicielle](#architecture-logicielle)
- [Codage binaire à 7 bits](#codage-binaire-à-7-bits)
- [Moteur de décision multicritère](#moteur-de-décision-multicritère)
- [Commandes disponibles](#commandes-disponibles)
- [Statistiques intégrées](#statistiques-intégrées)
- [Bibliothèques requises](#bibliothèques-requises)
- [Installation et utilisation](#installation-et-utilisation)
- [Pistes d'évolution](#pistes-dévolution)

---

## Présentation

Ce projet implémente un **feu de signalisation intelligent** simulant 4 voies (NORD, EST, SUD, OUEST) d'un carrefour urbain. Contrairement à un feu classique à cycle fixe, le système :

- **Analyse en temps réel** l'état de chaque voie via un mot binaire à 7 bits
- **Applique une logique décisionnelle multicritère** (véhicule prioritaire, densité, occupation, désir de traverser, état de la destination)
- **Effectue une double validation** de la voie de destination avant d'accorder le feu vert
- **Gère les conflits** entre les 4 voies pour ne donner le vert qu'à une seule à la fois
- **Calcule des statistiques** consultables en temps réel via le port série

```
================================
NOUVEAU CYCLE — 4 VOIES ANALYSÉES
--- Voie NORD ---
Etat (7 bits) : 1011010
Vehicules detectes : 14
Direction : GAUCHE
Vehicule prioritaire : OUI
Decision : FEU VERT pour la voie NORD
Temps de passage : 17 secondes
================================
```

---

## Matériel requis

| Composant | Quantité | Rôle |
|---|---|---|
| Arduino Uno R3 | 1 | Unité centrale de traitement |
| LCD I2C 16×2 | 1 | Affichage des états et des informations |
| LED verte | 1 | Feu vert physique (passage autorisé) |
| LED rouge | 1 | Feu rouge physique (passage interdit) |
| Résistance 220 Ω | 2 | Protection des LEDs |
| Breadboard | 1 | Montage sans soudure |
| Câbles Dupont M/M et M/F | — | Connexions |

---

## Câblage

```
Arduino Uno        Composant
─────────────────────────────────────
Pin 11         →   LED verte (+ résistance 220Ω vers GND)
Pin 10         →   LED rouge (+ résistance 220Ω vers GND)
A4 (SDA)       →   LCD I2C — SDA
A5 (SCL)       →   LCD I2C — SCL
5V             →   LCD I2C — VCC
GND            →   LCD I2C — GND
```

> **Adresse I2C :** `0x27` par défaut. Si l'écran reste noir, essayer `0x3F` dans le code.

---

## Architecture logicielle

Le système repose sur un **automate à 3 états** exécuté sans `delay()` pour garantir la réactivité permanente aux commandes utilisateur.

```
         ┌─────────────────────────────┐
         │                             │
         ▼                             │
   ┌──────────┐    voie gagnante   ┌──────┐
   │ GENERER  │ ─────────────────► │ VERT │
   └──────────┘                    └──────┘
         │                             │
         │ personne ne peut passer     │ timer expiré
         ▼                             │
      ┌───────┐                        │
      │ ROUGE │ ◄──────────────────────┘
      └───────┘
         │
         │ timer expiré
         └──────────────► GENERER
```

### Modules principaux

```
carrefour_intelligent.ino
│
├── genererEtat()              — génération aléatoire des états 7 bits
├── calculerVoieDestination()  — décodage direction (bits 1,0) → index voie
├── evaluerVoie()              — moteur de décision multicritère
├── determinerGagnant()        — résolution des conflits entre les 4 voies
├── nouveauCycle()             — orchestration d'un cycle complet (silencieux)
├── afficherNormal()           — affichage LCD en mode surveillance
├── afficherDiagnostic()       — affichage LCD lors d'une commande utilisateur
├── imprimerReponseCommande()  — bloc #### dans le Moniteur Série
└── loop()                     — lecture série + machine à états + LCD
```

---

## Codage binaire à 7 bits

Chaque voie est représentée par un **mot binaire de 7 bits** (valeur entre 0 et 127) :

```
 Bit 6   Bit 5   Bit 4   Bit 3   Bit 2   Bit 1   Bit 0
  │       │       │       │       │       │       │
  │       │       │       │       │       └───────┘
  │       │       │       │       │       Direction (MSB, LSB)
  │       │       │       │       │
  │       │       │       │       └── Désir de traverser
  │       │       │       └────────── Voie actuellement occupée
  │       │       └────────────────── Voie de destination encombrée
  │       └────────────────────────── Forte densité de trafic
  └────────────────────────────────── Véhicule prioritaire (ambulance…)
```

### Décodage des directions (bits 1 et 0)

| Bit 1 (MSB) | Bit 0 (LSB) | Direction | Voie de destination (depuis NORD) |
|:-----------:|:-----------:|-----------|-----------------------------------|
| 0 | 0 | Tout droit | SUD |
| 0 | 1 | Droite | EST |
| 1 | 0 | Gauche | OUEST |
| 1 | 1 | _(Réservé — mouvement invalide)_ | — |

### Exemple de décodage

```
Mot binaire reçu : 1 0 1 1 0 1 0
                   │ │ │ │ │ │ │
                   │ │ │ │ │ └─┘── Direction : 10 → GAUCHE
                   │ │ │ │ └────── Désir de traverser : NON
                   │ │ │ └──────── Voie occupée : OUI
                   │ │ └────────── Destination encombrée : OUI
                   │ └──────────── Densité forte : NON
                   └────────────── Prioritaire : OUI ← déclenche le vert immédiatement
```

---

## Moteur de décision multicritère

La fonction `evaluerVoie()` applique **5 règles en cascade** — chaque règle peut bloquer définitivement ou laisser passer à la suivante :

```
evaluerVoie(voie i)
│
├─► Bit 6 = 1 (prioritaire) ?  ──── OUI ──► VERT (priorité absolue)
│
├─► Bit 3 = 1 (occupée) ?      ──── OUI ──► ROUGE (voie bloquée)
│
├─► Bit 2 = 0 (pas de désir) ? ──── OUI ──► ROUGE (personne n'attend)
│
├─► Direction invalide (11) ?  ──── OUI ──► ROUGE (mouvement impossible)
│
└─► Destination encombrée ?
      ├── Déclarée (bit 4 = 1)     ─┐
      └── Réelle (voie dest : bit 3 ou 5 actif) ─┘
             OUI (l'une ou l'autre) ──► ROUGE
             NON ──► VERT (toutes les conditions validées)
```

### Résolution des conflits (4 voies simultanées)

```
Priorité 1 : voie avec véhicule prioritaire + direction valide
Priorité 2 : voie avec le plus de véhicules (parmi celles validées)
Aucune     : tous les feux restent rouges → DUREE_ROUGE secondes
```

### Durée dynamique du feu vert

```cpp
tempsRestant = 5 + (vehiculesVoie[gagnant] / 2)
                 + (bitDensite(etatVoie[gagnant]) ? 5 : 0);
// Encadré entre 5 et 20 secondes
```

---

## Commandes disponibles

Ouvrir le **Moniteur Série** (9600 bauds), taper la commande et appuyer sur Entrée.

| Commande | Description |
|----------|-------------|
| `N` / `E` / `S` / `O` | Changer la voie active (Nord / Est / Sud / Ouest) |
| `1` | Feu local + raison de la décision (alterne toutes les 1,8s) |
| `2` | Feu de la voie opposée + raison |
| `3` | Nombre de véhicules sur les 4 voies |
| `4` | Historique des 8 dernières décisions + raison de la dernière |
| `5` | État binaire complet des 4 voies + voie gagnante |
| `6` | Liste des voies avec un véhicule prioritaire |
| `7` | Statistique : trafic cumulé depuis le démarrage |
| `8` | Statistique : équité du feu vert (% par voie) |
| `9` | Statistique : fréquence des véhicules prioritaires |
| `10` | État simultané des 3 autres voies (voie + feu + véhicules) |
| `0` ou `#` | Retour à l'affichage de surveillance normal |

### Format de réponse dans le Moniteur Série

```
########################################
>>> COMMANDE : changer la voie active -> NORD
----------------------------------------
Voie consultee   : NORD
Etat binaire     : 1011010
Vehicules        : 14
Prioritaire      : OUI
Direction        : GAUCHE
Feu actuel       : VERT (12s restantes)
Raison           : Vehicule prioritaire present sur cette voie
########################################
```

---

## Statistiques intégrées

| Menu | Indicateur | Utilité |
|------|------------|---------|
| `7` | Trafic cumulé par voie | Identifier les axes les plus chargés pour prioriser les aménagements |
| `8` | % de feux verts par voie | Contrôler l'équité algorithmique (aucune voie défavorisée) |
| `9` | Nb + fréquence des urgences | Dimensionner le système de priorité (feux sonores, signalétique) |

---

## Bibliothèques requises

Installer depuis le **Gestionnaire de bibliothèques** de l'IDE Arduino (Outils → Gérer les bibliothèques) :

| Bibliothèque | Version testée | Auteur |
|---|---|---|
| `LiquidCrystal_I2C` | 1.1.2 | Frank de Brabander / johnrickman |
| `Wire` | _(incluse dans Arduino IDE)_ | Arduino |

> ⚠️ Le warning *"LiquidCrystal_I2C prétend être exécutable sur all..."* est normal et n'empêche pas le fonctionnement.

---

## Installation et utilisation

```bash
# 1. Cloner le dépôt
git clone https://github.com/VOTRE_NOM/carrefour-intelligent-arduino.git

# 2. Ouvrir dans l'IDE Arduino
#    Fichier → Ouvrir → carrefour_intelligent.ino

# 3. Installer les bibliothèques (voir section précédente)

# 4. Sélectionner la carte
#    Outils → Type de carte → Arduino Uno

# 5. Téléverser
#    Cliquer sur la flèche "Téléverser" (ou Ctrl+U)

# 6. Ouvrir le Moniteur Série
#    Outils → Moniteur Série → 9600 bauds
```

---

## Pistes d'évolution

- 🔌 **Capteurs physiques** : HC-SR04 (ultrason) pour remplacer la génération aléatoire
- 📡 **Réseau inter-carrefours** : modules nRF24L01 ou ESP8266 pour partager les états
- 🚶 **Feux piétons** : bouton poussoir + bit 7 dans le mot binaire (extension à 8 bits)
- 🕒 **Horodatage** : module RTC DS3231 + carte SD pour journaliser avec l'heure réelle
- 🌐 **Dashboard web** : ESP32 + MQTT + Node-RED pour visualiser les stats en ligne
- 🧠 **IA embarquée** : TensorFlow Lite Micro pour un apprentissage adaptatif du trafic

---

## Auteur

Projet réalisé dans le cadre du cours d'Architecture des systèmes embarqués — INGC2 / MIAGE2, année 2025-2026.

---

*Système opérationnel — Arduino Uno R3 — Mémoire programme : ~42% | Mémoire RAM : ~25%*
