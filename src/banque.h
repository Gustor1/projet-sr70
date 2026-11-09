/*
 * banque.h - Interface de la banque utilisee par le menu et les tests.
 * Auteurs : Eliott LE, Matthias LUTZ, Jad ABDUL RAZEK (SR70, 2026)
 * Le detail des processus, threads et IPC est cache dans banque.c.
 */
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

/* Cree les IPC, le processus journal et le processus banque. Retour : 1 si pret. */
int banque_demarrer(void);
/* Arrete banque et journal, attend leur fin et supprime toutes les ressources. */
void banque_arreter(void);
/* 1 si Ctrl+C ou SIGTERM a ete recu. */
int banque_interrompue(void);
/* Affiche les PID et les identifiants IPC (utile avec ipcs et ps). */
void banque_decrire(void);
/* Envoie une operation a la banque et attend la reponse (5 s maximum).
   destination : compte destinataire pour un transfert ; montant en centimes. */
Reponse banque_envoyer(int client, Operation operation, int source,
                       int destination, Montant montant);
const char *banque_message(Statut statut);
const char *banque_client(int client);
/* Lectures en memoire partagee (System V puis POSIX). Retour : 1 si lu. */
int banque_lire_tableau(TableauBord *copie);
int banque_lignes_journal(unsigned long *lignes);
/* Utilises par les tests. */
Montant banque_total(void);
long banque_pid_serveur(void);
int banque_test_priorites(void);
#endif
