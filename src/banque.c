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

typedef struct { int proprietaire; Montant solde; } Compte;
typedef struct {
    long type; /* priorite de l'operation */
    struct {
        pid_t pid;
        unsigned numero;
        int client, operation, source, destination;
        Montant montant;
    } corps;
} MessageRequete;
typedef struct {
    long type; /* PID du client destinataire */
    struct { unsigned numero; Reponse resultat; } corps;
} MessageReponse;

/* Segment System V. Le semaphore est dans le segment pour etre
   reellement partage entre le processus principal et la banque. */
typedef struct {
    sem_t verrou;
    TableauBord donnees;
} SegmentPartage;

static Compte comptes[NB_COMPTES]; /* utilises seulement par la banque */
static pthread_mutex_t verrou_comptes;
static sem_t employes_prets;
static int demandes = -1, reponses = -1, segment = -1;
static SegmentPartage *partage = NULL;
static CompteurJournal *compteur = NULL;
static pid_t serveur = -1, journal = -1, proprietaire = -1;
static int tube_journal = -1;
static char chemin_tube[64], nom_posix[64];
static volatile sig_atomic_t interruption, delai_depasse;

static void recevoir_signal(int signal_recu) {
    (void)signal_recu;
    interruption = 1;
}

static void recevoir_alarme(int signal_recu) {
    (void)signal_recu;
    delai_depasse = 1;
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

/* Avec msgrcv(..., -3, ...), le plus petit type est lu en premier. */
static long priorite(int operation) {
    if (operation == TRANSFERT) return 1;
    if (operation == DEPOT || operation == RETRAIT) return 2;
    return 3;
}

/* Appelee sous le mutex : on verifie tout avant de modifier un solde. */
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

/* Recopie les comptes dans la memoire partagee (appelee sous le mutex). */
static void publier(const Reponse *resultat) {
    int i;
    while (sem_wait(&partage->verrou) == -1 && errno == EINTR) { }
    partage->donnees.total = 0;
    for (i = 0; i < NB_COMPTES; i++) {
        partage->donnees.soldes[i] = comptes[i].solde;
        partage->donnees.total += comptes[i].solde;
    }
    if (resultat && resultat->statut == SUCCES) partage->donnees.reussies++;
    else if (resultat) partage->donnees.refusees++;
    sem_post(&partage->verrou);
}

/* Envoie une ligne au processus journal par le tube nomme. */
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
    if (write(tube_journal, ligne, (size_t)n) == -1) tube_journal = -1; /* journal arrete */
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
            break; /* file supprimee : fin de la banque */
        }
        rep.type = (long)req.corps.pid;
        rep.corps.numero = req.corps.numero;
        pthread_mutex_lock(&verrou_comptes);
        rep.corps.resultat = executer(&req);
        publier(&rep.corps.resultat);
        journaliser(&req, &rep.corps.resultat);
        pthread_mutex_unlock(&verrou_comptes);
        if (msgsnd(reponses, &rep, sizeof rep.corps, 0) == -1 && errno != EINTR) break;
    }
    return NULL;
}

static void supprimer_files(void) {
    if (demandes != -1) msgctl(demandes, IPC_RMID, NULL);
    if (reponses != -1) msgctl(reponses, IPC_RMID, NULL);
}

/* Code du processus banque. SIGINT et SIGTERM y sont bloques :
   le thread principal les attend avec sigwait. */
static void servir(int tube_pret, const sigset_t *signaux) {
    pthread_t employes[NB_EMPLOYES];
    int i, signal_recu;
    char pret = 'P';
    signal(SIGPIPE, SIG_IGN); /* si le journal s'arrete, write echoue sans tuer la banque */
    tube_journal = open(chemin_tube, O_WRONLY); /* attend que le journal ouvre l'autre bout */
    pthread_mutex_init(&verrou_comptes, NULL);
    sem_init(&employes_prets, 0, 0);
    for (i = 0; i < NB_COMPTES; i++) {
        comptes[i].proprietaire = i / 2 + 1;
        comptes[i].solde = 100000; /* 1000 EUR */
    }
    publier(NULL);
    for (i = 0; i < NB_EMPLOYES; i++) pthread_create(&employes[i], NULL, travailler, NULL);
    for (i = 0; i < NB_EMPLOYES; i++) sem_wait(&employes_prets);
    /* Previent le pere que la banque est prete. */
    if (write(tube_pret, &pret, 1) != 1) _exit(EXIT_FAILURE);
    close(tube_pret);
    sigwait(signaux, &signal_recu);
    supprimer_files(); /* reveille les employes bloques dans msgrcv */
    for (i = 0; i < NB_EMPLOYES; i++) pthread_join(employes[i], NULL);
    sem_destroy(&employes_prets);
    pthread_mutex_destroy(&verrou_comptes);
    close(tube_journal); /* le journal lit alors la fin du tube */
    shmdt(partage);
    _exit(EXIT_SUCCESS);
}

/* Fils journal : son entree standard devient le tube nomme, puis il
   execute le programme journal. */
static void lancer_journal(void) {
    int fd;
    signal(SIGINT, SIG_IGN);  /* reste ignore apres exec : Ctrl+C ne le coupe pas */
    signal(SIGTERM, SIG_IGN);
    fd = open(chemin_tube, O_RDONLY);
    dup2(fd, STDIN_FILENO);
    close(fd);
    execl(PROGRAMME_JOURNAL, "journal", FICHIER_JOURNAL, nom_posix, (char *)NULL);
    perror("execl journal"); /* on arrive ici seulement si exec a echoue */
    _exit(127);
}

int banque_demarrer(void) {
    int tube[2], fd;
    char pret = 0;
    ssize_t lu = 0;
    sigset_t signaux, ancien_masque;
    struct sigaction action = {0};
    interruption = 0;
    serveur = journal = -1;
    proprietaire = getpid();
    /* Sans SA_RESTART : un signal interrompt msgrcv, qui renvoie EINTR. */
    sigemptyset(&action.sa_mask);
    action.sa_handler = recevoir_signal;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = recevoir_alarme;
    sigaction(SIGALRM, &action, NULL);

    /* Tube nomme banque -> journal. Il sert aussi de fichier pour ftok. */
    snprintf(chemin_tube, sizeof chemin_tube, "/tmp/sr70-banque-%ld.fifo", (long)getpid());
    if (mkfifo(chemin_tube, 0600) == -1) { chemin_tube[0] = '\0'; return 0; }

    /* Files de messages et segment System V, cles obtenues avec ftok. */
    demandes = msgget(ftok(chemin_tube, 'R'), IPC_CREAT | 0600);
    reponses = msgget(ftok(chemin_tube, 'A'), IPC_CREAT | 0600);
    segment = shmget(ftok(chemin_tube, 'S'), sizeof(SegmentPartage), IPC_CREAT | 0600);
    if (demandes == -1 || reponses == -1 || segment == -1) { banque_arreter(); return 0; }
    partage = shmat(segment, NULL, 0);
    if (partage == (void *)-1) { partage = NULL; banque_arreter(); return 0; }
    memset(partage, 0, sizeof *partage);
    sem_init(&partage->verrou, 1, 1); /* 1 : partage entre processus */

    /* Memoire partagee POSIX pour le compteur du journal. */
    snprintf(nom_posix, sizeof nom_posix, "/sr70-journal-%ld", (long)getpid());
    fd = shm_open(nom_posix, O_CREAT | O_RDWR, 0600);
    if (fd == -1) { banque_arreter(); return 0; }
    if (ftruncate(fd, sizeof(CompteurJournal)) == -1) { close(fd); banque_arreter(); return 0; }
    compteur = mmap(NULL, sizeof(CompteurJournal), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (compteur == MAP_FAILED) { compteur = NULL; banque_arreter(); return 0; }
    compteur->lignes = 0;
    sem_init(&compteur->verrou, 1, 1);

    fflush(NULL);
    journal = fork();
    if (journal == 0) lancer_journal();
    if (journal == -1 || pipe(tube) == -1) {
        if (journal > 0) kill(journal, SIGKILL);
        banque_arreter(); return 0;
    }

    /* La banque bloque SIGINT/SIGTERM avant de creer ses threads. */
    sigemptyset(&signaux);
    sigaddset(&signaux, SIGINT);
    sigaddset(&signaux, SIGTERM);
    sigprocmask(SIG_BLOCK, &signaux, &ancien_masque);
    serveur = fork();
    if (serveur == 0) {
        close(tube[0]);
        servir(tube[1], &signaux);
    }
    close(tube[1]);
    sigprocmask(SIG_SETMASK, &ancien_masque, NULL);
    if (serveur != -1) {
        do { lu = read(tube[0], &pret, 1); } while (lu == -1 && errno == EINTR && !interruption);
    }
    close(tube[0]);
    if (serveur == -1 || lu != 1 || interruption) {
        kill(journal, SIGKILL);
        banque_arreter(); return 0;
    }
    return 1;
}

/* Envoi d'une requete puis attente bloquante de la reponse.
   alarm(DELAI_MAX) evite d'attendre pour toujours si la banque ne repond plus. */
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
            ssize_t taille = msgrcv(reponses, &rep, sizeof rep.corps, (long)getpid(), 0);
            if (taille != -1 && rep.corps.numero == numero) { resultat = rep.corps.resultat; break; }
            /* Autre erreur qu'un signal, alarme ou Ctrl+C : on abandonne. */
            if (taille == -1 && (errno != EINTR || delai_depasse || interruption)) break;
            /* Sinon : ancienne reponse arrivee en retard, on l'ignore. */
        }
    }
    alarm(0);
    return resultat;
}

void banque_arreter(void) {
    int statut;
    if (getpid() != proprietaire) return;
    if (serveur > 0) {
        kill(serveur, SIGTERM);
        while (waitpid(serveur, &statut, 0) == -1 && errno == EINTR) { }
        serveur = -1;
    }
    if (journal > 0) {
        while (waitpid(journal, &statut, 0) == -1 && errno == EINTR) { } /* fin du tube */
        journal = -1;
    }
    supprimer_files();
    demandes = reponses = -1;
    if (partage != NULL) {
        sem_destroy(&partage->verrou);
        shmdt(partage);
        partage = NULL;
    }
    if (segment != -1) shmctl(segment, IPC_RMID, NULL);
    segment = -1;
    if (compteur != NULL) {
        sem_destroy(&compteur->verrou);
        munmap(compteur, sizeof(CompteurJournal));
        compteur = NULL;
    }
    if (nom_posix[0] != '\0') shm_unlink(nom_posix);
    nom_posix[0] = '\0';
    if (chemin_tube[0] != '\0') unlink(chemin_tube);
    chemin_tube[0] = '\0';
}

void banque_decrire(void) {
    printf("Client PID=%ld | Banque PID=%ld | Journal PID=%ld\n",
           (long)getpid(), (long)serveur, (long)journal);
    printf("Files requetes=%d reponses=%d | Segment=%d | Tube nomme=%s\n",
           demandes, reponses, segment, chemin_tube);
}

/* Lecture directe du segment partage, sans passer par les files. */
int banque_lire_tableau(TableauBord *copie) {
    if (partage == NULL) return 0;
    while (sem_wait(&partage->verrou) == -1 && errno == EINTR) { }
    *copie = partage->donnees;
    sem_post(&partage->verrou);
    return 1;
}

int banque_lignes_journal(unsigned long *lignes) {
    if (compteur == NULL) return 0;
    while (sem_wait(&compteur->verrou) == -1 && errno == EINTR) { }
    *lignes = compteur->lignes;
    sem_post(&compteur->verrou);
    return 1;
}

Montant banque_total(void) {
    TableauBord tableau;
    return banque_lire_tableau(&tableau) ? tableau.total : -1;
}

long banque_pid_serveur(void) { return (long)serveur; }

/* Verifie l'ordre de lecture des priorites sur une file temporaire. */
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
