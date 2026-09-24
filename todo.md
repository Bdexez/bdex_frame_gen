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
- [ ] **Fondus et changements d'éclairage** — essayé, non retenu (2026-09-24) :
  SAD centré (moyenne retirée) dans le matcher. Seul : −1,7 dB démo, −9,7
  panoramique sous-pixel (la moyenne est discriminante, et les SAD plus petits
  laissent la pénalité absolue dominer). Avec `zero_bias_contrast=0.1` :
  fondu (−4 %/frame) **+9,1 dB**, mais démo −0,4, rapide −0,2, panoramique
  rapide −1,25, scintillement ±12 % −0,5. Reste à faire : ne l'activer que sur
  détection d'un changement global de luminosité (moyennes de luma des deux
  frames au niveau grossier), et compenser le gain côté synthèse (sur le
  scintillement, même le fondu simple bat la synthèse : 27,1 vs 26,2 dB).
- [~] **Détection du HUD** — amélioré (2026-09-24) : `static_guard` ne
  s'applique plus que si le flow *au pixel même* indique du mouvement (un
  glyphe fixe dans un contenu qui bouge), avec un seuil d'immobilité 8× plus
  strict. HUD +0,95 dB, démo +0,04, rapide −0,13 (contre −0,20), panoramique
  sous-pixel −2,6 (contre −7,7 avec l'ancien seuil). Reste : la perte sur les
  arêtes en mouvement rapide (−0,22). L'historique sur N frames ne
  discrimine pas : le fond sous un petit objet qui passe est lui aussi
  immobile depuis longtemps.
- [x] **Warp « en avant » pour l'extrapolation** — fait : `extrapolate_splat`
  (opt-in). En ligne : normal +1,08 dB, rapide +0,41, arêtes +1,56 ; x3 +1,05.
  Coût : +0,8 ms GPU (Vega 8). Candidat pour le défaut en extrapolation.
- [x] **Deux hypothèses de mouvement par bloc** — fait sous une forme moins
  coûteuse : `pixel_vectors` (opt-in) essaie par pixel les vecteurs non
  mélangés des 4 blocs voisins et garde la trajectoire la plus cohérente.
  En ligne : normal +0,39, rapide +0,57, arêtes en rotation jusqu'à +4,7 dB ;
  avec `subpixel` + `zero_bias_contrast=0.1` : rapide +1,07. Synthèse ×2
  (+1,4 ms sur Vega 8).

## Évaluation

- [~] **Corpus dans la CI** — infrastructure faite : `tools/make_corpus.py`
  (séquences synthétiques déterministes : HUD, panoramique doux, sous-pixel,
  fondu) et le test ctest `corpus` (sans écran, ~13 s), qui compare défaut et
  options candidates, en interpolation et extrapolation, à
  `tests/corpus_reference.json` (tolérance 1 dB / 0,01 SSIM ; références
  mesurées sur Vega 8, à confirmer sur lavapipe au premier run CI).
  Reste : **ajouter de vraies séquences de jeux** (Kenshi, un FPS, un jeu
  chargé en HUD) via `BDEX_CORPUS_DIR`, stockées en assets de release.
  Noté : sur `subpixel`, les options candidates perdent 1,2 dB (51,9 → 50,7,
  invisible) — à comprendre.
- [x] **Métriques perceptives** — fait : `eval_sequence.py` rapporte le SSIM
  (luma, fenêtres 8×8) et un scintillement (variation de l'erreur entre
  frames générées successives), plus `--json`. Ex. démo rapide :
  `pixel_vectors` SSIM 0,938 → 0,953, scintillement 4,37 → 3,34.
- [x] **Réglage automatique** — fait : `tools/tune.py SEQ...` (descente par
  coordonnées sur une grille de réglages, objectif = gain PSNR moyen par
  séquence, rejet des valeurs dont la pire séquence perd plus de 0,2 dB).
  ~1 min sur le mini-corpus. À faire tourner sur la démo + de vraies
  séquences de jeux avant de changer un défaut (le corpus synthétique seul
  surpondère le panoramique).

## Performance et latence

- [x] **Limiteur de fps intégré** — fait : `fps_limit` (GUI comprise), mesuré
  45,0 fps pile sur la démo. Reste possible : une valeur `auto` (taux de
  rafraîchissement / multiplicateur), qui demande de connaître le
  rafraîchissement (`VK_GOOGLE_display_timing` / present timing).
- [x] **Multiplicateur dynamique** — fait : `target_fps` (GUI comprise).
  Démo, cible 144, max ×4 : 30 fps → ×4, 50 → ×3, 72 → ×2, 150 → ×1.
  Même limite que `fps_limit` : la cible est saisie à la main faute de
  connaître le rafraîchissement.
- [ ] **Pacing précis** via `VK_EXT_present_timing` ; prise en charge du VRR —
  reporté (2026-09-24) : RADV local n'expose pas `present_timing`, seulement
  `present_wait`/`present_id`, que le layer masque au jeu (blocage
  VKD3D-Proton) et devrait activer pour son propre swapchain ; bénéfice non
  mesurable ici (ni instrument de timing d'affichage, ni écran VRR). Le mode
  mailbox + pacing actuel est déjà celui qui convient au VRR.
- [ ] **Profilage par étape sur la RTX 3060 Ti** (nécessite ta machine :
  `BDEX_FG_PROFILE=1`) avant toute optimisation
  (barrières plus fines, recalcul du coût en série dans `block_match`).

## Fiabilité

- [x] **Couches de validation Khronos dans la CI** — fait : test ctest
  `validation` (démo à travers le layer, validation + synchronisation, les
  deux modes, toutes les passes optionnelles). Vérifié que la couche est
  bien sous notre layer (erreur injectée → 42 VUID). Résultat actuel :
  0 message. `vulkan-validationlayers` ajouté à la CI Linux.
- [x] **ASan et TSan** — fait : job CI `sanitizers` (ASan+UBSan, TSan) sur
  les tests GPU. Localement (RADV) : unit, offline, intégration, acquire et
  la démo avec toutes les options, dumps, limiteur et multiplicateur
  dynamique — 0 rapport ASan/UBSan/TSan.
- [x] **Test du block matcher** — fait : ctest `matcher` exécute le vrai
  `block_match.comp` (lavapipe suffit) sur des images à égalités exactes et
  compare chaque bloc à un modèle C++ de la sélection historique. Vérifié :
  la sélection séquentielle de `0549e44` réintroduite → 32 écarts sur 128
  blocs ; shader actuel → 0.
- [x] **Repli en pass-through sur exception** — vérifié : un échec de
  `FrameGen` (pool de descripteurs sous-dimensionné, reproduit exprès) donne
  bien un swapchain en pass-through. Le segfault observé venait d'ailleurs,
  et c'était un vrai bug, corrigé : un jeu qui quitte sans détruire son
  device faisait détruire les swapchains virtuels par les destructeurs
  statiques, après la fermeture de la connexion Wayland → crash à la sortie
  (`wl_proxy_marshal` depuis `vkDestroySwapchainKHR`).

## Produit

- [x] **Rechargement de la config à chaud** — fait : le layer relit le fichier
  (≤ 0,5 s) et applique les réglages modifiables en cours de partie ; clé
  `paused` + `bdex-framegen --toggle` (raccourci on/off à lier dans le
  bureau). Mesuré : ×2 → ×1 en 0,6 s, et retour. Limite : lu depuis le
  thread de présentation, sans verrou côté lecture (jeux à plusieurs threads
  de présentation simultanés : non couvert).
- [x] **Rapport de diagnostic** — fait : `bdex-framegen --report [FICHIER]`
  (système, session, GPU/pilote, chargement du layer, manifestes, variables
  d'environnement utiles, config, fin du log).
- [x] **Profils par jeu fournis** — mécanisme fait : `layer/profiles.conf`
  installé et chargé avant le fichier utilisateur (jamais `enabled`/`paused`,
  jamais `[*]`). Une entrée vérifiée : Kenshi en interpolation. Liste
  anti-cheat : + EasyAntiCheat_EOS, GameMon64, PunkBuster. **À enrichir avec
  chaque jeu que tu valides.**
- [ ] **Packaging et versions** : AUR ou Flatpak pour la GUI, releases
  versionnées (toujours 0.3.0), section Windows dans le README.
- [ ] **Valider upscaling, HDR et 10 bits** sur de vrais écrans.
