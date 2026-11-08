# Analyse : DVB-S / DVB-S2 / S2X, ce qui reste optimisable (3/11/2026)

Mesures sur l'hôte (i5-9500), version 9080435 (égaliseur inclus), service
dvbrxd@0 actif pendant les mesures. Temps CPU par thread (perf task-clock).

## Où va le CPU aujourd'hui

**DVB-S2 8PSK 3/4, 27,5 Mbd, 55 MS/s (2 échantillons par symbole), -t 2** : 1,93 cœur
| thread | cœur | contenu |
|---|---|---|
| drx-timing | 0,68 | boucle de Gardner + demi-bande ×2 (0,21) + memcpy d'append |
| drx-ldpc ×2 | 0,47 | 2,8 itérations |
| drx-plsync | 0,28 | synchro PL, égaliseur, rotate |
| drx-filter | 0,27 | NCO + filtre adapté RRC (41 coefficients à 2 échantillons par symbole) |
| principal | 0,20 | lecture du fichier et copie (en direct : thread UHD) |
| sortie | 0,03 | BCH, BB |

**256APSK 135/180 à 50-52 Mbd, en direct** : environ 3,4 itérations avec l'égaliseur.
Tient en temps réel avec de la marge (capture à 45 Mbd : 1,6× le temps réel).

**DVB-S QPSK 7/8, 30 Mbd** : déjà sur 2 threads (frontal et back-end Viterbi).
Le frontal (drx_dvbs_process : PLL par symbole, 16 ns/sym) prend 0,48 cœur à
30 Mbd, plafond vers 60 Mbd. Le back-end (Viterbi 5 ns/bit + RS) prend environ 0,3 cœur.
Sur un B210, le DVB-S plafonne de toute façon vers 41 Mbd (roll-off 0,35
dans 56 MHz) : le CPU n'est pas limitant.

## Constats

1. **Bug de robustesse DVB-S (prioritaire, hors CPU).** Selon l'endroit d'une
   coupure du flux (fichier bouclé, ou débordement USB comblé par des zéros),
   le récepteur resynchronise les octets mais **ne se remet jamais** : tous
   les paquets suivants sont en échec RS (5 291 sur 5 291), avec `-l 7/8` comme
   avec `-l auto`. Une autre coupure peut le remettre en phase. Hypothèse :
   la phase du commutateur du désentrelaceur de Forney (anneau unique de
   l'agent C) ou l'ambiguïté de phase QPSK ne sont pas réévaluées tant que les
   octets de synchronisation restent visibles. Correctif : après N paquets RS
   consécutifs en échec, forcer une resynchronisation complète (recherche
   d'hypothèse, Forney remis à zéro).
   **Charge : 0,5 jour** (reproduction avec le fichier bouclé, correctif, matrice DVB-S).
   **Corrigé le 5/11** : la cause était un saut de 180° de la porteuse après la coupure.
   Les bits inversés échangent 0x47 et 0xB8, et le contrôle de synchro acceptait les deux.
   La couche externe suit l'inversion quand 0xB8 apparaît 4 fois ou plus sur 8 débuts de
   paquet. `work/bench/dvbs78_30m.fc32` mis 10 fois bout à bout : 26 503 paquets en échec sur
   52 884 avant, 126 après (environ 14 par jonction).
2. **Demi-bande ×2 inutile à 2 échantillons par symbole.** À exactement 2 éch./sym,
   l'interpolation cubique ne perd rien (29,95 contre 29,96 dB sur un signal
   à 30 dB) ; elle perd 0,55 dB à 2,2 et 0,3 dB à 2,4. Activer la demi-bande seulement
   quand sps < 2, ou quand sps n'est pas proche d'un entier :
   **-0,21 cœur** sur les configurations à 2 éch./sym (défaut `-S 2×Rs`).
   **Charge : 1 heure** (+ matrice).
   **Écarté le 8/11 : faux en conditions réelles.** La mesure du 3/11 était faite à exactement
   2,000 éch./sym, sans écart d'horloge. Avec 15 ppm (toujours présents entre deux radios), la
   phase d'échantillonnage parcourt toutes les positions et l'interpolation cubique seule perd
   3 dB à 2,0 (25,9 contre 28,9 dB sur un DVB-S à 30 dB) ; à 2,048 (61,44 Msps pour 30 Mbd) elle
   perd 2,1 dB même sans écart. La demi-bande reste nécessaire jusqu'à UP_SPS.
3. **DVB-S2 sans pilotes : pas d'égalisation.** L'égaliseur n'apprend que sur
   les trames à pilotes. Pour l'APSK sans pilotes à fort débit, on peut
   l'apprendre sur les 90 symboles d'en-tête, ou en mode piloté par les décisions sur les données.
   **Charge : 0,5 à 1 jour.**
   **Fait le 5/11** : apprentissage sur l'en-tête et sur les décisions sûres (erreur
   inférieure à 0,1 dmin²) des 1 024 premiers symboles. Sur chaque trame au début, puis
   une sur 4, puis une sur 16. Les coefficients ne sont gardés que s'ils réduisent
   l'erreur résiduelle d'au moins 0,3 dB par rapport à l'absence d'égalisation. Le filtre
   n'est plus appliqué tant que les coefficients valent l'unité.

## Fait le 8/11/2026 (DVB-S QPSK 7/8 30 Mbd à 61,44 Msps, i5-9500, -t 2)

Total **1,59 → 1,13 cœur (-29 %)**, mesuré par thread sur 10 s (`/proc/<pid>/task/*/stat`), l'ancienne
et la nouvelle version l'une après l'autre :

| thread | avant | après | quoi |
|---|---|---|---|
| drx-plsync | 0,61 | 0,29 | boucle de porteuse DVB-S par blocs de 8 (`pll_run`) + valeurs souples |
| drx-timing | 0,36 | 0,35 | inchangé (Gardner 0,20, demi-bande 0,12) |
| principal | 0,33 | 0,29 | lecture du fichier (en direct : la réception USB) |
| drx-filter | 0,16 | 0,11 | filtre adapté : demi-longueur 6 au lieu de 10 (règle 1,6/roll-off, 6 à 24) |
| arrière (Viterbi, RS) | 0,13 | 0,09 | |

- **Boucle de porteuse DVB-S par blocs** (`dvbs.c`, `pll_run`) : la technique de `frame_pll()` du
  DVB-S2 (rotation figée sur 8 symboles, poids `w_phase`/`w_freq` équivalents à la boucle
  symbole par symbole dans le domaine linéaire), en tableaux de réels que GCC vectorise ; rotation
  exacte (puissances de e^-jω, et `pll()` aussi : son développement d'ordre 2 avançait de plus que
  φ à fort décalage et ω s'écartait de la vraie fréquence) ; bruit estimé sur la moyenne du bloc.
  Mêmes paquets et même SNR que l'ancienne boucle aux seuils (1/2 à 3,5-4 dB, 3/4 à 7 dB, 7/8 à
  7,5 dB), avec du bruit de phase et à 30 dB ; meilleure aux grands décalages (verrouille jusqu'à
  0,115 cycle/symbole, l'ancienne décrochait à 0,115 et affichait un SNR faux au-delà de 0,05) ;
  matrice DVB-S 15/15. Le banc en boucle (fichier de 0,16 s) fait une recherche à chaque jonction :
  une partie du gain mesuré (le Viterbi des hypothèses) n'existe pas sur un flux continu ; la boucle
  seule passe d'environ 0,32 à 0,17 cœur.
- **Filtre adapté** (`drx_frontend_cfg_default`, donc mode pont, fichiers et matrices) : demi-longueur
  1,6/roll-off bornée à 6..24 symboles, au lieu de 10 partout : DVB-S2 0,05 → 24, 0,10 → 16,
  0,20 → 8, DVB-S → 6, DVB-C → 11. Une première règle 1,2/roll-off (6 à 0,20, 12 à 0,10) perdait un
  peu près du seuil au débit d'échantillonnage minimal ; balayage (3 tirages, échecs BCH, QPSK 3/4,
  8PSK 3/4, 32APSK 4/5) : à 2 éch./symbole la longueur ne change rien ; à 1,34 éch./symbole et
  0,20, 6 → 7/13/35, 8 → 4/3/24, 10 → 10/5/32 ; à 1,23 et 0,10, 12 → 1/0/27, 16 → 4/0/6,
  20 → 5/0/15. À 32 dB : au plus 0,15 dB sans égaliseur.
- **Non-régression** (`tests/regress.py`, ancienne 6022c9e contre nouvelle, même fichier décodé par
  les deux, 15 ppm d'horloge, décalage de porteuse, au point de la matrice et 1 à 1,5 dB plus bas) :
  - DVB-S, 40 cas (5 rendements, 1,5 / 2 / 2,048 / 2,5 éch./symbole) : 39 identiques, 1 signalé,
    refait sur 5 tirages : sorties identiques ;
  - DVB-C, 40 cas : 37 identiques, 2 mieux, 1 à la limite (256-QAM 1,278 éch./symbole, 20 erreurs
    contre 14 sur 4 050 paquets) ;
  - DVB-S2, 84 cas (7 vecteurs, roll-off 0,20 / 0,10 / 0,05, 2 éch./symbole et minimal) : totaux
    paquets de la nouvelle / de l'ancienne : 0,05 +30 % (minimal) et +8 % (2), 0,10 +6 % et +0,5 %,
    0,20 +1,3 % et +0,2 %, moins d'échecs BCH dans chaque groupe ; 8 cas isolés à quelques erreurs
    près, au seuil, sur un tirage.
  - Bout en bout par `/dev/dvb` (`tests/bridge_file.sh` : démon `-d 1 -F`, `dvbv5-zap`, TS
    enregistré 15 s, en temps réel) : DVB-S 7/8, DVB-C 64-QAM, DVB-S2 8PSK 0,20 et QPSK 0,05 à
    1,17 éch./symbole se verrouillent, sans perte d'échantillons ; mêmes paquets et mêmes erreurs
    (raccords de la boucle du fichier) que l'ancienne version, DVB-S2 0,05 un peu moins d'échecs BCH.
- **Le Viterbi** ne pèse que 0,09 cœur en DVB-S : ses pistes (4 phases, traceback empaqueté)
  servent surtout le DVB-T.
- **Boucle de rythme par lots : écartée.** Compteurs du thread drx-timing (i5-9500) : 2,32
  instructions par cycle, ports 0, 1 et 5 occupés à 47, 51 et 38 % : rien n'est saturé, la boucle
  est limitée par ses dépendances. TDELAY 6 ou 8 au lieu de 4 (plus de symboles en vol) ne change
  rien (0,35 cœur), et un traitement par lots demanderait des transpositions sur le port 5. Le
  thread se partage entre Gardner (0,20 cœur) et la demi-bande (0,12, déjà repliée et vectorisée).
- **Zéro copie en entrée : reporté.** La copie de `drx_mt_push` fait ~490 Mo/s à 61,44 Msps
  (~0,05 cœur) ; le gain réel dépend du thread de réception (conversion d'UHD, mesure de niveau,
  copie : trois passes), à mesurer en direct (le B210 n'était plus branché sur l'hôte le 8/11).
  Piste : recevoir en sc16 et faire conversion, niveau et dépôt dans le bloc en une passe.
- **Viterbi en 4 phases, traceback empaqueté : reportés** (surtout utiles au DVB-T : ~5 % et
  ~1-1,5 % de son CPU, voir `dvbt-viterbi-outer.md`).

## Pistes CPU (gain estimé, charge)

| piste | origine | gain | charge | intérêt |
|---|---|---|---|---|
| Seuil de la demi-bande (constat 2) | mesure | -0,21 cœur à 2 éch./sym | 1 h | **fort** |
| Boucle de rythme par lots (SoA de TDELAY symboles) | agent A | jusqu'à 2× sur drx-timing, environ -0,2 cœur | 1-2 j, risque d'exactitude bit à bit | moyen |
| Zéro copie en entrée (drx_mt_push, append, buffer PL) | agents A et D | -0,1 à -0,15 cœur | 1 j (API sblock + tools/dvbrx.c) | moyen |
| Filtre adapté : rrc_delay 10 → 6, ou FFT overlap-save comme chan.c | nouveau | -0,1 (delay) à -0,18 cœur (FFT) | 2 h (delay, à valider en MER) / 1 j (FFT) | moyen |
| Filtre adapté intégré au canaliseur en mode hub (RRC dans le domaine fréquentiel) | nouveau | supprime drx-filter en hub | 1-2 j | moyen (multicanal) |
| LDPC : recharger les voies convergées | mesure | -18 % de LDPC près du seuil | 2-3 j | faible depuis l'égaliseur (3-4 it.) |
| LDPC : arrêt anticipé fusionné (sans passe bad() finale) | agent D | -5 % de LDPC | 1 j, change les itérations | faible |
| PLL DVB-S par blocs (technique des poids de frame_pll) | agent D (S2) | environ 2× sur le frontal DVB-S | 1 j | faible (CPU non limitant) |
| Viterbi : ordonnancement en 4 phases | agent C | -20 % trellis (DVB-S et T) | 1-2 j | faible pour S, moyen pour T |
| Sortie de traceback empaquetée | agent C | -1 à 1,5 % | 0,5 j | faible |
| Grandes pages / listes LIFO de buffers | agent D | démarrage surtout | 0,5 j | faible |
| Démodulation 64/128APSK par anneaux | agent D | n/a : ce ne sont pas des produits anneau × phase | - | non |

Déjà faits depuis les notes des agents : la compression LDPC (écartée par
l'agent D, elle s'est révélée payante à plusieurs décodeurs), la démodulation par anneaux
pour le 256APSK, les fminf/fmaxf de la boucle de rythme (devenus des branches), et
moins de décodeurs LDPC (3 au lieu de 5, démodulation à part).

## Recommandation

1. Bug de resynchronisation DVB-S (0,5 j) : c'est une perte de service en
   direct après le moindre débordement USB.
2. Seuil de la demi-bande (1 h) : gain immédiat sur toutes les configurations à 2 éch./sym.
3. Égaliseur sur l'en-tête pour le S2 sans pilotes (0,5-1 j), si des flux réels sans
   pilotes à haut débit sont visés.
4. Le reste (boucle de rythme par lots, zéro copie, filtre adapté par FFT) : environ 3-4 jours
   pour -0,4 à -0,5 cœur au total, utile surtout en multicanal.
