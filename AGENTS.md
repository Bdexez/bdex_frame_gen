# AGENTS.md — contexte projet pour l'assistant

Ce fichier est la mémoire de travail du projet : il résume l'architecture, les
commandes, les invariants et les pièges, pour qu'une session ultérieure puisse
reprendre le projet sans re-dérivation. Sources de vérité complémentaires :
**`docs/MEMOIRE.md`** (la mémoire consolidée, lisible par l'utilisateur :
fonctionnement par système, historique, chiffres, problèmes ouverts — à tenir
à jour à chaque session), `README.md` (options, usage Linux), `docs/windows-port.md` (mode 1),
`docs/windows-capture.md` (mode 2, checklists de validation hardware),
`docs/windows-README.md` (usage Windows), `ROADMAP.md` (état et reste à faire),
`docs/corpus-jeux.md` (enregistrer des jeux, trancher les options en attente),
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
| Plateforme | Linux (shipping), Windows (compilé CI, exécuté sous Wine en CI, jamais validé sur hardware) | Windows x64 |
| Latence | ~0 ms (dans le present path) | +~1 frame (inherent à la capture) |
| Anti-cheat | se rétracte si EAC/BattlEye/GameGuard/XIGNCODE3/miHoYo détecté, ou dans un jeu compétitif listé par nom d'exécutable (VAC : CS2, Dota 2… ; `platformIsCompetitiveGame`) | sûr (pas d'injection) |
| Couverture | jeux Vulkan natifs (et Proton via DXVK/VKD3D) | tous les APIs (D3D9/10/11/12, GL, Vulkan) |

Les deux partagent le même moteur : `FrameGen` (`layer/src/framegen.cpp`) et
tous les shaders de `layer/shaders/`.

## 3. Carte du repo

- `layer/src/` — la couche : `layer.cpp` (négociation loader, dispatch,
  `vkGetInstanceProcAddr`), `swapchain.cpp` (virtualisation des swapchains,
  côté thread du jeu : création, acquire, `present`) + `swapchain_worker.cpp`
  (le worker : `runJob`, copies, présentations cadencées, retour d'affichage ;
  pacing), `present_slice.{h,cpp}` (réindexation des tableaux `pNext` lors
  d'un present multi-swapchain), `framegen.cpp` (orchestration du pipeline ;
  `framegen_output.cpp` = étage de sortie de l'image réelle, CAS ;
  `framegen_internal.h` = push constants et disposition du tampon d'état ;
  `descriptor_planner.{h,cpp}` = pools de descripteurs à la demande),
  **`fg_pass.h` + `passes/`** = passes optionnelles (voir ci-dessous),
  `config.cpp` (registre des options),
  `device.cpp`, `platform.cpp` (shims Linux/Windows), `format.cpp`, `log.cpp`.
- `layer/shaders/` — `downsample`, `block_match`, `flow_smooth`,
  `flow_refine`, `reduce_cost`, `interpolate`, `upscale`, `cas`,
  `common.glsl`. Embrasés en SPIR-V au build (`cmake/EmbedShader.cmake`,
  cibles `*_f32` / `f16` / `u32` / `u64` selon le format de stockage luma).
- `capture/linux/` — **`bdex-capture` Linux** (2026-10-03) : `x11_capture.cpp`
  (XComposite redirect automatique + XDamage + XShm, anneau de 3 segments
  alignés page), `presenter.cpp` (équivalent Linux de `VkCtx` : surface Xlib,
  segments importés en mémoire hôte `VK_EXT_external_memory_host` sinon
  copiés, `FrameGen` inchangé, `--gpu` = un autre GPU que le jeu = mode deux
  GPU de LS), `main.cpp` (fenêtre override-redirect sans région d'entrée,
  boucle cadencée comme `capture/src/main.cpp`, raccourcis XGrabKey). Pas
  sous gamescope (sa couche WSI présente hors de la fenêtre X), pas de
  Wayland natif : `--wayland` (`pw_capture.cpp`, portail ScreenCast par
  GDBus + flux PipeWire, overlay sur `--rect`). Test `capture_linux` (Xvfb ;
  GPU : gamescope headless **sans** `ENABLE_GAMESCOPE_WSI`) ;
  `tools/ci/wayland-capture-check.sh` (sway headless, xdpw, PipeWire).
- `capture/src/` — `capture.cpp` (WGC, frame pool, ring D3D11), `vkctx.cpp`
  (interop D3D11→Vulkan + pilote `FrameGen` + swapchain overlay), `launcher.cpp`
  (fenêtre de config native Win32 ; calibration, rapport, retrait des
  injections), `injection.cpp` (inspection du jeu, DXVK/VKD3D-Proton,
  enregistrement de la couche HKCU), `report.cpp`, `main.cpp` (`planRun`).
- `tools/calibrate/` — `bdex-calibrate` (Linux + Windows) : coût du moteur
  sur ce GPU, preset et multiplicateur recommandés.
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
  chaque push ; `distros` (build et démo dans 6 distributions) ; `package`
  (archives x86_64/aarch64, vérifiées par `archive-check.sh` avant release). Pièges runner (cf. §9) : paquet `glslc` (pas `glslc-tools`),
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
bdex-framegen --tuned-profile [JEU] [--apply]           # décisions d'online_tune -> section [JEU] de config
bdex-framegen --record DIR [N] -- <jeu>                 # enregistre N images réelles (corpus)
tools/corpus_pack.py DIR out.tar.gz --name X            # séquence -> archive (release GitHub "corpus", notée en CI)
tools/eval_sequence.py <dir_ppm> --variant KEY=VAL     # éval hors ligne (bdex_offline, sans fenêtre) sur séquence enregistrée
tools/pending_decisions.py SEQ... [--only K,...]       # options en attente notées sur des jeux enregistrés : verdict par option
tools/replay_bench.py SEQ [--multiplier M] [--other-layer NAME]  # éval EN LIGNE : séquence rejouée comme un jeu (bdex_demo --replay) à travers la couche
bdex-framegen --calibrate [--apply]                     # bdex-calibrate : coût sur ce GPU, preset/multiplicateur recommandés
tools/train_flow.py [--write]                           # entraîne learned_flow (numpy, flux vrai de synthèse, bdex_offline --dump-flow)
tools/fetch_translation_layers.py OUT                   # DXVK + VKD3D-Proton épinglés (paquet Windows : injection D3D)
tools/ci/wayland-capture-check.sh                       # bdex-capture --wayland de bout en bout (sway headless, portail wlr, PipeWire)
tools/package-linux.sh [outdir] [version]              # archive Linux précompilée (sur la machine même)
tools/package-native.sh ARCHIVE [outdir] [version]     # .deb + .rpm depuis une archive (dpkg-deb/rpmbuild, sinon Docker)
tools/ci/archive-check.sh PAQUET.deb|.rpm [image...]   # installe le paquet (apt/dnf/zypper) : charge, profils lus, x2
bdex-framegen --sandboxes [--remove]                   # couche copiée dans les lanceurs Flatpak / Snap
bdex-framegen --mangohud -- <jeu>                      # MangoHud sous la couche (sinon au-dessus : i/s du jeu)
bdex-framegen-gui --text                               # panneau en mode texte (automatique sans GTK 4)
nix build .#default .#layer32                          # paquets Nix (flake.nix, packaging/nix/)
tools/ci/package-release.sh [outdir] [version]         # archives de release x86_64 + aarch64 (Docker, comme le job CI `package`)
tools/ci/archive-check.sh ARCHIVE [image|suite...]     # installe une archive ailleurs : charge + génère x2 (vkcube, Xvfb)
tools/ci/distro-check.sh [image...]                    # build + tests + démo dans d'autres distributions (job CI `distros`)
tools/ci/proton-check.sh                               # D3D11 (DXVK) et D3D12 (VKD3D-Proton) sous Wine à travers la couche (lavapipe, Xorg dummy 60 Hz : Xvfb = 0 Hz, DXVK divise par 0)
tools/ci/proton-wayland-check.sh                       # le même sous le pilote Wayland de Wine 11 (WineHQ), weston headless, sans X
tools/ci/wine-layer-check.sh                           # la couche Windows (DLL MinGW) chargée par le chargeur officiel Windows sous Wine : x2
tools/ci/lib32-check.sh                                # couche 32 bits exécutée (vkcube i386)
bdex-capture --list | --pid PID [--gpu N] [-m N]        # génération par capture (X11/Xwayland), autre GPU possible
tools/ci/decky-check.sh [outdir]                       # plugin Decky (packaging/decky) : tsc + rollup (Docker Node), zip
tools/ci/fuzz.sh [secondes] [cible...]                # libFuzzer + ASan/UBSan sur les analyseurs (config, statut, options_ui, cache online_tune) ; entrée fautive -> tests/fuzz/corpus/
tools/ci/panel-check.sh build/options_ui.json [image]  # panneau sur écran virtuel en conteneur (versions SteamOS : Arch)
bdex-framegen --always-on [JEU] | --always-off [JEU]   # générer sans le panneau (mode Jeu SteamOS)
bdex-framegen --gpu screen|discrete|integrated|list -- <jeu>  # portable hybride (pas de copie PRIME)
tools/ci/flatpak-check.sh                              # extension Flatpak (packaging/flatpak) : build + loader du runtime
tools/ci/srpm-check.sh | debsrc-check.sh | gentoo-check.sh | void-check.sh  # paquets source (job `source-packages`)
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

**Pacing (depuis le 2026-09-29) — l'origine est la fin GPU, pas l'appel du
jeu.** Mesuré : caler les images sur l'heure de `vkQueuePresentKHR` du jeu
faisait perdre *toutes* les images générées dès que la synthèse finissait
plus d'une demi-période après cet appel (jeu limité par le GPU) — générée et
réelle prêtes ensemble, mailbox garde la seconde. Le worker (`runJob`) :
interpolation = toutes les copies soumises d'emblée (le thread du jeu
n'attend que leur *soumission* : `markSubmitted` tôt), attente de la fence
du slot de synthèse (`waitGpu`), puis seuls les `vkQueuePresentKHR` sont
temporisés (`paceTarget` : origine + I·k/m, espacement ≥ ½ créneau avec la
présentation précédente, grille d'affichage si connue). Extrapolation =
réelle tout de suite, puis prédites paresseuses calées sur la fence de la
copie réelle. **Abandon des prédites** (depuis le 2026-09-29, 2e passe) :
seulement quand la *première image du present suivant est prête sur le GPU*
(`Job::ready` = fence du slot de synthèse, ou `FrameSlot::readyFence` de la
soumission vide d'une extrapolation différée ; `predictedCancelled` /
`waitUntilOrCancelled` la surveillent) — pas à l'appel de `present()` :
un jeu limité par le GPU appelle `present(N+1)` une image avant la fin de
son rendu, et toutes les prédites tombaient (hybride ×3 : 116 i/s affichées
au lieu de 283). Les prédites de l'hybride (et de l'extrapolation empaquetée)
passent par les **images de transit** dès le début du job, qui est alors
« soumis » : `present(N+1)` n'attend plus le cadencement de la réelle (le jeu
tombait de 95 à 58 i/s). Extrapolation différée : prédites copiées à leur
heure depuis les sorties (`lazyOutputs_`) ; si la synthèse suivante n'est
pas différée, `present()` lève `cancelPredicted_` et attend. **Deux files** par
défaut (`present_queue`, `DeviceData::splitQueue`) : synthèse sur
`computeQueue` (2e file de la même famille), copies + présentations sur
`queue` — le blit de présentation (X11/Xwayland) attendait sinon la synthèse
suivante. Le signal d'acquisition du jeu part sur la file de synthèse
(garde l'ordre GPU « synthèse N avant l'image N+1 du jeu »).
**Ordre des images** (`frame_order.h`, `FrameOrder`) : m positions par
image du jeu, la réelle en `real` (m−1 interpolation, 0 extrapolation,
1 **hybride**), sortie o en position o avant la réelle (t ∈ (0,1)) ou o+1
après (t ∈ (1,2)) ; une seule définition pour la couche, la capture et
`bdex_offline`, écrite pour redonner i/m et 1+i/m bit à bit.
`frame_state.comp` reçoit `realPos` (interp/extrap décidé par sortie). Le
worker : positions ≤ real copiées d'emblée, prédites après la réelle
paresseuses et annulables (`Job::realIndex`).
**Jusqu'à ×8** (`fg::kMaxFrames = 8`) : état `t[8]` (la réelle en `[7]`,
`kStateHead = 64`, décalages suivants +16), `displayMs[8]`, `synthDone[8]`,
16 slots worker (`kProfileSlots = 20`). La swapchain réelle vise
`minImageCount + m − 1` images, mais une surface peut en plafonner le
nombre (gamescope : 8 max pour min 3 → 6 acquises à la fois) : les images
au-delà d'`acquireAhead_` passent par des **images de transit**
(`FrameGen::ensureStaging/recordStage/recordCopyStaged`, allouées par
`prepareStaging` à chaque reconstruction) copiées d'emblée, puis vers la
swapchain à leur présentation — sinon le jeu attendait le pacing (×8 :
6,8 i/s au lieu de 30).
**Adaptatif** (`adaptive`, 2026-09-29) : `chooseMultiplier` accumule
I/P (P = 1000/cible, cible = `target_fps` ou la fréquence de l'écran) et
rend n = partie entière (1..multiplier), le reste reporté ; `present()`
donne alors les temps d'affichage nominaux (k·P, sur la grille si connue)
à `frame_state` (t par temps d'affichage, obligatoire : le i/m fixe serait
faux) et `job.intervalMs = n·P` (le pacing espace de P). 50 i/s → 144 Hz :
144,0 i/s affichées. Même logique dans `VkCtx::synthesize` (capture), avec
la fréquence du moniteur de l'overlay (`monitorHz`).
`max_frame_latency` (1 par défaut, seulement en génération) : `present()`
attend, hors `mutex_`, la fence du slot de l'image N−1. Puis
**`latency_limiter`** (défaut 1, `syncFrameStart`) : la durée de cette
attente W = l'avance du jeu sur le GPU ; retenue += ½·(W − marge), divisée
par 2 dès que W ≈ 0 (GPU peut-être au repos), plafond ¾·I, sommeil puis
attente active ½ ms ; la marge suit la gigue (×1,5 à chaque retard, ×0,99
sinon, plancher 1 ms + 5 % de I). Mesuré 4K lourd non plafonné, machine
calme : image→écran 26,8 → 18,3 ms, 97 → 101 i/s ; ne retenir qu'une
fraction de W est pire (0,6 : 20,9 ms ; 0,3 : 24,8) ; CPU occupé ailleurs
(réplique CI en parallèle) : −6 % d'i/s. Léger et plafonné : identiques.
Le cas « jeu limité par son rendu » (`--gpu-load`) n'est pas jugeable :
gamescope bloque le jeu dans `present` et le limiteur ne retient rien. Stats :
`next frame held N ms`. `frame-to-screen` part du retour de `present()`,
donc un jeu qui a son propre limiteur (sommeil après `present`) y compte
ce sommeil. Banc sans fenêtre :
gamescope `--backend headless` a le retour d'affichage (present_wait) mais
tourne sans `CAP_SYS_NICE` (compositeur affamé par nos longs dispatchs) — le
cas « lourd » de référence est 4K preset quality + pixel_vectors + bicubic à
66 i/s (GPU ~62 %), stats `displayed N fps` / `replaced before display`.
`bdex_demo --gpu-load N` (N dessins de la scène par image) simule un jeu
limité par son propre rendu — mais alors gamescope headless compose au
rythme du jeu (affiché = FPS du jeu, quoi qu'on fasse, même sans
génération) : ce cas ne se juge pas côté affichage sur ce banc. Seul le cas
où **notre** synthèse domine (démo non plafonnée `--fps 1000`, réglages
lourds, 4K) laisse gamescope composer à ~200 Hz.

**Latence, 2026-10-02.** `interp_hold` (défaut 1, bit à bit) : en
interpolation, les positions jusqu'à la réelle espacées de h/m
(`FrameOrder::offset`, champ `Job::order`, `paceTarget`), `t` = 1 −
(real − p)·h/m (le contenu reste à distance constante de l'affichage),
bornée à un rafraîchissement d'écart (`displayHz`) ; seule la couche
l'applique (`FrameOrder::of(cfg, m, withHold)`), pas la capture ni
`bdex_offline`, ni l'adaptatif. `latency_limiter` s'efface si le jeu a
Reflex/Anti-Lag actif (`DeviceData::appPacesLatency`, hooks
`vkSetLatencySleepModeNV`/`vkLatencySleepNV`/`vkAntiLagUpdateAMD`).
`synth_priority` : priorité globale haute de la file de synthèse quand sa
famille est à elle (`async_compute`), repli sur `VK_ERROR_NOT_PERMITTED`.
Les décisions du cadencement sont des fonctions pures de `pacing.h`
(testées par `tests/test_pacing.cpp`, simulateur) : y mettre toute nouvelle
règle de cadencement, avec son scénario. **`vkCreateDevice` réessayé : remettre `chain->u.pLayerInfo` au lien
suivant avant chaque essai** (les couches du dessous l'avancent ; sinon
plantage sous la couche WSI de gamescope).

### Passes optionnelles (`fg_pass.h`, depuis le 2026-09-25)

Une fonctionnalité opt-in qui ajoute du travail GPU est une classe `FgPass`
dans `layer/src/passes/` : `create`/`destroy` + hooks (`frameBegin`,
`slotBegin`, `analysisDone`, `flowBegin`, `overrideConfig`,
`needsPreviousFlow`, `fineFlow`, `synthesisInputs`, `realOutput`). Elle ne
voit le moteur qu'à travers `FgContext`. Ajout = le fichier, une ligne dans
`passes/registry.cpp` (ordre = ordre des hooks ; `online_tune` en premier,
il note le flux avant qu'on le réécrive), la source dans
`BDEX_ENGINE_SOURCES` (`layer/CMakeLists.txt`, liste unique pour la couche,
`bdex_offline` et `bdex_capture`), l'option dans le registre de `config.cpp`.
Migrées : `flow_temporal`, `online_tune`, `upscale_temporal`,
`extrapolate_accel`, `extrapolate_splat` (hook `beforeSynthesis` →
`SynthesisInputs`, carte via `setSplatMap`), le HUD fps (`hud_overlay.comp`,
hook `afterOutput`, dessiné sur l'image finale — donc plus accentué par
CAS). `dedup` reste dans `FrameGen` : c'est le contrôle du pipeline (passes
de flux annulées en dispatch indirect, tampon d'état partagé avec
`frame_timing`), pas une passe ajoutée. Nouvelle :
`subpixel_lk` (Lucas-Kanade), `reveal_fill` (extrapolation : source la plus
lente quand le point fixe oscille ET que la trajectoire est incohérente —
sans ce second test, la rotation de la démo perdait 0,4 dB ; en
interpolation, un test de visibilité par trajectoire a perdu 1 dB dans les
zones d'occultation, `pixel_vectors` y gagne +1,3 : ne pas y revenir),
`hud_mask` (mesuré 2026-09-25 : le masque dans l'accumulation
d'`upscale_temporal` — historique sans mouvement sur le HUD — ne change
rien, HUD opaque ou transparent : le test aller-retour du flux rejette déjà
ces pixels, et l'upscale ne peut pas rendre le HUD plus net que sa
résolution de rendu ; seul le masque dans la synthèse compte : +0,20 dB
sur la scène HUD à l'échelle 0,75, masque appris : n'apprend que
sous mouvement de caméra — 25 % des pixels changés ; à 1 %, la démo à
caméra fixe perdait 0,3 dB en extrapolation). La synthèse a un binding 8
« masque statique » générique (`FgContext::setStaticMask`, 1×1 à zéro
sinon).
**`online_tune` — sonde de synthèse** (`passes/synthesis_probe.*`) : note
les options de synthèse sur une image que le jeu a rendue et que la
synthèse n'a pas vue (interpolation : B reconstruite de A et C, flux
composés ; extrapolation : C prédite de A et B). Erreur en RMS (le L1 ne
voyait pas les gains concentrés, cf. `static_guard` / HUD). `static_guard`
exclu (l'écart de deux images amplifie son défaut connu). Stabilité :
dénominateur plancher 0,05, et un gain de prédiction ≥ 0,05 passe outre
(caméra qui accélère). Redémarrage sur changement durable de mouvement ×3
ou de luminosité ±0,2 (`OnlineTune::observe`). Mesure clé : en interpolation le sous-pixel
(LK ou parabolique) ne paie qu'avec `synth_bicubic` — c'est le bilinéaire
de la synthèse qui floute, pas la précision du vecteur. 

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

**Depuis 0.6, `zero_bias_cap=0.006` est le défaut** (le biais cesse de
croître au-delà de 3 px : panoramique +2,7 dB, balayage +2,4, occlusions
+0,7 ; un bonus *constant* (plafond 0,002) gagnait davantage sur le très
rapide mais perdait 0,14 dB sur la démo `--speed 2`). Branche uniforme dans
la variante allégée, l'expression historique dans le `else` : `cap=0` est
identique bit à bit (vérifié sur NVIDIA ; l'invariant CI le fixe à 0).
`scene_cut_hist=0.05` (défaut) : la réduction du coût (`reduce_cost.comp`)
ramène le coût moyen vers `scene_cut_low` tant que l'histogramme de luma
(`scene_hist.comp`, EMD 1-D sur un niveau grossier) n'a pas bougé — coupures
≥ 0,18, grain / mouvement < 0,02 ; placée là pour ne pas toucher la variante
allégée de la synthèse (le contenu normal reste identique bit à bit).
`scene_cut_local` (auto sur GPU dédié, retiré avec `pixel_vectors` au-delà
de 4K ×2 : il impose la variante complète de la synthèse) : `scene_hist`
calcule aussi un EMD par tuile (`tileHist`, `kStateTileHist = 1280`), et
chaque tuile exige *le sien* à **4×** le seuil de l'image (le mouvement
fait entrer et sortir du contenu d'une tuile : au seuil de l'image, démo
très rapide −0,18 dB ; ×2 −0,03 ; ×4 0,00 ; menu surgissant +1,40 dans
tous les cas).
**Depuis 0.4.0, `zero_bias_contrast=0.02` est le défaut** (A/B hors ligne :
jamais < −0,01 dB, gratuit en GPU) ; le « chemin historique » de l'invariant
est donc `zero_bias_contrast=0` (posé par `check_matcher_invariant.py`).
`flow_temporal` a été écarté des défauts : +0,28 ms de flow en 1440p
(`quality`) pour +0,1 dB.

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
(ou section absente) ; `BDEX_FG=1` l'active. **`require_app=1`** (écrit par la GUI avec
`enabled=1`, 2026-09-26, demande utilisateur : « exactement le même système
que sous Windows ») : chaque jeu Vulkan est **armé** (swapchain virtuelle,
images passées telles quelles, `paused` + HUD coupé) sauf programmes de
bureau (`platformIsNonGame`, GTK 4 via `platformIsDesktopApp` — GTK 4 dessine
en Vulkan) ; la GUI sélectionne un jeu détecté (`/proc/*/maps` contenant la
couche) et « Démarrer » écrit `app.session` = « pid panneau pid cible »
(`platformAppSession` ; heartbeat : la GUI réécrit le fichier chaque
seconde, plus vieux que 5 s = panneau fermé). **Dossier de session/état
Linux : `~/.cache/bdex-framegen/run`, pas `$XDG_RUNTIME_DIR`** — le conteneur
Steam (pressure-vessel, tous les jeux Proton) partage le home mais seulement
quelques sockets de `/run/user/<uid>` (vérifié : PRAGMATA ne pouvait pas
démarrer) ; le PID namespace, lui, est partagé par défaut (pids identiques).
Test en conteneur : `SteamLinuxRuntime_sniper/run -- <binaire>` (fournir
`libglfw.so.3` via `LD_LIBRARY_PATH` pour la démo).
`pollConfig` suit la cible ; les réglages qui dimensionnent l'étage de
sortie (`sameOutputSettings`) incrémentent `Config::rebuildGen` et la
swapchain reconstruit `FrameGen` (`rebuildOutput`, comme `applyGpuBudget`) ;
les réglages de création (`keepingCreationSettings` : render_scale, present
mode…) restent ceux du lancement — render_scale seulement si le panneau
était ouvert au lancement du jeu. Images réelles dimensionnées pour
x4 (2 + 4) pour permettre x4 à chaud. Hors `require_app`, chemin inchangé. `BDEX_FG=1` contourne. Presets : `enhanced` (balanced + opt-ins mesurés positifs ensemble, dont
`zero_bias_contrast=0.02` — à 0,1 il perd 1,2 dB sur le replay de la démo
avec `pixel_vectors`), `balanced` (défaut),
`quality` (flow_scale 1, levels 5, iterations 2), `performance` (flow_scale 4,
refine_all 0, iterations 0), `latency` (extrapolation + x2), `auto`
(iGPU→performance, dGPU avec ≥2 Gio de VRAM→balanced). Les knobs flow
avisés (`levels`, `search*`, `refine*`, `flow_scale`, `flow_iterations`)
désactivent l'auto-tune. Voir `Config` dans `layer/src/config.h` pour la liste
complète (multiplier 1-8, render_scale, upscale_filter, sharpness, scene cut
0.05/0.09, overlay, debug=flow|split|passthrough, dump PPM…).

**Ajouter une option** (depuis le 2026-09-25) : le membre dans `Config`,
puis **une ligne** dans le registre `registry()` de `config.cpp` (`B`/`I`/`F` :
clé, alias, bornes, `kLive` si relue à chaque image sans dimensionner de
ressource, colonne `Tune` si `online_tune` peut l'essayer). Une syntaxe
spéciale va dans `Config::apply` + `kCustomKeys`. Pour qu'elle apparaisse
dans la GUI GTK **et** le launcher Windows : une ligne dans
`layer/options_ui.conf` (libellé, aide, surfaces ; type, bornes et défaut
viennent du registre) **et sa traduction dans `layer/options_ui.en.conf`**
(le build échoue sinon). Langue des interfaces (2026-10-03) : français si la
langue du système l'est (`LANGUAGE`/`LC_ALL`/`LC_MESSAGES`/`LANG`, Windows :
`GetUserDefaultUILanguage`), anglais sinon, `BDEX_LANG=fr|en` force ; panneau :
`tr()` + table `_EN` (gabarits `{}` pour les f-strings, rendus par motif),
launcher : `T(fr, en)`, `statusDescribe(f, english)`. La GUI lit `options_ui.json` (généré par
`tools/options/bdex_options` au build, installé dans share/bdex-framegen) ;
le launcher embarque le `.conf` (`options_ui_conf.h`, octets). Le build
échoue sur une clé inconnue du `.conf` ; le test `option_keys` exige la clé
dans le tableau du README.

## 8. Produit Windows capture — architecture et pièges hardware

Chaîne : `GraphicsCaptureItem` (WGC) → frame pool free-threaded → ring de 3
textures D3D11 `B8G8R8A8` (`SHARED_NTHANDLE | SHARED_KEYEDMUTEX`, régénéré si
la fenêtre grandit) → import Vulkan (`VK_KHR_external_memory_win32` +
`VK_KHR_win32_keyed_mutex`, allocation dédiée, acquire/release chaînés au
submit, barriers de ownership `VK_QUEUE_FAMILY_EXTERNAL`) → `VkCtx` construit
le `DeviceData` du layer sur le device Vulkan de l'app et pilote `FrameGen`
inchangé → swapchain Win32 (mailbox) dans un overlay topmost sans activation
qui suit la fenêtre jeu (`WS_EX_TRANSPARENT` pour laisser passer les clics),
pacing comme `presentOne` (résolution du timer à 1 ms, `timeBeginPeriod`,
et attentes « dormir puis tourner » `sleepUntil`/`frameBefore` : à 15,6 ms,
défaut par process depuis Windows 10 2004, une attente de 16,7 ms finissait
à 31 ms — image générée collée à la réelle, réelle en retard). Sélection GPU discrète par LUID commun D3D/Vulkan
(`--gpu <n|nom>`, clé `gpu`). **Injection ou capture** (2026-10-03, demande
utilisateur, latence) : `method` auto|layer|capture ; auto = couche si
`<pid>.status` montre une swapchain armée sur le GPU choisi, capture sinon
(GPU autre que celui du jeu, anti-triche, jeu non Vulkan) — même règle dans
`plan()` du panneau Linux (qui lance `bdex-capture`) et `chooseInjection`
de `bdex_capture` (`runInjection` = `app.session` réécrit chaque seconde).
Les applis de capture posent `BDEX_FG=1` avant `Config::load` (sinon
`require_app` les met en pause). **Windows, depuis le 2026-10-03 : injection
par défaut** (`planRun`, `capture/src/injection.{h,cpp}`) — couche
enregistrée par le lanceur, DXVK/VKD3D-Proton copiés à côté des jeux D3D
(injectés au prochain lancement, capturés en attendant) ; anti-triche →
capture, **sauf Rocket League** (`platformAntiCheatException`, demande
utilisateur) ; avertissement avant tout lancement sur un jeu avec
anti-triche, sans bloquer. Logiciel **propriétaire** depuis le même jour
(`LICENSE`, `THIRD_PARTY_NOTICES.md` à tenir à jour). Options CLI : `--list/--title/--pid`, `--multiplier`, `--mode`,
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
- **Un test de script qui isole `HOME` doit aussi isoler `XDG_CONFIG_HOME`,
  `XDG_CACHE_HOME`, `XDG_DATA_HOME`** : le poste de dev les définit, et les
  scripts les suivent. Vécu le 2026-10-02 : `uninstall.sh --purge` testé
  avec `HOME` seul a supprimé le vrai `~/.cache/bdex-framegen`
  (`tests/linux_scripts.sh` les isole tous désormais).
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
- Capture Windows : interpolation par défaut depuis le 2026-09-26, comme la
  couche (l'extrapolation à bas fps : beaucoup plus d'artefacts, retour
  utilisateur Linux vs Windows) ; priorité : défauts <
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
- **Tests ctest** : `determinism` (chaque option exécutée deux fois, images
  identiques ; une nouvelle option doit y être ajoutée), `matcher` (vrai shader vs modèle C++ du départage ; attrape
  0549e44), `offline`, `corpus` (références `tests/corpus_reference.json`,
  `--update` après un changement voulu), `formats`, `validation` (Khronos +
  sync), plus le job CI `sanitizers` (ASan+UBSan, TSan).
- **Options opt-in (défaut 0 = bit à bit)** : `zero_bias_contrast`,
  `subpixel`, `pixel_vectors`, `static_guard` (gardé par le flow au pixel),
  `temporal_prediction`, `extrapolate_splat` ; **défaut changé** :
  l'extrapolation suit le warp partout (l'ancien `extrapolate_hold` a été
  retiré le 2026-10-04).
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

### Compatibilité matériel / distributions (2026-09-30)

- **Tests sans fenêtre** (demande utilisateur) : `ctest` complet ouvre la
  démo sur l'écran de l'utilisateur (integration, validation, acquire,
  multithread). En local : `ctest -R 'unit|matcher|eval_quality|offline|
  corpus|formats|determinism|min_limits|option_keys'` directement, les tests
  d'affichage dans `gamescope --backend headless -- env -u WAYLAND_DISPLAY
  ctest -R 'integration|validation|swapchain_stress|acquire|multithread'`, ou Docker.
- **Archives** (`tools/ci/package-release.sh`, job CI `package`) : SPIR-V
  compilé par glslc sous Ubuntu 24.04 (le build testé), couches liées sous
  Ubuntu 20.04 (glibc 2.31, GCC 10 via liens `/usr/local/bin`, CMake Kitware)
  → x86_64 (64 + 32 bits, il faut `gcc-multilib` générique pour
  `asm/errno.h`) et aarch64 en croisé (`tools/ci/aarch64-linux-gnu.cmake`,
  conteneur séparé : multilib et croisé en conflit). Mesuré : glibc 2.29
  (x86_64), 2.30 (arm64), contre 2.36 sous Debian 12 (`arc4random`,
  `_dl_find_object`, pthread 2.34…). `package-linux.sh` échoue au-delà de
  `BDEX_MAX_GLIBC` (readelf -V, toutes architectures). Les dépôts backports
  de Debian 11 sont archivés : ne pas y revenir. `options_ui.json` d'une
  compilation native pour le croisé (`BDEX_OPTIONS_JSON`).
- **Sources** : GCC ≥ 10 (pas de `from_chars` flottant : flux en locale
  classique), en-têtes Vulkan ≥ 1.3.204 (`layer/src/vk_compat.h` : blocs
  EXT_surface/swapchain_maintenance1 recopiés si absents, `bdexNext` pour
  les `pNext` non const de 1.3.239, `switch` entier sur `sType`), glslc
  sinon glslangValidator (directive `GL_GOOGLE_include_directive` ajoutée :
  SPIR-V de glslc identique octet pour octet), `BDEX_SPIRV_DIR`
  (précompilé), `BDEX_VULKAN_INCLUDE_DIR`, pas de loader requis pour la
  couche. `tools/ci/distro-check.sh [image…]` (job CI `distros`) : build
  `-Werror` + tests sans GPU + la démo à travers la couche sur Xvfb +
  lavapipe, dans chaque conteneur (Ubuntu 22.04 : chargeur 1.3.204, ×2,00).
- **Petits GPU** : `checkPassLimits` (framegen.cpp) compare chaque passe aux
  limites (images de stockage / échantillonnées / tampons par étage, taille
  de groupe lue dans le SPIR-V) et lève `VK_ERROR_FEATURE_NOT_PRESENT` avec
  la raison (repli en passage direct). `flow_refine` lit les lumas en
  `sampler2D` (3 images de stockage, garanti 4). Matcher complet (~21 Ko de
  mémoire partagée) seulement si ≥ 32 Ko (`matcherFull_`, sinon variante
  allégée seule + termes optionnels à 0 dans `effectiveConfig`). Test
  `min_limits` (bdex_offline `--limit-shared 16 --limit-storage-images 4`).
- **Flatpak** (Steam, Heroic, Lutris, Bottles) : `packaging/linux/flatpak.sh`
  copie couche + manifestes dans `~/.var/app/<app>/data` (XDG_DATA_HOME du
  bac à sable, même chemin absolu dedans), profils et config ; le panneau
  recopie la config (`mirror_config`), écrit la session dans les dossiers
  de statut des applis (`~/.var/app/<app>/.cache/bdex-framegen/run`, HOME
  du bac à sable avec `--persist=.`) et dans celui du jeu ciblé vu par
  `/proc/<pid>/root` + son environnement, avec son PID interne (dernier
  champ `NSpid`) ; `read_status` lit `<pid interne>.status` par la même vue.
  Simulé avec bwrap (`--unshare-pid`, HOME remplacé) : détection, statut,
  session, config OK. Snap non pris en charge.
- **Archives vérifiées chez l'utilisateur** (`tools/ci/archive-check.sh`,
  job `package`) : `install.sh` puis `vulkaninfo` (charge, device) et
  `vkcube` 12 s sur Xvfb (`generation on` + dernier ratio ≥ 1,9) dans
  Ubuntu 20.04/22.04/24.04, Debian 11/12, Fedora ; aarch64 : arbre Debian
  arm64 (`debootstrap --foreign` + `dpkg-deb -x`, rien d'exécuté dedans)
  lancé par `qemu-aarch64-static -L` dans un conteneur x86_64 — pas de
  binfmt sur l'hôte. Pièges : qemu-user traduit `open()` vers l'arbre mais
  pas `faccessat()` (le loader teste l'existence du manifeste avec) → couche
  installée aux vrais chemins, `uname` factice pour `install.sh` ; le Mesa
  20.3 de Debian 11 exige DRI3 pour présenter (Xvfb n'en a pas) → génération
  non vérifiée là ; Ubuntu 20.04 (focal) *sans* focal-updates n'a pas
  lavapipe ; `vulkaninfo` lancé par chemin complet écrit « USAGE:
  /usr/bin/vulkaninfo ». Mesuré : ×2,00 partout ailleurs, arm64 compris
  (Debian 12 arm64 émulé : 2,6 → 5,2 i/s, tous les shaders compilés par
  lavapipe pour aarch64).
- **Manifeste** : format **1.1.2** + `library_arch` (64/32). Mesuré par
  version du loader : ≤ 1.2.162 (Ubuntu 20.04, Debian 11) avertissent
  « invalid layer manifest file version » pour 1.2.0 *et* 1.2.1 ; ≥ 1.3.239
  appliquent `library_arch` quel que soit le format (plus d'erreur « wrong
  ELF class » pour la couche de l'autre largeur) ; 1.3.204 l'ignore. Ne pas
  remonter le format (`register-layer.ps1` garde 1.2.0 : loaders Windows
  récents).
- **musl** (Alpine) : le crash de la démo et de `bdex_offline` venait de
  lavapipe — LLVM compile nos shaders dans *ses* threads, 128 Ko de pile par
  défaut sous musl (gdb : récursion `SimplifyDemandedBits`). Nos threads
  n'y sont pour rien (essai de threads à 8 Mo : crash identique, retiré).
  `distro-check.sh` précharge sous musl une bibliothèque qui appelle
  `pthread_setattr_default_np` (8 Mo) : ×2,00. Les pilotes GPU ne compilent
  pas avec LLVM ; ne rien changer dans la couche pour ça.

### Paquets de distribution, Nix, panneau texte (2026-09-30, suite)

- **`.deb` / `.rpm`** (`tools/package-native.sh`, job CI `package`, joints
  aux releases) : refaits depuis les archives (même glibc 2.29, runtime C++
  statique) ; couche dans le dossier de bibliothèque de la distribution
  (`lib/x86_64-linux-gnu`, `lib/i386-linux-gnu` ; `lib64`, `lib`). La couche
  cherche ses profils en `../share` puis `../../share`
  (`platformBundledProfilesPath`) ; `BDEX_INSTALL_LIBDIR` pour un build.
  rpm : `AutoReqProv: no` (la couche 32 bits tirerait glibc.i686),
  dépendances riches pour openSUSE. Vérifiés par `archive-check.sh`
  (apt : Ubuntu 20.04–24.04, Debian 12 ; dnf/zypper : Fedora, Tumbleweed).
  Piège : l'archive de `dist/` construite en local (glibc 2.36) ne charge
  pas sous Ubuntu 22.04 — toujours partir de `package-release.sh`.
- **AUR** : `packaging/arch/bdex-framegen-{bin,git}` + `.SRCINFO`
  (`makepkg --printsrcinfo`), non publiés (dépôt privé → sources 404).
- **Nix** : `flake.nix` + `packaging/nix/{package,module}.nix` ; `layer32`
  (pkgsi686Linux) ne garde que lib32, son manifeste et les profils (fichiers
  identiques : pas de collision dans le profil). Test : conteneur
  `nixos/nix` avec un volume `/nix`, sources copiées dans un dépôt git
  (un flake ne voit que les fichiers suivis).
- **Panneau texte** (`text_main` dans `bdex-framegen-gui`) : sans GTK 4,
  `_Stub` remplace Gtk/Gio/GLib… pour que les classes restent définies ;
  `display_hz` lit xrandr. `panel-check.sh` le pilote par `script` (pty).
- **Snap** : `flatpak.sh` et `sandbox_dirs()` (panneau) / `flatpak_dirs`
  (CLI) couvrent `~/snap/steam/current` ; non vérifié sur un vrai snap.
- **Ordre des couches** : le loader place MangoHud au-dessus de nous
  (mesuré, Arch) ; `VK_INSTANCE_LAYERS=ours:MangoHud` le met dessous ;
  `VK_LOADER_LAYERS_ENABLE` ne change pas l'ordre. vkcube est exclu (liste
  des non-jeux) : le copier sous un autre nom pour un test d'armement.

### SteamOS / gamescope (2026-09-30)

- **SteamOS 3.9.2** (liste des paquets : `steamdeck-images.steamos.cloud/
  steamdeck/<build>/*.manifest.pacman`) : gamescope 3.16.30, glibc 2.43,
  loader 1.4.350, Mesa 26, python 3.14 + python-gobject 3.56 + gtk4 4.22,
  **ni libadwaita ni zenity** → panneau en GTK 4 simple. Vérifié par
  `tools/ci/panel-check.sh` (Arch = mêmes versions ; + Debian 12, Ubuntu
  22.04), Xvfb dans le conteneur.
- **Mode Jeu** : une seule application à la fois → le modèle « Démarrer »
  (`require_app`) y est impossible. `require_app = 0` dans `[jeu]` (ou `[*]`)
  = génère sans le panneau ; panneau « Mode Jeu — sans le panneau »,
  `bdex-framegen --always-on/--always-off [JEU]`, ou `BDEX_FG=1 %command%`.
  `Config::processNamed` (section du processus qui touche `enabled` /
  `require_app`, ou `BDEX_FG=1`) ; sinon, activé globalement, les non-jeux
  sont exclus comme sous `require_app` (gamescope — le compositeur du mode
  Jeu charge la couche —, gamescope-wl, mangoapp, steam...).
- **Couche WSI de gamescope** (`VkLayer_FROG_gamescope_wsi`, activée par
  `ENABLE_GAMESCOPE_WSI=1` que gamescope pose pour ses enfants) : *sous* la
  nôtre (`~/.local/share` avant `/usr/share`). Elle répond elle-même à
  `vkGetRefreshCycleDurationGOOGLE` (vkroots rend son wrapper même sans
  l'extension activée) : **ne pas activer `VK_GOOGLE_display_timing`**, elle
  la transmet au pilote (NVIDIA ne l'a pas). `DeviceData::gamescope`,
  `pollGamescope` (worker, après chaque job, ≤ 10/s, tout de suite si le
  limiteur bascule).
- **Limite de Steam** (menu rapide) : `g_nSteamCompMgrTargetFPS`, callbacks
  retenus (1 vblank sur rafraîchissement/limite), état 1 dans
  `GAMESCOPE_LIMITER_FILE` (uint32, toujours écrit) et le protocole
  `gamescope_limiter` ; FIFO imposé côté gamescope aux applis ordinaires ;
  aux « frame limiter aware » (DXVK ≥ 2.3, vkd3d ≥ 2.12,
  `GAMESCOPE_WSI_FRAME_LIMITER_AWARE=1`) FIFO seul exposé + OUT_OF_DATE à
  chaque bascule. `refresh_cycle` = celui de la limite. **Bug gamescope** :
  il n'envoie le cycle d'une surface que s'il change → une swapchain
  recréée garde le défaut de la couche WSI (16,67 ms) → cycle retenu par
  surface (`DeviceData::gamescopeCycles`).
- Sous la limite : multiplicateur adaptatif visant la limite (quel que soit
  le multiplicateur), le worker présente une image une fois la précédente
  affichée (`waitDisplayed`, `fbDoneId_`), un jeu sans image générée attend
  l'affichage de la précédente (`gateOnDisplay`), l'adaptatif prend le temps
  propre du jeu (`gameOwnMs_`) s'il attend > ¼ d'image. Mesures (démo 30
  i/s ×2, écran 60 Hz, limite 30) : avant 15 i/s, 233 ms ; après 30 i/s,
  19 ms (aware) / 66 ms (natif) ; 20 i/s sous 30 → 30 ; 40 sous 60 → 60.
  Pièges vécus : attendre l'affichage de *tout* le job en générant fait
  tomber un jeu de 40 à 31 i/s (cercle vicieux avec l'adaptatif) ;
  l'horloge d'affichage mesurée prend 30 Hz pour un écran 60 Hz présenté un
  vblank sur deux (d'où le cycle de gamescope comme indice).
- **Rattrapage** (partout) : `catchUp` si le jeu a attendu le worker > ¼
  d'image (hors `gateOnDisplay`) ou après 8 jobs d'affilée en retard d'un
  rafraîchissement (1re image du job vs le 1er vblank atteignable une fois
  prête, `origin + 0,3 P` ; ou job commencé ≥ 0,9 P après sa remise). Ne pas
  mesurer le retard sur l'image réelle vs `origin + slot` : faux positifs
  quand la synthèse finit juste après un vblank (1 à 3 images sautées / 2 s).
- **Banc** : `gamescope --backend headless -r 60` + `gamescopectl
  debug_set_fps_limit N` (0 = retirée) lancé *dans* l'enfant ;
  `tests/gamescope_limit.sh` (ctest, saute sans gamescope). Charger la build
  par un `XDG_DATA_HOME` privé — pas `VK_IMPLICIT_LAYER_PATH`
  (`tools/layer_env.sh`), qui masque la couche WSI, ni l'environnement de
  l'hôte, qui charge la couche **installée** dans `~/.local`.
- **Handle de la swapchain** : `handle()` = `real_` à la création ;
  `followSurface` remplace `real_` → métadonnées HDR, display timing,
  `VK_NV_low_latency2`, compteur, local dimming relayés à `currentReal()`.
- Statut : `limit_hz` (`--report`, panneau).

### CI Linux — pièges (2026-09-24)

- Le loader d'Ubuntu 24.04 (1.3.275) **ignore `VK_IMPLICIT_LAYER_PATH`** (et
  parfois `VK_ADD_IMPLICIT_LAYER_PATH`). Les tests chargent la build voulue en
  copiant son manifeste dans un `XDG_DATA_HOME` privé (`tools/layer_env.sh`,
  `layer_env()` dans `check_matcher_invariant.py`, `test-xdg` généré par
  CMake pour `acquire_timeout`) ; rien n'est installé au niveau système en CI.
- Les étapes `run` sont en `bash -eo pipefail` : `cmd | grep -q` échoue quand
  grep trouve (SIGPIPE). Capturer la sortie puis `grep -q <<<"$out"`.
- **Miroirs Ubuntu intermittents** (vécu le 2026-10-07, focal) : `apt-get
  update` n'avertit que (code 0), l'installation échoue ensuite sans rien
  dire du réseau. Les conteneurs des scripts `tools/ci/` montent
  `tools/ci/apt-get` en `/usr/local/sbin/apt-get` (devant `/usr/bin`), qui
  réessaie sur erreur réseau seulement ; tout nouveau `docker run` qui
  utilise apt doit le monter aussi.
- **Avant de pousser un changement de CI** : `tools/ci/run.sh` (build) et
  `tools/ci/run.sh asan|tsan` rejouent les jobs dans Docker, même loader,
  lavapipe.
- **Job `windows-syntax`** (linux.yml, à chaque push) : le même contrôle
  MinGW x64/i686, en une minute, à côté des résultats Linux.
- **Avant de pousser du C++** : `tools/ci/run.sh mingw` compile (syntaxe)
  ce que la CI Windows construit, avec MinGW x64 **et i686** — le passage
  32 bits attrape les erreurs propres à MSVC Win32 (handles Vulkan non
  dispatchables = `uint64_t`, pas de `reinterpret_cast`), vues sinon
  seulement après le push. Faux positif connu : aucun (l'app capture et
  `bdex_offline` ne sont pas compilés en 32 bits / sous Windows, le job
  suit les mêmes règles que `CMakeLists.txt`).
  **Angle mort** (vécu le 2026-10-04) : MinGW « posix » inclut `windows.h`
  par ses threads, pas MSVC — un `LARGE_INTEGER` sans `windows.h` dans un
  fichier de la couche y passe et casse MSVC. Les appels Win32 vont dans
  `platform.cpp`. Et MSVC `/W4 /WX` refuse une variable qui masque un
  paramètre (C4457 : `msg` dans une `WndProc`).

### Variantes allégées des shaders (2026-09-25)

`block_match.comp` et `interpolate.comp` ont une constante de
spécialisation `kOptIns` (défaut `true` = tout le code). `makePass(...,
lean=true)` crée en plus `Pass::lean` (constante à `false`), liée quand les
options concernées sont à 0 (matcher : grad/census/rot/chroma ; synthèse :
décidé sur les push constants). Raison : les opt-ins déclarés
inconditionnellement coûtaient ~21 Ko de shared au matcher (occupation ÷2)
et des registres à la synthèse → flow + synthèse ×2 depuis le 14/09
(2,7 → 1,35 ms en 1440p, preset quality), latence ressentie en
interpolation. **Un nouvel opt-in de ces shaders doit être gardé par
`kOptIns &&`, et ses tableaux partagés dimensionnés par une constante
`kOptIns ? N : 1`** — sauf dans `interpolate.comp` depuis le 2026-10-03 :
chaque opt-in y a son bit de `kFeatures` (constante 1, `SYNTH_*` de
`framegen_internal.h`, garde `kX && pc.x`), le moteur lie la variante
compilée pour exactement l'ensemble utilisé (`synthesisVariant`, pré-construite
par `prebuildSynthesisVariants`) : un nouvel opt-in de la synthèse = un bit,
sa constante, son test dans le calcul du masque (deux endroits). Mesuré :
la variante complète coûtait 0,33 ms de plus que l'allégée à travail égal
(1440p) ; défauts dGPU 0,99 → 0,66 ms. Ne pas écrire `kOptIns && (kFeatures
& n)` : l'optimiseur de glslc échoue (« does not dominate its use ») (glslang refuse un indice constant hors d'une taille
spécialisée : `rsum` reste pleine taille). Sur NVIDIA, les branches mortes
changent la génération de code (FMA) : `main` différait de `f2cec5f` de
1 LSB en interpolation, la variante allégée lui est identique.
Mesure GPU sans fenêtre : `gamescope --backend headless -W 2560 -H 1440 --
env BDEX_FG_PROFILE=1 BDEX_FG_LOG=2 bdex_demo --size 2560x1440
--ignore-resize` (lignes `gpu ms:`).

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
  cf. retour Kenshi). Supprimé (l'option `extrapolate_hold` aussi, le 2026-10-04) :
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

Tenus à jour dans `ROADMAP.md` (où en est le projet, ce qui reste) et
`docs/MEMOIRE.md` §9 (problèmes ouverts, essais à ne pas refaire). Retenir :
- **Options retirées le 2026-10-04** (mesurées négatives ou neutres) :
  `alt_vectors`, `affine_flow`, `flow_upsample`, `confidence_fallback`,
  `extrapolate_hold`. Bits `SYNTH_*` 128, 512, 8192 libres. Vérification :
  hachage des sorties de `bdex_offline` pour chaque réglage de
  `test_determinism.py` × séquences × modes, avant et après — identiques.
  Méthode à reprendre pour toute suppression ou refactorisation du moteur.
- **Où est l'erreur** (2026-10-04, séquences type jeu 1280×720) : la caméra
  rapide est suivie (34 dB à 80 px par paire) ; 60 à 80 % de l'erreur est
  au contour des objets qui bougent par eux-mêmes (2 à 4 % des pixels).
  Sous-pixel, bicubique, `learned_flow`, `levels=5`, `flow_iterations=3`
  n'y changent rien. Une piste de qualité doit viser ces contours.
- **Options en attente de vrais jeux** : `tools/pending_decisions.py
  SEQ…` (verdict par option ; liste `CANDIDATES` à tenir à jour quand une
  option est tranchée ou ajoutée).

---

*Maintenir ce fichier à jour quand l'architecture ou les invariants changent.*
