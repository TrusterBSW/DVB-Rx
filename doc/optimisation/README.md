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
