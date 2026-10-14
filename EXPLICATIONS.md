# Comprendre le projet, étape par étape

## 1. L'idée générale

Imagine une banque avec plusieurs clients et deux employés. Un client ne modifie jamais directement les comptes. Il écrit une demande, la dépose dans une file, puis attend une réponse. Un employé récupère une demande, contrôle sa validité et réalise l'opération.

Dans le programme, un employé est un **thread** : un chemin d'exécution indépendant à l'intérieur du processus. Les trois clients de démonstration ont également chacun leur thread. Ils peuvent avancer en concurrence, selon les décisions du système d'exploitation.

Le menu est exécuté par le thread principal. Quand tu utilises le menu, il joue le rôle d'un client ; quand tu lances la démonstration, il crée les trois threads clients puis attend leur fin.

```text
Client Alice ----\
Client Bob -------+--> file de requêtes --> employé 1 ou employé 2
Client Charlie --/                               |
                                                v
                                   validation et accès aux comptes
                                                |
                      client réveillé <-- réponse succès/échec
```

Les comptes sont partagés. Les employés ont donc besoin de se coordonner pour éviter de les modifier simultanément de façon incohérente.

## 2. Les données principales

### Un compte

Dans `banque.c`, un compte contient deux champs :

```c
typedef struct {
    int proprietaire;
    Montant solde;
} Compte;
```

Le propriétaire est un identifiant client entre 1 et 3. Le solde est un entier `long long`, renommé `Montant` pour rendre le code plus clair. Les six comptes sont rangés dans un tableau.

L'utilisateur voit les numéros 1 à 6 ; en C, les indices vont de 0 à 5. Pour accéder au compte numéro `source`, on utilise donc `comptes[source - 1]`, après avoir vérifié que ce numéro existe.

### Pourquoi des centimes ?

Un montant de 12,50 € est enregistré comme `1250`. Avec les nombres flottants, certaines valeurs décimales ne sont pas représentées exactement. En centimes, les additions et soustractions restent exactes.

Pour afficher `1250`, la division entière `1250 / 100` donne 12 euros et le reste `1250 % 100` donne 50 centimes. Le format `%02lld` affiche toujours deux chiffres pour les centimes.

Chaque compte est limité à un milliard d'euros. On vérifie la place restante avec `SOLDE_MAX - solde` avant un crédit. Les montants invalides sont refusés avant les calculs bancaires.

### Une requête et une réponse

Une `Requete` indique : le client, l'opération, le compte source, le compte destinataire et le montant. Elle contient aussi une `Reponse` et un sémaphore `terminee`.

La réponse contient un statut et le solde source à l'instant du traitement. Un statut peut être `SUCCES`, `FONDS_INSUFFISANTS`, `ACCES_REFUSE`, etc. Un solde de `-1` indique qu'on ne peut pas fournir le solde, notamment si le compte n'appartient pas au client.

Les types `enum` remplacent des nombres difficiles à comprendre par des noms : `TRANSFERT` est plus lisible que `3` dans la logique bancaire.

## 3. Le trajet exact d'une demande

La fonction publique est :

```c
Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant);
```

Par exemple :

```c
Reponse resultat = banque_envoyer(1, TRANSFERT, 1, 3, 5000);
```

Cela signifie : Alice demande un transfert de 50 € du compte 1 vers le compte 3.

Voici les étapes :

1. Le thread client construit sa requête locale `r`.
2. Il crée `r.terminee`, initialisé à zéro.
3. Il attend une place libre dans la file.
4. Il verrouille la file, y ajoute l'adresse `&r`, puis libère le verrou.
5. Il signale qu'une demande est disponible.
6. Il attend le signal `r.terminee`, sans effectuer de boucle d'attente active.
7. Un employé extrait la demande et libère une place dans la file.
8. L'employé verrouille les comptes, exécute l'opération, écrit la réponse puis libère le verrou.
9. L'employé signale `r.terminee` : le client peut reprendre.
10. Le client détruit son sémaphore et renvoie la réponse à son appelant.

Pourquoi stocker une adresse vers une variable locale est-il valide ici ? Parce que `banque_envoyer` reste en attente tant que l'employé n'a pas terminé. Sa variable `r` existe donc encore pendant tout le traitement. Après le signal, l'employé ne doit plus accéder à cette requête. Une version où le client retourne immédiatement nécessiterait une autre gestion de sa durée de vie.

## 4. Les mutex : protéger les données

Un **mutex** donne l'accès à une zone critique à un seul thread à la fois. Les autres attendent sa libération.

### Le verrou de la file

`verrou_file` protège le tableau des requêtes et `taille_file`. Sans lui, deux clients pourraient lire la même taille, écrire au même emplacement et perdre une demande.

Les opérations « ajouter une requête » et « choisir puis retirer une requête » sont chacune effectuées entièrement sous ce verrou.

### Le verrou des comptes

`verrou_comptes` protège la vérification et la modification des soldes. Même une consultation prend ce verrou pour ne pas lire un état intermédiaire.

Supposons un compte de 100 € et deux retraits simultanés de 80 €. Sans protection, les deux employés pourraient voir 100 € et accepter tous les deux. Avec le verrou :

1. Le premier employé lit 100 €, accepte le retrait et laisse 20 €.
2. Il libère le verrou.
3. Le second lit 20 € et refuse le retrait.

On protège donc **la vérification et la modification ensemble**, pas seulement l'écriture finale.

### Pourquoi un seul verrou pour tous les comptes ?

C'est plus simple à expliquer et évite de gérer deux verrous dans chaque transfert. Les requêtes arrivent de plusieurs clients et deux employés les prennent en charge, mais les modifications bancaires sont sérialisées par ce verrou global.

Une banque industrielle pourrait utiliser un verrou par compte pour traiter davantage d'opérations simultanément. Cela demanderait notamment un ordre fixe de prise des verrous pour éviter les interblocages. Ce degré de complexité n'est pas nécessaire ici.

Les employés libèrent le verrou de la file avant de prendre celui des comptes. Ils ne gardent donc pas ces deux verrous simultanément.

## 5. Les sémaphores : attendre un événement

Un **sémaphore** possède un compteur. Attendre consomme une unité ; s'il n'y en a aucune, le thread est suspendu. Signaler ajoute une unité et permet à un thread en attente de reprendre.

| Sémaphore | Valeur initiale | Signification |
|---|---|---|
| `places` | 16 | Emplacements disponibles dans la file |
| `disponibles` | 0 | Requêtes à récupérer, puis signaux de fermeture |
| `r.terminee` | 0 | Réponse disponible pour cette requête précise |

Quand la file est pleine, les clients attendent une place au lieu d'écrire hors du tableau. Quand elle est vide, les employés attendent une demande. Chaque client reçoit sa propre réponse, grâce à son sémaphore dédié.

Un mutex et un sémaphore n'ont donc pas exactement le même rôle : le mutex protège une donnée ; le sémaphore sert ici à attendre une ressource ou une réponse.

## 6. Les priorités

Les niveaux sont définis dans `priorite` :

| Opération | Priorité |
|---|---|
| Transfert | 3 |
| Dépôt ou retrait | 2 |
| Consultation | 1 |

La fonction `extraire` parcourt la file et retient la première demande ayant la plus grande priorité. Elle décale ensuite les éléments pour supprimer la demande sélectionnée.

Exemple : si la file contient consultation, dépôt, transfert, retrait, l'ordre de sélection sera transfert, dépôt, retrait, consultation.

Le test utilise `>` et pas `>=`. Ainsi, deux demandes de même priorité conservent leur ordre d'arrivée dans la file : c'est FIFO, « premier entré, premier sorti ».

Cette priorité s'applique aux demandes **encore présentes dans la file**. Elle n'interrompt pas une opération déjà extraite. Avec deux employés, l'ordre des réponses ou de prise du verrou des comptes peut différer de l'ordre d'extraction. Il ne s'agit pas non plus de régler les priorités des threads dans Windows ou Linux.

Une arrivée continue de transferts pourrait retarder longtemps les consultations. Ce projet utilise une priorité stricte, sans mécanisme de vieillissement ; la démonstration est finie, donc toutes ses requêtes finissent par être traitées.

## 7. L'atomicité d'un transfert

Un transfert doit retirer de l'argent à la source **et** ajouter exactement la même somme à la destination.

La fonction `executer` vérifie le client, l'existence et la propriété du compte source, l'opération, le montant, la destination, la différence entre les deux comptes, les fonds disponibles et le plafond du destinataire.

Toutes les validations précèdent ces deux instructions :

```c
source->solde -= r->montant;
destination->solde += r->montant;
```

Elles sont exécutées sous le même mutex. Aucun autre employé ne peut observer l'état situé entre les deux lignes. Si une validation échoue, aucune des deux lignes n'est exécutée : aucun remboursement correctif n'est nécessaire.

L'atomicité garantie ici concerne la concurrence dans le programme en fonctionnement. Ce n'est pas une garantie de conservation après un arrêt brutal de la machine : il n'y a ni sauvegarde ni journal durable.

Un transfert conserve le total de la banque. Un dépôt l'augmente ; un retrait le diminue. Cette propriété sert directement dans les tests.

## 8. La communication et le sens du mot IPC

Au sens strict, IPC désigne la communication entre processus. Ici, le sujet autorise aussi les threads distincts. Nous retenons cette possibilité et utilisons leur mémoire commune pour échanger des messages.

La file contient des pointeurs vers les requêtes. C'est possible parce que tous les threads appartiennent au même processus et partagent le même espace d'adressage. Un autre processus indépendant ne pourrait pas utiliser directement ces pointeurs.

Il s'agit donc d'une **file applicative en mémoire partagée entre threads**, et non d'une file de messages du système d'exploitation. Ce choix satisfait la variante par threads du sujet et reste accessible à un niveau intermédiaire.

## 9. Comprendre la démonstration

Chaque client effectue successivement : dépôt de 100 €, retrait de 25 €, transfert de 50 € vers son deuxième compte, transfert de 10 € au client suivant, tentative de retrait excessif et consultation.

Les trois clients exécutent leur liste en concurrence. Chaque client attend cependant la réponse de sa demande avant d'envoyer la suivante.

Sur une banque neuve :

- Le total initial est `6 × 1 000 = 6 000 €`.
- Les dépôts ajoutent `3 × 100 = 300 €`.
- Les retraits acceptés enlèvent `3 × 25 = 75 €`.
- Les transferts ne changent pas le total.
- Les trois retraits excessifs sont refusés.
- Le total final est donc `6 225 €`.

Les comptes 1, 3 et 5 terminent à 1 025 € ; les comptes 2, 4 et 6 à 1 050 €. Les soldes intermédiaires affichés peuvent varier selon l'ordre des transactions. Le mutex `affichage` évite de mélanger les lignes produites par les clients ; il ne protège pas les comptes.

## 10. Démarrage et fermeture

`banque_demarrer` initialise les comptes, les verrous et les sémaphores avant de créer les employés. Il faut l'appeler une seule fois par session bancaire.

À la fermeture, le programme attend d'abord la fin de tous les clients. Puis `banque_arreter` positionne le drapeau d'arrêt et envoie deux signaux pour réveiller les deux employés éventuellement bloqués sur une file vide. Chaque employé constate que la file est vide et que l'arrêt est demandé, puis termine.

Le thread principal attend ensuite les employés avant de détruire les outils de synchronisation. Il ne faut pas appeler `banque_envoyer` après la fermeture, ni arrêter la banque alors que des clients continuent d'envoyer des demandes.

Les erreurs système de création ou d'attente des outils de synchronisation arrêtent le programme. Les refus métier, comme un solde insuffisant, renvoient simplement un statut et permettent de continuer.

## 11. Windows et Linux

`synchronisation.h` rassemble les différences entre systèmes : `CreateThread` et les objets Windows d'un côté, `pthread_create` et les sémaphores POSIX de l'autre.

`#ifdef _WIN32` choisit la branche Windows pendant la compilation. Le reste du programme utilise des noms simples comme `thread_creer`, `mutex_prendre` ou `semaphore_attendre`.

Le mot `static inline` permet de définir ces petites fonctions dans l'en-tête sans conflit entre les fichiers C qui l'incluent. Il n'est pas nécessaire de maîtriser toute l'API Windows pour comprendre la logique bancaire.

## 12. Ce que les tests vérifient

Les tests fonctionnels vérifient les soldes exacts, les transferts internes et externes, le retrait du solde exact, les droits sur les comptes, les entrées invalides, les fonds insuffisants et les plafonds. Ils vérifient aussi qu'un transfert refusé ne modifie aucun de ses deux comptes.

Le test de priorité prépare une file connue sans signaler de travail aux employés, puis contrôle directement l'ordre d'extraction. Il est déterministe et ne dépend pas du hasard de l'ordonnanceur.

Deux scénarios utilisent chacun 24 threads et 100 requêtes par thread :

1. Tous tentent de retirer 10 € du compte 1, initialement à 1 000 €. Exactement 100 retraits doivent réussir ; le solde final doit être zéro et les autres refus doivent être des insuffisances de fonds.
2. La moitié transfère un centime du compte 1 au compte 3 ; l'autre moitié fait l'inverse. Les 2 400 transferts doivent réussir, les deux soldes doivent retrouver leur valeur initiale et le total rester à 6 000 €.

Un sémaphore de départ rassemble les producteurs avant de les libérer. Il favorise leur concurrence sans garantir une exécution réellement simultanée sur 24 cœurs. Les 24 producteurs dépassent les 16 places de la file et sollicitent sa capacité limitée ; le test ne mesure pas le taux de remplissage.

## 13. Ordre de lecture conseillé et présentation orale

Lis d'abord les types dans `banque.h`, puis `executer` pour comprendre les règles bancaires. Suis ensuite `banque_envoyer` et `travailler` pour comprendre l'échange. Termine par la file prioritaire, le menu, les tests et l'adaptation système.

Pour présenter le projet, tu peux dire :

> J'ai choisi la variante par threads autorisée par le sujet. Les clients déposent leurs requêtes dans une file partagée. Deux employés sélectionnent les demandes selon leur priorité. Un mutex protège la file et un autre protège les validations et modifications des comptes. Les sémaphores servent à attendre une place, une requête ou une réponse. Les montants sont stockés en centimes. Un transfert refusé ne modifie aucun solde et les tests concurrents vérifient notamment l'absence de découvert et la conservation de l'argent lors des transferts.

Le projet est une simulation en console avec des identités fixes, des données temporaires et un verrou global. Ces choix sont assumés pour garder le code lisible et concentrer le travail sur la concurrence demandée dans le sujet.
