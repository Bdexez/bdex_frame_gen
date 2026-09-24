# TODO — axes d'amélioration

État au 2026-09-24. Mesures de référence : `ROADMAP.md` et `CHANGELOG.md`.
Outils : `tools/run_eval.sh` (démo, en ligne) et `tools/eval_sequence.py`
(séquences enregistrées, hors ligne, A/B avec `--variant`).

## Décisions en attente

- [ ] **Défaut de `zero_bias`** : activer `zero_bias_contrast=0.1` par défaut
  (non négatif partout, cf. plus bas) plutôt que passer `zero_bias` à 0, au moins pour l'extrapolation et
  l'app de capture ? Mesuré : rapide +0,75 dB (arêtes en rotation +2,8),
  normal −0,10 ; en extrapolation, gain sur toutes les scènes testées.
  Attention : déplace la référence bit à bit de la CI, et le launcher Windows
  et la GUI GTK éditent ce réglage.
- [ ] **Enregistrer une séquence Kenshi** (caméra en mouvement, HUD visible)
  pour trancher `zero_bias`, `static_guard` et `temporal_prediction` sur du
  vrai contenu :
  `BDEX_FG=1 BDEX_FG_MULTIPLIER=1 BDEX_FG_DUMP=/tmp/kenshi BDEX_FG_DUMP_FRAMES=240 %command%`
  puis `tools/eval_sequence.py /tmp/kenshi --variant ZERO_BIAS=0 --variant STATIC_GUARD=1`
- [ ] Vérifier la CI Linux sur GitHub (dépôt privé, non visible sans compte).
- [ ] Supprimer la branche locale `backup/pre-rebase` si tout est bon.

## Qualité de l'image

- [x] **`zero_bias` proportionnel au contraste** — fait : `zero_bias_contrast`
  (opt-in). À 0,1 : normal +0,02 dB, rapide +0,48 (rotation +2,7), panoramique
  doux +17 dB. Meilleur candidat pour le défaut (remplace la question
  `zero_bias=0`). du bloc (gradient moyen déjà
  en mémoire partagée), plutôt qu'une pénalité fixe : garder la stabilité des
  HUD texturés sans fondu fantôme sur les contenus doux.
- [x] **Flow sous-pixel** — fait : `subpixel` (opt-in), ajustement parabolique
  sur fenêtre 8×8 au niveau fin. Normal +0,08, rapide +0,06 ; avec
  `zero_bias_contrast=0.1` : +0,10 / +0,54 dB. Coût GPU non mesurable sur
  Vega 8 (bruit).
- [ ] **Fondus et changements d'éclairage** : SAD centré (moyenne du bloc
  retirée) pour ne plus casser l'appariement sur un flash ou un fondu.
- [ ] **Détection du HUD sur plusieurs frames** : masque d'immobilité accumulé
  sur ~10 frames, pour distinguer un glyphe fixe d'un petit objet rapide
  (indiscernables avec deux frames). Version aboutie de `static_guard`.
- [ ] **Warp « en avant » pour l'extrapolation** : projeter chaque pixel vers
  sa position future, puis remplir les trous avec le fond, au lieu d'étirer
  les zones découvertes.
- [ ] **Deux hypothèses de mouvement par bloc**, choix pixel par pixel dans la
  synthèse, pour les frontières d'objets.

## Évaluation

- [ ] **Corpus de séquences de vrais jeux dans la CI** (Kenshi, un FPS, un jeu
  chargé en HUD), en assets de release, rejoué par `bdex_offline` sur
  lavapipe à chaque push.
- [ ] **Métriques perceptives** : SSIM et mesure de scintillement entre frames
  générées successives, en plus du PSNR.
- [ ] **Réglage automatique** : script de recherche des meilleurs
  `zero_bias` / `smoothness` / seuils sur le corpus.

## Performance et latence

- [ ] **Limiteur de fps intégré**, calé sur un sous-multiple du
  rafraîchissement (60 → 120, 72 → 144).
- [ ] **Multiplicateur dynamique** selon les fps réels : couper quand le jeu
  tient déjà le rafraîchissement, passer de x2 à x3 quand il chute.
- [ ] **Pacing précis** via `VK_EXT_present_timing` ; prise en charge du VRR.
- [ ] **Profilage par étape sur la RTX 3060 Ti** avant toute optimisation
  (barrières plus fines, recalcul du coût en série dans `block_match`).

## Fiabilité

- [ ] **Couches de validation Khronos dans la CI** (validation de
  synchronisation comprise), sur lavapipe.
- [ ] **ASan et TSan** sur les tests d'intégration (`swapchain.cpp` : mutex,
  thread de travail, variables de condition).
- [ ] **Test du block matcher sur CPU** (coûts et départage des égalités),
  qui aurait attrapé la régression `0549e44` sans GPU.
- [ ] **Repli en pass-through sur exception** dans `FrameGen` : une erreur du
  layer ne doit jamais faire planter le jeu (cf. le segfault causé par le
  pool de descripteurs pendant le développement de `temporal_prediction`).

## Produit

- [ ] **Rechargement de la config à chaud** (surveillance du fichier) :
  changements de la GUI GTK appliqués en direct, raccourci on/off sous Linux.
- [ ] **Rapport de diagnostic en une commande** (`bdex-framegen --report`) :
  logs, config, GPU, pilote, jeu.
- [ ] **Profils par jeu fournis** : réglages éprouvés et liste anti-cheat
  maintenue.
- [ ] **Packaging et versions** : AUR ou Flatpak pour la GUI, releases
  versionnées (toujours 0.3.0), section Windows dans le README.
- [ ] **Valider upscaling, HDR et 10 bits** sur de vrais écrans.
