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
CAS + HUD fps en option. Projet C++20 / Vulkan / CMake, shaders GLSL→SPIR-V
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
  pacing), `present_slice.{h,cpp}` (réindexation des tableaux `pNext` lors
  d'un present multi-swapchain), `framegen.cpp` (orchestration du pipeline), `config.cpp`,
  `device.cpp`, `platform.cpp` (shims Linux/Windows), `format.cpp`, `log.cpp`.
- `layer/shaders/` — `downsample`, `block_match`, `flow_smooth`,
  `flow_refine`, `reduce_cost`, `interpolate`, `upscale`, `cas`,
  `common.glsl`. Embrasés en SPIR-V au build (`cmake/EmbedShader.cmake`,
  cibles `*_f32` / `f16` / `u32` / `u64` selon le format de stockage luma).
- `capture/src/` — `capture.cpp` (WGC, frame pool, ring D3D11), `vkctx.cpp`
  (interop D3D11→Vulkan + pilote `FrameGen` + swapchain overlay), `launcher.cpp`
  (fenêtre de config native Win32), `main.cpp`.
- `tools/offline/bdex_offline.cpp` — le moteur `FrameGen` sans fenêtre ni layer
  sur une séquence PPM (1 frame sur `multiplier` en entrée, les autres = vérité
  terrain) ; piloté par `tools/eval_sequence.py` (PSNR global / mouvement /
  arêtes + références répétition et fondu, A/B `--variant`). Reproduit
  exactement les frames du layer en ligne (hors compteur de la démo).
- `demo/` — démo Vulkan (scène animée) servant de cobaye pour l'éval et les
  tests ; `tools/run_eval.sh` + `tools/eval_quality.py` (PSNR vs référence).
- `tools/` — GUI de contrôle GTK4 (`bdex-framegen-gui`, Linux), `install.sh` /
  `uninstall.sh` (x64 + x86), scripts PowerShell d'enregistrement du layer.
- `tests/` — tests unitaires (config, formats), alignement de l'évaluateur,
  réindexation des métadonnées de present sur deux swapchains,
  `integration.sh` (démo 30 fps à travers la couche, vérifie le ratio x2 et
  "generation on") et `test_acquire.cpp` (délais nuls/finis après épuisement
  des images virtuelles, puis réveil à la libération).
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
ctest --test-dir build --output-on-failure              # unit + éval + intégration GPU
tests/integration.sh build/demo/bdex_demo build/layer   # e2e GPU (X11/Wayland)
tools/run_eval.sh                                       # éval qualité (PSNR)
tools/make_corpus.py DIR                                # corpus synthétique (HUD, panoramique doux, sous-pixel, fondu)
tools/tune.py SEQ...                                    # réglage automatique (descente par coordonnées)
bdex-framegen --toggle | --report                       # pause à chaud (raccourci) | rapport de diagnostic
tools/eval_sequence.py <dir_ppm> --variant KEY=VAL     # éval hors ligne (bdex_offline, sans fenêtre) sur séquence enregistrée
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
`render_scale < 1`) → `cas` (sharpening) → HUD fps. Entre l'analyse et le flow, la passe d'état
`frame_state` (seulement avec `frame_timing` / `dedup`) calcule t et
les répétitions sur GPU. En extrapolation avec
`extrapolate_splat`, une passe `splat` projette d'abord les pixels vers leur
position future (carte `r32ui`, `atomicMin` priorité|index). Pacing par sommeil quand le
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
(iGPU→performance, dGPU avec ≥2 Gio de VRAM→balanced). Les knobs flow
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
persistées dans la config partagée). **Hotkey global** : Ctrl+Alt+Q quitte,
Ctrl+Alt+G bascule la génération on/off à chaud (`VkCtx::toggleGeneration`,
off = passthrough des frames réelles) ; rebindable via la clé config `hotkey`
(parseur `parseHotkey` dans `main.cpp`). **Profils par jeu** : `bdex_capture`
charge la config avec le nom du process *jeu* ciblé
(`Config::load(extraProcess)`), donc une section `[jeu]` (écrite par la case
« Profil pour ce jeu » du launcher, ou à la main) prime sur les globaux pour
ce titre — le chemin layer reste `Config::load("")`, bit-for-bit inchangé.

Validé sur hardware le 2026-09-15 (laptop Intel iGPU + RTX 3050) : PRAGMATA
(D3D12), Dark Souls III, Skyrim SE (D3D11), Fallout: New Vegas (D3D9) — x2
extrapolation stable. Bugs réels trouvés sur hardware et leur fix (ne pas les
réintroduire) :
- **Flicker** : un descriptor/une image de stage partagés entre frames en vol
  → command buffers par soumission (`VkCtx::nextCmd`, `kCmdSlots`).
- **Double curseur** (jeux qui cachent le curseur matériel) : pas de class
  cursor sur l'overlay, `GetCursorInfo` force la dissimulation.
- **`log_file`** : `%VAR%`/`~` expansés avant ouverture (`platformExpandPath`).
- **Arrêt WGC** : fermer la session, révoquer le callback, puis joindre le
  thread avant de libérer pool/ring ; le callback possède son événement de
  réveil via `shared_ptr`, sans capturer `Capture` ou `Impl`.

## 9. Pièges transverses

- `hide_present_ext=1` (défaut) : cacher `swapchain_maintenance1` / present_id
  au jeu, sinon VKD3D-Proton se bloque (la sémantique vise la vraie swapchain
  qu'on virtualise).
- Les tests d'intégration nécessitent un display (skip propre sinon) ; le test
  e2e exerce la génération sur GPU — le faire tourner après toute modif de
  shader.
- Les images virtuelles conservent tous les bits `imageUsage` demandés par le
  jeu ; si Vulkan ne peut pas créer une image ordinaire compatible, revenir au
  swapchain du pilote. Ne jamais retirer silencieusement un usage demandé.
- `run_eval.sh` ignore le fichier de config utilisateur (`BDEX_FG_CONFIG=/dev/null`)
  mais accepte les overrides `BDEX_FG_*` explicites. L'évaluateur publie le
  nombre de frames comparables, attendues et exportées, puis échoue sous le
  seuil minimal (75 % des frames attendues, arrondi à l'entier inférieur).
  Il conserve les dumps en `uint8` pour limiter la mémoire et mesure aussi les
  pixels révélés par la barre jaune et les arêtes du carré en rotation.
- Capture Windows : la valeur par défaut `extrapolate=1` est posée avant
  lecture du fichier/env (`Config::loadWithDefaults`) ; priorité : défauts <
  fichier < env < preset CLI < options CLI explicites < launcher. Le launcher
  lit le mode effectif, y compris `BDEX_FG_MODE`.
- La CI Windows utilise le SDK Vulkan `1.4.357.0` et contrôle son SHA256 avant
  installation ; elle compile le mode 1 avec MSVC x64/Win32, sans l'exécuter
  sur hardware.
- Les artefacts se diagnostiquent avec `BDEX_FG_DEBUG=flow` (voir le flux) et
  `dump_dir` (PPM), cf. CONTRIBUTING.md.
- CI Linux depuis le 2026-09-21 (`linux.yml`, verte) : unit + intégration
  (dont épuisement des images de swapchain et `VK_NOT_READY`/`VK_TIMEOUT`) +
  gate PSNR sur lavapipe à chaque push (x2 normal/rapide, x3/x4), et
  comparaison bit à bit du chemin par défaut avec `f2cec5f` (vrai pré-census).
  On peut toujours tout lancer à la main (cf. §4). Le seuil PSNR (28,0/23,0)
  est calibré au chiffre lavapipe réel, qui colle au GPU à ~0,04 dB — toute
  régression matcher le fait chuter.

### Session 2026-09-24 — outils, tests et options ajoutés

- **Éval hors ligne** : `bdex_offline` (moteur sans fenêtre sur séquence PPM,
  `--format`, `--render-scale`) + `eval_sequence.py` (PSNR global/mouvement/
  arêtes, SSIM, scintillement, références répétition/fondu, `--variant`,
  `--json`). Reproduit exactement les frames du layer en ligne.
- **Tests ctest** : `matcher` (vrai shader vs modèle C++ du départage ; attrape
  0549e44), `offline`, `corpus` (références `tests/corpus_reference.json`,
  `--update` après un changement voulu), `formats`, `validation` (Khronos +
  sync), plus le job CI `sanitizers` (ASan+UBSan, TSan).
- **Options opt-in (défaut 0 = bit à bit)** : `zero_bias_contrast`,
  `subpixel`, `pixel_vectors`, `static_guard` (gardé par le flow au pixel),
  `temporal_prediction`, `extrapolate_splat` ; **défaut changé** :
  l'extrapolation suit le warp partout (`extrapolate_hold=1` = ancien).
  Produit : `fps_limit`, `target_fps` (multiplicateur dynamique), `paused` +
  rechargement à chaud de la config, profils fournis `layer/profiles.conf`.
- **Push constants** : plage de 96 octets (`kPushSize`), `static_assert` sur
  `InterpPC`/`MatchPC`. Depuis le 2026-09-25 les pools de descripteurs
  s'ouvrent à la demande (`DescriptorPlanner`, enregistré par `makePass`) :
  une passe déclare ses bindings et alloue, plus aucun total à tenir à jour.
- **Bug corrigé** : tables globales jamais détruites (crash à la sortie des
  jeux sous Wayland quand le device n'est pas détruit).
- **Tags** : v0.2.0+ ne sont plus ancêtres de `main` (réécriture d'historique).

### Session 2026-09-24 (suite) — huit axes implémentés

Tous opt-in (défaut = bit à bit, vérifié sur tout le corpus, formats et
x2/x3/x4), sauf le correctif HDR (les chemins SDR restent identiques).
- **HDR** : `encodingForFormat(format, colorSpace)` → `transfer`
  (`TRANSFER_PERCEPTUAL/LINEAR/SCRGB/PQ`) ; scRGB et PQ ramenés en lumière
  linéaire relative au blanc SDR (203 nits) puis `sqrt`, comme le sRGB
  décodé. Une échelle PQ a été mesurée et rejetée (−3 à −10 dB).
  `bdex_offline --colorspace scrgb|hdr10`.
- **Tampon d'état GPU** (`stateBuf_`, `frame_state.comp`, disposition
  `kState*` dans framegen.cpp) : horloge de contenu, `t` par sortie, drapeau
  de répétition, arguments de dispatch indirect. **Aucune relecture CPU avant
  la présentation.** `frame_timing` (t au temps réel), `dedup` (répétitions :
  passes de flow annulées par `vkCmdDispatchIndirect` à 0 groupe, sortie
  réelle remplacée par une extrapolation via `recordUpscaleReal(cur, prev)`,
  qui force `packReal_`). Les formules de t retombent exactement sur i/m à
  cadence régulière (écrites « i/m + correction »).
- **`scene_cut_local`** : grille 8×8 dans `reduce_cost` (groupes 1..64).
- **`upscale_temporal`** (2026-09-25) : `upscale_temporal.comp` écrit
  `taaHist_[h]` (rgba16f, taille écran, ping-pong) une fois par image
  (`recordTemporalUpscale`, appelé par la 1re synthèse ou par
  `recordUpscaleReal` ; `frame_` compté dans `recordAcquireSources`, seul
  appel présent dans tous les chemins, y compris pause). L'image réelle est
  empaquetée 1:1 depuis l'historique ; les synthèses lisent
  `dsInterpTaa_` (historiques précédent/courant) — `interpolate.comp` est
  indépendant de la taille des sources. Mesure : le gain vient surtout de
  la synthèse depuis l'upscale ; l'accumulation sans jitter du jeu et avec
  un flux entier floute (poids 0,5 : −0,6 à −1,6 dB). Test aller-retour du
  flux indispensable (bords : jusqu'à −4 dB sans).
- **SSIM de `eval_sequence.py`** : corrigé le 2026-09-25 (sommes float64) ;
  les SSIM antérieurs sur les replays pleine taille (demoN…) étaient faux.
- **`online_tune`** (2026-09-25) : `self_eval.comp` au début de
  `recordFlow` (avant que le matching ne réécrive le flow) note le flow
  précédent sur l'image qui arrive (prédiction à vitesse constante vs
  « pas de mouvement ») ; sommes en virgule fixe par slot de profilage
  (`evalBuf_`, lues dans `profileBegin` quand le slot est réutilisé, donc
  après sa fence — le mode capture appelle `profileBegin` pour ça).
  `OnlineTune` (partagé dans `DeviceData`, survit aux reconstructions) :
  blocs ABBA de 8 flows, gain **absolu** du ratio (le relatif explosait sur
  les contenus quasi parfaits : +90 % pour une différence invisible).
  Surcharges appliquées dans `effectiveConfig()` *avant* celles de
  `gpu_budget`. `subpixel` n'est candidat qu'en extrapolation (le critère
  est celui de l'extrapolation : +2 dB extrap / −0,9 dB interp sur le même
  panoramique). Cache `online-tune-<jeu>-<mode>.txt` (`Config::tuneName`).
- **`flow_temporal`** (2026-09-25) : passe `flow_temporal.comp` après le flow
  du niveau 0, avant `reduce_cost` ; réutilise la copie `prevBwd_` (comme
  `extrapolate_accel`) et les lumas du niveau 0 (un set par parité). Choix
  vérifié par SAD (ancien vecteur gardé s'il est à ≤ 2 px et à tolérance
  près du SAD du nouveau) ; les vecteurs fractionnaires (`subpixel`) ne sont
  pas touchés. Le mélange pondéré a été essayé et perd 0,5 dB (retard sur
  les vrais changements de mouvement) — ne pas y revenir.
- **`extrapolate_accel`** : copie du flow arrière fin précédent (`prevBwd_`,
  usage TRANSFER_SRC sur le flow du niveau 0) ; estimateur moyenné 3×3
  bilatéral, porte de cohérence, trajectoire entière dans l'image. Chaque
  garde a été mesuré (sans eux : jusqu'à −0,9 dB sur mouvement constant).
- **`chroma_weight`** : pyramide Cb/Cr (`downsample_chroma.comp`), bindings
  6/7 du matcher. Le gagnant historique (réduction en arbre, §6) reste le
  tenant. Perte connue : les rayures quasi isoluminantes qui défilent plus
  vite que leur période (boîte hachurée de la démo, −1,2 dB).
- **`gpu_budget`** : gouverneur `budget.{h,cpp}` (testé), niveaux 1–2 =
  surcharges (`FrameGen::effectiveConfig`), 3–4 = reconstruction avec
  `flowScaleFactor`.
- **GL / gamescope** : `--gl` force aussi le fournisseur Mesa (sinon le GL
  NVIDIA garde le jeu, hors de portée, sans erreur) ; profil fourni
  `[gamescope] hide_present_ext = 0` (backend SDL requis).
- **Outils** : `bdex_offline --jitter/--dup/--spacing/--steady-content/
  --verbose` (chaque frame affichée notée contre ce que le joueur devrait
  voir, modèle de pacing de swapchain.cpp), `eval_sequence.py
  --offline-args` (PSNR plafonné à 60 dB quand les réelles sont notées),
  corpus + `popup`, `swing`, `chroma` ; démo `--repeat N` ; `validation.sh`
  couvre toutes les options (et l'échelle complète du budget).
- `kPushSize` passe à 128 octets (minimum garanti).
- `run_eval.sh` échoue parfois (« 0 comparable ») quand le compositeur
  impose une autre taille de fenêtre : relancer. Les démos rejouées hors
  ligne (copier `~/.cache/bdex-eval/gt/*real*` en `fNNNN.ppm`) sont plus
  rapides et reproductibles pour itérer.

### Session 2026-09-25 — huit axes de plus, rendus génériques

- **Matcher** : sortie anticipée des blocs statiques (bit à bit, −25 à
  −38 % du temps de flow sur la démo) ; `rot_weight` lisait hors de sa
  tuile (déplacement absolu vs tuile centrée sur `base`, et candidat zéro
  hors fenêtre) → non déterministe, corrigé ; `chroma_weight` refuse un
  challenger loin du mouvement de la paire précédente.
- **Profilage** : `bdex_offline` + `BDEX_FG_PROFILE=1` et `--bench N`
  (flow répété, GPU à fréquence stable). Mesuré : lissage + barrière ≈ 0 ms,
  flow en demi-précision inutile (textures ~32 k texels) → non faits.
- **Extrapolation** : la synthèse est soumise par le worker *après* la
  copie/présentation de la frame réelle quand celle-ci est copiée directement
  (`Job::synthSlot`, `submitDeferredSynthesis`) : 3,5 → 1,5 ms (balanced).
- **`dedup` partiel** : grille 8×8 de pixels différents (`tileDiff`) ;
  répétition si ≤ 7 tuiles changées juste après ≥ 32 (HUD 60 i/s sur monde
  30 i/s). `bdex_offline --hud-counter`.
- **Retour d'affichage** (`present_feedback`, défaut 1) : la couche active
  `present_id`/`present_wait` pour elle seule si supportés (réessai sans),
  thread `feedbackLoop` (pause pendant `followSurface`, arrêt avant
  destruction), `DisplayClock` (testé) pour la grille de rafraîchissement.
  Grille valide → pacing calé (0,3 période avant le vblank), `t` sur
  l'affichage prédit (`FrameTiming::displayMs`, `kStateDisplay`, défaut),
  `target_fps`/`fps_limit = auto`. Poste de dev (Hyprland) : pas de grille
  (VRR / compositeur) → rien ne change. `bdex_offline --refresh HZ
  [--refresh-known]` simule un écran fixe : +2 à +15 dB.
- **Capture Windows HDR** (`capture_hdr`, auto) : scRGB FP16 de bout en bout
  si l'écran du jeu est en HDR (DXGI G2084/P2020) et que import FP16 +
  surface scRGB existent ; sinon SDR. Syntaxe vérifiée via MinGW (Docker)
  sauf `capture.cpp` (C++/WinRT, CI Windows seulement).
- **Threads** : chaque swapchain a ses propres pools de commandes (un par
  thread) — le pool unique du device était utilisé par le thread du jeu *et*
  le worker (et crashait le pilote à deux swapchains) ; config en instantané
  par swapchain (`snapshotConfig`), le worker ne lit que le `Job`. Test
  `multithread_config` (deux files requises, sinon 77 — c'est le cas sous
  lavapipe). TSan local inutilisable avec le pilote NVIDIA (SEGV interne).

### CI Linux — pièges (2026-09-24)

- Le loader d'Ubuntu 24.04 (1.3.275) **ignore `VK_IMPLICIT_LAYER_PATH`** (et
  parfois `VK_ADD_IMPLICIT_LAYER_PATH`). Les tests chargent la build voulue en
  copiant son manifeste dans un `XDG_DATA_HOME` privé (`tools/layer_env.sh`,
  `layer_env()` dans `check_matcher_invariant.py`, `test-xdg` généré par
  CMake pour `acquire_timeout`) ; rien n'est installé au niveau système en CI.
- Les étapes `run` sont en `bash -eo pipefail` : `cmd | grep -q` échoue quand
  grep trouve (SIGPIPE). Capturer la sortie puis `grep -q <<<"$out"`.
- **Avant de pousser un changement de CI** : `tools/ci/run.sh` (build) et
  `tools/ci/run.sh asan|tsan` rejouent les jobs dans Docker, même loader,
  lavapipe.

## 10. Décisions & mesures clés (retenir les chiffres)

- Éval qualité historique (2026-09-15, affichage fixé) : baseline **31,0 dB normal /
  23,7 dB fast** (`--speed 2`). La taille imposée par le compositeur change les
  valeurs absolues ; comparer les variantes sur le même affichage. Le régime
  faible = **mouvement rapide**, et
  c'est un problème d'ambiguïté du coût de matching, pas de rayon ni de
  régularisation (`levels=5` byte-identique ; les deux directions de
  `smoothness` dégradent).
- `grad_weight` (09-15) : fast **+0,5 dB**, normal **−2,1 dB** → opt-in.
- census (09-17) : large **+0,45 fast / −1,5 normal** ; gating serré seul
  non-négatif (+0,12 normal) ; soft weight dominé → opt-in.
- Sub-pixel parabolique : −0,1 les deux. Zero-bias adaptatif : +0,16 fast.
  Le PSNR whole-frame sous-estime le bénéfice anti-tearing (localisé aux
  arêtes en mouvement).
- Extrapolation (2026-09-24) : l'ancien repli « pixel incohérent → garder le
  pixel en place » dessinait l'objet à son ancienne position (double image,
  cf. retour Kenshi). Supprimé (`extrapolate_hold=0` par défaut, 1 = ancien) :
  +1,8 dB normal / +1,0 rapide en ligne, zones révélées 7,6 → 20,3 dB.
- `zero_bias` (0,002 absolu, non normalisé par le contraste) est la cause
  principale des échecs en mouvement rapide : sur contenu peu contrasté le
  candidat zéro gratuit gagne → fondu fantôme. `zero_bias=0` : rapide +0,75 dB
  en ligne (rotation +2,8), normal −0,10 ; en extrapolation gain partout
  (+0,3 à +0,5). Défaut non changé (décision utilisateur, déplace la référence
  CI). `temporal_prediction=1` n'est utile qu'avec un `zero_bias` faible
  (panoramique 64 px/paire : 23,5 → 36,0 dB).
- - Interpolation ajoute ~8,4 ms de hold ; extrapolation ~0,1 ms (preset
  `latency`).
- L'évaluation accepte x2/x3/x4 et rapporte le PSNR des zones changeantes et
  des arêtes en mouvement (en excluant le compteur de frames des masques
  locaux). `occlusion_strength` est un essai opt-in de
  cohérence aller/retour dans la synthèse ; défaut 0. Sur Radeon Vega 8,
  x2/40 frames, force 1 n'a pas amélioré le PSNR normal/fast (28,63/22,04 dB
  dans les deux cas) ; arêtes en mouvement : −0,01 dB dans les deux cas. Sur
  llvmpipe à affichage fixe, x2/12 frames : 31,91/23,64 dB normal/fast,
  inchangés avec force 1. Un
  poids plus agressif fondé sur le désaccord des deux candidats a perdu
  0,10 dB normal sous Xvfb/llvmpipe (12 frames), sans gain sur les arêtes
  rapides ; essai abandonné.
- Nouveau prototype `occlusion_strength=1` : réduit aussi le repli en fondu
  simple quand les deux trajectoires ont un coût élevé. Radeon Vega 8,
  x2/30 frames (26 alignées), défaut→option : normal 29,35→29,43 dB global,
  19,97→20,16 dB arêtes, 25,88→26,69 dB zones révélées, 21,94→22,18 dB
  rotation ; rapide (`--speed 2`) 22,12→22,15 / 15,35→15,38 /
  19,62→19,95 / 13,88→13,95 dB. Coût GPU interp bruité, environ 7–16 ms
  défaut / 9–15 ms option sur cet iGPU ; défaut laissé à 0. 38 dumps du
  chemin à 0 sont identiques octet pour octet au shader précédent.

## 11. Axes d'amélioration

**Qualité du flow (le levier principal)**
1. **Côté synthèse** : valider le prototype opt-in d'occlusion sur GPU stable
   et sur jeux réels ; une gestion explicite de visibilité par direction reste
   à explorer après les mesures census/grad.
2. Matcher invariant aux rotations/zoom (modèle affine local par bloc).
3. Étendre les métriques locales existantes (zones révélées / rotation) à des
   scènes plus variées ; le PSNR global masque encore le tearing.
4. Régression qualité automatisée : runner CI auto-hébergé avec GPU tournant
   `run_eval.sh` à chaque push (ou nuit), traquer les dB.
5. Test hôte du block matcher (référence CPU des coûts + tie-breaking dans
   `tests/`) — aurait attrapé la régression `0549e44` sans GPU.

**Produit Windows capture (mode 2)**
6. Phase 3 restante : tray, fallback Desktop Duplication, HDR/VRR, multi-GPU,
   borderless plein écran, installateur + signing Authenticode, passe Input
   robuste (le `--fit` et les clics via `WS_EX_TRANSPARENT` à durcir). Faits :
   hotkey global (toggle) et profils par jeu.
7. Évaluer la qualité phase 2 sur hardware : flow vs build Linux, régime bas
   fps (x2 à 30 fps), multipliers x3/x4, interpolation vs extrapolation.
8. Compléter la mesure de latence de la chaîne capture : le log actuel mesure
   la sortie du pool WGC jusqu'au retour de `vkQueuePresentKHR`, mais pas le
   délai entre la présentation du jeu et WGC, ni le balayage écran.

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
