<p align="center">
  <img src="assets/CloudNav-icon.png" width="132" alt="Icône CloudNav">
</p>

# CloudNav 1.2.2

CloudNav est un utilitaire Windows 11 natif et portable qui rassemble deux réglages souvent dispersés : les éléments cloud affichés dans le volet de l’Explorateur et l’emplacement des dossiers personnels Windows.

**[Télécharger CloudNav pour Windows](https://github.com/patriceac/CloudNav/releases/latest/download/CloudNav.exe)**

Un seul fichier `.exe`, sans installation ni runtime applicatif supplémentaire.

## Fonctions

- afficher ou masquer l’entrée **My Drive** dans le volet de navigation ;
- afficher ou masquer le compte **OneDrive** détecté ;
- masquer la lettre du lecteur virtuel **Google Drive** sans bloquer l’accès aux fichiers ;
- repointer Bureau, Documents, Images, Téléchargements, Musique et Vidéos vers leur emplacement local, OneDrive, Google Drive ou un dossier personnalisé ;
- copier, déplacer ou repointer seulement les fichiers avec une confirmation explicite ;
- mémoriser la position de la fenêtre et la ramener sur un écran visible si la configuration des moniteurs change.

CloudNav détecte les chemins et libellés présents sur le PC : aucun nom de compte ou chemin utilisateur n’est codé en dur dans l’application.

## Précautions

Les cases de la fenêtre principale modifient uniquement la visibilité du volet Explorer. Masquer le lecteur Google Drive utilise la politique Windows `NoDrives` : l’icône disparaît, mais le lecteur et ses fichiers restent accessibles.

Les changements de dossiers personnels sont présentés dans un récapitulatif séparé avant exécution. CloudNav valide toutes les destinations, bloque les chemins imbriqués et restaure les emplacements précédents si une opération groupée échoue.

Quand Bureau, Documents ou Images quitte OneDrive pour Google Drive, CloudNav peut désactiver la sauvegarde des dossiers connus OneDrive avant le repointage. La confirmation explique la portée de cette stratégie et aucun fichier OneDrive n’est supprimé.

Si une tâche planifiée nommée `OneDrive-GDrive-Bidirectional-Sync` est présente, CloudNav considère que les deux racines sont déjà synchronisées et recommande **Repointage seulement**. Sans ce signal, **Copier** reste le choix prudent par défaut.

## Utilisation

1. Télécharger `CloudNav.exe` depuis la dernière Release GitHub.
2. Vérifier que Google Drive et/ou OneDrive sont installés et démarrés.
3. Lancer l’exécutable, sans installation.
4. Choisir les éléments du volet Explorer ou ouvrir **Dossiers personnels…**.
5. Vérifier le récapitulatif, puis accepter la demande d’administration Windows lorsqu’elle est nécessaire.

L’Explorateur peut redémarrer une fois pour relire sa configuration ; les fenêtres de dossiers ouvertes sont alors fermées.

## Configuration requise

- Windows 11 x64 ;
- Google Drive pour ordinateur et/ou OneDrive selon les fonctions utilisées ;
- aucune bibliothèque ni aucun runtime applicatif supplémentaire.

L’interface de CloudNav est actuellement en français.

## Compiler

Visual Studio Build Tools 2022 avec les outils C++ x64 est requis. Depuis PowerShell :

```powershell
.\build.ps1 -Configuration Release
```

La Release utilise le runtime C++ statique (`/MT`) et produit :

- `build\Release\CloudNav.exe` ;
- `build\Release\CloudNavTests.exe` ;
- `build\Release\CloudNavWindowPositionTests.exe`.

Le script exécute automatiquement les tests logiques. Les scénarios d’interface et d’intégration destinés au banc Windows isolé se trouvent dans `tests/`.
