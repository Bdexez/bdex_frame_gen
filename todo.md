# TODO — axes d'amélioration

État au 2026-09-24. Mesures de référence : `ROADMAP.md` et `CHANGELOG.md`.
Outils : `tools/run_eval.sh` (démo, en ligne) et `tools/eval_sequence.py`
(séquences enregistrées, hors ligne, A/B avec `--variant`).

## Décisions en attente (et ce qui demande ton matériel)

- [ ] **Options à passer par défaut ?** Toutes mesurées non négatives (ou
  presque) sur la démo en ligne et le corpus hors ligne :
  - `zero_bias_contrast=0.1` : normal +0,02, rapide +0,48 dB (rotation +2,7) ;
  - `pixel_vectors=1` : normal +0,39, rapide +0,57, rotation jusqu'à +4,7 dB ;
    coût synthèse ×2 (+1,4 ms Vega 8) ;
  - `subpixel=1` : +0,08 / +0,06 ; les trois ensemble : rapide +1,07 dB ;
  - `extrapolate_splat=1` (extrapolation) : normal +1,08, rapide +0,41 ;
    +0,8 ms.
  Changer un défaut d'interpolation déplace la référence bit à bit de la CI
  (`check_matcher_invariant.py`, f2cec5f) : à mettre à jour délibérément.
  Le launcher Windows et la GUI GTK éditent `zero_bias`.
- [ ] **Enregistrer une séquence Kenshi** (caméra en mouvement, HUD visible)
  pour trancher `zero_bias`, `static_guard` et `temporal_prediction` sur du
  vrai contenu :
  `BDEX_FG=1 BDEX_FG_MULTIPLIER=1 BDEX_FG_DUMP=/tmp/kenshi BDEX_FG_DUMP_FRAMES=240 %command%`
  puis `tools/eval_sequence.py /tmp/kenshi --variant ZERO_BIAS_CONTRAST=0.1
  --variant PIXEL_VECTORS=1 --variant STATIC_GUARD=1`, ou `tools/tune.py` sur
  plusieurs séquences ; les ajouter au corpus via `BDEX_CORPUS_DIR`.
- [x] CI Linux verte sur GitHub (confirmé le 2026-09-24, après le passage au
  chargement du layer par `XDG_DATA_HOME`). Rejouable localement : `tools/ci/run.sh`.
- [ ] Supprimer la branche locale `backup/pre-rebase` si tout est bon.
- [ ] Tags de release orphelins et sortie de la 0.4.0 (voir « Packaging »).

## Fait le 2026-09-24 (suite) — à valider en jeu

Tout est opt-in et mesuré (voir CHANGELOG) ; candidats au défaut après
essai sur de vrais jeux :
- [ ] `chroma_weight=1` (+12 dB isoluminant, +11 dB panoramique coloré ;
  −1,2 dB sur les rayures qui défilent de la démo) ;
- [ ] `dedup=1` (+0,9 à +7 dB sur contenu répété, identique sinon ; à
  vérifier sur une cinématique à 30 i/s et un émulateur) ;
- [ ] `scene_cut_local=1` (+1 dB sur un menu, neutre ailleurs) ;
- [ ] `frame_timing` : gagnant sur un jeu à pas variable, −0,6 dB au pire sur
  pas fixe — à trancher par jeu (profil) ;
- [ ] `extrapolate_accel` (extrapolation) : caméra qui accélère +1 à +2,7 dB ;
- [ ] `upscale_temporal=0.05` avec `render_scale=0.5` : vérifier en jeu
  (netteté des images générées, fantômes autour des HUD) ;
- [ ] `online_tune=1` : vérifier les décisions sur de vrais jeux (journal
  `online_tune:`) et leur cohérence d'une session à l'autre ; le critère
  (prédiction de l'image suivante) note le mouvement, pas le scintillement ;
- [ ] `flow_temporal=1` : démo +0,13 dB, scintillement −5 % ; à confirmer sur
  des jeux (textures répétées, sols, feuillages) ;
- [ ] `gpu_budget=0.25` sur l'iGPU Vega 8 : vérifier que la génération ne
  fait plus chuter les fps du jeu ;
- [ ] HDR réel (scRGB / HDR10) sur un jeu ; gamescope + un émulateur.
- Mode capture Windows : `frame_timing`/`dedup` branchés (non compilé
  localement, la CI le vérifiera au prochain push).

## Fait le 2026-09-25 — à vérifier sur du matériel

- [ ] `present_feedback` sur un écran à **fréquence fixe** (X11 sans
  compositeur, ou session DRM/gamescope) : le poste de dev (Hyprland, VRR)
  ne montre pas de grille. Vérifier dans les stats « display NNN Hz » puis
  `target_fps = auto`.
- [ ] Capture Windows en HDR sur un écran HDR (log `colour: HDR`).
- [ ] CI Windows après push : `capture.cpp` (WinRT) n'a pas été compilé ici.
- [ ] `async_compute=1` sur le Vega 8 (AMD, moteurs compute en parallèle) :
  comparer les fps du jeu et `PROFILE=1` avec/sans ; perte mesurée sur NVIDIA.

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
- [~] **Packaging et versions** — fait : `packaging/arch/PKGBUILD`
  (`bdex-framegen-git`, testé avec `makepkg` : layer, manifeste, profils, GUI,
  licence) et une vraie section Windows dans le README (app de capture
  recommandée, DLL non validée sur matériel).
  **À décider par toi :**
  - Les tags `v0.2.0`, `v0.3.0`, `v0.4.0-pre1/2` ne sont **plus des ancêtres
    de `main`** (ils pointent sur l'historique d'avant la réécriture qui a
    retiré les attributions) : les recréer sur les commits équivalents ?
  - Sortir la 0.4.0 (version CMake + section du CHANGELOG + tag `v0.4.0`,
    qui publie une Release GitHub via la CI Windows).
  - Publication AUR (PKGBUILD `packaging/arch/bdex-framegen-bin` et `-git`
    prêts, avec `.SRCINFO` ; il faut un dépôt public et un compte AUR).
    `.deb` / `.rpm` joints aux releases et flake NixOS : faits le 30/09.
    Flatpak de la GUI : écarté (le bac à sable ne voit pas les processus de
    l'hôte) — le mode texte du panneau couvre les systèmes sans GTK 4.
- [~] **Valider upscaling, HDR et 10 bits** — chemins du moteur validés : test
  ctest `formats` (`bdex_offline --format rgba8|bgra8|srgb|rgb10|rgba16f`,
  `--render-scale`) : BGRA8 identique au RGBA8, 10 bits et FP16 à 65–66 dB
  (quantification), sRGB à 0,15 dB du RGBA8 face à la vérité terrain,
  upscaling ×0,75/×0,5 à 1,1–1,5 dB du natif. Reste **sur ta machine** : un
  vrai jeu en swapchain 10 bits / HDR (scRGB, PQ) et l'upscaling à l'écran.
