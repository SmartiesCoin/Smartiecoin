# Smartiecoin Masternode Setup Guide (SMT)

This guide explains how to deploy a **Regular Masternode** on Smartiecoin.

It is written for the current SMT chain settings in this repository:

- Regular MN collateral: `15,000 SMT`
- Evo collateral: `75,000 SMT`
- Mainnet P2P port: `8383`
- Mainnet collateral confirmations: `15`

## 1. Architecture

Use two environments:

- **Controller wallet** (local machine): holds collateral and sends ProTx registration/update transactions.
- **Masternode server (VPS)**: runs `smartiecoind` with `-masternodeblsprivkey`.

Do not run collateral and hot masternode process in the same wallet/node.

## 2. Prerequisites

- Fully synced Smartiecoin node/wallet.
- At least `15,000 SMT` (+ fees) in the controller wallet.
- Public VPS with static IP.
- Firewall/NAT open for `8383/tcp`.
- `jq` installed on the controller machine (for easier key parsing in examples).

## 3. Create Controller Wallet and Keys

```bash
CLI=smartiecoin-cli
WALLET=mn_controller

$CLI createwallet "$WALLET"

# Generate operator BLS keys
BLS_JSON=$($CLI bls generate)
OPERATOR_SECRET=$(echo "$BLS_JSON" | jq -r '.secret')
OPERATOR_PUB=$(echo "$BLS_JSON" | jq -r '.public')

# Generate addresses
COLLATERAL_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress)
OWNER_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress)
VOTING_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress)
PAYOUT_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress)
FEE_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress)
```

Save these values securely.

## 4. Create Collateral UTXO

Send exactly `15,000 SMT` to the collateral address:

```bash
TXID=$($CLI -rpcwallet=$WALLET sendtoaddress "$COLLATERAL_ADDR" 15000)
echo "$TXID"
```

Wait until it reaches at least `15` confirmations (mainnet).

## 5. Find Collateral Outpoint

```bash
$CLI -rpcwallet=$WALLET masternode outputs
```

Expected output is an array with entries like:

`"<collateral_txid>-<vout>"`

Example parsing:

```bash
OUTPOINT=$($CLI -rpcwallet=$WALLET masternode outputs | jq -r '.[0]')
COLL_TXID="${OUTPOINT%-*}"
COLL_VOUT="${OUTPOINT##*-}"
echo "$COLL_TXID $COLL_VOUT"
```

## 6. Configure the Masternode VPS

Edit `~/.smartiecoincore/smartiecoin.conf` on the VPS:

```ini
server=1
daemon=1
listen=1
port=8383
externalip=YOUR_PUBLIC_IP:8383

txindex=1
prune=0
peerbloomfilters=1

rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcuser=smartierpc
rpcpassword=CHANGE_ME_STRONG_PASSWORD

masternodeblsprivkey=PASTE_OPERATOR_SECRET_HERE
```

Start node:

```bash
smartiecoind -daemon
smartiecoin-cli getblockcount
```

## 7. Register the Masternode (ProTx)

Run this from the controller wallet node:

```bash
MN_IP="YOUR_PUBLIC_IP"

PROTX_HASH=$($CLI -rpcwallet=$WALLET protx register \
"$COLL_TXID" "$COLL_VOUT" "[\"$MN_IP:8383\"]" \
"$OWNER_ADDR" "$OPERATOR_PUB" "$VOTING_ADDR" \
0 "$PAYOUT_ADDR" "$FEE_ADDR" true)

echo "$PROTX_HASH"
```

Notes:

- `operatorReward` is `0` in this example.
- `coreP2PAddrs` is passed as JSON array (required format).

## 8. Verify Status

On controller:

```bash
$CLI protx info "$PROTX_HASH"
$CLI masternode list status
```

On VPS:

```bash
smartiecoin-cli masternode status
```

Look for enabled/valid state and matching service address.

## 9. Update Service IP/Port (if needed)

If your VPS IP changes:

```bash
$CLI -rpcwallet=$WALLET protx update_service \
"$PROTX_HASH" "[\"NEW_IP:8383\"]" "$OPERATOR_SECRET" "" "$FEE_ADDR" true
```

## 10. Common Failure Causes

- Collateral is not exactly `15000 SMT`.
- Collateral has insufficient confirmations.
- Collateral UTXO was spent after registration.
- `masternodeblsprivkey` does not match the ProTx operator key.
- Port `8383` blocked by firewall/cloud rules.
- VPS started with `prune=1` or without `txindex=1`.

## 11. Security Checklist

- Keep `OPERATOR_SECRET` private.
- Use strong unique RPC credentials.
- Restrict RPC to localhost (`127.0.0.1`).
- Backup controller wallet and record `PROTX_HASH`.
- Monitor VPS uptime and disk space.

