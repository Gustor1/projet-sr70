#include "banque.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

static int controles, echecs;
#define VERIFIER(condition) do { controles++; if (!(condition)) { \
    fprintf(stderr, "ECHEC ligne %d : %s\n", __LINE__, #condition); echecs++; } } while (0)

static Montant solde(int compte) {
    return banque_envoyer((compte + 1) / 2, CONSULTATION, compte, 0, 0).solde;
}

/* Le journal doit avoir recu une ligne par requete traitee par la banque. */
static int journal_complet(void) {
    TableauBord t;
    unsigned long lignes;
    sleep(1); /* le journal est un autre processus : on lui laisse le temps de lire */
    return banque_lire_tableau(&t) && banque_lignes_journal(&lignes) &&
           lignes == t.reussies + t.refusees;
}
typedef struct { int mode; int succes; int erreurs; } Travail;
static void concurrence(Travail *travail) {
    int i;
    for (i = 0; i < 100; i++) {
        Reponse r;
        if (travail->mode == 0) r = banque_envoyer(1, RETRAIT, 1, 0, 1000);
        else if (travail->mode == 1) r = banque_envoyer(1, TRANSFERT, 1, 3, 1);
        else r = banque_envoyer(2, TRANSFERT, 3, 1, 1);
        if (r.statut == SUCCES) travail->succes++;
        else if (travail->mode != 0 || r.statut != FONDS_INSUFFISANTS) travail->erreurs++;
    }
}

static void tester_concurrence(int retraits) {
    pid_t clients[24];
    int depart[2], resultats[2];
    int i, succes = 0, erreurs = 0, statut;
    char jetons[24] = {0};
    if (!banque_demarrer()) { VERIFIER(0); return; }
    if (pipe(depart) == -1) { VERIFIER(0); banque_arreter(); return; }
    if (pipe(resultats) == -1) {
        VERIFIER(0); close(depart[0]); close(depart[1]); banque_arreter(); return;
    }
    fflush(NULL);
    for (i = 0; i < 24; i++) {
        clients[i] = fork();
        if (clients[i] == -1) { perror("fork test"); exit(EXIT_FAILURE); }
        if (clients[i] == 0) {
            Travail travail = { retraits ? 0 : (i % 2 + 1), 0, 0 };
            char jeton;
            close(depart[1]); close(resultats[0]);
            if (read(depart[0], &jeton, 1) != 1) _exit(EXIT_FAILURE);
            close(depart[0]);
            concurrence(&travail);
            /* Petit resultat ecrit en une fois dans un tube commun. */
            if (write(resultats[1], &travail, sizeof travail) != sizeof travail) _exit(EXIT_FAILURE);
            close(resultats[1]);
            _exit(EXIT_SUCCESS);
        }
    }
    close(depart[0]); close(resultats[1]);
    VERIFIER(write(depart[1], jetons, sizeof jetons) == sizeof jetons);
    close(depart[1]);
    for (i = 0; i < 24; i++) {
        Travail travail;
        size_t recus = 0;
        while (recus < sizeof travail) {
            ssize_t lu = read(resultats[0], (char *)&travail + recus, sizeof travail - recus);
            if (lu == -1 && errno == EINTR) continue;
            if (lu <= 0) break;
            recus += (size_t)lu;
        }
        VERIFIER(recus == sizeof travail);
        if (recus == sizeof travail) { succes += travail.succes; erreurs += travail.erreurs; }
    }
    close(resultats[0]);
    for (i = 0; i < 24; i++) {
        pid_t pid;
        do { pid = waitpid(clients[i], &statut, 0); } while (pid == -1 && errno == EINTR);
        VERIFIER(pid == clients[i] && WIFEXITED(statut) && WEXITSTATUS(statut) == 0);
    }
    {   /* Les compteurs du segment partage ne doivent perdre aucune requete. */
        TableauBord t;
        VERIFIER(banque_lire_tableau(&t));
        VERIFIER(t.reussies == (retraits ? 100UL : 2400UL));
        VERIFIER(t.refusees == (retraits ? 2300UL : 0UL));
        VERIFIER(t.soldes[0] == (retraits ? 0 : 100000) && t.soldes[2] == 100000);
    }
    VERIFIER(journal_complet()); /* 2 400 lignes, aucune perdue ni doublee. */
    VERIFIER(erreurs == 0);
    VERIFIER(succes == (retraits ? 100 : 2400));
    VERIFIER(solde(1) == (retraits ? 0 : 100000));
    VERIFIER(solde(3) == 100000);
    VERIFIER(banque_total() == (retraits ? 500000 : 600000));
    banque_arreter();
}

int main(void) {
    if (!banque_demarrer()) { fprintf(stderr, "Demarrage impossible\n"); return EXIT_FAILURE; }
    VERIFIER(banque_test_priorites());
    VERIFIER(banque_total() == 600000);
    {   /* Etat initial publie par la banque avant d'annoncer qu'elle est prete. */
        TableauBord t;
        VERIFIER(banque_lire_tableau(&t));
        VERIFIER(t.soldes[0] == 100000 && t.soldes[5] == 100000);
        VERIFIER(t.reussies == 0 && t.refusees == 0);
    }
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
    {   /* Le tableau partage reflete exactement les comptes de la banque. */
        TableauBord t;
        int i;
        Montant somme = 0;
        VERIFIER(banque_lire_tableau(&t));
        for (i = 0; i < NB_COMPTES; i++) somme += t.soldes[i];
        VERIFIER(t.soldes[0] == 0 && t.soldes[1] == 101000 && t.soldes[2] == SOLDE_MAX);
        VERIFIER(t.total == somme && banque_total() == somme);
        VERIFIER(t.refusees == 14); /* Les 14 requetes refusees ci-dessus. */
    }
    VERIFIER(journal_complet());
    {   /* Banque suspendue (SIGSTOP) : alarm arrete l'attente apres 5 s. */
        pid_t pid = (pid_t)banque_pid_serveur();
        time_t debut;
        kill(pid, SIGSTOP);
        sleep(1); /* laisse au systeme le temps d'arreter la banque */
        debut = time(NULL);
        VERIFIER(banque_envoyer(1, CONSULTATION, 2, 0, 0).statut == SERVICE_INDISPONIBLE);
        VERIFIER(time(NULL) - debut >= 4 && time(NULL) - debut <= 7);
        kill(pid, SIGCONT);
        /* La reponse arrivee en retard est ignoree grace a son numero. */
        VERIFIER(solde(1) == 0 && solde(2) == 101000);
    }
    banque_arreter();
    tester_concurrence(1);
    tester_concurrence(0);
    printf("%d controles, %d echecs.\n", controles, echecs);
    return echecs ? EXIT_FAILURE : EXIT_SUCCESS;
}
