

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================== LCD I2C ==================
LiquidCrystal_I2C lcd(0x27, 16, 2); // si écran noir/blanc sans texte : essayer 0x3F

// ================== LEDS (feu REEL de la voie active) ==================
const int LED_V = 11;
const int LED_R = 10;

// ================== VOIES ==================
const byte NB_VOIES = 4;
const char* nomVoie[NB_VOIES] = {"NORD", "EST", "SUD", "OUEST"};
// Index :                         0       1      2      3

byte etatVoie[NB_VOIES];        // mot binaire 7 bits de chaque voie
int  vehiculesVoie[NB_VOIES];   // nombre de vehicules par voie (cycle courant)
int  destVoie[NB_VOIES];        // voie de destination calculee (-1 = invalide)
bool souhaiteVoie[NB_VOIES];    // la voie souhaite-t-elle passer (apres validation) ?
bool vertVoie[NB_VOIES];        // decision finale (vert) pour chaque voie
int  voieGagnante = -1;         // voie ayant obtenu le feu vert ce cycle

byte voieActive = 0; // voie actuellement affichee/controlee par le feu reel (LED)

// ================== ETATS DU SYSTEME ==================
enum EtatSysteme { GENERER, VERT, ROUGE };
EtatSysteme etatSysteme = GENERER;

int tempsRestant = 0;
unsigned long chronoMillis = 0;
const int DUREE_ROUGE = 4; // secondes d'observation si personne ne passe

// Historique des decisions de la voie active (8 derniers cycles)
char historique[9] = "--------";

// ================== STATISTIQUES CUMULEES (depuis le demarrage) ==================
unsigned long totalCycles = 0;
unsigned long totalVehicules[NB_VOIES] = {0, 0, 0, 0};
unsigned int  vertCount[NB_VOIES] = {0, 0, 0, 0};
unsigned long totalPrioritaire = 0;

// ================== AFFICHAGE TEMPORAIRE (apres une commande) ==================
bool diagActif = false;
char diagCommande = 0;
unsigned long diagFinMillis = 0;
const unsigned long DUREE_DIAG = 4000;

unsigned long dernierAffichageNormal = 0;

// ================== DETECTION DE LA SEQUENCE "1"+"0" = "10" ==================
bool attenteSuite1 = false;
unsigned long attenteSuite1Debut = 0;
const unsigned long DELAI_ATTENTE_10 = 300; // ms

// ============================================================
//                     FONCTIONS UTILITAIRES
// ============================================================

void etatBinaireBuf(byte e, char *buf) {
  for (int i = 6; i >= 0; i--) buf[6 - i] = bitRead(e, i) ? '1' : '0';
  buf[7] = '\0';
}

bool bitPrioritaire(byte e)   { return bitRead(e, 6); }
bool bitDensite(byte e)       { return bitRead(e, 5); }
bool bitDestEncombree(byte e) { return bitRead(e, 4); }
bool bitOccupee(byte e)       { return bitRead(e, 3); }
bool bitDesir(byte e)         { return bitRead(e, 2); }
bool bitDirMSB(byte e)        { return bitRead(e, 1); }
bool bitDirLSB(byte e)        { return bitRead(e, 0); }

void serialPrintDirection(byte e) {
  bool m = bitDirMSB(e), l = bitDirLSB(e);
  if (!m && !l) Serial.print(F("TOUT DROIT"));
  else if (!m &&  l) Serial.print(F("DROITE"));
  else if ( m && !l) Serial.print(F("GAUCHE"));
  else Serial.print(F("RESERVE"));
}

size_t lcdPrintDirection(byte e) {
  bool m = bitDirMSB(e), l = bitDirLSB(e);
  if (!m && !l) return lcd.print(F("TOUT DROIT"));
  if (!m &&  l) return lcd.print(F("DROITE"));
  if ( m && !l) return lcd.print(F("GAUCHE"));
  return lcd.print(F("RESERVE"));
}

void genererEtat(byte &etat, int &vehicules) {
  etat = random(0, 128);
  vehicules = random(0, 21);
}

int calculerVoieDestination(int voie, byte etat) {
  bool m = bitDirMSB(etat), l = bitDirLSB(etat);
  if (!m && !l) return (voie + 2) % NB_VOIES;
  if (!m &&  l) return (voie + 1) % NB_VOIES;
  if ( m && !l) return (voie + 3) % NB_VOIES;
  return -1;
}

bool evaluerVoie(int i) {
  byte e = etatVoie[i];
  if (bitPrioritaire(e)) return true;
  if (bitOccupee(e))     return false;
  if (!bitDesir(e))      return false;

  int dest = destVoie[i];
  if (dest == -1) return false;

  bool destDeclareeEncombree = bitDestEncombree(e);
  byte etatDest = etatVoie[dest];
  bool destReellementEncombree = bitOccupee(etatDest) || bitDensite(etatDest);

  if (destDeclareeEncombree || destReellementEncombree) return false;
  return true;
}

int determinerGagnant() {
  for (int i = 0; i < NB_VOIES; i++) {
    if (bitPrioritaire(etatVoie[i]) && destVoie[i] != -1) return i;
  }
  int meilleur = -1, maxVeh = -1;
  for (int i = 0; i < NB_VOIES; i++) {
    if (souhaiteVoie[i] && vehiculesVoie[i] > maxVeh) {
      maxVeh = vehiculesVoie[i];
      meilleur = i;
    }
  }
  return meilleur;
}

char calculerRaison(int i) {
  byte e = etatVoie[i];
  if (vertVoie[i]) {
    if (bitPrioritaire(e)) return 'P';
    return 'L';
  }
  if (bitOccupee(e)) return 'O';
  if (!bitDesir(e))  return 'D';
  if (destVoie[i] == -1) return 'X';
  bool destEnc = bitDestEncombree(e);
  if (!destEnc) {
    byte ed = etatVoie[destVoie[i]];
    destEnc = bitOccupee(ed) || bitDensite(ed);
  }
  if (destEnc) return 'E';
  return 'C';
}

size_t lcdPrintRaison(char r) {
  switch (r) {
    case 'P': return lcd.print(F("Prioritaire"));
    case 'L': return lcd.print(F("Voie libre"));
    case 'O': return lcd.print(F("Voie occupee"));
    case 'D': return lcd.print(F("Pas de desir"));
    case 'X': return lcd.print(F("Dir invalide"));
    case 'E': return lcd.print(F("Dest encombree"));
    case 'C': return lcd.print(F("Autre voie gagne"));
  }
  return lcd.print(F("?"));
}

void serialPrintRaison(char r) {
  switch (r) {
    case 'P': Serial.print(F("Vehicule prioritaire present sur cette voie")); break;
    case 'L': Serial.print(F("Voie libre, vehicule(s) en attente, voie la plus chargee")); break;
    case 'O': Serial.print(F("Voie actuellement occupee (bit occupation actif)")); break;
    case 'D': Serial.print(F("Aucun vehicule ne souhaite traverser")); break;
    case 'X': Serial.print(F("Direction codee invalide (combinaison reservee)")); break;
    case 'E': Serial.print(F("Voie de destination encombree (declaree ou verifiee)")); break;
    case 'C': Serial.print(F("Une autre voie avait la priorite ou etait plus chargee")); break;
    default:  Serial.print(F("Inconnue")); break;
  }
}

void ajouterHistorique(bool vert) {
  for (int i = 0; i < 7; i++) historique[i] = historique[i + 1];
  historique[7] = vert ? 'V' : 'R';
}

void mettreAJourLED() {
  digitalWrite(LED_V, vertVoie[voieActive] ? HIGH : LOW);
  digitalWrite(LED_R, vertVoie[voieActive] ? LOW  : HIGH);
}

// ============================================================
//   GENERATION + DECISION (1 CYCLE COMPLET) - SANS LOGS AUTOMATIQUES
// ============================================================
void nouveauCycle() {

  for (int i = 0; i < NB_VOIES; i++) {
    genererEtat(etatVoie[i], vehiculesVoie[i]);
    destVoie[i] = calculerVoieDestination(i, etatVoie[i]);
  }
  for (int i = 0; i < NB_VOIES; i++) {
    souhaiteVoie[i] = evaluerVoie(i);
  }
  voieGagnante = determinerGagnant();
  for (int i = 0; i < NB_VOIES; i++) vertVoie[i] = (i == voieGagnante);

  totalCycles++;
  for (int i = 0; i < NB_VOIES; i++) {
    totalVehicules[i] += vehiculesVoie[i];
    if (vertVoie[i]) vertCount[i]++;
    if (bitPrioritaire(etatVoie[i])) totalPrioritaire++;
  }

  if (voieGagnante != -1) {
    tempsRestant = 5 + (vehiculesVoie[voieGagnante] / 2) + (bitDensite(etatVoie[voieGagnante]) ? 5 : 0);
    if (tempsRestant > 20) tempsRestant = 20;
    etatSysteme = VERT;
  } else {
    tempsRestant = DUREE_ROUGE;
    etatSysteme = ROUGE;
  }

  ajouterHistorique(vertVoie[voieActive]);
  chronoMillis = millis();
  mettreAJourLED();
}

// ============================================================
//                  AFFICHAGE LCD - MODE NORMAL
// ============================================================
void afficherNormal() {
  lcd.setCursor(0, 0);
  size_t n = 0;
  n += lcd.print(nomVoie[voieActive]);
  n += lcd.print(' ');
  n += lcd.print(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
  n += lcd.print(' ');
  n += lcd.print(tempsRestant);
  n += lcd.print('s');
  while (n < 16) { lcd.print(' '); n++; }

  lcd.setCursor(0, 1);
  n = 0;
  int phase = (millis() / 1800) % 3;
  if (phase == 0) {
    n += lcd.print(F("Vehicules:"));
    n += lcd.print(vehiculesVoie[voieActive]);
  } else if (phase == 1) {
    n += lcd.print(F("Prioritaire:"));
    n += lcd.print(bitPrioritaire(etatVoie[voieActive]) ? F("OUI") : F("NON"));
  } else {
    n += lcd.print(F("Dir:"));
    n += lcdPrintDirection(etatVoie[voieActive]);
  }
  while (n < 16) { lcd.print(' '); n++; }
}

// ============================================================
//        AFFICHAGE TEMPORAIRE (apres une commande clavier PC)
// ============================================================
void afficherDiagnostic(char cmd) {
  cmd = toupper(cmd);
  size_t n;

  if (cmd == 'N' || cmd == 'E' || cmd == 'S' || cmd == 'O') {
    lcd.setCursor(0, 0);
    n = lcd.print(F("Voie active:"));
    while (n < 16) { lcd.print(' '); n++; }

    lcd.setCursor(0, 1);
    n = lcd.print(nomVoie[voieActive]);
    n += lcd.print(F(" V:"));
    n += lcd.print(vehiculesVoie[voieActive]);
    n += lcd.print(' ');
    n += lcd.print(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
    while (n < 16) { lcd.print(' '); n++; }
    return;
  }

  switch (cmd) {

    case '1': {
      int phase = (millis() / 1800) % 2;
      if (phase == 0) {
        lcd.setCursor(0, 0);
        n = lcd.print(F("1-FEU LOCAL"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcd.print(nomVoie[voieActive]);
        n += lcd.print(' ');
        n += lcd.print(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
        n += lcd.print(' ');
        n += lcd.print(tempsRestant);
        n += lcd.print('s');
        while (n < 16) { lcd.print(' '); n++; }
      } else {
        lcd.setCursor(0, 0);
        n = lcd.print(F("1-POURQUOI ?"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcdPrintRaison(calculerRaison(voieActive));
        while (n < 16) { lcd.print(' '); n++; }
      }
      break;
    }

    case '2': {
      int opp = (voieActive + 2) % NB_VOIES;
      int phase = (millis() / 1800) % 2;
      if (phase == 0) {
        lcd.setCursor(0, 0);
        n = lcd.print(F("2-FEU OPPOSE"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcd.print(nomVoie[opp]);
        n += lcd.print(' ');
        n += lcd.print(vertVoie[opp] ? F("VERT") : F("ROUGE"));
        while (n < 16) { lcd.print(' '); n++; }
      } else {
        lcd.setCursor(0, 0);
        n = lcd.print(F("2-POURQUOI ?"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcdPrintRaison(calculerRaison(opp));
        while (n < 16) { lcd.print(' '); n++; }
      }
      break;
    }

    case '3':
      lcd.setCursor(0, 0);
      n = lcd.print(F("3-VEHICULES"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      n = lcd.print(F("N"));
      n += lcd.print(vehiculesVoie[0]);
      n += lcd.print(F(" E"));
      n += lcd.print(vehiculesVoie[1]);
      n += lcd.print(F(" S"));
      n += lcd.print(vehiculesVoie[2]);
      n += lcd.print(F(" O"));
      n += lcd.print(vehiculesVoie[3]);
      while (n < 16) { lcd.print(' '); n++; }
      break;

    case '4': {
      int phase = (millis() / 1800) % 2;
      if (phase == 0) {
        lcd.setCursor(0, 0);
        n = lcd.print(F("4-HISTORIQUE"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcd.print(F("H:"));
        n += lcd.print(historique);
        while (n < 16) { lcd.print(' '); n++; }
      } else {
        lcd.setCursor(0, 0);
        n = lcd.print(F("Dernier:"));
        n += lcd.print(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
        while (n < 16) { lcd.print(' '); n++; }
        lcd.setCursor(0, 1);
        n = lcdPrintRaison(calculerRaison(voieActive));
        while (n < 16) { lcd.print(' '); n++; }
      }
      break;
    }

    case '5':
      lcd.setCursor(0, 0);
      n = lcd.print(F("5-ETAT CTRL"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      if ((millis() / 1500) % 2 == 0) {
        char bufEtat[8];
        etatBinaireBuf(etatVoie[voieActive], bufEtat);
        n = lcd.print(nomVoie[voieActive]);
        n += lcd.print(':');
        n += lcd.print(bufEtat);
      } else {
        n = lcd.print(F("Gagnant:"));
        if (voieGagnante == -1) {
          n += lcd.print(F("AUCUNE"));
        } else {
          n += lcd.print(nomVoie[voieGagnante]);
        }
      }
      while (n < 16) { lcd.print(' '); n++; }
      break;

    case '6': {
      lcd.setCursor(0, 0);
      n = lcd.print(F("6-PRIORITAIRES"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      n = 0;
      bool aucun = true;
      for (int i = 0; i < NB_VOIES; i++) {
        if (bitPrioritaire(etatVoie[i])) {
          n += lcd.print(nomVoie[i][0]);
          n += lcd.print(' ');
          aucun = false;
        }
      }
      if (aucun) n += lcd.print(F("Aucun"));
      while (n < 16) { lcd.print(' '); n++; }
      break;
    }

    case '7': {
      int phase = (millis() / 2000) % NB_VOIES;
      lcd.setCursor(0, 0);
      n = lcd.print(F("7-TRAFIC CUMULE"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      n = lcd.print(nomVoie[phase]);
      n += lcd.print(':');
      n += lcd.print(totalVehicules[phase]);
      n += lcd.print(F(" veh"));
      while (n < 16) { lcd.print(' '); n++; }
      break;
    }

    case '8': {
      int phase = (millis() / 2000) % NB_VOIES;
      unsigned long pct = (totalCycles > 0) ? (vertCount[phase] * 100UL) / totalCycles : 0;
      lcd.setCursor(0, 0);
      n = lcd.print(F("8-EQUITE FEUX"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      n = lcd.print(nomVoie[phase]);
      n += lcd.print(':');
      n += lcd.print(pct);
      n += lcd.print(F("% vert"));
      while (n < 16) { lcd.print(' '); n++; }
      break;
    }

    case '9': {
      int phase = (millis() / 1800) % 2;
      lcd.setCursor(0, 0);
      n = lcd.print(F("9-URGENCES VIP"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      if (phase == 0) {
        n = lcd.print(F("Total:"));
        n += lcd.print(totalPrioritaire);
        n += lcd.print(F(" det"));
      } else {
        unsigned long pctVip = (totalCycles > 0) ? (totalPrioritaire * 100UL) / ((unsigned long)totalCycles * NB_VOIES) : 0;
        n = lcd.print(F("Freq:"));
        n += lcd.print(pctVip);
        n += lcd.print('%');
      }
      while (n < 16) { lcd.print(' '); n++; }
      break;
    }

    // ============ NOUVEAU : MENU 10 (code interne 'A') ============
    case 'A': {
      lcd.setCursor(0, 0);
      n = lcd.print(F("10-AUTRES VOIES"));
      while (n < 16) { lcd.print(' '); n++; }

      lcd.setCursor(0, 1);
      n = 0;
      bool premier = true;
      for (int i = 0; i < NB_VOIES; i++) {
        if (i == voieActive) continue; // on ne montre que les 3 AUTRES voies
        if (!premier) n += lcd.print(' ');
        n += lcd.print(nomVoie[i][0]);
        n += lcd.print(vertVoie[i] ? F("V") : F("R"));
        n += lcd.print(vehiculesVoie[i]);
        premier = false;
      }
      while (n < 16) { lcd.print(' '); n++; }
      break;
    }

    default:
      lcd.setCursor(0, 0);
      n = lcd.print(F("Commande"));
      while (n < 16) { lcd.print(' '); n++; }
      lcd.setCursor(0, 1);
      n = lcd.print(F("inconnue"));
      while (n < 16) { lcd.print(' '); n++; }
      break;
  }
}

// ============================================================
//   REPONSE "INTERACTIVE" IMMEDIATE DANS LE MONITEUR SERIE
// ============================================================
void imprimerReponseCommande(char cmd) {
  Serial.println();
  Serial.println(F("########################################"));

  if (cmd == 'N' || cmd == 'E' || cmd == 'S' || cmd == 'O') {
    Serial.print(F(">>> COMMANDE : changer la voie active -> "));
    Serial.println(nomVoie[voieActive]);
  } else if (cmd == 'A') {
    Serial.println(F(">>> COMMANDE : 10 (etat des 3 autres voies)"));
  } else {
    Serial.print(F(">>> COMMANDE : ")); Serial.println(cmd);
  }
  Serial.println(F("----------------------------------------"));

  char bufEtat[8];

  switch (cmd) {
    case 'N': case 'E': case 'S': case 'O': case '1':
      etatBinaireBuf(etatVoie[voieActive], bufEtat);
      Serial.print(F("Voie consultee   : ")); Serial.println(nomVoie[voieActive]);
      Serial.print(F("Etat binaire     : ")); Serial.println(bufEtat);
      Serial.print(F("Vehicules        : ")); Serial.println(vehiculesVoie[voieActive]);
      Serial.print(F("Prioritaire      : ")); Serial.println(bitPrioritaire(etatVoie[voieActive]) ? F("OUI") : F("NON"));
      Serial.print(F("Direction        : ")); serialPrintDirection(etatVoie[voieActive]); Serial.println();
      Serial.print(F("Feu actuel       : ")); Serial.print(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
      Serial.print(F(" (")); Serial.print(tempsRestant); Serial.println(F("s restantes)"));
      Serial.print(F("Raison           : ")); serialPrintRaison(calculerRaison(voieActive)); Serial.println();
      break;

    case '2': {
      int opp = (voieActive + 2) % NB_VOIES;
      Serial.print(F("Voie opposee     : ")); Serial.println(nomVoie[opp]);
      Serial.print(F("Feu oppose       : ")); Serial.println(vertVoie[opp] ? F("VERT") : F("ROUGE"));
      Serial.print(F("Raison oppose    : ")); serialPrintRaison(calculerRaison(opp)); Serial.println();
      break;
    }

    case '3':
      for (int i = 0; i < NB_VOIES; i++) {
        Serial.print(nomVoie[i]); Serial.print(F(" : ")); Serial.print(vehiculesVoie[i]); Serial.println(F(" vehicules"));
      }
      break;

    case '4':
      Serial.print(F("Historique (8 derniers cycles, voie active) : "));
      Serial.println(historique);
      Serial.print(F("Derniere decision : ")); Serial.println(vertVoie[voieActive] ? F("VERT") : F("ROUGE"));
      Serial.print(F("Raison            : ")); serialPrintRaison(calculerRaison(voieActive)); Serial.println();
      break;

    case '5':
      for (int i = 0; i < NB_VOIES; i++) {
        etatBinaireBuf(etatVoie[i], bufEtat);
        Serial.print(nomVoie[i]); Serial.print(F(" -> ")); Serial.println(bufEtat);
      }
      Serial.print(F("Voie gagnante ce cycle : "));
      if (voieGagnante == -1) {
        Serial.println(F("AUCUNE"));
      } else {
        Serial.println(nomVoie[voieGagnante]);
      }
      break;

    case '6': {
      Serial.print(F("Vehicules prioritaires : "));
      bool aucun = true;
      for (int i = 0; i < NB_VOIES; i++) {
        if (bitPrioritaire(etatVoie[i])) { Serial.print(nomVoie[i]); Serial.print(' '); aucun = false; }
      }
      if (aucun) Serial.print(F("Aucun"));
      Serial.println();
      break;
    }

    case '7':
      Serial.println(F("STATISTIQUE : Trafic cumule par voie depuis le demarrage"));
      for (int i = 0; i < NB_VOIES; i++) {
        Serial.print(nomVoie[i]); Serial.print(F(" : ")); Serial.print(totalVehicules[i]); Serial.println(F(" vehicules au total"));
      }
      Serial.print(F("Nombre de cycles analyses : ")); Serial.println(totalCycles);
      Serial.println(F("=> Utile au gouvernement pour prioriser les amenagements routiers."));
      break;

    case '8':
      Serial.println(F("STATISTIQUE : Equite d'attribution du feu vert par voie"));
      for (int i = 0; i < NB_VOIES; i++) {
        unsigned long pct = (totalCycles > 0) ? (vertCount[i] * 100UL) / totalCycles : 0;
        Serial.print(nomVoie[i]); Serial.print(F(" : ")); Serial.print(pct); Serial.println(F("% des cycles"));
      }
      Serial.println(F("=> Permet de verifier qu'aucune voie n'est defavorisee."));
      break;

    case '9': {
      Serial.println(F("STATISTIQUE : Vehicules prioritaires (urgences) detectes"));
      Serial.print(F("Total detecte (toutes voies) : ")); Serial.println(totalPrioritaire);
      unsigned long pctVip = (totalCycles > 0) ? (totalPrioritaire * 100UL) / ((unsigned long)totalCycles * NB_VOIES) : 0;
      Serial.print(F("Frequence moyenne : ")); Serial.print(pctVip); Serial.println(F("% des passages controles"));
      Serial.println(F("=> Utile pour dimensionner un systeme de priorite (feu sonore, etc.)."));
      break;
    }

    // ============ NOUVEAU : MENU 10 (code interne 'A') ============
    case 'A':
      Serial.println(F("Etat simultane des 3 autres voies :"));
      for (int i = 0; i < NB_VOIES; i++) {
        if (i == voieActive) continue;
        Serial.print(nomVoie[i]);
        Serial.print(F(" : "));
        Serial.print(vertVoie[i] ? F("VERT") : F("ROUGE"));
        Serial.print(F(" - "));
        Serial.print(vehiculesVoie[i]);
        Serial.println(F(" vehicules"));
      }
      break;
  }

  Serial.println(F("########################################"));
  Serial.println();
}

// ============================================================
//   TRAITE UNE COMMANDE DEJA IDENTIFIEE (reutilisable)
// ============================================================
void traiterCommande(char c) {
  char up = toupper(c);
  if (up == 'N' || up == 'E' || up == 'S' || up == 'O' || (up >= '1' && up <= '9') || up == 'A') {

    if (up == 'N') voieActive = 0;
    if (up == 'E') voieActive = 1;
    if (up == 'S') voieActive = 2;
    if (up == 'O') voieActive = 3;
    if (up == 'N' || up == 'E' || up == 'S' || up == 'O') mettreAJourLED();

    diagActif = true;
    diagCommande = up;
    diagFinMillis = millis() + DUREE_DIAG;

    imprimerReponseCommande(up);

  } else if (up == '0' || up == '#') {
    diagActif = false;
    lcd.clear();
  }
}

// ============================================================
//                 LECTURE DES COMMANDES (PORT SERIE)
// ============================================================
char lireCommandeSerie() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') return 0;
    return c;
  }
  return 0;
}

// ============================================================
//                          SETUP
// ============================================================
void setup() {
  Serial.begin(9600);
  randomSeed(analogRead(A0));

  pinMode(LED_V, OUTPUT);
  pinMode(LED_R, OUTPUT);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Carrefour"));
  lcd.setCursor(0, 1);
  lcd.print(F("Intelligent..."));

  Serial.println(F("=== SYSTEME DE GESTION DE CARREFOUR INTELLIGENT ==="));
  Serial.println(F("Tapez une touche puis Entree :"));
  Serial.println();
  Serial.println(F("  N/E/S/O : changer la voie active (Nord/Est/Sud/Ouest)"));
  Serial.println(F("  1 : feu local + raison        4 : historique + raison"));
  Serial.println(F("  2 : feu oppose + raison        5 : etat binaire complet"));
  Serial.println(F("  3 : vehicules (cycle actuel)   6 : vehicules prioritaires"));
  Serial.println(F("  7 : stat - trafic cumule"));
  Serial.println(F("  8 : stat - equite des feux verts"));
  Serial.println(F("  9 : stat - frequence vehicules prioritaires"));
  Serial.println(F("  10 : etat simultane des 3 autres voies"));
  Serial.println(F("  0 ou # : retour affichage normal"));
  Serial.println();

  delay(1500);
  lcd.clear();
  etatSysteme = GENERER;
}

// ============================================================
//                          LOOP
// ============================================================
void loop() {

  char cmd = lireCommandeSerie();

  // ---- Detection de la sequence "1" + "0" = commande "10" ----
  if (attenteSuite1) {
    if (cmd == '0') {
      attenteSuite1 = false;
      traiterCommande('A'); // code interne pour le menu 10
      cmd = 0;
    } else if (cmd != 0) {
      attenteSuite1 = false;
      traiterCommande('1'); // ce n'etait pas un "10" : on traite le "1" seul
      // cmd reste inchange, traite normalement plus bas
    } else if (millis() - attenteSuite1Debut > DELAI_ATTENTE_10) {
      attenteSuite1 = false;
      traiterCommande('1');
    }
  }

  if (cmd == '1' && !attenteSuite1) {
    attenteSuite1 = true;
    attenteSuite1Debut = millis();
    cmd = 0; // on patiente pour voir si un "0" suit
  }

  if (cmd) {
    traiterCommande(cmd);
  }

  switch (etatSysteme) {
    case GENERER:
      nouveauCycle();
      break;

    case VERT:
    case ROUGE:
      if (millis() - chronoMillis >= 1000) {
        chronoMillis = millis();
        tempsRestant--;
        if (tempsRestant <= 0) {
          etatSysteme = GENERER;
        }
      }
      break;
  }

  if (diagActif) {
    if (millis() > diagFinMillis) {
      diagActif = false;
      lcd.clear();
    } else {
      afficherDiagnostic(diagCommande);
    }
  } else {
    if (millis() - dernierAffichageNormal >= 300) {
      dernierAffichageNormal = millis();
      afficherNormal();
    }
  }
}