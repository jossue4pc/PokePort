"""Builds PokePort's bundled initial pipeline cache (src/runtime/assets/pipeline/initial_pipeline_cache.db).

aurora merges this "seed" into each player's pipeline cache on start and compiles those pipelines while the
startup screen is shown, instead of the first time an effect appears in game (the source of first-time
stutters, e.g. breaking a crate). The upstream seed is Mario Kart Wii's, which PokePark never uses.

Each row is a GX pipeline *configuration* (TEV/blend/vertex-format state) recorded by aurora: no game code,
textures or models.

Usage (maintainers):
  python tools/build_pipeline_seed.py OUT.db CACHE.db [CACHE.db ...] [--exclude MKW_SEED.db]

Inputs are pipeline_cache.db files from PokePark\\UserData\\Cache after playing. Rows from --exclude are
dropped (except aurora's generic type-0 clear pipelines, which every game uses).
"""
import argparse
import os
import sqlite3

SCHEMA = 1
SEED_ID = 'pokepark-r8ae01'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out')
    parser.add_argument('caches', nargs='+')
    parser.add_argument('--exclude', action='append', default=[])
    parser.add_argument('--version', type=int, default=1)
    args = parser.parse_args()

    excluded = set()
    for path in args.exclude:
        db = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
        excluded |= {(t, h) for t, h in db.execute('SELECT type, hash FROM pipeline_cache WHERE type != 0')}
        db.close()

    rows = {}
    for path in args.caches:
        db = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
        for row in db.execute('SELECT type, hash, config_version, config_size, config, first_frame_used FROM pipeline_cache'):
            key = (row[0], row[1])
            if key in excluded:
                continue
            # Keep the earliest first use, so the most common pipelines are compiled first.
            if key not in rows or row[5] < rows[key][5]:
                rows[key] = row
        db.close()

    if os.path.exists(args.out):
        os.remove(args.out)
    out = sqlite3.connect(args.out)
    out.executescript('''
        CREATE TABLE aurora_schema(value INTEGER);
        CREATE TABLE pipeline_seed_metadata(seed_id TEXT PRIMARY KEY NOT NULL, version INTEGER NOT NULL);
        CREATE TABLE pipeline_cache (
          type INTEGER NOT NULL,
          hash INTEGER NOT NULL,
          config_version INTEGER NOT NULL,
          config_size INTEGER NOT NULL,
          config BLOB NOT NULL,
          first_frame_used INTEGER NOT NULL,
          PRIMARY KEY (type, hash)
        );
    ''')
    out.execute('INSERT INTO aurora_schema(value) VALUES (?)', (SCHEMA,))
    out.execute('INSERT INTO pipeline_seed_metadata(seed_id, version) VALUES (?, ?)', (SEED_ID, args.version))
    out.executemany('INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?)', sorted(rows.values(), key=lambda r: r[5]))
    out.commit()
    out.execute('VACUUM')
    out.close()
    by_type = {}
    for (t, _h) in rows:
        by_type[t] = by_type.get(t, 0) + 1
    print('%s: %d pipelines %s (excluded %d)' % (args.out, len(rows), by_type, len(excluded)))


if __name__ == '__main__':
    main()
