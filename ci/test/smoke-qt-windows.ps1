param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryDir
)

$ErrorActionPreference = 'Stop'
$BinaryDir = (Resolve-Path $BinaryDir).Path
$gui = Join-Path $BinaryDir 'smartiecoin-qt.exe'
$cli = Join-Path $BinaryDir 'smartiecoin-cli.exe'
$walletTool = Join-Path $BinaryDir 'smartiecoin-wallet.exe'
$datadir = Join-Path $env:RUNNER_TEMP ('smt-qt-smoke-' + [guid]::NewGuid().ToString('N'))
$walletDir = Join-Path $datadir 'wallets'
$walletPath = Join-Path $walletDir 'smoke'
$rpcPort = '19394'
$process = $null

if (!(Test-Path $gui) -or !(Test-Path $cli) -or !(Test-Path $walletTool)) {
    throw "Missing Qt, CLI, or wallet tool binary in $BinaryDir"
}
New-Item -ItemType Directory -Path $datadir | Out-Null
New-Item -ItemType Directory -Path $walletDir | Out-Null
$walletOutput = & $walletTool "-datadir=$datadir" "-wallet=$walletPath" create 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "Could not create isolated smoke wallet: $($walletOutput -join ' ')"
}

$cliBase = @(
    '-regtest',
    "-datadir=$datadir",
    "-rpcport=$rpcPort",
    '-rpcuser=smoke',
    '-rpcpassword=smoke'
)
function Invoke-SmokeCli([string[]]$ExtraArgs) {
    $allArgs = @($script:cliBase) + @($ExtraArgs)
    $output = & $script:cli @allArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "smartiecoin-cli failed ($LASTEXITCODE): $($output -join ' ')"
    }
    return $output
}

try {
    $guiArgs = @(
        '-regtest',
        ('-datadir="' + $datadir + '"'),
        ('-walletdir="' + $walletDir + '"'), '-wallet=smoke',
        '-server', '-listen=0', '-dnsseed=0', '-connect=0', '-discover=0',
        '-upnp=0', '-natpmp=0', "-rpcport=$rpcPort",
        '-rpcuser=smoke', '-rpcpassword=smoke'
    )
    # Do not redirect child output here: Start-Process redirection can block
    # until the GUI exits. Readiness is checked through the isolated RPC port.
    $process = Start-Process -FilePath $gui -ArgumentList $guiArgs -PassThru -WindowStyle Hidden

    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        if ($process.HasExited) {
            throw "smartiecoin-qt exited early with code $($process.ExitCode)"
        }
        try {
            $null = Invoke-SmokeCli @('getblockcount')
            $ready = $true
            break
        } catch {
            Start-Sleep -Seconds 2
        }
    }
    if (!$ready) { throw 'Qt RPC did not become ready within 60 seconds' }

    # The wallet file is pre-created in this disposable datadir and explicitly
    # loaded by Qt at startup. This exercises WalletModel construction and its
    # Boost signal connections—the path implicated by the v0.5.1 report.
    $null = Invoke-SmokeCli @('-rpcwallet=smoke', 'getwalletinfo')
    $address = (Invoke-SmokeCli @('-rpcwallet=smoke', 'getnewaddress') | Select-Object -Last 1).Trim()
    if ([string]::IsNullOrWhiteSpace($address)) { throw 'Qt smoke wallet returned no address' }
    $null = Invoke-SmokeCli @('generatetoaddress', '2', $address)
    $height = (Invoke-SmokeCli @('getblockcount') | Select-Object -Last 1).Trim()
    if ($height -ne '2') { throw "Expected regtest height 2, got '$height'" }

    $null = Invoke-SmokeCli @('stop')
    if (!$process.WaitForExit(45000)) { throw 'Qt did not shut down after RPC stop' }

    $logPath = Join-Path $datadir 'regtest\debug.log'
    if (!(Test-Path $logPath)) { throw "Missing Qt smoke log: $logPath" }
    $log = Get-Content -Raw $logPath
    if ($log -notmatch 'Shutdown: done') { throw 'Qt smoke did not log a clean shutdown' }
    if ($log -match '(?i)assertion failed|px\s*!=\s*0') { throw 'Qt smoke log contains the reported assertion' }

    Write-Host "Qt wallet-model smoke passed: regtest height $height; clean shutdown."
} finally {
    if ($null -ne $process -and !$process.HasExited) {
        try { $null = Invoke-SmokeCli @('stop') } catch {}
        $null = $process.WaitForExit(10000)
    }
}