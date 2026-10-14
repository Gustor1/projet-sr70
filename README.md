# Simulation de système bancaire en C

Projet pédagogique réalisé à partir du sujet **Simulation Avancée de Système Bancaire** (`banque.pdf`). Toutes les fonctionnalités proposées dans le document sont couvertes : comptes multiples, dépôt, retrait, transfert, consultation, priorités, concurrence, protection des transactions et échange requête/réponse.

Le code reste volontairement accessible : tableaux de taille fixe, structures, fonctions, deux mutex et des sémaphores. Pas de bibliothèque externe ni de base de données.

## Démarrer sous Windows

Prérequis : GCC (MinGW) accessible avec `gcc --version`. Dans PowerShell, depuis ce dossier :

```powershell
./build.ps1 -Test -Demo
./build/banque.exe
```

La première commande compile les deux exécutables, lance les tests puis la démonstration. La seconde ouvre le menu interactif sur une nouvelle banque.

Si la politique PowerShell bloque les scripts, compiler directement :

```powershell
mkdir build -Force
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc src/main.c src/banque.c -o build/banque.exe
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc tests/test_banque.c src/banque.c -o build/test_banque.exe
./build/test_banque.exe
./build/banque.exe --demo
./build/banque.exe
```

## Démarrer sous Linux

Avec GCC, Make et les bibliothèques système POSIX :

```sh
make test
make demo
./build/banque
```

La version Windows a été compilée et exécutée sur la machine de travail. La branche POSIX est fournie pour Linux, mais n'a pas été exécutée ici. Le Makefile vise Linux ; sous Windows, utiliser les commandes ci-dessus.

## Utilisation

Chaque lancement initialise cette banque :

| Identifiant client | Nom | Comptes | Solde initial par compte |
|---|---|---|---|
| 1 | Alice | 1 et 2 | 1 000,00 € |
| 2 | Bob | 3 et 4 | 1 000,00 € |
| 3 | Charlie | 5 et 6 | 1 000,00 € |

Alice est sélectionnée au démarrage. Le menu permet de :

1. Déposer de l'argent sur un de ses comptes.
2. Retirer de l'argent de l'un de ses comptes.
3. Transférer de l'argent vers son deuxième compte ou un compte d'un autre client.
4. Consulter le solde d'un de ses comptes.
5. Changer de client.
6. Afficher les deux comptes du client actif.
7. Lancer trois clients simultanément sur les soldes actuels.
0. Quitter proprement.

Les montants acceptent `12`, `12,5`, `12,50` ou `12.50`. Les valeurs négatives, nulles, trop grandes ou avec plus de deux décimales sont refusées. Une fin d'entrée (EOF) ferme proprement l'application.

**Exemple :** choisir `3`, puis source `1`, destination `3`, montant `50`. Alice envoie 50 € à Bob. Choisir ensuite `6` pour voir les comptes d'Alice, puis `5` et `2` pour devenir Bob et consulter le compte 3.

Le choix du client simule une identité ; ce n'est pas une authentification. Les données sont uniquement en mémoire et repartent de zéro à chaque lancement. La démonstration du menu modifie les comptes existants, sans les réinitialiser.

## Fichiers à lire

| Fichier | Rôle |
|---|---|
| `src/banque.h` | Types et fonctions publiques |
| `src/banque.c` | Comptes, validations, file prioritaire et employés |
| `src/main.c` | Menu, lecture des montants, démonstration |
| `src/synchronisation.h` | Adaptation threads/mutex/sémaphores Windows et Linux |
| `tests/test_banque.c` | Tests fonctionnels et concurrents |
| `EXPLICATIONS.md` | Explication détaillée du fonctionnement |
| `VALIDATION.md` | Vérifications réellement exécutées et limites |

## Correspondance avec le sujet

| Fonctionnalité du PDF | Réalisation |
|---|---|
| Plusieurs clients et comptes | Trois clients, deux comptes chacun |
| Dépôt, retrait, consultation | `executer`, accessible par le menu et les threads clients |
| Transfert interne et vers un autre client | Même opération, destination libre parmi les six comptes |
| Priorisation | Transfert > dépôt/retrait > consultation ; FIFO à priorité égale |
| Clients concurrents | Trois threads de démonstration ; 24 producteurs dans les tests |
| Banque distincte des clients | Deux threads employés consomment la file |
| Verrouillage | Mutex des comptes et mutex de la file |
| Transactions atomiques | Validation puis modification sous le même verrou |
| Échec si fonds insuffisants | Réponse explicite, aucun changement des soldes |
| Communication et réponse | File en mémoire partagée, sémaphores et réponse dédiée par requête |

**Choix concernant l'IPC :** le document autorise explicitement « processus ou threads distincts ». Ce projet choisit les threads : la communication utilise donc la mémoire commune du processus, une file de messages construite dans le programme et des sémaphores. Il ne prétend pas utiliser une file IPC système entre processus séparés. Si une consigne supplémentaire impose `fork`, des pipes ou une file System V, cette variante ne suffit pas ; il faudrait adapter l'architecture.
