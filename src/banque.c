#include "banque.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>

#define NB_EMPLOYES 2
#define DELAI_MAX 5

typedef struct { int proprietaire; Montant solde; } Compte;
typedef struct {
    long type; /* Positif et obligatoirement en premiere position. */
    struct {
        pid_t pid;
        unsigned numero;
        int client, operation, source, destination;
        Montant montant;
    } corps;
} MessageRequete;
typedef struct {
    long type; /* PID du destinataire. */
    struct { unsigned numero; Reponse resultat; } corps;
} MessageReponse;

/* Contenu du segment de memoire partagee. Le semaphore doit etre DANS le
   segment : apres fork, une variable ordinaire serait copiee, pas partagee. */
typedef struct {
    sem_t verrou;        /* sem_init(..., 1, 1) : binaire, partage entre processus. */
    TableauBord donnees; /* Ecrit par la banque, lu par le processus principal. */
} SegmentPartage;

/* Les comptes sont modifies uniquement dans le processus banque.
   Les clients heritent des identifiants IPC par fork, pas des soldes vivants. */
static Compte comptes[NB_COMPTES];
static pthread_mutex_t verrou_comptes;
static sem_t employes_prets;
static int demandes = -1, reponses = -1, segment = -1;
static SegmentPartage *partage = NULL;
static pid_t serveur = -1, proprietaire = -1;
static volatile sig_atomic_t interruption;

static void recevoir_signal(int signal_recu) {
    (void)signal_recu;
    interruption = 1; /* Aucun printf ni nettoyage dans le gestionnaire. */
}

int banque_interrompue(void) { return interruption != 0; }

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

/* Avec msgrcv(..., -3, ...), le plus PETIT type passe en premier. */
static long priorite(int operation) {
    if (operation == TRANSFERT) return 1;
    if (operation == DEPOT || operation == RETRAIT) return 2;
    return 3;
}

/* Appelee sous verrou : toutes les validations precedent les modifications. */
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
    if (source->proprietaire != client) {
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
        if (montant > SOLDE_MAX - source->solde) rep.statut = PLAFOND_DEPASSE;
        else source->solde += montant;
    } else if (operation == RETRAIT) {
        if (montant > source->solde) rep.statut = FONDS_INSUFFISANTS;
        else source->solde -= montant;
    } else {
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
            source->solde -= montant;
            destination->solde += montant;
        }
    }
    rep.solde = source->solde;
    return rep;
}

/* Recopie les comptes dans le segment partage. Appelee sous verrou_comptes :
   l'ordre des verrous est toujours mutex puis semaphore, donc pas d'interblocage. */
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
    sem_post(&partage->verrou); /* V() */
}

static void *travailler(void *argument) {
    (void)argument;
    sem_post(&employes_prets);
    for (;;) {
        MessageRequete req;
        MessageReponse rep = {0};
        ssize_t taille = msgrcv(demandes, &req, sizeof req.corps, -3, 0);
        if (taille == -1) {
            if (errno == EINTR) continue;
            break; /* IPC_RMID reveille les employes bloques. */
        }
        if (taille != (ssize_t)sizeof req.corps || req.corps.pid <= 0) continue;
        rep.type = (long)req.corps.pid;
        rep.corps.numero = req.corps.numero;
        pthread_mutex_lock(&verrou_comptes);
        rep.corps.resultat = executer(&req);
        publier(&rep.corps.resultat); /* Avant la reponse : le client voit un tableau a jour. */
        pthread_mutex_unlock(&verrou_comptes);
        while (msgsnd(reponses, &rep, sizeof rep.corps, 0) == -1) {
            if (errno != EINTR) return NULL;
        }
    }
    return NULL;
}

static void supprimer_files(void) {
    if (demandes != -1) msgctl(demandes, IPC_RMID, NULL);
    if (reponses != -1) msgctl(reponses, IPC_RMID, NULL);
    /* Ne pas modifier les identifiants pendant que les employes les lisent. */
}

/* Appelee par le processus principal seulement, quand la banque est terminee.
   shmdt detache ; seul shmctl(IPC_RMID) detruit le segment. */
static void supprimer_segment(void) {
    if (partage != NULL) {
        sem_destroy(&partage->verrou);
        shmdt(partage);
        partage = NULL;
    }
    if (segment != -1) shmctl(segment, IPC_RMID, NULL);
    segment = -1;
}

/* SIGINT et SIGTERM sont bloques dans tous les threads du serveur.
   Son thread principal les attend avec sigwait, puis nettoie normalement. */
static void servir(int tube_pret, const sigset_t *signaux) {
    pthread_t employes[NB_EMPLOYES];
    int i, signal_recu;
    char pret = 'P';
    if (pthread_mutex_init(&verrou_comptes, NULL) != 0 ||
        sem_init(&employes_prets, 0, 0) == -1) _exit(EXIT_FAILURE);
    for (i = 0; i < NB_COMPTES; i++) {
        comptes[i].proprietaire = i / 2 + 1;
        comptes[i].solde = 100000;
    }
    publier(NULL); /* Soldes initiaux visibles avant la premiere transaction. */
    for (i = 0; i < NB_EMPLOYES; i++) {
        if (pthread_create(&employes[i], NULL, travailler, NULL) != 0) {
            supprimer_files(); _exit(EXIT_FAILURE);
        }
    }
    for (i = 0; i < NB_EMPLOYES; i++) {
        while (sem_wait(&employes_prets) == -1) {
            if (errno != EINTR) _exit(EXIT_FAILURE);
        }
    }
    /* Le pere attend cet octet avant d'envoyer la premiere transaction. */
    if (write(tube_pret, &pret, 1) != 1) {
        supprimer_files(); _exit(EXIT_FAILURE);
    }
    close(tube_pret);
    sigwait(signaux, &signal_recu);
    supprimer_files(); /* Debloque msgrcv ET msgsnd avant les pthread_join. */
    for (i = 0; i < NB_EMPLOYES; i++) pthread_join(employes[i], NULL);
    sem_destroy(&employes_prets);
    pthread_mutex_destroy(&verrou_comptes);
    shmdt(partage); /* Le parent garde son attachement et supprimera le segment. */
    _exit(EXIT_SUCCESS);
}

int banque_demarrer(void) {
    int tube[2];
    char pret = 0;
    ssize_t lu;
    sigset_t signaux, ancien_masque;
    struct sigaction action = {0};
    interruption = 0;
    proprietaire = getpid();
    action.sa_handler = recevoir_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) return 0;
    demandes = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (demandes == -1) return 0;
    reponses = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (reponses == -1) { supprimer_files(); return 0; }
    /* Segment cree et attache AVANT fork : la banque herite de l'attachement. */
    segment = shmget(IPC_PRIVATE, sizeof(SegmentPartage), IPC_CREAT | 0600);
    if (segment == -1) { supprimer_files(); return 0; }
    partage = shmat(segment, NULL, 0);
    if (partage == (void *)-1) {
        partage = NULL; supprimer_segment(); supprimer_files(); return 0;
    }
    memset(partage, 0, sizeof *partage);
    if (sem_init(&partage->verrou, 1, 1) == -1) {
        shmdt(partage); partage = NULL; supprimer_segment(); supprimer_files(); return 0;
    }
    if (pipe(tube) == -1) { supprimer_segment(); supprimer_files(); return 0; }
    sigemptyset(&signaux);
    sigaddset(&signaux, SIGINT);
    sigaddset(&signaux, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signaux, &ancien_masque) == -1) {
        close(tube[0]); close(tube[1]); supprimer_segment(); supprimer_files(); return 0;
    }
    fflush(NULL);
    serveur = fork();
    if (serveur == 0) {
        close(tube[0]);
        servir(tube[1], &signaux);
    }
    close(tube[1]);
    sigprocmask(SIG_SETMASK, &ancien_masque, NULL);
    if (serveur == -1) { close(tube[0]); supprimer_segment(); supprimer_files(); return 0; }
    do { lu = read(tube[0], &pret, 1); } while (lu == -1 && errno == EINTR && !interruption);
    close(tube[0]);
    if (lu != 1 || pret != 'P' || interruption) {
        banque_arreter(); return 0;
    }
    return 1;
}

/* Attentes bornees et interruptibles cote client. Un serveur disparu
   ne doit pas laisser un processus bloque indefiniment. */
static int temps_ecoule(const struct timespec *debut) {
    struct timespec maintenant;
    clock_gettime(CLOCK_MONOTONIC, &maintenant);
    return maintenant.tv_sec - debut->tv_sec > DELAI_MAX ||
           (maintenant.tv_sec - debut->tv_sec == DELAI_MAX && maintenant.tv_nsec >= debut->tv_nsec);
}

Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant) {
    static unsigned numero;
    MessageRequete req = {0};
    MessageReponse rep;
    Reponse erreur = { SERVICE_INDISPONIBLE, -1 };
    struct timespec debut, pause = { 0, 1000000 };
    if (interruption || demandes == -1 || reponses == -1) return erreur;
    req.type = priorite(operation);
    req.corps.pid = getpid();
    req.corps.numero = ++numero;
    req.corps.client = client;
    req.corps.operation = operation;
    req.corps.source = source;
    req.corps.destination = destination;
    req.corps.montant = montant;
    clock_gettime(CLOCK_MONOTONIC, &debut);
    while (msgsnd(demandes, &req, sizeof req.corps, IPC_NOWAIT) == -1) {
        if ((errno != EAGAIN && errno != EINTR) || interruption || temps_ecoule(&debut)) return erreur;
        nanosleep(&pause, NULL);
    }
    for (;;) {
        ssize_t taille;
        if (interruption || temps_ecoule(&debut)) return erreur;
        taille = msgrcv(reponses, &rep, sizeof rep.corps, (long)getpid(), IPC_NOWAIT);
        if (taille == (ssize_t)sizeof rep.corps && rep.corps.numero == numero)
            return rep.corps.resultat;
        if (taille == -1 && errno != ENOMSG && errno != EINTR) return erreur;
        nanosleep(&pause, NULL);
    }
}

void banque_arreter(void) {
    int statut;
    if (getpid() != proprietaire) return;
    if (serveur > 0) {
        kill(serveur, SIGTERM);
        while (waitpid(serveur, &statut, 0) == -1 && errno == EINTR) { }
        serveur = -1;
    }
    supprimer_files();
    demandes = reponses = -1;
    supprimer_segment(); /* Apres waitpid : plus aucun employe ne peut ecrire. */
}

void banque_decrire(void) {
    printf("Client PID=%ld | Banque PID=%ld | Files requetes=%d reponses=%d | Memoire partagee=%d\n",
           (long)getpid(), (long)serveur, demandes, reponses, segment);
}

/* Lecture sans appel systeme pour les donnees elles-memes : seul le semaphore
   en demande. L'attente est bornee au cas ou la banque serait tuee verrou pris. */
int banque_lire_tableau(TableauBord *copie) {
    struct timespec limite;
    if (partage == NULL || interruption) return 0;
    clock_gettime(CLOCK_REALTIME, &limite);
    limite.tv_sec += DELAI_MAX;
    while (sem_timedwait(&partage->verrou, &limite) == -1) {
        if (errno != EINTR || interruption) return 0;
    }
    *copie = partage->donnees; /* Copie coherente : la banque ne peut pas ecrire. */
    sem_post(&partage->verrou);
    return 1;
}

Montant banque_total(void) {
    TableauBord tableau;
    return banque_lire_tableau(&tableau) ? tableau.total : -1;
}

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
