# Validation sur vrai matériel — Linux

Tout ce qui suit a été vérifié en conteneur (lavapipe, Xvfb, gamescope
headless), jamais sur la machine d'un joueur. Cette liste dit quoi essayer et
quoi renvoyer. Pour chaque essai : **`bdex-framegen --report`** pendant que le
jeu tourne (il écrit `bdex-framegen-report.txt`), plus une phrase sur ce qui
se voit à l'écran.

Le résultat attendu partout : le compteur (`overlay = 2`, ou « Compteur » dans
le panneau) affiche *généré* ≈ 2 × *jeu* en ×2, et le rapport dit
`generation on`.

| # | Configuration | Installer | Essayer | Ce qui peut casser |
|---|---|---|---|---|
| 1 | **Steam en Flatpak** (Bazzite, Fedora, Mint) | archive `./install.sh`, ou l'extension Flatpak (`packaging/flatpak`) | un jeu Proton (DXVK), un jeu 32 bits | couche pas chargée dans le bac à sable ; panneau qui ne voit pas le jeu (espace de PID) ; copie obsolète après mise à jour (le panneau doit afficher « Couche mise à jour dans… ») |
| 2 | **Steam Deck, mode Jeu** | archive en mode Bureau | `bdex-framegen --always-on`, un jeu à 30 i/s, limite Steam désactivée | performance de la synthèse (RDNA2 8 CU), 90 Hz OLED, HDR |
| 3 | **Ubuntu, Steam en Snap** | archive (`install.sh` copie dans `~/snap/steam`) | un jeu Proton | AppArmor qui refuse de charger la `.so` (le rapport donne l'état d'AppArmor) |
| 4 | **Fedora, SELinux enforcing** | `.rpm` ou paquet COPR | un jeu natif et un jeu Proton | refus SELinux (`sudo ausearch -m avc -ts recent` à joindre) |
| 5 | **NVIDIA + KDE Plasma** (Wayland et X11) | n'importe lequel | un jeu à fps élevé (Valheim) | pas de retour d'affichage ; fréquence mal lue (comparer avec celle de KDE) |
| 6 | **Portable hybride** (Intel/AMD + NVIDIA) | n'importe lequel | `bdex-framegen --gpu list`, puis `--gpu screen -- jeu` et `--gpu discrete -- jeu` | copie PRIME (le rapport la signale) ; mauvais GPU choisi |
| 7 | **Debian 12 / Ubuntu 22.04 depuis les sources** | `tools/install.sh` avec `gcc-multilib g++-multilib` | un jeu 32 bits | couche 32 bits absente (`ls ~/.local/lib32`) |
| 8 | **GNOME Wayland sans GTK 4 Python** (mode texte) | archive | `bdex-framegen-gui` depuis le menu | aucun terminal ouvert ; fréquence 0 Hz |
| 9 | **Arch / CachyOS** | AUR `bdex-framegen-git` | un jeu natif Vulkan | — |
| 10 | **Gentoo / Void** | overlay `packaging/gentoo`, template `packaging/void` | un jeu natif | dépendances manquantes à l'exécution |

Renvoyer : le rapport, la distribution et sa version, le GPU, le compositeur,
et pour les cas 1, 3, 4 la manière dont Steam est installé.
