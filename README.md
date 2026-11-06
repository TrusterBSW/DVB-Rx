# dvbrx

Réception DVB temps réel sur radio logicielle (USRP B210, bladeRF 2.0,
HackRF One), exposée comme un adaptateur standard `/dev/dvb` : DVB-S,
DVB-S2, DVB-S2X, DVB-C, DVB-T et DVB-T2. Contrôle LNB (13/18 V, 22 kHz,
DiSEqC) déporté sur un ESP32. Supervision, scan et alignement par une page
web (`apps/dvbrx-web`).

Documentation : `dvbrx --help`, `man dvbrx` et `man dvbrx-web` (sources
dans `doc/man/`, installées par `packaging/install.sh`), ce README,
`doc/dvbrx-web-api.md` (socket de contrôle), `doc/lnb-protocol.md`.

## Mise en place

    git clone --recursive https://github.com/igorauad/gr-dvbs2rx third_party/gr-dvbs2rx
    cmake -S third_party/gr-dvbs2rx -B third_party/gr-dvbs2rx/build \
          -DCMAKE_INSTALL_PREFIX=$PWD/prefix -DCMAKE_BUILD_TYPE=Release \
          -DNATIVE_OPTIMIZATIONS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build third_party/gr-dvbs2rx/build -j8
    cmake --install third_party/gr-dvbs2rx/build
    . ./env.sh

    make -C libdvbrx                                  # bibliothèque C + fe_bench
    cmake -S gr-dvbrx -B gr-dvbrx/build && cmake --build gr-dvbrx/build   # bloc GNU Radio

Dépendances système : gnuradio (+ gnuradio-dev sur Debian), libuhd, FFTW
(libfftw3-dev, simple précision), pybind11, tsduck, ffmpeg. Pour `dvbrx`
seul : FFTW et, selon les cartes voulues, libuhd-dev, libbladerf-dev,
libhackrf-dev (chaque pilote est compilé si son en-tête est présent).

## Organisation

- `libdvbrx/` : récepteurs DVB complets en C (GPL-3.0-or-later, une partie
  étant portée de gr-dvbs2rx) :
  - `frontend.c` : correction de fréquence (AFC), filtre adapté, synchro
    rythme symbole (Gardner + interpolation cubique), AGC ;
  - `pl.c` : synchro trame PL, décodage PLS, désembrouillage, récupération de
    porteuse (pilotes + interpolation, ou PLL décisionnelle sans pilotes) ;
  - `demap.c` : démappeur max-log + désentrelacement, toutes les MODCOD
    DVB-S2 et DVB-S2X jusqu'au 256APSK (hors VL-SNR) ;
  - `ldpc.c` : LDPC layered min-sum, 32 trames en AVX2 / 64 en AVX-512 ;
  - `bch.c`, `bb.c` : BCH, désembrouillage BB, reconstruction TS par ISI ;
  - `dvbs.c` : DVB-S (1re génération) : porteuse, détection automatique du
    rendement / ambiguïté de phase / poinçonnage, Viterbi K=7, désentrelaceur,
    Reed-Solomon (204,188), dispersion d'énergie ;
  - `dvbc.c` : DVB-C (EN 300 429) : fréquence par la raie de puissance 4
    (FFT), égaliseur CMA puis décisionnel, PLL, décodage différentiel des
    quadrants, 16 à 256-QAM, détection automatique de la constellation ;
  - `dvbt.c` : DVB-T (EN 300 744) : détection mode 2K/8K et intervalle de
    garde sur le préfixe cyclique, fréquence fractionnaire et entière,
    estimation de canal sur les pilotes dispersés (interpolation à pente de
    phase compensée), phase commune et horloge d'échantillonnage sur les
    pilotes continus, TPS, désentrelacements symbole et bit, démapping
    pondéré par |H|², Viterbi ; flux HP en modulation hiérarchique ;
  - `dvbt2.c` : DVB-T2 (EN 302 755, profil de base, SISO) : P1 (S1/S2,
    fréquence), intervalle de garde, symboles P2 et L1-pre / L1-post (LDPC
    16K raccourci et poinçonné), toutes tailles FFT 1K à 32K, mode étendu,
    motifs PP1 à PP8, symbole de clôture, désentrelacements fréquentiel,
    temporel et de cellules, constellations tournées (retard Q cyclique),
    LDPC 16K/64K (dont les codes propres au T2 : 2/3 normal, 3/5 court),
    BCH, couche BB (décodage FEC sur 2 threads dédiés, trames décodées en
    parallèle et restituées dans l'ordre) ; un PLP (`-i`) ; tables générées par
    `gen/gen_t2_tables.py` depuis gr-dtv ;
  - `chan.c` : canaliseur multicanal (banc de filtres par convolution
    rapide, overlap-save : une FFT de la capture partagée par tous les
    canaux, puis par canal une petite IFFT à son propre débit ; l'inverse du
    banc de synthèse de dvbtx) ;
  - `resamp.c` : rééchantillonneur polyphase (débit SDR → débit OFDM,
    rapport suivant l'horloge de l'émetteur) ;
  - `conv.c` (Viterbi, poinçonnage) et `outer.c` (synchro paquet,
    désentrelaceur, RS, dispersion d'énergie) : couches communes DVB-S,
    DVB-C et DVB-T ; `fft.c` : FFT (OFDM) ;
  - `rx.c` (mono-thread) et `rxmt.c` (pipeline multi-thread) ;
  - `gen/` : générateurs des tables (FEC, LDPC, et constellations /
    entrelacement / codes PLS mesurés sur l'émetteur gr-dtv, sources dans
    `third_party/gnuradio-dtv`) ;
  - `tools/dvbrx.c` : le récepteur (démon `dvbrxd@N` en mode pont) ;
    `tools/sdr_uhd.c`, `sdr_bladerf.c`, `sdr_hackrf.c` : les pilotes de
    cartes (USRP par l'API C d'UHD, libbladeRF, libhackrf), ou fichier IQ.
- `gr-dvbrx/`, `apps/dvbrx-dvbs2-rx` : étape intermédiaire (front-end C dans
  la chaîne GNU Radio gr-dvbs2rx).
- `tests/` : vecteurs de référence (émetteur gr-dtv) et tests par étage.
- `bench/` : mesures de débit et de MER.

## Regarder la TV

    make -C libdvbrx
    # B210, fréquence intermédiaire L-band (après LNB), 27,5 Msym/s :
    libdvbrx/build/dvbrx -u type=b200 -f 1210e6 -s 27.5e6 -o udp://127.0.0.1:1234
    mpv udp://127.0.0.1:1234            # ou : dvbrx ... -o - | mpv -

Options utiles : `-S` fréquence d'échantillonnage (défaut 2 × débit symbole,
un rapport non entier ≥ 1,6 fonctionne), `-G` gain en dB ou `-G auto`
(voir plus bas), `-g` gold code PLS,
`-i` ISI (multistream ; sans `-i`, en UDP chaque ISI va sur PORT + ISI),
`-t` threads LDPC, `-l` DVB-S, `-c QAM` DVB-C (roll-off 0,15), `-T MHZ`
DVB-T, `-2 MHZ` DVB-T2 (largeur de canal ; `-S` débit, défaut 1,12 × débit
OFDM, soit 10,24 Msps en 8 MHz ; `-i` PLP). Sur fichier :
`-F fichier.fc32`, `-R` pour cadencer en temps réel (en boucle).

Plusieurs multiplex d'une même capture (voir « Multicanal ») : `-m` répété,
un canal par option, `-S` débit de la capture, `-f` centre (défaut : milieu
des canaux) :

    dvbrx -u type=b200 -G 25 -S 30.72e6 \
        -m f=646e6,sys=t2,bw=8,o=udp://127.0.0.1:1234 \
        -m f=655e6,sys=c,sr=6.9e6,qam=64,o=udp://127.0.0.1:1235

Clés : `f` (Hz), `sys` (`s`, `s2`, `c`, `t`, `t2`), `sr` (sym/s), `bw` (MHz),
`ro`, `qam`, `fec` (DVB-S), `isi` / `plp`, `gold`, `o` (sortie). `-x HZ` :
un seul canal décalé de HZ dans une capture à `-S`.

## Carte DVB standard : `/dev/dvb/adapterN`

Le module noyau `kernel/dvbrx` crée des adaptateurs DVB-S/S2 classiques
(frontend, demux, dvr, net) servis par `dvbrx -d N` ; les applications DVB
(dvbv5-zap, tvheadend, VLC, Kaffeine…) les utilisent comme une carte.

    # module (testé sur noyaux 6.12 et 7.0)
    make -C kernel && sudo modprobe dvb-core && sudo insmod kernel/dvbrx.ko
    # ou installation durable : sudo dkms install ./kernel  (dkms.conf fourni)

    # démon : B210, contrôleur LNB sur l'ESP32
    sudo libdvbrx/build/dvbrx -d 0 -u type=b200 -L serial:/dev/ttyACM0
    # sans matériel : un fichier IQ rejoué en boucle à la cadence réelle
    sudo libdvbrx/build/dvbrx -d 0 -F signal.fc32

    dvbv5-zap -a 0 -l UNIVERSAL -c chaines.conf -r MaChaine

- La fréquence demandée par l'application est la FI après LNB (les applis la
  calculent à partir de leur configuration LNB) ; débit symbole, ISI
  (`STREAM_ID`) et gold code PLS (`SCRAMBLING_SEQUENCE_INDEX`) sont repris
  du réglage. Sans ISI, le premier flux reçu est délivré.
- Les commandes LNB (13/18 V, 22 kHz, burst, DiSEqC) sont transmises au
  contrôleur, de manière synchrone : voir `doc/lnb-protocol.md`.
- État remonté : verrouillage (`FE_HAS_LOCK` dès que le TS sort), C/N
  (statistiques DVBv5), trames reçues / trames en erreur.
- DVB-S (première génération) : l'application demande `SYS_DVBS`, le
  rendement vient de `INNER_FEC` (ou détection automatique).
- tvheadend voit trois frontends sur l'adaptateur : DVB-S (S/S2/S2X),
  DVB-C et DVB-T (T et T2), utilisables un à la fois ; activer ceux voulus
  et leur associer un réseau (Configuration → DVB Inputs → TV adapters).
- Le frontend porte le nom de la carte et son numéro de série
  (« dvbrx B210 31A5B7C », « dvbrx bladeRF 2.0 xA4 0a0896e9… ») : le démon
  ouvre la carte à son démarrage et le donne au module (`DVBRX_IOC_NAME`),
  avant que tvheadend ne démarre (`Type=notify`, `Before=tvheadend.service`).
  tvheadend lit ce nom en découvrant l'adaptateur, et en tire l'identifiant
  de sa configuration : chaque carte garde la sienne. Changer de carte
  tvheadend en marche demande de redémarrer tvheadend, puis d'activer les
  frontends de la nouvelle carte et de leur associer les réseaux.
  Testé en radio réelle via le HackRF : scan puis lecture de chaînes en
  DVB-S, DVB-S2, DVB-T, DVB-T2 et DVB-C ; les paramètres (constellation,
  rendement, garde, FFT) sont remontés à tvheadend. DVB-S2X : en radio
  (dvbtx avec le S2X ajouté, 16APSK 26/45, roll-off 0,10, pilotes : scan,
  lecture d'Arte sans erreur ; 64APSK 128/180 demande ~15 dB, au-delà de ce
  que donne le lien HackRF → B210 à 1,25 GHz, ~12-16 dB), et sur fichier
  pour 64APSK 128/180 ; les 55 MODCOD S2X émis par dvbtx (radio nulle)
  sont décodés sans erreur.

### Cartes

| | USRP B210 | bladeRF 2.0 micro | HackRF One |
|---|---|---|---|
| `-u` | `uhd:serial=…` (ou `type=b200`) | `bladerf:*:serial=…` | `hackrf:NUMÉRO` |
| Débit max | 61,44 Msps | 61,44 Msps | 20 Msps |
| Débit symbole max, DVB-S (roll-off 0,35) | 41 Msym/s | 41 Msym/s | 13,3 Msym/s |
| DVB-S2 roll-off 0,20 / 0,05 | 46 / 52,7 Msym/s | 46 / 52,7 Msym/s | 15 / 17,1 Msym/s |
| DVB-C (roll-off 0,15) | 48 Msym/s | 48 Msym/s | 15,6 Msym/s |
| Sensibilité (câble SFR, 5/11/2026) | référence | ≈ B210 (±1,5 dB) | 6-7 dB de moins (mesuré derrière un hub USB qui le perturbait, voir « Débit USB partagé ») |
| Horodatage (trous comblés) | oui | oui (FPGA ≤ 0.15) | non |
| Multicanal (`-n`, `-m`) | oui | oui | non |

- bladeRF : le FPGA doit aller avec libbladeRF (0.15.3 avec libbladeRF
  2024.05 ; un FPGA 0.16 ne livre qu'un quart des échantillons) ; la version
  est affichée au démarrage. Règle udev dans `packaging/60-dvbrx.rules`.
- B210 et bladeRF (AD9361) : entre 138 et 250 MHz, les signaux à trois fois
  la fréquence accordée se replient dans la bande (filtre passe-bas ~300 MHz
  nécessaire).
- Débit d'échantillonnage (mode pont) : 2 échantillons par symbole si la
  carte le peut, sinon au moins débit symbole × (1 + roll-off) / 0,9 : le
  signal doit rester dans 90 % de la bande (au bord, les filtres de
  l'AD9361 coûtent ~3 dB, mesure dvbtx). Soit 1,5 éch./symbole en DVB-S,
  1,28 en DVB-C, 1,33 à 1,17 en DVB-S2 (roll-off 0,20 à 0,05 ; roll-off
  automatique : compté 0,35). Sur fichier (6/11/2026), aucune perte de
  SNR à ces débits par rapport à 2 éch./symbole (mais en réel, voir « Débit
  USB partagé » : ~2,4 dB de moins en DVB-C sur le câble) : DVB-S2 QPSK à 32APSK,
  avec et sans pilotes, 18/18 ; DVB-C 16 à 256-QAM (le 256-QAM accroche un
  peu plus lentement à 1,28 : ~20 paquets perdus à l'accord) ; DVB-S à 1,5.
  En réel (6/11/2026, dvbtx → coupleur sur câble fermé, FI 1200 MHz, gain
  TX 20 dB, sondages en mode pont) :

  | Signal | Carte | Débit | Éch./symbole | SNR | Erreurs en régime |
  |---|---|---|---|---|---|
  | DVB-S2 QPSK 3/4, 46 Msym/s, roll-off 0,20 | B210 | 61,44 Msps | 1,34 | 17,8 dB | 0 (TS 66,8 Mbit/s) |
  | DVB-S2 QPSK 3/4, 50 Msym/s, 0,10 | B210 | 61,44 Msps | 1,23 | 17,2 dB | 0 (72,4 Mbit/s) |
  | DVB-S2 QPSK 3/4, 52 Msym/s, 0,05 | B210 | 61,44 Msps | 1,18 | 16,6 dB | 0 (75,6 Mbit/s) |
  | DVB-S2 QPSK 3/4, 15 Msym/s, 0,20 | HackRF | 20 Msps | 1,33 | ~20 dB | pertes USB (hub) |
  | DVB-S2 QPSK 3/4, 17 Msym/s, 0,05 | HackRF | 20 Msps | 1,18 | ~19 dB | pertes USB (hub) |
  | DVB-S QPSK 3/4, 13,3 Msym/s, 0,35 | B210 / HackRF | 30,72 / 20 Msps | 2,3 / 1,5 | 21,0 / 16,3 dB | 0 / 2,6 % |

  Avant cette règle, le mode pont refusait tout au-delà de 37,3 Msym/s sur
  B210. Les échecs FEC de l'accrochage (quelques secondes, gain automatique)
  ne sont pas comptés. Le HackRF fait 2 à 7 % de paquets en erreur en DVB-S
  même à 2 éch./symbole et sans perte USB : défaut à part, à étudier.
  Suréchantillonner davantage n'apporte rien : HackRF sur le DVB-C 602 MHz
  (64-QAM, 6,875 Mbd, gain 70, 4 × 30 s alternés), 1,8-2,0 % de paquets en
  erreur à 13,75 Msps (2 éch./symbole) contre 3,9-4,5 % à 20 Msps.
- Débit symbole maximal : le démon annonce au frontend (`FE_GET_INFO`,
  `DVBRX_IOC_LIMITS`) celui du roll-off le plus faible (0,05), ou, en
  multicanal, celui de la bande utile de la capture. dvb-core refuse alors
  aussitôt (`EINVAL`) un réglage DVB-S/S2/C au-delà, par exemple un
  transpondeur de 22 000 kBd sur un HackRF ; tvheadend affiche « failed to
  tune » et passe à un autre tuner libre (essai du 6/11/2026 : bascule sur
  le bladeRF, flux en 2,9 s). Sans autre tuner, la chaîne ne démarre pas.
  Entre les deux limites (un DVB-S à 15 Msym/s sur un HackRF), le réglage
  passe le noyau mais le démon le refuse (« no usable sample rate ») :
  l'application constate l'absence de signal.

### Plusieurs cartes en même temps

Un démon par carte, chacun sur son adaptateur, chaque carte désignée par
son numéro de série (l'ordre de branchement ne compte pas) :

    # /etc/modprobe.d/dvbrx.conf : options dvbrx adapters=2
    # /etc/dvbrxd/0.conf : DVBRXD_ARGS="-u bladerf:*:serial=0a0896e9… -G auto -t 3"
    # /etc/dvbrxd/1.conf : DVBRXD_ARGS="-u hackrf:334c64dc39395367 -G auto -t 2"
    sudo systemctl enable --now dvbrxd@0 dvbrxd@1

tvheadend voit deux adaptateurs nommés d'après leur carte et peut recevoir
deux multiplex à la fois ; dvbrx-web trouve les deux sockets et les présente
avec leur carte. Essai du 6/11/2026 (bladeRF + HackRF, câble SFR, via
tvheadend) : DVB-T 554 MHz sur le bladeRF sans erreur pendant que le HackRF
recevait le DVB-C 602 MHz (QAM64, SNR 24,8 dB, 3 % de blocs en erreur : le
HackRF est à la limite sur ce câble) ; aucun débordement.

### Débit USB partagé

L'USB est limitant : une carte AD9361 (B210, bladeRF) envoie 4 octets par
échantillon, soit 246 Mo/s à 61,44 Msps et 220 Mo/s pour un transpondeur
de 27 500 kBd (55 Msps) ; l'USB 3 (5 Gbit/s) n'en porte qu'environ
350 Mo/s utiles par bus. Une carte seule tient, deux cartes en satellite
large sur le même bus non. Le HackRF (2 octets par échantillon, 40 Mo/s à
20 Msps) est déjà au plafond de l'USB 2 (~42 Mo/s).

**Des ports différents ne veulent pas dire des contrôleurs différents** :
tous les ports d'un contrôleur (et ceux des hubs branchés dessus) partagent
sa bande. Un contrôleur xHCI a deux bus, l'un USB 2 et l'autre USB 3, sur
des lignes séparées : une carte USB 2 (HackRF) ne prend rien à une carte
USB 3 du même contrôleur. `lsusb -t` montre les bus (`Bus 002 … 5000M`) et
ce qui est branché dessus ; ici le HackRF (usb1, 480M) et le bladeRF (usb2,
5000M) sont sur le même contrôleur `0000:00:14.0`.

Les démons se partagent la bande d'un bus. Chacun trouve le bus de sa carte
dans `/sys/bus/usb` (par son numéro de série) et écrit ce qu'il utilise
dans `/run/dvbrx/usb-N` :

- à un réglage, il prend son plein débit (2 éch./symbole) s'il est seul sur
  le bus, sinon ce que les autres laissent, sans descendre sous le minimum
  du canal (débit symbole × (1 + roll-off) / 0,9) ;
- s'il manque de place, il en demande : les autres cartes du bus redescendent
  à ce qu'elles peuvent laisser, par un nouveau réglage (environ 1 s sans
  TS pour elles), puis il se règle ; sans place au bout de 2,5 s, il prend
  son minimum quand même (débordements probables) ;
- une carte redescendue garde ce débit jusqu'à son prochain réglage (changement
  de chaîne) : remonter couperait le flux, pour au plus quelques dB ;
- budget par bus : 350 Mo/s en USB 3, 42 Mo/s en USB 2 ; `-U MO/S` le
  change (à mettre identique sur toutes les cartes d'un bus), `-U 0`
  désactive le partage ; les courtes captures des scans de dvbrx-web ne sont
  pas comptées, ni les autres périphériques du bus (clé TNT, disque…) ; le
  statut (`radio.usb`) et dvbrx-web montrent l'occupation.
- Vécu le 6/11/2026 : un HackRF à 20 Msps derrière un petit hub USB 2,
  partagé avec une clé RTL2832 et un B210 (inactifs), perdait ~14 fois par
  seconde des échantillons (compteur « shortfalls » du HackRF,
  `hackrf_debug --state`, et `hackrf_transfer` seul en faisait autant),
  contre aucune perte une fois seul sur le hub (clé détachée).
- Même sans perte, le HackRF derrière ce petit hub non alimenté voyait la
  phase de son oscillateur sauter d'environ 90°, deux fois toutes les
  102,4 ms (alimentation USB) : 2 à 7 % de paquets en erreur en DVB-S et en
  DVB-C, quels que soient gain, débit et accord. Branché en direct sur un
  autre PC, la même carte n'a aucun saut. Un HackRF veut un port direct ou un
  hub alimenté.
- Carte débranchée : le démon s'arrête (« the SDR does not answer any more »)
  au lieu d'insister (libbladeRF finissait par planter, SIGFPE) ; systemd le
  relance et il rouvre la carte quand elle revient.

Descendre a un prix en conditions réelles : sur le câble SFR, bladeRF sur le
DVB-C 658 MHz (64-QAM), SNR 26,7 dB à 13,75 Msps (2 éch./symbole) contre
24,3 dB à 8,78 Msps (1,28), 0 erreur dans les deux cas (multiplex voisins à
±8 MHz). C'est pourquoi le démon ne descend que si la carte ou le bus
l'impose. Essai de la mécanique le 6/11/2026 avec une carte voisine simulée
sur le bus du bladeRF : réglage à 12,5 Msps au lieu de 13,75 quand elle
occupait 300 Mo/s, et redescente de 13,75 à 12,5 Msps en cours de lecture
tvheadend quand elle en demandait 300.

### Multicanal : plusieurs adaptateurs sur un seul B210

    sudo modprobe dvbrx adapters=2
    sudo dvbrx -d 0 -n 2 -u type=b200 -G 25 -W 30.72e6   # /dev/dvb/adapter0 et 1

Une seule capture à débit fixe (`-W`, défaut 30,72 Msps) sert `-n`
adaptateurs : le canaliseur (`chan.c`) extrait chaque multiplex à son propre
débit et chaque adaptateur a son récepteur. Le centre de la capture suit les
canaux en service : il se met sur le premier, puis se recentre (reréglage
du B210, les canaux en cours suivent sans décrocher) quand un nouveau canal
ne tient pas ; un réglage qui ne tient pas avec les canaux des autres
adaptateurs (90 % de la bande utile) est refusé (pas de verrouillage).
`-f` fixe le centre. Satellite : les adaptateurs partagent le LNB ; une
commande (polarisation, bande, DiSEqC) qui changerait ce que reçoit un autre
adaptateur en service est refusée (`EBUSY`), la répétition de l'état courant
est acceptée.

- Gain : c'est la puissance de tous les multiplex de la capture qui compte ;
  le gain réglé pour un canal seul sature (B210, deux multiplex HackRF :
  erreurs à 35 dB, 0 erreur à 20-28 dB) : utiliser `-G auto`.
- Coût : le canaliseur coûte ~0,15 cœur à 30,72 Msps (une FFT de 16384
  points tous les 14336 échantillons, puis une IFFT par canal) ; chaque
  multiplex coûte sa démodulation (i5-9500 : DVB-T 8K 64-QAM 5/6 ~1,3 cœur,
  DVB-C 256-QAM ~1,1, DVB-T2 8K 64-QAM ~1,2, DVB-S2 selon le débit).
- Mesuré en radio réelle (HackRF dvbtx : DVB-T2 8K 64-QAM 3/4 à 646 MHz +
  DVB-C 64-QAM 6,9 Mbd à 655 MHz, B210 à 30,72 Msps) sur l'hôte sans
  virtualisation : 57 s, 0 bloc FEC en échec, 0 paquet en erreur sur les
  deux (31,3 + 37,5 Mbit/s), 0 débordement, 2,5 cœurs. Trois multiplex
  (T2 + T + C) sur fichier dans 30,72 Msps : 0 erreur hors accrochage.
- Dans la VM (redirection USB de QEMU), le B210 déborde au-delà de ~15 Msps
  (2 à 3 fois/s à 30,72 Msps, encore ~1/s à 20,48 Msps), quel que soit le
  tampon : le multicanal y perd des trames. Pour du large bande, donner à
  la VM un contrôleur USB en passthrough PCIe, ou faire tourner dvbrx
  sur la machine hôte.

### Installation durable (testée sur Debian 13)

    sudo packaging/install.sh 1     # module DKMS, udev, utilisateur dvbrxd, service
    sudoedit /etc/dvbrxd/0.conf     # DVBRXD_ARGS="-u type=b200 -L serial:/dev/ttyACM0 -t 3"
    sudo systemctl enable --now dvbrxd@0

Le module est chargé au démarrage, `/dev/dvbrxN` et `/dev/dvb` sont au
groupe `video`, le service `dvbrxd@N` lance `/usr/local/bin/dvbrx` sous
l'utilisateur `dvbrxd`. Testé avec
dvbv5-scan / dvbv5-zap et VLC (`cvlc dvb-s2://frequency=11778000000
--dvb-srate=...`). Pour l'USRP, ajouter la règle udev d'UHD (paquet
uhd-host) si `dvbrx` ne voit pas le B210.

### B210 en pratique (clone LibreSDR)

Depuis le 3/11/2026, le récepteur tourne directement sur l'hôte Proxmox
(module dvbrx en dkms, `dvbrxd@0`, tvheadend) : 0 débordement à 30,72 Msps,
DVB-S2 lu via tvheadend sans erreur. La VM de test a été sauvegardée
(vzdump) puis supprimée. Les notes ci-dessous sur la VM restent valables si
on y revient.

- Clone LibreSDR : copier son image FPGA et son `/etc/uhd/uhd.conf`
  (horloge maître fixe 61,44 MHz : les débits possibles sont 61,44/N Msps ;
  dvbrx prend le débit réellement obtenu, 30,72 Msps pour 15,5 Msym/s).
- Gain automatique `-G auto` (recommandé) : dvbrx mesure le niveau moyen
  et l'écrêtage de l'ADC sur des fenêtres de 0,5 s et règle le gain du B210
  pour un niveau de -24 dBFS (±4 dB), en le baissant aussitôt (6 à 10 dB)
  si l'ADC écrête. Mesuré en DVB-T 64-QAM : MER plat (27,1-27,5 dB) de -30
  à -15 dBFS, écrêtage à partir de -10 dBFS, MER 22 dB à -5 dBFS, plus de
  verrouillage au-delà ; parti de 45 dB, le gain se stabilise en ~4 s. Le
  niveau et le gain sont affichés dans les statistiques (`-22.2 dBFS gain
  30`, `CLIP` en cas d'écrêtage).
- Choisir l'entrée avec `-A TX/RX` ou `-A RX2` (sur ce montage, le signal
  arrive sur TX/RX).
- Tampons USB plus grands, indispensables dans une VM :
  `-u type=b200,num_recv_frames=512,recv_frame_size=16360` (pertes de 42 %
  à 0,5 % à 30,72 Msps).
- Le signal doit rester dans 90 % de la bande échantillonnée : à
  20,48 Msps pour 15,5 Msym/s (roll-off 0,2, 91 %), avant l'égaliseur, le
  SNR s'effondrait. La règle du mode pont en tient compte (voir « Cartes »).
- Les interruptions du contrôleur USB (plusieurs milliers par seconde)
  ralentissent le thread qui partage leur CPU : `packaging/dvbrx-irq-pin`
  les met sur le dernier CPU et `dvbrxd-cpu.conf.example` (à copier dans
  `/etc/systemd/system/dvbrxd@.service.d/cpu.conf`) garde dvbrx sur les
  autres. Sans cela, dvbrx perd des échantillons dans la VM.
- Un débordement USB : les échantillons perdus (comptés d'après les
  horodatages du B210) sont remplacés par autant de zéros, les récepteurs
  gardent leur synchronisation temporelle (DVB-T2 : quelques symboles
  effacés au lieu d'une réacquisition) ; `ovf N (x ms)` dans les
  statistiques. Le statut LOCK remonté à `/dev/dvb` est maintenu 1 s pour
  que les applications ne refassent pas un réglage à chaque micro-coupure.

Premier essai réel (HackRF en câble direct, DVB-S2 8PSK 9/10 pilotes,
15,5 Msym/s à 1200 MHz, multiplex 10950 H via tvheadend) : verrouillage,
SNR ~14 dB, 0 erreur CRC, 6 services trouvés au scan.

## Banc de test sans signal

    # Vecteurs de référence par étage, puis tests
    python3 tests/txvectors.py work/ts1m.ts work/vec/qpsk34 -m QPSK3/4 --pilots
    tests/run_backend.sh            # démappeur -> LDPC -> BCH -> BB
    tests/run_pl.sh 0.01 0.5        # synchro PL -> TS (décalage, marge SNR)
    tests/run_s2x.py 3.0            # les 55 MODCOD DVB-S2X, Shannon + 3 dB
    # multistream / VCM : un flux TS par ISI, MODCOD au choix, gold code
    python3 tests/mis_gen.py mis.fc32 --stream 1:a.ts:QPSK3/4 \
        --stream 2:b.ts:16APSK3/4 --gold 131073 --pilots
    libdvbrx/build/dvbrx_file -i 2 -g 131073 -n 12 mis.fc32 isi2.ts
    # DVB-S : codage de canal gr-dtv (DVB-T, identique), décodage auto
    python3 tests/dvbs_gen.py in.ts dvbs.fc32 --rate 3/4
    libdvbrx/build/dvbrx_file -l auto -n 7.4 dvbs.fc32 out.ts
    tests/run_s2x.py 3.0 64APSK nopilots
    # roll-off quelconque (S2X : 0.15/0.10/0.05) depuis des trames PL
    python3 tests/txshape.py work/vec/.../plframe.fc32 iq.fc32 --rolloff 0.05 --esn0 8


    # Flux TS de test (mire + son 1 kHz)
    mkdir -p work && ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 \
        -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 30 \
        -c:v libx264 -b:v 4M -c:a mp2 -f mpegts -muxrate 5M work/test.ts

    # TS -> modulateur DVB-S2 -> bruit/décalage -> démodulateur -> TS, + débit
    bench/loopback.py -m 8PSK3/4 --snr 9 --ts-bytes 8000000 --keep
    bench/loopback.py --rx-app dvbs2-rx ...          # chaîne gr-dvbs2rx d'origine

    # Débit en régime établi (fichier en boucle) ; 22194 = symboles/trame
    # pour 8PSK, trame normale, pilotes
    bench/steady.sh apps/dvbrx-dvbs2-rx work/loopback/<iq>.fc32 8PSK3/4 22194

    # Front-end seul : débit (C pur, puis bloc GNU Radio) et MER
    libdvbrx/build/fe_bench <iq.fc32> 2 0.2 <symboles.fc32>
    python3 bench/fe_gr_bench.py <iq.fc32> --rotator
    python3 bench/mer.py <symboles.fc32>

    # Matrice de tests : toutes les normes et leurs paramètres (générés,
    # rééchantillonnés à un débit SDR, +15 ppm, décalage de fréquence, bruit)
    python3 tests/matrix.py [dvbt] [dvbt2] [dvbc] [dvbs] [--quick]
    # plusieurs multiplex côte à côte dans une capture large bande
    python3 tests/wideband.py wide.fc32 --rate 30.72e6 --seconds 1.2 --snr 32 \
        t2.fc32:9142857.14:-9.5e6 t.fc32:9142857.14:0.5e6 c.fc32:13.8e6:9.5e6

Matrice (3/11/2026) : DVB-T 60/60 (2K/8K, QPSK à 64-QAM, 1/2 à 7/8, GI 1/32
et 1/4, 5 à 8 MHz), DVB-C 15/15 (16 à 256-QAM, 2 à 3,07 éch./symbole),
DVB-S 10/10, DVB-T2 98/98 (4 constellations × 6 rendements × 16K/64K, et
toutes les tailles FFT × intervalles de garde × motifs de pilotes × mode
étendu, largeurs 1,7 à 10 MHz). Seul cas non testable avec gr-dtv : 16K PP1
étendu (son générateur de pilotes a 3636 au lieu de 13636, table G.2, alors
que son mappeur compte les bonnes cellules : signal incohérent) ; vérifié
avec dvbtx, ainsi que 32K PP8 19/128 et 16K PP2 étendus : 0 erreur.

## Pistes futures

Voir `doc/A_FAIRE.md` : robustesse (resynchronisation DVB-S, égaliseur sans
pilotes, accrochage DVB-T), DVB-S sur bladeRF et HackRF à mesurer, et les
pistes d'optimisation de `doc/optimisation/`.

## Optimisation CPU (3/11/2026)

Sur i5-9500, cœurs nécessaires en temps réel (avant → après) : DVB-T 8K
64-QAM 5/6 1,25 → 0,44 ; DVB-T2 8K 64-QAM 3/4 0,94 → 0,29 ; DVB-C 256-QAM
0,98 → 0,41 ; DVB-S2 8PSK 3/4 à 27,5 Mbd 1,86 → 1,23. Détails, impasses et
pistes restantes : `doc/optimisation/`.

## Résultats dvbrx (i5-9500, 6 cœurs, 8PSK 3/4 à 8,5 dB, pilotes)

- 36,7 Msym/s maximum (chaîne C complète) ; temps réel tenu à 27,5 et
  30 Msym/s sans perte.
- Décodage sans erreur à 0,5 dB du seuil QEF pour QPSK, 8PSK, 16APSK et
  32APSK, décalage porteuse jusqu'à ±0,1 symbole⁻¹ (± 2,75 MHz à 27,5 Msym/s)
  et au-delà avec l'AFC.
- DVB-S2X : les 47 MODCOD (QPSK à 64APSK, trames normales et courtes)
  décodent à Shannon + 3 dB avec pilotes ; seuils mesurés à environ
  Shannon + 1,75 à 2,25 dB (8PSK 13/18 ~7,7 dB, 64APSK 4/5 ~16,0 dB).
  64APSK : 36,7 Msym/s max, temps réel 30 Msym/s sans perte.
- Roll-off 0,20 à 0,05 (bande de la boucle de rythme réduite pour les
  faibles roll-off) ; à 0,05, prévoir ~1 dB de marge au-dessus du seuil.
- 128APSK et 256APSK (contribution professionnelle) : décodés à Shannon +
  3 dB ; 256APSK 3/4 en temps réel jusqu'à 27,5 Msym/s ; tolérance au bruit
  de phase ~0,2°/symbole (marche aléatoire), un LNB de qualité est requis.
- Multistream (MIS) et VCM : sélection d'ISI, gold code ou root code PLS
  (`-g 131073` ou `-g r2052`), plusieurs MODCOD entrelacées trame à trame
  (lots LDPC par MODCOD, sortie remise dans l'ordre des trames).
- DVB-S : les 5 rendements détectés automatiquement et décodés à 0,5 dB du
  seuil de la norme (1/2 jusqu'à Es/N0 3,5 dB) ; 38 Msym/s max, temps réel
  30 Msym/s sans perte (Viterbi sur un thread séparé).
- DVB-C : 16 à 256-QAM décodés sans paquet faux au Es/N0 de la norme
  (16-QAM 17 dB, 32-QAM 20 dB, 64-QAM 23 dB, 128-QAM 26 dB ; 256-QAM au
  seuil à 29 dB), décalage porteuse ±0,04 symbole⁻¹, constellation
  détectée automatiquement ; plancher de MER du récepteur 46 dB ; 6,9 Msym/s
  64-QAM tenus en temps réel (pipeline multi-thread).
- DVB-T : signaux réels de dvbtx (2K 16-QAM 2/3 GI 1/8, 8K 64-QAM 5/6
  GI 1/32) et de gr-dtv décodés sans erreur (MER > 100 dB sur signal
  numérique pur) ; avec passage 9,14 → 10,24 Msps, horloge −60 à +40 ppm,
  porteuse ±20 kHz, écho −6 dB à 5 µs et SNR 25 dB : 0 paquet perdu ;
  8K 64-QAM 5/6 en temps réel (3 threads : rééchantillonnage, OFDM,
  Viterbi), réglage via `/dev/dvb` (`dvbv5-zap`) vérifié.
- DVB-T2 : signaux réels de dvbtx décodés sans aucun bloc FEC en échec :
  32K étendu PP7 GI 1/128 256-QAM tourné 2/3 (profil par défaut de dvbtx),
  8K PP2 GI 1/8 en QPSK, 16-QAM et 64-QAM, rendements 1/2 à 5/6, tourné ou
  non ; avec passage à 10,24 Msps, horloge ±60 ppm et porteuse ±20 kHz :
  sans erreur ; écho −6 dB seul ou bruit seul (64-QAM 3/4 jusqu'à 22 dB)
  sans erreur, combinés à 25 dB : ~7 % de blocs en échec (démapping tourné
  découplé, à améliorer) ; temps réel 8K (thread OFDM à ~70 %), `/dev/dvb`
  vérifié (`dvbv5-zap`, PLP par `STREAM_ID`).
- Note gr-dtv : son entrelaceur de cellules T2 calcule mal le décalage
  P(r) pour les blocs FEC r ≥ 1 d'un bloc TI ; `tests/dvbt2_gen.py --ti fec`
  (un bloc FEC par bloc TI) contourne le problème.
- Signaux de référence : `dvbtx-engine` (10.5.1.23) avec la radio `null`
  (`args = file=...`) produit de l'IQ réel DVB-S/S2/C/T/T2 sans toucher à
  l'émission en cours.
- Note : jusqu'au 3/11/2026, `tests/txshape.py --esn0` ajoutait 3 dB de
  bruit de moins que demandé (corrigé) ; les mesures au niveau symbole
  (`dvbrx_file -n`) n'étaient pas concernées.
- Limites : sans pilotes, l'APSK S2X dense (8APSK, 16APSK 26/45, 32APSK)
  ne décode pas de façon fiable ; VL-SNR non géré.

## Résultats chaîne GNU Radio (i5-9500, 8PSK 3/4, trame normale, pilotes)

| Chaîne | Débit régime établi |
|---|---|
| gr-dvbs2rx d'origine | 11,9 Msym/s |
| dvbrx-dvbs2-rx, SNR 11 dB | 33,7 Msym/s |
| dvbrx-dvbs2-rx, SNR 8,5 dB | 30,4 Msym/s |
| dvbrx-dvbs2-rx, SNR 8,1 dB (seuil) | 19 Msym/s (limité par le LDPC) |
