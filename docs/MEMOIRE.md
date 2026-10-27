# Mémoire du projet bdex-framegen

État au **5 octobre 2026** — dernière version publiée **0.5.6** (05/10), qui
regroupe tout depuis la 0.5.5 : parité Lossless Scaling, compatibilité
matériel/distributions, SteamOS, capture Linux, injection Windows par défaut,
ménage du 04/10. Première version propriétaire.
Ce qui reste : [`ROADMAP.md`](../ROADMAP.md).

Ce fichier rassemble ce qu'il faut savoir pour comprendre, utiliser et faire
évoluer le logiciel **sur chaque système** : comment il marche, ce qui a été
amélioré, les chiffres mesurés, ce qui reste ouvert. Il se lit seul. Pour le
détail : [`README.md`](../README.md) (usage, toutes les options),
[`AGENTS.md`](../AGENTS.md) (architecture et pièges, pour l'assistant),
[`CHANGELOG.md`](../CHANGELOG.md) (journal), `docs/windows-*.md` (Windows).
**À mettre à jour à chaque session.**

## Sommaire

1. [En bref](#1-en-bref)
2. [Le moteur, commun à tous les systèmes](#2-le-moteur-commun-à-tous-les-systèmes)
3. [La couche Vulkan : présentation et cadencement](#3-la-couche-vulkan--présentation-et-cadencement)
4. [Fonctionnement par système](#4-fonctionnement-par-système)
5. [Activation et configuration](#5-activation-et-configuration)
6. [Outils, tests et CI](#6-outils-tests-et-ci)
7. [Historique des améliorations](#7-historique-des-améliorations)
8. [Chiffres à retenir](#8-chiffres-à-retenir)
9. [Problèmes connus et pistes](#9-problèmes-connus-et-pistes)
10. [Conventions de travail](#10-conventions-de-travail)

---

## 1. En bref

bdex-framegen **génère des images intermédiaires** entre celles d'un jeu, par
flux optique calculé sur le GPU — le principe de Lossless Scaling (LSFG) ou
d'AMD Fluid Motion Frames. Le jeu rend 30 i/s, l'écran en montre 60, 90,
jusqu'à ×8, ou de façon **adaptative** tout ce que permet la fréquence de
l'écran. En plus : upscaling spatial (bilinéaire, bicubique, Lanczos + netteté
CAS) et un compteur d'images à l'écran (HUD). L'objectif affiché : reproduire
la génération d'images de Lossless Scaling, en mieux.

**Deux produits, un seul moteur** (`FrameGen`, `layer/src/framegen.cpp`, et
tous les shaders de `layer/shaders/`) :

| | Couche Vulkan (« mode 1 ») | Application de capture (« mode 2 ») |
|---|---|---|
| Fichier | `libVkLayer_bdex_framegen.so` / `VkLayer_bdex_framegen.dll` | `bdex_capture.exe` |
| Principe | couche Vulkan implicite qui prend la main sur la présentation du jeu | capture la fenêtre du jeu (WGC), génère, réaffiche dans une surcouche |
| Systèmes | **Linux** (produit principal), Windows (compilée, jamais validée sur matériel) | Windows x64 |
| Jeux | Vulkan natifs, Direct3D via Proton (DXVK, VKD3D-Proton), OpenGL via Zink | tous (D3D9 à 12, OpenGL, Vulkan), en fenêtré ou sans bordure |
| Latence de principe | aucune capture : dans le chemin de présentation | + environ une image (capture) |
| Anti-triche | se retire toute seule (EAC, BattlEye…, jeux compétitifs) | sûre : aucune injection |

Projet C++20 / Vulkan / CMake ; shaders GLSL compilés en SPIR-V et embarqués
au build. Auteur : Bdexez.

---

## 2. Le moteur, commun à tous les systèmes

### Chaîne de traitement, à chaque image présentée par le jeu

1. **Historique** : les images que le jeu rend servent directement
   d'historique (pas de copie supplémentaire).
2. **Analyse** : pyramide de luminance (niveau fin d'environ 0,5 Mpx en
   automatique, réglable par `flow_scale`).
3. **Flux optique** : *block matching* hiérarchique, avant et arrière, blocs
   8×8 cherchés autour de la prédiction du niveau plus grossier ; coût = SAD de
   luminance + pénalités (`smoothness`, `zero_bias` relatif au contraste et
   plafonné) ; un candidat « immobile » garde les HUD stables. Puis lissage et
   raffinement 4×4. Options : vecteurs par pixel, sous-pixel (Lucas-Kanade ou
   parabolique), cohérence temporelle, gradient, census, chrominance…
4. **Coupure de scène** : coût moyen du matching + écart d'histogramme de
   luminance, pour l'image entière et par tuile. Sur une coupure, l'image
   générée devient une copie (ou un fondu) de la réelle.
5. **Synthèse**, dans l'un des trois modes :
   - **interpolation** (défaut) : warp bidirectionnel entre les deux images
     réelles, `t = i/m`. C'est le plus propre, mais l'image réelle est montrée
     une demi-image plus tard (×2).
   - **extrapolation** : l'image réelle part tout de suite et l'image suivante
     est prédite, soit ~0 ms de retenue. Les artefacts sont plus visibles sur
     les mouvements brusques ; les passes `extrapolate_splat` et `reveal_fill`
     les réduisent.
   - **hybride** (×3 et plus) : une image interpolée, la réelle, puis des
     prédictions ; la réelle n'a qu'1/m d'image de retard.
6. **Upscaling** si `render_scale < 1` (option temporelle), **CAS**, **HUD**.

Un **tampon d'état sur le GPU** calcule `t` au temps d'affichage réel, repère
les images répétées (`dedup`) et commande des passes en dispatch indirect,
**sans relecture par le CPU** avant la présentation.

### Qualité et réglage automatique

- **Profils** (`preset`) : `auto` (défaut), `performance`, `balanced`,
  `quality`, `latency` (extrapolation ×2), `enhanced`.
- **Choix de `auto`** : GPU intégré, logiciel ou moins de 2 Go de VRAM →
  `performance` ; GPU dédié → `balanced` + options mesurées positives
  (vecteurs par pixel, splat + reveal_fill, coupures par tuile).
- **`online_tune`** : essaie des options pendant le jeu. Une sonde reconstruit
  une image réelle que la synthèse n'a pas vue, et la décision est mise en
  cache par jeu.
- **`gpu_budget`** : gouverneur qui baisse la qualité si la génération dépasse
  une part du temps d'image du jeu.
- **Passes optionnelles** : classes `FgPass` (`layer/src/passes/`) ;
  **options** : une ligne dans le registre de `config.cpp`.

### Formats et matériel

- Formats : 8 bits RGBA/BGRA (UNORM et sRGB), 10 bits (A2B10G10R10 /
  A2R10G10B10), RGBA16F. En HDR, HDR10 (PQ) et scRGB sont ramenés en lumière
  linéaire relative au blanc SDR. Les autres formats passent sans génération.
- Le moteur tient dans les **limites que tout GPU Vulkan garantit** (4 images
  de stockage par shader, 16 Ko de mémoire partagée).
- Le matcher complet (~21 Ko partagés) ne sert que là où il y a 32 Ko ; sinon,
  sa variante allégée.
- Un GPU sous ces limites laisse passer les images, avec la raison dans
  `--report`. Le test `min_limits` simule un petit GPU.
- **Invariant sacré** : une option à 0 reproduit **bit à bit** le chemin
  historique. La CI compare le chemin par défaut au commit `f2cec5f`, et
  toute modification du matcher doit le préserver (§6 d'AGENTS.md, régression
  `0549e44`).

---

## 3. La couche Vulkan : présentation et cadencement

- **Swapchain virtuelle** : le jeu rend dans des images de la couche. Le
  handle qu'il reçoit est celui de la vraie swapchain à sa création ; si la
  couche la recrée dessous (fenêtre redimensionnée par Wine), les appels
  qu'elle relaie suivent la swapchain courante (`currentReal()`, corrigé le
  30/09).
- **Deux threads, deux files** : au `vkQueuePresentKHR`, le thread du jeu
  soumet la synthèse sur une file de calcul. Un *worker* copie les images vers
  la vraie swapchain, les cadence et les présente sur une autre file.
- **Mode de présentation** : `present_mode = auto` remplace le FIFO demandé
  par le jeu par **mailbox**. Pas de FIFO par défaut : il ajoute de la latence
  (choix de l'utilisateur).
- **Cadencement** (refait le 29/09) :
  - L'origine est la **fin du travail GPU**, pas l'appel du jeu : un jeu
    limité par le GPU perdait toutes ses images générées.
  - Les présentations suivent la **grille d'affichage**. Elle est apprise par
    `present_wait` (`DisplayClock`), transmise par le panneau, ou donnée par
    gamescope. Une image par vblank.
  - Les prédites sont abandonnées dès que l'image suivante est prête.
- **Multiplicateur** :
  - fixe, de ×2 à ×8 (images de transit quand la surface limite le nombre
    d'images) ;
  - « Auto » : le plus grand entier sous la fréquence de l'écran ;
  - « Adaptatif » : ratio fractionnaire, par exemple 50 i/s sur un écran
    144 Hz → 144 i/s affichées.
- **Latence** :
  - l'interpolation retient l'image réelle d'une demi-image (×2) ;
    l'extrapolation, d'environ 0,1 ms ;
  - `max_frame_latency = 1` : le jeu ne prend pas d'avance sur le GPU ;
  - `latency_limiter` : retient le début de l'image suivante, à la façon de
    Reflex ; **il s'efface** (02/10) quand le jeu a son propre Reflex
    (`VK_NV_low_latency2` en mode basse latence, `vkLatencySleepNV` dans la
    dernière seconde) ou Anti-Lag (`vkAntiLagUpdateAMD` actif) : deux
    retenues empilées se gênent, celle du pilote est mieux informée (non
    essayé sur un jeu Reflex) ;
  - **`interp_hold`** (02/10, défaut 1 = identique au bit près) : fraction de
    la retenue habituelle de l'image réelle en interpolation. En dessous de
    1, les positions jusqu'à la réelle sont espacées de h/m d'intervalle
    (`FrameOrder::offset`, `paceTarget`) et chaque image montre le contenu
    de son instant d'affichage (`t` = 1 − (real − p)·h/m) : mouvement juste,
    cadence irrégulière. Bornée à un rafraîchissement d'écart (`displayHz`) :
    sans borne, 0,5 n'affichait que 116 images sur 120 et 0,25, 95 (deux
    images dans le même rafraîchissement). Seulement dans la couche
    (`FrameOrder::of(…, withHold)`) : la capture et `bdex_offline` restent à
    l'espacement régulier ; hors adaptatif. Les temps d'affichage passent au
    tampon d'état pour que `frame_timing` suive. **`auto` (−1)** : 1,25
    rafraîchissement d'écart (exactement un : 1,2 image/s perdue sur 80), et
    seulement si le gain dépasse 2 ms ; mesuré ×2 à 144 Hz : 30 i/s 56,7 →
    48,3 ms, 40 i/s 43,8 → 40,7, aucune image perdue, 60 i/s inchangé ;
  - **`pipeline_cache`** (03/10, défaut 1) : `VkPipelineCache` de la couche sur
    disque (`DeviceData::pipelineCache`, en-tête vérifié contre
    `pipelineCacheUUID`, écrit après la construction de `FrameGen` et à la
    destruction du device) ; démarrage 0,88 → 0,15 s sans le cache du pilote
    NVIDIA ;
  - **`thread_priority`** (03/10, défaut 1, `thread_priority.cpp`) : SCHED_RR
    si permis, sinon rtkit `MakeThreadHighPriority` nice −10 (libdbus par
    `dlopen`, connexion privée fermée aussitôt), sinon rien. Vérifié : rtkit
    l'accorde sur le poste de dev. Gain mesuré faible (réveil 39 → 25 µs sous
    charge, images affichées inchangées : gamescope headless sans
    CAP_SYS_NICE manque lui-même de CPU) ;
  - **`precise_wake`** (03/10, défaut 1) : `PR_SET_TIMERSLACK` à 1 ns sur le
    thread de présentation ; retard de réveil 56 → 7 µs (repos), 130 → 38 µs
    (12 processus occupés). Attente active des 0,5 dernière ms : **refusée**
    (sous charge, le `yield` rend le CPU : 1 ms de retard, 114,7 images
    affichées sur 120) ;
  - **`vrr_hold`** (03/10, défaut 1) : écran sans grille fixe mais avec
    retour d'affichage (≥ 120 images vues, `variableRefresh`) → `interp_hold`
    1 traité comme auto ; simulé (`pacing_vrr_auto_hold`) : réelle 3,8 ms plus
    tôt à 40 i/s ×2, 144 Hz max. Non mesuré sur un vrai écran VRR ;
  - `synth_priority` (02/10, avec `async_compute`) : priorité globale haute
    pour la file de synthèse (sa famille a son propre `VkDeviceQueueCreateInfo`
    ; une famille partagée avec le jeu n'a qu'une priorité). Refusée sans
    privilèges (NVIDIA, amdgpu sans CAP_SYS_NICE : `VK_ERROR_NOT_PERMITTED`,
    mesuré par une sonde : MEDIUM accepté, HIGH/REALTIME refusés) → device
    recréé sans ;
  - **`skip_fast`** (02/10, activé par défaut, multiplicateur fixe) : pas de
    génération quand le jeu est déjà à ≥ 85 % de la fréquence de l'écran
    (reprise sous 75 %, ½ s d'hystérésis) — cas Valheim 130 i/s sur 144 Hz.
    Mesuré 130 i/s ×2 sur 144 Hz : image→écran 16,0 → 11,8 ms. Statut
    `reason=fast` (panneau, `status.cpp`) ; fréquence inconnue : sans effet.
    Fréquence utilisée (`fastGameHz`) : le cycle de gamescope seulement si la
    limite Steam n'a jamais été active sur ce device (sinon il peut rester
    celui de la limite levée : un jeu à 30 i/s restait sans génération,
    attrapé par `gamescope_limit`), ailleurs le max mesurée / panneau ;
  - **Mesure réelle** (02/10) : `bdex_demo --latency-test [auto]` (flash blanc
    par entrée, journal `--latency-log`) + `latency_log` côté couche (chaque
    image affichée, retour d'affichage) → `tools/latency_report.py` (entrée
    → premier photon / image réelle) ; `tools/latency_bench.sh` compare des
    réglages dans gamescope sans fenêtre ; ctest `latency_probe` ;
  - `low_latency` et `fps_limit` complètent.
- **Rattrapage** (30/09) : quand la présentation prend du retard, une image
  est montrée sans générée. Sinon une file qui s'est formée ne se vide jamais
  quand le jeu produit au rythme exact de l'écran.
- **Extensions cachées au jeu** : `swapchain_maintenance1` et
  `present_id`/`present_wait` (1 et 2), qui visent une swapchain que la
  couche virtualise. VKD3D-Proton se bloquait sinon.
- **Statut en direct** : un fichier par processus, lu par le panneau et
  `bdex-framegen --report`. Emplacement : `~/.cache/bdex-framegen/run/<pid>.status`
  sous Linux, `%TEMP%\bdex-framegen\` sous Windows. Il contient i/s du jeu
  et en sortie, fréquence, latence, limite Steam, passes actives.

---

## 4. Fonctionnement par système

### 4.1 Linux, bureau

**Installation**
- Archive précompilée : `./install.sh` installe dans `~/.local`, sans root.
  Même sur système immuable (SteamOS, Bazzite, Silverblue).
- Contenu installé :
  - `lib/` (64 bits), `lib32/` (jeux 32 bits) ;
  - manifestes dans `share/vulkan/implicit_layer.d/` ;
  - `bin/bdex-framegen` (lanceur, diagnostic) et `bin/bdex-framegen-gui`
    (panneau) ;
  - profils, description des options, raccourci d'application.
- Depuis les sources : `tools/install.sh`.
- **Paquets de distribution** (depuis le 30/09, `tools/package-native.sh`,
  refaits à partir des archives, publiés avec chaque release) :
  - `.deb` amd64/arm64 : couche dans `/usr/lib/x86_64-linux-gnu` (32 bits :
    `/usr/lib/i386-linux-gnu`) ; dépend de `libc6 (>= 2.29)`, `libvulkan1` ;
    recommande `python3-gi`, `gir1.2-gtk-4.0` ;
  - `.rpm` x86_64/aarch64 : `/usr/lib64` (32 bits : `/usr/lib`), `AutoReqProv:
    no` (sinon la couche 32 bits tirerait une glibc 32 bits), dépendances
    riches `(vulkan-loader or libvulkan1)` pour Fedora et openSUSE ;
    `rpm-ostree install` sur Bazzite / Silverblue ;
  - Arch : `packaging/arch/bdex-framegen-bin` (l'archive) et `-git`
    (sources, couche 32 bits, runtime C++ statique), avec `.SRCINFO` —
    **prêts pour l'AUR, non publiés** (dépôt privé : l'URL des sources
    renvoie 404) ;
  - NixOS : `flake.nix` (paquet, `layer32`, module
    `programs.bdex-framegen.enable` qui lie `share/vulkan` dans le profil
    système). Nix sur une autre distribution : non recommandé (couche liée à
    la glibc du store, chargée dans des jeux à glibc plus ancienne).
  - La couche trouve ses profils à `../share` **ou `../../share`** de son
    dossier (`platformBundledProfilesPath`) ; `BDEX_INSTALL_LIBDIR` règle le
    dossier au build.
  - Un paquet n'écrit pas dans le home : `bdex-framegen --sandboxes` copie
    la couche dans les lanceurs Flatpak/Snap (le postinst le rappelle). Après
    une mise à jour, le panneau recopie à son ouverture les copies périmées
    (`flatpak.sh --check`), `--check` sort en 2 et `--report` les signale.
  - **Toute copie d'une `.so` installée passe par un nouveau fichier**
    (`flatpak.sh` : copie puis renommage ; `install.sh` : suppression puis
    copie) : copier par-dessus tronque la bibliothèque qu'un jeu en cours a
    mappée (SIGBUS). Testé par `tests/linux_scripts.sh` (inode qui change,
    fichier ouvert intact).
  - **Paquets source** (02/10, job CI `source-packages`, chacun construit
    depuis le checkout dans le conteneur de la distribution puis installé) :
    `packaging/rpm/bdex-framegen.spec` (COPR, OBS ; Fedora 44, Tumbleweed),
    `packaging/debian/` (PPA, OBS ; Ubuntu 24.04/22.04, Debian 12), overlay
    Gentoo `packaging/gentoo` (`ABI_X86="64 32"`), template Void (glibc,
    musl). Pièges : dnf satisfait `libstdc++-devel` de gcc-c++ par le seul
    paquet i686 (pas de `c++config.h` 64 bits) → `libstdc++-devel%{?_isa}`
    exigé, et `libatomic` 64 + 32 ; les macros `%cmake` de Fedora et
    d'openSUSE diffèrent (dossier, répertoire courant) → `cmake` simple ;
    Void : xbps-src en conteneur = utilisateur du groupe `xbuilder`, uchroot,
    `--privileged` (le mode ethereal ne bootstrappe pas) ; Gentoo : seules les
    phases de l'ebuild (`ebuild … install`), vulkan-loader[abi_x86_32] non
    fusionné (pile X/Wayland 32 bits à compiler).
  - **Couche 32 bits depuis les sources** : `tools/install.sh` testait
    `/usr/lib32/libvulkan.so.1` (chemin d'Arch seulement) et sautait la
    couche 32 bits sous Debian/Ubuntu/Fedora ; il teste maintenant la
    liaison d'une bibliothèque C++ 32 bits. Fedora : `glibc-devel.i686
    libstdc++-devel.i686 libatomic.i686`.
- **Panneau sans GTK 4** (Ubuntu 20.04, Debian 11) : mode texte dans un
  terminal (`text_main`), même session/battement, mêmes options ; lancé
  depuis le menu, il ouvre un terminal (`$TERMINAL`, `xdg-terminal-exec`,
  Ptyxis, kgx, foot, kitty… : Fedora 41+ et GNOME 47+ n'ont aucun des
  anciens) ; `--text` le force. Fréquence de l'écran : `hyprctl`,
  `kscreen-doctor` (Plasma seulement : ailleurs il attend indéfiniment, vu
  sous Hyprland, d'où aussi un `--report` bloqué jusqu'au 02/10),
  `wlr-randr`, DisplayConfig de GNOME, puis xrandr (qui ne voit que
  Xwayland : 143,78 Hz lus pour 143,91). Sans GTK, des classes factices (`_Stub`) remplacent les modules
  GTK pour que les classes de fenêtre restent définies (et
  `tests/panel_logic.py` tourne sans GTK).
- **MangoHud** : chargé seul, il se place *au-dessus* de la couche (côté
  jeu) — mesuré : 330 i/s comptées pour 660 affichées, dessin sur les
  réelles seulement. `bdex-framegen --mangohud` (= `VK_INSTANCE_LAYERS=
  VK_LAYER_BDEX_framegen:VK_LAYER_MANGOHUD_overlay_x86_64:…_x86`) le met
  dessous. `VK_LOADER_LAYERS_ENABLE` ne change pas l'ordre.

**Principe** : la couche est **implicite**. Elle se charge dans chaque
programme Vulkan et reste **transparente** tant qu'elle n'est pas activée
(voir [§5](#5-activation-et-configuration)).

**Serveurs graphiques**
- **X11 / Xwayland** : chemin le plus éprouvé.
- **Wayland natif** : fonctionne, upscaling compris. Une première swapchain
  « jetable » apprend la taille de la fenêtre, puis le jeu recrée la sienne à
  la taille réduite.
- **Retour d'affichage** (`present_wait`) :
  - gamescope : grille exacte ;
  - Hyprland (poste de dev, VRR) : pas de grille fixe → rien ne change ;
  - KDE + NVIDIA : pas de retour du tout → le panneau transmet la fréquence
    qu'il lit.

**GPU**
- NVIDIA propriétaire : poste de dev, RTX 3060 Ti, 144 Hz.
- AMD et Intel sous Mesa (RADV, ANV) ; Radeon Vega 8 testée pour la qualité.
- NVK ; lavapipe (logiciel, pour les tests).
- Portables hybrides : le jeu doit tourner sur le GPU relié à l'écran (le
  rapport le signale). `bdex-framegen --gpu screen|discrete|integrated|
  nvidia|amd|intel|list -- jeu` (02/10) : `MESA_VK_DEVICE_SELECT` (+
  `_FORCE_DEFAULT_DEVICE`), `DRI_PRIME`, PRIME offload NVIDIA
  (`__NV_PRIME_RENDER_OFFLOAD`, `__VK_LAYER_NV_optimus`), et
  `DXVK_FILTER_DEVICE_NAME` / `VKD3D_FILTER_DEVICE_NAME` (Proton choisit son
  GPU lui-même). Liste par vulkaninfo, écrans par `/sys/class/drm`.

**OpenGL** : `bdex-framegen --gl -- jeu` fait passer le jeu par Zink
(OpenGL sur Vulkan, en forçant le GL de Mesa).

**Exclusions automatiques**
- Programmes de bureau : applications GTK 4 (elles dessinent en Vulkan),
  navigateurs, Discord/Electron, Steam et steamwebhelper, OBS, gamescope,
  mangoapp…
- Jeux protégés par un anti-triche (EAC, BattlEye, GameGuard, XIGNCODE3,
  PunkBuster, miHoYo) ou compétitifs (CS2, Dota 2…) : la couche s'y retire.

**Outils**
- `bdex-framegen-gui` : le panneau.
- `bdex-framegen` :
  - `-- jeu` : lancer un jeu avec la couche ;
  - `--toggle` : pause à chaud ;
  - `--report` : diagnostic en français ;
  - `--always-on` / `--always-off` : génération sans le panneau ;
  - `--record` : enregistrer une séquence ;
  - `--tuned-profile` : profil issu d'`online_tune` ;
  - `--check` : la couche est-elle vue par le loader.
  - `--issue-url [FICHIER]` : lien vers le formulaire d'issue pré-rempli
    (`.github/ISSUE_TEMPLATE/bug_report.yml`) depuis un rapport — aussi
    affiché à la fin de `--report`, bouton « Rapport… » du panneau ; seulement
    le résumé (jamais config ni journal), `$HOME` remplacé par `~`.

### 4.2 Jeux Proton et runtime Steam

**Ce qui marche**
- La couche marche dans le conteneur du runtime Steam (pressure-vessel),
  comme MangoHud. Kenshi (DXVK) : ×2 validé.
- Les blocages DXVK/VKD3D (Oblivion Remastered) sont réglés. La couche ne
  transmet pas `VK_SUBOPTIMAL_KHR`, cache les extensions de présentation et ne
  présente jamais en FIFO.
- **Vérifié en CI depuis le 03/10** (`tools/ci/proton-check.sh`, job
  `source-packages`) : un programme Direct3D 11 (`tests/proton/d3d11_probe.cpp`,
  MinGW) sous Wine 9 + DXVK 2.5.3, puis un programme Direct3D 12
  (`d3d12_probe.cpp`) sous VKD3D-Proton 3.0.1 + le `dxgi` de DXVK (comme
  Proton ; avec le `dxgi` de Wine, VKD3D-Proton plante), lavapipe, Ubuntu
  24.04 — la couche se charge dans le processus Wine (winevulkan), génère,
  ×2,00 dans les deux cas. Piège : sous
  Xvfb, Wine voit l'écran à 0 Hz et DXVK (2.5 à 2.7) divise par zéro au
  premier `Present` ; d'où Xorg + pilote `dummy` avec un mode à 60 Hz
  (`tests/proton/xorg-dummy.conf`). DXVK 3.x refuse le lavapipe de Mesa 25.2
  (`VK_KHR_load_store_op_none`).
- **Pilote Wayland de Wine** (`PROTON_ENABLE_WAYLAND`, 03/10,
  `tools/ci/proton-wayland-check.sh`) : Wine 11 de WineHQ, sans serveur X
  (weston headless), DXVK : chemin Wayland natif de la couche, ×2,00. Le
  Wine 9 d'Ubuntu n'a pas de Vulkan dans son pilote Wayland.

**Points propres au runtime Steam**
- Dossier de session et de statut dans `~/.cache/bdex-framegen/run`, pas
  dans `$XDG_RUNTIME_DIR` : le conteneur Steam ne le partage pas (PRAGMATA ne
  démarrait pas).
- Couche 32 bits pour les jeux 32 bits (DXVK 32 bits).
- L'archive embarque son runtime C++ : celui de Steam peut être plus ancien.

**Diagnostic** : `BDEX_FG_LOG_FILE` et `PROTON_LOG=1`. **Journal de plantage** (02/10, `crash_log`, `crash.cpp`) : une faute
dont l'instruction (ou l'un des 4 premiers appelants) est dans la couche
écrit `~/.cache/bdex-framegen/crash-<jeu>-<pid>.txt` (décalages pour
addr2line, réglages), puis chaîne vers le gestionnaire précédent : Wine se
sert de SIGSEGV, on ne garde jamais une faute. Installé seulement si la
couche est active et hors anti-triche ; `--report` les reprend. Les couches de
validation ne se chargent pas dans pressure-vessel.

**Réglage par jeu** : option de lancement Steam `BDEX_FG=1 %command%`, ou
section `[exe]` dans la configuration.

### 4.3 Lanceurs Flatpak (Steam, Heroic, Lutris, Bottles)

- **Le problème** : leur bac à sable ne voit ni `~/.local` ni `~/.config`.
- **À l'installation**, `install.sh` (via `flatpak.sh`) copie la couche et ses
  manifestes dans `~/.var/app/<app>/data`, l'`XDG_DATA_HOME` du bac à sable,
  au même chemin absolu dedans. Il copie aussi les profils et la
  configuration.
- **Le panneau** recopie la configuration à chaque enregistrement. Il trouve
  les jeux (PID interne `NSpid`, fichiers vus par `/proc/<pid>/root`) et leur
  écrit la session. `--report` et `--toggle` suivent.
- **Installer les lanceurs d'abord**, ou relancer `install.sh` après (ou
  `bdex-framegen --sandboxes`).
- **Extension Flatpak** (02/10, `packaging/flatpak`,
  `org.freedesktop.Platform.VulkanLayer.BdexFramegen`, runtime 25.08, prête
  pour Flathub, comme MangoHud/vkBasalt) : toute appli Flatpak sur ce
  runtime charge la couche (64 et 32 bits, `/usr/lib/extensions/vulkan/
  BdexFramegen/lib/<triplet>`), `flatpak update` la met à jour.
  `flatpak.sh` ne copie alors plus la couche dans l'appli (copies retirées :
  une copie périmée serait chargée d'abord), seulement la config, plus une
  marque `data/bdex-framegen/extension` lue par le CLI ; le panneau détecte
  lui-même la branche du runtime de l'appli (`metadata`). Vérifiée par
  `tools/ci/flatpak-check.sh` (conteneur, SDK en volume Docker) : chargée
  par le loader 64 bits du runtime, listée (`VK_LAYER_BDEX_framegen_32`) par
  le loader i386. Pièges : pas de `vulkaninfo` dans le SDK (sonde C
  `tools/ci/vk_layer_probe.c`) ; sans `--allow=multiarch`, seccomp tue un
  programme 32 bits (SIGSYS, code 159) ; aucun pilote 32 bits monté dans le
  SDK (GL32 est pour les applis qui le déclarent, comme Steam).
- **Snap** (Steam d'Ubuntu, depuis le 30/09) : bibliothèque dans
  `~/snap/steam/common/bdex-framegen` (gardée d'une révision à l'autre),
  manifeste, configuration et session dans le HOME du snap
  (`~/snap/steam/current/.local/share`, `.config`, `.cache`). Pas d'espace
  de PID propre aux snaps. Écrit d'après l'organisation de snapd et simulé
  dans un faux home ; **jamais essayé sur un vrai snap** (AppArmor pourrait
  refuser le chargement).

### 4.4 SteamOS, Steam Deck et sessions gamescope

**SteamOS 3.9.2** (image du 25/09/2026, liste des paquets publiée par Valve) :
- gamescope 3.16.30, glibc 2.43, loader Vulkan 1.4.350, Mesa 26 (RADV) ;
- python 3.14 + PyGObject 3.56 + GTK 4.22 ;
- **ni libadwaita ni zenity** → le panneau utilise son mode GTK 4 simple (vérifié).

**Mode Bureau** (KDE Plasma) : installation par Konsole (`./install.sh`,
dans `~/.local` : les mises à jour de SteamOS la conservent) ; le panneau
fonctionne.

**Mode Jeu** : gamescope en compositeur + Steam, **une seule application à la
fois**. Le panneau ne peut donc pas rester ouvert à côté du jeu. Trois voies :
- dans le panneau, **« Mode Jeu — sans le panneau »** : tous les jeux, ou le
  jeu sélectionné, génèrent dès leur lancement ;
- en terminal, `bdex-framegen --always-on [JEU]` / `--always-off` ;
- option de lancement Steam `BDEX_FG=1 %command%`.

Même activée partout, la couche épargne gamescope (le compositeur charge la
couche lui aussi), Steam et les applications de bureau.

**Couche WSI de gamescope** : elle est sous la nôtre, et la couche y lit deux
choses :
- la **cadence réelle** (60 Hz, 90 Hz sur OLED) via
  `vkGetRefreshCycleDurationGOOGLE`, sans activer l'extension (NVIDIA ne l'a
  pas) ;
- l'**état du limiteur** de Steam (`GAMESCOPE_LIMITER_FILE`).

Défaut de gamescope contourné : une swapchain recréée ne reçoit jamais la
cadence ; la couche garde la dernière par surface.

**Limite d'images/s de Steam** (menu rapide) : gamescope n'affiche rien
au-delà, images générées comprises. Sous la limite, la couche :
- génère seulement jusqu'à elle ;
- n'envoie une image qu'une fois la précédente affichée ;
- fait attendre l'affichage de l'image précédente à un jeu déjà à la limite,
  comme DXVK.

Pour **60 i/s depuis 30** : limite sur *Désactivée* (ou sur la fréquence de
l'écran), et plafonner le jeu lui-même (son limiteur, ou `fps_limit = 30`),
ou multiplicateur *Adaptatif*.

**Divers** :
- HDR (Deck OLED) : passe, métadonnées comprises.
- Preset automatique `performance` (GPU intégré RDNA2, 8 CU).
- Jeux 32 bits : couverts. Exécutée pour la première fois le 03/10
  (`tools/ci/lib32-check.sh` : vkcube 32 bits, lavapipe i386, ×2,00).

**Autres cas gamescope**
- **Sessions gamescope** ailleurs (Bazzite, ChimeraOS…) : même fonctionnement.
- **gamescope imbriqué** sur un bureau, backend SDL : le profil fourni
  `[gamescope] hide_present_ext = 0` permet de générer sur la sortie du
  compositeur (tout ce qui tourne dedans, par exemple glxgears 337 → 675 i/s).
  Le backend Wayland par défaut présente des dmabufs, pas de swapchain : la
  couche ne peut rien y faire.

**Non validé sur un vrai Deck** : tout a été mesuré dans un gamescope
headless de la même génération.

**Plugin Decky** (03/10, `packaging/decky`) : menu d'accès rapide du Deck
en mode Jeu — génération dans tous les jeux (`require_app = 0` dans `[*]`),
multiplicateur, mode, pause, compteur, état du jeu en cours (fichiers de
statut). Le backend `main.py` édite la configuration comme le panneau
(clé réécrite une seule fois dans les lignes globales, sections des jeux
gardées) ; construit par `tools/ci/decky-check.sh` (Docker Node 20, pnpm,
`tsc`), zip joint aux releases, testé sans Decky (`decky_backend`). **Jamais
lancé sur un vrai Deck** : à valider (Decky « Installer depuis un ZIP »).
Réglages « pour ce jeu seulement » (section du jeu en cours, nom tiré du
fichier de statut). Depuis le 03/10, sans le panneau (`require_app = 0`),
un multiplicateur / mode / réglage de flux changé dans le fichier reconstruit
l'étage de sortie du jeu en cours (avant : seules les options « live »), et
`require_app` basculé met en pause / relance (test `live_config`).

### 4.5 Distributions, musl et ARM64

**Archives précompilées**
- Construction : SPIR-V compilé par glslc (Ubuntu 24.04), couches liées sous
  Ubuntu 20.04 (GCC 10, runtime C++ statique), avec
  `tools/ci/package-release.sh`.
- Besoin : **glibc ≥ 2.29** (x86_64) ou 2.30 (aarch64). Cela couvre Ubuntu
  20.04 et plus, Debian 11+, Mint 20+, Pop!_OS, Fedora, Arch/Manjaro/
  EndeavourOS, SteamOS/Bazzite, openSUSE Leap 15.3+ et Tumbleweed, RHEL 9…
- Avant le 30/09, il fallait glibc 2.36, d'où « ça ne marche pas chez un
  ami » sur les distributions plus anciennes.
- **Manifeste** au format 1.1.2 avec `library_arch` (64/32) : aucun
  avertissement des vieux loaders, pas d'erreur « wrong ELF class » avec les
  récents.
- **Vérification à chaque CI** (`tools/ci/archive-check.sh`), comme un
  utilisateur :
  - installation, chargement (`vulkaninfo`) et **génération ×2 avec vkcube**
    sur Ubuntu 20.04, 22.04 et 24.04, Debian 11 et 12, Fedora ;
  - l'arm64 dans un Debian arm64 émulé (qemu-user) ;
  - Debian 11 : chargement seulement, son Mesa ne sait pas présenter sur Xvfb ;
  - les `.deb` (apt) sur Ubuntu 20.04 à 24.04 et Debian 12, les `.rpm` (dnf,
    zypper) sur Fedora et openSUSE Tumbleweed : chargement, **profils lus**
    (un profil `[vulkaninfo]` ajouté doit apparaître), ×2.
- **Panneau** (job CI `panel`, `tools/ci/panel-check.sh`) : Ubuntu 20.04
  (mode texte), Debian 12 et Arch (GTK 4 avec et sans libadwaita, texte).
- **Nix** (job CI `nix`) : paquets 64 et 32 bits construits, couche chargée
  par le loader du store. Module NixOS évalué en conteneur : les deux
  manifestes dans `system.path`, sans collision.

**Depuis les sources**
- Prérequis : GCC ≥ 10 ou Clang, en-têtes Vulkan ≥ 1.3.204, glslc ou
  glslangValidator. Le loader n'est pas nécessaire pour la couche seule.
  Options utiles : `-DBDEX_VULKAN_INCLUDE_DIR`, `-DBDEX_SPIRV_DIR`.
- Vérifié à chaque CI (`tools/ci/distro-check.sh`), build `-Werror` + démo à
  travers la couche : Ubuntu 22.04, Debian 12, Fedora, Arch, openSUSE
  Tumbleweed, Alpine.

**musl (Alpine, Void musl, Chimera)**
- Compiler depuis les sources : les archives visent glibc.
- lavapipe (le Vulkan logiciel de Mesa) plante sous musl : ses threads n'ont
  que 128 Ko de pile et LLVM les dépasse. C'est Mesa, pas la couche. Les
  pilotes GPU ne compilent pas avec LLVM.

**ARM64**
- Archive croisée, pour Asahi (Mac Apple Silicon), portables Snapdragon,
  Raspberry Pi 5 (V3D : 4 images de stockage, d'où le respect des limites
  garanties).
- Jamais essayée sur du vrai matériel ARM.
- **Asahi** : Steam y lance les jeux x86 dans une machine virtuelle muvm +
  FEX, dont le loader Vulkan x86_64 ne peut pas charger une couche ARM64 ;
  installer l'archive x86_64 à côté ne marche pas non plus (même nom de
  manifeste, `library_arch` ne distingue que 32/64). Non couvert.

### 4.5 bis Linux, génération par capture (`bdex-capture`)

Depuis le 03/10 (`capture/linux`). Pour les jeux où la couche se retire
(anti-triche), les programmes qui ne présentent pas en Vulkan, et **deux
GPU** : `--gpu` fait générer sur le GPU de l'écran pendant que le jeu rend
sur l'autre (le mode deux GPU de Lossless Scaling).

**Chaîne** : fenêtre du jeu redirigée hors écran (XComposite, automatique :
l'overlay ne se capture pas lui-même), une image = un événement XDamage,
lue par XShm dans un anneau de 3 segments alignés page, importés tels quels
en mémoire hôte (`VK_EXT_external_memory_host`, NVIDIA et lavapipe
acceptent ; sinon copie) → historique → `FrameGen` inchangé → swapchain
Xlib d'une fenêtre override-redirect **sans région d'entrée** (clics et
touches vont au jeu), relevée toutes les 250 ms, qui suit la fenêtre.
Cadencement repris de la capture Windows (images prédites abandonnées dès
qu'une nouvelle image du jeu arrive). Réglages : fichier de config avec la
section du jeu (processus de la fenêtre, `.exe` sous Proton), `BDEX_FG_*`,
puis `-m/--mode/--preset/--hud`. Ctrl+Alt+G / Ctrl+Alt+Q.

**Mesures** (RTX 3060 Ti, gamescope headless sans sa couche WSI, jeu à
60 i/s, 144 Hz) : ×2 120 i/s, 10,4 ms de l'arrivée de l'image à l'écran en
interpolation, 0,6 ms en extrapolation ; hybride ×3 178 i/s, 7,8 ms. Piège
corrigé : une image du jeu repérée pendant l'attente d'une prédite faisait
encore attendre 20 ms dans `pump()` (l'hybride tombait à 30 i/s).

**Wayland natif** (03/10, `--wayland`, `pw_capture.cpp`) : portail
ScreenCast par GDBus (choix gardé, `restore_token` en cache) puis flux
PipeWire copié dans l'anneau aligné page (import en place). L'overlay couvre
`--rect` (défaut : tout l'écran X) — la position d'une fenêtre Wayland est
au compositeur. Banc : `tools/ci/wayland-capture-check.sh` (sway headless
deux sorties, xdpw `chooser_type=none`, la démo en plein écran sur la 1re,
l'overlay sur la 2de) : ×2,00, en place. Pièges vécus : le binaire sway a
`cap_sys_nice` (refusé en conteneur : en copier une version sans) ;
proposer `maxFramerate` (à 0/0 xdpw n'envoie qu'une image) ; la démo
masquée par l'overlay tombe à 2 i/s (fenêtre occultée).

**Limites** : pas sous gamescope (mode Jeu du Deck : les jeux y présentent
par la couche WSI de gamescope, la fenêtre X reste vide), lavapipe trop lent
pour les prédites (normal : elles arrivent après l'image suivante du jeu) ;
Wayland natif jamais essayé sur un vrai bureau (KDE, GNOME, Hyprland : le
dialogue du portail et l'overlay au-dessus d'un jeu plein écran).

### 4.6 Windows, mode 1 — la couche Vulkan

**Installation et configuration**
- `VkLayer_bdex_framegen.dll` (x64 et Win32). Installation :
  `register-layer.ps1 -Dll …` (registre des couches implicites ; `-Wow6432`
  pour les jeux 32 bits). `unregister-layer.ps1` pour retirer.
- Configuration : `%APPDATA%\bdex-framegen\bdex-framegen.conf`, mêmes clés
  que sous Linux. `log_file=%TEMP%\bdex-fg.log` est conseillé (les `%VAR%`
  sont développés), car la sortie d'erreur d'un jeu ne va nulle part.

**Jeux concernés** : Vulkan natifs et émulateurs **uniquement**. Un jeu
Direct3D natif ne passe pas par Vulkan : c'est le mode 2.

**Option de lancement Steam** : `cmd /c "set BDEX_FG=1 && %command%"` (la
syntaxe Linux n'y vaut pas).

**Limites**
- `VK_EXT_full_screen_exclusive` n'est pas transmis : préférer le mode
  fenêtré sans bordure.
- **Jamais lancée sur un Windows réel** : construite et testée
  (tests unitaires) par la CI MSVC, syntaxe vérifiée localement par MinGW.
- **Exécutée sous Wine depuis le 03/10** (`tools/ci/wine-layer-check.sh`,
  job `source-packages`) : DLL construite par MinGW
  (`tools/ci/mingw/x86_64-w64-mingw32.cmake`, runtime lié statiquement,
  printf de MinGW pour `%zu`), enregistrée comme couche implicite
  (`HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers`, manifeste à côté de la
  DLL), chargée par le chargeur officiel Windows 1.4.357 (`vulkan-1.dll` du
  runtime LunarG) dans un programme Win32 Vulkan
  (`tests/proton/vulkan_probe.cpp`), Wine + winevulkan + lavapipe : ×2,00.
  Corrigés au passage : priorité du fil de présentation sous Windows
  (MMCSS « Games », sinon `THREAD_PRIORITY_HIGHEST` — elle ne faisait
  rien) et minuterie à 1 ms pendant que le fil tourne (`precise_wake` :
  sinon les attentes cadencées durent jusqu'à 15,6 ms).
  Non vérifiable sous Wine : un jeu Direct3D rendu Vulkan par DXVK sous
  Windows passerait par la couche (DXVK charge `vulkan-1.dll` quand
  `winevulkan.dll` n'existe pas), mais sous Wine DXVK + le chargeur officiel
  plantent même sans la couche (03/10, appel à l'adresse 0 juste après
  `vkGetInstanceProcAddr`) : à essayer sur un vrai Windows.
  La DLL 32 bits se construit et se lie avec MinGW i686, mais le chargeur
  officiel x86 ne trouve pas `winevulkan` dans un préfixe Wine 32 bits
  (`vkCreateInstance` → `INCOMPATIBLE_DRIVER`, alors que le `vulkan-1` de
  Wine fonctionne) : non exécutée (03/10).

### 4.6 bis Injection ou capture, et le GPU de génération (03/10)

Demande utilisateur : « injection quand c'est possible (surtout sur Linux),
sinon capture (surtout sur Windows), pour maximiser le gain de latence ; avec
deux GPU, pouvoir générer sur l'un ou l'autre ». La couche est dans la
présentation du jeu (0 image de latence), la capture ajoute ~1 image.

- **Décision** (`plan()` du panneau, `chooseInjection` de `bdex_capture`,
  même règle) : `method = layer` → couche ; `capture` → capture ; `auto` :
  capture si le GPU choisi n'est pas celui du jeu (`device=` du statut),
  si anti-triche, si aucune swapchain armée (`ready`/`generating`/`paused`)
  dans `<pid>.status` ; couche sinon.
- **Linux** : le panneau lance/arrête `bdex-capture --pid N [--gpu NOM]`
  (sortie dans `~/.cache/bdex-framegen/capture.log`), « Capturer une autre
  fenêtre… » (clic sur la fenêtre). Méthode et GPU dans le panneau et le mode
  texte (`c`, `g`, `w`, `a`).
- **Windows** : le launcher a « Méthode » et « GPU de génération » en tête
  de la colonne de droite ; s'il trouve la couche enregistrée
  (`HKLM/HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers`) il écrit
  `enabled = 1` + `require_app = 1` (les jeux Vulkan sont armés). Injection =
  `runInjection` : `app.session` « pid cible Hz » réécrit chaque seconde
  jusqu'à la fermeture du jeu ou Ctrl+Alt+Q (Ctrl+Alt+G écrit la cible 0),
  icône de notification ; non vérifié sur Windows (MinGW seulement).
- **Piège corrigé** : sous `require_app = 1`, les applis de capture
  chargeaient leur config comme un jeu non ciblé (en pause, HUD coupé) ; elles
  posent `BDEX_FG=1` avant `Config::load` (Windows : `_putenv_s`, que
  `getenv` voit — pas `SetEnvironmentVariable`). Cas « armed » de
  `tests/capture_linux.sh`.
- Vérifié (gamescope headless, HOME isolé) : méthode injection → le jeu
  « 30 → 60 i/s (×2) » par la couche ; capture → couche ×1,00 et
  bdex-capture ×2,00.

- **`zero_bias = 0` sur GPU dédié** (même jour, accord de l'utilisateur) :
  le bonus gratuit du candidat « immobile » gagnait sur les contenus doux en
  mouvement rapide (fondu fantôme) ; le HUD statique reste immobile (coût
  exact nul à zéro). Corpus 960×540 avec les autres options auto : zoom +1,6
  / +1,7 / +2,2 dB (interp / hybride / extrap), panoramique +0,5 / +1,5 /
  +1,6 ; pertes : occlusions −0,1 / −0,3, HUD −0,13 en hybride.
- **Correctif CI (`swapchain_stress`, plein écran)** : après une
  présentation `OUT_OF_DATE`, le job suivant acquérait encore sur la vraie
  swapchain périmée ; la validation compte les images d'une présentation
  échouée comme tenues → VUID-...-07783. `realOutOfDate_` : plus
  d'acquisition sur elle, le jeu recrée sa swapchain.

### 4.6 ter Windows : injection par défaut (03/10)

Demande utilisateur : « injection par défaut sur tous les jeux, sauf les jeux
avec anti-triche (capture), sauf Rocket League (l'anti-triche ne détecte pas
encore, un ami teste) ; message de prévention sur un jeu avec anti-triche,
mais le lancement continue ».

- **Décision** (`planRun`, `capture/src/main.cpp`) : capture choisie →
  capture ; autre GPU que celui du jeu → capture ; anti-triche (sauf
  exception) → capture ; swapchain armée → injection (`runInjection`,
  `app.session`) ; sinon le jeu est préparé pour son prochain lancement et
  capturé en attendant : Vulkan → couche enregistrée ; D3D9/10/11 → DXVK à
  côté de l'exe ; D3D12 → VKD3D-Proton + dxgi de DXVK ; OpenGL → capture.
  Méthode « injection » forcée : retour au lanceur au lieu de capturer.
- **`capture/src/injection.{h,cpp}`** : `inspectGame` (chemin de l'exe,
  modules → API, en-tête PE → 32/64 bits, anti-triche : jeu compétitif,
  dossiers `EasyAntiCheat*`/`BattlEye` jusqu'à 3 parents, modules
  `easyanticheat*`/`beclient*`, processus), `installTranslation` (jamais
  par-dessus un fichier existant ; marqueur `bdex-framegen-injection.txt`,
  liste `%LOCALAPPDATA%\bdex-framegen\injections.txt`), `removeTranslation`,
  `registerLayerForUser` (comme `register-layer.ps1` : DLL + profils copiés
  dans `%LOCALAPPDATA%\bdex-framegen[32]`, manifeste, HKCU ; recopie si le
  paquet a changé), `antiCheatWarning`. `armLayer` n'écrase plus un
  `enabled`/`require_app` déjà présent.
- **Rocket League** : `platformAntiCheatException` (Windows seulement) — la
  couche reste active comme avec `ignore_anticheat`, `anticheat=` dans le
  statut. Sous DXVK, l'anti-triche (EAC) voit un `d3d11.dll` dans le
  dossier du jeu : risque réel, dit dans l'avertissement. À retirer de la
  liste dès que ça change.
- **Prévention** : `MessageBox` avant la génération (lanceur ; console en
  ligne de commande), dialogue/bandeau dans le panneau Linux
  (`game_anticheat` + `anticheat_warning`, mode texte compris), ligne
  « ⚠ Anti-triche détecté » dans `statusDescribe` et l'icône.
- **Paquet** : `tools/fetch_translation_layers.py` (mêmes versions et SHA256
  que `proton-check.sh`), artefact CI `bdex-framegen-windows-complete`
  (x64 + couche 32 bits dans `x86\`). VKD3D-Proton est LGPL-2.1 : DLL
  séparées non modifiées, remplaçables, source indiquée (notices).
- **Non vérifié sur Windows** (compilé MinGW x64/i686 seulement) : à
  valider sur une vraie machine — injection DXVK d'un jeu D3D11, D3D12 via
  VKD3D-Proton (moins éprouvé sous Windows que sous Proton), 32 bits.

### 4.7 Windows, mode 2 — `bdex_capture.exe`

**Chaîne de traitement**
1. Capture WGC de la fenêtre du jeu.
2. Anneau de 3 textures D3D11 partagées (handle NT + keyed mutex).
3. Import dans Vulkan (`external_memory_win32` + `win32_keyed_mutex`).
4. `FrameGen`, sans changement.
5. Swapchain Win32 dans une surcouche toujours au-dessus, transparente aux
   clics, qui suit la fenêtre du jeu.

**Interface**
- Lanceur natif : choix de la fenêtre, options, colonne d'options avancées,
  cadre « En cours ».
- Icône de notification.
- Raccourcis : **Ctrl+Alt+G** (génération on/off), **Ctrl+Alt+Q** (quitter).
- Profils par jeu. Ligne de commande : `--list`, `--title`, `--pid`,
  `--multiplier`, `--mode`, `--adaptive`, `--fit`, `--hud`…

**Fonctionnement**
- Interpolation par défaut ; hybride ; adaptatif sur la fréquence du
  moniteur de la surcouche.
- HDR : scRGB FP16 de bout en bout si l'écran du jeu est en HDR.
- Minuterie à 1 ms et attentes « dormir puis tourner » : à 15,6 ms, une
  attente de 16,7 ms finissait à 31 ms.

**Validé sur matériel le 15/09** (portable Intel iGPU + RTX 3050), ×2 stable
en extrapolation :
- PRAGMATA (D3D12) ;
- Dark Souls III et Skyrim SE (D3D11) ;
- Fallout: New Vegas (D3D9).

Bugs trouvés alors et corrigés : scintillement, double curseur,
`log_file`, arrêt WGC.

**Limites** : environ +1 image de latence ; fenêtré ou sans bordure
seulement, pas le plein écran exclusif (comme Lossless Scaling).

### 4.8 Ce qui n'est pas pris en charge

- **Systèmes et paquets** : macOS. (Snap : Steam seulement, non vérifié.)
- **Plein écran exclusif** sous Windows : ni en mode 1 (non transmis) ni en
  mode 2 (surcouche impossible).
- **Jeux protégés** par un anti-triche ou compétitifs : la couche s'y retire.
- **Swapchains** multicouches (VR) ou protégées : passage direct.
- **OpenGL** natif sans Zink.

---

## 5. Activation et configuration

**Fichier** : `~/.config/bdex-framegen.conf` (Windows : `%APPDATA%\…`), INI.
`[*]` vaut pour tous les jeux, `[nom]` pour l'exécutable `nom` (sans `.exe`).

**Priorité**
- Couche : défauts < profils fournis (`profiles.conf`, qui n'activent jamais
  rien) < fichier < variables `BDEX_FG_*`.
- Capture : s'y ajoutent ensuite le preset en ligne de commande, les options
  en ligne de commande, puis le lanceur.

| Façon d'activer | Ce qu'il se passe | Où |
|---|---|---|
| Panneau, « Démarrer » (`enabled = 1`, `require_app = 1`) | chaque jeu est armé (images passées telles quelles) ; seul le jeu ciblé génère, réglages en direct ; panneau fermé = arrêt | bureau Linux (comme le lanceur Windows) |
| « Mode Jeu — sans le panneau » (`require_app = 0` dans `[*]` ou `[jeu]`) | génère dès le lancement, panneau fermé ; programmes de bureau exclus | SteamOS en mode Jeu, ou au choix |
| `BDEX_FG=1` (option de lancement Steam, `bdex-framegen -- jeu`) | active ce processus, en contournant `require_app` | partout ; Windows : `cmd /c "set BDEX_FG=1 && %command%"` |
| `[jeu] enabled = 1` à la main | active ce jeu | fichier |

**Réglages**
- Relus à chaque image : mode, qualité du flux, pause, HUD…
- Reconstruisent l'étage de sortie (le panneau change le multiplicateur) :
  une image sans génération.
- Figés à la création : `render_scale`, mode de présentation. L'upscaling ne
  s'applique donc qu'aux jeux lancés panneau ouvert.

**Principales clés**
- `multiplier`, `adaptive`, `target_fps` ;
- `mode` (`interpolate` / `extrapolate` / `hybrid`) ; `preset` ;
- `fps_limit` ; `render_scale` / `upscale_filter` / `sharpness` ;
- `overlay` (HUD 0 à 6 : fps, fps de sortie, 1 % et 0,1 % bas, latence, et
  depuis le 02/10 le **graphe de cadencement** : 64 présentations, temps de
  trame du jeu coloré par rapport à l'intervalle visé, gris sans génération,
  marque rouge si une image a été écrasée — en push constants, 104 octets) ;
- `paused` ; `present_mode` ; `low_latency` ; `latency_limiter` ;
- `max_frame_latency` ; `scene_cut_*`.

La liste complète est dans le tableau du README ; le test `option_keys`
l'impose.

**Langue des interfaces** (03/10) : panneau GTK, mode texte et lanceur
Windows en anglais quand la langue du système n'est pas le français
(`BDEX_LANG=fr|en` force). Une option ajoutée demande sa ligne dans
`layer/options_ui.en.conf` (le build échoue sinon) ; un texte du panneau
passe par `tr()` et la table `_EN`. Le rapport `--report` reste en français.

---

## 6. Outils, tests et CI

**Outils d'évaluation**
- `bdex_offline` : le moteur sans fenêtre, sur une séquence PPM. Il
  reproduit exactement les images de la couche.
- `tools/eval_sequence.py` : PSNR global, mouvement, arêtes, SSIM,
  scintillement ; A/B avec `--variant`.
- `tools/run_eval.sh` : évaluation en ligne ; `tools/make_corpus.py`,
  `tools/corpus_pack.py` : corpus ; `tools/tune.py` : réglage automatique.

**Tests ctest (20)**

| Groupe | Tests |
|---|---|
| Sans fenêtre | `fuzz_config`, `fuzz_status`, `fuzz_options_ui`, `fuzz_online_tune` (03/10 : rejouent le corpus de `tests/fuzz/corpus/`, graines et entrées qui ont échoué ; `tools/ci/fuzz.sh` = libFuzzer + ASan + UBSan, job CI `fuzz` ; propriétés : options dans leurs bornes, profil qui n'active jamais la couche, cache relu à l'identique ; a trouvé `debug = confidence` lu hors du tableau de `describe()`), `unit`, `matcher` (vrai shader contre modèle C++ du départage), `eval_quality`, `offline`, `corpus`, `formats`, `min_limits`, `determinism` (chaque option deux fois), `option_keys`, `panel_logic` |
| Avec affichage | `integration` (×2 à travers la couche), `validation` (couches Khronos + synchronisation, toutes les options), `acquire_timeout`, `multithread_config`, `swapchain_stress` (03/10 : `bdex_demo --stress` — redimensionnements, recréations avec autre mode et nombre d'images, surfaces et fenêtres refaites, plein écran, minimisation — sous validation, puis ×N retrouvé ; a trouvé `FIFO_LATEST_READY` transmis tel quel) |
| Hors écran, sautés s'ils ne peuvent pas tourner | `gamescope_limit` (limite Steam rejouée dans un gamescope headless) |

**Règle** : jamais de fenêtre sur l'écran de l'utilisateur. Les tests
d'affichage tournent dans `gamescope --backend headless` ou dans Docker
(Xvfb + lavapipe).

**CI GitHub**
- `linux.yml`, à chaque push, sous 15 min (demande du 08/10) :
  - build + tests + seuil PSNR (28,0 / 23,0 dB) ;
  - sanitizers (ASan, UBSan, TSan) ;
  - fuzz ; syntaxe Windows (MinGW x64/i686) ; `secrets` (gitleaks).
- `compat.yml`, chaque nuit (si `main` a bougé dans les 24 h), sur les tags
  `v*` et à la main (Actions → compat → Run workflow) :
  - `distros` : 6 distributions ; `source-packages` (dont Proton/Wine) ;
    `panel` ; `nix` ;
  - `package` : archives + `archive-check`, jointes à la release.
- Chaque job a un `timeout-minutes` ; apt sur le runner passe par
  `tools/ci/runner-apt.sh` (le miroir Azure a bloqué une CI 4 h le 08/10).
- `windows.yml` : MSVC x64 et Win32 (SDK Vulkan en cache), tests, zips. En cas d'échec, les logs
  sont poussés sur les branches `ci-log-x64` / `ci-log-Win32`.
- **Le dépôt est privé** : l'assistant ne voit pas la CI.

**Rejouer localement, avant de pousser**
- `tools/ci/run.sh build` (et `asan`, `tsan`, `mingw`) : la CI Linux dans
  Docker.
- `tools/ci/distro-check.sh`, `archive-check.sh`, `panel-check.sh`,
  `package-release.sh`.

---

## 7. Historique des améliorations

**Versions publiées**

- **0.1.0 (12/09)** : la couche implicite, le flux optique GPU (pyramide +
  block matching), la synthèse bidirectionnelle, la présentation depuis un
  thread dédié.
- **0.2.0 (13/09)** :
  - réglage automatique selon le GPU, upscaling spatial + CAS ;
  - extrapolation et profils (dont `latency`), mailbox par défaut ;
  - premier panneau GTK 4.
- **0.3.0 (14/09)** : HUD ; **Proton corrigé** (pas de SUBOPTIMAL,
  extensions de présentation cachées).
- **15/09** (publié avec la 0.4.0) : portage Windows. Couche (mode 1)
  compilée en CI ; application de capture (mode 2) **validée sur matériel**,
  avec lanceur et raccourcis.
- **0.4.0 (25/09)**, grand lot :
  - HDR ; état GPU (`frame_timing`, `dedup`) ; coupures de scène locales ;
  - options d'extrapolation (accélération, splat, reveal_fill) ;
    chrominance ;
  - `gpu_budget`, `online_tune`, `upscale_temporal`, `flow_temporal` ;
  - retour d'affichage `present_wait` ; upscaling en Wayland natif ;
  - architecture modulaire (`FgPass`, registre d'options, pools de
    descripteurs à la demande) ;
  - variantes allégées des shaders (coût GPU ÷2) ;
  - `zero_bias_contrast` à 0,02 par défaut, preset `enhanced` ;
  - première archive Linux ; icône de notification sous Windows.
- **0.5.0 à 0.5.5 (26/09)** :
  - panneau Linux calqué sur le lanceur Windows (`require_app`,
    « Démarrer ») ;
  - dossier de session dans `~/.cache` (runtime Steam) ;
  - jeux compétitifs exclus ;
  - panneau sans libadwaita ;
  - multiplicateur « Auto » ;
  - lecture rapide de `--report` ; statistiques de coupure de scène ;
  - nombres lus quelle que soit la locale.

**Travail non publié en version (29–30/09, poussé sur `main`)**

- **Parité Lossless Scaling** :
  - cadencement depuis la fin GPU et deux files de présentation ;
  - mode hybride, multiplicateur adaptatif fractionnaire, jusqu'à ×8 ;
  - `max_frame_latency` et `latency_limiter` ;
  - prédites abandonnées au bon moment ;
  - défauts qualité sur GPU dédié (coupures par histogramme et par tuile,
    `zero_bias` plafonné, vecteurs par pixel, splat + reveal_fill).
- **Compatibilité matériel et distributions** :
  - archives glibc 2.29 (x86_64 64/32 bits) et nouvelle archive arm64 ;
  - lanceurs Flatpak ;
  - petits GPU (limites garanties) ;
  - compilation GCC 10+ / en-têtes 1.3.204+ / glslangValidator ;
  - manifeste relu sans avertissement ;
  - CI de distributions et vérification des archives là où elles tournent.
- **SteamOS** :
  - mode Jeu sans panneau ;
  - limite d'images/s de Steam gérée ; fréquence lue dans gamescope ;
  - rattrapage de la présentation ;
  - appels relayés à la swapchain courante (HDR…) ;
  - panneau vérifié sur les versions de SteamOS.
- **Distributions** (30/09, suite) :
  - paquets `.deb` / `.rpm` dans les releases, PKGBUILD AUR `-bin` / `-git`,
    flake NixOS ;
  - panneau en mode texte sans GTK 4 ;
  - Steam en Snap ; `--sandboxes` ; `--mangohud`.

**03/10 (local, pas encore poussé)**

- **Tests** : chemin Proton complet en CI (D3D11/DXVK, D3D12/VKD3D-Proton
  sous Wine, et pilote Wayland de Wine 11), tempêtes de swapchain sous
  validation, fuzzing des analyseurs (deux bogues trouvés et corrigés :
  `FIFO_LATEST_READY`, `debug = confidence`), couche 32 bits exécutée.
- **Produit** : panneau, mode texte et lanceur Windows en anglais hors
  locale française ; plugin Decky (Steam Deck) ; `bdex-capture` Linux
  (capture X11/Xwayland, deux GPU, jeux anti-triche).
- **Coût GPU** : génération 1440p ×2 1,63 → 0,92 ms, 4K ×2 2,96 → 1,84 ms
  (défauts dGPU) — carte de splat en tampon, synthèse compilée par jeu
  d'options, `interp_splat` à demi-résolution ; options qualité auto
  jusqu'à 4K ×3 ; `synth_bicubic` −16 % ; mémoire GPU affichée.

**04–05/10 (ménage, local)**

- **Cinq options retirées**, mesurées négatives ou neutres : `alt_vectors`,
  `affine_flow`, `flow_upsample`, `confidence_fallback` et `extrapolate_hold`
  (l'ancien comportement aux images doubles). Le code est parti des shaders,
  du moteur, du panneau et du lanceur. Toutes les autres sorties sont
  identiques au bit près : 276 cas (réglages × séquences × modes) hachés
  avant et après sur la RTX 3060 Ti.
- **Où est l'erreur restante** : sur des séquences type jeu en 1280×720
  (monde texturé qui défile, objets indépendants, HUD), la caméra rapide est
  bien suivie (34 dB à 80 px par paire). **60 à 80 % de l'erreur est au
  contour des objets qui bougent par eux-mêmes**, sur 2 à 4 % des pixels,
  même à caméra fixe et à mouvement entier exact. Rien n'y change : sous-pixel
  (LK ou parabolique), bicubique, `learned_flow`, `levels=5`,
  `flow_iterations=3`, `occlusion_strength`, `interp_splat=0`.
- **Outils** : `tools/pending_decisions.py` note les options en attente sur
  des jeux enregistrés et rend un verdict par option ; `docs/corpus-jeux.md`
  dit quoi enregistrer pour chaque décision. `ROADMAP.md` réécrit (l'ancien
  datait du 23/09, `todo.md` du 24/09, supprimé). `swapchain.cpp` coupé en
  deux : côté jeu, et `swapchain_worker.cpp` côté worker (sans changement de
  code).

### 07/10 — premiers enregistrements de vrais jeux

- **L'enregistrement faisait tomber le jeu à 3 i/s** (Fallout: New Vegas,
  Vega 8 : `game_fps=2,76` dans `DONE`). `present()` attendait la fence de
  l'image, puis convertissait et écrivait le PPM sur le thread du jeu, depuis
  une mémoire hôte non cachée. Un jeu à pas de temps variable avance alors de
  ~360 ms entre deux images : la séquence n'a plus le mouvement du jeu.
  Désormais `FrameRecorder` (`layer/src/recorder.*`) : copie GPU de l'image
  du jeu (taille de rendu, sans HUD ni upscale) dans un anneau de 6 tampons
  hôte en cache (`createBuffer(…, hostCached)`), et un thread écrit les
  fichiers. Conteneur CI (lavapipe), démo 1280×720 ×2 : 17,3 → 13,9 i/s
  pendant l'enregistrement avant, 17,7 → 17,0 après. **Un enregistrement
  fait avec une couche antérieure est à refaire.**
- Le panneau : vrai bouton « ● Enregistrer » (l'icône seule passait
  inaperçue), et `explorer` / `xalia` (Wine) ne sont plus pris pour des jeux.
  `--record` enregistre dès le lancement : à éviter dans un jeu à menus.

### 13/10 — revue sécurité et licences (`SECURITY.md`)

Demande : une liste de contrôle d'appli web (clés d'API, secrets git,
limitation des connexions, en-têtes, cookies, HTTPS, mots de passe, RLS,
dépendances…), appliquée seulement là où un logiciel local a un équivalent.
Écartés, sans objet : pas de serveur, de compte, de base, de réseau. Fait :
- **Secrets** : gitleaks sur tout l'historique, 305 commits, rien ; job
  `secrets` dans `linux.yml` à chaque push (quelques secondes ; faux
  positif → `.gitleaksignore`).
- **Panneau GTK** : libadwaita lit titres, sous-titres et toasts comme du
  balisage Pango — un nom de jeu avec `&` ou `<` faisait disparaître la
  ligne. `_PlainAdw` force le texte brut (fabriques, pas sous-classes :
  `AdwSwitchRow` est final ; texte posé *après* `use_markup`, sinon déjà
  analysé ; `AdwEntryRow` ignore `use_markup` pour son titre flottant :
  échappé). L'ancien `_m()` affichait `&lt;` dans le panneau GTK simple.
- **`/tmp`** : sans `HOME`, `platformStatusDir` (et le panneau) n'utilisent
  `/tmp/bdex-framegen-<uid>` que s'il est à nous en 0700 (sinon "").
- **Windows** : « Retirer les injections » ne supprime que les noms de DLL
  de traduction connus (le marqueur est dans le dossier du jeu). Contenu
  non comparé exprès : après une mise à jour de DXVK, les DLL de
  l'ancienne version ne seraient plus reconnues.
- **Rapports** (`--report`, lanceur) : chemin personnel masqué (`~`,
  `%USERPROFILE%`), propriétaires numériques (`ls -ln`).
- **LGPL (VKD3D-Proton)** : les archives binaires amont n'ont aucun texte
  de licence ; `fetch_translation_layers.py` ajoute ceux de DXVK et
  VKD3D-Proton (épinglés SHA256), et `--source` produit la source complète
  (sous-modules, commit vérifié, ~30 Mo) jointe à chaque release Windows
  (décision utilisateur : §4 au sens strict).
- **Releases et CI** : `SHA256SUMS-{linux,windows}.txt` ; signature
  Authenticode prête (étape « Sign », inactive sans les secrets
  `WINDOWS_SIGN_PFX_BASE64` / `_PASSWORD`, décision utilisateur) ; jeton
  en lecture seule par défaut ; actions tierces épinglées par commit ;
  Dependabot mensuel groupé (décision utilisateur) ; `pnpm audit --prod`
  au build du plugin Decky (une alerte « high » sur `braces`, outil de
  build seulement, sans correctif).
- Aucun accès réseau à l'exécution (vérifié) : rien à chiffrer ni à
  « forcer en HTTPS » ; les téléchargements de la CI vérifient leur SHA256
  ou passent par apt (signé).

---

## 8. Chiffres à retenir

| Sujet | Mesure |
|---|---|
| Qualité, démo (historique, 15/09) | 31,0 dB mouvement normal, 23,7 dB rapide |
| Qualité, CI (lavapipe) | 29,50 / 23,65 dB (seuils 28,0 / 23,0), à ~0,04 dB du GPU |
| Latence, interpolation ×2 à 60 i/s | image réelle retenue ~8,4 ms (~16,7 ms à 30 i/s) |
| Latence, extrapolation ×2 | ~0,1 ms |
| Entrée → écran, démo 40 i/s, 144 Hz (02/10, `tools/latency_bench.sh`, médianes premier photon / image réelle) | ×1 6,8 / 6,8 ms ; interpolation ×2 10,0 / 19,9 ; `interp_hold=auto` 9,2 / 16,2 ; extrapolation ×2 6,7 / 6,7 |
| `interp_hold = 0,5`, gamescope 144 Hz (02/10) | 40 i/s ×2 : image→écran 43,4 → 37,6 ms (79,3/80 affichées) ; 40 i/s ×3 : 47,7 → 45,0 ; 60 i/s ×2 : 30,3 → 29,8 |
| Coût GPU, variantes allégées (1440p, quality) | 2,7 → 1,35 ms |
| Coût GPU de la génération, défauts GPU dédié, ×2 (03/10, RTX 3060 Ti, `bdex_offline` + `BDEX_FG_PROFILE=1`) | 1440p 1,63 → 0,92 ms ; 4K 2,96 → 1,84 ms (analyse + flux + synthèse). Synthèse 1440p 1,20 → 0,49 ms : carte de splat en tampon (−0,25), variante compilée par jeu d'options (−0,33), `interp_splat` à demi-résolution (−0,18). Préréglage explicite `balanced` (sans les options auto) : synthèse 0,21 ms |
| Jeu limité par le GPU, 4K lourd, avant/après le cadencement GPU | 66 → 132 images affichées/s |
| `latency_limiter`, 4K lourd | image → écran 26,8 → 18,3 ms, 97 → 101 i/s |
| Hybride ×3, jeu limité par le GPU | 116 → 283 images affichées/s, jeu 58 → 94 i/s |
| Adaptatif | 50 i/s → 144 affichées sur 144 Hz |
| 30 i/s ×4 sur 144 Hz, `refresh_fill` + `pace_plan` (14/10, trois essais) | 120 → 142–143 affichées/s, répétitions 30 → 5/s, image réelle 33 → 40 ms |
| `interp_accel` (14/10) | jeux enregistrés +0,03 à +0,17 dB, scintillement jusqu'à −0,5 ; +0,1 ms en 1440p ×4 |
| `border_predict` (15/10) | virages ×4 +0,15 à +0,19 dB (720p et 1440p) ; PRAGMATA scintillement −0,25 / −0,32 ; gratuit |
| `false_detail` (14/10) | enregistrements 1440p +0,14 / +0,07 dB, scintillement −0,39 / −0,18 ; scènes ×4 +0,03 à +0,21 ; aucune perte ; ~1 ms par image générée en 1440p (+0,8 ms par image du jeu) |
| Limite Steam 30 i/s (démo 30 i/s ×2), avant | jeu 15 i/s, 233 ms présentation → écran |
| Limite Steam 30 i/s, après | jeu 30 i/s, 19 à 66 ms ; ×2 retrouvé après la levée |
| glibc demandée par l'archive | 2.36 → 2.29 |

Mesures refusées, à ne pas refaire :
- `grad_weight` : −2,1 dB en mouvement normal.
- census : −1,5 dB en mouvement normal.
- Mélange pondéré de `flow_temporal` : −0,5 dB.
- Sous-pixel parabolique : −0,1 dB.
- Échelle PQ en HDR : −3 à −10 dB.
- Attendre l'affichage de tout un job sous la limite Steam : le jeu tombe de
  40 à 31 i/s.
- Autour du HUD (14/10) : `hud_match` −0,2 dB, `hud_global` jusqu'à −0,9.
- Estimation du délai GPU de `pace_plan` symétrique : 52 % d'images hors plan.

Le détail de toutes les mesures est dans AGENTS.md §10 et le CHANGELOG.

---

## 9. Problèmes connus et pistes

**Essais écartés (03/10, coût GPU)** : splat d'extrapolation à demi-résolution
(un pixel par bloc 2×2) — la carte y est la source même des pixels, pas un
candidat : rotation −13 dB, popup −1,6, occlusion −1,1. (Celui de
`interp_splat`, un candidat, est à demi-résolution sans perte.) Constat à
creuser : à 960×540, `extrapolate_splat` n'est pas net (pan −0,26 dB,
HUD −0,19, rotation +0,14) — les +0,9 à +1,2 dB d'origine étaient mesurés
avec `reveal_fill`, à ×3. Tranché le 03/10 sur des enregistrements de la
démo (960×540) : +0,41 dB en extrapolation ×2 (rapide +0,05, hybride +0,05)
→ gardé par défaut ; `reveal_fill`, lui, est net partout (pan −1,8 dB sans).

**Création du device, nouvel essai** (corrigé le 02/10) : chaque couche
du dessous avance la même chaîne de liens en appelant plus bas ; un second
`vkCreateDevice` (repli sans retour d'affichage, sans priorité) repartait
de là où le premier l'avait laissée, sautait la couche WSI de gamescope et
faisait planter le jeu. Chaque essai repart du lien qui suit la couche
(`createDevice` dans `layer.cpp`).

**Jamais validé sur du vrai matériel**
- La couche Windows (mode 1).
- Un Steam Deck (performance sur RDNA2 8 CU, HDR OLED, 90 Hz).
- ARM64.
- Un petit GPU (V3D).
- Steam en Flatpak ; Steam en Snap.
- Les paquets `.deb` / `.rpm` / AUR / Nix sur une vraie machine (vérifiés en
  conteneur, lavapipe).

**Cas ouvert : KDE + NVIDIA (bureau de l'ami)**
- Pas de retour d'affichage.
- Valheim tournait déjà vers 130 i/s sur 144 Hz, donc ×2 n'apporte rien ou
  étrangle le jeu. Soupçon : repli de coupure de scène.
- Rapport 0.5.4 en attente.
- Principe : partir de « ça ne marche pas chez lui » et chercher la cause
  dans les données.

**Simulateur de cadencement** (02/10) : les décisions du cadencement
(`presentTargetMs`, borne d'`interp_hold`, retard d'un job, `StartLimiter`,
`AdaptivePacer`, `FastGate`) sont dans `layer/src/pacing.h`, sans Vulkan,
appelées telles quelles par la swapchain ; `tests/test_pacing.cpp` les rejoue
contre un jeu, un GPU et un écran mailbox simulés (chaque propriété vérifiée
par une mutation qui la casse). Modèle à retenir : le limiteur est en boucle
fermée (retenir le jeu réduit son attente) ; les images écrasées d'un
espacement trop serré n'arrivent que quand la couche ne connaît pas la
grille (seulement la fréquence), la règle d'atterrissage les évite sinon.

**Extrapolation, 03/10** : correction apprise pour t > 1 (poids `LCX_`, repli
sur l'image courante) : démo +0,12..+0,26, popup +0,32, mais scroll −3,07,
subpixel −0,38 → reste opt-in. Essais refusés : amortir la prédiction (démo
−0,12..−0,49, pan −9,2 : les erreurs ne sont pas des dépassements) ;
`extrapolate_accel` par défaut (swing +2,6 seulement, pan −0,78, scroll −1,4).

**`rot_weight` auto sur GPU dédié** (03/10) : variante « rotation seule » du
matcher (constante `kRot`, `Pass::rot`) — la variante complète réserve la
mémoire partagée de toutes les options (~21 Ko, occupation ÷2 : flux ×2,4) ;
rotation seule ~13,5 Ko : flux ×1,5. La variante allégée fixe désormais les
deux constantes à faux. Par-dessus pixel_vectors : occlude +1,17, rotation
8°/image +1,09, zoom 5 % +1,12, scroll +0,40, subpixel +0,23, démo
+0,07..+0,12, aucune perte. Séquence `zoom` ajoutée au corpus.

**`interp_splat`** (03/10, **auto sur GPU dédié**, comme pixel_vectors) :
les deux images projetées vers t (passe `extrapolate_splat`, modes 1 et 2,
carte de largeur 2W : l'extrapolation garde la moitié gauche, au bit près) ;
les sources arrivées en q sont des trajectoires candidates (erreur actuelle
> 0,02 et 10 % mieux). Seul : occlude +0,55, scroll +0,62, rotate +0,26, démo
+0,05..+0,08, aucune perte ; avec pixel_vectors : occlude +0,22, scroll
+0,41, démo +0,01..+0,03, **hud −0,12** (interaction avec pixel_vectors, ni
la marge ni le seuil n'y changent rien). +0,09 ms à 960×540.

**`alt_vectors`** (03/10, écarté, **retiré du code le 04/10**) : second candidat du matcher (≥ 2 px
du gagnant) essayé par pixel dans la synthèse. Sans marge : scroll −3,0,
subpixel −2,7 (il gagne sur du bruit) ; avec marge (×0,75 − 0,004) et seulement
si la trajectoire gardée est mauvaise (erreur > 0,04) : seul hud +0,57, pan
−0,62, scroll −1,74 ; par-dessus pixel_vectors rien à gagner (pan −0,57).
Tampons de stockage (pas d'image : 4 images de stockage garanties, test
`min_limits`). pixel_vectors trouve déjà les mêmes cas.

**`flow_upsample`** (03/10, écarté, **retiré du code le 04/10**) : sur-échantillonnage
bilatéral joint du flux aux frontières de mouvement. Mesuré contradictoire :
sans pixel_vectors rotate +0,51 / scroll −0,88 / démo −0,06, avec
pixel_vectors l'inverse (rotate −1,41, scroll +1,66). pixel_vectors fait déjà
le choix par pixel, avec contrôle photométrique : un flux plus lisse affaiblit
sa détection de frontière. Ne pas reprendre sans combiner les deux (le vecteur
sur-échantillonné comme candidat supplémentaire de pixel_vectors).

**`luma_match`** (03/10, **défaut 0,01**) : passe `luma_match.comp` après
l'analyse — pyramide précédente ramenée à la moyenne / l'écart-type de la
courante si toute l'image a changé de luminosité (≥ 13 tuiles 4×4 sur 16 dans
le même sens, d'au moins la moitié du décalage). Sans ce test, popup −1,77 dB
(un menu déplace la moyenne). pulse 27,0 → 35,2, fade +6,2 ; tout le reste
identique. La synthèse n'est pas touchée (elle lit les images elles-mêmes).

**Corpus difficile** (03/10) : `scroll`, `particles`, `smoke`, `rotate`,
`flash`, `pulse` ajoutés à `make_corpus.py`. Scores de départ (PSNR / fondu) :
scroll 32,1 / 22,7, particles 17,7 / 17,1, smoke 31,9 / 31,9, rotate 39,7 /
29,3, flash 39,3 / 23,9, **pulse 27,0 / 29,2 (pire que le fondu)**. Références
du test `corpus` recalculées sous lavapipe (conteneur CI) : les anciennes
dataient d'avant les gains de septembre. Toujours les calculer sous lavapipe
(un GPU dédié active d'autres options en auto).

**Flux appris** (03/10, `learned_flow`, désactivé) : MLP 23 → 16 → 2 qui
corrige chaque vecteur du flux le plus fin (≤ 1,5 px de niveau) d'après les
8 voisins relatifs, l'erreur de matching et sa pente à ±1 px, la partie
fractionnaire. Entraîné en numpy (`tools/train_flow.py`) sur des
mouvements de synthèse au flux vrai connu (panoramiques 1 à 27 px/image,
sous-pixel, balancement, zoom ; autres graines que le corpus) via
`bdex_offline --dump-flow` (flux + luminances du niveau 0) : EPE tenue à
part 0,52 → 0,21. Corpus : zoom +1,0 à +2,6, sous-pixel +0,5 à +2,2, fondu
+1,4, démo ±0,03, rotation −0,13 (interp) ; +0,3 ms/image générée en 1440p
(la synthèse coûte plus aussi : vecteurs fractionnaires → plus de tests de
frontière de pixel_vectors). Pas de PyTorch sur la machine. À réentraîner sur
des enregistrements de vrais jeux (« Enregistrer ») avant tout défaut.
`affine_flow` (même jour, **retiré du code le 04/10**) : candidat affine 3×3 dans
pixel_vectors, mesuré neutre (les vecteurs entiers laissent un résidu ≥ 0,5
px : il passe rarement le test, et rot_weight + pixel_vectors suivent déjà).

**Correction apprise** (02/10, `learned_correction`, expérimental) : réseau
63 → 8 → 1 entraîné en numpy (`tools/train_correction.py`, données =
sorties de `bdex_offline`), tête « mix » bornée (déplace le pixel vers le
fondu, jamais au-delà) ; passe `learned_correction.comp` après la synthèse,
avant le HUD (le HUD est désormais ajouté en dernier dans le registre).
Mesuré sur des séquences jamais vues : démo +0,49 dB, autres démos +0,10 /
+0,15, popup −0,59, subpixel −1,73 (le moteur retrouve les chiffres numpy
au centième près). Essais écartés : résidu libre (subpixel −11 dB), 16
unités (identique), écarts aux entrées en plus (subpixel −11). Le modèle ne
généralise pas au-delà de ses données : il faut des enregistrements de vrais
jeux (`--record`). +0,12 ms à 960×540. Désactivé par défaut.

**Confiance du flux** (02/10, `flow_confidence`, passe `flow_confidence.comp`
à la résolution du flux) : aller-retour des vecteurs sur contenu texturé
(zones unies exclues : sinon tout le damier de la démo était « incertain »)
et accord de luma ; part incertaine dans les stats, le statut, le panneau,
`--report` (démo 0,7 % à ¼ vitesse, 1,9 % normale, 6,9 % à ×3) ;
`debug=confidence` en rouge. Repli `confidence_fallback` (**retiré du code le 04/10** ; binding 9 de la
synthèse, variante complète seulement) **mesuré négatif** : 0,5 → démo +0,20,
rapide +0,02, corpus −0,2 à −1,1 dB, scintillement en hausse partout ;
laissé à 0. Ne pas y revenir sans nouvelle idée de repli (le fondu est plus
flou qu'un warp approximatif).

**Qualité** : le point faible mesuré le 04/10 est le contour des objets qui
bougent par eux-mêmes (60 à 80 % de l'erreur, cf. §7), plus que la caméra
rapide ; puis rotations et zoom (un bloc ne porte qu'une translation),
particules, pulsations de luminosité. Options en attente de vrais jeux :
`tools/pending_decisions.py` (guide : `docs/corpus-jeux.md`).

**Contours : c'est l'occultation** (05/10, `tools/edge_bench/` :
`make_sequence.py` still|pan|fast|lowcon, 1280×720, vérité exacte des
couches et vitesses ; `score.py --flow`). La bande de ±6 px autour des objets
(4 à 5 % des pixels) porte 62 à 91 % de l'erreur. Dedans, **les pixels
visibles dans une seule des deux images portent 51 à 78 % de toute
l'erreur** (still 78, pan 60, fast 51, lowcon 57), à 19–25 dB. Le vecteur
vrai absent des 4×4 texels voisins : 3 à 8 % seulement (lowcon 16).
- **Pas la résolution du flux** : `flow_scale=1` (un vecteur pour 4×4 px
  au lieu de 8×8) perd 1,4 dB dans la bande (still, pan), −0,6 à −1,3 dB
  sur l'image. Le flux par blocs est faux sur toute la bande (texel le plus
  proche juste à 39 % même pour les pixels visibles), le bon vecteur est
  dans le voisinage.
- **Marge** : choisir, avec la vérité, le meilleur échantillon pris d'un
  seul côté parmi les vecteurs des 16 texels autour de q donne 39–42 dB sur
  ces pixels (moteur 23) → jusqu'à ~+4 dB sur l'image.
- **Pourquoi le moteur rate** : une trajectoire fausse paraît cohérente
  (fond lisse : erreur médiane à deux côtés 0,011), le repli en fondu ne se
  déclenche presque jamais, l'objet bave.
- **Prototype** (branche locale `wip/occlusion-layers`, derrière
  `occlusion_strength ≥ 1,5`) : (a) trajectoire à deux côtés choisie au
  pixel parmi les vecteurs des 3×3 texels des deux champs, gardée seulement
  si chaque extrémité est expliquée par elle au moins aussi bien que par
  les autres, sur fenêtre décalée (meilleur des 4 quadrants 2×2 : un patch
  centré sur la frontière contient l'autre couche) → +0,22 / +0,17 / +0,09
  / +0,42 dB, aucune perte, mais synthèse 0,14 → 0,73 ms en 720p ;
  (b) échantillon d'un seul côté en occultation : quand le côté est juste,
  il retire la moitié de l'erreur totale de still (19,5 → 30 dB sur ces
  pixels), mais **l'ordre des couches est ambigu avec deux images** —
  « le plus rapide devant », couches = les deux vecteurs les plus fréquents,
  comparaison des deux hypothèses : toujours net négatif (−0,4 à −1 dB).
- **Suite** : l'image N−2 (anneau d'historique de 3) tranche les zones
  recouvertes — un fond recouvert entre N−1 et N était visible en N−2, sa
  vitesse se vérifie (prev(a) ≈ prevprev(a − v)), pas celle du mauvais
  vecteur. L'ordre établi là vaut pour le bord découvert du même objet.
  Rendre (a) abordable (porte sur l'erreur du moteur, moins de candidats).
- **Essai N−2, même jour** (branche `wip/occlusion-layers`, 2e commit) :
  l'image N−2 elle-même n'est pas sûre dans la couche (les sources sont les
  images de la swapchain, que le jeu réécrit), mais le **flux arrière de la
  paire précédente** (`prevBwd_`) porte la même information. Mesuré : aux
  pixels recouverts, il donne le vrai vecteur à l'extrémité dans prev dans
  60 à 79 % des cas (flux courant : 4 à 16 %), aux pixels découverts 2 à
  10 % — il reconnaît donc aussi le côté. Règle (candidat confirmé par la
  paire précédente, extrémités en désaccord, extrémité courante tenue par un
  autre mouvement → prev(a) seul ; veto si un candidat confirmé s'accorde
  aux deux bouts) : pixels recouverts bien traités 21 → 26 dB, mais
  déclenchements à tort sur des pixels visibles du bord des objets → net
  −0,4 à −2 dB après quatre versions (tolérance au quantum du flux,
  2×2 texels). **Cause commune à tous les essais** : au contour, le flux
  par blocs est faux dans *toutes* les paires (texels de 8×8 px à cheval),
  et le vrai vecteur manque parfois du voisinage ; une décision au pixel
  qui lit ces champs hérite de leur erreur. Piste restante : une
  segmentation de mouvement au pixel dans la bande (étiqueter chaque pixel
  de prev et de cur par le vecteur qui l'explique le mieux, sur fenêtre
  décalée, à pleine résolution), puis la décision d'occultation sur ces
  étiquettes plutôt que sur les champs de blocs.

**Windows**
- Transmettre le plein écran exclusif (mode 1).
- Installateur et signature.
- Repli Desktop Duplication (mode 2).
- Mesure complète de la latence de capture.

**Steam Deck** : le plugin Decky existe (`packaging/decky`, 03/10, cf. §4.4)
mais n'a jamais tourné sur un vrai Deck — reste la validation.

**CI GitHub** (dépôt privé, résultats rapportés par l'utilisateur) : verte
le 01/10 après `96b2f91`, jobs `distros`, `package` (archives, `.deb`,
`.rpm`, `archive-check`), `panel` et `nix` compris. Piège vécu : les runners
sont lents, lavapipe y dispute le CPU au jeu et le rattrapage retire des
images générées (×1,7) — les tests comptent donc sortie = 2 × jeu −
rattrapages.

**Paquets, suite** : publier `bdex-framegen-bin` / `-git` sur l'AUR, le
`.spec` sur COPR / OBS, `packaging/debian` sur un PPA, l'extension sur
Flathub (tous prêts et vérifiés en conteneur ; dépôt public et comptes
requis) ; Flatpak du panneau **impossible
en l'état** : le bac à sable a son propre espace de PID et ne voit pas les
jeux de l'hôte (`/proc/<pid>/maps`), le mode texte couvre les distributions
sans GTK 4. GPU sur portable hybride : `--gpu` (choix manuel ; pas
d'automatisme, le jeu sur le GPU dédié avec l'écran sur l'intégré reste le
cas normal). Validation sur vrai matériel : `docs/linux-validation.md`
(liste à confier à des testeurs).

**Banc gamescope headless** : sans `CAP_SYS_NICE`, il compose mal quand le
jeu est limité par son propre rendu. Le cadencement côté écran se valide sur
un vrai écran.

---

## 10. Conventions de travail

- **Licence** (03/10, demande de l'utilisateur) : logiciel **propriétaire**,
  tous droits réservés (`LICENSE`) ; les versions jusqu'à 0.5.5 incluse
  étaient MIT et le restent pour ceux qui les ont. Composants tiers :
  `THIRD_PARTY_NOTICES.md` (CAS d'AMD, en-têtes Khronos, DXVK livré sous
  Windows, GLFW de la démo) — à tenir à jour à chaque ajout. Métadonnées
  des paquets : `LicenseRef-proprietary` (Nix : `unfree`, autorisé par le
  flake pour lui seul ; Gentoo : `all-rights-reserved`).

- **Commits** sur `main`, au nom de Bdexez, sans ligne d'attribution à
  Claude. **Pousser seulement sur demande.**
- **Tests** : jamais de fenêtre sur l'écran de l'utilisateur.
  - Sans fenêtre : les tests `bdex_offline` et unitaires, directement.
  - Affichage : dans `gamescope --backend headless` ou Docker.
  - Mesures sous gamescope : charger la build par un `XDG_DATA_HOME` privé.
    L'environnement de l'hôte charge la couche **installée** ;
    `VK_IMPLICIT_LAYER_PATH` cache la couche WSI de gamescope.
- **Avant de pousser du C++** : `tools/ci/run.sh build` et `mingw`. Après
  toute modification du worker ou de la synchronisation, toujours la
  reproduction Docker (lavapipe a révélé un danger WAW que le GPU du poste ne
  montrait pas).
- **Défauts** : un nouveau réglage est une option à 0, identique bit à bit ;
  il ne devient défaut qu'après mesure (`eval_sequence.py`).
- **Langue** : échanges et documents internes (AGENTS.md, ce fichier) en
  français ; README en anglais.
- **FIFO par défaut** : non, l'utilisateur la rejette pour la latence.
- **Fiabilité des compteurs** : le compteur d'i/s de Steam est fiable
  (vérifié sur 4 machines), ne pas le remettre en cause.

### 13/10 (suite) — panneau : jeu non détecté après une réinstallation

Vécu : réinstallation (`install.sh`) pendant que PRAGMATA tournait → le
panneau affichait « Aucun jeu en cours ». Le jeu garde l'ancienne couche
(fichier supprimé) et, dans le conteneur Steam, `/proc/<pid>/maps` la nomme
`/ (deleted)` : la recherche de `libVkLayer_bdex_framegen` échouait. Le
panneau prend aussi un processus dont le `<pid>.status` a moins de 10 s
(`fresh_status_pids`) et, si sa couche a été remplacée (`layer_replaced`),
affiche « Lancé avant l'installation de cette version — relance le jeu ».
Fenêtre étroite : trois boutons en en-tête du groupe « Jeux détectés »
réduisaient titre et description à une lettre par ligne ; seul
« Actualiser » reste en en-tête, les boutons de capture ont leur ligne.

### 13/10 (suite) — artefacts en ×3/×4, limite d'images dans le panneau

Retour utilisateur (PRAGMATA, RTX 3060 Ti, 1440p 144 Hz sans VRR, Hyprland,
jeu à 30 i/s) : ×2 quasi propre, ×3/×4 « beaucoup d'artefacts », latence
très faible. Demande : plus de précision en ×3/×4 (+5 ms de latence
acceptés) et la limite d'images du jeu dans le panneau.

- **Panneau** : « Limite d'images du jeu » (`fps_limit`) passe dans le
  groupe « Génération d'images » (aucune, Auto = écran ÷ multiplicateur,
  30 à 120 i/s), avec les plafonds qui remplissent l'écran (144 Hz :
  ×2 → 72, ×3 → 48, ×4 → 36) ; mode texte : `l`.
- **Le moteur n'est pas en cause en ×4** : à t = 0,5 l'image ×4 est
  identique au bit près à celle de ×2 (147 paires) ; à 30 i/s réels la
  caméra bouge de 9 px (médiane) à 54 px par image à 853×480, dans la
  portée de la recherche (~±94 px). Les mêmes défauts restent juste à
  l'écran ¾ du temps au lieu de ½.
- **Gain mesuré n° 1 : les préréglages gardent les options du GPU dédié.**
  `autoConfigure` ne faisait rien dès qu'un préréglage était choisi : la
  config de l'utilisateur (`preset = quality`) tournait sans
  `pixel_vectors`, `interp_splat`, `rot_weight`, `temporal_prediction`,
  `zero_bias=0`, `scene_cut_local`. Avec elles, sur PRAGMATA : pixels
  grossièrement faux (aucun pixel vrai à ±2 px ne correspond, écart > 40)
  3,95 → 3,26 % en ×2, 7,78 → 6,22 % en ×3 ; PSNR +0,06 / +0,29 ;
  séquences `edge_bench` en ×4 +0,4 à +1,1 dB ; ~+1 ms de synthèse par
  image du jeu en 1440p ×4. Désormais : tout préréglage sauf `performance`
  (`Config::presetName`).
- **Gain n° 2 : la cadence de l'écran.** `bdex_offline --refresh` (piège :
  le jeu y tourne **toujours à 60 i/s**, `--spacing` = images d'entrée par
  image du jeu ; 30 i/s sur 144 Hz = `--spacing 8 --refresh 288`) :
  grille inconnue → connue, ×2 40,6 → 43,2 dB, ×3 36,3 → 38,5, ×4 33,0 →
  35,6, scintillement ÷2. Écran = m × jeu et grille connue : ×4 quasi
  parfait (scintillement 0,8). Sous Hyprland la grille n'est probablement
  pas mesurée : statut `display_grid` (1 mesurée, 0 pas de grille, absent
  inconnu) et `fps_limit`, message dans le panneau / le lanceur en ×3+ sans
  grille ni limite.
- **Essais écartés (mesurés, ne pas refaire)** :
  - `edge_flow` : vecteur par pixel aux frontières de mouvement choisi
    parmi les 3×3 blocs voisins par agrégation à poids adaptatifs
    (Yoon & Kweon, fenêtre 7×7 sur la luma) : edge_bench fixe +0,68 dB,
    rapide +0,18 sans `pixel_vectors`, mais neutre sur PRAGMATA
    (décisions bruitées : flou de mouvement, membres animés, transparences)
    et −0,23 par-dessus `pixel_vectors`. Seuil d'écart (2–4 px), marge :
    rien n'y fait.
  - `pixel_vectors` jugé sur fenêtre 3×3 (au lieu d'un pixel) : 0,00 sur
    le jeu, −0,06 à −0,12 sur edge_bench.
  - Prototype `wip/occlusion-layers` (05/10) sur le vrai jeu : 1,5 → −0,10
    dB, 1,8 → −0,68.
  - `despeckle` (pixel isolé → moyenne des voisins) : 0,03 % des pixels
    touchés — les « grains » noirs au bord de l'arme sont des fragments de
    rainures (lignes contrastées), pas des pixels isolés.
  - Par-dessus la nouvelle base : `levels=6`, `flow_temporal`,
    `flow_iterations=3`, `occlusion_strength=1`, `static_guard`,
    `smoothness=0.008` neutres ou négatifs ; `subpixel_lk=2` +
    `synth_bicubic` +0,14 sur le jeu mais −0,01 à −0,02 ailleurs ;
    `learned_correction` +0,16 sur le jeu, −0,6 à −2,3 ailleurs.
- **Pièges de banc** : `tests/test_corpus.py` et `test_offline.py` lisaient
  la config de l'utilisateur (corrigé : `BDEX_FG_CONFIG=/dev/null`) ;
  `eval_sequence.py` fixe `balanced`, qui sur un GPU dédié inclut
  désormais les options automatiques (les références du corpus restent
  celles de lavapipe : le test les coupe sur un GPU dédié). Mesure par
  pixel grossier : `gross.py`-like (min sur ±2 px, écart > 40).
- **Reste** : l'erreur restante est aux contours d'objets animés / flous de
  mouvement (non rigide) — hors de portée d'un flux à deux images ; la
  piste suivante est la cadence (obtenir la grille sous Wayland :
  `present_timing` / retour d'affichage du compositeur).

### 13/10 (suite) — limiteur sans latence, rotations rapides de la caméra

Retour utilisateur : « la latence induite par la limitation d'images du
logiciel est monstrueuse » (PRAGMATA avec `fps_limit`, Oblivion Remastered
avec la limite du jeu) ; nouveaux enregistrements des deux jeux.

- **Limiteur** : `limitFrameRate()` dormait au début de `present()` —
  l'image finie attendait son créneau. Mesuré (`latency_bench.sh`, démo non
  plafonnée + `fps_limit=40` contre démo plafonnée à 40) : ×1 30,2 ms contre
  6,7, ×2 (réelle) 43,3 contre 20,2. Le limiteur est maintenant à la fin de
  `present()`, après la remise de l'image et les attentes de
  `max_frame_latency` / `latency_limiter` : ×1 4,5 ms, ×2 19,9 ms, 40,0 i/s
  tenus. Piège du banc : la couche horodate le present quand elle le prend
  en main ; avec l'ancien limiteur la jointure (2 ms) ne trouvait aucun
  flash → `BDEX_LATENCY_JOIN_MS`. `latency_probe` vérifie désormais le
  plafond (≤ limite du jeu + 4 ms).
- **Observations** : `display_grid=0` confirmé sous Hyprland (pas de grille
  de rafraîchissement). PRAGMATA : rotations de 150–190 px par image à
  853×480 (≈ 560 px en 1440p), flou de mouvement, hologrammes transparents.
  Oblivion : rotation sur des pavés (texture dense, quasi périodique), fumée,
  flamme, brume ; 26 % d'images identiques = moments où la caméra est fixe
  (pas de saccade du jeu).
- **La portée de la recherche** : ±(4·2^(niveaux−1) + 30) pixels *d'analyse*.
  « Qualité » analysait en pleine résolution : en 1440p sa portée relative
  était 4× plus courte que celle de « Équilibré ». Mes bancs à 853×480 le
  cachaient — **toujours mesurer aussi à la résolution du jeu** (séquences
  agrandies en 1440p, `hd_*`).
- **Conflit mesuré** : un niveau grossier de ~40 px de large suit les
  rotations (+0,9 dB sur les jeux en 1440p) mais moyenne les objets
  indépendants (edge_bench rapide −1,3 dB) ; ~80 px l'inverse. Résolu par
  `global_motion` (`global_motion.comp`) : décalage dominant cherché
  exhaustivement (±16 px au niveau le plus grossier, ~0,05 ms), second
  centre de recherche par bloc grossier, gardé si le bloc s'y accorde mieux
  (lu dans l'emplacement du flux grossier, inutilisé au niveau le plus
  grossier ; `pc.global = 0` → identique bit à bit). Seul : −17/−19 % de
  pixels grossièrement faux en 1440p, ±0,04 dB ailleurs. Avec « Qualité » à
  demi-résolution au-delà de 2,6 Mpx (`flowScale = -1`, `flowScaleFor`) :
  −23/−29 %, +1,0/+0,5 dB, flux 6,9 → ~2 ms. Auto sur GPU dédié
  (`kAutoGlobalMotion`).
- **Reste** : une rotation de ~500 px entre deux images reste détruite
  (même vue à 15 i/s équivalents) ; la grille d'affichage sous Wayland ;
  contours d'objets animés.

### 13/10 (fin) — latence de l'interpolation, limite au point d'entrée

Retour : « le framecap a toujours de la latence » ; demande : améliorer
l'interpolation en latence et en qualité.

- **Le limiteur et les jeux Direct3D sous Proton** : DXVK et VKD3D-Proton
  appellent `vkQueuePresentKHR` depuis un fil à eux, et le jeu peut mettre
  1 à 3 images en file (latence maximale DXGI). Retenir ce present remplit
  la file : le jeu garde 1–2 images d'avance (30–55 ms à 36 i/s). Une
  limite dans le jeu dort avant la lecture des entrées et ne file rien. Les
  seuls points vus par la couche sur le fil du jeu : le sommeil Reflex
  (`vkLatencySleepNV`) et l'étape « input » d'Anti-Lag → la limite y dort
  (`DeviceData::capAtSleep`), le limiteur de present s'efface. Mesuré
  (démo `--anti-lag`, couche Mesa) : ×1 6,6 ms (limite du jeu 6,7).
- **Reflex bloqué par la couche** : `hide_present_ext` cache
  `VK_KHR_present_id`, dont dépend `VK_NV_low_latency2` → DXVK / VKD3D
  n'activent pas Reflex quand la couche est active. Piste : virtualiser
  present_id / present_wait (identifiants du jeu ↔ présents réels de la
  couche, attente sur l'affichage de l'image réelle, marqueurs traduits).
- **Latence de l'interpolation, mesurée (démo 30 i/s, 144 Hz)** : ×1 5,1 ms ;
  ×4 première image 11,7, image réelle 33,9 (la retenue 3/4 d'image du jeu =
  25 ms) ; `interp_hold=auto` sans effet à ×4 sur 144 Hz (images déjà à
  < 1,25 rafraîchissement) ; hybride 16,5 ms mais −0,6 à −1,1 dB et +15 à
  +30 % de pixels faux sur les jeux enregistrés (extrapolation −2 à −3,5).
- **Gain fait : la première image générée n'attend plus les autres**
  (`FrameSlot::cmd2` / `firstFence`, ×3+ avec deux files) : 1440p
  `quality`, réelle ×4 ~36 → ~32 ms, ×3 ~34 → ~30 ms. Validation de
  synchronisation (×8, hybride) et Docker lavapipe verts.

### 14/10 — ×4 sur un jeu à 30 i/s : cadence d'abord, puis l'image

Retour : Cyberpunk limité à 30 i/s dans le jeu (le limiteur de la couche
« ajoute toujours de la latence »), ×4, « encore pas mal d'artefacts ».
Demande : le maximum contre les artefacts en ×4 à 30 i/s, une idée après
l'autre, sans dépasser +10 ms de latence. Écran de l'utilisateur :
2560×1440 à 143,912 Hz, sans VRR (Hyprland 0.56, `allow_tearing = 0`),
RTX 3060 Ti, pilote 615.

- **La cadence était le gros défaut.** 30 i/s ×4 = 120 images pour 144
  rafraîchissements : une image tenue deux rafraîchissements sur cinq (24
  fois par seconde), et le contenu des images décalé de leur affichage
  jusqu'à un demi-rafraîchissement. Sans grille mesurée (`display_grid=0`
  sous Hyprland), chaque présentation espaçait ses images depuis sa propre
  fin GPU. Trois changements (commit e31b79a) :
  - `refresh_fill` (défaut 1 = dès ×3) : ×m qui ne remplit pas l'écran à
    moins d'une image près → m ou m+1 images par image du jeu, une par
    rafraîchissement (le cadencement adaptatif), étage de sortie dimensionné
    pour m+1 ;
  - `virtual_grid` : sans grille mesurée, une grille à la période de sortie
    et à une phase à nous, déplacée d'une demi-période quand des images
    sont écrasées (ou, 47423df, affichées à moins d'un tiers de période de
    la précédente : retours d'affichage bruités d'une surface X11) ;
  - `pace_plan` : sur une grille, chaque image est prévue pour son
    rafraîchissement dès l'appel du jeu (délai GPU estimé, prudent :
    +0,5 à la hausse, 0,05 à la baisse, mesuré seulement quand le worker a
    vraiment attendu la fence — sinon il comptait son propre retard : 9 ms
    pour 4,6) et présentée pour lui. Avant : prévision depuis l'appel du
    jeu, GPU encore au travail → 35 à 67 % des images affichées un
    rafraîchissement après leur contenu.
  Mesuré (gamescope sans fenêtre 144 Hz, démo 30 i/s ×4, trois essais) :
  120 → 142–143 images affichées/s, répétitions 30 → 5/s, hors plan 67 →
  10 %, image réelle 33 → 40 ms (+7 ms). 40 i/s ×3 : +5 ms. ×2 à 60 i/s :
  +7 ms pour une latence de 14 ms → pas par défaut (`refresh_fill = 2`).
  Le banc varie beaucoup d'un lancement à l'autre (0,4 à 5 répétitions/s
  à réglages égaux) : toujours trois essais.
- **Xwayland + NVIDIA** : une surface X11 (jeux Proton) n'a pas `mailbox`
  (FIFO, IMMEDIATE, FIFO_LATEST_READY — sans l'extension de device qu'il
  exige) → `present_mode=auto` prend IMMEDIATE, sans déchirure sous
  Hyprland (tearing interdit). Hypothèse probable pour `display_grid=0` :
  les retours d'affichage d'Xwayland y sont trop bruités pour la tolérance
  de 0,5 ms.
- **`interp_accel`** (auto GPU dédié, commit 36f136a) : la trajectoire
  courbe quand le mouvement a changé depuis la paire précédente (écart
  `a·t(1−t)/2`). Accélération calculée une fois par image à la résolution
  du flux (`accel_map.comp`, binding 10 de la synthèse), sur 5×5 blocs, et
  gardée seulement si les voisins concordent (cohérence 0,85–0,98 ; sans
  ce test, objets à vitesse constante −0,4 dB). Jeux enregistrés +0,03 à
  +0,17 dB, scintillement jusqu'à −0,5 ; synthétique ≤ −0,02 ; +0,1 ms en
  1440p ×4 (par pixel : +3,8 ms).
- **`static_soft` en extrapolation** (15/10, vers l'image courante — un fondu
  au-delà extrapolerait la luminosité) : +0,076 dB en moyenne, aucune perte,
  scènes avec HUD +0,05 à +0,42, arêtes +0,6 à +6 ; interpolation identique.
- **`extrapolate_accel` re-mesuré** (15/10, suite complète en extrapolation) :
  −0,132 dB, scintillement en hausse sur les scènes synthétiques (+0,1 à
  +0,7) ; seuls PRAGMATA 3 (+0,34) et Oblivion HD (+0,47) gagnent. Reste
  opt-in.
- **Bilan de la nuit du 14 au 15/10** (suite complète, interpolation, base
  `54dc548` → `119b609`) : +0,098 dB en moyenne, pixels faux −3,2 % ;
  1440p +0,21 / +0,13, scintillement −0,79 / −0,46 ; PRAGMATA scintillement
  −0,15 à −0,42 ; virages ×4 +0,18 à +0,27 ; Cyberpunk −0,01 / +0,02 ; pire
  −0,06 (`edge_bench` rapide). Adoptés : `false_detail` (et en
  extrapolation : +0,15 dB), `border_predict`, plafond d'`interp_accel` 0,75 ;
  opt-in : `parallax_search`.
  Coût réel de `false_detail` (pente de 20 répétitions, 1440p, enregistrement
  PRAGMATA au mouvement rapide) : tuile ~1 ms par image générée, carte des
  entrées ~0,6 ms + dilatation ~0,2 ms une fois par image du jeu ; la 1re
  image générée attend ~2 ms de plus (et non 0,7 comme écrit d'abord).
  Essais pour réduire la tuile (garde HUD depuis la mémoire partagée,
  couleurs décodées une fois, moyenne séparable) : dans le bruit (0,9–1,2 ms).
  Carte de détail par entrée gardée d'une image à l'autre (l'entrée courante
  devient la précédente) : moitié du calcul de la carte, mêmes images ; en
  tampons de stockage (5 images de stockage dépassaient les 4 garanties :
  `min_limits` l'a attrapé en CI).
  Latence (banc de cadence 720p, 5 essais alternés `54dc548` / fin de nuit) :
  image réelle présentation → écran médiane ~42,0 → ~42,4 ms (+0,4, dans le
  bruit), affichées ~143/s des deux côtés, hors plan 7–34 % → 5–11 %.
- **`false_detail`** (auto GPU dédié, passe `passes/false_detail.cpp`,
  `false_detail.comp`, après la synthèse et avant `learned_correction` /
  le HUD) : diagnostic — sur les scènes ×4 de type jeu, les images générées
  ont 20 à 33 % de détail fin (|laplacien|) de *plus* que la vérité : la
  mosaïque des blocs voisins aux vecteurs différents sur du contenu flou
  (virage avec flou de mouvement) ; `pixel_vectors`, `interp_splat`,
  `interp_accel` n'en sont pas la cause (le flux lui-même). Règle : détail
  de l'image générée (moyenne 5×5 du |laplacien| de la luma, unités 8 bits)
  > 1,5 × la cellule 4×4 la plus détaillée des deux entrées à ±16 px + 2 →
  vers la moyenne 5×5 ; pas là où les deux entrées ont ce détail au pixel
  même (3×3, seuil 8 : HUD immobile). Essais : référence non dilatée
  (moyenne autour de q) −2 à −9 dB sur `edge_bench` (un objet net arrive sur
  un fond lisse) ; plus grande fenêtre de moyenne : pire (la moyenne dilue) ;
  remplacement par le fondu : −0,1 à −0,9 ; flou bilatéral : gain perdu (les
  marches de la mosaïque sont fortes) ; sans le garde HUD : arêtes de la scène
  sombre −2,7 dB. Seuil 1,0 : −1,4 dB (trop de vrai détail touché) ; rayon 8
  px : plus de gain mais `eb_fast` −0,07. Coût : la carte des entrées
  (une fois par image du jeu, un fil par pixel : un fil par cellule
  lisant 6×6 coûtait 1,3 ms) ; tuile 22×22 en mémoire partagée ; recopie
  seulement des tuiles changées. Le banc `bdex_offline --bench` varie de
  ±1 ms d'un lancement à l'autre au début (fréquence du GPU) : comparer les
  2e et 3e lancements. `test_offline.py` coupe `false_detail` pour juger
  `scene_cut_local` seul (il relève les deux côtés). Étendu à
  l'extrapolation le 15/10 : suite complète en `mode=extrapolate`, toutes les
  séquences en hausse, +0,154 dB en moyenne (pire +0,03), 1440p +0,26 /
  +0,13, scintillement −1,10 / −0,48 ; arêtes des déplacements latéraux
  −0,33 / −0,39. Seuil 1,25 au lieu de 1,5 pour les images prédites (leur
  mosaïque est plus grossière) : +0,023 de plus, chaque séquence ≥ +0,01.
  Idem en interpolation quand le flux est à demi-résolution (1440p
  « qualité ») : +0,016, chaque séquence 1440p ≥ +0,01, scintillement en baisse.
  1,1 : encore +0,01 / +0,015 mais arêtes latérales −0,17 ; 1,25 gardé (marge :
  1,0 sans le garde perdait −1,4).
  Seuil du garde HUD 8 → 12 (15/10) : arêtes +0,05 à +0,09 sur les virages,
  rien en baisse (6 : −0,002 ; 16 ≈ 12).
  Rayon de la moyenne à l'échelle des blocs du flux (2 à pleine
  résolution, 4 à la moitié : 1440p « qualité ») : enregistrements 1440p
  +0,03 / +0,01 dB, scintillement −0,10 / −0,04 de plus ; rien ailleurs.
- **Repli de la synthèse** (pixels qu'aucune trajectoire n'explique, fondu
  non déplacé à t) : fondu « adouci » (plus près de l'image proche) neutre ;
  image la plus proche seule −0,07 dB en moyenne, `edge_bench` −0,24. Le
  fondu linéaire reste.
- **Re-mesurés sur la base du 14/10 au soir** : `flow_temporal` neutre
  (−0,001 dB, ne vaut pas ses 0,28 ms) ; `subpixel_lk=2` + `synth_bicubic`
  +0,02 en moyenne (enregistrements 853×480 jusqu'à +0,18) mais 1440p −0,02
  / −0,05 et scintillement en hausse : pas par défaut (le sous-pixel ne paie
  pas avec le flux à demi-résolution) ; flux pleine résolution en 1440p
  (`flow_scale=1`) −0,05 dB, scintillement +0,3.
- **Bords de l'écran pendant un virage** (14/10) : sur les scènes ×4 de
  virage, 37 % de l'erreur est dans les 2 × 6 % de colonnes du bord (×4
  l'erreur du centre) ; sur les enregistrements Cyberpunk, rien de plus
  qu'au centre (×0,7–1,0), PRAGMATA ×1,3–1,5. Essai dans la synthèse :
  trajectoire dont l'autre extrémité sort de l'image gardée seule (pas de
  mélange ni de repli), celle dont la source sort écartée : −0,1 à −0,4 dB
  (bord d'entrée mieux, bord de sortie bien pire), chaque moitié seule
  négative aussi. Cause : le flux des blocs du bord est faux (leur
  correspondance tombe hors de l'image ; le matcher compare au pixel de bord
  répété) — la correction irait dans le matcher (SAD sur la partie dans
  l'image), pas dans la synthèse.
- **Pires images de l'enregistrement PRAGMATA 1440p** (15/10) : la n° 9 est
  une coupure de caméra (le personnage et le HUD sautent de ~650 px entre la
  vérité et l'image suivante) ; cohérence avant/arrière du flux : 94 %
  d'incohérents (> 8 px), mais les virages éclairs (~300 px par paire) en
  ont 75–92 % — pas un critère de coupure ; et même là, la synthèse bat la
  répétition et le fondu (14,5 / 12,0 / 13,3 dB). Plusieurs paires (n° 1,
  3, 5, 7) ont une vérité quasi identique à une extrémité (35 dB contre
  l'image d'avant pour la n° 7) : pas des répétitions (aucune dans les
  enregistrements : 0 image identique au milieu d'un mouvement) mais des
  démarrages / arrêts brusques — écarts successifs 1,1 puis 14,6 : le
  mouvement commence après la vérité, et la vitesse constante place le
  contenu à mi-chemin trop tôt (paires de 2 images : effet doublé).
- **Options auto revérifiées sur la base du 15/10** : sans `rot_weight`
  −0,021 dB (Oblivion −0,10, scintillement +0,13) ; sans
  `temporal_prediction` −0,040 (virages −0,32, scintillement des jeux +0,1 à
  +0,5) — sauf la scène synthétique sombre (+0,31, arêtes +1,6 sans elle) ;
  Cyberpunk, sombre aussi, perd sans elle (−0,04 / −0,09). Gardées.
- **Plafond de l'accélération d'`interp_accel`** (`accel_map.comp`, 0,5·|B| + 1 :
  un démarrage depuis l'arrêt n'est corrigé qu'à moitié, un arrêt pas du
  tout — |B| ≈ 0) : 1,0·|B| + 1 → +0,002 dB, scintillement des jeux −0,07 à
  −0,24, pixels faux −1 %, mais Cyberpunk −0,05 ; 2,0·|B| −0,006 ;
  0,5·max(|B|, |a|) (arrêts compris) −0,026 (PRAGMATA −0,17). Pas changé
  (paires de 2 images des enregistrements : accélérations doublées). Sur la
  nouvelle scène `make_game.py flick` (×4 exact : immobile, puis 600 px en
  133 ms avec départ et arrêt en douceur) : base 38,60 dB, 1,0·|B| et
  2,0·|B| 38,61, max(|B|,|a|) 37,59, sans plafond 37,33 — et
  `interp_accel=0` 38,68 : le modèle à accélération constante ne décrit pas
  un coup de poignet ; le plafond le protège. **Adopté : 0,75·|B| + 1**
  (même jour) : moyenne +0,005, scintillement −0,02 à −0,21 sur 6 des 9
  enregistrements, pixels faux −1,2 %, pire −0,03 (Cyberpunk cp1) ;
  scènes synthétiques et coup de poignet ±0,00. Départ depuis l'arrêt (paire
  précédente immobile : a doublé, x = v·t²) : 38,61 — rien ; pendant le coup
  de poignet l'erreur est le flou et l'ampleur, pas la courbe.
- **`false_detail` : recomposer plutôt que flouter** (15/10) — les pixels
  en mosaïque relus dans les deux images le long du flux moyen des 5×5
  texels voisins : virages −0,12 / −0,39 dB (1440p) ; moitié flou, moitié
  recomposition −0,01 / −0,06. Le flou reste (dans la mosaïque, le flux
  moyen est lui-même un mélange de vecteurs faux). Flou gaussien (σ = 0,6 ou 1,0 × rayon)
  au lieu de la boîte : ±0,01, inchangé. Référence compensée par le mouvement (cellules
  aussi là où le flux dit que le contenu était dans chaque entrée) : −0,011,
  rien gagné sur les objets rapides (`eb_fast` 0,00) — dans la mosaïque, le
  flux faux va chercher du détail ailleurs et empêche le lissage.
- **Seuil de frontière de `pixel_vectors`** (0,5 px ; 0,25 et 1,0) : ±0,001 dB,
  inchangé.
- **`hud_mask` 3** (15/10, au lieu de 4) : virages +0,04 / +0,05, mais faible
  contraste −0,05 ; 4 reste.
- **`search_fine` 4 en 1440p** (portée fine doublée en pixels d'écran par la
  demi-résolution) : −0,000 dB ; 3 reste. `false_detail` sans les options du
  GPU dédié (lavapipe) : +0,01 seulement — pas de test de CI discriminant.
- **`refine_bias` en 1440p** (15/10) : 0,375 +0,005 (arêtes −0,14), 0,625 −0,034 ;
  0,5 reste.
- **`smoothness` en 1440p** (flux à demi-résolution : vecteurs deux fois plus
  courts, pénalité relative double) : 0,002 +0,005 dB, 0,004 −0,007 sur les
  séquences 1440p — 0,003 reste.
- **Modèle global de caméra** (`camera_model`, 15/10, essai) : mouvement du
  décor ajusté sur tout le flux fin (quadratique 5+5 coefficients, approx.
  de l'homographie d'une rotation ; moindres carrés repondérés Cauchy,
  3 passes ; texels immobiles exclus), donné aux texels où il explique la
  fenêtre 8×8 aussi bien (tolérance de SAD). Sans condition : arêtes −0,4 à
  −0,9 dB (au contour d'un objet le vecteur de la caméra a souvent un SAD
  brut plus bas que celui retenu par le matcher, qui pénalise l'écart à la
  prédiction — et l'objet perd son vecteur parmi les candidats de la
  synthèse) ; seulement si ≥ 6 voisins sur 8 suivent déjà le modèle :
  −0,02 dB en moyenne, PRAGMATA −0,07. Le matcher suit déjà la caméra ; où
  il s'en écarte, c'est surtout un objet. Abandonné.
- **`pixel_vectors` sur 3×3 texels** (au lieu des 4 autour, 15/10, pensé
  pour le flux à demi-résolution du 1440p) : arêtes −0,4 à −0,9 dB partout
  — la leçon des candidats jugés sur la concordance des extrémités tient.
- **`parallax_search`** (15/10, opt-in, passe `passes/parallax_search.cpp`,
  `flow_parallax.comp`, crochet `fineFlow` avant `subpixel_lk`) : nouvelle
  scène `make_game.py fence` (rambarde : poteaux de 3–8 px tous les
  20–60 px, ×2,5 la vitesse du fond, sans flou de mouvement) : erreur ×19
  dans la zone de la rambarde ; le flux n'y a **aucun** vecteur à −80 (celui
  des poteaux), tout à −32 (le fond) : les niveaux grossiers effacent les
  poteaux, les niveaux fins cherchent à ±3 autour du fond, le médian 3×3
  effacerait une colonne isolée. Règle : là où la fenêtre 8×8 est mal
  expliquée (moyenne > 0,015), essayer ×0,5 / ×1,5 / ×2 / ×2,5 / ×3 du
  vecteur (±1 px le long du mouvement), garder si SAD < 0,7× et si l'autre
  sens a le candidat opposé autour du point d'arrivée (3×3 texels). Sans
  l'accord des deux sens : rambarde +0,44 mais virage −0,20, panoramique
  −0,37 (multiples trouvés par hasard sur du contenu flou) ; accord sur le
  seul texel d'arrivée : rambarde +0,16. Suite : rambarde +0,32, pixels
  faux −11 %, latéral +0,02 / +0,03, reste ±0,02. Coût (pente de 20
  répétitions) ~1 ms par image du jeu sur PRAGMATA 1440p (9×9 candidats et
  fenêtre pleine : +6 à +11 ms ; damier et 3 décalages par multiple) : sur
  le chemin de la latence, budget de l'objectif presque atteint → opt-in.
  Puis fenêtre de référence lue une fois pour tous les candidats : ~0,6 ms
  (pente de 20 répétitions, fréquence stable) ; puis multiples exacts
  d'abord et ±1 px autour du meilleur seulement (7 fenêtres au lieu de 15,
  mêmes images à 0,01 dB près) : ~0,3 ms. Toujours opt-in.
- **`border_predict`** (15/10, auto GPU dédié, `block_match.comp`) :
  diagnostic — sur les virages ×4, 37 % de l'erreur dans les 2 × 6 % de
  colonnes du bord ; la synthèse n'y peut rien (essai ci-dessus) : le flux
  des blocs du bord est faux, leur correspondance est hors de l'image et
  le matcher compare au pixel de bord répété (cible lue avec `clamp`). Règle :
  correspondance prédite à plus de moitié hors de l'image → la prédiction
  (niveau grossier ; caméra au plus grossier), recherche sautée. Suite :
  +0,05 dB en moyenne, virages +0,15 à +0,19, PRAGMATA scintillement −0,25 /
  −0,32, pire −0,06 (`edge_bench`) ; corpus : `pan` +0,13 / +0,38
  (extrapolation), mais `popup` −0,33 et `hud` −0,15 (fond de bruit : au
  surgissement du menu, de grands vecteurs faux « sortent » aussi de
  l'image). Variantes : seuil ¼ / ¾ / tout hors image moins bons que ½ ;
  sans le niveau le plus grossier : virage +0,11 au lieu de +0,19 ; exiger
  l'accord des voisins (≤ 1–4 px) : `popup` −0,12 mais virage +0,12 —
  gardée la règle simple. Avec la règle de bord dans la synthèse en plus :
  pire. `test_offline.py` la coupe pour juger `scene_cut_local`.
  Complément essayé : SAD des candidats sur la seule partie de la cible
  dans l'image (normalisé par quadrant) : virage −0,10, le reste ±0,03 —
  pas retenu.
- **Cadence en 1440p** (15/10, banc de cadence en 2560×1440, démo 30 i/s ×4,
  5 essais alternés avant/après `false_detail` + `border_predict`) : affichées
  ~144/s des deux côtés, image réelle 40–42 ms des deux côtés, répétitions
  2–7/s (bruit du banc) — pas de régression. Mais « hors plan » 15 à 92 %
  selon l'essai, avant comme après : la distribution affichage − plan est
  bimodale (0 ou +1 rafraîchissement, parfois +½), alors qu'en 720p ~10 %.
  gamescope sans fenêtre (sans `CAP_SYS_NICE`) rate lui-même des
  rafraîchissements à 2560×1440 : pas jugeable sur ce banc ; à vérifier
  sur l'écran de l'utilisateur (`cadence_report.py`, colonne « off plan »).
  Suite (15/10) : démo rendue en 2560×1440 mais gamescope composant en
  1280×720 (sa charge réduite) : toujours bimodal (24–91 %) ; marge de
  présentation 0,5 période avant le rafraîchissement au lieu de 0,3 :
  8–51 % contre 14–73 % — la variation d'un essai à l'autre domine (phase de
  la grille estimée sur les horodatages de gamescope ?). Rien changé.
- **Banc 1440p ×4** (14/10, scratchpad `hd4/` : scènes `make_game.py`
  agrandies ×2 en Lanczos — la vérité ×4 exacte, flux à demi-résolution
  comme en jeu chez l'utilisateur) : `false_detail` +0,18 / +0,19 / +0,10 dB,
  pixels faux −4 %. `pending_decisions.py` sur les enregistrements (base
  actuelle) : `chroma_weight` 0,00, `static_guard` −0,01, `learned_flow`
  −0,27 (−1,25 en extrapolation) : rien à adopter.
- **`global_motion` anisotrope** (14/10, rayon x = largeur/3 au lieu de
  min(largeur, hauteur)/3, plafond 24 ou 32 ; essayé pour le pire virage de
  l'enregistrement 1440p PRAGMATA, ~700 px par paire) : neutre (−0,005 à
  +0,002 dB), scintillement et pixels faux en hausse sur ce même
  enregistrement (+0,2) : plus de fausses correspondances globales ; ce
  virage échoue pour une autre raison (parallaxe du personnage proche). Pas
  repris.
- **`global_motion`** : rayon d'un tiers du niveau grossier jusqu'à 24
  (au lieu d'un quart jusqu'à 16) : pixels faux −1 à −5 % sur les jeux.
- **Écartés (mesurés)** :
  - `hud_match` (pixels de HUD remplis par la scène dans l'analyse) :
    −0,2 dB ;
  - `hud_global` (vecteur caméra essayé près du HUD dans la synthèse) :
    jusqu'à −0,9 dB, le masque apprend aussi des pixels unis du décor, et
    les piliers flous du premier plan y prennent le vecteur caméra ;
  - `synth_bicubic` par défaut : +0,004 dB, scintillement en hausse ;
  - phase de la grille virtuelle par moyenne circulaire des heures
    d'affichage : plus de répétitions dans gamescope ;
  - pas d'effet domino sur un raté du plan : l'image écrasée fausserait le
    réglage de phase ;
  - rayon global 32 : moins bon que 24.
- **`content_clock`** (défaut 1, efac0cc) : images datées par l'affichage
  → t dans l'intervalle *réel* de la paire (comme `frame_timing`). Simulé
  ×4 30 i/s sur grille, temps d'image ±40 % : +0,45 à +0,58 dB,
  scintillement −0,5 à −0,9 ; pas fixe + présents irréguliers +0,2 à +0,3 ;
  jeu régulier identique bit à bit. (Le −0,6 de `frame_timing` en pas fixe
  était sans heures d'affichage.)
- **`tools/cadence_report.py LOG`** : la cadence réelle d'une partie depuis
  `latency_log` (affichées/s, tenues/s, hors plan, grille) — pour juger le
  remplissage sur le vrai écran.
- **Banc des jeux enregistrés** : enregistrés à 30 i/s et notés en ×2, leurs
  paires ont *deux* images d'écart : le double du déplacement d'une vraie
  paire à 30 i/s. Le virage flou de `cp2` qui se brise en blocs y reste
  cohérent entre images consécutives (quelques blocs locaux).
- **Encore écartés** : `smooth_contrast` (pénalité d'écart plus forte sur
  bloc peu contrasté : −0,13 à −0,65 dB, `cp2` compris) ;
  `occlusion_strength=1` sur la base actuelle (−0,01 en moyenne, −0,31) ;
  `flow_iterations=1` (neutre) ; gain de luminance de l'analyse sur la
  scène sombre (×2 +0,2, ×4 −0,5) ; préréglages `balanced` / `enhanced`
  au lieu de `quality` (−0,06 / −0,12 ; mais scène sombre +0,4, et
  `levels=4` y gagne +0,8 en perdant sur les virages rapides de PRAGMATA).
- **Suite (matin du 14/10)** :
  - `refresh_fill` aussi avec « Auto » (`target_fps`, 8ea2671) : l'entier
    choisi sous la fréquence laissait le même trou (30 i/s : 120 → 142
    affichées, tenues 28,5 → 4/s, +7,8 ms ; 40 i/s : +5,6 ms) ;
  - `hud_mask` à 4 images au lieu de 8 sur GPU dédié (5f85c14) : HUD
    +0,01 à +0,16 dB, extrapolation +0,13, pire −0,01 (2 : −0,72 en faible
    contraste ; 16 : −0,14) ;
  - plancher du banc gamescope : la démo à 144 i/s sans génération a déjà
    8–9 images tenues/s → les ~4/s du ×4 rempli y sont au plancher ;
  - écartés : renforcer la netteté des images générées (−0,12 à −0,34 dB
    dès 0,05) ; `hud_global` même strict (erreur > 0,06, divisée par 2 :
    −0,45) — trois échecs sur les trous du HUD, ne plus y revenir ; lissage
    3×3 de la luminance d'analyse contre le grain (−0,3) ; marge fixe sur
    le délai GPU du plan (+1 à 2,5 ms, pas moins de répétitions) ;
    `static_soft` plus rapide (0,6 : synthétique +, Cyberpunk −0,05) ;
  - scène « fps » ajoutée à `make_game.py` : le contour de l'arme sort
    propre ;
  - conseils de réglage du jeu : couper le grain de film (bruit nouveau à
    chaque image, imprévisible, il pulse entre réelles et générées) et
    réduire le flou de mouvement (virages très flous : blocs).
- **`search_fine` 3** (rayon des niveaux fins, préréglages `balanced`,
  `enhanced`, `quality`) : jeux enregistrés +0,02 à +0,08 dB, scintillement
  et pixels faux en baisse partout, scènes à arêtes jusqu'à +1,2, pire
  −0,05 ; +0,1 à +0,2 ms de flux en 1440p ; démo CI 29,50 → 29,58 dB.
  `search_radius` 6 sans effet (plafond `MAXR` = 4 du shader).
  `search_fine` 4 : +0,04 de plus en moyenne mais scènes de type jeu −0,01 à
  −0,07 et 81 candidats au lieu de 49 → 3 gardé.
- **`smoothness` 0,003** (au lieu de 0,004) : jeux enregistrés jusqu'à
  +0,08 dB (Cyberpunk +0,08 / +0,06), moins de scintillement ; 0,002
  gagnait plus (+0,17 sur Cyberpunk) mais le corpus `scroll` (motif quasi
  périodique) y perdait 1,5 dB — le corpus de ctest l'a attrapé, ma suite
  ne l'avait pas. Démo CI 29,58 → 29,53 / rapide 23,70 → 23,72.
- **Seuils re-mesurés sans changement** (suite + corpus ctest) : repli en
  fondu de la synthèse (0,12–0,25 ; 0,18–0,35 neutre, 0,08–0,18 −0,04 dB) ;
  mélange des deux extrémités d'une trajectoire (0,04–0,12 ; 0,06–0,18
  +0,02 seulement, et une constante de la synthèse casse l'invariant bit à
  bit) ; `refine_bias` (0,5 ; 0,25 et 0,375 : jeux réels un peu mieux,
  arêtes des scènes de synthèse −0,1 à −0,3 ; 1,0 : corpus `occlude`
  −4 dB). `flow_iterations` 3 neutre, `smoothness` 0,008 −0,23.
  Seuils de `interp_splat` (acceptation 0,9 → 0,75, déclenchement 0,02 →
  0,01) : ±0,004 dB. La synthèse est réglée au plafond de ce banc.
  Candidat temporel dans la synthèse (flux de la paire précédente essayé
  comme trajectoire là où celle-ci concorde mal, gardé à 0,8× l'erreur) :
  Cyberpunk −0,31 / −0,25 dB, corpus `scroll` −1,1 — il concorde par hasard.
  Candidat « mouvement nul » aux contours (avec `pixel_vectors`) : Cyberpunk
  −0,30 / −0,35, `scroll` −1,3. **Leçon** : quatre candidats de trajectoire
  ajoutés et retenus parce que leurs extrémités concordent mieux (caméra
  près du HUD ×2, paire précédente, mouvement nul) ont tous perdu — une
  concordance de couleur trouvée en plus est surtout un faux appariement.
  Ne plus ajouter de candidat jugé sur ce seul critère.
- **Synthèse par couches du 05/10 (`wip/occlusion-layers`, (a)) reprise sur
  la base actuelle** : trajectoire choisie parmi les vecteurs des 3×3 texels
  des deux champs, fenêtre décalable 2×2, gardée si chaque extrémité est
  mieux expliquée par elle. Elle gagnait +0,1 à +0,4 dB le 05/10, sans
  `pixel_vectors` ni `interp_splat` ; aujourd'hui partout en contour :
  −0,10 dB en moyenne, marche avant −0,57 (le zoom fait différer tous les
  vecteurs voisins, le modèle par couches ne tient pas) ; seulement où le
  moteur échoue (min(ef, eb) > 0,04 / 0,08, erreur de fenêtre < 0,7×) :
  −0,02 à −0,04, marche −0,22 à −0,36 ; seules les scènes `edge_bench`
  gagnent (+0,05 à +0,13). Là où les deux trajectoires échouent, le mélange
  prudent bat une trajectoire nette mais incertaine. Abandonné.
- **Délai GPU du plan au 90ᵉ centile** (32 derniers) au lieu de la moyenne
  « vite à la hausse, lente à la baisse » : 3 essais semblaient donner
  −2,5 ms et moins de ratés ; l'A/B à 5 essais alternés dit l'inverse
  (latence 41,4 contre 41,1 ms, hors plan 24,7 % contre 11,8 %). Gardé
  l'original. Leçon : sur ce banc, **toujours** un A/B alterné à 5 essais.
- **Piste non faite** : remplir le rafraîchissement manquant *après*
  l'image réelle (extrapolation t ≈ 1,2) → image réelle ~7 ms plus tôt
  qu'avec le remplissage, mais une extrapolation à t = 1,25 vaut une
  interpolation à t = 0,5 (−1,5 à −2,9 dB par rapport à t = 0,25).
- **Constat** : les images générées des jeux enregistrés n'ont que 85–88 %
  du détail fin de la vérité (pompage de netteté possible) ; le bicubique
  n'y change rien de mesurable.
