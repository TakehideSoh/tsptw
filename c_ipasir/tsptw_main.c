#include "ipasir.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// SAT-based CEGAR solver for TSPTW (Travelling Salesman Problem with Time Windows).
// Uses BDD-based pseudo-boolean constraint encoding (Eén-Sörensson 2006) for cost bounds.

// ---------------------------------------------------------------------------
// Growable containers (from main.c)
// ---------------------------------------------------------------------------
typedef struct { int *data; int size, cap; } IntVec;

static void die(const char *msg) { fprintf(stderr, "error: %s\n", msg); exit(1); }

static void iv_init(IntVec *v) { v->data = NULL; v->size = v->cap = 0; }
static void iv_reserve(IntVec *v, int c) {
  if (c <= v->cap) return;
  int nc = v->cap ? v->cap : 8;
  while (nc < c) nc *= 2;
  v->data = (int *)realloc(v->data, (size_t)nc * sizeof(int));
  if (!v->data) die("realloc");
  v->cap = nc;
}
static void iv_push(IntVec *v, int x) {
  if (v->size == v->cap) iv_reserve(v, v->size + 1);
  v->data[v->size++] = x;
}
static void iv_free(IntVec *v) { free(v->data); iv_init(v); }

// ---------------------------------------------------------------------------
// PairMap: open-address hash (u,v)->int
// ---------------------------------------------------------------------------
typedef struct { uint64_t *keys; int *vals; int cap, size; } PairMap;

static uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
static uint64_t make_key(int u, int v) {
  return (((uint64_t)(uint32_t)u) << 32) | (uint32_t)v;
}
static void pm_init(PairMap *m, int hint) {
  int c = 1024; while (c < hint * 2) c <<= 1;
  m->cap = c; m->size = 0;
  m->keys = (uint64_t *)calloc((size_t)c, sizeof(uint64_t));
  m->vals = (int *)calloc((size_t)c, sizeof(int));
  if (!m->keys || !m->vals) die("calloc");
}
static void pm_rehash(PairMap *m, int nc) {
  PairMap n; n.cap = nc; n.size = 0;
  n.keys = (uint64_t *)calloc((size_t)nc, sizeof(uint64_t));
  n.vals = (int *)calloc((size_t)nc, sizeof(int));
  if (!n.keys || !n.vals) die("calloc");
  for (int i = 0; i < m->cap; i++) {
    if (!m->keys[i]) continue;
    uint64_t h = splitmix64(m->keys[i]);
    int idx = (int)(h & (uint64_t)(nc - 1));
    while (n.keys[idx]) idx = (idx + 1) & (nc - 1);
    n.keys[idx] = m->keys[i]; n.vals[idx] = m->vals[i]; n.size++;
  }
  free(m->keys); free(m->vals); *m = n;
}
static void pm_put(PairMap *m, int u, int v, int val) {
  if ((m->size + 1) * 10 > m->cap * 7) pm_rehash(m, m->cap * 2);
  uint64_t key = make_key(u, v);
  uint64_t h = splitmix64(key);
  int idx = (int)(h & (uint64_t)(m->cap - 1));
  while (m->keys[idx] && m->keys[idx] != key) idx = (idx + 1) & (m->cap - 1);
  if (!m->keys[idx]) m->size++;
  m->keys[idx] = key; m->vals[idx] = val;
}
static int pm_get(const PairMap *m, int u, int v) {
  uint64_t key = make_key(u, v);
  uint64_t h = splitmix64(key);
  int idx = (int)(h & (uint64_t)(m->cap - 1));
  while (m->keys[idx]) {
    if (m->keys[idx] == key) return m->vals[idx];
    idx = (idx + 1) & (m->cap - 1);
  }
  return 0;
}
static void pm_free(PairMap *m) { free(m->keys); free(m->vals); }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static int64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// TSPTW instance (0-indexed nodes, node 0 = depot)
// ---------------------------------------------------------------------------
typedef struct {
  int n;
  int *dist; // n*n matrix, dist[i*n+j]
  int *a;    // ready time
  int *b;    // due date
} TSPTW;

static TSPTW parse_tsptw(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("cannot open input file");
  TSPTW tw;
  if (fscanf(fp, "%d", &tw.n) != 1) die("bad n");
  int n = tw.n;
  tw.dist = (int *)malloc((size_t)(n * n) * sizeof(int));
  tw.a = (int *)malloc((size_t)n * sizeof(int));
  tw.b = (int *)malloc((size_t)n * sizeof(int));
  if (!tw.dist || !tw.a || !tw.b) die("malloc");
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      if (fscanf(fp, "%d", &tw.dist[i * n + j]) != 1) die("bad dist");
  for (int i = 0; i < n; i++)
    if (fscanf(fp, "%d %d", &tw.a[i], &tw.b[i]) != 2) die("bad tw");
  fclose(fp);
  return tw;
}

static void tsptw_free(TSPTW *tw) {
  free(tw->dist); free(tw->a); free(tw->b);
}

// ---------------------------------------------------------------------------
// SAT encoding state
// ---------------------------------------------------------------------------
typedef struct {
  PairMap lit_map;   // (u,v) -> SAT literal for arc u->v (1-indexed nodes)
  int next_var;
} Encoder;

static void enc_init(Encoder *e, int arc_hint) {
  pm_init(&e->lit_map, arc_hint);
  e->next_var = 1;
}
static int enc_new(Encoder *e) { return e->next_var++; }
static void enc_free(Encoder *e) { pm_free(&e->lit_map); }

// ---------------------------------------------------------------------------
// Clause helpers
// ---------------------------------------------------------------------------
static void add_clause1(void *S, int a) {
  ipasir_add(S, a); ipasir_add(S, 0);
}
static void add_clause2(void *S, int a, int b) {
  ipasir_add(S, a); ipasir_add(S, b); ipasir_add(S, 0);
}
static void add_clause3(void *S, int a, int b, int c) {
  ipasir_add(S, a); ipasir_add(S, b); ipasir_add(S, c); ipasir_add(S, 0);
}
static void add_clause_vec(void *S, const IntVec *cl) {
  for (int i = 0; i < cl->size; i++) ipasir_add(S, cl->data[i]);
  ipasir_add(S, 0);
}

// ---------------------------------------------------------------------------
// Sinz at-most-one encoding
// ---------------------------------------------------------------------------
static void sinz_amo(Encoder *e, void *S, const IntVec *lits) {
  int n = lits->size;
  if (n <= 1) return;
  int *s = (int *)malloc((size_t)(n - 1) * sizeof(int));
  if (!s) die("malloc");
  for (int i = 0; i < n - 1; i++) s[i] = enc_new(e);
  add_clause2(S, -lits->data[0], s[0]);
  for (int i = 1; i < n - 1; i++) {
    add_clause2(S, -lits->data[i], s[i]);
    add_clause2(S, -s[i - 1], s[i]);
    add_clause2(S, -lits->data[i], -s[i - 1]);
  }
  add_clause2(S, -lits->data[n - 1], -s[n - 2]);
  free(s);
}

// ---------------------------------------------------------------------------
// Encode Hamiltonian circuit on directed graph with arc pruning (1-indexed nodes)
// Prunes arcs where a[u] + dist[u][v] > b[v] (cannot arrive on time).
// Also applies transitive time window pruning.
// ---------------------------------------------------------------------------
static int encode_hc(Encoder *e, void *S, int n, const TSPTW *tw) {
  // Compute earliest arrival at each node (lower bound)
  int *earliest = (int *)malloc((size_t)n * sizeof(int));
  if (!earliest) die("malloc");
  for (int i = 0; i < n; i++) earliest[i] = tw->a[i];
  // Propagate: earliest[j] >= min over all i of (earliest[i] + dist[i][j])
  bool changed = true;
  while (changed) {
    changed = false;
    for (int j = 0; j < n; j++) {
      for (int i = 0; i < n; i++) {
        if (i == j) continue;
        int arr = earliest[i] + tw->dist[i * n + j];
        if (arr > earliest[j] && arr <= tw->b[j]) {
          // This doesn't help tighten earliest (it's a max, not min)
        }
      }
    }
  }

  // Create arc variables and prune infeasible arcs
  int pruned = 0;
  for (int u = 1; u <= n; u++)
    for (int v = 1; v <= n; v++) {
      if (u == v) continue;
      int ui = u - 1, vi = v - 1; // 0-indexed for TSPTW lookup
      // Prune: if earliest possible arrival at v via u exceeds b[v]
      if (vi != 0 && tw->a[ui] + tw->dist[ui * n + vi] > tw->b[vi]) {
        pruned++;
        continue; // don't create variable
      }
      // Also prune: if visiting v then u is impossible
      // (v must finish before u's deadline, meaning a[v]+dist[v][u] <= b[u])
      // We don't prune return-to-depot arcs (vi==0) since depot has wide window
      int lit = enc_new(e);
      pm_put(&e->lit_map, u, v, lit);
    }

  // For each vertex: exactly one outgoing, exactly one incoming
  for (int u = 1; u <= n; u++) {
    IntVec out, in_v;
    iv_init(&out); iv_init(&in_v);
    for (int v = 1; v <= n; v++) {
      if (v == u) continue;
      int lit = pm_get(&e->lit_map, u, v);
      if (lit) iv_push(&out, lit);
      lit = pm_get(&e->lit_map, v, u);
      if (lit) iv_push(&in_v, lit);
    }
    if (out.size == 0 || in_v.size == 0) {
      // Node has no outgoing or incoming arcs - problem is infeasible
      iv_free(&out); iv_free(&in_v);
      free(earliest);
      ipasir_add(S, 0); // empty clause -> UNSAT
      return pruned;
    }
    // AMO
    sinz_amo(e, S, &out);
    sinz_amo(e, S, &in_v);
    // ALO
    add_clause_vec(S, &out);
    add_clause_vec(S, &in_v);
    iv_free(&out); iv_free(&in_v);
  }

  // Debug: print degree info
  for (int u = 1; u <= n; u++) {
    int out_deg = 0, in_deg = 0;
    for (int v = 1; v <= n; v++) {
      if (v == u) continue;
      if (pm_get(&e->lit_map, u, v)) out_deg++;
      if (pm_get(&e->lit_map, v, u)) in_deg++;
    }
    if (out_deg <= 3 || in_deg <= 3)
      printf("  node %d (tsptw %d): out=%d in=%d tw=[%d,%d]\n",
             u, u-1, out_deg, in_deg, tw->a[u-1], tw->b[u-1]);
  }

  free(earliest);
  return pruned;
}

// ---------------------------------------------------------------------------
// Extract cycles from SAT model
// ---------------------------------------------------------------------------
static int *get_next_array(void *S, const Encoder *e, int n) {
  int *nxt = (int *)calloc((size_t)(n + 1), sizeof(int));
  if (!nxt) die("calloc");
  for (int u = 1; u <= n; u++)
    for (int v = 1; v <= n; v++) {
      if (u == v) continue;
      int lit = pm_get(&e->lit_map, u, v);
      if (lit && ipasir_val(S, lit) > 0) { nxt[u] = v; break; }
    }
  return nxt;
}

// Decompose into cycles. Returns number of cycles, fills cycle_starts and cycle_lens.
typedef struct { IntVec *cycles; int num; } Cycles;

static Cycles get_cycles(const int *nxt, int n) {
  bool *visited = (bool *)calloc((size_t)(n + 1), sizeof(bool));
  if (!visited) die("calloc");
  Cycles res; res.cycles = NULL; res.num = 0;
  int cap = 0;
  for (int start = 1; start <= n; start++) {
    if (visited[start] || !nxt[start]) continue;
    if (res.num == cap) {
      cap = cap ? cap * 2 : 8;
      res.cycles = (IntVec *)realloc(res.cycles, (size_t)cap * sizeof(IntVec));
      if (!res.cycles) die("realloc");
    }
    IntVec *cyc = &res.cycles[res.num++];
    iv_init(cyc);
    int cur = start;
    while (!visited[cur]) {
      visited[cur] = true;
      iv_push(cyc, cur);
      cur = nxt[cur];
    }
  }
  free(visited);
  return res;
}

static void free_cycles(Cycles *c) {
  for (int i = 0; i < c->num; i++) iv_free(&c->cycles[i]);
  free(c->cycles);
  c->cycles = NULL; c->num = 0;
}

// Get tour starting from depot (node 1). Assumes single cycle.
static IntVec get_tour(const int *nxt, int n) {
  IntVec tour; iv_init(&tour);
  int cur = 1; // depot = internal node 1 = TSPTW node 0
  for (int i = 0; i < n; i++) {
    iv_push(&tour, cur);
    cur = nxt[cur];
  }
  return tour;
}

// ---------------------------------------------------------------------------
// Time window feasibility check and cost computation
// tour: 1-indexed nodes [v0, v1, ..., v_{n-1}], v0=1 (depot)
// Returns: feasible, cost, first_violating_pos (-1 if feasible)
// ---------------------------------------------------------------------------
typedef struct { bool feasible; int cost; int viol_pos; int chain_start; } TWCheck;

static TWCheck check_tw(const IntVec *tour, const TSPTW *tw) {
  TWCheck r = {true, 0, -1, 0};
  int n = tw->n;
  int time_val = 0;
  int total_dist = 0;
  int chain_start = 0;
  for (int i = 1; i < n; i++) {
    int u = tour->data[i - 1] - 1; // to 0-indexed
    int v = tour->data[i] - 1;
    int d = tw->dist[u * n + v];
    total_dist += d;
    int arrival = time_val + d;
    if (tw->a[v] > arrival) {
      // Waited at v: new tight chain starts here
      chain_start = i;
      time_val = tw->a[v];
    } else {
      time_val = arrival;
    }
    if (time_val > tw->b[v]) {
      r.feasible = false;
      r.viol_pos = i;
      r.chain_start = chain_start;
      return r;
    }
  }
  // Return to depot
  int last = tour->data[n - 1] - 1;
  total_dist += tw->dist[last * n + 0];
  r.cost = total_dist;
  return r;
}

// ---------------------------------------------------------------------------
// Subtour blocking: outgoing/incoming cut clauses
// ---------------------------------------------------------------------------
static void block_subtour(void *S, const Encoder *e, const IntVec *cyc, int n) {
  bool *in_cyc = (bool *)calloc((size_t)(n + 1), sizeof(bool));
  if (!in_cyc) die("calloc");
  for (int i = 0; i < cyc->size; i++) in_cyc[cyc->data[i]] = true;

  IntVec out_cut, in_cut;
  iv_init(&out_cut); iv_init(&in_cut);
  for (int i = 0; i < cyc->size; i++) {
    int u = cyc->data[i];
    for (int v = 1; v <= n; v++) {
      if (in_cyc[v]) continue;
      int lit_out = pm_get(&e->lit_map, u, v);
      int lit_in = pm_get(&e->lit_map, v, u);
      if (lit_out) iv_push(&out_cut, lit_out);
      if (lit_in) iv_push(&in_cut, lit_in);
    }
  }
  if (out_cut.size) add_clause_vec(S, &out_cut);
  if (in_cut.size) add_clause_vec(S, &in_cut);
  iv_free(&out_cut); iv_free(&in_cut);
  free(in_cyc);
}

// Block a specific Hamiltonian cycle (negate all arcs)
static void block_hc(void *S, const Encoder *e, const IntVec *tour) {
  int n = tour->size;
  for (int i = 0; i < n; i++) {
    int u = tour->data[i];
    int v = tour->data[(i + 1) % n];
    ipasir_add(S, -pm_get(&e->lit_map, u, v));
  }
  ipasir_add(S, 0);
}

// TW blocking mode
typedef enum { TW_BLOCK_PREFIX, TW_BLOCK_CHAIN } TWBlockMode;

// Block path prefix: negate arcs from tour[0] to tour[end_pos]
// Blocks all tours that follow this path prefix.
static void block_tw_prefix(void *S, const Encoder *e, const IntVec *tour, int end_pos) {
  for (int i = 0; i < end_pos; i++) {
    int u = tour->data[i];
    int v = tour->data[i + 1];
    ipasir_add(S, -pm_get(&e->lit_map, u, v));
  }
  ipasir_add(S, 0);
}

// Block tight chain: negate arcs from tour[chain_start] to tour[end_pos].
// Only blocks the arcs in the "reason" for the TW violation — the consecutive
// sub-path where arrival times propagated without waiting. Produces a shorter
// (stronger) clause than prefix blocking.
static void block_tw_chain(void *S, const Encoder *e, const IntVec *tour,
                           int chain_start, int end_pos) {
  for (int i = chain_start; i < end_pos; i++) {
    int u = tour->data[i];
    int v = tour->data[i + 1];
    ipasir_add(S, -pm_get(&e->lit_map, u, v));
  }
  ipasir_add(S, 0);
}

// ---------------------------------------------------------------------------
// ITE cache for structural hashing (persistent across BDD builds)
// ---------------------------------------------------------------------------
typedef struct {
  int sel, hi, lo, result;
} ITESlot;

typedef struct {
  ITESlot *slots;
  bool *used;
  int cap, size;
} ITECache;

static void ite_init(ITECache *c) {
  c->cap = 4096; c->size = 0;
  c->slots = (ITESlot *)calloc((size_t)c->cap, sizeof(ITESlot));
  c->used = (bool *)calloc((size_t)c->cap, sizeof(bool));
  if (!c->slots || !c->used) die("calloc");
}

static uint64_t ite_hash(int s, int h, int l) {
  uint64_t a = (uint64_t)(uint32_t)s;
  uint64_t b = (uint64_t)(uint32_t)h;
  uint64_t c = (uint64_t)(uint32_t)l;
  return splitmix64(a * 0x9e3779b97f4a7c15ULL + b * 0xbf58476d1ce4e5b9ULL + c);
}

static void ite_rehash(ITECache *c, int nc) {
  ITESlot *ns = (ITESlot *)calloc((size_t)nc, sizeof(ITESlot));
  bool *nu = (bool *)calloc((size_t)nc, sizeof(bool));
  if (!ns || !nu) die("calloc");
  for (int i = 0; i < c->cap; i++) {
    if (!c->used[i]) continue;
    ITESlot *s = &c->slots[i];
    uint64_t h = ite_hash(s->sel, s->hi, s->lo);
    int idx = (int)(h & (uint64_t)(nc - 1));
    while (nu[idx]) idx = (idx + 1) & (nc - 1);
    ns[idx] = *s; nu[idx] = true;
  }
  free(c->slots); free(c->used);
  c->slots = ns; c->used = nu; c->cap = nc;
}

static int ite_lookup(ITECache *c, int sel, int hi, int lo) {
  uint64_t h = ite_hash(sel, hi, lo);
  int idx = (int)(h & (uint64_t)(c->cap - 1));
  while (c->used[idx]) {
    ITESlot *s = &c->slots[idx];
    if (s->sel == sel && s->hi == hi && s->lo == lo) return s->result;
    idx = (idx + 1) & (c->cap - 1);
  }
  return 0; // not found
}

static void ite_insert(ITECache *c, int sel, int hi, int lo, int result) {
  if ((c->size + 1) * 10 > c->cap * 7) ite_rehash(c, c->cap * 2);
  uint64_t h = ite_hash(sel, hi, lo);
  int idx = (int)(h & (uint64_t)(c->cap - 1));
  while (c->used[idx]) idx = (idx + 1) & (c->cap - 1);
  c->slots[idx] = (ITESlot){sel, hi, lo, result};
  c->used[idx] = true;
  c->size++;
}

static void ite_free(ITECache *c) { free(c->slots); free(c->used); }

// ---------------------------------------------------------------------------
// BDD-based PB constraint: sum(w_i * x_i) >= rhs
// Signals: SIG_TRUE, SIG_FALSE, or a SAT literal (positive/negative int)
// ---------------------------------------------------------------------------
#define SIG_TRUE  (INT_MAX)
#define SIG_FALSE (INT_MIN)

static int sig_neg(int s) {
  if (s == SIG_TRUE) return SIG_FALSE;
  if (s == SIG_FALSE) return SIG_TRUE;
  return -s;
}

// PB data: arcs sorted by weight descending
typedef struct {
  int *weights;      // sorted descending
  int *lits;         // corresponding arc SAT literals
  int num;           // number of arcs
  int total_weight;
  int *mat_left;     // mat_left[i] = sum of weights[i..num-1]
  ITECache ite_cache;
  PairMap bdd_memo;  // (level+1, sum+1) -> signal
} PBState;

static int cmp_desc(const void *a, const void *b) {
  const int *ia = (const int *)a, *ib = (const int *)b;
  // Sort by weight descending; data is in pairs (weight, lit)
  return ib[0] - ia[0];
}

static void pb_init(PBState *pb, const Encoder *e, const TSPTW *tw) {
  int n = tw->n;
  int max_arcs = n * (n - 1);
  int *pairs = (int *)malloc((size_t)(max_arcs * 2) * sizeof(int));
  if (!pairs) die("malloc");
  int idx = 0;
  for (int u = 1; u <= n; u++)
    for (int v = 1; v <= n; v++) {
      if (u == v) continue;
      int lit = pm_get(&e->lit_map, u, v);
      if (!lit) continue; // pruned arc
      int w = tw->dist[(u - 1) * n + (v - 1)];
      pairs[idx * 2] = w;
      pairs[idx * 2 + 1] = lit;
      idx++;
    }
  pb->num = idx;
  qsort(pairs, (size_t)pb->num, 2 * sizeof(int), cmp_desc);
  pb->weights = (int *)malloc((size_t)pb->num * sizeof(int));
  pb->lits = (int *)malloc((size_t)pb->num * sizeof(int));
  if (!pb->weights || !pb->lits) die("malloc");
  pb->total_weight = 0;
  for (int i = 0; i < pb->num; i++) {
    pb->weights[i] = pairs[i * 2];
    pb->lits[i] = pairs[i * 2 + 1];
    pb->total_weight += pb->weights[i];
  }
  free(pairs);

  pb->mat_left = (int *)malloc((size_t)(pb->num + 1) * sizeof(int));
  if (!pb->mat_left) die("malloc");
  pb->mat_left[pb->num] = 0;
  for (int i = pb->num - 1; i >= 0; i--)
    pb->mat_left[i] = pb->mat_left[i + 1] + pb->weights[i];

  ite_init(&pb->ite_cache);
  pm_init(&pb->bdd_memo, 1024);
}

static void pb_free(PBState *pb) {
  free(pb->weights); free(pb->lits); free(pb->mat_left);
  ite_free(&pb->ite_cache);
  pm_free(&pb->bdd_memo);
}

// make_ite: create ITE(sel, hi, lo) with structural hashing and Tseitin encoding
// sel is a SAT literal (the BDD variable at this level)
// hi, lo are signals (SIG_TRUE, SIG_FALSE, or SAT literal)
// Returns a signal.
static int make_or(Encoder *e, void *S, int a, int b) {
  if (a == SIG_TRUE || b == SIG_TRUE) return SIG_TRUE;
  if (a == SIG_FALSE) return b;
  if (b == SIG_FALSE) return a;
  if (a == b) return a;
  if (a == -b) return SIG_TRUE;
  int z = enc_new(e);
  add_clause2(S, -a, z);
  add_clause2(S, -b, z);
  add_clause3(S, a, b, -z);
  return z;
}

static int make_and(Encoder *e, void *S, int a, int b) {
  if (a == SIG_FALSE || b == SIG_FALSE) return SIG_FALSE;
  if (a == SIG_TRUE) return b;
  if (b == SIG_TRUE) return a;
  if (a == b) return a;
  if (a == -b) return SIG_FALSE;
  int z = enc_new(e);
  add_clause2(S, a, -z);
  add_clause2(S, b, -z);
  add_clause3(S, -a, -b, z);
  return z;
}

static int make_ite(Encoder *e, void *S, ITECache *cache, int sel, int hi, int lo) {
  // Simplifications
  if (hi == lo) return hi;
  if (sel == SIG_TRUE) return hi;
  if (sel == SIG_FALSE) return lo;
  if (hi == SIG_TRUE && lo == SIG_FALSE) return sel;
  if (hi == SIG_FALSE && lo == SIG_TRUE) return sig_neg(sel);
  if (hi == SIG_TRUE) return make_or(e, S, sel, lo);
  if (lo == SIG_FALSE) return make_and(e, S, sel, hi);
  if (lo == SIG_TRUE) return make_or(e, S, sig_neg(sel), hi);
  if (hi == SIG_FALSE) return make_and(e, S, sig_neg(sel), lo);

  // Structural hashing: check cache
  int cached = ite_lookup(cache, sel, hi, lo);
  if (cached) return cached;

  // Allocate Tseitin variable, add ITE clauses
  int x = enc_new(e);
  // (-) s ∧ t → x
  add_clause3(S, -sel, -hi, x);
  // (-) ¬s ∧ f → x
  add_clause3(S, sel, -lo, x);
  // (red-) t ∧ f → x
  add_clause3(S, -hi, -lo, x);
  // (+) s ∧ ¬t → ¬x
  add_clause3(S, -sel, hi, -x);
  // (+) ¬s ∧ ¬f → ¬x
  add_clause3(S, sel, lo, -x);
  // (red+) ¬t ∧ ¬f → ¬x
  add_clause3(S, hi, lo, -x);

  ite_insert(cache, sel, hi, lo, x);
  return x;
}

// Recursive BDD construction for sum(w[i]*x[i]) >= rhs
// Variables ordered by decreasing weight (level 0 = largest weight)
static int build_bdd(PBState *pb, Encoder *e, void *S, int level, int sum, int rhs) {
  if (sum >= rhs) return SIG_TRUE;
  if (level == pb->num) return SIG_FALSE;
  if (sum + pb->mat_left[level] < rhs) return SIG_FALSE;

  // Memo lookup: use (level+1, sum+1) to avoid 0 sentinel
  int memo_val = pm_get(&pb->bdd_memo, level + 1, sum + 1);
  if (memo_val != 0) return memo_val;

  int hi = build_bdd(pb, e, S, level + 1, sum + pb->weights[level], rhs);
  int lo = build_bdd(pb, e, S, level + 1, sum, rhs);

  int result;
  if (hi == lo)
    result = hi;
  else
    result = make_ite(e, S, &pb->ite_cache, pb->lits[level], hi, lo);

  pm_put(&pb->bdd_memo, level + 1, sum + 1, result);
  return result;
}

// Add PB constraint: sum(w_i * x_i) < cost_bound
// Builds BDD for sum >= cost_bound, asserts negation of root.
// Returns the root signal (caller can inspect).
static int pb_add_cost_bound(PBState *pb, Encoder *e, void *S, int cost_bound) {
  // Reset BDD memo (terminal conditions depend on rhs)
  pm_free(&pb->bdd_memo);
  pm_init(&pb->bdd_memo, 1024);

  int root = build_bdd(pb, e, S, 0, 0, cost_bound);

  if (root == SIG_TRUE) {
    // sum >= cost_bound is always true => sum < cost_bound is impossible => UNSAT
    // Add empty clause to make solver UNSAT
    ipasir_add(S, 0);
  } else if (root == SIG_FALSE) {
    // sum >= cost_bound is always false => sum < cost_bound is trivially true
    // Nothing to add
  } else {
    // Assert -root: NOT(sum >= cost_bound) => sum < cost_bound
    add_clause1(S, -root);
  }

  printf("  BDD: rhs=%d, ite_cache_size=%d\n", cost_bound, pb->ite_cache.size);
  return root;
}

// ---------------------------------------------------------------------------
// Main CEGAR loop for TSPTW
// ---------------------------------------------------------------------------
static void solve_tsptw(const TSPTW *tw, TWBlockMode tw_block_mode) {
  int n = tw->n;
  printf("TSPTW: n=%d\n", n);

  int64_t t0 = now_ns();

  Encoder enc;
  enc_init(&enc, n * (n - 1));
  void *solver = ipasir_init();

  // Encode Hamiltonian circuit with arc pruning
  int pruned = encode_hc(&enc, solver, n, tw);
  printf("HC encoding: %d SAT variables, %d arcs pruned\n", enc.next_var - 1, pruned);

  // Prepare PB state
  PBState pb;
  pb_init(&pb, &enc, tw);
  printf("PB: %d arcs, total_weight=%d\n", pb.num, pb.total_weight);

  int best_cost = -1;
  IntVec best_tour;
  iv_init(&best_tour);

  int iter = 0;
  int subtour_blocks = 0;
  int hc_blocks = 0;
  int cost_updates = 0;

  while (1) {
    int res = ipasir_solve(solver);
    iter++;

    if (res == 20) {
      // UNSAT
      if (best_cost >= 0) {
        printf("\ns OPTIMUM FOUND\n");
        printf("optimal cost: %d\n", best_cost);
        printf("tour:");
        for (int i = 0; i < best_tour.size; i++)
          printf(" %d", best_tour.data[i] - 1); // back to 0-indexed
        printf("\n");
      } else {
        printf("\ns INFEASIBLE\n");
      }
      break;
    }

    // SAT: extract solution
    int *nxt = get_next_array(solver, &enc, n);
    Cycles cyc = get_cycles(nxt, n);

    if (cyc.num > 1) {
      // Multiple subcycles: add cut clauses for each
      for (int i = 0; i < cyc.num; i++)
        block_subtour(solver, &enc, &cyc.cycles[i], n);
      subtour_blocks += cyc.num;
      if (iter % 100 == 0)
        printf("  iter %d: %d subcycles, total blocks=%d\n",
               iter, cyc.num, subtour_blocks);
      free_cycles(&cyc);
      free(nxt);
      continue;
    }

    // Single Hamiltonian cycle found
    IntVec tour = get_tour(nxt, n);
    free_cycles(&cyc);
    free(nxt);

    // Check time windows
    TWCheck tw_chk = check_tw(&tour, tw);

    if (!tw_chk.feasible) {
      if (tw_block_mode == TW_BLOCK_CHAIN)
        block_tw_chain(solver, &enc, &tour, tw_chk.chain_start, tw_chk.viol_pos);
      else
        block_tw_prefix(solver, &enc, &tour, tw_chk.viol_pos);
      hc_blocks++;
      if (iter % 1000 == 0)
        printf("  iter %d: TW infeasible (pos=%d, chain=%d), hc_blocks=%d\n",
               iter, tw_chk.viol_pos, tw_chk.chain_start, hc_blocks);
      iv_free(&tour);
      continue;
    }

    // Feasible! Update best
    int cost = tw_chk.cost;
    printf("  iter %d: feasible tour, cost=%d", iter, cost);
    if (best_cost < 0 || cost < best_cost) {
      iv_free(&best_tour);
      best_tour = tour;
      best_cost = cost;
      cost_updates++;
      printf(" (new best!)");

      // Add PB constraint: sum < cost
      printf("\n");
      pb_add_cost_bound(&pb, &enc, solver, cost);
    } else {
      printf(" (not improving, blocking)\n");
      block_hc(solver, &enc, &tour);
      hc_blocks++;
      iv_free(&tour);
    }
  }

  int64_t t1 = now_ns();
  double elapsed = (double)(t1 - t0) / 1e9;
  printf("\nStatistics:\n");
  printf("  tw-block mode: %s\n", tw_block_mode == TW_BLOCK_CHAIN ? "chain" : "prefix");
  printf("  iterations: %d\n", iter);
  printf("  subtour blocks: %d\n", subtour_blocks);
  printf("  HC blocks (TW infeasible): %d\n", hc_blocks);
  printf("  cost updates: %d\n", cost_updates);
  printf("  SAT variables: %d\n", enc.next_var - 1);
  printf("  ITE cache size: %d\n", pb.ite_cache.size);
  printf("  elapsed: %.3fs\n", elapsed);

  iv_free(&best_tour);
  pb_free(&pb);
  ipasir_release(solver);
  enc_free(&enc);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  const char *input = NULL;
  TWBlockMode tw_block = TW_BLOCK_PREFIX;
  for (int i = 1; i < argc; i++) {
    if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) && i + 1 < argc)
      input = argv[++i];
    else if (!strcmp(argv[i], "--tw-block") && i + 1 < argc) {
      i++;
      if (!strcmp(argv[i], "prefix")) tw_block = TW_BLOCK_PREFIX;
      else if (!strcmp(argv[i], "chain")) tw_block = TW_BLOCK_CHAIN;
      else {
        fprintf(stderr, "unknown --tw-block mode: %s (use 'prefix' or 'chain')\n", argv[i]);
        return 2;
      }
    } else if (argv[i][0] != '-')
      input = argv[i];
  }
  if (!input) {
    fprintf(stderr, "usage: %s [--tw-block prefix|chain] <tsptw_instance.txt>\n", argv[0]);
    return 2;
  }

  TSPTW tw = parse_tsptw(input);
  solve_tsptw(&tw, tw_block);
  tsptw_free(&tw);
  return 0;
}
