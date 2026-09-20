# AGENTS.md — contexte projet pour l'assistant

Ce fichier est la mémoire de travail du projet : il résume l'architecture, les
commandes, les invariants et les pièges, pour qu'une session ultérieure puisse
reprendre le projet sans re-dérivation. Sources de vérité complémentaires :
`README.md` (options, usage Linux), `docs/windows-port.md` (mode 1),
`docs/windows-capture.md` (mode 2, checklists de validation hardware),
`docs/windows-README.md` (usage Windows), `ROADMAP.md` (état + mesures),
`CHANGELOG.md` (journal par version).

---

## 1. En une phrase

**bdex-framegen** génère des frames de jeu par optical flow GPU (comme Lossless
Scaling / AFMF maison) : pyramide luma + block matching hiérarchique, puis
synthèse par interpolation ou extrapolation ; upscaling spatial + sharpening
CAS + HUD fps en option. Projet C++17 / Vulkan / CMake, shaders GLSL→SPIR-V
embarqués au build. Auteur : Bdexez.

## 2. Les deux produits

| | Mode 1 — couche Vulkan | Mode 2 — app capture (Windows) |
|---|---|---|
| Binaire | `libVkLayer_bdex_framegen.so` / `.dll` | `bdex_capture.exe` |
| Principe | layer implicite qui virtualise les swapchains du jeu | capture WGC de la fenêtre jeu, traitement, re-présentation en overlay |
| Plateforme | Linux (shipping), Windows (compilé CI, jamais validé sur hardware) | Windows x64 |
| Latence | ~0 ms (dans le present path) | +~1 frame (inherent à la capture) |
| Anti-cheat | se rétracte si EAC/BattlEye/GameGuard/XIGNCODE3/miHoYo détecté | sûr (pas d'injection) |
| Couverture | jeux Vulkan natifs (et Proton via DXVK/VKD3D) | tous les APIs (D3D9/10/11/12, GL, Vulkan) |

Les deux partagent le même moteur : `FrameGen` (`layer/src/framegen.cpp`) et
tous les shaders de `layer/shaders/`.

## 3. Carte du repo

- `layer/src/` — la couche : `layer.cpp` (négociation loader, dispatch,
  `vkGetInstanceProcAddr`), `swapchain.cpp` (virtualisation des swapchains,
  pacing), `framegen.cpp` (orchestration du pipeline), `config.cpp`,
  `device.cpp`, `platform.cpp` (shims Linux/Windows), `format.cpp`, `log.cpp`.
- `layer/shaders/` — `downsample`, `block_match`, `flow_smooth`,
  `flow_refine`, `reduce_cost`, `interpolate`, `upscale`, `cas`,
  `common.glsl`. Embrasés en SPIR-V au build (`cmake/EmbedShader.cmake`,
  cibles `*_f32` / `f16` / `u32` / `u64` selon le format de stockage luma).
- `capture/src/` — `capture.cpp` (WGC, frame pool, ring D3D11), `vkctx.cpp`
  (interop D3D11→Vulkan + pilote `FrameGen` + swapchain overlay), `launcher.cpp`
  (fenêtre de config native Win32), `main.cpp`.
- `demo/` — démo Vulkan (scène animée) servant de cobaye pour l'éval et les
  tests ; `tools/run_eval.sh` + `tools/eval_quality.py` (PSNR vs référence).
- `tools/` — GUI de contrôle GTK4 (`bdex-framegen-gui`, Linux), `install.sh` /
  `uninstall.sh` (x64 + x86), scripts PowerShell d'enregistrement du layer.
- `tests/` — tests unitaires (config, formats) + `integration.sh` (démo 30 fps
  à travers la couche, vérifie le ratio x2 et "generation on").
- `.github/workflows/windows.yml` — build MSVC x64 + Win32, tests, zip par arch
  (`bdex-framegen-windows-<arch>`), logs poussés sur `ci-log-<arch>` en cas
  d'échec ; tags `v*` → GitHub Release (pré-release si suffixe `-`).
- `.github/workflows/linux.yml` — build + tests unit/intégration (Xvfb,
  lavapipe) + gate qualité PSNR (échec sous 28,0 dB normal / 23,0 dB rapide) à
  chaque push. Pièges runner (cf. §9) : paquet `glslc` (pas `glslc-tools`),
  ICD lavapipe résolu par glob + vérifié `vulkaninfo`, couche enregistrée dans
  `implicit_layer.d` global.

## 4. Commandes

```bash
cmake -S . -B build && cmake --build build -j$(nproc)   # Linux
ctest --test-dir build --output-on-failure              # unit + intégration
tests/integration.sh build/demo/bdex_demo build/layer   # e2e GPU (X11/Wayland)
tools/run_eval.sh                                       # éval qualité (PSNR)
```

Windows : **pas de machine Windows dans l'environnement** — le build MSVC, les
tests et le packaging sont faits par la CI à chaque push ; l'artefact x64
contient `bdex_capture.exe` + le DLL layer + scripts + docs. Les logs d'échec
sont relisables sans compte GitHub sur la branche orpheline `ci-log-x64`.

## 5. Pipeline de génération (par présentation)

La séquence (identique dans la couche et dans `VkCtx::present`) :
history ring (3 images) → **analysis** (`downsample` : pyramide luma, niveau
fin ~0,5 Mpx auto) → **flow** par niveau (`block_match` avant+arrière autour de
la prédiction du niveau coarse, `flow_smooth`, `flow_refine` 4×4) →
`reduce_cost` (détection de cut de scène, seuils calibrés) → **synthèse** :
`interpolate` (warp bidirectionnel des deux frames + blend le long des
trajectoires, t=0.5 ; référence en cut) ou `extrapolate` (prédit la frame
suivante, ~0 ms de hold) → `upscale` (bilinear / Catmull-Rom / Lanczos-2, si
`render_scale < 1`) → `cas` (sharpening) → HUD fps. Pacing par sommeil quand le
present mode n'est pas FIFO ; `PRESENT_MODE=auto` transforme la demande FIFO du
jeu en mailbox.

## 6. Le block matcher — le cœur, ses invariants et ses pièges

`block_match.comp` : un workgroup par bloc 8×8, 64 threads en 4 quadrants de
16 ; la cible est cherchée dans une fenêtre (2R+1)² centrée sur la prédiction
du niveau coarse (R = `search_radius`=4 au niveau coarse, `search_fine`=2
ailleurs) ; tuiles stagées en shared memory. Coût = SAD luma
(`+ grad_weight × SAD gradient` opt-in) + pénalités `smoothness` (déviation de
la prédiction) et `zero_bias` (déplacement absolu). Un candidat « zéro motion »
supplémentaire garantit la stabilité des HUD statiques. Le coût exporté
(`costOut`) est le SAD luma du gagnant — les seuils de cut de scène sont
calibrés dessus, ne pas y exporter le coût gonflé par le gradient.

**Invariant n°1 — bit-for-bit à poids nuls.** `grad_weight = 0` et
`census_weight = 0` (défauts) doivent reproduire **exactement** le
comportement historique (mêmes coûts, mêmes comparaisons, même tie-breaking).
Le commit `0549e44` (2026-09-17) a réintroduit des artefacts en production en
cassant cet invariant : un scan séquentiel « premier indice gagne à égalité » a
remplacé la réduction par arbre (sous-arbre gauche gagne à égalité), et les
égalités exactes de coûts arrivent dans les zones plates / textures
périodiques. Fix `095ed10` : restaurer la sélection d'origine (minimum par
thread stride + réduction) quand `census_weight = 0`, le scan séquentiel ne
servir que pour le re-ranking opt-in. **Toute future modif du matcher doit soit
préserver ce chemin bit-for-bit, soit être une option explicite.**

## 7. Configuration

Priorité : défauts < `~/.config/bdex-framegen.conf` (Windows :
 `%APPDATA%\bdex-framegen\bdex-framegen.conf`) < env `BDEX_FG_<KEY>`.
Sections `[nom]` par process (le nom du jeu, sans `.exe`), `[*]` global.
La couche est toujours chargée et reste pass-through tant que `enabled=0`
(ou section absente) ; `BDEX_FG=1` l'active. Presets : `balanced` (défaut),
`quality` (flow_scale 1, levels 5, iterations 2), `performance` (flow_scale 4,
refine_all 0, iterations 0), `latency` (extrapolation + x2), `auto`
(iGPU→performance, dGPU→balanced, grosse dGPU→quality). Les knobs flow
avisés (`levels`, `search*`, `refine*`, `flow_scale`, `flow_iterations`)
désactivent l'auto-tune. Voir `Config` dans `layer/src/config.h` pour la liste
complète (multiplier 1-4, render_scale, upscale_filter, sharpness, scene cut
0.05/0.09, overlay, debug=flow|split|passthrough, dump PPM…).

## 8. Produit Windows capture — architecture et pièges hardware

Chaîne : `GraphicsCaptureItem` (WGC) → frame pool free-threaded → ring de 3
textures D3D11 `B8G8R8A8` (`SHARED_NTHANDLE | SHARED_KEYEDMUTEX`, régénéré si
la fenêtre grandit) → import Vulkan (`VK_KHR_external_memory_win32` +
`VK_KHR_win32_keyed_mutex`, allocation dédiée, acquire/release chaînés au
submit, barriers de ownership `VK_QUEUE_FAMILY_EXTERNAL`) → `VkCtx` construit
le `DeviceData` du layer sur le device Vulkan de l'app et pilote `FrameGen`
inchangé → swapchain Win32 (mailbox) dans un overlay topmost sans activation
qui suit la fenêtre jeu (`WS_EX_TRANSPARENT` pour laisser passer les clics),
pacing comme `presentOne`. Sélection GPU discrète par LUID commun D3D/Vulkan
(`--gpu`). Options CLI : `--list/--title/--pid`, `--multiplier`, `--mode`,
`--preset`, `--filter`, `--sharpness`, `--hud`, `--scale/--fit`, `--fifo`.
Sans argument : launcher natif (sélecteur de fenêtre + options avancées flow,
persistées dans la config partagée).

Validé sur hardware le 2026-09-15 (laptop Intel iGPU + RTX 3050) : PRAGMATA
(D3D12), Dark Souls III, Skyrim SE (D3D11), Fallout: New Vegas (D3D9) — x2
extrapolation stable. Bugs réels trouvés sur hardware et leur fix (ne pas les
réintroduire) :
- **Flicker** : un descriptor/une image de stage partagés entre frames en vol
  → command buffers par soumission (`VkCtx::nextCmd`, `kCmdSlots`).
- **Double curseur** (jeux qui cachent le curseur matériel) : pas de class
  cursor sur l'overlay, `GetCursorInfo` force la dissimulation.
- **`log_file`** : `%VAR%`/`~` expansés avant ouverture (`platformExpandPath`).

## 9. Pièges transverses

- `hide_present_ext=1` (défaut) : cacher `swapchain_maintenance1` / present_id
  au jeu, sinon VKD3D-Proton se bloque (la sémantique vise la vraie swapchain
  qu'on virtualise).
- Les tests d'intégration nécessitent un display (skip propre sinon) ; le test
  e2e exerce la génération sur GPU — le faire tourner après toute modif de
  shader.
- Les artefacts se diagnostiquent avec `BDEX_FG_DEBUG=flow` (voir le flux) et
  `dump_dir` (PPM), cf. CONTRIBUTING.md.
- CI Linux depuis le 2026-09-21 (`linux.yml`, verte) : unit + intégration +
  gate PSNR sur lavapipe à chaque push. On peut toujours tout lancer à la main
  (cf. §4). Le seuil PSNR (28,0/23,0) est calibré au chiffre lavapipe réel, qui
  colle au GPU à ~0,04 dB — toute régression matcher le fait chuter.

## 10. Décisions & mesures clés (retenir les chiffres)

- Éval qualité (reproductible depuis `a296641`) : baseline **31,0 dB normal /
  23,7 dB fast** (`--speed 2`). Le régime faible = **mouvement rapide**, et
  c'est un problème d'ambiguïté du coût de matching, pas de rayon ni de
  régularisation (`levels=5` byte-identique ; les deux directions de
  `smoothness` dégradent).
- `grad_weight` (09-15) : fast **+0,5 dB**, normal **−2,1 dB** → opt-in.
- census (09-17) : large **+0,45 fast / −1,5 normal** ; gating serré seul
  non-négatif (+0,12 normal) ; soft weight dominé → opt-in.
- Sub-pixel parabolique : −0,1 les deux. Zero-bias adaptatif : +0,16 fast.
  Le PSNR whole-frame sous-estime le bénéfice anti-tearing (localisé aux
  arêtes en mouvement).
- Interpolation ajoute ~8,4 ms de hold ; extrapolation ~0,1 ms (preset
  `latency`).

## 11. Axes d'amélioration

**Qualité du flow (le levier principal)**
1. **Côté synthèse** : gestion des occlusions dans `interpolate` (exclusion /
   confidence par direction de warp) — identifié comme le prochain levier après
   les mesures census/grad.
2. Matcher invariant aux rotations/zoom (modèle affine local par bloc).
3. Métrique d'éval locale (zones en mouvement / arêtes) — le PSNR global
   masque le tearing ; une métrique ciblée permettrait enfin de trancher
   grad/census au lieu de les shipper opt-in.
4. Régression qualité automatisée : runner CI auto-hébergé avec GPU tournant
   `run_eval.sh` à chaque push (ou nuit), traquer les dB.
5. Test hôte du block matcher (référence CPU des coûts + tie-breaking dans
   `tests/`) — aurait attrapé la régression `0549e44` sans GPU.

**Produit Windows capture (mode 2)**
6. Phase 3 restante : hotkey global, profils par jeu, tray, fallback Desktop
   Duplication, HDR/VRR, multi-GPU, borderless plein écran, installateur +
   signing Authenticode, passe Input robuste (le `--fit` et les clics via
   `WS_EX_TRANSPARENT` à durcir).
7. Évaluer la qualité phase 2 sur hardware : flow vs build Linux, régime bas
   fps (x2 à 30 fps), multipliers x3/x4, interpolation vs extrapolation.
8. Mesurer la latence réelle de la chaîne capture (timestamps
   arrivée→présentation) au lieu du ressenti.

**Windows mode 1 (layer)**
9. Validation hardware complète (checklist §3 de `docs/windows-port.md`) —
   le DLL est compilé en CI mais jamais lancé sur Windows.
10. Forward de `VK_EXT_full_screen_exclusive` à travers la swapchain
    virtualisée ; build 32-bit + `WOW6432Node` ; outil de config Windows (la
    GUI GTK est Linux-only) ; installateur.

**Plateforme / infra**
11. Packaging Linux (paquets distro / Flatpak pour la GUI) ; polish de
    `windows-README.md` (section Windows dans le README principal).
12. Piste phase 4 (optionnelle, fragile) : present-hook injection D3D11/12 en
    mode low-latency opt-in par jeu solo — jamais par défaut (anti-cheat).

---

*Maintenir ce fichier à jour quand l'architecture ou les invariants changent.*
