/* libvrs C API (VRS_코덱스일체화_v1.md §4.1 P-5). One handle per library user; every function may be called from any thread. */
#ifndef VRS_H
#define VRS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct vrs_h vrs_h;
typedef struct {
  const char* sessions_dir;   /* the rollout tree (the lossless originals) */
  const char* index_dir;      /* the index shards; NULL or "" = no persistence */
  unsigned workers;           /* restore workers; 0 = min(12, hardware threads) */
  const unsigned* cpus;       /* worker i is pinned to cpus[i % ncpus]; NULL = not pinned */
  unsigned ncpus;
  int segwin;                 /* VRS_클로드코드_v1 4판 window (the service's VRS9_SEGWIN=1) */
  uint64_t budget;            /* libvrs extension (P-5 has no budget argument): token budget of vrs_assemble; 0 = 258400 */
} vrs_opts;

int  vrs_open(const vrs_opts*, vrs_h** out);          /* 0 ok; workers and the file watcher start, the call returns at once (P-2 runs in the background) */
int  vrs_ready(vrs_h*);                               /* 1 ready, 0 restoring, <0 error */
int  vrs_insert(vrs_h*, const char* path, uint64_t off, const char* line, size_t len);  /* line = the rollout line at byte offset off (no newline); idempotent per (path, off) */
int  vrs_written(vrs_h*, const char* path, uint64_t off);  /* the line inserted at (path, off) is on disk: the library lets go of its copy; 0 ok, -3 no copy held for that key, -2 unknown file, -1 bad argument */
int  vrs_assemble(vrs_h*, const char* path, char** json, size_t* len);  /* 0 ok, 1 not ready, 2 required input over budget, <0 error */
int  vrs_assemble_b(vrs_h*, const char* path, uint64_t budget, char** json, size_t* len);  /* the same with an explicit budget */
int  vrs_wait(vrs_h*, const char* path, uint64_t timeout_ms);  /* 6th stage: 0 = the store can answer `path` exactly (ready and its lineage set up; NULL path = ready), 1 = not within timeout_ms, <0 = error */
void vrs_free(char*);
void vrs_close(vrs_h*);

/* test hooks (not part of P-5) */
typedef struct {
  uint64_t items, turns, sessions, lines_applied, tail_lines, restored_items, cells_n, cells_hash;
  uint64_t theta_checked, theta_mismatch, theta_decreasing, theta_maxgroup;
  uint64_t shards_loaded, shards_rebuilt, shard_corrupt, shard_err, lemma_e;
  uint64_t b_total, b_miss, pend, tasks, lin_waits;  /* turns in the order (VI-08), selects that found nothing, closed turns not counted yet, tasks run by the workers, lineage waits registered */
  int64_t ready_us;
  uint64_t uw_lines, uw_bytes, uw_added, uw_released, uw_unknown;  /* P-3': unwritten lines held now (count, bytes), copies made / released, vrs_written on an unknown key */
  uint64_t fold_checked, fold_bad_parse, fold_bad_rec, fold_rec_bytes, perm_checked, perm_bad;  /* T-int-7 (only a library built with VRS_FOLD_CHECK counts these) */
  uint64_t holes_skipped, nul_stops, insert_left, insert_waits;  /* NUL-only holes skipped at restore, live reads that stopped at a NUL, vrs_insert calls that found the file taken and left their line to the taker, turns of every yield loop in the library (a thread gave up its time slice for another thread; 0 = nobody waited) */
  uint64_t late_events, sweep_left, reval_checked, recounts, count_retries, ctr_n, ctr_bytes, obs_rebuilt, a0_rebuilt, retired_n, retired_bytes, freed_n, settle_ok, settle_bad, dep_pairs, dep_viol;  /* P-8: turns that came in below others, sweep tasks in flight, contributions checked / counted again, counts redone because a turn came in below, contributions held (count, bytes), observers / A0 made again, replaced objects waiting to be freed (count, bytes) / freed so far, restored contributions taken / refused */
  uint64_t bad_lines, runs, run_max, y_max, x_max, x_total, y_total, revisit_tasks, pass_retries, dup_starts, lines_in;  /* stage 5 (appended; insert_left and sweep_left above stay for the layout and are always 0): lines the policy skipped, claimed runs / the longest, largest |Y| / re-seen x of one late turn, their totals, revisit tasks, observer retries by CAS, same-ordinal starts, lines handed in */
} vrs_dbg;
void vrs_dbg_stats(vrs_h*, vrs_dbg*);
const char* vrs_dbg_fold_msg(vrs_h*);   /* the first mismatch of the T-int-7 comparison, "" if none */
int  vrs_dbg_session_hash(vrs_h*, const char* path, uint64_t* out);
int  vrs_dbg_border_stat(vrs_h*, uint64_t* days, uint64_t* maxn, uint64_t* total);   /* buckets in use, largest bucket, turns in the order */
int  vrs_dbg_dump_cells(vrs_h*, const char* file);   /* every cell: index, an, ab, whether a lineage root is set, whether two are */
int  vrs_dbg_obs_dump(vrs_h*, const char* path, const char* file);   /* per closed turn: the observer fields */
int  vrs_dbg_unit_dump(vrs_h*, const char* file);   /* per turn: the contribution unit */
int  vrs_dbg_obs_hash(vrs_h*, const char* path, uint64_t* out);   /* the observers of all closed turns of one session (k, reference turns, rules), order-free */

#ifdef __cplusplus
}
#endif
#endif
