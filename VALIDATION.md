# Validation réalisée

Vérifications exécutées le 14 octobre 2026 sous Windows avec MinGW GCC 6.3.0.

## Compilation et tests C

Commande exécutée :

```powershell
./build.ps1 -Test -Demo
```

Les deux exécutables ont été compilés avec `-std=c11 -Wall -Wextra -Wpedantic -Werror -O2`. Aucun avertissement ni erreur de compilation.

Résultat des tests : **39 contrôles, 0 échec**. Le programme de test a ensuite été exécuté cinq fois supplémentaires avec un délai maximal de 30 secondes par exécution : toutes les exécutions ont réussi sans blocage.

Chaque exécution comprend deux scénarios de concurrence de 2 400 demandes chacun : retraits concurrents sur le même compte et transferts dans les deux sens. Les assertions portent sur les statuts, les soldes exacts et le total bancaire.

## Démonstration

Exécution réelle de trois threads clients et deux employés. Les trois retraits excessifs ont été refusés. Résultat final sur une banque neuve :

| Comptes | Solde final chacun |
|---|---|
| 1, 3, 5 | 1 025,00 € |
| 2, 4, 6 | 1 050,00 € |
| Total | 6 225,00 € |

## Interface console

Des entrées ont été envoyées à l'exécutable via un script Python ponctuel utilisant `subprocess.run`, avec contrôle du code de sortie et du texte produit. Python n'est pas nécessaire pour compiler, exécuter ou tester le cœur du projet.

Cas réellement vérifiés :

- Choix de menu non numérique : refus puis nouvelle saisie.
- Montants négatif, nul, à trois décimales ou excessivement grand : refus.
- Dépôt de `12,50` : solde exact de `1012,50 EUR`.
- Transfert de `2.50` : source à `1010,00 EUR`, destination à `1002,50 EUR`.
- Changement vers Bob puis consultation du compte destinataire.
- Lignes de 1 000 caractères dans le menu et la saisie du montant : rejet sans désynchroniser les saisies suivantes.
- Fin d'entrée pendant une opération : fermeture propre, sans blocage.

## Limites des preuves

La branche Linux/POSIX n'a pas été exécutée dans cet environnement. Aucun outil spécialisé de détection des courses mémoire n'a été lancé. Les tests concurrents donnent une preuve comportementale sur les scénarios exécutés ; ils ne constituent pas une preuve formelle pour tous les ordonnancements possibles.

L'ordre prioritaire est vérifié directement dans la file. Il ne garantit pas l'ordre de fin des opérations déjà extraites par deux employés. Il n'y a pas de persistance, d'authentification réelle, de processus clients indépendants ni de garantie de reprise après panne.
