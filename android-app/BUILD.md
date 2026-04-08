# BRL Telemetry App – APK bauen

## Voraussetzungen

- Node.js 18+ (https://nodejs.org)
- Ein kostenloses Expo-Konto (https://expo.dev/signup)

## APK bauen (Cloud-Build – kein Android Studio nötig)

```bash
# 1. In das App-Verzeichnis wechseln
cd android-app

# 2. Abhängigkeiten installieren
npm install

# 3. EAS CLI installieren (einmalig)
npm install -g eas-cli

# 4. Anmelden
eas login

# 5. Projekt initialisieren (einmalig – erzeugt projektId in app.json)
eas init

# 6. APK bauen
eas build --platform android --profile preview
```

Nach ~10 Minuten erscheint ein Download-Link für die APK.
Die APK auf das Android-Gerät kopieren und installieren.
(Unter Android: Einstellungen → Sicherheit → "Unbekannte Quellen" erlauben)

## App lokal entwickeln (optional, braucht Android Studio)

```bash
npx expo start
# Dann "a" drücken für Android-Emulator
```

## App-Funktionen

### Verbinden
- Laptimer WiFi aktivieren (Einstellungen → WiFi AP)
- Handy mit Laptimer-WLAN verbinden (SSID: BRL-Laptimer)
- App öffnen → IP-Adresse eingeben (AP-Modus: 192.168.4.1)
- "Verbinden" drücken

### Sessions herunterladen
- Reiter "Gerät" → alle Sessions auf dem Laptimer werden angezeigt
- "Herunterladen" → Session wird lokal gespeichert
- Danach kann das Handy wieder ins normale WLAN wechseln

### Telemetrie ansehen
- Reiter "Lokal" → gespeicherte Sessions
- Session antippen → Rundenzeiten, Sektoren, Deltazeiten
- "Karte" → GPS-Strecke auf OpenStreetMap
  - Beste Runde = grün, andere Runden = farbig
  - Beste Runde: Geschwindigkeits-Farbgebung (blau=langsam → rot=schnell)
  - Runden einzeln ein-/ausblenden

## Hinweise

- Karte benötigt Internetverbindung für OpenStreetMap-Kacheln (kein API-Key nötig)
- Sessions bleiben lokal auf dem Handy gespeichert
- Für iOS-Build: `eas build --platform ios --profile preview` (Apple-Entwicklerkonto erforderlich)
