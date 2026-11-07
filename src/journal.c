/* Programme journal, lance par la banque avec fork + execl.
   Son entree standard a ete redirigee (dup2) vers le tube nomme :
   il lit les lignes envoyees par la banque et les recopie dans un fichier.
   Usage : journal FICHIER NOM_MEMOIRE_POSIX */
#include "journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
    char ligne[256];
    FILE *sortie;
    CompteurJournal *compteur;
    int fd;
    if (argc != 3) {
        fprintf(stderr, "Usage : %s FICHIER NOM_MEMOIRE_POSIX\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* Apres exec, on retrouve la memoire partagee par son nom. */
    fd = shm_open(argv[2], O_RDWR, 0);
    if (fd == -1) { perror("shm_open"); return EXIT_FAILURE; }
    compteur = mmap(NULL, sizeof(CompteurJournal), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (compteur == MAP_FAILED) { perror("mmap"); return EXIT_FAILURE; }

    sortie = fopen(argv[1], "w");
    if (sortie == NULL) { perror("fopen"); return EXIT_FAILURE; }
    fprintf(sortie, "=== Journal de la session (processus principal PID=%ld) ===\n", (long)getppid());

    /* fgets renvoie NULL quand la banque a ferme le tube. */
    while (fgets(ligne, sizeof ligne, stdin) != NULL) {
        fputs(ligne, sortie);
        sem_wait(&compteur->verrou);
        compteur->lignes++;
        sem_post(&compteur->verrou);
    }
    fprintf(sortie, "=== Fin : %lu operations ===\n", compteur->lignes);
    fclose(sortie);
    munmap(compteur, sizeof(CompteurJournal));
    return EXIT_SUCCESS;
}
