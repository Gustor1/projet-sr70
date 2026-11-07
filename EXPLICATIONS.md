# Explications du projet

## 1. Correspondance avec les consignes

| Consigne | Réalisation |
|---|---|
| Plusieurs clients, chacun avec un ou plusieurs comptes | 3 clients (Alice, Bob, Charlie), 2 comptes chacun |
| Dépôt, retrait, transfert (vers soi ou un autre client), consultation | Menu choix 1 à 4, fonction `executer()` |
| Priorité des opérations (ex. transfert avant consultation) | Type du message : transfert = 1, dépôt/retrait = 2, consultation = 3 |
| Opérations concurrentes protégées par sémaphores ou mutex | 2 threads employés, mutex `verrou_comptes`, sémaphores |
| Transactions atomiques, échec propre si le solde est insuffisant | Vérification puis modification sous le même mutex ; un refus ne change rien |
| Communication entre clients et banque (tubes, files, mémoire partagée) | Files de messages, tube, tube nommé, mémoire partagée System V et POSIX |

## 2. Correspondance avec les cours

| Cours | Utilisation dans le projet |
|---|---|
| Processus | `fork`, `getpid`, `waitpid`, `_exit` ; `execl` pour lancer le programme journal |
| Tubes | `pipe` : la banque prévient le père qu'elle est prête ; `mkfifo` : tube nommé banque → journal ; `dup2` : redirection de l'entrée du journal |
| Signaux | `sigaction`, `kill`, `sigprocmask`, `sigwait` pour l'arrêt ; `alarm` pour limiter l'attente ; `SIG_IGN` |
| Files de messages | `ftok`, `msgget`, `msgsnd`, `msgrcv`, `msgctl` ; priorités par type |
| Threads et sémaphores | `pthread_create`, `pthread_join`, mutex, `sem_init`, `sem_wait`, `sem_post` |
| Mémoire partagée | System V : `shmget`, `shmat`, `shmdt`, `shmctl` ; POSIX : `shm_open`, `mmap`, `munmap`, `shm_unlink` |

## 3. Les processus

```text
Processus principal (menu)
 ├── fork + exec ──> journal
 ├── fork ─────────> banque (thread principal + 2 threads employés)
 └── fork ─────────> clients Alice, Bob, Charlie (choix 7)
```

- `fork()` renvoie 0 dans le fils et le PID du fils dans le père.
- Le père attend chaque fils avec `waitpid()` pour ne pas laisser de zombie.
- Dans le menu, c'est le processus principal qui joue le client choisi. Dans la démonstration (choix 7), trois vrais processus clients sont créés.

## 4. Les files de messages

Deux files : une pour les requêtes, une pour les réponses. Leurs clés sont obtenues avec `ftok` à partir du tube nommé de la session :

```c
demandes = msgget(ftok(chemin_tube, 'R'), IPC_CREAT | 0600);
reponses = msgget(ftok(chemin_tube, 'A'), IPC_CREAT | 0600);
```

Une requête contient : le PID du client, un numéro, le client, l'opération, les comptes et le montant.

**Priorité** : le type du message vaut 1 pour un transfert, 2 pour un dépôt ou un retrait, 3 pour une consultation. Les employés lisent avec :

```c
msgrcv(demandes, &req, sizeof req.corps, -3, 0);
```

Avec `-3`, `msgrcv` prend le message de plus petit type, donc le plus prioritaire.

**Réponse** : son type est le PID du client. Chaque client lit seulement ses réponses avec `msgrcv(reponses, ..., getpid(), 0)`.

## 5. Les threads et le mutex

La banque a deux threads employés qui traitent les requêtes en parallèle. Les comptes sont communs aux deux threads, donc protégés par un mutex :

```c
pthread_mutex_lock(&verrou_comptes);
rep.corps.resultat = executer(&req);   /* vérifications puis modification */
pthread_mutex_unlock(&verrou_comptes);
```

Un transfert vérifie d'abord le compte, le montant et le solde, puis débite et crédite sous le même verrou : il est **atomique**. Si le solde est insuffisant, la réponse est « Solde insuffisant » et aucun solde ne change.

Un sémaphore `employes_prets` (initialisé à 0) permet au thread principal de la banque d'attendre que les deux employés aient démarré.

## 6. Le tube de démarrage

Avant de créer la banque, le père crée un tube avec `pipe`. Quand la banque est prête, elle écrit l'octet `P` dans le tube. Le père attend cet octet avec `read` avant d'envoyer la première requête.

## 7. La mémoire partagée System V : le tableau de bord

Après chaque requête, la banque recopie les six soldes, le total et le nombre de requêtes réussies ou refusées dans un segment de mémoire partagée. Le processus principal le lit directement (choix 8), sans envoyer de message.

```c
segment = shmget(ftok(chemin_tube, 'S'), sizeof(SegmentPartage), IPC_CREAT | 0600);
partage = shmat(segment, NULL, 0);
sem_init(&partage->verrou, 1, 1);   /* 1 = partagé entre processus */
```

Le sémaphore est placé **dans** le segment. Une variable ordinaire serait copiée par `fork`, et chaque processus aurait son propre sémaphore. La banque prend le sémaphore pour écrire, le processus principal le prend pour lire.

À la fin : `shmdt` pour se détacher, puis `shmctl(segment, IPC_RMID, NULL)` pour supprimer le segment.

## 8. Le journal : tube nommé, exec, dup2 et mémoire POSIX

La banque garde un historique des opérations. L'écriture du fichier est faite par un programme séparé, `build/journal`.

1. Le père crée un **tube nommé** : `mkfifo("/tmp/sr70-banque-PID.fifo", 0600)`.
2. Il crée un fils avec `fork`. Le fils ouvre le tube en lecture, le met à la place de son entrée standard avec `dup2(fd, STDIN_FILENO)`, puis lance le programme journal avec `execl`.
3. La banque ouvre le tube en écriture et envoie une ligne pour chaque opération.
4. Le journal lit son entrée standard avec `fgets` et recopie les lignes dans `build/journal.txt`. Quand la banque se termine, le tube est fermé, `fgets` renvoie `NULL` et le journal s'arrête.

Exemple de fichier obtenu :

```text
=== Journal de la session (processus principal PID=488) ===
DEPOT (Alice) compte 1 : 100,00 EUR -> Succes
RETRAIT (Alice) compte 1 : 5000,00 EUR -> Solde insuffisant
TRANSFERT (Alice) compte 1 vers compte 3 : 5,00 EUR -> Succes
=== Fin : 3 operations ===
```

Le fils ignore SIGINT et SIGTERM (`SIG_IGN`) avant `exec`. Un signal ignoré le reste après `exec` : un Ctrl+C n'arrête donc pas le journal avant qu'il ait tout écrit.

Le journal compte les opérations reçues dans une **mémoire partagée POSIX**. Après `exec`, le journal ne connaît plus les variables du père. Il retrouve la zone grâce à son nom :

```c
fd = shm_open("/sr70-journal-PID", O_RDWR, 0);
compteur = mmap(NULL, sizeof(CompteurJournal), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

Le processus principal affiche ce compteur dans le tableau de bord. Le journal est un autre processus, il peut donc avoir un léger retard sur la banque.

## 9. Les signaux

- **Ctrl+C / SIGTERM** : le gestionnaire installé avec `sigaction` met seulement une variable `interruption` à 1. Le programme la voit et s'arrête proprement.
- **Dans la banque**, SIGINT et SIGTERM sont bloqués avec `sigprocmask`. Le thread principal les attend avec `sigwait`, puis supprime les files, ce qui réveille les employés, et attend les employés avec `pthread_join`.
- **`alarm`** : un client attend sa réponse avec un `msgrcv` bloquant. Pour ne pas attendre pour toujours si la banque ne répond plus, il appelle `alarm(5)` avant d'attendre. Au bout de 5 secondes, SIGALRM interrompt `msgrcv`, qui renvoie `EINTR`, et le client affiche « Service indisponible ». `alarm(0)` annule l'alarme quand la réponse arrive.

## 10. Fermeture

Quand on quitte :

1. le père envoie SIGTERM à la banque avec `kill` ;
2. la banque supprime les files, attend ses threads et se termine, ce qui ferme le tube nommé ;
3. le journal lit la fin du tube, termine son fichier et se termine ;
4. le père attend la banque et le journal avec `waitpid`, puis supprime le segment (`shmctl`), la mémoire POSIX (`shm_unlink`) et le tube nommé (`unlink`).

## 11. Démonstration et tests

**Démonstration** (choix 7 ou `make demo`) : trois processus clients font chacun un dépôt de 100 €, un retrait de 25 €, deux transferts, un retrait trop grand (refusé) et une consultation. Sur une banque neuve, le total passe de 6 000 € à 6 225 €.

**Tests** (`make test`) :
- toutes les règles : dépôt, retrait, transfert, solde insuffisant, compte d'un autre client, montant invalide, plafond ;
- l'ordre des priorités dans une file ;
- 24 processus qui font 100 opérations chacun en même temps : exactement 100 retraits acceptés sur 1 000 €, et 2 400 transferts sans perte d'argent ;
- le tableau de bord et le compteur du journal ;
- `alarm` : la banque est suspendue avec SIGSTOP, et la requête est abandonnée au bout de 5 secondes.
