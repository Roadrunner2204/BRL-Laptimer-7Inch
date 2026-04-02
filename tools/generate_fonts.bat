@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

echo.
echo  BRL Laptimer -- LVGL Font Generator
echo  =====================================
echo.

:: ---------------------------------------------------------------------------
:: Paths (relative to this .bat file location)
:: ---------------------------------------------------------------------------
set "SCRIPT_DIR=%~dp0"
set "FONT_FILE=%SCRIPT_DIR%Montserrat-Regular.ttf"
set "OUTPUT_DIR=%SCRIPT_DIR%..\src\ui\fonts"

:: ---------------------------------------------------------------------------
:: Check: Montserrat-Regular.ttf present?
:: ---------------------------------------------------------------------------
if not exist "%FONT_FILE%" (
    echo  FEHLER: Montserrat-Regular.ttf nicht gefunden!
    echo.
    echo  Bitte:
    echo    1. fonts.google.com aufrufen
    echo    2. "Montserrat" suchen
    echo    3. "Get font" -- "Download all" klicken
    echo    4. ZIP entpacken
    echo    5. Montserrat-Regular.ttf in diesen Ordner kopieren:
    echo       %SCRIPT_DIR%
    echo.
    pause
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: Check: Node.js / lv_font_conv installed?
:: ---------------------------------------------------------------------------
where node >nul 2>&1
if errorlevel 1 (
    echo  FEHLER: Node.js nicht gefunden!
    echo.
    echo  Bitte nodejs.org aufrufen, LTS-Version herunterladen und installieren.
    echo  Danach diese .bat erneut ausfuehren.
    echo.
    pause
    exit /b 1
)

where lv_font_conv >nul 2>&1
if errorlevel 1 (
    echo  lv_font_conv nicht gefunden -- wird jetzt installiert...
    echo.
    call npm install -g lv_font_conv
    if errorlevel 1 (
        echo.
        echo  FEHLER: Installation von lv_font_conv fehlgeschlagen.
        echo  Bitte manuell ausfuehren: npm install -g lv_font_conv
        pause
        exit /b 1
    )
)

:: ---------------------------------------------------------------------------
:: Create output directory
:: ---------------------------------------------------------------------------
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

:: Unicode ranges:
::   0x20-0x7E  Basic Latin (ASCII)
::   0xA0-0xFF  Latin-1 Supplement (Ä Ö Ü ä ö ü ß À Ã Ñ etc.)
set "RANGES=0x20-0x7E,0xA0-0xFF"

echo  Generiere Fonts in: %OUTPUT_DIR%
echo.

:: ---------------------------------------------------------------------------
:: Generate all sizes
:: ---------------------------------------------------------------------------
for %%S in (14 16 20 24 32 40 48) do (
    set "OUTFILE=%OUTPUT_DIR%\brl_font_montserrat_%%S.c"
    echo    Groesse %%S px  --^>  brl_font_montserrat_%%S.c
    call lv_font_conv ^
        --font "%FONT_FILE%" ^
        -r "%RANGES%" ^
        --size %%S ^
        --format lvgl ^
        --bpp 4 ^
        --no-compress ^
        --force-fast-kern-format ^
        -o "!OUTFILE!"
    if errorlevel 1 (
        echo.
        echo  FEHLER bei Groesse %%S -- Abbruch.
        pause
        exit /b 1
    )
)

echo.
echo  =============================================
echo   Fertig! Alle 7 Font-Dateien generiert.
echo  =============================================
echo.
echo  Naechster Schritt:
echo    1. include\lv_conf.h oeffnen
echo    2. Diese Zeile aendern:
echo         #define BRL_USE_EXTENDED_FONTS  0
echo       auf:
echo         #define BRL_USE_EXTENDED_FONTS  1
echo    3. PlatformIO neu bauen (Build)
echo.
pause
