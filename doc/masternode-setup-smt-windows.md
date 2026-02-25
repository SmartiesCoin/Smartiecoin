# Smartiecoin Masternode Guide for Windows (Simple)

This is the Windows version of the old-school flow:

1. Create collateral.
2. Send `15000 SMT` to yourself.
3. Wait confirmations.
4. Register MN.

Chain values:

- Regular MN collateral: `15000 SMT`
- Evo collateral: `75000 SMT`
- Mainnet port: `8383`
- Required confirmations: `15`

## 1. Requirements

- Smartiecoin Core v0.0.4 installed on Windows.
- Wallet fully synced.
- Public IP (if running from home) and port `8383` open in router/firewall.
- `smartiecoin-cli.exe` available (from release zip).

Data dir on Windows:

`%APPDATA%\SmartiecoinCore`

Usually:

`C:\Users\<YourUser>\AppData\Roaming\SmartiecoinCore`

## 2. Configure single-wallet MN

Edit:

`%APPDATA%\SmartiecoinCore\smartiecoin.conf`

Use:

```ini
server=1
listen=1
port=8383
externalip=YOUR_PUBLIC_IP:8383

txindex=1
prune=0

masternodeblsprivkey=PASTE_OPERATOR_SECRET_HERE
disablewallet=0
```

Restart Smartiecoin Core after editing.

## 3. Open PowerShell and set variables

Open PowerShell in the folder where `smartiecoin-cli.exe` is located and run:

```powershell
$CLI = ".\smartiecoin-cli.exe"
$DATADIR = "$env:APPDATA\SmartiecoinCore"
$WALLET = "main"
```

Create wallet once (safe if it already exists):

```powershell
& $CLI -datadir="$DATADIR" createwallet $WALLET
```

If wallet already exists, continue.

## 4. Generate keys and addresses

```powershell
$bls = (& $CLI -datadir="$DATADIR" bls generate) | ConvertFrom-Json
$operatorSecret = $bls.secret
$operatorPub = $bls.public

$collateralAddr = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET getnewaddress "mn_collateral"
$ownerAddr = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET getnewaddress "mn_owner"
$votingAddr = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET getnewaddress "mn_voting"
$payoutAddr = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET getnewaddress "mn_payout"
$feeAddr = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET getnewaddress "mn_fee"

$operatorSecret
$operatorPub
```

Put `$operatorSecret` into `smartiecoin.conf` as `masternodeblsprivkey=...` and restart wallet/node.

## 5. Send collateral to yourself

```powershell
$txid = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET sendtoaddress $collateralAddr 15000
$txid
```

Wait until confirmations >= 15:

```powershell
& $CLI -datadir="$DATADIR" -rpcwallet=$WALLET gettransaction $txid
```

## 6. Get collateral outpoint

```powershell
$outpoints = (& $CLI -datadir="$DATADIR" -rpcwallet=$WALLET masternode outputs) | ConvertFrom-Json
$first = $outpoints[0]
$parts = $first -split '-'
$collTxid = $parts[0]
$collVout = [int]$parts[1]

$collTxid
$collVout
```

## 7. Register masternode

Replace public IP:

```powershell
$mnIp = "YOUR_PUBLIC_IP"

$protxHash = & $CLI -datadir="$DATADIR" -rpcwallet=$WALLET protx register `
  "$collTxid" $collVout "[`"$mnIp:8383`"]" `
  "$ownerAddr" "$operatorPub" "$votingAddr" `
  0 "$payoutAddr" "$feeAddr" $true

$protxHash
```

## 8. Verify

```powershell
& $CLI -datadir="$DATADIR" protx info $protxHash
& $CLI -datadir="$DATADIR" masternode list status
& $CLI -datadir="$DATADIR" masternode status
```

If it is not enabled immediately, wait a few blocks.

## 9. One-call alternative (optional)

If you want fewer manual steps, you can fund + register in one call:

```powershell
$mnIp = "YOUR_PUBLIC_IP"
& $CLI -datadir="$DATADIR" -rpcwallet=$WALLET protx register_fund `
  "$collateralAddr" "[`"$mnIp:8383`"]" `
  "$ownerAddr" "$operatorPub" "$votingAddr" `
  0 "$payoutAddr" "$feeAddr" $true
```

## 10. Most common errors

- Collateral is not exactly `15000 SMT`.
- Not enough confirmations.
- Wrong `masternodeblsprivkey` (does not match operator pubkey in ProTx).
- Port `8383` is closed.
- `externalip` is missing or wrong.
