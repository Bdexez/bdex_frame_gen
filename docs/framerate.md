# Cadences quelconques — analyse et validation

Travail du 23 octobre 2026, à partir de `889ce3c` et de la mémoire de Claude
(`~/.claude/projects/-home-bdex-delivery-bdex-frame-gen/memory/`,
`AGENTS.md`, `docs/MEMOIRE.md`).

## Comportement demandé

- Accepter une source stable, fluctuante ou sans limite d'images/s.
- Conserver le multiplicateur fixe choisi : ×4 à 30 = 120 sorties/s ; à
  60 = 240 ; à 200 = 800, dans la capacité du matériel.
- Accepter des sorties au-delà des Hz. Un écran à 144 Hz ne peut afficher
  800 images entières distinctes/s : les présentations excédentaires sont
  remplacées. Cela ne justifie pas de plafonner la production à 144.
- Séparer les temps du contenu, l'estimation de cadence et les heures de
  présentation. Une moyenne des intervalles sert à prévoir ; elle ne
  remplace pas le temps des deux images utilisées pour le mouvement.
- Préserver l'adaptatif et les options explicites de l'utilisateur. Aucun
  besoin d'activer `refresh_fill`, de plafonner le jeu ou de changer les Hz.
- Tester exclusivement dans un affichage isolé, sans modifier la session
  Hyprland de l'utilisateur.

## Diagnostic du code

Le moteur n'a pas de constante imposant 30 ou 60 i/s. `FrameOrder` gère
×2..×8 ; les intervalles mesurés, `IntervalFilter`, le tampon GPU
`frame_state`, et la normalisation de l'accélération existent déjà. Les
derniers réglages se concentraient toutefois sur 30 i/s ×4 et 60 i/s ×2.
Les défauts restants se trouvent surtout dans leur cadencement :

1. **Plan désactivé au-dessus des Hz de sortie.** `swapchain.cpp` ne
   planifiait que si `m * période <= 1,02 * intervalle`. Le worker utilisait
   alors l'heure où il observait la fence, qui peut être postérieure à sa
   complétion lorsqu'il présente encore le job précédent. Réutiliser cette
   heure pour prévoir le suivant peut entretenir le retard.
2. **Réservation de rafraîchissements sans capacité correspondante.** La
   correction du 21/10 autorisait des images à partager un rafraîchissement,
   mais réservait toujours le premier à chaque job et un autre après sa
   réelle. Cette réservation est impossible quand la source elle-même
   dépasse les Hz, et trop coûteuse pour certaines cadences hybrides.
3. **Première copie et synchronisation explicite.** La première présentation
   doit précéder la soumission des copies qui attendent le reste de la
   synthèse (`deferRest`). Attendre un tic d'écran avant cette présentation
   peut aussi retenir la soumission du job suivant. Ne pas contourner
   `deferRest` : cela réintroduirait le bug corrigé le 20/10.
4. **Attente de soumission confondue avec retard d'affichage.** À cadence
   libre, le producteur atteint forcément le débit du pipeline. Passer à
   une réelle seule dès que cette attente dépasse un quart de l'intervalle
   faisait osciller ×4/×1, même sans file d'affichage croissante. Le banc
   sans limite donnait environ ×2 au lieu de ×4.
5. **`skip_fast=1` par défaut.** Cette option suspendait la génération dès
   85 % des Hz, indépendamment du multiplicateur demandé. Elle contredisait
   donc le choix de conserver ×N à toute cadence.
6. **Source sans limite mal estimée.** Une image très courte (< 0,5 ms)
   était ignorée. Lorsqu’elle alterne avec une attente de soumission longue,
   seule la moitié longue comptait dans l’estimation : la durée estimée
   augmentait et le producteur ralentissait encore. Tout intervalle positif
   inférieur à 500 ms compte désormais pour le cadencement.
7. **Limiteur à plus de 250 i/s.** Les bornes de son `std::clamp` devenaient
   inversées pour un intervalle inférieur à 4 ms. Les bornes sont désormais
   ordonnées, y compris à 360 et 1 000 i/s.
8. **Une image de transit manquante en ×8.** `presentCeiling` plafonnait
   toutes les présentations à sept, limite nécessaire seulement quand
   `even_motion` présente exclusivement des générées. Avec six images
   acquises à la fois, un transit au lieu de deux était alloué pour un job
   de huit ; le jeu attendait alors le cadencement de sa dernière image
   avant de soumettre la synthèse suivante. Les modes normaux gardent
   désormais les huit positions (sept générées + réelle).

## Corrections

Les décisions restent des fonctions pures dans `pacing.h` :

- `sharesRefresh` réserve des rafraîchissements uniquement si la source
  laisse assez de place ; les autres sorties les partagent.
- `planLands` planifie aussi les multiplicateurs fixes au-delà des Hz de
  sortie, sans tolérance de 2 % qui sur-réservait autour d'un rapport entier.
- Si la **source** dépasse les Hz, le worker utilise un espacement inférieur
  à la période de l'écran, depuis la disponibilité GPU effective, sans
  réserver un rafraîchissement à chaque job (`Job::gridPacing`).
- `GridPacer` prend aussi ce chemin quand les sorties dépassent les Hz et
  que l'attente de soumission dépasse un quart de l'intervalle. Cette attente
  est comprise dans les FPS mesurés : ceux-ci ne prouvent pas que le
  producteur est lent par lui-même. Huit jobs sans pression permettent de
  rétablir la grille ; si chaque sortie tient à nouveau dans un
  rafraîchissement, le retour est immédiat. Cela évite les allers-retours
  sous appels rapides/attentes alternés.
- Dans ce chemin, aucun délai GPU prédit ne fixe les présentations : il
  pourrait apprendre une attente due à la présentation précédente et
  l'amplifier. Le plan et `MotionClock` sont réinitialisés.
- La marge d'une demi-sortie après la présentation précédente est aussi
  retirée dans ce chemin : l'origine observée par un worker déjà occupé
  est suffisamment tardive. La rajouter entretenait encore un retard sous
  surcharge CPU, surtout en ×8 (`presentTargetMs`, `continuous`).
- Pour les plans de grille, le délai GPU estimé part de `Job::preparedAt`,
  après les attentes CPU. Le temps CPU déjà écoulé est ajouté au plan
  courant une seule fois ; les temps du contenu partent toujours de `t0`.
- `needsCatchUp` conserve toutes les sorties du multiplicateur fixe : le
  débit effectif de la source suit les attentes de soumission et les fences
  déjà présentes. Les modes automatiques/adaptatifs gardent leur rattrapage
  sur pression du worker ou retard d'affichage persistant. Un mode fixe ne
  bascule plus silencieusement sur une réelle seule sous charge.
- `skip_fast` devient opt-in. L'option et son hystérésis restent disponibles.

La grille, le cadencement adaptatif, les temps de contenu et les shaders du
matcher restent distincts. Aucun changement du matcher ni de ses poids.
La capture Linux/Windows partage `Config`, `pacing.h` et le moteur ; ses
boucles de présentation n'utilisent pas la planification de la couche.

## Vérifications reproductibles

`tests/test_pacing.cpp` couvre désormais 462 combinaisons : ×2..×8,
23,976..300 i/s, écrans à 60/90/120/143,912/165/240 Hz, avec l'heure de
complétion réellement observable par un worker occupé. S'y ajoutent rampes,
alternance des intervalles, changement durable de cadence, ralentissement
isolé et traversées des Hz. Vérifications : nombre de sorties conservé,
retard borné, retour à faible latence après accélération. Les anciens
scénarios adaptatifs, VRR, `tight_hold`, changement de phase et rattrapage
restent exécutés. Un scénario producteur + worker, sans limite de FPS,
vérifie aussi que le débit ne se bloque pas à 72/96/144 i/s à cause de
son propre plan. Les modes hybride et extrapolation sont couverts avec
l’annulation des prédites quand la source suivante est prête.

`tests/test_frame_state.cpp` envoie au vrai shader GPU des plans à
23,976/35,9/36,2/59,94/90/200 i/s, ×2..×8 : chaque temps `t` est comparé à
sa valeur analytique, y compris les sorties partageant un rafraîchissement.

Le banc en ligne conserve les logs et vérifie la fin du **processus démo**,
indépendamment d'un éventuel crash de gamescope à sa fermeture :

```bash
python3 tools/framerate_bench.py --output /tmp/fg-rates \
  --fps 23.976 47 59.94 90 200 variable uncapped --multipliers 2 4 8 --check
python3 tools/framerate_bench.py --output /tmp/fg-heavy \
  --fps 30 60 90 variable uncapped --preset quality --size 2560x1440 --check
python3 tools/framerate_bench.py --output /tmp/fg-60x4 \
  --fps 60 --preset quality --size 2560x1440 --seconds 8 --check --max-motion-jumps 2
```

`variable` est un rejeu d'images à horloge enregistrée, avec changements de
cadence, rampe jusqu'à 220 i/s et alternance 45/200. `uncapped` utilise la
nouvelle option `bdex_demo --uncapped`, sans sommeil limitant le jeu.
`summary.json` rapporte sorties, réelles seules, délai de la réelle et
présence de la grille X. `--check` échoue si le multiplicateur mesuré
en interpolation perd plus de 5 % de sorties ou si des rattrapages
apparaissent. Le banc nettoie le groupe de processus isolé si gamescope
reste bloqué après la fin de la démo. Les fichiers `source.csv` exposent toute surcharge
du rejeu. Les mesures d'affichage de gamescope sous charge GPU ne prouvent
pas la fluidité sur l'écran physique.
La section `cadence` ajoute le rapport de `cadence_report.py` : les sauts de
temps du contenu, les tenues et les présentations dans le même
rafraîchissement. `--max-motion-jumps N` échoue au-delà de N sauts/s ; si les
temps de contenu X ne sont pas disponibles, le contrôle échoue également.
Choisir le seuil pour un scénario contrôlé, en tenant compte de la charge
concurrente du GPU et du compositeur.
Quand `nvidia-smi` est disponible, `gpu_before` et `gpu_after` conservent la
charge, la mémoire et la fréquence GPU autour de chaque cas : vérifier la
charge concurrente avant de comparer deux débits.

Mesures et limites de validation : voir la session du 23/10 dans
[`MEMOIRE.md`](MEMOIRE.md).

## Éviter le calcul répété d'une même sortie

Sur une grille connue, plusieurs sorties d'un même job peuvent avoir
exactement le même instant GPU. Avec les mêmes sources et le même mode,
elles ont les mêmes pixels. Le moteur copie alors l'image finie vers
l'autre sortie au lieu de refaire sa synthèse et ses filtres. Toutes les
images et toutes les présentations du multiplicateur restent présentes.
Les phases distinctes, les modes différents et `learned_correction` actif
conservent leur calcul complet ; sans grille GPU, aucune réutilisation.

`ctest --test-dir build -R output_reuse --output-on-failure` compare les
octets GPU au calcul complet, avec formats SDR/HDR, upscale/CAS,
interpolation, hybride, prédiction, répétitions et filtres optionnels.
Pour une comparaison manuelle, `bdex_offline --raw-dumps` exporte aussi
les pixels empaquetés ; `--no-output-reuse` force la référence complète.
Ces options de vérification ne modifient pas les réglages du jeu.

Sur le banc 1440p quality/144 Hz à 60 i/s, avec douze dessins de la scène
par image réelle (`--gpu-load 12`), le coût de synthèse médian baisse de
3,97 à 2,97 ms en ×4 et de 7,10 à 2,99 ms en ×8. Débits conservés :
240 et environ 480 présentations/s, aucun rattrapage. Les fréquences GPU
varient automatiquement : ce sont des mesures de coût sur ce banc,
pas une promesse de gain de FPS ou de latence dans tous les jeux.

## Paires d'un et de deux rafraîchissements : `even_fast` (29/10)

Travail du 29 octobre 2026. Objectif : que la génération reste fluide quand
le jeu n'est ni à 30 ni à 60 i/s, sans ajouter de latence.

### Ce que le banc à source régulière ne voyait pas

Le cadencement du 23/10 conservait ×N et la cadence des présentations à
toute cadence, mais il cale chaque image réelle sur un rafraîchissement et
interpole les générées entre deux réelles ainsi calées. Quand une image du
jeu dure entre un et deux rafraîchissements (72 à 137 i/s sur 144 Hz,
30 à 57 sur 60 Hz), les paires alternent un et deux rafraîchissements : à
100 i/s sur 144 Hz, le contenu avance de 10 ms en un rafraîchissement, puis
de 5 et 5 ms. Simulateur, source parfaitement régulière : 15 (80 i/s) à
54 (100 i/s) sauts de mouvement par seconde, 1,8 à 2,4 ms d'écart rms par
rafraîchissement, quel que soit le multiplicateur. Le nombre d'images, le
compteur de rattrapages et la grille étaient pourtant parfaits : c'est le
mouvement qui saccade.

La gigue des vrais jeux aggrave aussi les cadences intermédiaires. Les
enregistrements de l'utilisateur donnent 1,2 à 3,4 ms d'écart-type à 30 et
60 i/s, et jusqu'à 19 ms pour Cyberpunk sans limite à 23 i/s. Le banc en ligne
avec la trace Cyberpunk à 60 i/s (σ ≈ 3 ms) montrait 7,1 sauts/s contre 0 sur
la source régulière.

### Outils

- `tools/cadence_report.py` rapporte maintenant l'écart rms du pas de contenu
  par rafraîchissement (un rafraîchissement tenu compte un pas de 0), l'écart-
  type du délai contenu→écran, et le **délai moyen du contenu** (rafraîchissement
  remis moins l'heure de présentation du contenu montré) : la latence du
  mouvement, que l'image réelle seule ne mesure plus quand les générées le
  portent.
- `tools/framerate_bench.py --trace TIMESTAMPS_CSV` rejoue les temps d'images
  d'un vrai jeu (le `timestamps.csv` d'un enregistrement du panneau), en
  boucle ; `--gpu-load N` passe à la démo un jeu limité par son propre rendu.

### Pistes écartées (simulateur, boucle fermée jeu + GPU + worker + Xwayland)

- Choisir le rafraîchissement de la réelle par diffusion d'erreur (au plus
  près de « réelle précédente + dt ») : moins de sauts à 72–87 i/s, mais plus
  de tenues à 45 i/s et sur les traces réelles.
- Planifier sur une horloge lissée (boucle à verrouillage de phase sur les
  `present()`) : pire. Suivre l'heure brute de chaque `present()` compense la
  gigue du contenu, dont l'heure de présentation fait partie.
- `EvenClock` tel quel (marque haute du temps d'image qui décroît en 30 s,
  délai relevé à chaque raté) : régulier, mais +10 ms de délai.
- La réelle en secours présentée à l'avance du rafraîchissement suivant :
  elle retarde le worker et s'affiche trop souvent.

### Le mode retenu

`even_fast` (défaut 1) : multiplicateur fixe ×3 et plus, interpolation pure,
grille d'affichage connue (`x11_clock` ou retour d'affichage), pas en FIFO,
une image du jeu entre 1,08 et 1,95 rafraîchissement (quittée hors de
1,03..2,0). Chaque rafraîchissement couvert par la paire reçoit une générée
qui montre le contenu de son propre instant (horloge `EvenClock` en mode
`fallback`). Les ×N−1 générées sont réparties sur ces rafraîchissements, et
plusieurs sur un même rafraîchissement sont la même image, que `FrameGen`
copie. L'image réelle part juste après le dernier, à un quart de période
(`pacing::kFallbackAfter`), sans réserver de rafraîchissement : la première
image du job suivant la remplace, sauf si elle est en retard. Dans ce cas,
la réelle s'affiche à la place d'une tenue, et le délai n'est pas relevé pour
autant. Le délai visé est la marque haute du temps d'image (qui redescend de
0,2 ms par image), plus le délai GPU, plus la marge de prêt X11 (1,5 ms),
moins 0,5 ms. Une paire sans aucun rafraîchissement (image plus courte qu'un
rafraîchissement) présente toutes ses images en secours, avec les générées à
t = 1 : toujours ×N présentations par image du jeu. `even_fast = 2` ajoute,
à ×4 et plus, la plage 2–3 rafraîchissements (48 à 72 i/s sur 144 Hz) :
encore plus régulier, mais 2 à 5 ms de délai en plus, donc pas par défaut.

Simulateur, 143,912 Hz, ×4, gigue de 1 ms (`pacing_even_fast_*`) : écart
rms 1,8–2,3 → 0,22–0,29 ms, sauts 15–38 → 0/s, délai moyen du contenu
+0,6 à +1,5 ms (×3 à 80 i/s : +2,5 ; ×8 : −0,1 à −5,8, la réelle calée y
attendait 7/8 d'intervalle). Robuste aux pics isolés (95 i/s avec une image
de 30 ms par seconde : 36 → 0 sauts/s) et aux balayages de cadence. Au-delà
de 3 ms de gigue, le gain diminue ; à 4 ms, le plan calé reste comparable.
Hors de sa plage (30, 47, 60, 71,5, 200 i/s) et en ×2, rien ne change :
résultats identiques au bit près dans le simulateur.

### Mesures en ligne (banc isolé, gamescope sans tête, Xwayland 144 Hz)

A/B avec `a80b38e`, cas alternés référence/nouveau, 8 s chacun, démo 640×360
`performance` sauf mention. **Le jeu de l'utilisateur (State of Decay 2)
tournait sur le même GPU** (77 à 100 % d'occupation, notée dans chaque
`summary.json`) : les écarts de délai de l'ordre de 1 ms sont dans le bruit
(60 i/s, mode inactif : 18,0 / 19,0 ms). Délai = délai moyen du contenu
(`cadence_report.py`).

| ×4 | écart rms (ms) | sauts/s | délai (ms) |
|---|---|---|---|
| 80 i/s stable | 1,98 → 0,42 | 16,1 → 0,17 | 16,0 → 16,4 |
| 95 i/s stable | 2,46 → 0,41 | 46,0 → 0,17 | 14,2 → 14,4 |
| 105 i/s stable | 2,45 → 0,48 | 62,7 → 0,33 | 14,1 → 13,5 |
| 120 i/s stable | 2,03 → 0,27 | 0 → 0 | 12,6 → 12,9 |
| 80 i/s, gigue 1 ms | 1,84 → 0,26 | 17,0 → 0 | 14,4 → 15,7 |
| 95 i/s, gigue 1 ms | 2,33 → 0,25 | 37,8 → 0 | 13,2 → 13,8 |
| 105 i/s, gigue 1 ms | 2,26 → 0,30 | 37,0 → 0 | 12,3 → 12,7 |
| 120 i/s, gigue 1 ms | 2,88 → 0,57 | 4,8 → 0,33 | 12,6 → 13,4 |
| 95 i/s, pic de 30 ms par seconde | 2,79 → 1,78 | 38,9 → 2,8 | 12,4 → 13,7 |
| balayage 60 → 140 → 60 i/s | 2,94 → 2,14 | 7,5 → 4,7 | 14,2 → 16,2 |
| 30 / 60 / 200 i/s (hors plage) | inchangés | inchangés | inchangés |

×3 à 95 i/s : 2,46 → 0,10 ms, 46 → 0 sauts/s, délai +1,0 ms ; ×8 à 95 i/s :
2,45 → 0,36 ms, 46 → 0, délai −1,4 ms. Toujours ×N sorties par image.

1440p `quality`, ×4, GPU saturé par le jeu de l'utilisateur : à 95 i/s
visés, la référence ne tenait que 80,7 i/s de source (322 sorties/s), la
nouvelle version 95 (380) ; écart 1,77 → 0,36 ms, sauts 6,7 → 0,17/s, délai
24,3 → 20,1 ms. Le mode synthétise moins d'images distinctes (une par
rafraîchissement couvert, ~1,4 par image du jeu à 95 i/s ; les doublons sont
copiés), ce qui laisse du GPU au jeu. À 80 i/s : 1,98 → 0,41 ms, délai +1,2.
Mesures de banc, pas une validation ressentie sur l'écran de l'utilisateur.

`even_fast = 2`, ×4, même banc et même charge du jeu : 60 i/s stable,
écart 1,48 → 0,24 ms, délai 18,8 → 20,7 ms ; 60 i/s avec 1 ms de gigue,
1,56 → 0,69 ms, 17,5 → 20,2 ms ; 55 i/s avec gigue, 1,43 → 0,58 ms, +2,7 ms
(mesure prise avant la borne ci-dessous). Une première version laissait le
délai remonter au-delà des sorties disponibles : à 2,4 rafraîchissements par
image pour 3 générées, la paire débordait, le délai était recoupé et le
mouvement sautait de deux périodes (1,0 saut/s, écart 1,39 ms). La remontée
est maintenant bornée par les sorties (`first + maxFrames·P`).
