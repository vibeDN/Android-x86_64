// SPDX-License-Identifier: GPL-2.0-or-later
// TDLib-ish workload: sqlite (WAL file db, inserts/index/selects) + zlib deflate/inflate.
// usage: dbz_bench DBPATH      prints "<name> <ms> <checksum>" like bench.c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"
#include "zlib.h"

static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define CK(x) do { if ((x) != SQLITE_OK) { fprintf(stderr, "sqlite: %s at %d\n", sqlite3_errmsg(db), __LINE__); exit(1); } } while (0)

static uint64_t b_sqlite(const char* path) {
  sqlite3* db; sqlite3_stmt* st; uint64_t sum = 0;
  unlink(path);
  CK(sqlite3_open(path, &db));
  CK(sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
                      "CREATE TABLE msg(id INTEGER PRIMARY KEY, chat INT, date INT, text TEXT);", 0, 0, 0));
  CK(sqlite3_prepare_v2(db, "INSERT INTO msg(chat,date,text) VALUES(?,?,?)", -1, &st, 0));
  char text[128];
  for (int t = 0; t < 100; t++) {
    CK(sqlite3_exec(db, "BEGIN", 0, 0, 0));
    for (int i = 0; i < 1000; i++) {
      int n = t * 1000 + i;
      snprintf(text, sizeof text, "message %d in chat %d: lorem ipsum dolor sit amet %x", n, n % 97, n * 2654435761u);
      sqlite3_bind_int(st, 1, n % 97); sqlite3_bind_int(st, 2, 1700000000 + n); sqlite3_bind_text(st, 3, text, -1, SQLITE_STATIC);
      if (sqlite3_step(st) != SQLITE_DONE) { fprintf(stderr, "insert failed\n"); exit(1); }
      sqlite3_reset(st);
    }
    CK(sqlite3_exec(db, "COMMIT", 0, 0, 0));
  }
  sqlite3_finalize(st);
  CK(sqlite3_exec(db, "CREATE INDEX msg_chat ON msg(chat, date)", 0, 0, 0));
  CK(sqlite3_prepare_v2(db, "SELECT count(*), sum(length(text)) FROM msg WHERE chat=? AND date>?", -1, &st, 0));
  for (int q = 0; q < 2000; q++) {
    sqlite3_bind_int(st, 1, q % 97); sqlite3_bind_int(st, 2, 1700000000 + (q * 37) % 100000);
    while (sqlite3_step(st) == SQLITE_ROW) sum += (uint64_t)sqlite3_column_int64(st, 0) * 31 + (uint64_t)sqlite3_column_int64(st, 1);
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);
  CK(sqlite3_prepare_v2(db, "SELECT text FROM msg WHERE text LIKE '%chat 13:%' ORDER BY date DESC LIMIT 50", -1, &st, 0));
  while (sqlite3_step(st) == SQLITE_ROW) sum += (uint64_t)sqlite3_column_bytes(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(db);
  unlink(path);
  return sum;
}

static uint64_t b_zlib(void) {
  size_t n = 8 << 20; unsigned char* in = malloc(n);
  uint64_t r = 1;
  for (size_t i = 0; i < n; i++) { r = r * 6364136223846793005ull + 1442695040888963407ull; in[i] = (r >> 60) < 3 ? (unsigned char)(r >> 33) : "abcdefgh ijklmn "[i & 15]; }
  uLongf clen = compressBound(n); unsigned char* c = malloc(clen); unsigned char* d = malloc(n);
  if (compress2(c, &clen, in, n, 6) != Z_OK) exit(2);
  uLongf dlen = n;
  for (int k = 0; k < 4; k++) if (uncompress(d, &dlen, c, clen) != Z_OK || dlen != n) exit(3);
  uint64_t s = (uint64_t)clen << 32 | crc32(0, d, (uInt)dlen);
  free(in); free(c); free(d);
  return s;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/tmp/dbz_bench.db";
  double t = now_ms(); uint64_t s = b_sqlite(path); printf("%-8s %9.1f %016llx\n", "sqlite", now_ms() - t, (unsigned long long)s);
  fflush(stdout);
  t = now_ms(); s = b_zlib(); printf("%-8s %9.1f %016llx\n", "zlib", now_ms() - t, (unsigned long long)s);
  return 0;
}
