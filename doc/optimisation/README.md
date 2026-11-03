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
