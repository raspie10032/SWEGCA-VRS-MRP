#!/usr/bin/env python3
"""Exhaustive small-state check of G2-T2, independent of daemon internals."""
import json


def check(cleaners):
    completed = 0
    # s=1,c=1,m=1, writer has append then publish. Each cleaner captures c,
    # checks s, unlinks only if covered, then rechecks and republishes if dirty.
    def visit(s, c, m, positions, eligible, observed):
        nonlocal completed
        if positions[0] == 2 and all(p == 4 for p in positions[1:]):
            assert s <= c or m, (s, c, m, positions, eligible, observed)
            completed += 1
            return
        for actor, at in enumerate(positions):
            if at == (2 if actor == 0 else 4): continue
            ns, nc, nm = s, c, m
            ee, oo, pp = list(eligible), list(observed), list(positions)
            pp[actor] += 1
            if actor == 0:
                if at == 0: ns += 1
                else: nm = True
            else:
                k = actor - 1
                if at == 0: oo[k] = c
                if at == 1: ee[k] = oo[k] >= s
                if at == 2 and ee[k]: nm = False
                if at == 3 and ee[k] and s > oo[k]: nm = True
            visit(ns, nc, nm, pp, ee, oo)
    visit(1, 1, True, [0] * (cleaners + 1), [False] * cleaners, [0] * cleaners)
    return completed


if __name__ == '__main__':
    print(json.dumps({'one_cleaner_interleavings': check(1), 'two_cleaner_interleavings': check(2),
                      'pass': True, 'boundary': 'finite interleavings; no host hook or durability claim'}))
