/*
 * main.c - Programme principal : menu interactif et demonstration.
 *
 * Le processus principal joue le role du client choisi dans le menu.
 * La demonstration (choix 7 ou option --demo) cree trois vrais processus
 * clients avec fork, qui envoient leurs operations en meme temps.
 */
#include "banque.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* Les montants sont stockes en centimes (entiers) pour eviter les erreurs
   d'arrondi des float : 1234 centimes s'affichent "12,34 EUR". */
static void afficher_montant(Montant montant) {
    printf("%lld,%02lld EUR", montant / 100, montant % 100);
}

static void afficher_reponse(Reponse r) {
    printf("%s", banque_message(r.statut));
    if (r.solde >= 0) { printf(" | solde source : "); afficher_montant(r.solde); }
    printf("\n");
}

/* Lit une ligne au clavier. fgets (plutot que scanf) permet de rejeter
   proprement une saisie incorrecte. Retourne 0 en fin d'entree ou apres
   Ctrl+C (fgets est alors interrompu par le signal). */
static int lire_ligne(const char *question, char *texte, size_t taille) {
    int c;
    if (banque_interrompue()) return 0;
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

/* Redemande tant que la saisie n'est pas un entier entre min et max. */
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

/* Consulte les deux comptes du client en passant par la banque (messages). */
static int afficher_comptes(int client) {
    int compte;
    printf("Comptes de %s (client %d) :\n", banque_client(client), client);
    for (compte = client * 2 - 1; compte <= client * 2; compte++) {
        Reponse rep = banque_envoyer(client, CONSULTATION, compte, 0, 0);
        printf("  Compte %d : ", compte);
        afficher_reponse(rep);
        if (rep.statut == SERVICE_INDISPONIBLE) return 0;
    }
    return 1;
}

/* Tableau de bord lu directement en memoire partagee, sans message. */
static int afficher_tableau(void) {
    TableauBord t;
    unsigned long lignes;
    int compte;
    if (!banque_lire_tableau(&t)) {
        puts("Tableau de bord indisponible."); return 0;
    }
    puts("Tableau de bord (lu directement en memoire partagee) :");
    for (compte = 1; compte <= NB_COMPTES; compte++) {
        printf("  Compte %d (%s) : ", compte, banque_client((compte + 1) / 2));
        afficher_montant(t.soldes[compte - 1]);
        printf("\n");
    }
    printf("  Total banque : "); afficher_montant(t.total);
    printf("\n  Requetes reussies : %lu | refusees : %lu\n", t.reussies, t.refusees);
    if (banque_lignes_journal(&lignes))
        printf("  Operations recues par le journal (memoire POSIX) : %lu\n", lignes);
    return 1;
}

/* Code execute par chaque processus client de la demonstration :
   depot, retrait, transfert interne, transfert vers le client suivant,
   retrait trop grand (doit etre refuse), puis consultation. */
static int client_demo(int client) {
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
        if (r.statut == SERVICE_INDISPONIBLE) {
            printf("[%s PID=%ld] %s : %s\n", banque_client(client),
                   (long)getpid(), libelles[i], banque_message(r.statut));
            return 0;
        }
        printf("[%s PID=%ld] %s : %s | solde source : %lld,%02lld EUR\n",
               banque_client(client), (long)getpid(), libelles[i],
               banque_message(r.statut), r.solde / 100,
               r.solde >= 0 ? r.solde % 100 : 0);
    }
    return 1;
}

/* Cree trois processus clients (fork), les laisse travailler en parallele,
   les attend (waitpid) puis affiche les soldes et le tableau de bord. */
static int demonstration(void) {
    pid_t clients[NB_CLIENTS];
    int nombre = 0, i, ok = 1;
    Montant avant = banque_total(), apres;
    if (avant < 0 || banque_interrompue()) return 0;
    puts("\nTrois processus clients en parallele, deux threads employes dans la banque.");
    fflush(NULL);
    for (i = 0; i < NB_CLIENTS; i++) {
        pid_t pid = fork();
        if (pid == 0) { /* fils : un client, puis _exit pour ne pas revenir au menu */
            int resultat = client_demo(i + 1);
            fflush(stdout);
            _exit(resultat ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        if (pid == -1) { perror("fork client"); ok = 0; break; }
        clients[nombre++] = pid;
    }
    /* Le pere attend chaque fils : pas de processus zombie. Si Ctrl+C arrive
       pendant l'attente, les clients restants recoivent SIGTERM. */
    for (i = 0; i < nombre; i++) {
        int statut;
        pid_t attendu;
        do {
            int j;
            if (banque_interrompue()) {
                for (j = i; j < nombre; j++) kill(clients[j], SIGTERM);
            }
            attendu = waitpid(clients[i], &statut, 0);
        } while (attendu == -1 && errno == EINTR);
        if (attendu == -1 || !WIFEXITED(statut) || WEXITSTATUS(statut) != 0) ok = 0;
    }
    if (!ok || banque_interrompue()) return 0;
    puts("\nSoldes apres demonstration :");
    for (i = 1; i <= NB_CLIENTS; i++) {
        if (!afficher_comptes(i)) return 0;
    }
    apres = banque_total();
    if (apres < 0) return 0;
    printf("Total avant : "); afficher_montant(avant);
    printf(" | Total apres : "); afficher_montant(apres);
    puts("\nSur une banque neuve : augmentation attendue de 225 EUR (depots - retraits).");
    sleep(1); /* laisse au journal le temps de lire les dernieres lignes */
    return afficher_tableau();
}

int main(int argc, char **argv) {
    int choix, client = 1, source, destination, ok = 1;
    Montant montant;
    setvbuf(stdout, NULL, _IOLBF, 0); /* affichage ligne par ligne, meme redirige */
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--demo") != 0)) {
        fprintf(stderr, "Usage : %s [--demo]\n", argv[0]); return EXIT_FAILURE;
    }
    if (!banque_demarrer()) {
        fprintf(stderr, "Impossible de demarrer la banque.\n"); return EXIT_FAILURE;
    }
    banque_decrire();
    if (argc == 2) ok = demonstration();
    else {
        puts("SIMULATION BANCAIRE - 3 clients, 2 comptes de 1000 EUR chacun.");
        puts("Alice : 1,2 | Bob : 3,4 | Charlie : 5,6. Identites simulees, sans mot de passe.");
        for (;;) {
            printf("\nClient actif : %s\n", banque_client(client));
            puts("1. Depot\n2. Retrait\n3. Transfert\n4. Consultation\n5. Changer de client\n6. Mes comptes\n7. Demonstration concurrente\n8. Tableau de bord (memoire partagee)\n0. Quitter");
            if (!lire_entier("Choix : ", 0, 8, &choix) || choix == 0) break;
            if (choix == 8) {
                if (!afficher_tableau()) { ok = 0; break; }
            } else if (choix == 5) {
                if (!lire_entier("Client (1 Alice, 2 Bob, 3 Charlie) : ", 1, NB_CLIENTS, &client)) break;
            } else if (choix == 6) {
                if (!afficher_comptes(client)) { ok = 0; break; }
            }
            else if (choix == 7) {
                if (!demonstration()) { ok = 0; break; }
            }
            else {
                destination = 0; montant = 0;
                if (!lire_entier("Compte source : ", 1, NB_COMPTES, &source)) break;
                if (choix == TRANSFERT && !lire_entier("Compte destinataire : ", 1, NB_COMPTES, &destination)) break;
                if (choix != CONSULTATION && !lire_montant(&montant)) break;
                Reponse rep = banque_envoyer(client, (Operation)choix, source, destination, montant);
                afficher_reponse(rep);
                if (rep.statut == SERVICE_INDISPONIBLE) { ok = 0; break; }
            }
        }
    }
    banque_arreter(); /* arrete banque et journal, supprime toutes les ressources IPC */
    puts("Banque fermee proprement. Les donnees de simulation ne sont pas conservees.");
    return banque_interrompue() ? 130 : (ok ? EXIT_SUCCESS : EXIT_FAILURE);
}
