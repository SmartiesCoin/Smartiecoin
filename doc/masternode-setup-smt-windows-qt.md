# Smartiecoin Masternode on Windows Qt (Minimal Commands)

This guide is for miners who want to do almost everything from the Qt wallet UI.

You will use GUI for most steps and only a few commands in **Debug Console**.

Network values:

- Regular MN collateral: `15000 SMT`
- Evo collateral: `75000 SMT`
- Mainnet port: `8383`
- Required confirmations: `15`

## 1. What you need

- Smartiecoin Qt v0.0.4 running and fully synced.
- Public IP and port `8383` open on router/firewall (or VPS).
- Wallet has at least `15000 SMT` + fees.

## 2. Create addresses from Qt (no CLI)

In **Receive** tab, create these addresses (labels suggested):

- `mn_collateral`
- `mn_owner`
- `mn_voting`
- `mn_payout`
- `mn_fee`

Save/copy all five addresses somewhere safe.

## 3. Send collateral to yourself (Qt Send tab)

In **Send** tab:

- `Pay To`: paste `mn_collateral` address
- `Amount`: `15000`
- Send transaction

Wait until this collateral tx has at least `15` confirmations.

## 4. Generate BLS key (Qt Debug Console)

Open Qt console:

- `Window -> Console` (or `Help -> Debug window -> Console`, depending build)

Run:

```text
bls generate
```

You will get:

- `secret` (operator private key)
- `public` (operator public key)

## 5. Set masternode key in `smartiecoin.conf`

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

masternodeblsprivkey=PASTE_BLS_SECRET_HERE
disablewallet=0
```

Restart Qt wallet.

## 6. Get collateral outpoint (1 command)

In Qt Debug Console run:

```text
masternode outputs
```

It returns something like:

`"txid-vout"`

Split it into:

- `COLL_TXID`
- `COLL_VOUT`

## 7. Register masternode (1 command)

Still in Debug Console, run this with your real values:

```text
protx register "COLL_TXID" COLL_VOUT "[\"YOUR_PUBLIC_IP:8383\"]" "OWNER_ADDR" "OPERATOR_PUBKEY" "VOTING_ADDR" 0 "PAYOUT_ADDR" "FEE_ADDR" true
```

Where:

- `OWNER_ADDR` = `mn_owner`
- `VOTING_ADDR` = `mn_voting`
- `PAYOUT_ADDR` = `mn_payout`
- `FEE_ADDR` = `mn_fee`
- `OPERATOR_PUBKEY` = `public` from `bls generate`

Output is your `PROTX_HASH`.

## 8. Verify (2 commands)

```text
protx info "PROTX_HASH"
masternode status
```

If not ready immediately, wait a few blocks.

## 9. Super-short command count

If you do addresses + send from Qt UI, you only need these console commands:

1. `bls generate`
2. `masternode outputs`
3. `protx register ...`
4. `protx info ...` (optional verify)
5. `masternode status` (optional verify)

## 10. Common failures

- Collateral not exactly `15000 SMT`.
- Less than `15` confirmations.
- Wrong BLS secret in `smartiecoin.conf`.
- Closed `8383` port.
- Wrong `externalip`.
