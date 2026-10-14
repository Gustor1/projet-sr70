param([switch]$Test, [switch]$Demo)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc src/main.c src/banque.c -o build/banque.exe
    if ($LASTEXITCODE -ne 0) { throw 'Compilation de la banque echouee.' }
    & gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc tests/test_banque.c src/banque.c -o build/test_banque.exe
    if ($LASTEXITCODE -ne 0) { throw 'Compilation des tests echouee.' }
    if ($Test) {
        & ./build/test_banque.exe
        if ($LASTEXITCODE -ne 0) { throw 'Tests echoues.' }
    }
    if ($Demo) {
        & ./build/banque.exe --demo
        if ($LASTEXITCODE -ne 0) { throw 'Demonstration echouee.' }
    }
} finally { Pop-Location }
