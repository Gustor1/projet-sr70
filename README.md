# Simulation bancaire SR70 en C

Projet SR70 : simulation d'une banque avec plusieurs clients qui font des opérations en même temps. Le programme utilise les notions vues en cours : processus, tubes, signaux, files de messages, threads, sémaphores et mémoire partagée.

Le sujet est dans [docs/consignes-banque.pdf](docs/consignes-banque.pdf). Les explications détaillées sont dans [EXPLICATIONS.md](EXPLICATIONS.md).

## Lancer le programme

Le programme utilise des appels système Linux. Sous Windows, il tourne dans Ubuntu (WSL).

Depuis Windows, dans le dossier du projet :

```bat
lancer.cmd
```

ou en PowerShell :

```powershell
./build.ps1 -Run          # menu
./build.ps1 -Test -Demo   # tests puis démonstration
```

Directement sous Linux, **depuis le dossier du projet** :

```sh
make            # compile build/banque, build/journal et build/test_banque
./build/banque  # menu
make test       # tests
make demo       # démonstration avec trois processus clients
```

Prérequis : GCC et Make.

## Utilisation

| Client | Numéro | Comptes | Solde de départ |
|---|---|---|---|
| Alice | 1 | 1 et 2 | 1 000 € chacun |
| Bob | 2 | 3 et 4 | 1 000 € chacun |
| Charlie | 3 | 5 et 6 | 1 000 € chacun |

Menu :

1. Dépôt
2. Retrait
3. Transfert (entre ses comptes ou vers un autre client)
4. Consultation d'un compte
5. Changer de client
6. Voir ses deux comptes
7. Démonstration : trois processus clients en parallèle
8. Tableau de bord (lu en mémoire partagée)
0. Quitter

Les montants s'écrivent `12`, `12,50` ou `12.50`. Un retrait ou un transfert supérieur au solde est refusé et ne modifie rien.

Après chaque session, `build/journal.txt` contient la liste des opérations.

## Organisation

```text
Processus principal (menu)
 ├── fork + exec ──> journal   : lit le tube nommé, écrit build/journal.txt
 ├── fork ─────────> banque    : 2 threads employés, comptes protégés par un mutex
 └── fork ─────────> clients   : Alice, Bob, Charlie (démonstration)

clients ──file de requêtes──> banque ──file de réponses──> clients
banque  ──mémoire partagée System V──> tableau de bord (processus principal)
banque  ──tube nommé──> journal ──mémoire partagée POSIX──> processus principal
```

## Fichiers

- `src/banque.h`, `src/banque.c` : la banque, les IPC et les règles bancaires.
- `src/main.c` : le menu et la démonstration.
- `src/journal.h`, `src/journal.c` : le programme journal.
- `tests/test_banque.c` : les tests.
- `docs/consignes-banque.pdf` : le sujet.
- `docs/rapport/rapport.html` : source du rapport.

## Préparer le rendu

Depuis PowerShell, dans le dossier du projet :

```powershell
./rendu.ps1
```

Le script crée dans `rendu/` le rapport `rapport-sr70.pdf` (imprimé par Edge depuis `docs/rapport/rapport.html`) et l'archive `projet-sr70-source.zip` du code source.

## Si le programme a été tué brutalement

Avec `kill -9`, le programme ne peut pas nettoyer. Pour supprimer ce qui reste :

```sh
ipcs -q -m                 # voir les files et segments
ipcrm -q ID   ipcrm -m ID  # supprimer une file / un segment
rm /tmp/sr70-banque-*.fifo /dev/shm/sr70-journal-*
```
