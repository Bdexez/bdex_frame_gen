# bdex-framegen

Génération de frames (« frame generation ») pour les jeux Vulkan sous Linux,
sous la forme d'un **layer Vulkan implicite**. Le layer s'insère entre le jeu
et le pilote, récupère chaque image rendue par le jeu, estime le mouvement
entre deux images consécutives (flux optique sur GPU) et insère une ou
plusieurs images intermédiaires synthétisées : un jeu qui tourne à 30 fps est
affiché à 60 fps (x2), 90 (x3) ou 120 (x4).

Fonctionne avec les jeux Vulkan natifs et avec les jeux Direct3D via
DXVK / VKD3D-Proton (Steam Play), sans modification du jeu.

```
Jeu ──► vkQueuePresentKHR ──► [layer bdex-framegen] ──► écran
                                │
                                │  thread du jeu (≈0,1 ms) :
                                ├─ copie de l'image dans un historique
                                ├─ pyramide de luminance + block matching hiérarchique
                                │  (flux optique avant et arrière, affinage 4x4)
                                ├─ synthèse des images intermédiaires (warping bidirectionnel
                                │  pondéré par la cohérence photométrique, détection de cut)
                                │
                                │  thread de présentation du layer :
                                └─ copie dans la vraie swapchain, cadencement, présentation :
                                   image(s) générée(s) puis image réelle
```

Le jeu rend dans des images privées fournies par le layer ; la swapchain réelle
appartient au layer et est présentée depuis un thread dédié, sur une file
(queue) Vulkan séparée de celle du jeu quand le GPU en a une. Le jeu n'est
jamais bloqué par la présentation des images générées.

## Prérequis

* Linux, Vulkan 1.1+ (loader ≥ 1.3.234), GPU avec support compute (RADV, ANV, NVIDIA).
* Pour compiler : CMake ≥ 3.20, un compilateur C++20, `glslc` (shaderc),
  les en-têtes Vulkan ; GLFW (optionnel) pour l'application de démo.

Sous Arch : `pacman -S cmake gcc vulkan-headers shaderc glfw`.

## Compilation et installation

```sh
tools/install.sh              # compile et installe dans ~/.local
# ou manuellement :
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix ~/.local
```

L'installation dépose :

* `~/.local/lib/libVkLayer_bdex_framegen.so`
* `~/.local/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json`
* `~/.local/bin/bdex-framegen` (lanceur)

Désinstallation : `tools/uninstall.sh`.

`install.sh` compile aussi la variante 32 bits (`VK_LAYER_BDEX_framegen_32`,
dans `~/.local/lib32`) quand un compilateur multilib et `/usr/lib32/libvulkan.so.1`
sont présents, pour les jeux 32 bits. À la main : `cmake -DBDEX_32BIT=ON`.

## Utilisation

Le layer est **implicite mais désactivé par défaut** : il ne fait rien tant que
la variable d'environnement `BDEX_FG=1` n'est pas définie.

```sh
BDEX_FG=1 ./mon_jeu                      # x2 (défaut)
bdex-framegen -m 3 -- ./mon_jeu          # x3 via le lanceur
bdex-framegen --fullres --profile -- vkcube
```

Steam → propriétés du jeu → options de lancement :

```
BDEX_FG=1 %command%
BDEX_FG=1 BDEX_FG_MULTIPLIER=3 %command%
```

Sans installation, depuis l'arbre de build :

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer BDEX_FG=1 ./build/demo/bdex_demo --fps 30
```

### Démo

`bdex_demo` rend une scène procédurale (objets en mouvement, HUD statique,
compteur de frames) à un débit plafonné :

```sh
build/demo/bdex_demo --fps 30                 # sans le layer : 30 fps
BDEX_FG=1 build/demo/bdex_demo --fps 30       # avec : 60 fps affichés
```

Le layer affiche périodiquement dans le terminal : `game 30.0 fps -> output 60.0 fps (x2.00)`.

## Options

Toutes les options se donnent par variables d'environnement `BDEX_FG_<CLÉ>`
ou dans `~/.config/bdex-framegen.conf` (`clé = valeur`, une par ligne ;
l'environnement est prioritaire).

| Clé | Défaut | Description |
|---|---|---|
| `BDEX_FG` | – | `1` active le layer, `0` le désactive |
| `MULTIPLIER` | `2` | images affichées par image rendue : 2, 3 ou 4 |
| `FULLRES` | `0` | flux optique calculé en pleine résolution (plus net, ~3× plus coûteux) |
| `LEVELS` | `4` | niveaux de la pyramide de flux (1–6) |
| `SEARCH` | `4` | rayon de recherche au niveau le plus grossier (1–4) |
| `SEARCH_FINE` | `2` | rayon de recherche aux niveaux fins (1–4) |
| `REFINE` | `1` | passe d'affinage du flux en blocs 4×4 |
| `PRESENT_MODE` | `app` | force le mode de présentation : `fifo`, `mailbox`, `immediate`, `relaxed` |
| `PACING` | `1` | espacement temporel des images en modes non-FIFO |
| `SCENE_CUT_LOW` / `SCENE_CUT_HIGH` | `0.10` / `0.22` | seuils de coût de matching pour détecter un changement de plan |
| `DEBUG` | `none` | `flow` (visualise le flux), `split` (gauche générée / droite réelle), `passthrough` (layer actif sans génération) |
| `STATS` / `STATS_INTERVAL` | `1` / `5` | statistiques dans le terminal |
| `PROFILE` | `0` | temps GPU par étape dans les statistiques |
| `SHARED_QUEUE` | `0` | force l'utilisation de la file du jeu (test) ; dans ce cas FIFO est remplacé par mailbox + cadencement |
| `LOG` | `1` | verbosité 0–3 ; `LOG_FILE` pour écrire dans un fichier |
| `DUMP` / `DUMP_FRAMES` | – / `24` | écrit les images présentées (PPM) dans un dossier, pour le débogage |

## Comportement et limites

* **Latence** : comme toute génération de frames, l'image réelle est affichée
  une demi-frame plus tard (à x2). Le layer ne modifie pas les entrées.
* **Vsync (FIFO)** : sur un écran 60 Hz, x2 impose au jeu un maximum de 30 fps,
  x3 de 20 fps, etc. – la file de présentation FIFO absorbe les images
  supplémentaires. Utilisez `PRESENT_MODE=mailbox` ou `immediate` (ou un écran
  VRR) pour ne pas brider le jeu.
* **Coût** : sur une Vega 8 intégrée (≈1,1 TFLOPS), ~3,5 ms de GPU par image
  rendue en 1000×1000 avec les réglages par défaut (flux en demi-résolution),
  ~8 ms en pleine résolution. Le thread du jeu passe ≈0,1 ms dans
  `vkQueuePresentKHR`. Sur un GPU dédié le coût est négligeable.
* **Qualité** : le flux optique par block matching gère bien les translations et
  les HUD statiques ; les rotations rapides, les occlusions importantes et les
  motifs répétitifs produisent des artefacts locaux. Un changement de plan
  désactive la synthèse pour cette image. `DEBUG=flow` permet de voir ce que
  le layer « comprend » du mouvement.
* Formats de swapchain pris en charge : RGBA/BGRA 8 bits (UNORM et sRGB),
  A2B10G10R10 / A2R10G10B10, RGBA16F. Les autres passent sans génération.
* Non pris en charge (transparent) : swapchains multi-couches (VR), protégées.
* Extensions de présentation gérées : `VK_KHR_present_id` / `present_wait`
  (l'identifiant est attaché à l'image réelle), `VK_EXT_swapchain_maintenance1`
  (fence de présentation, changement de mode, `vkReleaseSwapchainImagesEXT`).
* Testé avec : la démo, `vkcube` (Wayland et XWayland), `vkgears`, le
  conteneur Steam Linux Runtime (le layer y est visible et chargé).

## Développement

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure       # tests unitaires + test bout-en-bout
tools/run_eval.sh build 40                        # PSNR des images générées vs vérité terrain
```

`tools/run_eval.sh` rend la démo à 60 fps (référence) puis à 30 fps à travers
le layer et compare chaque image générée à l'image réelle correspondante.
À titre indicatif : ~30 dB pour les images générées, contre ~25 dB pour un
simple fondu des deux images voisines et ~22 dB pour une duplication.

Arborescence :

* `layer/src/layer.cpp` – points d'entrée du layer, dispatch, hooks Vulkan
* `layer/src/swapchain.cpp` – swapchain virtuelle (acquire / present)
* `layer/src/framegen.cpp` – ressources et enregistrement des passes GPU
* `layer/shaders/` – shaders de calcul (pyramide, block matching, affinage, interpolation)
* `demo/` – application de test GLFW
* `tools/` – lanceur, scripts d'installation et d'évaluation

## Licence

MIT.
