# Pistes futures

Liste des chantiers à reprendre, du plus prioritaire au moins prioritaire.
Les pistes d'optimisation CPU détaillées sont dans `doc/optimisation/` :
`analyse-dvbs-s2.md` pour DVB-S/S2/S2X, et les notes des agents pour
T, T2, C et le frontal.

## Robustesse et qualité de réception

- **Bug de resynchronisation DVB-S** : après certaines coupures du flux,
  tous les paquets restent en échec RS jusqu'à la coupure suivante. Voir
  `optimisation/analyse-dvbs-s2.md`, constat 1. Charge : 0,5 jour.
- **Égaliseur pour le DVB-S2 sans pilotes** : apprentissage sur l'en-tête
  (90 symboles) ou piloté par les décisions sur les données. Charge : 0,5-1 jour.
- **DVB-T2 32K** : refaire l'essai radio (les pertes près du seuil sont
  corrigées hors ligne, avec le plafond LDPC à 30/15 et `NFEC` 3, mais pas
  encore revérifiées en radio).
- **DVB-C** : validé en radio (B210 en émission : 256-QAM à 31 dB) et sur
  le câble SFR (602 MHz, 64-QAM, 28,5 dB, le 4/11/2026).
- **DVB-T2 sur le câble SFR (586 MHz), priorité haute** : voir
  `work/cable-sfr.txt`.
  1. Ajouter l'inversion spectrale au T2, comme au DVB-C. Le réseau inverse le
     spectre de ce multiplex ; c'est facile, en essayant les deux sens au P1.
     Le faire aussi pour le DVB-T, par sécurité.
  2. Le L1-post en 64-QAM ne passe pas la FEC sur le vrai signal (cellules à
     environ 56 dB), alors qu'il passe sur gr-dtv. Notre implémentation et
     gr-dtv partagent sans doute un écart à la norme : relire EN 302 755
     §7.3.3 (entrelacement de bits, démux, ordre des bits du 64-QAM pour le
     L1). La capture réelle `~/dvbrx/work/real/t586.fc32` (hôte) sert de
     référence ; `DRX_T2_DEBUG=1` trace les étapes.

## Scan

- **Page web de scan du câble** (et de l'antenne) : balayage de bande SDR
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

## Matériel radio

État actuel : seul UHD (B200/B210, dont le clone LibreSDR) est géré.
`tools/dvbrx.c` contient une `struct radio` propre à UHD (`#ifdef HAVE_UHD`),
plus une source fichier. libhackrf, libbladeRF et SoapySDR sont installés
sur le portable.

- **BladeRF** (libbladeRF) :
  - bladeRF 2.0 micro : AD9361 comme le B210, jusqu'à 61,44 MS/s, 12 bits.
    Mêmes débits que le B210, et l'égaliseur PL sera tout aussi utile aux
    débits élevés ;
  - bladeRF x40/x115 (LMS6002D) : 40 MS/s.
  
  Points d'attention :
  - horodatage des blocs (API sync avec métadonnées), pour combler les trous
    de débordement comme avec UHD ;
  - format SC16Q11, à convertir en fc32 ou à passer directement en sc16.
- **HackRF** (libhackrf) :
  - 20 MS/s au maximum et ADC 8 bits, environ 40-45 dB de dynamique (en
    émission, il plafonnait le DVB-C 256-QAM à 17,8 dB) ;
  - semi-duplex, sans horodatage : détection des pertes par compteur
    d'échantillons seulement ;
  - gains LNA, VGA et amplificateur séparés, à intégrer à l'AGC `-G auto` ;
  - limité aux débits symbole modérés (DVB-S/S2 jusqu'à environ 15 MBd, et C, T, T2).
- **Approche** : extraire une interface radio (ouvrir, accorder, régler le
  gain, démarrer/arrêter, lire avec horodatage et débordements, niveau
  d'entrée), puis un backend par pilote.
  - Variante : un backend SoapySDR couvrirait HackRF, BladeRF, RTL-SDR,
    LimeSDR et Pluto d'un coup. Les métadonnées de temps et de débordement
    varient selon les modules.
  - Charge : environ 1 jour pour l'interface (en gardant UHD identique), puis
    0,5-1 jour par backend natif (ou 1-1,5 jour pour SoapySDR), plus les essais radio.
  - Options de ligne de commande : `-u` devient un argument générique
    (`uhd:type=b200`, `bladerf:`, `hackrf:`), avec le même service
    `dvbrxd@N` et la même AGC.

## Autres

- **Contrôle LNB par ESP32** (objectif d'origine du projet) : protocole
  décrit dans `doc/lnb-protocol.md`.
- Côté émetteur (projet dvbtx) : CPU du banc de filtres à 114 % à 52 MBd,
  sans sous-débit pour l'instant (à suivre par la session dvbtx).
