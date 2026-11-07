# Comprendre la version Linux et ses notions SR70

## 1. Ce qui a changé

La première version utilisait des threads dans un seul processus et une file construite dans un tableau. La nouvelle version utilise les mécanismes Linux présentés dans les cours : de vrais processus, deux files System V, un tube et des signaux. Les règles bancaires et la représentation des montants restent les mêmes.

Cette version remplace le transport Windows. Elle se lance sous Linux ou depuis CMD via Ubuntu/WSL avec `lancer.cmd`. Il n'y a aucune donnée bancaire enregistrée sur disque à migrer. L'ancienne version reste récupérable dans l'historique Git, au commit `8f52222`, si un retour est nécessaire.

## 2. Organisation des processus

```text
Processus principal : menu et supervision
    |
    +-- fork --> Processus banque
    |              +-- thread principal : attend un signal d'arrêt
    |              +-- thread employé 1 : reçoit et traite les demandes
    |              +-- thread employé 2 : reçoit et traite les demandes
    |
    +-- fork --> Client Alice   (pendant la démonstration)
    +-- fork --> Client Bob     (pendant la démonstration)
    +-- fork --> Client Charlie (pendant la démonstration)

Clients -- file de requêtes --> banque -- file de réponses --> clients
Banque  -- écrit -->  [segment de mémoire partagée]  -- lu par --> processus principal
```

Le processus principal joue lui-même le rôle du client lorsque tu utilises le menu. Le changement Alice/Bob/Charlie choisit une identité simulée. Le choix 7 crée réellement trois processus clients supplémentaires.

`fork()` renvoie zéro au fils et son PID au père. Le programme distingue les deux branches avec cette valeur. `getpid()` permet d'afficher le PID et d'adresser la réponse au bon processus.

Le père utilise `waitpid()` pour attendre chaque enfant et récupérer son statut. Cela évite de laisser des processus zombies. Les fils terminent avec `_exit()` après avoir vidé leurs affichages : ils ne continuent pas dans le menu du père.

Le processus principal ne crée aucun thread. Tous ses `fork` clients sont donc réalisés depuis un processus à un seul thread. Les employés appartiennent au processus banque, séparé : on évite ainsi les difficultés d'un `fork` effectué par un programme déjà multithreadé.

## 3. Où sont les comptes ?

Les six comptes sont un tableau utilisé uniquement par la banque. Chaque compte contient l'identifiant de son propriétaire et un solde `long long` en centimes. La banque initialise chaque compte à `100000`, soit 1 000 €.

Après `fork`, deux processus ne partagent pas automatiquement les changements de leurs variables ordinaires. Un client n'accède donc jamais au tableau pour consulter un solde. Il envoie un message au serveur, qui détient les valeurs à jour.

Les deux threads employés partagent, eux, la mémoire du processus banque. Ils peuvent accéder aux mêmes comptes, avec un mutex pour les protéger.

Les comptes « officiels » restent donc dans la banque. Une **copie publiée** existe en plus dans un segment de mémoire partagée : c'est le tableau de bord expliqué à la section 8 bis. Les clients ne modifient jamais ce segment ; seule la banque y écrit.

## 4. Création des files System V

`banque_demarrer()` crée deux files avec :

```c
msgget(IPC_PRIVATE, IPC_CREAT | 0600);
```

La première reçoit les transactions ; la seconde contient les réponses. Chaque lancement obtient de nouvelles files, ce qui permet de lancer deux banques indépendantes en même temps. Le mode `0600` réserve l'accès au propriétaire de l'objet.

`IPC_PRIVATE` demande la création d'un nouvel objet ; ce n'est pas une authentification. Les identifiants entiers des files sont connus des processus descendants grâce à `fork`. Il n'est donc pas nécessaire de passer par `ftok` pour les retrouver.

Les messages commencent obligatoirement par un `long type`, suivi d'une structure `corps`. La taille transmise à `msgsnd` et `msgrcv` est `sizeof message.corps`, sans le champ `type`.

Les corps sont composés de valeurs. On ne transmet ni pointeur vers une requête locale ni sémaphore dans le message : une adresse d'un processus n'est pas un moyen de désigner une variable utilisable dans un autre processus.

## 5. Contenu des messages

Une requête contient le PID du client, un numéro de requête, l'identifiant client simulé, l'opération, les numéros des comptes et le montant.

Une réponse contient le numéro de requête et le résultat : statut et solde source. Son type est le PID du destinataire.

```c
msgrcv(reponses, &rep, sizeof rep.corps, (long)getpid(), IPC_NOWAIT);
```

Le type positif permet au client de ne récupérer que les réponses destinées à son processus. Le numéro permet d'ignorer une ancienne réponse arrivée après une attente expirée. Une seule demande à la fois est autorisée par processus client ; cette fonction n'est pas prévue pour plusieurs threads appelants du même client.

Le PID sert au routage de la simulation, pas à prouver une identité ou à authentifier un utilisateur.

## 6. Priorités des opérations

Avec l'API System V, le plus petit type représente ici la plus haute priorité :

| Opération | Type |
|---|---|
| Transfert | 1 |
| Dépôt / retrait | 2 |
| Consultation | 3 |

Les employés appellent :

```c
msgrcv(demandes, &req, sizeof req.corps, -3, 0);
```

Le type négatif demande le message du plus petit type inférieur ou égal à 3 ; à type égal, les messages sont pris dans l'ordre d'arrivée. On obtient donc la priorité des transferts et FIFO entre demandes de même priorité.

La priorité s'applique aux messages encore dans la file. Elle n'interrompt pas une transaction déjà extraite. Deux employés peuvent terminer dans un ordre différent de leur ordre d'extraction. Une arrivée continue de transferts pourrait retarder les consultations : aucun mécanisme de vieillissement n'est ajouté.

Le test de priorité remplit une file temporaire avec les types 3, 2, 1, 2 avant de lire. Il vérifie réellement l'ordre 1, 2, 2, 3 et l'ordre d'arrivée entre les deux messages de type 2, puis supprime cette file.

## 7. Pourquoi conserver un mutex ?

Recevoir un message complet n'empêche pas les deux employés de modifier simultanément un compte. L'atomicité du transport et l'atomicité bancaire sont deux problèmes distincts.

Chaque employé verrouille `verrou_comptes` avant d'appeler `executer()`, puis le libère après avoir obtenu la réponse. Les validations et les modifications sont donc dans la même section critique.

Pour deux retraits de 80 € sur un solde de 100 €, le premier employé accepte et laisse 20 €. Le second voit ensuite 20 € et refuse. Il ne peut pas utiliser un solde lu avant le premier retrait.

Un transfert vérifie d'abord le propriétaire de la source, la validité du montant, la destination, la différence entre les comptes, les fonds et le plafond. Seulement ensuite il débite la source et crédite la destination, sous le même verrou. Un refus métier ne change aucun solde.

Ce verrou global est simple et évite les problèmes de prise de deux verrous de comptes. Les clients sont concurrents, mais les modifications de soldes sont sérialisées. C'est un compromis volontaire pour un projet de niveau intermédiaire.

## 8. Rôle des sémaphores et du tube au démarrage

La banque crée un sémaphore POSIX anonyme :

```c
sem_init(&employes_prets, 0, 0);
```

Le deuxième argument est zéro : le sémaphore est partagé entre les threads d'un même processus. Chaque employé signale son démarrage avec `sem_post`. Le thread principal de la banque effectue deux `sem_wait` avant de déclarer le service prêt.

La banque transmet ensuite un octet `P` dans un tube créé avant le `fork`. Le parent attend cet octet avec `read`. Cela sert de confirmation : le parent ne commence pas les transactions tant que les deux employés ne sont pas initialisés.

Le père ferme l'extrémité d'écriture et le fils celle de lecture. Après la confirmation, chacun ferme son descripteur restant. Si le serveur meurt avant d'écrire, le père reçoit une fin de fichier au lieu d'attendre éternellement un écrivain encore ouvert.

Un seul tube suffit, car cette confirmation ne va que de la banque vers le parent. Les échanges bancaires bidirectionnels utilisent leurs deux files distinctes.

## 8 bis. Mémoire partagée : le tableau de bord

### Pourquoi un segment partagé ?

Le cours compare les deux approches : une file de messages demande un appel système à chaque envoi ou lecture, alors qu'un segment attaché avec `shmat` se lit comme une variable ordinaire. Le projet utilise donc **les deux, chacune pour ce qu'elle fait bien** :

- les **files** transportent les transactions, car il faut une demande, une priorité et une réponse adressée au bon client ;
- la **mémoire partagée** expose l'état de la banque (six soldes, total, compteurs de requêtes), que le processus principal lit sans déranger les employés et sans passer par la file.

### Contenu et création

```c
typedef struct {
    sem_t verrou;         /* sémaphore binaire entre processus */
    TableauBord donnees;  /* soldes[6], total, reussies, refusees */
} SegmentPartage;
```

Dans `banque_demarrer()`, **avant** le `fork` de la banque :

```c
segment = shmget(IPC_PRIVATE, sizeof(SegmentPartage), IPC_CREAT | 0600);
partage = shmat(segment, NULL, 0);   /* NULL : le système choisit l'adresse */
sem_init(&partage->verrou, 1, 1);    /* 1 = entre processus, valeur 1 = libre */
```

Le fils banque hérite de l'attachement : il n'a pas besoin de rappeler `shmat`. Comme pour les files, `IPC_PRIVATE` suffit car les processus ont un lien de parenté (pas besoin de `ftok`).

### Pourquoi le sémaphore est DANS le segment

Le cours montre `sem_init(&s, 1, 0)` pour synchroniser un père et un fils. Ce `1` ne suffit pas à lui seul : si `s` était une variable globale ordinaire, `fork` en ferait une **copie** dans le fils, et père et fils manipuleraient deux sémaphores différents. Le sémaphore doit donc être placé dans une mémoire réellement partagée, ici le segment lui-même.

### Synchronisation

La mémoire partagée n'offre aucune synchronisation native. Sans protection, le principal pourrait lire le tableau pendant qu'un employé n'a recopié que la moitié des soldes, et voir un total faux.

- **Écriture** (`publier()` dans la banque) : `sem_wait` (P), recopie des six soldes, du total et du compteur, `sem_post` (V).
- **Lecture** (`banque_lire_tableau()` dans le principal) : P, copie de toute la structure, V. On travaille ensuite sur la copie locale.

`publier()` est appelée par l'employé **pendant qu'il tient encore le mutex** des comptes, juste après `executer()`. Deux conséquences :

1. Les publications se font dans le même ordre que les transactions : le tableau n'est jamais « en retard » d'une opération puis « en avance » d'une autre.
2. Elle a lieu **avant** l'envoi de la réponse : quand un client reçoit sa réponse, le tableau en tient déjà compte.

Les verrous sont toujours pris dans le même ordre (mutex puis sémaphore) et le lecteur ne prend que le sémaphore : aucun interblocage n'est possible.

Côté lecture, l'attente est bornée avec `sem_timedwait` (cinq secondes) : si la banque était tuée par SIGKILL exactement pendant qu'elle tenait le sémaphore, le principal ne resterait pas bloqué.

### Fin de vie du segment

- La banque appelle `shmdt` avant `_exit` (le noyau le ferait aussi automatiquement).
- Le principal, **après** `waitpid` de la banque, appelle `sem_destroy`, `shmdt`, puis `shmctl(segment, IPC_RMID, NULL)`.

Comme le cours l'indique, `shmdt` ne détruit pas le segment : seul `shmctl(IPC_RMID)` le fait. Après `shmdt`, l'adresse ne doit plus être utilisée ; c'est pourquoi `partage` est remis à `NULL` et `banque_lire_tableau()` renvoie alors 0.

`banque_total()` lit maintenant le total dans ce segment. Avant, il passait par une requête interne spéciale dans la file ; cette requête a été retirée.

## 9. Signaux et fermeture

Dans le client, `sigaction` associe SIGINT et SIGTERM à un gestionnaire minimal qui positionne un `volatile sig_atomic_t`. On n'appelle pas `printf`, `waitpid` ou le nettoyage IPC depuis ce gestionnaire. Le code normal observe le drapeau et ferme la session.

La banque hérite de ces deux signaux bloqués avant la création de ses threads. Ses employés héritent aussi du masque. Le thread principal les reçoit de manière synchrone avec `sigwait`, sans exécuter un gestionnaire au milieu d'une transaction.

Pour quitter normalement :

1. Le parent attend la fin des processus clients de la démonstration.
2. Il envoie SIGTERM à la banque avec `kill`.
3. La banque sort de `sigwait` et supprime les deux files avec `msgctl(..., IPC_RMID, NULL)`.
4. Les employés bloqués sur `msgrcv` ou `msgsnd` sont réveillés avec une erreur de suppression.
5. La banque attend ses employés avec `pthread_join`, détruit son sémaphore et son mutex, détache le segment avec `shmdt`, puis termine.
6. Le parent récolte la banque avec `waitpid`, effectue un nettoyage de secours des files, puis détruit le sémaphore partagé et supprime le segment avec `shmctl(..., IPC_RMID, NULL)`.

Les identifiants des files restent constants pendant que les employés les utilisent. On les invalide dans le parent seulement après l'arrêt.

SIGKILL ne peut pas être intercepté. Si le parent et le serveur sont tous les deux tués ainsi, aucun de leurs codes de nettoyage ne peut tourner. Les identifiants affichés au lancement permettent un nettoyage ciblé avec `ipcrm -q` (files) et `ipcrm -m` (segment).

## 10. Attentes et erreurs de communication

Les employés attendent de manière bloquante sur une file vide. Les clients utilisent `IPC_NOWAIT`, une pause d'une milliseconde avec `nanosleep` et une échéance de cinq secondes mesurée avec `CLOCK_MONOTONIC`. Cela limite l'attente si le serveur disparaît sans avoir supprimé ses files.

Ce compromis est plus facile à comprendre qu'un mécanisme complet de surveillance et de reconnexion. Il entraîne de petites vérifications périodiques côté client, et un léger coût de latence dans les tests.

`SERVICE_INDISPONIBLE` signifie qu'une réponse n'a pas pu être confirmée. Une demande déjà envoyée a pu être exécutée avant la perte de sa réponse. Il ne faut donc pas renvoyer automatiquement un dépôt ou un retrait : le programme quitte la session après cette erreur dans les opérations manuelles. Les refus métier, comme les fonds insuffisants, sont des résultats confirmés et permettent de continuer.

Les données ne sont pas persistantes ; une session n'a pas de reprise après panne. Le numéro de requête sert à reconnaître les réponses, pas à rendre les transactions rejouables.

## 11. Montants et contrôles

Les montants sont des entiers en centimes. Pour lire `12,50`, le programme construit `12 * 100 + 50`, sans nombre flottant. Il refuse une troisième décimale, les valeurs non positives et les montants au-delà du plafond.

Le crédit vérifie la place restante avec `SOLDE_MAX - solde` avant d'additionner. Le transfert conserve le total de la banque. Un dépôt l'augmente et un retrait le diminue. Le total est calculé par la banque sous le mutex, puis publié dans le segment partagé ; le parent ne recalcule jamais à partir d'une copie locale ancienne des comptes.

## 12. Démonstration et tests

Chaque client de démonstration dépose 100 €, retire 25 €, transfère 50 € vers son deuxième compte et 10 € au client suivant, tente un retrait excessif puis consulte son solde.

Sur une banque neuve, le total passe de 6 000 € à 6 225 €. Les comptes 1, 3, 5 terminent à 1 025 € ; les comptes 2, 4, 6 à 1 050 €. Les affichages intermédiaires dépendent de l'ordre d'exécution.

Les tests C gardent les vérifications des règles initiales et créent maintenant 24 processus producteurs. Un tube libère leur départ ; un autre collecte leurs compteurs de résultats. Chaque processus effectue 100 demandes. Les résultats écrits dans le tube sont plus petits que `PIPE_BUF` et envoyés en une écriture, tandis que le lecteur gère une éventuelle lecture partielle.

Le premier scénario concurrence des retraits de 10 € sur un compte à 1 000 € : exactement 100 réussites, puis des refus. Le second réalise 2 400 transferts d'un centime dans les deux sens : tous réussissent et les soldes initiaux sont retrouvés.

Après chaque scénario concurrent, les tests C lisent aussi le tableau partagé : 100 réussites et 2 300 refus pour les retraits, 2 400 réussites et aucun refus pour les transferts. Aucune mise à jour n'est perdue malgré les deux employés.

Les tests système vérifient les PID, les trois threads de la banque, les files réelles dans `/proc/sysvipc/msg` et le segment dans `/proc/sysvipc/shm`, les signaux, la disparition du serveur, le nettoyage IPC, l'indépendance de deux sessions et l'affichage du tableau de bord.

## 13. Lien avec les cours et ordre de lecture

| Support fourni | Partie mise en pratique |
|---|---|
| Processus, pages PDF 10 à 13 | `fork`, PID, attente et fin des enfants |
| Tubes, pages PDF 2 à 8 | Tube de démarrage, fermeture des extrémités, lectures/écritures |
| Signaux, pages PDF 3 à 5 et 9 à 11 | SIGINT/SIGTERM, envoi, gestion et masquage ; `sigwait` complète le mécanisme |
| Files de messages, pages PDF 3 à 7 | Messages structurés, types, files System V et suppression |
| Threads et sémaphores, pages PDF 5 à 12 et 15 à 25 | Employés, attente, mutex, sections critiques, sémaphores POSIX entre threads et entre processus |
| Mémoire partagée (shm) | `shmget`, `shmat`, `shmdt`, `shmctl(IPC_RMID)` pour le tableau de bord, synchronisé par sémaphore |

La version utilise les notions de chacun des six supports sans chercher à appeler toutes les fonctions citées. Elle ne nécessite pas `exec`, `alarm`, `mkfifo`, `dup2`, ni la mémoire partagée POSIX (`shm_open`, `mmap`).

Pour comprendre le code, commence par `banque.h`, puis `executer` dans `banque.c`. Suis ensuite `banque_envoyer`, `travailler`, `banque_demarrer` et `servir`. Termine par la démonstration dans `main.c` et les tests.

Pour présenter le projet : « Les clients sont des processus distincts qui communiquent avec une banque par deux files System V. Deux threads employés sélectionnent les demandes selon leur type. Un mutex protège les comptes et les transferts. Un sémaphore et un tube confirment le démarrage. La banque publie son état dans un segment de mémoire partagée, protégé par un sémaphore entre processus, que le processus principal lit directement. Les signaux permettent l'arrêt ; le serveur supprime ses files et le parent attend les processus enfants. »
