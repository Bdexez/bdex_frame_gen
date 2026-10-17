# Analyse des saccades — 16 octobre 2026

Demande : observer les enregistrements, identifier des améliorations puis les
implémenter et les vérifier une par une, en poursuivant les recherches tant
que l'utilisateur ne demande pas l'arrêt. Les demandes de confirmation sont
à éviter ; l'accès complet est actif et l'approbation désactivée pour cette
session. Les corrections et installations locales autorisées continuent
sans nouvelle demande de permission.

## Sources examinées

Sept dossiers dans `~/.cache/bdex-framegen/recordings`, chacun de 150 images
853×480 (1 050 images au total). Échantillons visuels aux indices 0, 45, 90,
135 dans chaque séquence, puis différences entre toutes les images voisines.

| Jeu et heure du dossier | i/s dans DONE | Observation visuelle |
| --- | ---: | --- |
| Cyberpunk 2077, 15:03:44 | 36,20 | Menu au début, rotations et flou de mouvement marqué |
| Cyberpunk 2077, 15:03:56 | 35,99 | Rotations, fortes variations de netteté déjà dans les sources |
| Oblivion Remastered, 15:25:56 | 30,02 | Extérieur, feuillage et végétation fine |
| Oblivion Remastered, 15:26:21 | 30,21 | Architecture, pavés et rotation devant la prison |
| PRAGMATA, 15:12:58 | 30,29 | Rotations autour du personnage et des consoles |
| PRAGMATA, 15:13:13 | 30,30 | Personnage proche, parallaxe, effet transparent |
| PRAGMATA, 15:13:42 | 30,18 | Rotations, grands contrastes et structures fines |

Ces fichiers sont les images réelles du jeu, pas une vidéo horodatée de la
sortie affichée. `DONE` conserve seulement une estimation finale des i/s.
Leurs dates de fichier reflètent l'écriture asynchrone et ne peuvent pas
servir d'horloge des images. On ne peut donc pas attribuer les à-coups
ressentis à une cause unique avec ces captures seules.

Réglages trouvés : `quality`, interpolation ×4, présentation automatique,
HUD 2, pilotage par le panneau. Contexte écran consigné dans MEMOIRE :
143,912 Hz, fréquence fixe, Hyprland et RTX 3060 Ti.

## Correction 1 : le mouvement entre deux images réelles

Le remplissage de chaque rafraîchissement alterne quatre et cinq images à
30 i/s sur 144 Hz. Les deux images réelles arrivent donc à quatre ou cinq
rafraîchissements d'écart. L'ancien calcul de `t` divisait le temps entre
la sortie et la réelle courante par l'intervalle du contenu, environ
33,33 ms. Le décalage de phase entre les deux réelles se concentrait sur
le premier pas de mouvement de la paire suivante.

`phase_smooth=1` utilise l'intervalle entre les rafraîchissements prévus des
deux réelles comme dénominateur de l'interpolation. Le mouvement rejoint
progressivement la réelle courante ; aucune présentation n'est déplacée.
L'horloge du contenu utilisée par le flux et la détection de répétition
reste intacte. Pas de passe GPU ni de retenue supplémentaire.

Application : couche, interpolation pure avec plan de présentation valide.
L'historique est invalidé à la première paire, après une pause, un retour
sans plan ou un saut d'horloge. Hybride, extrapolation et `dedup` gardent
leur calcul. Réglage disponible à chaud dans le panneau, section Cadence.

### Mesures et vérifications

Simulateur existant, 600 images d'un jeu à 30 i/s, écran 143,912 Hz,
variations de l'heure de présentation et du délai GPU :

| Mesure du pas de mouvement | Avant | Après |
| --- | ---: | ---: |
| Écart RMS entre pas de contenu et pas d'affichage | 1,277 ms | 0,625 ms |
| Plus grand écart | 5,539 ms | 1,385 ms |
| Heures et nombre de présentations | identiques | identiques |

Tests réussis : 84 tests unitaires, clés des options, logique du panneau,
shader `frame_state` sur GPU (phase, horloges de contenu, répétitions et
prédictions), invariant du matcher, formats, déterminisme, intégration ×2,
modifications à chaud. Builds Release x86_64 et i386 réussis.

Le simulateur mesure un gain sur ce défaut précis. Cela ne prouve pas que
les saccades ressenties en jeu ont disparu ; ni le réveil du worker ni le
compositeur ne sont rendus plus réguliers par cette correction.

## Correction 3 : suivre l'écran autour d'un rapport entier et au-delà

`refresh_fill` ne s'activait que si le nombre de rafraîchissements par image
du jeu dépassait le multiplicateur de plus de 2 %. Avec ×4, le chemin fixe
restait donc actif à 36–37 i/s sur 144 Hz : plusieurs origines GPU, et
parfois plus de sorties que de rafraîchissements. Il suit maintenant la
fréquence de l'écran tant que deux sorties par image du jeu au minimum
suffisent et que le plafond alloué peut la remplir. Le choix ×4 devient
par exemple une alternance ×3/×4 à 36,2 i/s ; ×8 est aussi couvert.
L'opt-out reste `refresh_fill=0`, l'hybride/extrapolation restent exclus.

Simulateur avec délais GPU variables, écran sans grille mesurable
(grille virtuelle après correction), `phase_smooth` actif des deux côtés :

| Base du jeu | RMS du pas avant | Après |
| --- | ---: | ---: |
| 36 i/s | 1,199 ms | 0,093 ms |
| 36,2 i/s | 1,207 ms | 0,312 ms |
| 37 i/s | 1,458 ms | 0,628 ms |
| 40 i/s | 2,003 ms | 0,984 ms |
| 48 i/s | 2,457 ms | 0,094 ms |
| 50 i/s | 2,439 ms | 0,925 ms |

Tests unitaires (86), options/panneau, intégration et `gamescope_limit`
réussis. Ce dernier vérifie le maintien et la levée de la limite Steam.

Essais matériels : trois A/B alternés, démo 36,2 i/s ×4, préréglage quality,
1280×720, gamescope headless 144 Hz avec sa couche WSI. Contexte de panneau reproduit dans un
dossier d'état privé (rafraîchissement 144 Hz, heartbeat de session). Les
deux premières secondes sont exclues de `cadence_report.py`. Médianes des
trois essais : affichées 143,3 → 142,6 i/s, tenues 6,3 → 6,3/s, latence
réelle 32,6 → 33,9 ms ; rattrapages sans génération dans les stats stables
4 → 3 par essai. Pas de gain matériel net établi avec ces mesures : les
retours d'affichage de gamescope varient (hors plan après : 11–51 %).
Ce banc ne mesure pas directement les trajectoires dans les images.
Logs et script de reproduction temporaires :
`/tmp/bdex-refresh-cap-bench/`, `/tmp/bdex_refresh_cap_bench.py`.

## Correction 4 : résorber un petit retard sans jeter toute la paire

Le journal debug du même banc à 36,2 i/s montre des rattrapages avec
**0,000 ms d'attente du worker**, intervalle **27,62 ms**, délai estimé
de première sortie **7,19 à 8,36 ms**, déclenchés par huit jobs tardifs
consécutifs. Le GPU suit ; c'est la présentation qui garde un décalage.
L'ancien rattrapage montrait alors uniquement la réelle et réinitialisait
la continuité de phase de l'interpolation.

`catchup_smooth=1` retire une sortie et garde les intermédiaires lorsque :
interpolation pure avec horloge adaptative et grille valide, historique
valide, `phase_smooth` et `pace_plan` actifs, pas de `dedup`, attente worker
≤ un quart de l'intervalle et première sortie estimée prête en moins de la
moitié de celui-ci. Dans les autres cas, rattrapage immédiat conservé.
Pour ×4 réduit à ×3, la paire continue en trois pas au lieu d'un bond
direct à la réelle. Cela rend de la place sur la grille plus progressivement.

Tests : 87 tests unitaires (conditions de repli et progression du mouvement),
options/panneau, intégration, `gamescope_limit`, shader d'état GPU avec le
job raccourci ×4 → ×3. Tous réussis.

Trois nouveaux A/B matériels alternés (même banc avec couche WSI que ci-dessus) :

| Mesure par essai | Avant | Après |
| --- | --- | --- |
| Jobs entièrement sans génération | 0 / 3 / 0 | 0 / 0 / 0 |
| Sorties retirées progressivement | 0 / 0 / 0 | 6 / 4 / 1 |
| Médiane de la latence réelle | 32,4 ms | 32,7 ms |
| Médiane d'images affichées/s | 144,0 | 143,4 |
| Médiane de tenues/s | 2,1 | 2,8 |

Les traces confirment que les nouveaux rattrapages gardent trois sorties,
sans attente du worker. Le banc ne démontre pas de gain global d'i/s ni de
répétitions ; la correction porte sur le saut volontaire de contenu lors
du rattrapage. Le ressenti sur le jeu reste à mesurer. Opt-out à chaud :
`catchup_smooth=0`. Logs temporaires : `/tmp/bdex-catchup-bench/`,
trace de diagnostic `/tmp/bdex-catchup-trace/`, script
`/tmp/bdex_catchup_bench.py` ; référence avant correction
`/tmp/bdex-before-catchup/`.

Vérification du chemin proche de celui du joueur : trois A/B de plus en
**X11/Xwayland sans couche WSI**. Le premier lancement retirait
`ENABLE_GAMESCOPE_WSI` seulement avant gamescope, qui le remettait dans
l'environnement du jeu. Le lancement corrigé le retire **dans le processus
enfant**, ainsi que `WAYLAND_DISPLAY`. Journaux vérifiés : aucune ligne
`[Gamescope WSI]`, surface en présentation immediate, rafraîchissement
non mesuré, grille virtuelle fournie par le contexte du panneau.

| Mesure X11 sans WSI | Avant | Après |
| --- | --- | --- |
| Jobs entièrement sans génération, trois essais | 3 / 4 / 0 | 0 / 0 / 0 |
| Sorties retirées progressivement | 0 / 0 / 0 | 0 / 0 / 7 |
| Médiane d'images affichées/s | 143,0 | 143,7 |
| Médiane de tenues/s | 1,5 | 1,5 |
| Médiane de la latence réelle | 30,3 ms | 30,7 ms |

Ce résultat confirme la conservation des intermédiaires lors des nouveaux
rattrapages sur ce chemin aussi. Il ne prouve pas la disparition de toutes
les saccades en jeu. Logs/script : `/tmp/bdex-catchup-x11-bench/`,
`/tmp/bdex_catchup_x11_bench.py`.

Les lancements du banc gamescope se terminent par un signal au démontage,
après `Primary child shut down!`, avant comme après la correction (comportement
déjà signalé par `latency_bench.sh`). Les mesures portent sur les dix secondes
où la démo et les retours fonctionnent ; elles ne valident pas ce démontage.

## Correction 5 : réagir aux changements durables de cadence

Le rejeu horodaté permet maintenant de distinguer le rythme demandé des
retards ajoutés par la couche ou la machine de test. Un banc contrôlé passe
de 30 à 60 puis revient à 30 i/s (100 sources à chaque rythme), ×4, quality,
1280×720 sur gamescope headless 144 Hz, en X11 sans sa couche WSI. Les
images de Cyberpunk sont réutilisées pour la charge de traitement ; **ces
temps sont synthétiques**, les captures originales n'ont pas d'horloge.
Ils ne décrivent donc pas le rythme de la partie enregistrée.

À la hausse des i/s, l'ancien lissage garde un intervalle trop long pendant
plusieurs images. La génération demande trop de sorties, le worker finit
par bloquer le jeu et le rattrapage supprime toute une paire. Trace avant :
16 puis 27 ms d'attente alors que le jeu vient de passer à 16,67 ms/image.

`pacing::IntervalFilter` garde le lissage 0,8/0,2, sauf lorsque deux
intervalles se ressemblent à ±10 % et s'écartent tous deux de plus de 25 %
de l'estimation, du même côté. Leur moyenne devient alors la nouvelle
estimation. Un intervalle invalide (hors 0,5..500 ms) efface le candidat.
La couche et les captures Linux/Windows utilisent cette règle commune.
Le temps de contenu exact du shader reste celui de chaque source.

Cinq A/B alternés, deux premières secondes conservées dans les journaux
source et première seconde exclue du rapport d'affichage. Médianes des
cinq essais ; la ligne de transition mesure les indices source 100..124 :

| Mesure | Avant | Après |
| --- | ---: | ---: |
| Latence réelle des 25 premières images après la hausse | 28,76 ms | 19,55 ms |
| Plus grande attente worker par essai | 27,12 ms | 13,31 ms |
| Rattrapages entièrement sans génération par essai | 4 | 2 |
| Décalage cumulé du rejeu dû à sa surcharge | 11,07 ms | 0 ms |
| Images affichées/s | 142,9 | 143,5 |
| Tenues/s | 2,2 | 1,1 |
| Latence réelle sur toute la partie stable | 32,8 ms | 31,6 ms |

Rattrapages sans génération, essais individuels : **6/3/4/4/4 → 2/2/2/2/2**.
Le gain local à la transition est présent dans les cinq paires ; la sortie
reste soumise au compositeur et ces chiffres ne garantissent pas le
ressenti du jeu. L'ancien filtre reste identique sur les scénarios de
gigue, temps alternés et pics isolés vérifiés par les tests. Le remplacement
par les intervalles bruts seuls a été écarté : au plafond ×5, l'alternance
court/long perd des sorties dans un modèle de comptage. La confirmation
sur deux intervalles conserve le lissage dans ce cas.

Référence avant : `/tmp/bdex-before-interval-response/` ; reproduction :
`/tmp/bdex_interval_response_bench.py` ; journaux et mesures structurées :
`/tmp/bdex-timed-transition-bench/summary.json`. Comme les autres essais,
gamescope termine par un signal après fermeture du processus enfant ;
les mesures et les comparaisons concernent uniquement sa partie active.

Vérification : 92 tests unitaires, clés des options, panneau, intégration,
capture Linux (quatre modes), rejeu de qualité et des horodatages, limite
Steam, contrôle MinGW x64/i686 et liaison de la capture réussis. Correction
installée en Release 64/32 bits ; bibliothèques, démo et capture identiques
aux builds, configuration conservée (SHA256 comparé). L'exécution Windows
sur matériel reste à vérifier.

## Correction 6 : ne pas confondre durée des paires et accélération (16–17/10)

### Observation des sorties de Cyberpunk

120 sources consécutives de la capture 15:03:56, synthèse ×4/quality sans
espacer les sources : suivi de points aller-retour sur A, les trois
intermédiaires et B. 111 paires ont au moins huit points exploitables,
18 781 trajectoires retenues. L'écart-type médian des quatre pas normalisés
est 0,0052 (pas idéal 0,25) ; 0,16 % des trajectoires retenues font plus
de la moitié du mouvement au dernier pas. Ce suivi exclut les points
illisibles, occultés ou incohérents : ce n'est pas une validation de toute
l'image. Il n'indique pas de téléportation générale dans la synthèse de ces
points, malgré le flou important des sources. Les quelques progressions
plus courbes ne permettent pas de distinguer accélération réelle et temps
d'image variable : les captures originales n'ont pas d'horodatage.

Inspection visuelle de la paire 95→96 et ses trois intermédiaires :
`/tmp/bdex-cp-pair96.png`. Script de suivi : `/tmp/bdex_track_motion.py`
(OpenCV installé uniquement dans `/tmp/bdex-motion-deps`), résultats
`/tmp/bdex-cp-motion.json`. Les 72 premières sorties comparées après la
modification du shader, à cadence constante, sont identiques octet par
octet à la référence précédente.

### Défaut reproduit avec une horloge connue

`interp_accel` et `extrapolate_accel` comparaient deux déplacements comme
s'ils couvraient la même durée. Une vitesse constante produit pourtant un
déplacement deux fois plus grand sur 33 ms que sur 16,5 ms ; leur différence
n'est pas une accélération. Avec `r = I_courant/I_précédent`, l'accélération
en unités de l'intervalle courant au carré est :

`a = 2r/(1+r) × (−B + rB0)`

Le moteur fournit ce rapport à la carte d'accélération et à l'extrapolation
lorsque l'horloge du contenu est utilisée (`frame_timing`, ou `content_clock`
avec heures d'affichage). Trois protections :

- l'horodatage ne crée pas une accélération plus grande que celle observée
  dans les vecteurs et n'en inverse pas le sens ; cela protège le contenu à
  pas fixe présenté de façon irrégulière ;
- le seuil de bruit suit l'amplification de l'erreur de quantification du
  vecteur précédent ; sinon, son erreur d'un pixel devenait plusieurs pixels
  et passait pour une accélération ;
- le contrôle de visibilité prédit le pas précédent avec `B/r`, et
  l'historique temporel invalide supprime la courbure.

Intervalles égaux à ±0,5 %, horloge non utilisée ou `dedup` : expression
historique. Avec `dedup`, les durées des images distinctes sont sur GPU ;
les intervalles CPU ne peuvent pas les remplacer. Le rapport appartient à
la paire de flux gardée par la passe, y compris dans la sonde qui utilise
la paire précédente avant que `flowBegin` ne la réinitialise. Aucune passe
GPU supplémentaire.

### Comparaison de qualité

Même filtre de cadence des deux côtés : le banc hors ligne utilise
maintenant `pacing::IntervalFilter`, comme les deux produits. Référence
isolée compilée avec le rapport forcé à 1 ; paramètres identiques,
quality ×4, `frame_timing=1`, accélération 1, douze sources par cas,
`--jitter 0.6 --spacing 4`. Les scores couvrent toutes les sorties, réelles
comprises (plafond 60 dB du banc). Le panoramique est à 384×256 ; les deux
scènes de jeu sont à 1280×720 avec personnage, parallaxe et flou.

| Cas à temps variables | PSNR avant → après | Gain | Scintillement avant → après |
| --- | --- | ---: | --- |
| Panoramique, interpolation | 36,63 → 37,41 dB | +0,78 dB | baisse de 0,80 |
| Panoramique, extrapolation | 31,85 → 35,42 dB | +3,57 dB | baisse de 5,89 |
| Rotation, interpolation | 37,90 → 38,01 dB | +0,11 dB | baisse de 0,21 |
| Rotation, extrapolation | 33,58 → 34,32 dB | +0,74 dB | baisse de 1,56 |
| Coup de souris, interpolation | 43,94 → 44,18 dB | +0,24 dB | baisse de 0,008 |
| Coup de souris, extrapolation | 38,41 → 38,92 dB | +0,51 dB | baisse de 0,054 |

Pour le contenu à pas fixe avec seuls les appels irréguliers, différences
globales de PSNR entre −0,011 et +0,009 dB ; le panoramique est inchangé.
La première formule sans garde dégradait ce cas de 0,25 dB : elle a été
remplacée. Les gains de +1,34/+4,15 dB du premier essai ne sont pas les
chiffres de la comparaison finale : durée et filtre de cadence différaient.
Mesures finales : `/tmp/bdex-accel-timing-eval/aligned-final-results.log`,
fichiers `*-aligned-before.json` / `*-aligned-after.json` ; reproduction :
`/tmp/bdex_accel_timing_final_eval.py`. Référence compilée dans
`/tmp/bdex-accel-final-baseline/build`.

### Coût et cadence

Cinq A/B alternés, 2560×1440, quality ×4, 144 Hz, rejeu contrôlé
30→60→30 i/s avec images de Cyberpunk pour la charge, X11 sans WSI :

| Médiane des cinq essais | Avant | Après |
| --- | ---: | ---: |
| Affichées/s | 143,7 | 143,7 |
| Tenues/s | 1,0 | 1,1 |
| Latence réelle médiane | 35,1 ms | 35,0 ms |
| Affichées hors plan | 0,3 % | 0,3 % |
| Flux GPU par source dans les périodes à 30 i/s | 1,96 ms | 1,95 ms |
| Synthèse GPU de tous ses intermédiaires | 6,07 ms | 6,00 ms |

Coût comparable sur ce banc, pas de gain global de cadence revendiqué.
La première série sans tolérance aux écarts minimes donnait davantage de
tenues (médianes 1,5→3/s) et des retours moins bien alignés au plan, sans
surcoût GPU net. La version finale garde le chemin historique à ±0,5 %
et les cinq nouveaux A/B ci-dessus sont comparables. Ce banc reste sensible
à la phase du compositeur et aux réveils du retour d'affichage ; cela ne
prouve pas une suppression des saccades en jeu. Journaux et `summary.json` :
`/tmp/bdex-accel-timing-cadence-deadband/`, script
`/tmp/bdex_accel_timing_deadband_bench.py`. Première série conservée dans
`/tmp/bdex-accel-timing-cadence-bench/`. Même signal connu au démontage de
gamescope, après fermeture de l'enfant ; mesures sur la partie active.

### Défaut restant à ce stade

L'accélération extrapolée conservait une erreur au bord entrant du
panoramique : 34,86 dB sans accélération contre 34,58 avec, alors que la
zone avec historique valide était identique. La fiabilité du flux précédent
était suspectée. La correction suivante identifie une autre cause : le
traitement des zones révélées était sauté dans la branche accélérée.

Vérifications : 92 tests unitaires, options/panneau, shader d'état et carte
d'accélération sur GPU, qualité hors ligne, corpus, formats, déterminisme,
petites limites et matcher. Intégration, capture Linux, réglages à chaud et
rejeu réussis avec `FRAME_TIMING=1`. Contrôle MinGW x64/i686 réussi,
Windows non exécuté sur matériel. Installation Release 64/32 bits le 17/10,
bibliothèques et capture identiques aux builds ; configuration préservée
(SHA256). La nouvelle couche sera chargée au prochain lancement du jeu.

## Correction 7 — zones révélées avec accélération (17/10)

La recherche de source à vitesse constante appliquait `reveal_fill` en
cas d'oscillation et d'incohérence. La recherche avec `extrapolate_accel`
ne le faisait jamais, même pour une accélération localement nulle. Les
deux branches appliquent maintenant le même choix du fond plus lent.
Aucune nouvelle passe, image ou option. Le test de vitesse constante
couvre l'image entière, y compris le bord entrant, avec `reveal_fill=1`
explicite pour exercer aussi ce traitement sur un GPU logiciel.

Panoramique entier, balanced ×4, accélération 1, horloge active :

| Contenu / appels | Avant | Après | Scintillement, différence |
| --- | ---: | ---: | ---: |
| Durées variables | 34,575 dB | 34,866 dB | −0,142 |
| Contenu à pas fixe, appels irréguliers | 23,433 dB | 23,600 dB | −0,019 |

Les scènes 720p de rotation avec personnage/parallaxe/flou et coups de
souris restent proches : −0,015 à 0 dB global, arêtes jusqu'à −0,107 dB,
SSIM +0,0002 à +0,0008. L'interpolation est identique dans les cas testés.
Ces mesures qualifient des artefacts d'extrapolation, pas la cadence
d'affichage du jeu. Résultats `/tmp/bdex-reveal-accel-pan.log` et
`/tmp/bdex-reveal-accel-games.log`, JSON dans leurs dossiers associés.

Deux prototypes de validation du flux précédent (copie du flux avant puis
copie du coût de matching) n'ont presque rien changé au panoramique ; ils
ont été retirés intégralement. Le défaut ne justifiait pas leur mémoire et
leurs copies supplémentaires. Le test `offline` complet et le test GPU
`frame_state` réussissent avec la correction conservée.

Coût GPU hors ligne sur la scène 720p, quality ×4, trois A/B alternés :
médiane des trois synthèses par entrée **2,20→2,21 ms** ; analyse 0,09 ms
et flux 1,42 ms inchangés. Avec `reveal_fill=0`, 33 sorties identiques
octet par octet. Les quatre cas du test sur image entière passent aussi
sur lavapipe dans Ubuntu 24.04. Intégration de la couche et capture Linux
(quatre modes) réussies, accélération/remplissage/horloge activés, sous
gamescope headless sans WSI. Son signal 139 connu survient au démontage,
après la réussite des tests et la fermeture de l'enfant. Journaux :
`/tmp/bdex-reveal-accel-perf/`, `/tmp/bdex-reveal-accel-software.log`,
`/tmp/bdex-reveal-accel-online.log`.

Correction installée localement le 17/10 en Release 64/32 bits ; couches
et capture identiques aux builds. Réglages préservés (SHA256 inchangé).
Chargement au prochain lancement du jeu, sans redémarrage forcé.

## Correction 8 — rattrapage plus rapide après un créneau perdu (17/10)

Le rattrapage progressif attendait huit jobs tardifs, même lorsqu'un
nouveau décalage du plan était déjà visible. `pacing::PlanSlip` signale
une hausse du nombre de périodes entre le plan du premier intermédiaire
et son créneau atteignable à la présentation. Le worker demande alors au
thread du jeu de répartir la prochaine paire sur une sortie de moins.
Le même décalage n'est signalé qu'une fois. Démarrage, pause, nouvelle
fréquence et déplacement d'une demi-phase réinitialisent l'observation.

La récupération précoce conserve les gardes de `catchup_smooth` :
interpolation pure avec plan, horloge adaptative, `phase_smooth`, historique
valide, au moins trois sorties, GPU et worker sans pression. Inactive en
`low_latency`, hybride, extrapolation et `dedup`. Aucune nouvelle passe,
allocation ou attente GPU. Les autres retards durables gardent le
détecteur à huit jobs.

**Écarté** : retirer dans le worker un intermédiaire déjà calculé. Modèle
à 100 Hz : premier pas de contenu 20 ms contre 13,33 ms avec une paire
recalculée sur trois sorties ; cela amplifie précisément le saut de
mouvement recherché. Ce prototype est entièrement retiré.

Simulation du chemin retenu, 36 i/s / 144 Hz, un retard ponctuel : le
rafraîchissement de retard persiste **13→1 images du jeu**, plus grand
pas pendant la récupération **11,111→11,111 ms**, même nombre de sorties
retirées. Le test conserve une borne sur le délai de l'ancien détecteur,
qui varie avec la phase. Tests unitaires du détecteur et du scénario :
**94 réussis**.

### GPU : récupération d'un retard contrôlé

Trois A/B alternés, 2560×1440, source répétée à 30 i/s, quality ×4,
gamescope headless 144 Hz sans WSI. Deux bibliothèques **de test** reçoivent
les mêmes réveils retardés de 9 ms aux jobs 90/180/270 ; la référence
désactive seulement la demande précoce. La source de production ne contient
aucun de ces délais ni ce commutateur de test.

| Mesure | Avant | Après |
| --- | ---: | ---: |
| Latence moyenne des 15 premières réelles après perturbation, médiane des essais | 43,04 ms | 38,65 ms |
| Excédent positif sur la médiane locale avant perturbation, même agrégation | 4,49 ms | 1,16 ms |
| Affichées/s, médiane | 143,8 | 143,7 |
| Tenues/s, médiane | 0,8 | 2,0 |
| Paires entièrement sans génération | 0/0/0 | 0/0/0 |

Chaque essai moyenne ses trois perturbations ; les 18 perturbations ont
bien été enregistrées. Le gain local de latence se répète dans les trois
paires. **Aucun gain global de cadence ou de ressenti en jeu n'est établi** ;
le nombre d'images tenues n'est pas amélioré dans cette série. Le banc
gamescope reste variable. Le signal 11 connu survient après fermeture de
l'enfant dans les six cas, sur la phase de démontage.

Un essai distinct alternant des sources figées/en mouvement a déclenché la
voie précoce dans une paire seulement ; insuffisant pour conclure sur la
cadence globale. Le premier stimulus par changement de config est écarté
(heartbeat fautif et reconstruction de l'étage de sortie).

Journaux, méthodes et résumé : `/tmp/bdex-plan-slip-delays/`, scripts
`/tmp/bdex_plan_slip_controlled_build.py` et
`/tmp/bdex_plan_slip_delay_bench.py`. Bibliothèques de test dans
`/tmp/bdex-plan-slip-controlled/` ; elles ne sont pas installées.

Vérifications finales : 94 tests unitaires, clés des options et logique du
panneau réussis ; intégration et réglages à chaud réussis sous gamescope
headless sans WSI avec `FRAME_TIMING=1`. Contrôle MinGW x64/i686, ordre
des en-têtes et liaison des appels de la capture réussis. Windows non
exécuté sur matériel. Installation locale Release 64/32 bits vérifiée par
comparaison avec les builds de production ; configuration préservée.
La couche sera chargée au prochain lancement du jeu. Aucun commit ou push.

## Pistes conservées après l'arrêt demandé

L'utilisateur a demandé de terminer la correction en cours puis d'arrêter.
Les pistes ci-dessous ne sont pas lancées.

1. Horodatages implémentés dans les enregistrements du panneau :
   `FrameRecorder::queued` reçoit le `t0` monotone de la présentation du
   jeu, le writer produit `timestamps.csv` avant `DONE`. Le pack/fetch
   préserve les temps sur une sous-séquence et refuse d'en perdre
   silencieusement. `recording_report.py` donne les percentiles et pics.
   Tests `record_live` (40 images sur GPU, horloge cohérente, génération
   maintenue) et `recording_times` (sélection, archive, restauration,
   métadonnées invalides et ancien format) réussis. Les anciens fichiers
   sans horloge ne permettent pas de reconstituer leurs temps exacts.
   Le rejeu de cette horloge est maintenant implémenté :
   `bdex_demo --replay SEQ --replay-timing --replay-log FILE`. Il préserve
   les écarts d'une sélection, termine à la dernière source, refuse une
   horloge manquante/incohérente et journalise les temps visés/réels.
   Les retards >0,5 ms décalent la suite sans rafale de rattrapage, et sont
   visibles dans le CSV et le résumé du terminal. Tests CPU et démo Vulkan
   réussis (cadence irrégulière, sélection, fin et préservation des sources).
   Cela donne un banc de diagnostic ; aucun gain de fluidité n'est attribué
   à cet outil seul. Le banc PSNR garde son mode à cadence constante.
2. Examiner les plans ratés sous charge GPU et les variations de temps d'image
   qui restent après la correction des transitions. Mesurer les intervalles affichés et le pas de
   mouvement séparément : un nombre élevé d'i/s ne prouve pas leur régularité.
3. Sur les rotations très floues de Cyberpunk, mesurer la trajectoire des
   sorties entre deux sources consécutives. Une évaluation qui saute une
   source de ces captures à 30 i/s double artificiellement le déplacement
   d'une vraie paire à 30 i/s ; elle ne reproduit pas les essais du joueur.

Les enregistrements originaux sont conservés. Copie temporaire de la couche
64 bits avant correction : `/tmp/bdex-before-phase/` ; montage d'observation :
`/tmp/bdex-recordings-20261016.png`.
