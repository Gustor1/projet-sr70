#ifndef SYNCHRONISATION_H
#define SYNCHRONISATION_H

/* Petite couche pour compiler le meme programme sous Windows et Linux. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
typedef HANDLE Thread;
typedef CRITICAL_SECTION Mutex;
typedef HANDLE Semaphore;
#define FONCTION_THREAD DWORD WINAPI
#define FIN_THREAD 0
static inline void mutex_creer(Mutex *m) { InitializeCriticalSection(m); }
static inline void mutex_prendre(Mutex *m) { EnterCriticalSection(m); }
static inline void mutex_liberer(Mutex *m) { LeaveCriticalSection(m); }
static inline void mutex_detruire(Mutex *m) { DeleteCriticalSection(m); }
static inline void semaphore_creer(Semaphore *s, unsigned n) {
    *s = CreateSemaphore(NULL, (LONG)n, 32767, NULL);
    if (!*s) { perror("CreateSemaphore"); exit(EXIT_FAILURE); }
}
static inline void semaphore_attendre(Semaphore *s) {
    if (WaitForSingleObject(*s, INFINITE) != WAIT_OBJECT_0) exit(EXIT_FAILURE);
}
static inline void semaphore_signaler(Semaphore *s) {
    if (!ReleaseSemaphore(*s, 1, NULL)) exit(EXIT_FAILURE);
}
static inline void semaphore_detruire(Semaphore *s) { CloseHandle(*s); }
static inline void thread_creer(Thread *t, DWORD (WINAPI *f)(void *), void *arg) {
    *t = CreateThread(NULL, 0, f, arg, 0, NULL);
    if (!*t) { perror("CreateThread"); exit(EXIT_FAILURE); }
}
static inline void thread_attendre(Thread t) {
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) exit(EXIT_FAILURE);
    CloseHandle(t);
}
#else
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
typedef pthread_t Thread;
typedef pthread_mutex_t Mutex;
typedef sem_t Semaphore;
#define FONCTION_THREAD void *
#define FIN_THREAD NULL
static inline void verifier(int erreur) {
    if (erreur) { fprintf(stderr, "Erreur de synchronisation: %d\n", erreur); exit(EXIT_FAILURE); }
}
static inline void mutex_creer(Mutex *m) { verifier(pthread_mutex_init(m, NULL)); }
static inline void mutex_prendre(Mutex *m) { verifier(pthread_mutex_lock(m)); }
static inline void mutex_liberer(Mutex *m) { verifier(pthread_mutex_unlock(m)); }
static inline void mutex_detruire(Mutex *m) { verifier(pthread_mutex_destroy(m)); }
static inline void semaphore_creer(Semaphore *s, unsigned n) { verifier(sem_init(s, 0, n)); }
static inline void semaphore_attendre(Semaphore *s) {
    while (sem_wait(s) != 0) { if (errno != EINTR) verifier(errno); }
}
static inline void semaphore_signaler(Semaphore *s) { verifier(sem_post(s)); }
static inline void semaphore_detruire(Semaphore *s) { verifier(sem_destroy(s)); }
static inline void thread_creer(Thread *t, void *(*f)(void *), void *arg) {
    verifier(pthread_create(t, NULL, f, arg));
}
static inline void thread_attendre(Thread t) { verifier(pthread_join(t, NULL)); }
#endif
#endif
