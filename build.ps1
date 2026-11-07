param([switch]$Test, [switch]$Demo, [switch]$Run, [string]$Distribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & wsl.exe -d $Distribution --cd $PSScriptRoot -- make all
    if ($LASTEXITCODE -ne 0) { throw 'Compilation Linux echouee. Verifier WSL, GCC et Make dans Ubuntu.' }
    if ($Test) {
        & wsl.exe -d $Distribution --cd $PSScriptRoot -- make test
        if ($LASTEXITCODE -ne 0) { throw 'Tests echoues.' }
    }
    if ($Demo) {
        & wsl.exe -d $Distribution --cd $PSScriptRoot -- ./build/banque --demo
        if ($LASTEXITCODE -ne 0) { throw 'Demonstration echouee.' }
    }
    if ($Run) {
        & wsl.exe -d $Distribution --cd $PSScriptRoot -- ./build/banque
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 130) { throw 'Execution interrompue par une erreur.' }
    }
} finally { Pop-Location }
