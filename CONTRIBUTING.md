# Contribuer à bdex-framegen

Merci de vous intéresser au projet. Rapports de bugs, retours de tests sur de
vrais jeux, idées et pull requests sont les bienvenus.

## Signaler un bug ou un jeu qui ne fonctionne pas

Le plus utile est un retour de test sur un vrai jeu, car le layer n'a pour
l'instant été validé qu'avec la démo, `vkcube`, `vkgears` et le conteneur
Steam Linux Runtime. Ouvrez une issue avec :

1. **Le log du layer** en verbosité 2 (ou 3 si le problème survient au
   chargement) :
   ```
   BDEX_FG=1 BDEX_FG_LOG=2 BDEX_FG_LOG_FILE=/tmp/bdex.log %command%
   ```
   Les premières lignes indiquent la configuration, le GPU, la file utilisée
   et le format de swapchain ; c'est souvent suffisant pour comprendre.
2. **Votre environnement** : distribution, GPU et pilote (`vulkaninfo --summary`),
   compositeur (Wayland/X11), version du loader Vulkan.
3. **Le jeu** et la façon de le lancer (natif, DXVK, VKD3D-Proton, version de
   Proton), ainsi que les options de lancement.
4. **Le symptôme** : pas d'effet, crash, image noire, artefacts, saccades…
   Pour les artefacts, une capture avec `BDEX_FG_DEBUG=flow` et une autre avec
   `BDEX_FG_DEBUG=split` aident beaucoup.

Si le jeu plante, vérifiez d'abord qu'il fonctionne avec
`BDEX_FG_DEBUG=passthrough` (layer chargé, swapchain virtualisée, mais aucune
génération) : cela distingue un problème d'intégration Vulkan d'un problème
dans les shaders.

## Proposer une modification

### Mise en place

```sh
git clone git@github.com:Bdexez/bdex_frame_gen.git
cd bdex_frame_gen
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Le layer se teste sans l'installer :

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer BDEX_FG=1 ./build/demo/bdex_demo --fps 30
```

### Avant d'ouvrir une pull request

- `ctest` passe (tests unitaires + test bout-en-bout).
- Le layer reste propre sous la validation Khronos, synchronisation comprise :
  ```sh
  VK_LOADER_LAYERS_ENABLE='*validation' BDEX_FG=1 build/demo/bdex_demo --fps 30 --frames 90
  ```
  (pour activer la validation de synchronisation, voir `khronos_validation.validate_sync`
  dans un fichier `vk_layer_settings.txt`).
- Pour toute modification des shaders ou de l'interpolation, joignez les
  chiffres de `tools/run_eval.sh build 40` avant / après, ainsi que
  `BDEX_EVAL_ARGS="--speed 2.5" tools/run_eval.sh build 40` pour les
  mouvements rapides. Une amélioration de PSNR sur la démo n'est pas une
  preuve absolue, mais une régression en est une bonne.
- Pour toute modification des performances, joignez une ligne de statistiques
  avec `BDEX_FG_PROFILE=1` avant / après.
- Une PR = un sujet. Les commits sont décrits à l'impératif, avec un corps
  qui explique *pourquoi* quand ce n'est pas évident.
- Ajoutez une entrée dans `CHANGELOG.md` (section *Non publié*).

### Style

- C++20, `-Wall -Wextra` sans avertissement. Pas de dépendance externe au-delà
  des en-têtes Vulkan (le layer est chargé dans le processus du jeu : il doit
  rester léger et ne rien tirer de surprenant).
- Les fonctions Vulkan sont toujours appelées via les tables de dispatch
  (`dev.vt.*`, `inst->vt.*`), jamais via le loader.
- Tout objet dispatchable créé par le layer (queue, command buffer) passe par
  `adoptDispatch()` ; tout objet créé doit être détruit dans `destroyAll()`.
- Les shaders sont compilés pour SPIR-V 1.0 (`--target-env=vulkan1.0`) afin de
  fonctionner avec les applications Vulkan 1.0 ; n'utilisez pas d'extension
  SPIR-V sans repli.
- Les options de configuration se déclarent dans `config.h`, se parsent dans
  `Config::apply()`, sont testées dans `tests/test_config.cpp` et documentées
  dans le tableau du README.

## Idées de contributions

- Retours de tests sur des jeux DXVK / VKD3D-Proton et sur NVIDIA / Intel.
- Estimation de flux plus fine (flux dense par pixel, gestion des occlusions).
- Indicateur à l'écran (fps réel / affiché) sans dépendre du format de swapchain.
- Prise en charge de formats supplémentaires (HDR 10 bits avec espace colorimétrique PQ).
- Paquets pour les distributions (PKGBUILD, Flatpak).

## Licence

En contribuant, vous acceptez que votre code soit publié sous la licence MIT
du projet.
