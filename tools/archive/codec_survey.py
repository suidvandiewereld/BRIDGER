"""Round-trip a random sample of game assets through core_codec and report what fails.

    python tools/archive/codec_survey.py [count] [seed]
"""
import collections
import os
import random
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import core_codec as K
import decima_archive as D

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    paths = open(os.path.join(ROOT, "captures", "core", "prefetch_paths.txt")).read().split()
    index = {}
    archives = [D.Archive(p) for p in D.archives()]
    for a in archives:
        for h in a.files:
            index[h] = a
    candidates = [p for p in paths if D.path_hash(p) in index]
    sample = random.sample(candidates, min(count, len(candidates)))
    codec = K.Codec()
    ok = 0
    failures = collections.Counter()
    examples = {}
    types_seen = collections.Counter()
    for p in sample:
        data = index[D.path_hash(p)].read(D.path_hash(p))
        try:
            objs = codec.decode(data)
            for o in objs:
                types_seen[o["$type"] or "?undecoded"] += 1
            if codec.encode(objs) == data:
                ok += 1
            else:
                failures["roundtrip differs"] += 1
                examples.setdefault("roundtrip differs", p)
        except Exception as e:
            key = "%s: %s" % (type(e).__name__, str(e)[:120])
            failures[key] += 1
            examples.setdefault(key, p)
    print("round-tripped %d / %d" % (ok, len(sample)))
    for k, n in failures.most_common(25):
        print("  %4d  %s   e.g. %s" % (n, k, examples[k]))
    print("undecoded objects:", types_seen.get("?undecoded", 0), "of", sum(types_seen.values()))
    if codec.unknown:
        print("unknown types:", sorted(codec.unknown)[:40])


if __name__ == "__main__":
    main()
