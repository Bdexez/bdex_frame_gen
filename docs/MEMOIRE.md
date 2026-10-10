# Mémoire du projet bdex-framegen

État au **30 septembre 2026** — dernière version publiée **0.5.5** (26/09) ;
depuis, trois lots non publiés en version mais poussés sur `main` (jusqu'à
`86e6996`) : parité Lossless Scaling, compatibilité matériel/distributions,
SteamOS.

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
- Jeux 32 bits : couverts.

**Autres cas gamescope**
- **Sessions gamescope** ailleurs (Bazzite, ChimeraOS…) : même fonctionnement.
- **gamescope imbriqué** sur un bureau, backend SDL : le profil fourni
  `[gamescope] hide_present_ext = 0` permet de générer sur la sortie du
  compositeur (tout ce qui tourne dedans, par exemple glxgears 337 → 675 i/s).
  Le backend Wayland par défaut présente des dmabufs, pas de swapchain : la
  couche ne peut rien y faire.

**Non validé sur un vrai Deck** : tout a été mesuré dans un gamescope
headless de la même génération.

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

---

## 6. Outils, tests et CI

**Outils d'évaluation**
- `bdex_offline` : le moteur sans fenêtre, sur une séquence PPM. Il
  reproduit exactement les images de la couche.
- `tools/eval_sequence.py` : PSNR global, mouvement, arêtes, SSIM,
  scintillement ; A/B avec `--variant`.
- `tools/run_eval.sh` : évaluation en ligne ; `tools/make_corpus.py`,
  `tools/corpus_pack.py` : corpus ; `tools/tune.py` : réglage automatique.

**Tests ctest (15)**

| Groupe | Tests |
|---|---|
| Sans fenêtre | `unit`, `matcher` (vrai shader contre modèle C++ du départage), `eval_quality`, `offline`, `corpus`, `formats`, `min_limits`, `determinism` (chaque option deux fois), `option_keys`, `panel_logic` |
| Avec affichage | `integration` (×2 à travers la couche), `validation` (couches Khronos + synchronisation, toutes les options), `acquire_timeout`, `multithread_config` |
| Hors écran, sautés s'ils ne peuvent pas tourner | `gamescope_limit` (limite Steam rejouée dans un gamescope headless) |

**Règle** : jamais de fenêtre sur l'écran de l'utilisateur. Les tests
d'affichage tournent dans `gamescope --backend headless` ou dans Docker
(Xvfb + lavapipe).

**CI GitHub**
- `linux.yml` :
  - build + tests + seuil PSNR (28,0 / 23,0 dB) ;
  - sanitizers (ASan, UBSan, TSan) ;
  - syntaxe Windows (MinGW x64/i686) ;
  - `distros` : 6 distributions ;
  - `package` : archives + `archive-check` avant toute release.
- `windows.yml` : MSVC x64 et Win32, tests, zips. En cas d'échec, les logs
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
| Jeu limité par le GPU, 4K lourd, avant/après le cadencement GPU | 66 → 132 images affichées/s |
| `latency_limiter`, 4K lourd | image → écran 26,8 → 18,3 ms, 97 → 101 i/s |
| Hybride ×3, jeu limité par le GPU | 116 → 283 images affichées/s, jeu 58 → 94 i/s |
| Adaptatif | 50 i/s → 144 affichées sur 144 Hz |
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

Le détail de toutes les mesures est dans AGENTS.md §10 et le CHANGELOG.

---

## 9. Problèmes connus et pistes

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

**`alt_vectors`** (03/10, opt-in, écarté) : second candidat du matcher (≥ 2 px
du gagnant) essayé par pixel dans la synthèse. Sans marge : scroll −3,0,
subpixel −2,7 (il gagne sur du bruit) ; avec marge (×0,75 − 0,004) et seulement
si la trajectoire gardée est mauvaise (erreur > 0,04) : seul hud +0,57, pan
−0,62, scroll −1,74 ; par-dessus pixel_vectors rien à gagner (pan −0,57).
Tampons de stockage (pas d'image : 4 images de stockage garanties, test
`min_limits`). pixel_vectors trouve déjà les mêmes cas.

**`flow_upsample`** (03/10, opt-in, écarté des défauts) : sur-échantillonnage
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
`debug=confidence` en rouge. Repli `confidence_fallback` (binding 9 de la
synthèse, variante complète seulement) **mesuré négatif** : 0,5 → démo +0,20,
rapide +0,02, corpus −0,2 à −1,1 dB, scintillement en hausse partout ;
laissé à 0. Ne pas y revenir sans nouvelle idée de repli (le fondu est plus
flou qu'un warp approximatif).

**Qualité** : mouvement rapide (ambiguïté du coût SAD), rotations et zoom
(modèle affine par bloc à faire), occlusions.

**Windows**
- Transmettre le plein écran exclusif (mode 1).
- Installateur et signature.
- Repli Desktop Duplication (mode 2).
- Mesure complète de la latence de capture.

**Steam Deck** : un **plugin Decky Loader** (menu rapide : activer,
multiplicateur, pause en jeu). Le mode Jeu n'a aujourd'hui que « sans le
panneau » et les options de lancement.

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
