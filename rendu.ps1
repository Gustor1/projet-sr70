# Prepare le rendu dans le dossier rendu/ :
#   - SR70_Banque_LE_LUTZ_ABDUL-RAZEK_rapport.pdf : rapport (depuis docs/rapport/rapport.html)
#   - SR70_Banque_LE_LUTZ_ABDUL-RAZEK_code.zip    : code source du projet
# Auteurs : Eliott LE, Matthias LUTZ, Jad ABDUL RAZEK
# Usage (PowerShell, dans le dossier du projet) : ./rendu.ps1
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $nom = 'SR70_Banque_LE_LUTZ_ABDUL-RAZEK'
    New-Item -ItemType Directory -Force rendu | Out-Null

    # 1. Rapport PDF, imprime par Edge (ou Chrome) en mode sans fenetre.
    $navigateurs = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
                     "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
                     "$env:ProgramFiles\Google\Chrome\Application\chrome.exe")
    $navigateur = $navigateurs | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $navigateur) { throw 'Edge ou Chrome est necessaire pour generer le PDF.' }
    $source = (Resolve-Path 'docs\rapport\rapport.html').Path
    $pdf = Join-Path (Resolve-Path 'rendu').Path "${nom}_rapport.pdf"
    if (Test-Path $pdf) { Remove-Item $pdf }
    & $navigateur --headless=new --disable-gpu --no-pdf-header-footer "--print-to-pdf=$pdf" ('file:///' + ($source -replace '\\', '/')) | Out-Null
    for ($i = 0; $i -lt 30 -and -not (Test-Path $pdf); $i++) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $pdf)) { throw 'Le PDF n''a pas ete genere.' }
    Write-Host "Rapport : $pdf"

    # 2. Archive ZIP du code source (sans les executables de build/).
    $copie = Join-Path $env:TEMP $nom
    if (Test-Path $copie) { Remove-Item -Recurse -Force $copie }
    New-Item -ItemType Directory $copie | Out-Null
    $fichiers = 'src', 'tests', 'Makefile', 'build.ps1', 'lancer.cmd', 'README.md', 'EXPLICATIONS.md'
    foreach ($f in $fichiers) { Copy-Item -Recurse $f $copie }
    New-Item -ItemType Directory (Join-Path $copie 'docs') | Out-Null
    Copy-Item 'docs\consignes-banque.pdf' (Join-Path $copie 'docs')
    $zip = Join-Path (Resolve-Path 'rendu').Path "${nom}_code.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    # tar.exe (inclus dans Windows) ecrit des chemins avec '/', lisibles sous Linux,
    # contrairement a Compress-Archive qui utilise '\'.
    & tar.exe -a -c -f $zip -C $env:TEMP $nom
    if ($LASTEXITCODE -ne 0) { throw 'Creation de l''archive impossible.' }
    Remove-Item -Recurse -Force $copie
    Write-Host "Archive : $zip"
} finally { Pop-Location }
