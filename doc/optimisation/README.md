# Optimisation CPU de dvbrx (3/11/2026)

Mesures sur l'hôte 10.5.1.2 (i5-9500, AVX2, GCC 14), pipeline `dvbrx -F` sur
fichier, cœurs nécessaires en temps réel = temps CPU / durée du signal.

| Signal                                   | Avant | Après |
|------------------------------------------|-------|-------|
| DVB-T 8K 64-QAM 5/6, 10,24 Msps          | 1,25  | 0,44  |
| DVB-T2 8K 64-QAM 3/4, 10,24 Msps         | 0,94  | 0,29  |
| DVB-C 256-QAM 6,9 Mbd, 13,8 Msps         | 0,98  | 0,41  |
| DVB-S2 8PSK 3/4 pilotes, 27,5 Mbd, 2 sps | 1,86  | 1,23  |

Notes de reprise par domaine (ce qui a été fait, gains, impasses mesurées,
idées non essayées, commandes de mesure) :

- `frontend-dvbc.md` : frontal (filtre adapté, boucle de rythme) et DVB-C.
  Point ouvert : retard de la boucle de rythme (TDELAY, 4 retenu ; 2 plus
  rapide mais un cas réel DVB-C décalé donne 23995 erreurs, non expliqué) ;
  le cas DVB-C 256-QAM 2,226 sps de la matrice échoue ~1 fois sur 2 quel que
  soit le retard (bruit des générateurs non semé).
- `dvbt2-fft.md` : DVB-T2 et FFT.
- `dvbt-viterbi-outer.md` : Viterbi, dépoinçonnage, couche externe (RS,
  désentrelaceur), démappeur DVB-T.
- `dvbs2-pl-ldpc-demap.md` : synchro PL, LDPC, démappeur, BCH, BB, pipeline.

Banc : fichiers dans `work/bench/` (`s2_8psk34.fc32` dure 0,80 s à 27,5 Mbd),
profilage `sudo perf record -g` sur l'hôte (perf_event_paranoid = 4).

## DVB-T2 près du seuil : pertes d'échantillons (3/11/2026)

Symptôme en radio réelle (32K étendu 256-QAM 2/3, ~15 % de blocs FEC en
échec) : 1,6 M échantillons perdus. Reproduit hors ligne en rejouant
`work/real/dvbt2_def.fc32` bruité (`tests/channel.py --snr 18.0/18.3/18.5`,
10,24 Msps) en temps réel (`dvbrx -F ... -R`) : 4,2 M (18,3 dB) et 8,6 M
(18,0 dB) échantillons perdus en 30 s, aucun sur signal propre.

Cause : près du seuil, chaque lot LDPC (jusqu'à 32 blocs en parallèle)
tourne jusqu'à la convergence de son bloc le plus lent, donc jusqu'au
plafond d'itérations ; à 50 itérations le décodage demandait ~2,3 cœurs,
plus que les 2 threads FEC ; la démodulation attendait, la file
d'échantillons débordait (pertes par blocs entiers de 65536).

Correctif (dvbt2.c) : plafond 30 itérations (CPU -35 %, mêmes blocs
décodés à 18,3/18,5 dB), 15 quand une autre trame attend déjà le FEC
(dégradation douce au lieu de pertes), 3 threads FEC. Résultat : 0 perte
dans tous les cas, plus de paquets livrés près du seuil.

| SNR | plafond 50 | 30 | 20 | 15 | (échecs / 2290 blocs, CPU utilisateur pour 3 s)
|-----|------------|----|----|----|
| 18,0 dB | 827, 7,2 s | 1206, 5,0 s | 1884, 3,9 s | 2265, 3,5 s |
| 18,3 dB | 24, 7,2 s | 26, 4,8 s | 46, 3,7 s | 263, 2,9 s |
| 18,5 dB | 5, 6,8 s | 6, 4,5 s | 6, 3,5 s | 6, 3,0 s |

Piste : arrêt anticipé par bloc dans le décodeur LDPC (les blocs convergés
d'un lot continuent aujourd'hui à itérer) ; même effet probable en DVB-S2.

## DVB-S2X 256APSK à 35-45 Mbd sur un B210 (3/11/2026)

Objectif : 256APSK 135/180 roll-off 0,2 au débit maximal d'un B210
(61,44 Msps). Mesures sur l'hôte (i5-9500, 6 cœurs, VM et service dvbrxd@0
actifs), signal synthétique à Es/N0 24 dB, fichier bouclé 8 fois, décodage
aussi vite que possible (« × temps réel ») puis relecture temps réel `-R`
pendant 25 s.

| Étape | 35 Mbd | 40 Mbd | 45 Mbd |
|-------|--------|--------|--------|
| départ | ×0,85, pertes en `-R` | SNR 20,2 dB, 11,5 it. | refusé (< 1,5 sps) |
| démapping groupé (`minima_grp`) | ×0,88 | | |
| LDPC à messages compressés | ×1,20, 0 perte | ×0,71 | refusé |
| demi-bande ×2 avant la boucle de rythme | ×1,23, SNR 24,2 dB, 4,2 it. | ×1,19, 23,9 dB | ×1,11, 23,8 dB, 0 perte en `-R` (259 Mbit/s TS) |

- **Démapping 6 à 8 bits** (`demap.c`, `minima_grp`) : arbre de minimums par
  groupes de 16 points, 8 symboles à la fois ; 256APSK 46,8 → 20,1 ns par
  symbole, LLR identiques.
- **LDPC limité par la mémoire** : le débit ne bougeait pas de 2 à 6 threads
  (×0,9), `update` coûtait 3 fois plus de cycles à 5 threads qu'à 1. À
  135/180, les messages occupent 8,5 Mo par décodeur (degré 17 × 16 200 nœuds
  × 32 voies), relus et réécrits à chaque itération : le cache L3 (9 Mo) ne
  suit pas. Le prefetch des nœuds de variable n'y change rien. Les messages
  sont maintenant stockés compressés (min0, min1, index du minimum et bits de
  signe, 7 par octet avec un bit de garde) : 6 vecteurs par nœud au lieu de
  17, à partir du degré 9 (`COMP_MIN_DEG`). C'est bit exact (57 codes, AVX-512,
  AVX2 et C générique, harnais comparant décisions et itérations).
- **Frontal sous 2,5 échantillons par symbole** : l'interpolation cubique sur
  la sortie du filtre adapté coûtait environ 1,7 dB de MER à 1,75 sps,
  environ 4 dB à 1,54 sps, et le frontal refusait les débits sous 1,5 sps. La sortie du filtre est
  maintenant interpolée ×2 (demi-bande Kaiser, 16 coefficients par côté,
  environ 50 dB de réjection jusqu'à 0,44 fs) avant la boucle de Gardner :
  le SNR mesuré retrouve celui du signal, et le LDPC converge en 4,2
  itérations au lieu de 5,7. Le coût est d'environ 0,15 cœur dans le thread de rythme.
- **50 Mbd** (1,23 échantillon par symbole) : décodage correct, SNR mesuré de 23,8 à 24,0 dB et 4,2 à 4,5 itérations,
  mais 0,96× le temps réel au roll-off 0,10, 0,94× à 0,20 et 0,77× à 0,05 (un étage
  séquentiel à identifier), machine chargée. Le roll-off 0,20 occupe 60 MHz,
  au-delà des 56 MHz analogiques du B210.
- **Suite** : à 45 Mbd il reste environ 10 % de marge avec la machine chargée.
  Les prochains postes sont le démapping (1,7 s CPU pour 1,9 s de signal),
  la contention mémoire résiduelle du LDPC et le thread de rythme (0,6
  cœur). Côté émission, le dvbtx de l'i3 sature déjà vers 40 Mbd.
