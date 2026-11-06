# dvbrx-web

Interface web de supervision du récepteur SDR DVB **dvbrx** (B210, bladeRF ou HackRF exposé en
`/dev/dvb`). Résumé : `man dvbrx-web`, `dvbrx-web --help`.
Elle parle à dvbrx par son socket de contrôle (`/run/dvbrx/dvbrx-N.sock`, protocole décrit dans
`doc/dvbrx-web-api.md`). Python 3 (bibliothèque standard + numpy), page HTML/CSS/JS sans
dépendance externe ni CDN.

## Pages

| Onglet | Rôle | Prend la radio ? |
|---|---|---|
| Statut | radio (carte et numéro de série, fréquence, filtre, débit, gain, AGC, niveau, puissance estimée, débordements, LNB) et, par adaptateur, tous les paramètres du canal (norme, fréquence réelle pour le satellite, modulation, rendement, FFT/IG/PLP, SNR, décalage, débit TS, erreurs) ; rafraîchi chaque seconde | non |
| Constellation | nuage de points avec persistance ; demandé toutes les 500 ms seulement si l'onglet est affiché et la page visible | non |
| Scan rapide | FFT par fenêtres d’environ 50 MHz (48 utiles) (56 MS/s), détection des canaux (grille de 8 MHz pour le câble, porteuses pour le satellite) ; Sat ON = 4 combinaisons du LNB universel (VL, VH, HL, HH), soit 5 lignes | oui |
| Scan complet | identification de la norme (raie cyclique de \|x\|² → porteuse unique, sinon OFDM) puis `probe` ; DOCSIS reconnu (6,952 MBd sans PAT ni service) ; NIT exploitée ; export dvbv5 | oui |
| Alignement (satellite ou terrestre) | moniteur sur une porteuse satellite (combinaison du LNB réglable à la main) ou un multiplex DVB-T/T2 (fréquence ou canal UHF, largeur, PLP), grandes jauges (puissance, SNR, verrouillage), historique, son facultatif | oui |

Avec plusieurs récepteurs (un démon `dvbrxd@N` par carte), un sélecteur en haut de page les
présente avec leur carte (« dvbrx 1 — HackRF One 334c64dc39395367 ») ; scans et alignement se
font récepteur par récepteur.

Quand tvheadend utilise le récepteur, dvbrx répond `busy` : la page l'explique et propose de
forcer (l'accord en cours est interrompu).

tvheadend garde le frontal ouvert (donc la radio accordée et le décodage actif) après la fin
d'un visionnage, sauf si l'option **Power save** de ses frontaux dvbrx est cochée
(Configuration > DVB Inputs > TV adapters). Cochez-la : la radio est libérée quelques
secondes après la fin d'un visionnage, et la carte ne décode plus pour rien.

## Lancer

Avec le faux dvbrx (aucun matériel nécessaire) :

    cd apps/dvbrx-web
    ./mock_dvbrx.py --socket /tmp/dvbrx-mock-0.sock &          # --tune none : récepteur libre
    ./dvbrx-web --socket /tmp/dvbrx-mock-0.sock --data-dir /tmp/dvbrx-web --port 8099
    # http://localhost:8099/

Options du mock : `--tune dvbt2|dvbc|dvbs2|none` (chaîne regardée par « tvheadend » : les
commandes exclusives répondent `busy` sans `force`), `--no-lnb` (pas de contrôleur de LNB),
`--strict` (rien au-delà du contrat : pas de `pids`, `plps`, `fft`… dans `probe`, pas de niveau
dans le canal `monitor`), `--speed` (durée des probe, 1 = temps réel).

Sur l'hôte : `sudo packaging/install.sh --web` installe le programme
(`/usr/local/lib/dvbrx-web`, `/usr/local/bin/dvbrx-web`), `/etc/dvbrxd/web.conf` (exemple
commenté), `/var/lib/dvbrx-web` et `dvbrx-web.service`, **sans l'activer** :

    sudo systemctl enable --now dvbrx-web

## API HTTP (pour la page)

| Méthode | Chemin | Rôle |
|---|---|---|
| GET | `/api/config` | sockets, oscillateurs du LNB, plages |
| GET | `/api/status?s=N` | `status` de dvbrx enrichi (`rf_freq`, `if_freq`, `polarization`, `radio.lnb_lo`, `web_monitor`, `web_scan`) |
| GET | `/api/constellation?s=N&adapter=A&points=2048` | relais de `constellation` |
| POST | `/api/scan/start` `{s, mode: quick\|full, sat, force, rescan, cable_start, cable_stop}` | lance un scan en tâche de fond (un par récepteur) |
| GET | `/api/scan/progress?s=N` | progression, état (`running`, `done`, `cancelled`, `busy`, `error`), journal |
| POST | `/api/scan/cancel` `{s}` | annulation |
| GET | `/api/scan/result?s=N&mode=quick\|full` | dernier résultat (enregistré dans le répertoire de données, rechargé au démarrage) |
| GET | `/api/export?s=N&kind=cable\|sat` | fichier dvbv5 (téléchargement) |
| POST | `/api/monitor/start` `{s, delsys, freq (FI, Hz), symbol_rate, volt, tone, force}` | alignement : `monitor start` puis `lnb` |
| POST | `/api/monitor/stop` `{s}` | `monitor stop` |
| POST | `/api/lnb` `{s, volt, tone}` | commande du LNB |

Un `busy` de dvbrx donne HTTP 409 `{"ok": false, "error": "busy"}` ; dvbrx injoignable donne
HTTP 502.

## Export

Format dvbv5 (celui de `dvbv5-scan` et des fichiers `data/dvb-scan` de tvheadend), un fichier
pour le câble/terrestre, un pour le satellite. Fréquences en Hz pour le câble et le terrestre ;
en **kHz** pour le satellite (fréquence réelle, arrondie au MHz), comme
`/usr/share/tvheadend/data/dvb-scan/dvb-s/Astra-19.2E` (`FREQUENCY = 11362000`). Les canaux
DOCSIS ne sont pas exportés. Pour tvheadend : copier le fichier dans
`/usr/share/tvheadend/data/dvb-scan/dvb-c/` (ou `dvb-t/`, `dvb-s/`) et le choisir comme
« multiplex prédéfinis » du réseau.

## Détails de la détection

- **Câble** : moyenne de 128 FFT de 2048 points par fenêtre ; plancher = 5e centile ; une case
  de 8 MHz est occupée si son niveau moyen dépasse le plancher de 6 dB. Le décalage de la
  grille est déduit du spectre (creux aux frontières) ou fixé dans la configuration. Trois
  cases contiguës ou plus sans creux sont réunies en un « bloc large » (OFDM DOCSIS 3.1), non
  sondé.
- **Satellite** : plancher glissant (10e centile par tranche de 20 MHz, minimum sur ±60 MHz),
  seuil de 4 dB ; la largeur à −3 dB donne le débit symbole estimé, affiné ensuite par la raie
  cyclique de |x|². Une porteuse vue dans les deux bandes (autour de 11,7 GHz) n'est sondée
  que dans sa bande nominale.
- **Puissances** : dBm estimés = dBFS − gain + offset (offset de la configuration, sinon celui
  de dvbrx déduit de son statut, sinon −50 dB). Ce n'est pas une mesure calibrée.

## Évolutions possibles

- **Contrôle DiSEqC** : commutateurs multi-LNB (DiSEqC 1.0/1.1, une ligne de scan par entrée du
  commutateur) et positionneurs (DiSEqC 1.2, USALS : aller à une position, mémoriser). Le
  protocole du contrôleur de LNB le prévoit déjà (`DISEQC e0 10 38 f0`, cf.
  `doc/lnb-protocol.md`), mais l'API de contrôle de dvbrx ne l'expose pas encore : il faudrait
  par exemple `{"cmd": "diseqc", "bytes": "e01038f0"}`, exclusif comme `lnb`.
- Multi-PLP DVB-T2 et multistream DVB-S2 (ISI) : liste des PLP / ISI dans `probe`, un multiplex
  exporté par PLP / ISI.
- Envoi direct des multiplex à tvheadend (API JSON) au lieu de l'export de fichiers.
- Blind scan satellite plus fin (porteuses étroites et adjacentes de niveaux différents).
