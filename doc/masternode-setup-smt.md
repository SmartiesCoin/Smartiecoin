# Smartiecoin Masternode Guide (Simple / Old-School Style)

This is the short version most miners want: send collateral to yourself, register, done.

Network values:

- Regular MN collateral: `15,000 SMT`
- Evo collateral: `75,000 SMT`
- Mainnet port: `8383`
- Collateral confirmations: `15`

## 1. Quick Reality Check

- You **can** run MN + wallet in the same node process (single-wallet mode).
- You **do not need** a VPS if your machine is reachable from internet (`8383/tcp` open + stable public IP).
- Smartiecoin uses deterministic masternodes (ProTx), so one registration tx is still required.

## 2. Minimal Config (Single Wallet)

Put this in `smartiecoin.conf` on the machine that will run the masternode:

```ini
server=1
daemon=1
listen=1
port=8383
externalip=YOUR_PUBLIC_IP:8383

txindex=1
prune=0

masternodeblsprivkey=PASTE_OPERATOR_SECRET_HERE
disablewallet=0
```

Then restart node/wallet.

## 3. Old-School Fast Flow (Regular MN)

Use wallet CLI:

```bash
CLI=smartiecoin-cli
WALLET=main
```

If needed, create wallet once:

```bash
$CLI createwallet "$WALLET" || true
```

### Step A: Generate keys and addresses

```bash
BLS_JSON=$($CLI bls generate)
OPERATOR_SECRET=$(echo "$BLS_JSON" | jq -r '.secret')
OPERATOR_PUB=$(echo "$BLS_JSON" | jq -r '.public')

COLLATERAL_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_collateral")
OWNER_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_owner")
VOTING_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_voting")
PAYOUT_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_payout")
FEE_ADDR=$($CLI -rpcwallet=$WALLET getnewaddress "mn_fee")
```

Put `OPERATOR_SECRET` into `smartiecoin.conf` as `masternodeblsprivkey=...` and restart once.

### Step B: Send collateral to yourself

```bash
TXID=$($CLI -rpcwallet=$WALLET sendtoaddress "$COLLATERAL_ADDR" 15000)
echo "$TXID"
```

Wait for at least `15` confirmations.

### Step C: Get collateral outpoint

```bash
OUTPOINT=$($CLI -rpcwallet=$WALLET masternode outputs | jq -r '.[0]')
COLL_TXID="${OUTPOINT%-*}"
COLL_VOUT="${OUTPOINT##*-}"
echo "$COLL_TXID $COLL_VOUT"
```

### Step D: Register masternode

```bash
MN_IP="YOUR_PUBLIC_IP"

PROTX_HASH=$($CLI -rpcwallet=$WALLET protx register \
"$COLL_TXID" "$COLL_VOUT" "[\"$MN_IP:8383\"]" \
"$OWNER_ADDR" "$OPERATOR_PUB" "$VOTING_ADDR" \
0 "$PAYOUT_ADDR" "$FEE_ADDR" true)

echo "$PROTX_HASH"
```

### Step E: Verify

```bash
$CLI protx info "$PROTX_HASH"
$CLI masternode list status
$CLI masternode status
```

If status is not valid yet, give it a few blocks.

## 4. Even Shorter Alternative (No Manual `masternode outputs`)

If you prefer one-call funding + registration, use:

```bash
$CLI -rpcwallet=$WALLET protx register_fund \
"$COLLATERAL_ADDR" "[\"$MN_IP:8383\"]" \
"$OWNER_ADDR" "$OPERATOR_PUB" "$VOTING_ADDR" \
0 "$PAYOUT_ADDR" "$FEE_ADDR" true
```

This auto-creates collateral in the same operation.

## 5. Evo MN (If Needed)

Evo is still available in `v0.0.4`:

- `protx register_evo`
- `protx register_fund_evo`

Evo collateral is `75,000 SMT`.

## 6. Top 5 Mistakes

- Collateral is not exactly `15000 SMT`.
- Less than `15` confirmations.
- `masternodeblsprivkey` does not match operator public key used in ProTx.
- Port `8383` is closed.
- Node not reachable at `externalip`.
