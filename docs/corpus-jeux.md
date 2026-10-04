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

- **Panneau** : choisir le jeu détecté, bouton « Enregistrer ». Il garde les
  150 images réelles suivantes dans `~/.cache/bdex-framegen/recordings/`.
- **Ligne de commande** : `bdex-framegen --record ~/rec/<jeu>-<scène> 150 --
  %command%` (options de lancement Steam). L'enregistrement se fait sans
  génération : ×1, images réelles seulement.

Les fichiers s'appellent `sc0_<taille>_<n>_real_….ppm`. Si un dossier
contient aussi des `sc1_…` (la swapchain a été recréée, par exemple après un
changement de résolution), ne garder qu'une des deux séries.

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
