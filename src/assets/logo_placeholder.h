#pragma once
/**
 * Logo-Platzhalter
 *
 * SO ERSETZT DU DAS MIT DEINEM ECHTEN LOGO:
 * ──────────────────────────────────────────
 * 1. Logo-Bild als JPEG vorbereiten
 *    - Empfohlene Größe: 400 x 200 px (oder nach Wunsch)
 *    - Format: JPEG, Qualität 85-95%
 *    - Dateiname: logo.jpg
 *
 * 2. JPEG in C-Array konvertieren:
 *    Online-Tool: https://javl.github.io/image2cpp/
 *    Oder per Python:
 *      python -c "
 *      data = open('logo.jpg','rb').read()
 *      print('const uint8_t LOGO_JPG[] = {' + ','.join(hex(b) for b in data) + '};')
 *      print(f'const uint32_t LOGO_JPG_SIZE = {len(data)};')
 *      " > logo_data.h
 *
 * 3. Diese Datei ersetzen:
 *    #pragma once
 *    extern const uint8_t  LOGO_JPG[];
 *    extern const uint32_t LOGO_JPG_SIZE;
 *
 * 4. Eigene logo_data.cpp anlegen mit den Array-Daten.
 *
 * BIS DAHIN: Der Splash zeigt einen Text-Platzhalter.
 */
#define LOGO_USE_PLACEHOLDER  1
