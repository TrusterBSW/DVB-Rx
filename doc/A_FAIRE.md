# Pistes futures

Liste des chantiers à reprendre, du plus prioritaire au moins prioritaire.
Les pistes d'optimisation CPU détaillées sont dans `doc/optimisation/` :
`analyse-dvbs-s2.md` pour DVB-S/S2/S2X, et les notes des agents pour
T, T2, C et le frontal.

## Robustesse et qualité de réception

- *(fait le 5/11)* **Bug de resynchronisation DVB-S** : après une coupure, la porteuse
  pouvait se raccrocher à 180° ; les bits inversés échangent 0x47 et 0xB8, la synchro
  paraissait intacte et tous les paquets échouaient. La couche externe suit maintenant
  l'inversion (7 fois 0xB8 sur 8 débuts de paquet).
- *(fait le 5/11)* **Égaliseur pour le DVB-S2 sans pilotes** : apprentissage sur l'en-tête et
  sur les décisions sûres de la boucle de phase (1 024 premiers symboles). Les coefficients ne
  sont appliqués que s'ils battent l'absence d'égalisation d'au moins 0,3 dB.
  - 16APSK 3/4 avec un écho à -10 dB : 0 → 1 413-1 445 paquets sur 1 595.
  - 32APSK 3/4 avec un écho à -8 dB : SNR mesuré 13,7 → 21,7 dB.
  - Sans écho, rien ne change. S2X sans pilotes à 3 dB de Shannon : 30/55 contre 28/55 sans
    égaliseur, aucune régression ; avec pilotes : 55/55.
  - Reste : la récupération de porteuse sans pilotes à faible marge (16APSK 26/45 à 3/5,
    8PSK courtes, 256APSK 116/180...), qui échoue avec ou sans égaliseur.
- **DVB-T2 32K** (5/11) : en direct sur le 586 MHz SFR (32K étendu 256QAM 5/6, sans
  entrelacement temporel), 120 s par tvheadend : 0,3 % de blocs FEC en échec (bouffées du
  bloc TNT), 17 paquets TS en erreur sur 4 M, aucun échantillon perdu, 90 % d'un cœur. Rejoué
  en temps réel avec du bruit de part et d'autre du seuil : aucun échantillon perdu (environ
  170 % de CPU). Le seuil mesuré est vers 22-23 dB de C/N dans la bande, soit environ 2 dB de
  plus que la table de la norme (256QAM 5/6) : l'écart du démodulateur T2 au seuil théorique
  serait à mesurer sur signal synthétique.
- **DVB-C** : validé en radio (B210 en émission : 256-QAM à 31 dB) et sur
  le câble SFR (602 MHz, 64-QAM, 28,5 dB, le 4/11/2026).
- **DVB-T2 sur le câble SFR (586 MHz)** : reçu depuis le 4/11/2026, avec France 2
  UHD et Test UHD (commits 26a58cd et b21c349 : inversion, horloge mesurée par
  l'espacement des P1, mode HEM), puis def6dce : filtre analogique calé sur le
  canal demandé et AGC cherchant le meilleur SNR (le plafond fixe `-G auto:44`
  n'est plus nécessaire). Blocs FEC perdus : environ 0,6 % à 25,7 dB (470 → 230
  en 40 s). Reste à améliorer : un filtre RF de bande avant le B210 (le
  DOCSIS pèse encore sur l'ampli d'entrée et le mélangeur) ; le suivi d'horloge
  et de phase sur les porteuses des bords. (L'inversion spectrale du DVB-T est faite
  depuis le 5/11.) La
  répartition du gain dans l'AD9361 (moins d'ampli d'entrée, plus de gain après
  le filtre) n'est pas accessible par UHD (un seul élément « PGA », et l'AGC
  matérielle telle que UHD la configure affame le convertisseur) : il faudrait
  écrire dans les registres de l'AD9361. Détails dans `work/cable-sfr.txt`.

## Scan

- **dvbrx-web** (`apps/dvbrx-web`) fait le balayage FFT, le scan complet et l'export
  tvheadend. Reste à faire : le **DAB** ; dans la réponse `probe`, la FFT, la garde et le
  pilot pattern du DVB-T/T2, la **liste des PLP** d'un multiplex T2 (L1-post) et les
  paramètres effectifs du DVB-T (hiérarchie) ; le contrôle **DiSEqC** (commutateur, moteur)
  dans la page d'alignement et le scan satellite.
- *(historique)* **Page web de scan du câble** (et de l'antenne) : balayage de bande SDR
  (fenêtres de 56 MHz, classement QAM/OFDM par la raie cyclique et le
  préfixe cyclique, estimation du débit symbole), puis accrochage et lecture
  des tables (NIT, SDT) pour sortir les fréquences et les paramètres DVB-C,
  T et T2 (et **DAB** en bande III, 174-240 MHz : nouveau démodulateur OFDM
  1,536 MHz à écrire), avec un **fichier de multiplex pour tvheadend** (ou
  dvbv5). Base existante : `~/bandscan.py` sur l'hôte (premier balayage SFR :
  `work/cable-sfr.txt`). Le service doit libérer le B210 pendant le balayage
  (ou le hub doit exposer un mode scan). Charge : 2-3 jours sans DAB, plus
  2-4 jours pour un démodulateur DAB.
- Le DVB-C gère maintenant l'inversion spectrale (auto) et prend la
  modulation demandée comme un indice (fréquent sur les listes anciennes).

- **DVB-T sur le bloc TNT du câble SFR (538-594 MHz), pertes à 24-25 dB de SNR**
  (2026-11-05) : des bouffées de bruit large bande arrivent avec le bloc TNT.
  - **Forme** : durée de l'ordre de 1 ms, environ -20 dBc dans le canal, uniformes en fréquence
    et dans la durée du symbole, sans composante de phase cohérente. Un symbole touché tombe à
    environ 18 dB de MER, sous le seuil du 64QAM 5/6.
  - **Établi** : un récepteur DVB-T indépendant (Python, sans code de dvbrx) voit les mêmes
    symboles dégradés. Elles sont présentes en même temps dans les intervalles entre canaux TNT
    (566, 574, 590 MHz), absentes entre canaux DVB-C (606 MHz) et dans une bande vide
    (710-730 MHz). Elles arrivent par le coaxial (presque rien sur l'entrée RX2 non câblée) et
    ne dépendent pas de la charge de l'hôte.
  - **Dans le signal reçu** : leur rapport au signal ne change pas quand le gain du B210 varie
    de 38 à 50 dB. Le MER de base (environ 25 dB) varie à peine non plus : il est limité par le
    signal lui-même, pas par le bruit propre du B210 (un préampli n'apporterait rien). Le bloc
    TNT est remodulé par SFR (fréquences et noms SFR) : la source probable est le modulateur
    multicanal ou la tête de réseau (écrêtage du bloc, par exemple).
  - **Seuil de dvbrx** (bruit blanc, 8K 64QAM 5/6) : 1 paquet perdu sur 2 747 à 18,8 dB de MER
    mesuré, 39 % à 18,5 dB, quasiment tous à 18,3 dB ; environ 18,3 dB de C/N dans la bande
    (norme : 19,3 dB). Un téléviseur ne ferait pas mieux sur ces symboles ; ses pertes passent
    sans doute inaperçues (masquage d'erreurs, environ 0,05 % des paquets).
  - **Fait** : lissage de l'estimation de canal (MER +1,1 dB, pertes divisées par 1,8 sur une
    capture de 20 s). Au-dessus de 53 dB de gain, l'entrée du B210 sature.
  - **Trace** : `DRX_T_DEBUG=1` (par symbole, et erreur par seizième de bande quand le MER chute).

## Matériel radio

- **Fait** (2026-11-05) : interface de pilote SDR (`libdvbrx/tools/sdr.h`), avec UHD
  (`sdr_uhd.c`) et bladeRF par libbladeRF (`sdr_bladerf.c`).
  - `-u [PILOTE:]ARGS` : `uhd:type=b200` (ou sans préfixe, comme avant), `bladerf:` (premier
    bladeRF) ou `bladerf:*:serial=...`.
  - Le Makefile compile chaque pilote dont la bibliothèque est installée.
  - bladeRF : entrées `-A RX1` (par défaut) ou `-A RX2`. Échantillons SC16 Q11 horodatés :
    les trous sont comblés comme avec UHD. Le gain, le débit et le filtre viennent des plages
    annoncées par la carte.
  - Le multicanal (hub) est au-dessus de l'interface : il marche tel quel avec le bladeRF 2.0
    (même puce et mêmes débits que le B210). Il reste à le valider sur la carte.
- **À valider avec la carte** (bladeRF 2.0 micro xA4) :
  - L'image FPGA `hostedxA4.rbf` (v0.15.x pour libbladeRF 2024.05) n'est pas dans les dépôts
    de l'hôte. À télécharger chez Nuand dans `/usr/share/Nuand/bladeRF/`, sauf si la carte la
    charge depuis sa flash.
  - Puis réception autonome, service `dvbrxd` (`-u bladerf: -A RX1`), dvbrx-web, AGC et
    multicanal.
- **HackRF** (libhackrf, 5/11) : pilote `sdr_hackrf.c`, avec `-u hackrf:` ou `-u hackrf:NUMÉRO_DE_SÉRIE`.
  - **Débit** : 2 à 20 MS/s. Un seul canal : `-n`, `-W` et `-m` sont refusés.
  - **Lecture** : file circulaire de 0,4 s depuis le fil de libhackrf ; un débordement est
    signalé.
  - **Pertes** : pas d'horodatage, elles ne sont pas comblées. Les débordements internes de la
    carte (compteur du M0) sont comptés.
  - **Composante continue** : coupe-continu d'environ 300 Hz.
  - **Gain** : de 0 à 116 dB (ampli 14 au-delà de 102 dB, LNA par 8, VGA par 2) ; meilleure
    combinaison à 1 dB près.
  - **Filtre analogique** : la valeur de la table du MAX2837 immédiatement supérieure.
  - **Antenne** : alimentation coupée à l'ouverture et à chaque démarrage.
  - **dvbrx-web** : le statut annonce `rate_max` et `multi`, et le scan rapide fait des
    fenêtres de 19 MHz.
  - **Testé** avec une fausse libhackrf (file, repliement, coupe-continu, débordement, délai,
    gains). **Reste à tester sur la carte.**

## Autres

- **Contrôle LNB par ESP32** (objectif d'origine du projet) : protocole
  décrit dans `doc/lnb-protocol.md`.
- Côté émetteur (projet dvbtx) : CPU du banc de filtres à 114 % à 52 MBd,
  sans sous-débit pour l'instant (à suivre par la session dvbtx).
