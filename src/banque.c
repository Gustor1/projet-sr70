#include "banque.h"
#include "synchronisation.h"

#define CAPACITE_FILE 16
#define NB_EMPLOYES 2

typedef struct { int proprietaire; Montant solde; } Compte;
typedef struct {
    int client;
    Operation operation;
    int source;
    int destination;
    Montant montant;
    Reponse reponse;
    Semaphore terminee;
} Requete;

static Compte comptes[NB_COMPTES];
static Requete *file[CAPACITE_FILE];
static int taille_file;
static int arret;
static Mutex verrou_file, verrou_comptes;
static Semaphore disponibles, places;
static Thread employes[NB_EMPLOYES];
static const char *noms[] = { "Alice", "Bob", "Charlie" };

const char *banque_client(int client) {
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
        default: return "Operation inconnue";
    }
}

static int priorite(Operation operation) {
    if (operation == TRANSFERT) return 3;
    if (operation == DEPOT || operation == RETRAIT) return 2;
    return 1;
}

/* Le premier de priorite maximale gagne : FIFO entre priorites egales. */
static Requete *extraire(void) {
    int meilleur = 0;
    int i;
    Requete *resultat;
    for (i = 1; i < taille_file; i++) {
        if (priorite(file[i]->operation) > priorite(file[meilleur]->operation))
            meilleur = i;
    }
    resultat = file[meilleur];
    for (i = meilleur; i < taille_file - 1; i++) file[i] = file[i + 1];
    taille_file--;
    return resultat;
}

/* Appelee uniquement sous verrou_comptes. Toutes les validations precedent
   les modifications : un refus ne peut donc jamais produire un demi-transfert. */
static Reponse executer(const Requete *r) {
    Reponse rep = { SUCCES, -1 };
    Compte *source, *destination;
    if (r->client < 1 || r->client > NB_CLIENTS) {
        rep.statut = CLIENT_INVALIDE; return rep;
    }
    if (r->source < 1 || r->source > NB_COMPTES) {
        rep.statut = COMPTE_INVALIDE; return rep;
    }
    source = &comptes[r->source - 1];
    if (source->proprietaire != r->client) {
        rep.statut = ACCES_REFUSE; return rep;
    }
    rep.solde = source->solde;
    if (r->operation < DEPOT || r->operation > CONSULTATION) {
        rep.statut = OPERATION_INVALIDE; return rep;
    }
    if (r->operation == CONSULTATION) return rep;
    if (r->montant <= 0 || r->montant > SOLDE_MAX) {
        rep.statut = MONTANT_INVALIDE; return rep;
    }
    if (r->operation == DEPOT) {
        if (r->montant > SOLDE_MAX - source->solde) rep.statut = PLAFOND_DEPASSE;
        else source->solde += r->montant;
    } else if (r->operation == RETRAIT) {
        if (r->montant > source->solde) rep.statut = FONDS_INSUFFISANTS;
        else source->solde -= r->montant;
    } else {
        if (r->destination < 1 || r->destination > NB_COMPTES) {
            rep.statut = COMPTE_INVALIDE; return rep;
        }
        if (r->source == r->destination) {
            rep.statut = MEME_COMPTE; return rep;
        }
        destination = &comptes[r->destination - 1];
        if (r->montant > source->solde) rep.statut = FONDS_INSUFFISANTS;
        else if (r->montant > SOLDE_MAX - destination->solde) rep.statut = PLAFOND_DEPASSE;
        else {
            source->solde -= r->montant;
            destination->solde += r->montant;
        }
    }
    rep.solde = source->solde;
    return rep;
}

static FONCTION_THREAD travailler(void *argument) {
    (void)argument;
    for (;;) {
        Requete *r;
        semaphore_attendre(&disponibles);
        mutex_prendre(&verrou_file);
        if (taille_file == 0 && arret) {
            mutex_liberer(&verrou_file);
            break;
        }
        r = extraire();
        mutex_liberer(&verrou_file);
        semaphore_signaler(&places);
        mutex_prendre(&verrou_comptes);
        r->reponse = executer(r);
        mutex_liberer(&verrou_comptes);
        /* Ne plus toucher r ensuite : le client peut retourner de sa fonction. */
        semaphore_signaler(&r->terminee);
    }
    return FIN_THREAD;
}

void banque_demarrer(void) {
    int i;
    taille_file = 0;
    arret = 0;
    for (i = 0; i < NB_COMPTES; i++) {
        comptes[i].proprietaire = i / 2 + 1;
        comptes[i].solde = 100000; /* 1000,00 euros */
    }
    mutex_creer(&verrou_file);
    mutex_creer(&verrou_comptes);
    semaphore_creer(&disponibles, 0);
    semaphore_creer(&places, CAPACITE_FILE);
    for (i = 0; i < NB_EMPLOYES; i++) thread_creer(&employes[i], travailler, NULL);
}

Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant) {
    Requete r;
    r.client = client;
    r.operation = operation;
    r.source = source;
    r.destination = destination;
    r.montant = montant;
    semaphore_creer(&r.terminee, 0);
    semaphore_attendre(&places);
    mutex_prendre(&verrou_file);
    file[taille_file++] = &r;
    mutex_liberer(&verrou_file);
    semaphore_signaler(&disponibles);
    semaphore_attendre(&r.terminee);
    semaphore_detruire(&r.terminee);
    return r.reponse;
}

void banque_arreter(void) {
    int i;
    mutex_prendre(&verrou_file);
    arret = 1;
    mutex_liberer(&verrou_file);
    for (i = 0; i < NB_EMPLOYES; i++) semaphore_signaler(&disponibles);
    for (i = 0; i < NB_EMPLOYES; i++) thread_attendre(employes[i]);
    semaphore_detruire(&disponibles);
    semaphore_detruire(&places);
    mutex_detruire(&verrou_comptes);
    mutex_detruire(&verrou_file);
}

Montant banque_total(void) {
    Montant total = 0;
    int i;
    mutex_prendre(&verrou_comptes);
    for (i = 0; i < NB_COMPTES; i++) total += comptes[i].solde;
    mutex_liberer(&verrou_comptes);
    return total;
}

int banque_test_priorites(void) {
    Requete a = {0}, b = {0}, c = {0}, d = {0};
    int ok;
    a.operation = CONSULTATION; b.operation = DEPOT;
    c.operation = TRANSFERT; d.operation = RETRAIT;
    mutex_prendre(&verrou_file);
    file[0] = &a; file[1] = &b; file[2] = &c; file[3] = &d;
    taille_file = 4;
    ok = extraire() == &c;
    ok = (extraire() == &b) && ok;
    ok = (extraire() == &d) && ok;
    ok = (extraire() == &a) && ok;
    mutex_liberer(&verrou_file);
    return ok;
}
