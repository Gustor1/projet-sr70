/*
 * banque.c - Processus banque et communication avec les clients.
 *
 * Organisation des processus (creee par banque_demarrer) :
 *
 *   processus principal (menu / clients)
 *     |-- fork + exec --> journal : lit le tube nomme, ecrit build/journal.txt
 *     |-- fork --------> banque  : thread principal + NB_EMPLOYES threads employes
 *
 * Communications utilisees :
 *   - 2 files de messages System V : requetes (clients -> banque)
 *     et reponses (banque -> clients) ;
 *   - 1 tube anonyme : la banque previent le pere qu'elle est prete ;
 *   - 1 tube nomme   : la banque envoie une ligne par operation au journal ;
 *   - 1 segment de memoire partagee System V : tableau de bord (soldes, total) ;
 *   - 1 memoire partagee POSIX : compteur de lignes tenu par le journal.
 *
 * Synchronisation :
 *   - mutex verrou_comptes : un seul employe modifie les comptes a la fois ;
 *   - semaphore employes_prets : attente du demarrage des employes ;
 *   - semaphores places DANS les memoires partagees : acces entre processus.
 */
#include "banque.h"
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NB_EMPLOYES 2
#define DELAI_MAX 5 /* secondes d'attente maximum pour une reponse */
#define PROGRAMME_JOURNAL "build/journal"
#define FICHIER_JOURNAL "build/journal.txt"

/* ===================== Types internes ===================== */

typedef struct { int proprietaire; Montant solde; } Compte;

/* Message de requete. Le champ type est obligatoirement le premier (long > 0) :
   il sert de priorite (1 = transfert, 2 = depot/retrait, 3 = consultation). */
typedef struct {
    long type;
    struct {
        pid_t pid;          /* PID du client, pour lui renvoyer la reponse */
        unsigned numero;    /* numero de requete, pour reconnaitre la bonne reponse */
        int client, operation, source, destination;
        Montant montant;    /* en centimes */
    } corps;
} MessageRequete;

/* Message de reponse : son type est le PID du client destinataire,
   ce qui permet a chaque client de ne lire que ses propres reponses. */
typedef struct {
    long type;
    struct { unsigned numero; Reponse resultat; } corps;
} MessageReponse;

/* Contenu du segment de memoire partagee System V.
   Le semaphore est place DANS le segment : une variable globale ordinaire
   serait copiee par fork, et chaque processus aurait son propre semaphore. */
typedef struct {
    sem_t verrou;
    TableauBord donnees;
} SegmentPartage;

/* ===================== Variables globales ===================== */

/* Les comptes n'existent que dans le processus banque : les clients ne
   peuvent les lire ou les modifier qu'en envoyant un message. */
static Compte comptes[NB_COMPTES];
static pthread_mutex_t verrou_comptes;   /* protege comptes[] entre les employes */
static sem_t employes_prets;             /* semaphore entre threads (pshared = 0) */

static int demandes = -1, reponses = -1; /* identifiants des files de messages */
static int segment = -1;                 /* identifiant du segment System V */
static SegmentPartage *partage = NULL;   /* adresse du segment (shmat) */
static CompteurJournal *compteur = NULL; /* adresse de la memoire POSIX (mmap) */
static pid_t serveur = -1, journal = -1, proprietaire = -1;
static int tube_journal = -1;            /* extremite ecriture du tube nomme (banque) */
static char chemin_tube[64], nom_posix[64];

/* Modifiees par les gestionnaires de signaux : volatile sig_atomic_t. */
static volatile sig_atomic_t interruption, delai_depasse;

/* ===================== Signaux ===================== */

/* SIGINT / SIGTERM : on se contente de lever un drapeau. Le nettoyage est
   fait ensuite par le code normal (printf, free, etc. sont interdits ici). */
static void recevoir_signal(int signal_recu) {
    (void)signal_recu;
    interruption = 1;
}

/* SIGALRM : la reponse de la banque n'est pas arrivee a temps. */
static void recevoir_alarme(int signal_recu) {
    (void)signal_recu;
    delai_depasse = 1;
}

int banque_interrompue(void) { return interruption != 0; }

/* ===================== Fonctions utilitaires ===================== */

const char *banque_client(int client) {
    static const char *noms[] = { "Alice", "Bob", "Charlie" };
    return client >= 1 && client <= NB_CLIENTS ? noms[client - 1] : "Inconnu";
}

const char *banque_message(Statut statut) {
    switch (statut) {
        case SUCCES: return "Succes";
        case CLIENT_INVALIDE: return "Client inexistant";
        case COMPTE_INVALIDE: return "Compte inexistant";
        case ACCES_REFUSE: return "Ce compte n'appartient pas au client";
        case MONTANT_INVALIDE: return "Montant invalide";
        case FONDS_INSUFFISANTS: return "Solde insuffisant";
        case PLAFOND_DEPASSE: return "Plafond du compte depasse";
        case MEME_COMPTE: return "Les deux comptes sont identiques";
        case SERVICE_INDISPONIBLE: return "Service indisponible/interruption : resultat non confirme";
        default: return "Operation inconnue";
    }
}

/* Priorite = type du message. Les employes lisent avec msgrcv(..., -3, ...) :
   le message de plus PETIT type passe en premier, donc les transferts. */
static long priorite(int operation) {
    if (operation == TRANSFERT) return 1;
    if (operation == DEPOT || operation == RETRAIT) return 2;
    return 3;
}

/* ===================== Regles bancaires ===================== */

/* Execute une operation. Appelee SOUS le mutex verrou_comptes.
   Toutes les verifications sont faites avant la moindre modification :
   si une operation est refusee, aucun solde n'est change (atomicite).
   Retourne le statut et le solde du compte source apres l'operation. */
static Reponse executer(const MessageRequete *message) {
    const int client = message->corps.client;
    const int operation = message->corps.operation;
    const int numero_source = message->corps.source;
    const int numero_destination = message->corps.destination;
    const Montant montant = message->corps.montant;
    Reponse rep = { SUCCES, -1 };
    Compte *source, *destination;
    if (client < 1 || client > NB_CLIENTS) {
        rep.statut = CLIENT_INVALIDE; return rep;
    }
    if (numero_source < 1 || numero_source > NB_COMPTES) {
        rep.statut = COMPTE_INVALIDE; return rep;
    }
    source = &comptes[numero_source - 1];
    if (source->proprietaire != client) {   /* un client n'agit que sur ses comptes */
        rep.statut = ACCES_REFUSE; return rep;
    }
    rep.solde = source->solde;
    if (operation < DEPOT || operation > CONSULTATION) {
        rep.statut = OPERATION_INVALIDE; return rep;
    }
    if (operation == CONSULTATION) return rep;
    if (montant <= 0 || montant > SOLDE_MAX) {
        rep.statut = MONTANT_INVALIDE; return rep;
    }
    if (operation == DEPOT) {
        /* Comparaison sous forme de soustraction : evite un depassement d'entier. */
        if (montant > SOLDE_MAX - source->solde) rep.statut = PLAFOND_DEPASSE;
        else source->solde += montant;
    } else if (operation == RETRAIT) {
        if (montant > source->solde) rep.statut = FONDS_INSUFFISANTS;
        else source->solde -= montant;
    } else { /* TRANSFERT, vers un compte du meme client ou d'un autre client */
        if (numero_destination < 1 || numero_destination > NB_COMPTES) {
            rep.statut = COMPTE_INVALIDE; return rep;
        }
        if (numero_source == numero_destination) {
            rep.statut = MEME_COMPTE; return rep;
        }
        destination = &comptes[numero_destination - 1];
        if (montant > source->solde) rep.statut = FONDS_INSUFFISANTS;
        else if (montant > SOLDE_MAX - destination->solde) rep.statut = PLAFOND_DEPASSE;
        else {
            /* Debit et credit dans la meme section critique : aucun autre
               employe ne peut voir l'etat intermediaire. */
            source->solde -= montant;
            destination->solde += montant;
        }
    }
    rep.solde = source->solde;
    return rep;
}

/* ===================== Processus banque ===================== */

/* Recopie l'etat des comptes dans le segment partage (tableau de bord).
   Appelee sous le mutex : les verrous sont toujours pris dans le meme ordre
   (mutex puis semaphore), ce qui evite tout interblocage. */
static void publier(const Reponse *resultat) {
    int i;
    while (sem_wait(&partage->verrou) == -1 && errno == EINTR) { } /* P() */
    partage->donnees.total = 0;
    for (i = 0; i < NB_COMPTES; i++) {
        partage->donnees.soldes[i] = comptes[i].solde;
        partage->donnees.total += comptes[i].solde;
    }
    if (resultat && resultat->statut == SUCCES) partage->donnees.reussies++;
    else if (resultat) partage->donnees.refusees++;
    sem_post(&partage->verrou);                                    /* V() */
}

/* Envoie une ligne decrivant l'operation au journal, par le tube nomme.
   Une ligne fait moins de PIPE_BUF octets : write l'ecrit d'un seul bloc,
   donc les lignes des deux employes ne peuvent pas se melanger. */
static void journaliser(const MessageRequete *req, const Reponse *resultat) {
    static const char *noms[] = { "?", "DEPOT", "RETRAIT", "TRANSFERT", "CONSULTATION" };
    int operation = req->corps.operation;
    char ligne[200], detail[80] = "";
    int n;
    if (tube_journal == -1) return;
    if (operation < DEPOT || operation > CONSULTATION) operation = 0;
    if (operation == TRANSFERT)
        snprintf(detail, sizeof detail, " vers compte %d", req->corps.destination);
    if (operation != CONSULTATION)
        snprintf(detail + strlen(detail), sizeof detail - strlen(detail), " : %lld,%02lld EUR",
                 req->corps.montant / 100, llabs(req->corps.montant % 100));
    n = snprintf(ligne, sizeof ligne, "%s (%s) compte %d%s -> %s\n",
                 noms[operation], banque_client(req->corps.client), req->corps.source,
                 detail, banque_message(resultat->statut));
    /* Si le journal s'est arrete, write echoue (EPIPE) : on continue sans lui. */
    if (write(tube_journal, ligne, (size_t)n) == -1) tube_journal = -1;
}

/* Code d'un thread employe : boucle de traitement des requetes. */
static void *travailler(void *argument) {
    (void)argument;
    sem_post(&employes_prets); /* signale au thread principal que l'employe est pret */
    for (;;) {
        MessageRequete req;
        MessageReponse rep = {0};
        /* Attente bloquante ; -3 = message le plus prioritaire de type <= 3. */
        ssize_t taille = msgrcv(demandes, &req, sizeof req.corps, -3, 0);
        if (taille == -1) {
            if (errno == EINTR) continue;
            break; /* EIDRM : file supprimee a l'arret de la banque */
        }
        rep.type = (long)req.corps.pid;
        rep.corps.numero = req.corps.numero;

        pthread_mutex_lock(&verrou_comptes);      /* debut de section critique */
        rep.corps.resultat = executer(&req);
        publier(&rep.corps.resultat);             /* tableau a jour AVANT la reponse */
        journaliser(&req, &rep.corps.resultat);   /* lignes dans l'ordre d'execution */
        pthread_mutex_unlock(&verrou_comptes);    /* fin de section critique */

        if (msgsnd(reponses, &rep, sizeof rep.corps, 0) == -1 && errno != EINTR) break;
    }
    return NULL;
}

/* Supprime les deux files de messages (IPC_RMID).
   Les threads bloques dans msgrcv sont alors reveilles avec l'erreur EIDRM. */
static void supprimer_files(void) {
    if (demandes != -1) msgctl(demandes, IPC_RMID, NULL);
    if (reponses != -1) msgctl(reponses, IPC_RMID, NULL);
}

/* Code du processus banque (fils). Ne retourne jamais.
   SIGINT et SIGTERM ont ete bloques avant le fork : les threads crees ici
   heritent de ce masque, et seul le thread principal les recoit, de facon
   synchrone, avec sigwait. Aucun employe n'est donc interrompu en pleine
   transaction. */
static void servir(int tube_pret, const sigset_t *signaux) {
    pthread_t employes[NB_EMPLOYES];
    int i, signal_recu;
    char pret = 'P';
    signal(SIGPIPE, SIG_IGN); /* journal arrete : write renvoie une erreur au lieu de tuer la banque */

    /* L'ouverture d'un tube nomme bloque jusqu'a ce que le journal ouvre l'autre bout. */
    tube_journal = open(chemin_tube, O_WRONLY);
    if (tube_journal == -1) perror("banque : ouverture du tube nomme (journal desactive)");

    if (pthread_mutex_init(&verrou_comptes, NULL) != 0 || sem_init(&employes_prets, 0, 0) == -1) {
        perror("banque : initialisation des verrous");
        _exit(EXIT_FAILURE);
    }
    for (i = 0; i < NB_COMPTES; i++) {
        comptes[i].proprietaire = i / 2 + 1; /* comptes 1-2 : Alice, 3-4 : Bob, 5-6 : Charlie */
        comptes[i].solde = 100000;           /* 1000 EUR en centimes */
    }
    publier(NULL); /* soldes initiaux visibles dans le tableau de bord */

    for (i = 0; i < NB_EMPLOYES; i++) {
        if (pthread_create(&employes[i], NULL, travailler, NULL) != 0) {
            fprintf(stderr, "banque : creation d'un employe impossible\n");
            _exit(EXIT_FAILURE); /* le pere voit la fin du tube et abandonne le demarrage */
        }
    }
    for (i = 0; i < NB_EMPLOYES; i++) {
        while (sem_wait(&employes_prets) == -1 && errno == EINTR) { }
    }

    /* Previent le pere que la banque est prete (tube anonyme). */
    if (write(tube_pret, &pret, 1) != 1) _exit(EXIT_FAILURE);
    close(tube_pret);

    sigwait(signaux, &signal_recu); /* attend SIGINT ou SIGTERM */

    /* Arret propre : la suppression des files reveille les employes. */
    supprimer_files();
    for (i = 0; i < NB_EMPLOYES; i++) pthread_join(employes[i], NULL);
    sem_destroy(&employes_prets);
    pthread_mutex_destroy(&verrou_comptes);
    if (tube_journal != -1) close(tube_journal); /* le journal lit alors la fin du tube */
    shmdt(partage); /* le pere supprimera le segment */
    _exit(EXIT_SUCCESS);
}

/* Code du fils journal (avant exec). Ne retourne jamais.
   dup2 remplace l'entree standard par le tube nomme ; apres execl, le
   programme journal lit donc simplement stdin sans connaitre le tube. */
static void lancer_journal(void) {
    int fd;
    /* Un signal ignore le reste apres exec : Ctrl+C n'arrete pas le journal
       avant qu'il ait recopie toutes les lignes. Il s'arrete a la fin du tube. */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    fd = open(chemin_tube, O_RDONLY); /* bloque jusqu'a l'ouverture par la banque */
    if (fd == -1 || dup2(fd, STDIN_FILENO) == -1) {
        perror("journal : redirection du tube nomme");
        _exit(127);
    }
    close(fd);
    execl(PROGRAMME_JOURNAL, "journal", FICHIER_JOURNAL, nom_posix, (char *)NULL);
    perror("journal : execl"); /* on arrive ici seulement si exec a echoue */
    _exit(127);
}

/* ===================== Demarrage et arret ===================== */

/* Cree toutes les ressources, puis le journal et la banque.
   Retourne 1 quand la banque est prete, 0 en cas d'erreur (tout est nettoye). */
int banque_demarrer(void) {
    int tube[2], fd;
    char pret = 0;
    ssize_t lu = 0;
    sigset_t signaux, ancien_masque;
    struct sigaction action = {0};
    interruption = 0;
    serveur = journal = -1;
    proprietaire = getpid();

    /* 1. Gestionnaires de signaux. Pas de SA_RESTART : un signal interrompt
          les appels bloquants (msgrcv renvoie -1 avec errno = EINTR). */
    sigemptyset(&action.sa_mask);
    action.sa_handler = recevoir_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = recevoir_alarme;
    sigaction(SIGALRM, &action, NULL);

    /* 2. Tube nomme banque -> journal. Son nom contient le PID : deux sessions
          lancees en meme temps ont chacune le leur. Il sert aussi de fichier
          de reference pour ftok. */
    snprintf(chemin_tube, sizeof chemin_tube, "/tmp/sr70-banque-%ld.fifo", (long)getpid());
    if (mkfifo(chemin_tube, 0600) == -1) {
        perror("mkfifo");
        chemin_tube[0] = '\0';
        return 0;
    }

    /* 3. Files de messages et segment System V. ftok fabrique une cle a partir
          du chemin du tube et d'un caractere different pour chaque objet. */
    demandes = msgget(ftok(chemin_tube, 'R'), IPC_CREAT | 0600);
    reponses = msgget(ftok(chemin_tube, 'A'), IPC_CREAT | 0600);
    segment = shmget(ftok(chemin_tube, 'S'), sizeof(SegmentPartage), IPC_CREAT | 0600);
    if (demandes == -1 || reponses == -1 || segment == -1) {
        perror("msgget/shmget");
        banque_arreter();
        return 0;
    }
    /* Attache AVANT le fork : la banque herite de l'attachement. */
    partage = shmat(segment, NULL, 0);
    if (partage == (void *)-1) {
        perror("shmat");
        partage = NULL;
        banque_arreter();
        return 0;
    }
    memset(partage, 0, sizeof *partage);
    if (sem_init(&partage->verrou, 1, 1) == -1) { /* 1 = partage entre processus, valeur 1 = libre */
        perror("sem_init");
        banque_arreter();
        return 0;
    }

    /* 4. Memoire partagee POSIX pour le compteur du journal. Elle a un nom :
          le journal la retrouvera apres son exec avec shm_open. */
    snprintf(nom_posix, sizeof nom_posix, "/sr70-journal-%ld", (long)getpid());
    fd = shm_open(nom_posix, O_CREAT | O_RDWR, 0600);
    if (fd == -1 || ftruncate(fd, sizeof(CompteurJournal)) == -1) {
        perror("shm_open/ftruncate");
        if (fd != -1) close(fd);
        banque_arreter();
        return 0;
    }
    compteur = mmap(NULL, sizeof(CompteurJournal), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* la projection reste valide apres close */
    if (compteur == MAP_FAILED) {
        perror("mmap");
        compteur = NULL;
        banque_arreter();
        return 0;
    }
    compteur->lignes = 0;
    sem_init(&compteur->verrou, 1, 1);

    /* 5. Processus journal (fork puis exec dans le fils). */
    fflush(NULL); /* evite que le fils reecrive des sorties encore en tampon */
    journal = fork();
    if (journal == 0) lancer_journal();
    if (journal == -1 || pipe(tube) == -1) {
        perror("fork journal/pipe");
        if (journal > 0) kill(journal, SIGKILL); /* il attendait encore la banque */
        banque_arreter();
        return 0;
    }

    /* 6. Processus banque. SIGINT/SIGTERM sont bloques juste avant le fork pour
          que la banque et ses threads naissent avec ce masque (voir servir). */
    sigemptyset(&signaux);
    sigaddset(&signaux, SIGINT);
    sigaddset(&signaux, SIGTERM);
    sigprocmask(SIG_BLOCK, &signaux, &ancien_masque);
    serveur = fork();
    if (serveur == 0) {
        close(tube[0]); /* le fils ecrit seulement */
        servir(tube[1], &signaux);
    }
    close(tube[1]);     /* le pere lit seulement */
    sigprocmask(SIG_SETMASK, &ancien_masque, NULL);

    /* 7. Attente de l'octet 'P' : la banque est prete. Si elle meurt avant,
          read renvoie 0 (fin de fichier) au lieu de bloquer indefiniment. */
    if (serveur != -1) {
        do { lu = read(tube[0], &pret, 1); } while (lu == -1 && errno == EINTR && !interruption);
    } else {
        perror("fork banque");
    }
    close(tube[0]);
    if (serveur == -1 || lu != 1 || interruption) {
        kill(journal, SIGKILL);
        banque_arreter();
        return 0;
    }
    return 1;
}

/* Arrete la banque et le journal, puis supprime toutes les ressources IPC.
   Peut etre appelee apres un demarrage partiel : chaque ressource n'est
   liberee que si elle a ete creee. Seul le processus createur nettoie. */
void banque_arreter(void) {
    int statut;
    if (getpid() != proprietaire) return; /* un client fils ne doit rien supprimer */
    if (serveur > 0) {
        kill(serveur, SIGTERM);
        while (waitpid(serveur, &statut, 0) == -1 && errno == EINTR) { }
        serveur = -1;
    }
    if (journal > 0) {
        /* La banque terminee a ferme le tube : le journal finit son fichier et s'arrete. */
        while (waitpid(journal, &statut, 0) == -1 && errno == EINTR) { }
        journal = -1;
    }
    supprimer_files();
    demandes = reponses = -1;
    if (partage != NULL) {
        sem_destroy(&partage->verrou);
        shmdt(partage);           /* detache seulement */
        partage = NULL;
    }
    if (segment != -1) shmctl(segment, IPC_RMID, NULL); /* detruit le segment */
    segment = -1;
    if (compteur != NULL) {
        sem_destroy(&compteur->verrou);
        munmap(compteur, sizeof(CompteurJournal));
        compteur = NULL;
    }
    if (nom_posix[0] != '\0') shm_unlink(nom_posix);    /* detruit l'objet POSIX */
    nom_posix[0] = '\0';
    if (chemin_tube[0] != '\0') unlink(chemin_tube);    /* supprime le tube nomme */
    chemin_tube[0] = '\0';
}

void banque_decrire(void) {
    printf("Client PID=%ld | Banque PID=%ld | Journal PID=%ld\n",
           (long)getpid(), (long)serveur, (long)journal);
    printf("Files requetes=%d reponses=%d | Segment=%d | Tube nomme=%s\n",
           demandes, reponses, segment, chemin_tube);
}

/* ===================== Cote client ===================== */

/* Envoie une requete et attend la reponse (appels bloquants, comme en cours).
   alarm(DELAI_MAX) limite l'attente : si la banque ne repond plus, SIGALRM
   interrompt msgrcv (EINTR) et l'operation est declaree non confirmee.
   Un seul appel a la fois par processus client. */
Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant) {
    static unsigned numero;
    MessageRequete req = {0};
    MessageReponse rep;
    Reponse resultat = { SERVICE_INDISPONIBLE, -1 };
    if (interruption || demandes == -1) return resultat;
    req.type = priorite(operation);
    req.corps.pid = getpid();
    req.corps.numero = ++numero;
    req.corps.client = client;
    req.corps.operation = operation;
    req.corps.source = source;
    req.corps.destination = destination;
    req.corps.montant = montant;

    delai_depasse = 0;
    alarm(DELAI_MAX);
    if (msgsnd(demandes, &req, sizeof req.corps, 0) == 0) {
        for (;;) {
            /* Type = notre PID : on ne lit que les reponses qui nous sont destinees. */
            ssize_t taille = msgrcv(reponses, &rep, sizeof rep.corps, (long)getpid(), 0);
            if (taille != -1 && rep.corps.numero == numero) {
                resultat = rep.corps.resultat;
                break;
            }
            /* Erreur reelle (file supprimee), alarme ou Ctrl+C : on abandonne. */
            if (taille == -1 && (errno != EINTR || delai_depasse || interruption)) break;
            /* Sinon : reponse d'une ancienne requete abandonnee, on l'ignore. */
        }
    }
    alarm(0); /* annule l'alarme si la reponse est arrivee a temps */
    return resultat;
}

/* Lecture directe du tableau de bord en memoire partagee, sans message. */
int banque_lire_tableau(TableauBord *copie) {
    if (partage == NULL) return 0;
    while (sem_wait(&partage->verrou) == -1 && errno == EINTR) { }
    *copie = partage->donnees; /* copie coherente : la banque ne peut pas ecrire en meme temps */
    sem_post(&partage->verrou);
    return 1;
}

/* Lecture du compteur tenu par le journal (memoire POSIX). */
int banque_lignes_journal(unsigned long *lignes) {
    if (compteur == NULL) return 0;
    while (sem_wait(&compteur->verrou) == -1 && errno == EINTR) { }
    *lignes = compteur->lignes;
    sem_post(&compteur->verrou);
    return 1;
}

/* ===================== Fonctions pour les tests ===================== */

Montant banque_total(void) {
    TableauBord tableau;
    return banque_lire_tableau(&tableau) ? tableau.total : -1;
}

long banque_pid_serveur(void) { return (long)serveur; }

/* Verifie sur une file temporaire que msgrcv(..., -3, ...) lit bien les
   messages par priorite (types 3, 2, 1, 2 envoyes -> lus 1, 2, 2, 3). */
int banque_test_priorites(void) {
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    int i, ok = 1;
    long types[] = {3, 2, 1, 2};
    unsigned attendu[] = {2, 1, 3, 0};
    MessageRequete req = {0};
    if (id == -1) return 0;
    for (i = 0; i < 4; i++) {
        req.type = types[i]; req.corps.numero = (unsigned)i;
        if (msgsnd(id, &req, sizeof req.corps, IPC_NOWAIT) == -1) ok = 0;
    }
    for (i = 0; i < 4; i++) {
        if (msgrcv(id, &req, sizeof req.corps, -3, IPC_NOWAIT) == -1 ||
            req.corps.numero != attendu[i]) ok = 0;
    }
    msgctl(id, IPC_RMID, NULL);
    return ok;
}
