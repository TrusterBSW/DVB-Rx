# Interface de contrôle de dvbrx (pour dvbrx-web)

`dvbrx` en mode pont (`-d N`) ouvre un socket Unix de type flux,
`/run/dvbrx/dvbrx-N.sock` (option `-C CHEMIN` pour en changer). Les droits sont 0660,
le groupe est celui du service (`video`). Le protocole est le suivant :

- **Requêtes** : un objet JSON par ligne, terminé par `\n`.
- **Réponses** : un objet JSON par ligne, avec `"ok": true` ou bien `"ok": false, "error": "..."`.
- **Connexions** : plusieurs requêtes peuvent passer sur la même connexion. Plusieurs clients peuvent être connectés en même temps.
- **Unités** : les fréquences sont en Hz. Pour le satellite, c'est la **fréquence
  intermédiaire (FI)** vue par le B210 : dvbrx ne connaît pas l'oscillateur du LNB. C'est la page web qui fait la conversion.
- **Commandes non exclusives** : `status` et `constellation` sont des lectures. Elles fonctionnent pendant qu'une
  application regarde une chaîne, sans la perturber ni la verrouiller.
- **Entrée du B210** : `capture`, `probe` et `monitor start` acceptent un champ facultatif `"antenna"`
  (`"TX/RX"`, `"RX2"`), quand le satellite et le câble arrivent sur deux entrées différentes. Par défaut, c'est l'entrée du service.
- **Commandes exclusives** : `capture`, `probe` et `monitor start` prennent la radio. Elles échouent avec
  `"error": "busy"` quand une application (tvheadend…) a un accord en cours. On peut forcer avec
  `"force": true` : l'accord en cours est alors interrompu, et il reprendra à la
  prochaine demande de l'application.

## status

Requête : `{"cmd": "status"}`

```json
{"ok": true,
 "radio": {"source": "uhd", "model": "B210", "serial": "31A5B7C",
           "rate_max": 56000000, "multi": true,
           "freq": 586000000, "rate": 10240000, "bandwidth": 9020000,
           "gain": 50, "agc": "level|search|best|manual", "level_dbfs": -24.1,
           "input_dbm": -74.1, "overflows": 0, "filled": 0,
           "lnb": {"present": false, "volt": "off", "tone": false}},
 "channels": [
   {"adapter": 0, "state": "idle|tuned|probe|monitor",
    "delsys": "DVB-T2", "freq": 586000000,
    "symbol_rate": 0, "bandwidth": 8000000, "rolloff": 0,
    "locked": true, "snr_db": 25.6, "freq_offset_hz": 13100,
    "modulation": "QAM256", "code_rate": "5/6", "pilots": null,
    "fft": 32768, "guard": "1/128", "plp": 0,
    "ts_packets": 1269290, "ts_errors": 19, "ts_mbps": 47.4,
    "fec_blocks": 35552, "fec_errors": 230, "uptime": 39.1}]}
```

- **`input_dbm`** : estimation de la puissance d'entrée avant l'amplification, calculée comme
  `level_dbfs − gain + offset`. L’offset vaut 0 dB par défaut et se règle avec `-P DB` ; ce n'est pas une mesure calibrée.
- **`ts_mbps`** : débit TS utile, calculé sur les dernières secondes.
- **`source`** : `uhd`, `bladerf`, `hackrf` ou `file`. **`model`**, **`serial`** : modèle et
  numéro de série de la carte (`""` tant qu'elle n'est pas ouverte). **`rate_max`** : débit d'échantillonnage
  maximal de la radio (20 MS/s pour un HackRF) ; le scan en tient compte. **`multi`** : faux
  quand la radio ne sait servir qu'un canal (HackRF).
- **Champs non pertinents** pour la norme : à `null`.

## constellation

Requête : `{"cmd": "constellation", "adapter": 0, "points": 2048}`

```json
{"ok": true, "modulation": "QAM256", "points": [[0.12, -0.83], ...]}
```

- **Points** : symboles reçus après égalisation et correction de porteuse, normalisés à une
  puissance moyenne de 1. Pour le DVB-T2, ce sont les cellules de données avant la dérotation éventuelle : une
  constellation tournée apparaît tournée.
- **Coût** : la capture n'est active que 5 s après chaque requête. Hors de cette fenêtre, elle ne coûte rien. La page doit la
  redemander environ toutes les 500 ms tant que l'onglet est ouvert.
- **Canal non verrouillé ou sans symboles** : `"points": []`.

## capture (radio libre)

Requête : `{"cmd": "capture", "freq": 602000000, "rate": 56000000, "samples": 65536,
"gain": 40, "bandwidth": 0}`

```json
{"ok": true, "freq": 602000000, "rate": 56000000, "gain": 40, "level_dbfs": -21.3,
 "iq": "<base64 de float32 entrelacés I,Q, little-endian>"}
```

- **Échantillons** : 1 048 576 au plus.
- **`gain`** : `"auto"` vise environ −24 dBFS.
- **`bandwidth`** : 0 laisse le filtre analogique ouvert sur toute la capture.
- **Usage** : c'est la brique du scan rapide (FFT par fenêtres).

## probe (radio libre)

Requête :

```json
{"cmd": "probe", "delsys": "DVB-C", "freq": 602000000, "symbol_rate": 6875000,
 "bandwidth": 0, "rolloff": 0.15, "modulation": "auto", "plp": -1, "seconds": 6}
```

- **Normes** : `DVB-S`, `DVB-S2` (S2X compris), `DVB-C`, `DVB-T`, `DVB-T2`.
- **Paramètres** : pour le DVB-T/T2, `bandwidth` en Hz (8000000…) ; pour les autres, `symbol_rate`.
- **Déroulement** : dvbrx accorde un récepteur pendant `seconds` (AGC et filtre automatiques). Il renvoie l'état et les
  tables du flux :

```json
{"ok": true, "locked": true, "delsys": "DVB-C", "symbol_rate": 6875000, "bandwidth": 0,
 "rolloff": 0, "plp": -1, "pids": [[0, 3012, 0], [18, 1450, 0], [101, 412833, 411902]],
 "snr_db": 31.0, "freq_offset_hz": 900,
 "modulation": "QAM64", "code_rate": null, "ts_mbps": 36.2, "ts_errors": 17,
 "gain": 37, "level_dbfs": -24.6,
 "tsid": 4500, "onid": 64810, "network": "",
 "services": [{"sid": 4501, "name": "13EME RUE HD", "provider": "Amagi",
               "type": 25, "scrambled": true}],
 "nit": [{"delsys": "DVB-C", "freq": 650000000, "symbol_rate": 6875000,
          "modulation": "QAM64", "tsid": 4500}]}
```

- **`pids`** : `[pid, paquets, paquets brouillés]` de chaque PID vu. Un flux fait uniquement des PID
  0x1FFE/0x1FFF est du DOCSIS. `pilots` (booléen) n'apparaît qu'en DVB-S2.
- **`nit`** : multiplex annoncés par la NIT s'il y en a une, sinon `[]`.
- **`scrambled`** : vrai si les paquets vidéo et audio du service sont brouillés, quoi qu'en dise la SDT.

## lnb

Requête : `{"cmd": "lnb", "volt": "13|18|off", "tone": true}`

Réponse : `{"ok": true}`, ou `"error": "no LNB controller"` sans `-L`. Commande exclusive :
`"error": "busy"` quand une application est accordée (changer la polarisation couperait sa
réception), sauf avec `"force": true`.

## monitor (radio libre, pour l'alignement)

Démarrage :

```json
{"cmd": "monitor", "action": "start", "delsys": "DVB-S2", "freq": 1200000000,
 "symbol_rate": 27500000, "rolloff": 0.2}
```

Terrestre (antenne TNT) : `{"cmd": "monitor", "action": "start", "delsys": "DVB-T", "freq": 586000000,
"bandwidth": 8000000, "plp": -1}` (`DVB-T2` : `plp` -1 pour le premier PLP).

Arrêt : `{"cmd": "monitor", "action": "stop"}`

- **Fonctionnement** : la commande garde un récepteur sur cette porteuse jusqu'à `stop`. Il est visible dans `status` comme
  canal `"state": "monitor"`, avec `level_dbfs`, `input_dbm`, `gain` et SNR.
- **Symbole inconnu** : avec `"symbol_rate": 0`, seul le niveau est mesuré (puissance dans ±2 MHz).
- **Fin automatique** : 30 s sans requête `status` arrêtent le moniteur, pour qu'une page fermée ne garde pas la radio.
