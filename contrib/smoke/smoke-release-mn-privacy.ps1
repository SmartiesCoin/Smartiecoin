param(
    [ValidateSet("quick", "full")]
    [string]$Profile = "quick",
    [switch]$BuildBinaries,
    [switch]$ContinueOnFailure
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$bashPath = Join-Path $repoRoot ".local/msys_extract/msys64/usr/bin/bash.exe"

function Require-Command([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Missing required command: $Name"
    }
}

function Clear-ReadOnlyRecursively([string]$Path) {
    if (-not (Test-Path $Path)) {
        return
    }

    $readOnly = [System.IO.FileAttributes]::ReadOnly
    $items = @((Get-Item -Force $Path)) + (Get-ChildItem -Force -Recurse $Path)

    foreach ($item in $items) {
        if (($item.Attributes -band $readOnly) -ne 0) {
            $item.Attributes = ($item.Attributes -bxor $readOnly)
        }
    }
}

function Reset-FunctionalCache() {
    Get-Process smartiecoind -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    if (-not (Test-Path "test/cache")) {
        return
    }

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        cmd /c "attrib -R /S /D test\cache\*" | Out-Null
        cmd /c "rmdir /S /Q test\cache" | Out-Null
        if (-not (Test-Path "test/cache")) {
            return
        }
        Start-Sleep -Milliseconds 700
    }

    throw "Failed to reset test/cache after multiple attempts."
}

Push-Location $repoRoot
try {
    Require-Command "py"

    if ($BuildBinaries) {
        if (-not (Test-Path $bashPath)) {
            throw "MSYS bash not found at '$bashPath'. Cannot build binaries automatically."
        }

        $buildCmd = @(
            "cd /c/Dev/Smartiecoin"
            "export PATH='/mingw64/bin:/usr/bin:/bin'"
            "make -C src -j8 EVENT_CFLAGS='-IC:/Dev/Smartiecoin/.local/msys_extract/msys64/mingw64/include' LDFLAGS='-LC:/Dev/Smartiecoin/.local/msys_extract/msys64/mingw64/qt5-static/lib' smartiecoind.exe smartiecoin-cli.exe smartiecoin-util.exe"
        ) -join " && "

        & $bashPath -lc $buildCmd
        if ($LASTEXITCODE -ne 0) {
            throw "Binary build failed with exit code $LASTEXITCODE."
        }
    }

    $requiredBinaries = @(
        "src/smartiecoind.exe",
        "src/smartiecoin-cli.exe",
        "src/smartiecoin-util.exe"
    )
    foreach ($bin in $requiredBinaries) {
        if (-not (Test-Path $bin)) {
            throw "Required binary not found: '$bin'. Run with -BuildBinaries first."
        }
    }

    Clear-ReadOnlyRecursively "test/cache"

    $env:SMARTIECOINUTIL = (Resolve-Path "src/smartiecoin-util.exe").Path

    $quickTests = @(
        "rpc_masternode.py",
        "rpc_coinjoin.py"
    )

    $fullTests = @(
        "rpc_masternode.py",
        "rpc_coinjoin.py",
        "feature_masternode_params.py",
        "feature_dip3_deterministicmns.py --descriptors"
    )

    $tests = if ($Profile -eq "full") { $fullTests } else { $quickTests }

    $failed = @()

    foreach ($test in $tests) {
        Reset-FunctionalCache

        $runnerArgs = @(
            "test/functional/test_runner.py",
            "-j", "1",
            "-u",
            "--combinedlogslen=0",
            $test
        )

        Write-Host "Running smoke test: $test"
        & py -3 @runnerArgs
        if ($LASTEXITCODE -ne 0) {
            $failed += $test
            if (-not $ContinueOnFailure) {
                break
            }
        }
    }

    if ($failed.Count -gt 0) {
        Write-Host "Smoke failed for: $($failed -join ', ')"
        exit 1
    }

    Write-Host "Smoke suite passed."
    exit 0
}
finally {
    Pop-Location
}
