# Changelog

Toutes les évolutions notables de ce projet sont consignées ici.
Le format suit [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/) et le
projet respecte le [versionnage sémantique](https://semver.org/lang/fr/).

## [Non publié]

## [0.1.0] - 2026-09-12

Première version.

### Ajouté

- Layer Vulkan implicite `VK_LAYER_BDEX_framegen`, activé par `BDEX_FG=1`,
  qui virtualise la swapchain du jeu et présente des images intermédiaires
  (multiplicateur x2, x3 ou x4).
- Estimation du mouvement sur GPU : pyramide de luminance (R16F quand le
  pilote le permet), block matching hiérarchique 8×8 avant et arrière avec
  biais vers le mouvement nul, filtre médian 3×3, affinage en blocs 4×4.
- Synthèse des images intermédiaires par warping bidirectionnel avec recherche
  à point fixe de la position source, pondération par la cohérence
  photométrique, fondu dans les zones non expliquées et détection de
  changement de plan (seuils mesurés sur la démo).
- Présentation depuis un thread dédié sur une file de calcul séparée de celle
  du jeu ; cadencement des images générées en modes mailbox / immediate ;
  repli mailbox + cadencement quand la file doit être partagée.
- Prise en charge des formats de swapchain RGBA/BGRA 8 bits (UNORM et sRGB),
  A2B10G10R10, A2R10G10B10 et RGBA16F ; passage transparent des autres formats
  et des swapchains multi-couches ou protégées.
- Transfert de `VK_KHR_present_id`, des fences et changements de mode de
  `VK_EXT_swapchain_maintenance1`, et de `vkReleaseSwapchainImagesEXT` ;
  gestion de `oldSwapchain` (retrait de l'ancienne swapchain avant
  remplacement).
- Configuration par variables `BDEX_FG_*` et fichier
  `~/.config/bdex-framegen.conf` avec sections par jeu (`[Jeu.exe]`).
- Modes de débogage `flow`, `split` et `passthrough`, statistiques
  périodiques, profil GPU par étape (`PROFILE`), export des images présentées
  (`DUMP`).
- Application de démonstration GLFW (`bdex_demo`) avec scène procédurale,
  compteur de frames, mode déterministe, changement de plan et vitesse
  réglables.
- Tests unitaires (configuration, formats), test bout-en-bout (débit de sortie
  doublé) et outil d'évaluation de qualité (`tools/run_eval.sh`, PSNR contre
  une référence à 60 fps).
- Scripts `tools/install.sh` / `tools/uninstall.sh` (64 bits et 32 bits),
  lanceur `bdex-framegen` avec option `--check`.

### Corrigé

- Pointeur de dispatch des objets créés par le layer (queue, command buffers),
  sans lequel les layers situés en dessous (validation Khronos) abandonnaient.
- Shaders compilés pour SPIR-V 1.0 afin d'accepter les applications
  Vulkan 1.0 (`vkcube`).
- Transition de layout de l'image de swapchain ordonnée après l'étape
  d'attente du sémaphore d'acquisition (validation de synchronisation).

[Non publié]: https://github.com/Bdexez/bdex_frame_gen/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Bdexez/bdex_frame_gen/releases/tag/v0.1.0
