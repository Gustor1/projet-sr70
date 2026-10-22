# Simulation bancaire SR70 en C

Version console Linux, adaptée aux cours de processus, tubes, signaux, files de messages, threads et sémaphores. Trois clients possèdent chacun deux comptes et peuvent déposer, retirer, transférer et consulter leur solde.

La banque est un processus distinct. Ses deux threads employés reçoivent des demandes par **files de messages System V** et protègent les comptes avec un mutex POSIX. La démonstration crée trois processus clients avec `fork`.

## Lancer depuis Windows / CMD

Ubuntu via WSL est nécessaire pour les appels Linux étudiés en cours. Le lanceur ne les émule pas avec l'API Windows.

Depuis CMD, dans le dossier du projet :

```bat
lancer.cmd
```

Ou depuis PowerShell :

```powershell
./build.ps1 -Run
./build.ps1 -Test -Systeme -Demo
```

Le script utilise Ubuntu par défaut. Pour une autre distribution WSL :

```powershell
./build.ps1 -Distribution Debian -Run
```

Prérequis dans la distribution : GCC, Make, et Python 3 uniquement pour les tests système optionnels. Aucun paquet C tiers n'est nécessaire. Le programme compilé est `build/banque`, un exécutable Linux ; l'ancien `build/banque.exe` Windows ne correspond plus à cette version.

## Lancer directement sous Linux

```sh
make
./build/banque
```

Pour vérifier et démontrer :

```sh
make test            # tests C des règles bancaires et de la concurrence
make test-systeme    # tests Python des processus, signaux et ressources IPC
make demo            # trois processus clients en concurrence
```

Sous WSL, on peut aussi ouvrir Ubuntu, aller dans le dossier monté sous `/mnt/c/...`, puis utiliser ces commandes.

## Utilisation du menu

| Client | Identifiant | Comptes | Solde initial par compte |
|---|---|---|---|
| Alice | 1 | 1 et 2 | 1 000,00 € |
| Bob | 2 | 3 et 4 | 1 000,00 € |
| Charlie | 3 | 5 et 6 | 1 000,00 € |

Alice est sélectionnée au démarrage. Les choix sont :

1. Dépôt sur un de ses comptes.
2. Retrait d'un de ses comptes.
3. Transfert entre ses comptes ou vers un autre client.
4. Consultation du compte choisi.
5. Changement de client.
6. Consultation des deux comptes du client actif.
7. Démonstration concurrente sur les soldes actuels.
0. Fermeture.

Les montants acceptent `12`, `12,5`, `12,50` ou `12.50`. Les montants non positifs, trop grands ou avec plus de deux décimales sont refusés. Le solde maximal est d'un milliard d'euros par compte. Les montants sont représentés en centimes entiers.

Exemple : choisir `3`, compte source `1`, destinataire `3`, montant `50`. Alice transfère 50 € à Bob. Changer ensuite de client pour consulter le compte 3.

La sélection d'un client est une simulation d'identité, sans authentification. Les données restent en mémoire et sont réinitialisées au lancement. La démonstration du menu modifie les soldes de la session.

## Notions du cours mises en œuvre

| Notion | Utilisation concrète |
|---|---|
| `fork`, `getpid`, `waitpid`, `_exit` | Banque séparée, clients de démonstration, attente des enfants |
| `pipe`, `read`, `write`, `close` | Confirmation du démarrage de la banque ; synchronisation et résultats des tests |
| `msgget`, `msgsnd`, `msgrcv`, `msgctl` | Deux files système pour les requêtes et réponses |
| Types de messages et priorités | Transfert de type 1, dépôt/retrait de type 2, consultation de type 3 |
| `pthread_create`, `pthread_join` | Deux employés dans le processus banque |
| `pthread_mutex_*` | Protection atomique des validations et changements de soldes |
| `sem_init`, `sem_wait`, `sem_post`, `sem_destroy` | Attente du démarrage des deux employés |
| `sigaction`, `sigprocmask`, `sigwait`, `kill` | Interruption client et arrêt coordonné du serveur |
| `IPC_RMID` | Suppression des files à la fermeture |

Les tubes et files ont des rôles distincts : le tube transmet l'octet de confirmation de démarrage ; les files transportent les transactions structurées. Il n'est pas nécessaire d'ajouter `exec`, `alarm`, `mkfifo` et `dup2` pour les utiliser artificiellement.

## Observer les mécanismes

Au démarrage, le programme affiche les PID et les identifiants des deux files. Dans un autre terminal de la **même distribution Linux/WSL** :

```sh
ipcs -q
ps -eLf | grep banque
```

`Ctrl+C` ou `SIGTERM` provoque un arrêt coordonné. Le parent attend la banque et les clients ; les files sont supprimées. `SIGKILL` n'est pas interceptable : si tous les processus sont tués brutalement, des files peuvent rester. Supprimer alors uniquement les identifiants affichés par cette session avec `ipcrm -q IDENTIFIANT`.

## Comprendre et vérifier

- [EXPLICATIONS.md](EXPLICATIONS.md) : architecture, messages, verrous et fonctions expliqués.
- [VALIDATION.md](VALIDATION.md) : résultats réellement exécutés sous Ubuntu/WSL.
- `src/banque.h` : interface publique.
- `src/banque.c` : transport IPC, processus banque et règles bancaires.
- `src/main.c` : menu et processus clients de la démonstration.
- `tests/test_banque.c` : tests C.
- `tests/test_systeme.py` : tests système optionnels.

La priorité porte sur la sélection des messages encore en attente. Une requête déjà extraite n'est pas préemptée et l'ordre de fin des deux employés peut différer de l'ordre de sélection.

Si une réponse ne peut pas être confirmée sous cinq secondes ou si une interruption survient, l'opération retourne `SERVICE_INDISPONIBLE`. Cela ne garantit pas l'annulation d'une demande déjà envoyée : le programme n'effectue pas de renvoi automatique.
