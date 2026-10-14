#include "banque.h"
#include "synchronisation.h"
#include <string.h>
#include <ctype.h>
#include <errno.h>

static Mutex affichage;

static void afficher_montant(Montant montant) {
    printf("%lld,%02lld EUR", montant / 100, montant % 100);
}

static void afficher_reponse(Reponse r) {
    printf("%s", banque_message(r.statut));
    if (r.solde >= 0) { printf(" | solde source : "); afficher_montant(r.solde); }
    printf("\n");
}

/* fgets permet de rejeter une ligne incorrecte sans bloquer sur scanf. */
static int lire_ligne(const char *question, char *texte, size_t taille) {
    int c;
    printf("%s", question);
    fflush(stdout);
    if (!fgets(texte, (int)taille, stdin)) return 0;
    if (!strchr(texte, '\n') && !feof(stdin)) {
        while ((c = getchar()) != '\n' && c != EOF) { }
        texte[0] = '\0'; /* Ligne trop longue : elle sera refusee. */
    }
    texte[strcspn(texte, "\r\n")] = '\0';
    return 1;
}

static int lire_entier(const char *question, int min, int max, int *valeur) {
    char texte[80], *fin;
    long nombre;
    for (;;) {
        if (!lire_ligne(question, texte, sizeof texte)) return 0;
        errno = 0;
        nombre = strtol(texte, &fin, 10);
        if (fin != texte && *fin == '\0' && errno != ERANGE && nombre >= min && nombre <= max) {
            *valeur = (int)nombre; return 1;
        }
        printf("Entrez un entier entre %d et %d.\n", min, max);
    }
}

/* Conversion exacte, sans float : 12,34 devient 1234 centimes. */
static int lire_montant(Montant *montant) {
    char texte[80];
    for (;;) {
        size_t i = 0;
        Montant euros = 0, centimes = 0;
        int chiffres = 0, valide = 1;
        if (!lire_ligne("Montant en euros (ex. 12,50) : ", texte, sizeof texte)) return 0;
        while (isdigit((unsigned char)texte[i])) {
            euros = euros * 10 + (texte[i++] - '0');
            chiffres++;
            if (euros > SOLDE_MAX / 100) { valide = 0; break; }
        }
        if (valide && (texte[i] == ',' || texte[i] == '.')) {
            i++;
            if (!isdigit((unsigned char)texte[i])) valide = 0;
            else {
                centimes = (texte[i++] - '0') * 10;
                if (isdigit((unsigned char)texte[i])) centimes += texte[i++] - '0';
            }
        }
        if (valide && chiffres && texte[i] == '\0' &&
            euros * 100 + centimes > 0 && euros * 100 + centimes <= SOLDE_MAX) {
            *montant = euros * 100 + centimes; return 1;
        }
        puts("Montant positif attendu, avec au maximum deux decimales (plafond : 1 milliard).");
    }
}

static void afficher_comptes(int client) {
    int compte;
    printf("Comptes de %s (client %d) :\n", banque_client(client), client);
    for (compte = client * 2 - 1; compte <= client * 2; compte++) {
        printf("  Compte %d : ", compte);
        afficher_reponse(banque_envoyer(client, CONSULTATION, compte, 0, 0));
    }
}

static FONCTION_THREAD client_demo(void *argument) {
    int client = *(int *)argument;
    int source = client * 2 - 1;
    int suivant = client % NB_CLIENTS + 1;
    Operation operations[] = { DEPOT, RETRAIT, TRANSFERT, TRANSFERT, RETRAIT, CONSULTATION };
    Montant montants[] = { 10000, 2500, 5000, 1000, SOLDE_MAX, 0 };
    const char *libelles[] = { "Depot 100 EUR", "Retrait 25 EUR", "Transfert interne 50 EUR",
                              "Transfert autre client 10 EUR", "Retrait excessif", "Consultation" };
    int i;
    for (i = 0; i < 6; i++) {
        int destination = i == 2 ? source + 1 : suivant * 2 - 1;
        Reponse r = banque_envoyer(client, operations[i], source, destination, montants[i]);
        mutex_prendre(&affichage);
        printf("[%s] %s : ", banque_client(client), libelles[i]);
        afficher_reponse(r);
        mutex_liberer(&affichage);
    }
    return FIN_THREAD;
}

static void demonstration(void) {
    Thread clients[NB_CLIENTS];
    int ids[NB_CLIENTS], i;
    Montant avant = banque_total();
    puts("\nTrois clients en parallele, deux employes bancaires.");
    for (i = 0; i < NB_CLIENTS; i++) {
        ids[i] = i + 1;
        thread_creer(&clients[i], client_demo, &ids[i]);
    }
    for (i = 0; i < NB_CLIENTS; i++) thread_attendre(clients[i]);
    puts("\nSoldes apres demonstration :");
    for (i = 1; i <= NB_CLIENTS; i++) afficher_comptes(i);
    printf("Total avant : "); afficher_montant(avant);
    printf(" | Total apres : "); afficher_montant(banque_total());
    puts("\nSur une banque neuve : augmentation attendue de 225 EUR (depots - retraits).");
}

int main(int argc, char **argv) {
    int choix, client = 1, source, destination;
    Montant montant;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--demo") != 0)) {
        fprintf(stderr, "Usage : %s [--demo]\n", argv[0]); return EXIT_FAILURE;
    }
    banque_demarrer();
    mutex_creer(&affichage);
    if (argc == 2) demonstration();
    else {
        puts("SIMULATION BANCAIRE - 3 clients, 2 comptes de 1000 EUR chacun.");
        puts("Alice : 1,2 | Bob : 3,4 | Charlie : 5,6. Identites simulees, sans mot de passe.");
        for (;;) {
            printf("\nClient actif : %s\n", banque_client(client));
            puts("1. Depot\n2. Retrait\n3. Transfert\n4. Consultation\n5. Changer de client\n6. Mes comptes\n7. Demonstration concurrente\n0. Quitter");
            if (!lire_entier("Choix : ", 0, 7, &choix) || choix == 0) break;
            if (choix == 5) {
                if (!lire_entier("Client (1 Alice, 2 Bob, 3 Charlie) : ", 1, NB_CLIENTS, &client)) break;
            } else if (choix == 6) afficher_comptes(client);
            else if (choix == 7) demonstration();
            else {
                destination = 0; montant = 0;
                if (!lire_entier("Compte source : ", 1, NB_COMPTES, &source)) break;
                if (choix == TRANSFERT && !lire_entier("Compte destinataire : ", 1, NB_COMPTES, &destination)) break;
                if (choix != CONSULTATION && !lire_montant(&montant)) break;
                afficher_reponse(banque_envoyer(client, (Operation)choix, source, destination, montant));
            }
        }
    }
    banque_arreter();
    mutex_detruire(&affichage);
    puts("Banque fermee proprement. Les donnees de simulation ne sont pas conservees.");
    return EXIT_SUCCESS;
}
