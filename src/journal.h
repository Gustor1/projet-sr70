#ifndef JOURNAL_H
#define JOURNAL_H
#include <semaphore.h>

/* Memoire partagee POSIX entre le journal (qui ecrit) et le processus
   principal (qui lit pour le tableau de bord). */
typedef struct {
    sem_t verrou;
    unsigned long lignes; /* nombre d'operations recues par le journal */
} CompteurJournal;

#endif
