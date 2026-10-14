#include "banque.h"
#include "synchronisation.h"

static int controles, echecs;
#define VERIFIER(condition) do { controles++; if (!(condition)) { \
    fprintf(stderr, "ECHEC ligne %d : %s\n", __LINE__, #condition); echecs++; } } while (0)

static Montant solde(int compte) {
    return banque_envoyer((compte + 1) / 2, CONSULTATION, compte, 0, 0).solde;
}

typedef struct { int mode; int succes; int erreurs; } Travail;
static Semaphore depart;

static FONCTION_THREAD concurrence(void *argument) {
    Travail *travail = argument;
    int i;
    semaphore_attendre(&depart);
    for (i = 0; i < 100; i++) {
        Reponse r;
        if (travail->mode == 0) r = banque_envoyer(1, RETRAIT, 1, 0, 1000);
        else if (travail->mode == 1) r = banque_envoyer(1, TRANSFERT, 1, 3, 1);
        else r = banque_envoyer(2, TRANSFERT, 3, 1, 1);
        if (r.statut == SUCCES) travail->succes++;
        else if (travail->mode != 0 || r.statut != FONDS_INSUFFISANTS) travail->erreurs++;
    }
    return FIN_THREAD;
}

static void tester_concurrence(int retraits) {
    /* 24 producteurs > 16 places : sollicite aussi l'attente sur file pleine. */
    Thread threads[24];
    Travail travaux[24];
    int i, succes = 0, erreurs = 0;
    banque_demarrer();
    semaphore_creer(&depart, 0);
    for (i = 0; i < 24; i++) {
        travaux[i].mode = retraits ? 0 : (i % 2 + 1);
        travaux[i].succes = travaux[i].erreurs = 0;
        thread_creer(&threads[i], concurrence, &travaux[i]);
    }
    for (i = 0; i < 24; i++) semaphore_signaler(&depart);
    for (i = 0; i < 24; i++) {
        thread_attendre(threads[i]);
        succes += travaux[i].succes;
        erreurs += travaux[i].erreurs;
    }
    VERIFIER(erreurs == 0);
    VERIFIER(succes == (retraits ? 100 : 2400));
    VERIFIER(solde(1) == (retraits ? 0 : 100000));
    VERIFIER(solde(3) == 100000);
    VERIFIER(banque_total() == (retraits ? 500000 : 600000));
    semaphore_detruire(&depart);
    banque_arreter();
}

int main(void) {
    banque_demarrer();
    VERIFIER(banque_test_priorites());
    VERIFIER(banque_total() == 600000);
    VERIFIER(banque_envoyer(1, DEPOT, 1, 0, 1234).statut == SUCCES);
    VERIFIER(solde(1) == 101234);
    VERIFIER(banque_envoyer(1, RETRAIT, 1, 0, 234).statut == SUCCES);
    VERIFIER(solde(1) == 101000);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 2, 1000).statut == SUCCES);
    VERIFIER(solde(1) == 100000 && solde(2) == 101000);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 3, 2000).statut == SUCCES);
    VERIFIER(solde(1) == 98000 && solde(3) == 102000);
    VERIFIER(banque_envoyer(1, RETRAIT, 1, 0, 98001).statut == FONDS_INSUFFISANTS);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 3, 98001).statut == FONDS_INSUFFISANTS);
    VERIFIER(solde(1) == 98000 && solde(3) == 102000);
    VERIFIER(banque_envoyer(1, DEPOT, 1, 0, 0).statut == MONTANT_INVALIDE);
    VERIFIER(banque_envoyer(1, RETRAIT, 1, 0, -1).statut == MONTANT_INVALIDE);
    VERIFIER(banque_envoyer(1, DEPOT, 1, 0, SOLDE_MAX + 1).statut == MONTANT_INVALIDE);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 1, 1).statut == MEME_COMPTE);
    VERIFIER(banque_envoyer(1, CONSULTATION, 3, 0, 0).statut == ACCES_REFUSE);
    VERIFIER(banque_envoyer(1, RETRAIT, 3, 0, 1).statut == ACCES_REFUSE);
    VERIFIER(banque_envoyer(0, CONSULTATION, 1, 0, 0).statut == CLIENT_INVALIDE);
    VERIFIER(banque_envoyer(1, CONSULTATION, 0, 0, 0).statut == COMPTE_INVALIDE);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 7, 1).statut == COMPTE_INVALIDE);
    VERIFIER(banque_envoyer(1, (Operation)99, 1, 0, 0).statut == OPERATION_INVALIDE);
    VERIFIER(banque_envoyer(2, DEPOT, 3, 0, SOLDE_MAX - 102000).statut == SUCCES);
    VERIFIER(banque_envoyer(2, DEPOT, 3, 0, 1).statut == PLAFOND_DEPASSE);
    VERIFIER(banque_envoyer(1, TRANSFERT, 1, 3, 1).statut == PLAFOND_DEPASSE);
    VERIFIER(solde(1) == 98000 && solde(3) == SOLDE_MAX);
    VERIFIER(banque_envoyer(1, RETRAIT, 1, 0, 98000).statut == SUCCES);
    VERIFIER(solde(1) == 0);
    banque_arreter();
    tester_concurrence(1);
    tester_concurrence(0);
    printf("%d controles, %d echecs.\n", controles, echecs);
    return echecs ? EXIT_FAILURE : EXIT_SUCCESS;
}
