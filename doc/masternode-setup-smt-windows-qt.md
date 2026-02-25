# Smartiecoin Masternode on Windows (Qt-first, very simple)

This is the simplest Windows guide possible.
No PowerShell scripts.
Mostly Qt clicks, plus 2 required console commands.

Required commands:

1. `bls generate`
2. `protx register_fund ...`

## Quick network values

- Regular MN collateral: `15000 SMT`
- Evo MN collateral: `75000 SMT`
- Mainnet port: `8383`

## Step 1. Requirements

- Smartiecoin Qt `v0.0.4` fully synced.
- Enough balance for collateral + fee.
- Stable public IP and port `8383` open.

## Step 2. Create addresses (Qt only)

In the **Receive** tab, create 5 new addresses:

- `mn_collateral`
- `mn_owner`
- `mn_voting`
- `mn_payout`
- `mn_fee`

Save all 5 addresses.

## Step 3. Open Qt Console

In Qt open:

- `Window -> Console`

If needed, use:

- `Help -> Debug window -> Console`

## Step 4. Command 1 (BLS keypair)

Run:

```text
bls generate
```

It returns:

- `secret`
- `public`

Save both values.

## Step 5. Put BLS secret in config

Open:

`%APPDATA%\SmartiecoinCore\smartiecoin.conf`

Use this template (replace IP and BLS secret):

```ini
server=1
listen=1
port=8383
externalip=YOUR_PUBLIC_IP:8383
txindex=1
prune=0
masternodeblsprivkey=YOUR_BLS_SECRET
disablewallet=0
```

Save and restart Smartiecoin Qt.

## Step 6. Command 2 (full registration)

Run this template in Qt Console (replace all placeholders):

```text
protx register_fund "MN_COLLATERAL_ADDR" "[\"YOUR_PUBLIC_IP:8383\"]" "MN_OWNER_ADDR" "BLS_PUBLIC_KEY" "MN_VOTING_ADDR" 0 "MN_PAYOUT_ADDR" "MN_FEE_ADDR" true
```

Fields:

- `MN_COLLATERAL_ADDR`: your `mn_collateral` address
- `YOUR_PUBLIC_IP`: your node public IP
- `MN_OWNER_ADDR`: your `mn_owner` address
- `BLS_PUBLIC_KEY`: `public` value from step 4
- `MN_VOTING_ADDR`: your `mn_voting` address
- `MN_PAYOUT_ADDR`: your `mn_payout` address
- `MN_FEE_ADDR`: your `mn_fee` address

This command creates collateral and registers the masternode in one transaction.

## Step 7. Optional verification

Run:

```text
masternode status
```

If status is healthy, your MN is registered.

## Ultra short summary

1. Create 5 addresses in Receive.
2. Run `bls generate`.
3. Put `secret` in `smartiecoin.conf` and restart Qt.
4. Run `protx register_fund ...`.
