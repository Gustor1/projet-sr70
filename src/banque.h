#ifndef BANQUE_H
#define BANQUE_H

#define NB_CLIENTS 3
#define NB_COMPTES 6
/* 1 milliard d'euros par compte, pour garder des additions bornees. */
#define SOLDE_MAX 100000000000LL
typedef long long Montant;
typedef enum { DEPOT = 1, RETRAIT, TRANSFERT, CONSULTATION } Operation;
typedef enum {
    SUCCES, CLIENT_INVALIDE, COMPTE_INVALIDE, ACCES_REFUSE,
    MONTANT_INVALIDE, FONDS_INSUFFISANTS, PLAFOND_DEPASSE,
    MEME_COMPTE, OPERATION_INVALIDE, SERVICE_INDISPONIBLE
} Statut;
typedef struct {
    Statut statut;
    Montant solde; /* Solde source apres operation, -1 si non consultable. */
} Reponse;

/* Copie du tableau de bord publie par la banque en memoire partagee. */
typedef struct {
    Montant soldes[NB_COMPTES];
    Montant total;
    unsigned long reussies, refusees; /* Requetes traitees depuis le demarrage. */
} TableauBord;

/* Linux : demarrer avant les fork clients, arreter apres leurs waitpid.
   Un seul appel en cours par processus client. Retour : 1 si prete, 0 sinon. */
int banque_demarrer(void);
void banque_arreter(void);
int banque_interrompue(void);
void banque_decrire(void);
Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant);
const char *banque_message(Statut statut);
const char *banque_client(int client);
/* Lecture directe du segment partage, sans message. Retour : 1 si lu, 0 sinon. */
int banque_lire_tableau(TableauBord *copie);
/* Outils de verification a appeler sans clients actifs. */
Montant banque_total(void);
int banque_test_priorites(void);
#endif
