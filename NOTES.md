# Dexcom G7 / One+ / Stelo — BLE protocol

One protocol across G7, One+ and Stelo; they differ only by advertised name.

## Identity

- Name prefix selects product line: `DXCM…` = G7, `DX01…` = Stelo.
- Peripheral BLE address is random-static and rotates each sensor session.
- Credentials: a 4-digit pairing code and a data-matrix (certificate material),
  both on the applicator.

## GATT

Service `f8083532-849e-531c-c594-30f1f86a4ea5`. Characteristics share the suffix
`-849e-531c-c594-30f1f86a4ea5`:

| UUID prefix | Role                         | CCCD    |
|-------------|------------------------------|---------|
| `f8083534`  | Control / glucose (notify)   | control commands, EGV |
| `f8083535`  | Authentication (indicate)    | `0A/0B/02/03/04/05/0C/0D` |
| `f8083536`  | Backfill                     | historic records |
| `f8083538`  | J-PAKE round transport (WNR) | 160-byte packets |

## Connection sequence

1. Enable CCCDs: auth char (indicate, `0200`) + round-transport char (notify, `0100`).
2. Fresh pairing: J-PAKE rounds (`0A`). Bonded reconnect: skip the rounds.
3. Challenge-response auth (`02/03/04/05`) — every connection.
4. If AuthStatus `auth=02` (bond not yet established): certificate exchange, then
   write `06 1E` (time-extended). A bonded `auth=01` skips straight to step 5.
5. Enable CCCDs: control char (indicate, `0200`) + backfill char (notify, `0100`).
6. Get-data: write `4e` to the control char. The current EGV returns as a `4e`
   indication on the control char; further EGV/backfill records stream as
   notifications on the backfill char. (The official app also issues `32`,
   `ea00`, and other control queries; `4e` alone yields the current reading.)
7. Backfill request: `59 <startU32LE> <endU32LE>`.

## Authentication

Curve secp256r1 (P-256), SHA-256.

### First pairing — EC-J-PAKE

- Password = the 4 pairing-code bytes as a big-endian bignum, mod n.
- Schnorr (RFC-8235) signer identities (ASCII; phone signs "client", sensor "server"):
  - client = `63 6c 69 65 6e 74` ("client")
  - server = `73 65 72 76 65 72` ("server")
- ZKP hash = `SHA256( L|G  L|V  L|X  L|id )`; `L` = 4-byte big-endian length; each
  point uncompressed 65 bytes (`04|X|Y`); `V = g^v`; `X` = public key; mod n.
  Proof scalar `r = (v − H·x) mod n`. Verify: `g^r + X^H == V`.
- Round packet (160 bytes): `pub1(64) | pub2(64) | proof(32)`; points bare `X|Y`
  (no `04`). Written to `f8083538` in 20-byte WRITE_NO_RESPONSE chunks.
- Auth opcodes on the auth char: `0A` (J-PAKE round), `0B` (certificate),
  `0C`/`0D` (key challenge). Round `0A <n>`: the sensor sends its 160-byte packet
  as 8x20-byte notifications and the phone writes its 160-byte packet as 8x20-byte
  Write-Commands on the round-transport char.
- `sharedKey` (16 bytes) = `SHA256(x)[:16]`, `x` = affine X of the J-PAKE ECDH
  point. Persisted; reused on reconnect (reconnect skips the rounds).

### Certificate exchange — REQUIRED to establish a streamable bond

After J-PAKE + the 02/03/04/05 auth, a fresh pairing must complete a certificate
exchange before the sensor will stream. Verified on hardware: without it,
reconnects return AuthStatus `auth=02` and never stream; after it, reconnects
return `auth=01` and stream. Flow (all `0x...` on the auth char; bulk data on the
round-transport char in 20-byte chunks):

```
for cert in {0,1}:
  phone -> 0B <idx> <ourCertLen u32 LE>           announce our cert
  sensor-> 0B 00 <idx> <sensorCertLen u32 LE>     sensor's cert size
  sensor-> <sensorCert bytes>          (round char; note: a few chunks may
                                        arrive BEFORE the size announce)
  phone -> <ourCert bytes>             (round char, after receiving sensor's)
phone -> 0C <random16>                            key-challenge announce
sensor-> <64 bytes> (round char) + 0C 00 <challenge16> (auth char)
phone -> <signature 64 bytes> (round char) + 0D 00 02 (auth char)
sensor-> 0D 00 00 ...                              accepted
phone -> 06 1E                                     time-extended -> get data
```

- The two collector certs are static (embedded). The signature is
  ECDSA-P256 over `SHA256(challenge[2:18])` with an embedded device key,
  output raw `r|s` (64 bytes).
- A bonded reconnect (auth=01) skips both the rounds and the certificate
  exchange and goes straight to get-data.

### Every connection — challenge-response

```
phone -> sensor  02 <token8> 02               AuthRequest
sensor -> phone  03 <tokenHash8> <challenge8>  AuthChallenge
phone -> sensor  04 <challengeHash8>           ChallengeReply
sensor -> phone  05 <auth> <bond>              AuthStatus
```
`auth` = 01 means fully bonded → stream directly. `auth` = 02 means the bond is
not yet established → do the certificate exchange (above), then stream. `bond` = 01.

```
dex8(key16, data8) = AES-128-ECB(key16, data8 || data8)[:8]
tokenHash     = dex8(sharedKey, token)
challengeHash = dex8(sharedKey, challenge)
```

## Data

Realtime and backfill streams are unencrypted; the AES/sharedKey gates access
only and is never applied to data.

### EGV / backfill record (9 bytes)

```
[0:4]  uint32 LE  timestamp, seconds since session start
[4:6]  uint16 LE  glucose, mg/dL
[6:9]  status / trend (3 bytes)
```

Records spaced 300 s. Backfill records stream as notifications after a
`59 <startU32LE> <endU32LE>` request, which echoes the requested range.

### Control responses (indications)

`4e` current-EGV indication (>= 19 bytes):

```
[0]      0x4e
[1]      status
[2:6]    uint32 LE  clock (seconds since session start)
[6:8]    uint16 LE  sequence
[8:10]   (unused)
[10:12]  uint16 LE  age (seconds since this reading was taken)
[12:14]  uint16 LE  glucose: low 12 bits = mg/dL, high nibble = display-only flag
[14]     calibration / session state
[15]     int8       trend (÷10 = mg/dL/min; 127 = unavailable)
[16:18]  uint16 LE  predicted glucose (low 10 bits)
```

Other control responses: `32` session/status, `ea` status/battery,
`59` backfill acknowledgement (echoes the requested range).

## Test vectors

```
dex8:      key=6f8326744bef03faa520ad9c5cff673f  data=2a404290c4b63b01
           -> 13ab13f6975e3082
sharedKey: J-PAKE, pass="1155"  -> 6f8326744bef03faa520ad9c5cff673f
```
