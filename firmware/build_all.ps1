# Compile les 5 variantes du firmware (une par capteur) et produit un .bin unique par variante
# dans ..\dist\tp_<capteur>.bin, a flasher a l'adresse 0x0 (esptool-js dans Chrome).
#
# Prerequis : ESP-IDF 5.5.6 installe (installateur Windows d'Espressif).
# Usage (PowerShell, dans le dossier firmware) :
#   . 'C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1'   # active ESP-IDF
#   .\build_all.ps1                 # les 5 variantes
#   .\build_all.ps1 simule pir      # seulement certaines
param([string[]]$Capteurs = @('simule', 'bmp280', 'dht11', 'pir', 'analog'))

# 'Continue' : sous PowerShell 5.1, les avertissements ecrits sur stderr par idf.py
# ne doivent pas arreter le script ; on teste $LASTEXITCODE a la place.
$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot
$dist = Join-Path $PSScriptRoot '..\dist'
New-Item -ItemType Directory -Force $dist | Out-Null

foreach ($c in $Capteurs) {
    if (-not (Test-Path "sensors\$c")) { throw "Capteur inconnu : $c (voir le dossier sensors)" }
    $b = "build_$c"
    Write-Host "=== $c : compilation dans $b ===" -ForegroundColor Cyan
    # sdkconfig propre a chaque variante, genere depuis sdkconfig.defaults + sensors\<capteur>
    idf.py -B $b -D SDKCONFIG="$b/sdkconfig" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sensors/$c" build
    if ($LASTEXITCODE -ne 0) { throw "Echec de compilation : $c" }

    # Un seul fichier (bootloader + table de partitions + application) a flasher en 0x0
    Push-Location $b
    python -m esptool --chip esp32 merge_bin -o (Join-Path $dist "tp_$c.bin") '@flash_args'
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { throw "Echec de merge_bin : $c" }
    Write-Host "=== $c : OK -> dist\tp_$c.bin ===" -ForegroundColor Green
}
Get-ChildItem $dist -Filter 'tp_*.bin' | Select-Object Name, Length
