# Protocole du contrôleur LNB (ESP32)

`dvbrx` pilote l'alimentation du LNB, le 22 kHz et le DiSEqC à travers un
contrôleur externe (ESP32), joint par port série USB (`-L serial:/dev/ttyACM0`,
115200 bauds, 8N1) ou en UDP (`-L udp:HOTE:PORT`).

## Format

- Une commande par ligne, en ASCII, terminée par `\n`.
- Le contrôleur répond par une ligne : `OK` si la commande est exécutée,
  `ERR <message>` sinon. En UDP, une commande par datagramme, une réponse par
  datagramme.
- `dvbrx` attend la réponse 1 s au plus ; sans réponse, l'application DVB
  reçoit une erreur. Le contrôleur ne répond qu'une fois la commande
  **réellement appliquée** (tension stabilisée, trame DiSEqC émise) : les
  applications (tvheadend, dvbv5-zap…) gèrent elles-mêmes les délais entre
  commandes et comptent sur ce comportement synchrone.

## Commandes

| Commande | Effet |
|---|---|
| `VOLT 13` | alimentation 13 V (polarisation verticale / circulaire droite) |
| `VOLT 18` | alimentation 18 V (polarisation horizontale / circulaire gauche) |
| `VOLT OFF` | alimentation coupée |
| `TONE ON` | 22 kHz continu (bande haute d'un LNB universel) |
| `TONE OFF` | 22 kHz coupé |
| `BURST A` / `BURST B` | tone burst (mini-DiSEqC, satellite A/B) |
| `DISEQC e0 10 38 f0` | trame DiSEqC 1.x, 3 à 6 octets en hexadécimal |

Une séquence typique pour un LNB universel, bande basse, polarisation
verticale, derrière un commutateur DiSEqC (port 1) :

    VOLT 13
    TONE OFF
    DISEQC e0 10 38 f0
    OK (à chaque ligne)

## Rappels matériels

- Le 13/18 V et le 22 kHz sont injectés sur le câble coaxial côté LNB ; le
  port RX du B210 doit être protégé par un **bloqueur de continu** (DC block).
- Un circuit dédié (LNBH25/LNBH26, en I2C) génère proprement tension, 22 kHz
  et DiSEqC ; l'ESP32 se contente de le configurer et de répondre `OK`.
