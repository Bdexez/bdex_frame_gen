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

