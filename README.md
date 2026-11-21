<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">Evict</h1>

<p align="center">
  <b>Le videur de ton disque.</b><br>
  Evict scanne <i>tout</i> ton Mac en quelques secondes et te montre, en boîtes proportionnelles,<br>
  ce qui prend vraiment la place. Tu cliques, tu descends dans les dossiers, tu fais le ménage.
</p>

<p align="center">
  <img alt="macOS 13+" src="https://img.shields.io/badge/macOS-13%2B-111?logo=apple">
  <img alt="C + SwiftUI" src="https://img.shields.io/badge/C%20%2B%20SwiftUI-natif-e0473d">
  <img alt="0 dépendance" src="https://img.shields.io/badge/d%C3%A9pendances-0-2ea36b">
  <img alt="100% local" src="https://img.shields.io/badge/100%25-local%2C%20sans%20IA-3d7fe6">
</p>

![Evict : carte du disque en boîtes](docs/screenshot.png)
<p align="center"><sub>Capture réalisée sur un disque fictif.</sub></p>

## Ce que fait Evict

- **Tout le disque, très vite** : 5,7 millions de fichiers (426 Go) en 20 s sur un Mac mini M4 Pro, et 27 s à froid, juste après un redémarrage.
- **Une carte en boîtes** (treemap), façon finviz : la surface de chaque boîte est proportionnelle à la place occupée. Clic pour entrer dans un dossier, fil d'Ariane pour remonter.
- **Des couleurs qui parlent** :
  - 🟥 **régénérable** : caches, `node_modules`, DerivedData, builds, `.venv`…
  - 🟧 **archives et images disque** : zip, dmg, iso, images de VM…
  - 🟪 **médias**, 🟦 **applications**, 🟩 **données**, ⬛ **système**

  Un clic sur une catégorie la met en évidence.
- **Suppression intégrée** (app) : mise à la corbeille ou suppression définitive. Le mot de passe administrateur n'est demandé que si c'est nécessaire, et les chemins système sont protégés.
- **Masquer un dossier** (par exemple `System`) pour laisser plus de place au reste. Le choix est mémorisé entre les scans.
- **100 % local** : sans IA, sans télémétrie, sans dépendance. Juste un scanner en C et une page HTML.

## Installation

Prérequis : macOS 13 ou plus récent, et les outils de développement d'Apple (`xcode-select --install`).

```sh
git clone https://github.com/vassilidev/evict.git
cd evict
./install.sh
```

`install.sh` compile Evict.app, l'installe dans `/Applications` (ou `~/Applications`) puis l'ouvre. Ensuite, lance Evict depuis Spotlight quand tu veux, et utilise **⌘R** pour rescanner.

> **Au premier lancement**, Evict demande l'**Accès complet au disque**. Sans lui, macOS interrompt le scan avec une fenêtre d'autorisation par dossier protégé (Bureau, Documents, données des autres apps…). Clique sur « Ouvrir les réglages », active Evict, et le scan démarre tout seul. « Scanner quand même » reste possible.
> L'app est signée localement : il faut refaire cette autorisation après chaque `./install.sh`.

### En ligne de commande

```sh
./evict              # tout le disque, rapport ouvert dans le navigateur
./evict ~/Projects   # un seul dossier
```

Dans le navigateur, le menu propose **Copier la commande `rm -rf`** au lieu de supprimer directement.

## Utilisation

| Geste | Effet |
| --- | --- |
| Clic sur un dossier | Entrer dedans |
| Clic sur un fichier · clic droit | Menu : ouvrir, copier le chemin, masquer, supprimer |
| `⌫` ou `Échap` | Remonter d'un niveau |
| Clic sur une catégorie | La mettre en évidence |
| Bouton « N masqués » | Réafficher des dossiers masqués |
| `⌘R` (app) | Rescanner |

## Pourquoi c'est rapide

Mesuré sur le même Mac (Apple M4 Pro, SSD interne, 5,7 M d'entrées) :

| Méthode | Temps |
| --- | ---: |
| Python, `os.scandir` + `lstat`, 1 thread | 153 s |
| Première version C : chemins complets, file de travail globale, 36 threads | 41 s |
| **Evict** : `getattrlistbulk` + `openat`, 8 threads | **20 s** (27 s à froid) |

1. **`getattrlistbulk(2)`** : un seul appel système renvoie le nom, le type, la taille allouée et le nombre de liens de centaines d'entrées d'un coup. Il n'y a aucun `stat()` par fichier.
2. **`openat(parent, nom)`** : aucun chemin complet n'est jamais résolu, donc les threads ne se disputent pas les nœuds de `/` et de `/Users` dans le noyau.
3. **Une pile locale par thread** (parcours en profondeur) : le travail n'est partagé que lorsqu'un thread est inactif.
4. **8 threads, mesuré** : au-delà, APFS sature sur ses verrous internes. À 24 threads, le temps passé dans le noyau monte à 309 s cumulées pour aller *moins* vite.
5. **Pas de rapatriement iCloud** : avec `IOPOL_MATERIALIZE_DATALESS_FILES_OFF`, les fichiers iCloud ou Drive non téléchargés ne sont jamais rapatriés pendant le scan.
6. **Taille réelle sur disque** (`ATTR_FILE_ALLOCSIZE`) : les liens physiques ne sont comptés qu'une fois, et les fichiers de moins de 1 Mo sont regroupés. Le rapport d'un disque entier pèse environ 3 Mo.
7. **Rien n'est compté deux fois** : le volume Data, accessible via des firmlinks, et les volumes des autres conteneurs sont exclus.
8. **Pas de blocage indéfini** : si un dossier reste bloqué derrière une demande d'autorisation macOS, le scan s'arrête au bout de 30 s sans progression et rend des résultats partiels, avec un avertissement.

## Sous le capot

```
scan.c        scanner (C, sans dépendance)  →  JSON de l'arbre élagué
index.html    interface (HTML/CSS/JS pur)   ←  données injectées dans la page
app/          app macOS (SwiftUI + WKWebView, pont JS ↔ natif pour la suppression)
evict         script CLI : scan + page + ouverture dans le navigateur
install.sh    compile, signe localement et installe Evict.app
test.sh       compare le total du scanner à `du` sur un dossier de test
```

## Limites connues

- **macOS uniquement pour l'instant** : [Linux](https://github.com/vassilidev/evict/issues/1) et [Windows](https://github.com/vassilidev/evict/issues/2) sont prévus.
- **Clones APFS** : ils sont comptés à leur taille pleine, donc la somme peut dépasser l'espace réellement utilisé.
- **Snapshots** : les snapshots APFS et Time Machine ne sont pas visibles. Ils apparaissent dans la case « Invisible ».

## Feuille de route

- [ ] [Support natif Linux](https://github.com/vassilidev/evict/issues/1) : `getdents64` + `statx` par lots via io_uring, WebKitGTK
- [ ] [Support natif Windows](https://github.com/vassilidev/evict/issues/2) : lecture directe de la MFT NTFS, WebView2
