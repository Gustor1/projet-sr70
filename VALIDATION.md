# Validation de la version Linux IPC

Vérifications exécutées le **22 octobre 2026**, dans Ubuntu via WSL2, noyau Linux `6.6.87.2-microsoft-standard-WSL2`.

## Point de départ

Avant la migration, la version Windows a été recompilée et ses tests ont donné **39 contrôles, zéro échec**. Ce résultat est historique : les exécutables Windows ne correspondent plus à la version actuelle.

Les règles bancaires, les six soldes initiaux et les scénarios fonctionnels ont été conservés pendant le remplacement de la communication entre threads par des processus et des files System V.

## Compilation et moteur C

Commandes exécutées depuis Windows :

```powershell
./build.ps1 -Test -Demo
```

Ce script compile et exécute dans Ubuntu/WSL. Options : `-D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread`.

Résultat : **137 contrôles, zéro échec**, sans avertissement de compilation. Les tests comprennent les règles bancaires, les erreurs de droits ou de montant, les plafonds, l'absence de modification lors d'un transfert refusé, les priorités sur une véritable file System V, ainsi que deux scénarios de 24 processus effectuant 100 demandes chacun.

Le scénario de retraits simultanés accepte exactement 100 retraits de 10 € depuis 1 000 € et aboutit à zéro. Les 2 400 transferts en sens opposés du second scénario conservent les soldes et le total.

## Démonstration

Exécution réelle de trois processus clients, chacun avec son PID distinct, et de deux threads employés dans un processus banque séparé.

| Comptes | Solde final chacun |
|---|---|
| 1, 3, 5 | 1 025,00 € |
| 2, 4, 6 | 1 050,00 € |
| Total | 6 225,00 € |

Les trois retraits excessifs sont refusés.

## Tests système automatisés

Commande : `make test-systeme` dans Ubuntu/WSL. Le fichier `tests/test_systeme.py` utilise uniquement la bibliothèque standard de Python.

**9 scénarios réussis :**

1. Processus banque séparé, trois threads au total et fermeture normale.
2. Saisies invalides, décimales exactes, transfert et changement de client.
3. Lignes de 1 000 caractères et fin d'entrée pendant une saisie.
4. SIGINT pendant la saisie du menu.
5. SIGTERM envoyé au groupe de processus.
6. Arrêt du serveur par SIGTERM puis demande client.
7. Serveur tué par SIGKILL : erreur client après une attente bornée, puis nettoyage par le parent.
8. Deux banques simultanées avec files distinctes et soldes indépendants.
9. Démonstration depuis le menu avec trois PID clients et total final attendu.

Les tests contrôlent la présence réelle des files pendant la session, leur suppression après la fermeture et la disparition du processus serveur récolté par `waitpid`. Après les tests, `ipcs -q` n'affiche aucune file et la recherche des processus du projet ne trouve aucun processus restant dans cet environnement.

Le lanceur a aussi été exécuté depuis CMD avec une entrée `0` : compilation via WSL, affichage du menu et fermeture normale réussis. Les commandes PowerShell `./build.ps1 -Test -Systeme` ont été exécutées avec succès.

## Portée et limites

Ce sont des exécutions Linux réelles via WSL, pas une simulation de l'API Linux sous Windows. Aucune exécution n'a été réalisée sur une autre machine Linux ou la machine du professeur. Aucun détecteur spécialisé de courses mémoire n'a été lancé ; les tests concurrents ne constituent pas une preuve formelle de tous les ordonnancements possibles.

Le nettoyage est assuré pour les sorties normales et les signaux testés. SIGKILL appliqué à tous les processus empêche tout nettoyage applicatif. Une perte de communication peut laisser le résultat d'une opération non confirmé ; il n'y a ni rejeu automatique, ni sauvegarde, ni garantie de reprise après panne.
