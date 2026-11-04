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
Quand `nvidia-smi` est disponible, `gpu_before` et `gpu_after` conservent la
charge, la mémoire et la fréquence GPU autour de chaque cas : vérifier la
charge concurrente avant de comparer deux débits.

Mesures et limites de validation : voir la session du 23/10 dans
[`MEMOIRE.md`](MEMOIRE.md).
