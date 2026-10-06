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
    Reflex ;
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

