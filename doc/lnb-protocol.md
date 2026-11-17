# Protocole du contrôleur LNB (dvbrx-lnb)

`dvbrx` pilote l'alimentation du LNB, le 22 kHz et le DiSEqC à travers un
contrôleur externe (ESP32, firmware LNB-Driver), joint :

- par USB, en série : `-L serial:/dev/ttyACM0` (115200 bauds, 8N1 ; sans effet
  sur l'USB-Serial-JTAG de l'ESP32-C6) ;
- par le réseau, en UDP : `-L udp:HÔTE[:PORT]`, port 5199 par défaut. Le
  contrôleur s'annonce en mDNS (`_dvbrx-lnb._udp`, TXT `outputs=1 proto=1`) ;
  `HÔTE` peut être `lnb-driver.local` si le système résout les noms mDNS
  (`libnss-mdns`), sinon une adresse IP (`avahi-resolve -n lnb-driver.local`).
  Le nom est résolu à nouveau à chaque reconnexion.

La configuration du contrôleur (Wi-Fi, mise à jour, MQTT…) passe par sa propre
API HTTP (`http://lnb-driver.local/`), pas par ce protocole.

## Format

- Une commande par ligne, en ASCII, terminée par `\n` (`\r` ignoré), ou une
  par datagramme en UDP. Mots-clés insensibles à la casse.
- Réponse sur une ligne : `OK [données]` si la commande est exécutée,
  `ERR <message>` sinon (`ERR unknown command`, `ERR bad hex`,
  `ERR bad length`, `ERR unsupported`, `ERR no such output`).
- Numéro de séquence (protocole 1) : `#<n> ` en tête de la commande, renvoyé
  en tête de la réponse (`#42 VOLT 18` → `#42 OK`). `dvbrx` ne prend que la
  réponse qui porte le numéro de sa dernière commande et **ignore toute autre
  ligne** : journaux du firmware (préfixés `# `, dièse et espace), messages de
  la ROM au démarrage, réponse arrivée trop tard.
- UDP : sans réponse, `dvbrx` renvoie le même datagramme (même numéro) toutes
  les 300 ms ; le contrôleur garde la dernière réponse par (adresse, port, n)
  et la renvoie sans exécuter la commande une deuxième fois.
- `dvbrx` attend la réponse 1 s au plus ; sans réponse, l'application DVB
  reçoit une erreur (EIO).
- Le contrôleur répond une fois la commande **appliquée** (GPIO commuté, trame
  DiSEqC émise) : les applications (tvheadend, dvbv5-zap…) gèrent elles-mêmes
  les délais entre commandes.
- Sortie : préfixe optionnel `@N ` après le numéro (`#7 @0 VOLT 13`), sortie 0
  par défaut. `dvbrx` utilise la sortie 0.

## Ouverture

1. `dvbrx` ouvre le port série **sans toucher à DTR ni RTS** : sur
   l'USB-Serial-JTAG de l'ESP32-C6, RTS actif avec DTR inactif redémarre la
   puce ; Linux les lève tous les deux à l'ouverture et les baisse ensemble à
   la fermeture, sans redémarrage.
2. `HELLO` (sans numéro), répété toutes les 250 ms pendant 3 s (l'ESP répond
   environ 1 s après un redémarrage) :

       HELLO
       OK dvbrx-lnb 1 1 volt13,volt18,tone,burst,diseqc

   Version du protocole, nombre de sorties, capacités séparées par des
   virgules : `volt13`, `volt18`, `off` (coupure de l'alimentation), `tone`,
   `burst`, `diseqc`, et à venir `vmes` (tension mesurée), `ovc`
   (surintensité), `diseqc-rx` (réponses DiSEqC 2.x).
   Un contrôleur qui répond `ERR` ou autre chose est un contrôleur simple
   (protocole 0) : commandes sans numéro, toutes les capacités supposées.
   Sans réponse, `dvbrx` redemande avant la commande suivante.
3. `STATUS` donne l'état actuel :

       #1 STATUS
       #1 OK volt=13 tone=1

   Clé absente = inconnue (`vmes=13.6`, `ovc=0` si le matériel le permet).
   `dvbrx` le redemande avant de refuser une commande à un adaptateur qui
   partage le LNB, car la page web, MQTT ou Zigbee du contrôleur ont pu
   changer l'état.

Si le port série disparaît (ESP redémarré, USB réénuméré), `dvbrx` le rouvre,
refait `HELLO` et renvoie la commande.

## Commandes

| Commande | Effet |
|---|---|
| `VOLT 13` | 13 V (polarisation verticale / circulaire droite) |
| `VOLT 18` | 18 V (polarisation horizontale / circulaire gauche) |
| `VOLT OFF` | alimentation coupée ; `ERR unsupported` sans la capacité `off` |
| `TONE ON` | 22 kHz continu (bande haute d'un LNB universel) |
| `TONE OFF` | 22 kHz coupé |
| `BURST A` / `BURST B` | tone burst (mini-DiSEqC, satellite A/B) |
| `DISEQC e0 10 38 f0` | trame DiSEqC 1.x, 3 à 6 octets en hexadécimal |
| `DISEQC?` | réponse DiSEqC 2.x (capacité `diseqc-rx`, sinon `ERR unsupported`) |
| `HELLO` | version, sorties, capacités |
| `STATUS` | état : `volt=`, `tone=`, éventuellement `vmes=`, `ovc=` |

`VOLT` et `TONE` sont des primitives brutes : elles ne déclenchent pas le
renvoi automatique du DiSEqC que fait l'API web du contrôleur (c'est le noyau
ou tvheadend qui séquencent). Pendant une trame DiSEqC ou un burst, le
contrôleur coupe le 22 kHz continu et le rétablit ensuite.

Quand le contrôleur ne sait pas couper l'alimentation (pas de `off`), `dvbrx`
répond 0 à l'application pour `FE_SET_VOLTAGE(SEC_VOLTAGE_OFF)` (tvheadend
l'envoie à chaque mise en veille) sans rien envoyer : le LNB reste alimenté à
la dernière tension.

Séquence typique pour un LNB universel, bande basse, polarisation verticale,
derrière un commutateur DiSEqC (port 1) :

    #2 VOLT 13          →  #2 OK
    #3 TONE OFF         →  #3 OK
    #4 DISEQC e0 10 38 f0  →  #4 OK

Mesuré sur l'ESP32-C6 par USB : VOLT et TONE 3 à 5 ms, DISEQC (4 octets)
88 ms (15 ms de silence, trame, 15 ms de silence), BURST 45 ms.

## Rappels matériels

- Le 13/18 V et le 22 kHz sont injectés sur le câble coaxial côté LNB ; le
  port RX de la radio doit être protégé par un **bloqueur de continu** (DC
  block, ou le té de polarisation passif placé avant la radio).
- Contrôleur LNB-Driver : ESP32-C6, élévateur XL6009 à 21 V puis LM317
  (13,8 / 18,5 V par un transistor sur ADJ), 22 kHz, burst et DiSEqC 1.0 par
  le RMT. Une sortie, pas de coupure, pas de mesure de tension ni de courant.
  Firmware et documentation : projet LNB-Driver (`fw/README.md`, section
  « Protocole de commande dvbrx-lnb »).
