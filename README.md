<div align="center">

# bdex-framegen

**Génération de frames pour les jeux Vulkan sous Linux — un layer, zéro modification du jeu.**

[![Licence MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=cplusplus&logoColor=white)](#)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.0%2B-AC162C.svg?logo=vulkan&logoColor=white)](#)
[![Plateforme](https://img.shields.io/badge/plateforme-Linux-FCC624.svg?logo=linux&logoColor=black)](#)
[![Validation Khronos](https://img.shields.io/badge/validation%20Khronos-0%20erreur-success.svg)](#validation)

30 fps rendus → **60, 90 ou 120 fps affichés**, sur n'importe quel jeu Vulkan
(natif ou Direct3D via DXVK / VKD3D-Proton).

<img src="docs/demo.gif" alt="Démo : 30 fps à gauche, 60 fps avec bdex-framegen à droite (ralenti ×4)" width="812">

*La démo rendue à 30 fps (gauche) et ce que le layer affiche (droite), au ralenti ×4 : une image sur deux est synthétisée.*

</div>

---

## Sommaire

- [Comment ça marche](#comment-ça-marche)
- [Installation](#installation)
- [Utilisation](#utilisation)
- [Configuration](#configuration)
- [Performances et qualité](#performances-et-qualité)
- [Limites](#limites)
- [Validation](#validation)
- [Développement](#développement)
- [Arborescence](#arborescence)
- [Contribuer](#contribuer)

---

## Comment ça marche

`bdex-framegen` est un **layer Vulkan implicite** (`VK_LAYER_BDEX_framegen`).
Il s'insère entre le jeu et le pilote graphique, récupère chaque image que le
jeu présente, estime le mouvement entre deux images consécutives, puis insère
une ou plusieurs images intermédiaires avant l'image réelle.

```
Jeu ──► vkQueuePresentKHR ──► [ bdex-framegen ] ──► écran
                                     │
      thread du jeu (≈ 0,1 ms) :     │
      ├─ copie de l'image dans un historique
      ├─ pyramide de luminance + block matching hiérarchique
      │  (flux optique avant et arrière, filtre médian, affinage 4×4)
      └─ synthèse des images intermédiaires : warping bidirectionnel
         pondéré par la cohérence photométrique, détection de changement
         de plan, fondu dans les zones d'occlusion
                                     │
      thread de présentation :       │
      └─ copie dans la vraie swapchain, cadencement, présentation :
         image(s) générée(s) puis image réelle
```

Quelques points de conception :

| | |
|---|---|
| **Swapchain virtuelle** | Le jeu rend dans des images privées fournies par le layer. La vraie swapchain appartient au layer, qui décide quoi présenter et quand. |
| **Thread dédié** | Le jeu n'est jamais bloqué par la présentation des images générées : `vkQueuePresentKHR` lui coûte ≈ 0,1 ms. |
| **Queue séparée** | Le travail du layer tourne sur une file de calcul distincte de celle du jeu quand le GPU en a une (sinon la file est partagée et FIFO est remplacé par mailbox + cadencement). |
| **Tout sur GPU** | Cinq shaders de calcul (pyramide, matching, médian, affinage, interpolation), aucun aller-retour CPU. |
| **Changements de plan** | Quand le mouvement n'est pas explicable, le layer affiche l'image réelle plutôt qu'un mélange. |

<img src="docs/comparaison.png" alt="Image réelle, image générée, image réelle suivante" width="900">

*L'image du milieu n'a jamais été rendue par le jeu : elle est synthétisée à partir des deux images voisines.*

<details>
<summary>Voir le flux optique estimé (<code>BDEX_FG_DEBUG=flow</code>)</summary>
<br>
<img src="docs/flux.png" alt="Visualisation du flux optique" width="480">

*Teinte = direction du mouvement, saturation = amplitude. Le HUD immobile reste gris.*
</details>

---

## Installation

### Prérequis

- Linux, Vulkan 1.0+ (loader ≥ 1.3.234), un GPU avec support compute (RADV, ANV, NVIDIA…)
- Pour compiler : CMake ≥ 3.20, un compilateur C++20, `glslc` (shaderc), les en-têtes Vulkan ; GLFW (optionnel) pour la démo

```sh
# Arch Linux
sudo pacman -S cmake gcc vulkan-headers shaderc glfw vulkan-tools
```

### En une commande

```sh
tools/install.sh      # compile et installe dans ~/.local (64 bits + 32 bits si multilib)
```

L'installation dépose :

```
~/.local/lib/libVkLayer_bdex_framegen.so
~/.local/lib32/libVkLayer_bdex_framegen.so                      (jeux 32 bits)
~/.local/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json
~/.local/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json
~/.local/bin/bdex-framegen                                      (lanceur)
```

Désinstallation : `tools/uninstall.sh`.

<details>
<summary>Compilation manuelle</summary>

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix ~/.local

# variante 32 bits (multilib requis)
cmake -S . -B build32 -DCMAKE_BUILD_TYPE=Release -DBDEX_32BIT=ON
cmake --build build32 -j && cmake --install build32 --prefix ~/.local
```
</details>

---

## Utilisation

Le layer est installé de façon **implicite mais inerte** : il ne fait rien
tant que la variable `BDEX_FG=1` n'est pas définie.

```sh
BDEX_FG=1 ./mon_jeu                      # x2 (défaut)
bdex-framegen -m 3 -- ./mon_jeu          # x3 via le lanceur
bdex-framegen --fullres --profile -- vkcube
bdex-framegen --check                    # le loader trouve-t-il le layer ?
```

**Steam** → propriétés du jeu → *Options de lancement* :

```
BDEX_FG=1 %command%
BDEX_FG=1 BDEX_FG_MULTIPLIER=3 BDEX_FG_PRESENT_MODE=mailbox %command%
```

Sans installation, depuis l'arbre de build :

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer BDEX_FG=1 ./build/demo/bdex_demo --fps 30
```

Le layer affiche périodiquement ses statistiques dans le terminal :

```
[bdex-fg  4.128 INFO] game 30.0 fps -> output 60.0 fps (x2.00), present call 0.14 ms avg
```

### La démo

`bdex_demo` rend une scène procédurale (objets en mouvement, HUD statique,
compteur de frames) à un débit plafonné, pour tester sans jeu :

```sh
build/demo/bdex_demo --fps 30               # sans le layer : 30 fps
BDEX_FG=1 build/demo/bdex_demo --fps 30     # avec : 60 fps affichés
build/demo/bdex_demo --help                 # --size, --mode, --speed, --cut, --srgb…
```

---

## Configuration

Toutes les options se donnent par variables d'environnement `BDEX_FG_<CLÉ>`
ou dans `~/.config/bdex-framegen.conf` (`clé = valeur`, une par ligne ;
l'environnement est prioritaire).

| Clé | Défaut | Description |
|---|:---:|---|
| `BDEX_FG` | – | `1` active le layer, `0` le désactive |
| `MULTIPLIER` | `2` | images affichées par image rendue : 2, 3 ou 4 |
| `FULLRES` | `0` | flux optique en pleine résolution (plus net, ~2× plus coûteux) |
| `LEVELS` | `4` | niveaux de la pyramide de flux (1–6) |
| `SEARCH` / `SEARCH_FINE` | `4` / `2` | rayon de recherche au niveau grossier / aux niveaux fins (1–4) |
| `REFINE` | `1` | passe d'affinage du flux en blocs 4×4 |
| `PRESENT_MODE` | `app` | force `fifo`, `mailbox`, `immediate` ou `relaxed` |
| `PACING` | `1` | espacement temporel des images en modes non-FIFO |
| `SCENE_CUT_LOW` / `SCENE_CUT_HIGH` | `0.05` / `0.09` | seuils de détection d'un changement de plan |
| `DEBUG` | `none` | `flow` (visualise le flux), `split` (gauche générée / droite réelle), `passthrough` |
| `STATS` / `STATS_INTERVAL` | `1` / `5` | statistiques dans le terminal, période en secondes |
| `PROFILE` | `0` | temps GPU par étape dans les statistiques |
| `LOG` / `LOG_FILE` | `1` / – | verbosité 0–3, fichier de sortie |
| `DUMP` / `DUMP_FRAMES` | – / `24` | écrit les images présentées (PPM) dans un dossier |
| `SHARED_QUEUE` | `0` | force le partage de la file du jeu (test) |

### Réglages par jeu

Le fichier de configuration accepte des sections `[nom]` qui ne s'appliquent
qu'aux processus dont la ligne de commande contient un exécutable de ce nom
(`.exe` facultatif, insensible à la casse) :

```ini
# ~/.config/bdex-framegen.conf
multiplier = 2
log = 1

[Cyberpunk2077.exe]
multiplier = 3
present_mode = mailbox

[vkcube]
debug = flow
```

---

## Performances et qualité

Mesures sur une **AMD Radeon Vega 8** intégrée (≈ 1,1 TFLOPS), démo en 955×1036 :

| Configuration | GPU / image rendue | Sortie |
|---|:---:|:---:|
| Défaut (flux en demi-résolution) | ≈ 3,5 ms | 30 → 60 fps |
| `FULLRES=1` | ≈ 8 ms | 30 → 60 fps |
| `MULTIPLIER=4`, mailbox | ≈ 4 ms | 30 → 120 fps |

Sur un GPU dédié, le coût est négligeable. Le thread du jeu passe ≈ 0,1 ms
dans `vkQueuePresentKHR`.

Qualité mesurée avec `tools/run_eval.sh` (PSNR des images générées contre la
vraie image rendue à 60 fps) :

| Méthode | PSNR |
|---|:---:|
| Duplication de l'image précédente | ≈ 22 dB |
| Fondu des deux images voisines | ≈ 25 dB |
| **bdex-framegen** | **≈ 30 dB** |

---

## Limites

- **Latence** : comme toute génération de frames, l'image réelle est affichée
  une demi-frame plus tard (à x2). Les entrées ne sont pas modifiées.
- **Vsync (FIFO)** : sur un écran 60 Hz, x2 impose au jeu un maximum de 30 fps,
  x3 de 20 fps… Utilisez `PRESENT_MODE=mailbox` / `immediate` ou un écran VRR
  pour ne pas brider le jeu.
- **Artefacts** : le flux optique par block matching gère bien les
  translations et les HUD statiques ; rotations rapides, grandes occlusions et
  motifs répétitifs produisent des artefacts locaux. `DEBUG=flow` montre ce
  que le layer « comprend » du mouvement.
- **Formats** pris en charge : RGBA/BGRA 8 bits (UNORM et sRGB),
  A2B10G10R10 / A2R10G10B10, RGBA16F. Les autres passent sans génération.
- Non pris en charge (transparents) : swapchains multi-couches (VR), protégées.
- Extensions gérées : `VK_KHR_present_id` / `present_wait`,
  `VK_EXT_swapchain_maintenance1` (fence de présentation, changement de mode,
  `vkReleaseSwapchainImagesEXT`).

---

## Validation

Le layer est propre sous `VK_LAYER_KHRONOS_validation`, validation de
synchronisation comprise, avec la démo, `vkcube` et `vkgears` :

```sh
VK_LOADER_LAYERS_ENABLE='*validation' BDEX_FG=1 build/demo/bdex_demo --fps 30 --frames 90
```

Il se charge également dans le conteneur Steam Linux Runtime (pressure-vessel).

---

## Développement

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure     # tests unitaires + test bout-en-bout
tools/run_eval.sh build 40                      # PSNR vs vérité terrain
BDEX_EVAL_ARGS="--speed 2.5" tools/run_eval.sh build 40   # scène à mouvement rapide
```

`tools/run_eval.sh` rend la démo à 60 fps (référence) puis à 30 fps à travers
le layer, et compare chaque image générée à l'image réelle correspondante
grâce au compteur de frames dessiné à l'écran.

---

## Arborescence

```
layer/
├─ src/layer.cpp        points d'entrée du layer, dispatch, hooks Vulkan
├─ src/swapchain.cpp    swapchain virtuelle (acquire / present, thread de présentation)
├─ src/framegen.cpp     ressources GPU et enregistrement des passes
├─ src/config.cpp       options (environnement, fichier, sections par jeu)
└─ shaders/             downsample, block_match, flow_smooth, flow_refine, interpolate
demo/                   application de test GLFW (scène procédurale)
tests/                  tests unitaires et test d'intégration
tools/                  lanceur, install/uninstall, évaluation de qualité
```

---

## Contribuer

Les retours de tests sur de vrais jeux (DXVK, VKD3D-Proton, NVIDIA, Intel…)
sont ce qui manque le plus au projet : ouvrez une issue avec le log du layer
(`BDEX_FG_LOG=2`), votre GPU et la façon dont le jeu est lancé.

Pour proposer du code, lisez [CONTRIBUTING.md](CONTRIBUTING.md) : mise en
place, ce qu'il faut vérifier avant une pull request (tests, validation
Khronos, mesures de qualité et de performance) et conventions du code.
L'historique des versions est dans [CHANGELOG.md](CHANGELOG.md).

---

<div align="center">

Licence [MIT](LICENSE).

</div>
