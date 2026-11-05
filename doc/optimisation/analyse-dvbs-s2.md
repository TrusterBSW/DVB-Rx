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
3. **DVB-S2 sans pilotes : pas d'égalisation.** L'égaliseur n'apprend que sur
   les trames à pilotes. Pour l'APSK sans pilotes à fort débit, on peut
   l'apprendre sur les 90 symboles d'en-tête, ou en mode piloté par les décisions sur les données.
   **Charge : 0,5 à 1 jour.**
   **Fait le 5/11** : apprentissage sur l'en-tête et sur les décisions sûres (erreur
   inférieure à 0,1 dmin²) des 1 024 premiers symboles. Sur chaque trame au début, puis
   une sur 4, puis une sur 16. Les coefficients ne sont gardés que s'ils réduisent
   l'erreur résiduelle d'au moins 0,3 dB par rapport à l'absence d'égalisation. Le filtre
   n'est plus appliqué tant que les coefficients valent l'unité.

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
