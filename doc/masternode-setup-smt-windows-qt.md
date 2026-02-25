# Smartiecoin Masternode en Windows (Qt casi 100%)

Esta guia es la version mas simple posible para Windows.
No usa PowerShell ni scripts.
Solo usa Smartiecoin Qt + 2 comandos en la consola interna.

Comandos obligatorios:

1. `bls generate`
2. `protx register_fund ...`

## Datos rapidos

- Collateral MN regular: `15000 SMT`
- Collateral MN Evo: `75000 SMT`
- Puerto mainnet: `8383`

## Paso 1. Lo que necesitas

- Smartiecoin Qt `v0.0.4` sincronizada.
- Fondos para collateral + fee.
- IP publica fija y puerto `8383` abierto.

## Paso 2. Crear direcciones (solo clicks)

En la pestana **Receive**, crea 5 direcciones nuevas:

- `mn_collateral`
- `mn_owner`
- `mn_voting`
- `mn_payout`
- `mn_fee`

Guarda esas 5 direcciones en un bloc de notas.

## Paso 3. Abrir consola de Qt

En Qt abre:

- `Window -> Console`

Si no aparece, usa:

- `Help -> Debug window -> Console`

## Paso 4. Comando 1 (BLS)

Pega en consola:

```text
bls generate
```

Te devolvera:

- `secret`
- `public`

Guarda ambos valores.

## Paso 5. Poner el secret en el .conf

Abre este archivo:

`%APPDATA%\SmartiecoinCore\smartiecoin.conf`

Deja esto (cambiando solo tu IP y el BLS secret):

```ini
server=1
listen=1
port=8383
externalip=TU_IP_PUBLICA:8383
txindex=1
prune=0
masternodeblsprivkey=TU_BLS_SECRET
disablewallet=0
```

Guarda y reinicia Smartiecoin Qt.

## Paso 6. Comando 2 (registro completo)

Vuelve a la consola y pega esta plantilla, cambiando los campos:

```text
protx register_fund "MN_COLLATERAL_ADDR" "[\"TU_IP_PUBLICA:8383\"]" "MN_OWNER_ADDR" "BLS_PUBLIC_KEY" "MN_VOTING_ADDR" 0 "MN_PAYOUT_ADDR" "MN_FEE_ADDR" true
```

Que va en cada campo:

- `MN_COLLATERAL_ADDR`: direccion `mn_collateral`
- `TU_IP_PUBLICA`: IP de tu nodo
- `MN_OWNER_ADDR`: direccion `mn_owner`
- `BLS_PUBLIC_KEY`: valor `public` del paso 4
- `MN_VOTING_ADDR`: direccion `mn_voting`
- `MN_PAYOUT_ADDR`: direccion `mn_payout`
- `MN_FEE_ADDR`: direccion `mn_fee`

Este comando crea el collateral y registra el masternode en una sola transaccion.

## Paso 7. Revisar si quedo activo

Opcional en consola:

```text
masternode status
```

Si todo esta bien, tu MN ya queda registrado.

## Resumen ultra corto

1. Crear 5 direcciones en Receive.
2. Ejecutar `bls generate`.
3. Pegar `secret` en `smartiecoin.conf` y reiniciar Qt.
4. Ejecutar `protx register_fund ...`.
