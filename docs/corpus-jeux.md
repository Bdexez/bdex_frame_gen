# Enregistrer des jeux pour trancher les options en attente

Une dizaine d'options sont mesurées sur le corpus synthétique et la démo, mais
restent désactivées faute d'essai sur de vrais jeux (`docs/MEMOIRE.md` §9).
Les deux réseaux appris (`learned_flow`, `learned_correction`) sont aussi
bloqués : ils ne généralisent pas au-delà de leurs données d'entraînement.
Des enregistrements de jeux débloquent tout ça. Ce guide explique quoi
enregistrer, comment, puis comment obtenir un verdict en une commande.

## 1. Quoi enregistrer

Il faut environ 150 images réelles par scène, et une scène par cas. Chaque
ligne correspond à une décision en attente.

| Scène | Exemples | Options jugées |
|---|---|---|
| Caméra qui tourne vite, décor détaillé | jeu 3D, souris rapide | `learned_flow`, `flow_temporal`, `synth_bicubic` |
| Personnages ou véhicules qui passent devant le décor | foule, course, combat | `occlusion_strength` (le point faible mesuré, cf. §3) |
| HUD visible pendant un mouvement | jeu de tir, MMO, minicarte | `static_guard`, `hud_mask` |
| Cinématique à 30 i/s, ou émulateur | cinématique Proton, RetroArch | `dedup` |
| Décor coloré et peu contrasté | feuillage, eau, ciel | `chroma_weight` |
| Caméra qui accélère | véhicule qui démarre, saut | `extrapolate_accel` |
| Menu qui s'ouvre, texte qui défile | inventaire, dialogues | `learned_correction` |
| Lumière qui change | explosion, fondu, flash | (contrôle : `luma_match` est déjà actif) |

Varier les jeux compte plus que le nombre de scènes par jeu.

## 2. Comment enregistrer

- **Panneau** (à préférer) : une fois dans la scène, bouton « ● Enregistrer »
  sur la ligne du jeu (« Actualiser la liste » si le jeu a démarré après le
  panneau ; le bouton n'existe que pour un jeu armé). Il garde les 150 images
  réelles suivantes dans `~/.cache/bdex-framegen/recordings/`. Le jeu garde
  sa cadence : les images sont relues et écrites hors de son thread.
- **Ligne de commande** : `bdex-framegen --record ~/rec/<jeu>-<scène> 150 --
  %command%` (options de lancement Steam). Attention : il enregistre dès la
  première image, donc les logos et le menu ; seulement pour un programme
  qui démarre directement dans la scène. Sans génération : ×1, images
  réelles seulement.

Les enregistrements du panneau s'appellent `f0000.ppm`, `f0001.ppm`… puis
`DONE`. Depuis le 16/10, `timestamps.csv` conserve aussi l'heure monotone de
présentation de chaque source et son intervalle avec la précédente. Les
dates de fichier ne donnent pas cette information : les écritures sont
asynchrones. Pour mesurer les variations du temps d'image :

```bash
python3 tools/recording_report.py ~/.cache/bdex-framegen/recordings/<séquence>
```

Les archives créées par `corpus_pack.py` conservent ces temps, y compris avec
`--start` / `--count`, et `corpus_fetch.py` les restaure. Les captures plus
anciennes restent utilisables pour l'image, mais ne permettent pas de
retrouver le temps précis de chaque source. Cette mesure porte sur les
présentations du jeu ; pour les images affichées, utiliser `latency_log` et
`tools/cadence_report.py`.

**Profondeur du jeu** (11/11) : avec l'option `depth` activée (case
« Profondeur du jeu » ou `depth = 1`) et la génération lancée (« Démarrer »,
interpolation), l'enregistrement du panneau garde aussi la profondeur de
chaque image dans `depth/f0000.f32`… — la taille des images, un flottant par
pixel, Z inversé (plus grand = plus près), le texel du centre de chaque bloc
(pas de moyenne à travers un contour). `DONE` en donne le compte (`depth=150`)
et le statut du jeu dit si le tampon est trouvé (`depth=on`). C'est ce que
`bdex_offline --depth` lit : `pending_decisions.py` note alors `depth` sur
ces séquences, et `eval_sequence.py SEQ --variant DEPTH=1` suffit (le dossier
`depth/` est pris de lui-même). `corpus_pack.py` / `corpus_fetch.py` la
conservent. Sans génération, sans l'option ou en extrapolation : images
seules, comme avant.

Pour reproduire ces variations dans la démo à travers la couche :

```bash
BDEX_FG_CONFIG=/dev/null BDEX_FG=1 BDEX_FG_PRESET=quality BDEX_FG_MULTIPLIER=4 \
  build/demo/bdex_demo --replay <séquence> --replay-timing --replay-log /tmp/rejeu.csv
```

`--replay-timing` utilise `timestamps.csv` à la place de `--fps` et s'arrête
à la dernière source. `--replay-start` / `--replay-step` conservent les
écarts de temps des sources sélectionnées. L'option refuse une horloge
manquante ou incohérente. Le journal CSV donne le décalage source, l'heure
visée, l'heure d'appel de présentation et les retards qui décalent la suite
(`source_ms,target_ms,present_ms,shift_ms`, après le nom de l'image). Une
machine de rejeu trop lente décale les échéances suivantes, sans rafale de
rattrapage ; ces décalages sont aussi résumés dans le terminal. Le fichier
du journal doit être nouveau. Le nombre d'i/s et la fluidité d'un rejeu
fortement décalé ne représentent plus le rythme du jeu enregistré.

Ce mode mesure le comportement temporel. Le banc de qualité
`replay_bench.py` conserve sa cadence constante et ses sources espacées
pour disposer d'images de référence entre elles.

Le chemin dump de la ligne de commande produit
`sc0_<taille>_<n>_real_….ppm`, sans `timestamps.csv`. Si un dossier contient
aussi des `sc1_…` (swapchain recréée, par exemple après un changement de
résolution), ne garder qu'une des deux séries.

## 3. Obtenir les verdicts

```bash
tools/pending_decisions.py ~/.cache/bdex-framegen/recordings/* --build build
```

Le script note chaque option contre les réglages par défaut. Il le fait sur
chaque séquence, dans chaque mode où l'option agit, avec `bdex_offline`
(sans fenêtre). La référence est le preset automatique de ce GPU, c'est-à-dire
ce que reçoit un joueur. Verdicts possibles :

- `default?` : gain moyen ≥ +0,10 dB, aucune séquence sous −0,10 dB ;
- `no` : perte en moyenne ;
- `neutral` : rien ne change (à 0,02 dB près) ;
- `mixed` : gagne quelque part, perd ailleurs. Regarder la colonne « worst on ».

`--json out.json` garde le détail par séquence. Avant de changer un défaut :
la colonne de scintillement (plus bas = mieux), puis les invariants bit à bit
(`AGENTS.md` §6) et les références du test `corpus`.

Mesure de référence (04/10, RTX 3060 Ti) : sur des séquences synthétiques de
type jeu (monde texturé qui défile, objets indépendants, HUD), la caméra
seule est bien suivie jusqu'à 80 px par paire d'images (34 dB). **60 à 80 %
de l'erreur est sur le contour des objets qui bougent par eux-mêmes**, soit
2 à 4 % des pixels. Le sous-pixel, la synthèse bicubique, le flux appris, un
niveau de pyramide de plus et plus d'itérations n'y changent rien. Ce sont
donc les scènes « personnages devant le décor » qui disent le plus.

## 4. Les verser au corpus de la CI

```bash
tools/corpus_pack.py ~/rec/kenshi-ville kenshi-ville.tar.gz --name kenshi-ville --note "caméra + foule"
gh release create corpus --title "Corpus de jeux" --notes "Séquences enregistrées (tools/corpus_pack.py)"  # une fois
gh release upload corpus kenshi-ville.tar.gz
```

À chaque push, la CI Linux télécharge les archives de la release `corpus` et
les note comme le corpus synthétique (`tests/test_corpus.py`). Une séquence
sans référence est signalée, pas en échec. Les références se calculent sous
lavapipe, comme celles du corpus synthétique (un GPU dédié active d'autres
options en automatique) : `tools/corpus_fetch.py` puis `tests/test_corpus.py
build --update` dans le conteneur de `tools/ci/run.sh`.

Pour réentraîner les réseaux sur ces séquences :
`tools/train_correction.py --train SEQ… --test SEQ…`. Ne jamais tester sur
une séquence vue à l'entraînement.
