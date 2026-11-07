"""Tests Linux reels : processus, threads, signaux, isolation et nettoyage IPC.
Python est optionnel ; les tests du moteur restent ecrits en C (make test).
"""
from pathlib import Path
import os
import re
import signal
import subprocess
import time
import unittest

EXE = str(Path("build/banque").resolve())


def files_presentes():
    """Objets IPC System V existants : ("msg", id) pour les files, ("shm", id) pour les segments."""
    presents = set()
    for genre in ("msg", "shm"):
        lignes = Path(f"/proc/sysvipc/{genre}").read_text().splitlines()[1:]
        presents |= {(genre, int(ligne.split()[1])) for ligne in lignes}
    return presents


class Systeme(unittest.TestCase):
    def ouvrir(self):
        p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
        self.addCleanup(self.securite, p)
        ligne = p.stdout.readline()
        infos = re.search(r"Client PID=(\d+) \| Banque PID=(\d+) \| Files requetes=(\d+) reponses=(\d+)"
                          r" \| Memoire partagee=(\d+)", ligne)
        self.assertIsNotNone(infos, ligne)
        parent, serveur, q1, q2, segment = map(int, infos.groups())
        self.assertEqual(parent, p.pid)
        self.assertNotEqual(parent, serveur)
        ipc = {("msg", q1), ("msg", q2), ("shm", segment)}
        self.assertTrue(ipc <= files_presentes())
        return p, serveur, ipc

    @staticmethod
    def securite(p):
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.communicate()
        for flux in (p.stdin, p.stdout, p.stderr):
            if flux and not flux.closed:
                flux.close()

    def verifier_fermeture(self, p, serveur, files, entree="0\n", code=0):
        sortie, erreurs = p.communicate(entree, timeout=9)
        self.assertEqual(p.returncode, code, sortie + erreurs)
        self.assertIn("Banque fermee proprement", sortie)
        self.assertFalse(files & files_presentes())
        self.assertFalse(Path(f"/proc/{serveur}").exists(), "Serveur non recolte par waitpid")
        return sortie

    def test_processus_threads_et_sortie_normale(self):
        p, serveur, files = self.ouvrir()
        statut = Path(f"/proc/{serveur}/status").read_text()
        self.assertRegex(statut, r"Threads:\s+3\b")  # principal + deux employes
        self.verifier_fermeture(p, serveur, files)

    def test_saisies_et_transfert(self):
        p, serveur, files = self.ouvrir()
        entree = ("abc\n1\n1\n-2\n0\n12.345\n999999999999999999999\n12,50\n"
                  "3\n1\n3\n2.50\n5\n2\n4\n3\n0\n")
        sortie = self.verifier_fermeture(p, serveur, files, entree)
        for valeur in ("1012,50 EUR", "1010,00 EUR", "1002,50 EUR"):
            self.assertIn(valeur, sortie)
        self.assertEqual(sortie.count("Montant positif attendu"), 4)
        self.assertIn("Entrez un entier", sortie)

    def test_lignes_longues_et_eof(self):
        p, serveur, files = self.ouvrir()
        sortie = self.verifier_fermeture(p, serveur, files,
                                       "9" * 1000 + "\n1\n1\n" + "9" * 1000 + "\n")
        self.assertIn("Entrez un entier", sortie)
        self.assertIn("Montant positif attendu", sortie)

    def test_sigint_pendant_saisie(self):
        p, serveur, files = self.ouvrir()
        p.send_signal(signal.SIGINT)
        self.verifier_fermeture(p, serveur, files, entree=None, code=130)

    def test_sigterm_groupe(self):
        p, serveur, files = self.ouvrir()
        os.killpg(p.pid, signal.SIGTERM)
        self.verifier_fermeture(p, serveur, files, entree=None, code=130)

    def test_arret_serveur(self):
        p, serveur, files = self.ouvrir()
        os.kill(serveur, signal.SIGTERM)
        sortie = self.verifier_fermeture(p, serveur, files, "6\n", code=1)
        self.assertIn("Service indisponible", sortie)

    def test_serveur_tue_attente_bornee(self):
        p, serveur, files = self.ouvrir()
        os.kill(serveur, signal.SIGKILL)
        debut = time.monotonic()
        sortie = self.verifier_fermeture(p, serveur, files, "4\n1\n", code=1)
        self.assertIn("Service indisponible", sortie)
        self.assertLess(time.monotonic() - debut, 8)

    def test_deux_banques_independantes(self):
        p1, serveur1, files1 = self.ouvrir()
        p2, serveur2, files2 = self.ouvrir()
        self.assertFalse(files1 & files2)
        self.verifier_fermeture(p1, serveur1, files1, "1\n1\n100\n0\n")
        self.assertTrue(files2 <= files_presentes())
        sortie = self.verifier_fermeture(p2, serveur2, files2, "4\n1\n0\n")
        self.assertIn("1000,00 EUR", sortie)

    def test_demo_processus_et_total(self):
        p, serveur, files = self.ouvrir()
        sortie = self.verifier_fermeture(p, serveur, files, "7\n0\n")
        self.assertEqual(len(set(re.findall(r"\[(?:Alice|Bob|Charlie) PID=(\d+)\]", sortie))), 3)
        self.assertIn("Total apres : 6225,00 EUR", sortie)
        # 3 clients x (5 reussies + 1 refusee) puis 6 consultations des soldes.
        self.assertIn("Requetes reussies : 21 | refusees : 3", sortie)

    def test_tableau_memoire_partagee(self):
        p, serveur, files = self.ouvrir()
        sortie = self.verifier_fermeture(p, serveur, files, "1\n1\n100\n2\n3\n5000\n8\n0\n")
        self.assertIn("Tableau de bord (lu directement en memoire partagee)", sortie)
        self.assertIn("Compte 1 (Alice) : 1100,00 EUR", sortie)
        self.assertIn("Compte 3 (Bob) : 1000,00 EUR", sortie)
        self.assertIn("Total banque : 6100,00 EUR", sortie)
        self.assertIn("Requetes reussies : 1 | refusees : 1", sortie)


if __name__ == "__main__":
    unittest.main(verbosity=2)
