#!/usr/bin/env python3
"""Effet de chaque réglage alterné (src/torchlight_coeurs.cpp) par scène : régression sur les fenêtres, et paires voisines.

Usage : ab_regression.py <journal> nom=HH:MM:SS-HH:MM:SS [nom=...]   (segments de temps ; suffixe « ~ » au nom : le nombre
de dessins varie, il entre dans la régression). Variable : log de l'écart moyen entre images, et des autres mesures.
Marge : intervalle à 95 %. Fenêtres de chargement écartées (écart maximal >= 250 ms).
"""
import math
import re
import statistics as st
import sys

M = re.compile(r"(\d\d):(\d\d):(\d\d)\.\d+\].*\[coeurs\] etat=(\S+) masque=\S+ autres=(\d+) reserve=(-?\d+) images=(\d+) "
               r"dessins=(\d+) image=([\d.]+) attente=([\d.]+) ecart: moyen=([\d.]+) p50=([\d.]+) p99=([\d.]+) max=([\d.]+) "
               r"sup33=(\d+) sup50=(\d+)")


def ols(X, y):
    n, k = len(X), len(X[0])
    A = [[sum(X[r][i] * X[r][j] for r in range(n)) for j in range(k)] for i in range(k)]
    B = [sum(X[r][i] * y[r] for r in range(n)) for i in range(k)]
    M2 = [A[i][:] + [1.0 if i == j else 0.0 for j in range(k)] for i in range(k)]
    for i in range(k):
        p = max(range(i, k), key=lambda r: abs(M2[r][i]))
        M2[i], M2[p] = M2[p], M2[i]
        d = M2[i][i]
        M2[i] = [v / d for v in M2[i]]
        for r in range(k):
            if r != i:
                f = M2[r][i]
                M2[r] = [a - f * b for a, b in zip(M2[r], M2[i])]
    inv = [row[k:] for row in M2]
    beta = [sum(inv[i][j] * B[j] for j in range(k)) for i in range(k)]
    res = [y[r] - sum(X[r][i] * beta[i] for i in range(k)) for r in range(n)]
    s2 = sum(e * e for e in res) / max(1, n - k)
    return beta, [math.sqrt(max(0.0, s2 * inv[i][i])) for i in range(k)]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    W = []
    for l in open(sys.argv[1], errors="replace"):
        m = M.search(l)
        if m:
            g = m.groups()
            W.append(dict(t=int(g[0]) * 3600 + int(g[1]) * 60 + int(g[2]), etat={k: int(v) for k, v in (e.split(":") for e in g[3].split(","))},
                          n=int(g[6]), d=int(g[7]), image=float(g[8]), moyen=float(g[10]), p50=float(g[11]), p99=float(g[12]),
                          max=float(g[13]), s33=int(g[14]), s50=int(g[15]), rang=len(W)))
    facteurs = list(W[0]["etat"])
    for arg in sys.argv[2:]:
        nom, plage = arg.split("=")
        a, b = plage.split("-")
        h = lambda x: sum(int(v) * f for v, f in zip(x.split(":"), (3600, 60, 1)))
        F = [w for w in W if h(a) <= w["t"] <= h(b) and w["max"] < 250]
        varie = nom.endswith("~")
        print("%s : %d fenêtres, %.0f dessins (%d à %d), écart moyen %.2f ms" % (
            nom, len(F), st.mean(w["d"] for w in F), min(w["d"] for w in F), max(w["d"] for w in F), st.mean(w["moyen"] for w in F)))
        for c in ("moyen", "image", "p50", "p99"):
            X, y = [], []
            for w in F:
                ligne = [1.0] + [float(w["etat"][k]) for k in facteurs]
                if varie:
                    ligne.append(math.log(w["d"]))
                X.append(ligne)
                y.append(math.log(w[c]))
            b, se = ols(X, y)
            t = "  ".join("%s %+6.2f ± %.2f" % (k, 100 * (math.exp(b[i + 1]) - 1), 196 * se[i + 1]) for i, k in enumerate(facteurs))
            print("  %-6s %s" % (c, t))
        for k in facteurs:
            for v in (0, 1):
                G = [w for w in F if w["etat"][k] == v]
                if G:
                    im = sum(w["n"] for w in G)
                    print("  %-11s=%d fen %3d images %6d sup33 %5.2f %% sup50 %5.2f %% max>=100ms: %d max médian %5.1f maxmax %6.1f" % (
                        k, v, len(G), im, 100 * sum(w["s33"] for w in G) / im, 100 * sum(w["s50"] for w in G) / im,
                        sum(1 for w in G if w["max"] >= 100), st.median(w["max"] for w in G), max(w["max"] for w in G)))


if __name__ == "__main__":
    main()
