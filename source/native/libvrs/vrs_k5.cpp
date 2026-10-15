// K5 3판 (VRS_K5_query_v1.md §12–§14): query recall over the synapse of lib7. Included by vrs_lib.cpp inside its anonymous namespace (it uses
// the turn table, 𝔅, the observers, J and the reference turns of lib7 and only reads them, E3-i). Nothing here waits: the lists are insert-only
// (CAS), the units are replaced by CAS and checked against fingerprints (lib7 D11), the cells take differences by fetch_add (B-r9).

// ---------------- B-r2: cue lists ----------------
// A cue list L(h) = the claimed closed turns whose view holds h, by κ = (θₑ, ν, o0) from the largest. Two layers: the base, sorted arrays built once from
// the turns that were in 𝔅 when K5 started (one array per cue, in 64 parts by the high bits of h), and the overlay, an insert-only skip list per cue for
// the turns that closed after that. A turn is claimed by exactly one of the two (k5in 0 -> 1 base, 0 -> 2 overlay, by CAS).
// The query lists Lq(h) hold the turns w with h in Q(w), by θ₀(w) from the smallest: they find the units a new turn can change (B-r6 2).
constexpr uint32_t K5F = 64;
constexpr int K5NC = 30;
constexpr int K5L = 16;
constexpr uint32_t K5N8 = 8;

// 5판 B-t7: a synapse slot. an = a·2³² + n, ab = a′·2³² + b′. The first two lineage roots that add to it keep their count here (r = root + 1, claimed
// by CAS and never given back, so a root's shares always land in one place); any other root counts in rootcnt. lin = the roots with a positive count.
struct K5Syn { std::atomic<uint64_t> an{0}, ab{0}; std::atomic<uint32_t> r[2]{}, lin{0}; std::atomic<int32_t> c[2]{}; };
struct K5Node {
  uint64_t key; uint32_t id; uint8_t ht; uint8_t pad[3];
  std::atomic<K5Syn*> syn;            // 4판 §17 B-s2: the synapse (h, id) of a node of L(h) (kind 0), made by CAS on its first add; null for the other kinds
  std::atomic<K5Node*> nx[1];
};
K5Node* k5_node(uint64_t key, uint32_t id, int ht) {
  void* m = ::operator new(sizeof(K5Node) + sizeof(std::atomic<K5Node*>) * size_t(ht - 1));
  K5Node* n = static_cast<K5Node*>(m);
  n->key = key; n->id = id; n->ht = uint8_t(ht);
  new (&n->syn) std::atomic<K5Syn*>(nullptr);
  for (int l = 0; l < ht; l++) new (&n->nx[l]) std::atomic<K5Node*>(nullptr);
  return n;
}
struct K5Head { std::atomic<K5Node*> sent{nullptr}; std::atomic<uint32_t> n{0}; };
struct K5Part { std::vector<uint64_t> h; std::vector<uint32_t> off, id; std::unique_ptr<K5Syn[]> syn; };  // CSR: cue h[i] -> id[off[i] .. off[i+1]); syn: the synapse of each entry (L(h) only)
struct K5Base { K5Part kp[64], qp[64]; uint64_t bytes = 0; };

// ---------------- B-r3 / B-r4: units ----------------
struct K5U { uint64_t phi = 0; std::vector<uint32_t> U; };                     // U(s) sorted by turn id
// 4판 K5C (§17 B-s2): the direct items (y in D_x through Π: grade c, α | β << 1, hs = the indices into Q(x) of the cues whose P_{𝕄_x}(h) holds y,
// hs[h0 .. h0 + nh)), sorted by y; and the sums of the indirect cells (κ 15..29 with n > 0)
struct K5CE { uint32_t y; uint8_t c; uint8_t fl; uint16_t nh; uint32_t h0; };
struct K5CI { uint8_t k; uint32_t a2, b2, a, n; };
struct K5C { uint64_t phi = 0; uint32_t root = 0; std::vector<K5CE> e; std::vector<uint16_t> hs; std::vector<K5CI> ci; };
size_t k5c_bytes(const K5C& c) { return sizeof(K5C) + c.e.size() * sizeof(K5CE) + c.hs.size() * 2 + c.ci.size() * sizeof(K5CI); }
struct K5UCand { uint64_t phi = 0; std::vector<uint64_t> seeds; K5UCand* next = nullptr; };
struct K5CCand { uint64_t phi = 0; uint32_t root = 0; std::vector<uint64_t> seeds; K5C c; K5CCand* next = nullptr; };  // c: the unit with y unresolved (seeds[i] names e[i].y)

struct K5State {
  std::atomic<int> started{0}, gate{0}, base_pub{0}, ready{0};
  std::atomic<int64_t> pend{0};                 // units not installed yet (two per claimed turn) + revisit tasks in flight
  std::atomic<uint32_t> parts_done{0};
  std::vector<uint32_t> claimed;                // the base turns (written before the part tasks are pushed)
  std::atomic<K5Base*> base{nullptr};
  K5Base* building = nullptr;                   // the base while its parts are filled (each part task writes its own part)
  LFMap<K5Head> ok, oq, ns, nd;                 // overlay of L(h), overlay of Lq(h), per-session start lists ascending / descending (key = θ₀, then ord0)
  LFMap<std::atomic<uint32_t>> seedmap;         // identity of a turn (its seed) -> turn id (restored units name turns by seed)
  Cell cells[K5NC];
  std::atomic<uint64_t> ins{0}, units_built{0}, units_restored{0}, units_checked{0}, revisits{0}, rev_tasks{0}, shadow_n{0}, shadow_bad{0}, node_bytes{0}, unit_bytes{0}, list_entries{0};
  std::atomic<uint64_t> syn_bytes{0}, syn_miss{0};  // the synapse slots (base arrays and overlay nodes' cells); pairs whose slot was not found
  std::atomic<int64_t> ready_us{-1};
#ifdef VRS_K5_RACE
  uint64_t race_seed = 0;
#endif
};

constexpr uint64_t K5_IOTA = 0x4B35A1E7C0DEF00Dull;
inline uint64_t k5_iota(const TurnRec& t) { return ci_mix(t.seed ^ K5_IOTA); }

#ifdef VRS_K5_RACE
// T3-1c: at the places where two workers meet (a list link, a unit CAS, a cell add) a thread gives up its slice or spins, at places chosen by a seed
void k5_race(const Store& S, uint64_t site) {
  thread_local uint64_t st = 0;
  if (!st) st = S.k5->race_seed ^ (uint64_t(std::hash<std::thread::id>()(std::this_thread::get_id())) | 1);
  const uint64_t r = splitmix(st) ^ site;
  if ((r & 7) == 0) std::this_thread::yield();
  else if ((r & 7) == 1) { for (volatile int i = 0; i < int((r >> 8) & 1023); i = i + 1) {} }
}
#define K5_RACE(site) k5_race(S, (site))
#else
#define K5_RACE(site) ((void)0)
#endif

#ifdef VRS_K5_PROF
// KQ16 L2 input only (a separate build): CPU time per restart phase, summed over the workers
std::atomic<uint64_t> k5p_ns[8], k5p_n[8];
const char* const k5p_name[8] = {"walk", "conv_u", "phi_c", "decode_c", "apply_syn", "apply_cells", "attach", "build_c"};
struct K5Prof { int k; std::chrono::steady_clock::time_point t0; explicit K5Prof(int k_) : k(k_), t0(std::chrono::steady_clock::now()) {}
  ~K5Prof() { k5p_ns[k].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count())); k5p_n[k].fetch_add(1); } };
#define K5P(k) K5Prof k5p_##k(k)
#else
#define K5P(k) ((void)0)
#endif

TurnRec* tt_find(const Store& S, uint32_t id) {  // a turn whose chunk exists (ids above nturn or not yet made: null)
  if (!id) return nullptr;
  auto* c = S.TT.v.dir[id >> 10].load(std::memory_order_acquire);
  return c ? &c[id & 1023] : nullptr;
}
int k5_nu_cmp(const Store& S, uint32_t a, uint32_t b) {  // D-r1′: ν by path components
  if (a == b) return 0;
  return vp_path(S.sx(a).nu).compare(vp_path(S.sx(b).nu));
}
bool k5_kgt(const Store& S, uint32_t a, uint32_t b) {  // κ(a) > κ(b), κ = (θₑ, ν, o0)
  const TurnRec& x = *S.TT[a]; const TurnRec& y = *S.TT[b];
  if (x.the != y.the) return x.the > y.the;
  if (x.sg != y.sg) return k5_nu_cmp(S, x.sg, y.sg) > 0;
  return x.o0 > y.o0;
}
bool k5_addr_less(const Store& S, uint32_t sga, uint64_t oa, uint32_t sgb, uint64_t ob) {
  if (sga != sgb) return k5_nu_cmp(S, sga, sgb) < 0;
  return oa < ob;
}
bool k5_start_less(const TurnRec& a, const TurnRec& b) { return a.th0 != b.th0 ? a.th0 < b.th0 : a.ord0 < b.ord0; }  // same session: (θ₀, ord0)

// ---- skip lists. Kind 0: L(h), node a before b when κ(a) > κ(b) (key = θₑ). Kind 1: Lq(h), (θ₀, id) ascending (key = θ₀). Kind 2 / 3: per session,
// (θ₀, ord0) ascending / descending (key = θ₀). Insert-only; nothing is ever unlinked, so a predecessor found once stays a predecessor.
bool k5_before(const Store& S, int kind, const K5Node* a, uint64_t key, uint32_t id) {
  if (kind == 0) return a->key != key ? a->key > key : k5_kgt(S, a->id, id);
  if (kind == 1) return a->key != key ? a->key < key : a->id < id;
  if (kind == 2) return a->key != key ? a->key < key : S.TT[a->id]->ord0 < S.TT[id]->ord0;
  return a->key != key ? a->key > key : S.TT[a->id]->ord0 > S.TT[id]->ord0;
}
K5Node* k5_sent(K5Head& hd) {
  K5Node* s = hd.sent.load(std::memory_order_acquire);
  if (s) return s;
  K5Node* ns = k5_node(0, 0, K5L);
  if (hd.sent.compare_exchange_strong(s, ns, std::memory_order_acq_rel)) return ns;
  ::operator delete(ns);
  return s;
}
void k5_sk_insert(Store& S, LFMap<K5Head>& M, int kind, uint64_t h, uint64_t key, uint32_t id) {
  K5Head& hd = M.get(h ? h : 1)->val;  // the map takes no key 0 apart: 0 and 1 share a head (both lists stay sorted)
  K5Node* sent = k5_sent(hd);
  const uint64_t r = ci_mix(h ^ (uint64_t(id) * 0x9E3779B97F4A7C15ull) ^ uint64_t(kind));
  int ht = 1; for (uint64_t b = r; ht < K5L && (b & 3) == 0; b >>= 2) ht++;
  K5Node* nd = k5_node(key, id, ht);
  S.k5->node_bytes.fetch_add(sizeof(K5Node) + sizeof(void*) * size_t(ht - 1), std::memory_order_relaxed);
  K5Node* preds[K5L]; K5Node* succs[K5L];
  auto find = [&](int from) {
    K5Node* x = from == K5L - 1 ? sent : preds[from + 1];
    for (int l = from; l >= 0; l--) {
      K5Node* n = x->nx[l].load(std::memory_order_acquire);
      while (n && k5_before(S, kind, n, key, id)) { x = n; n = x->nx[l].load(std::memory_order_acquire); }
      preds[l] = x; succs[l] = n;
    }
  };
  find(K5L - 1);
  for (;;) {
    nd->nx[0].store(succs[0], std::memory_order_relaxed);
    K5_RACE(0x11);
    K5Node* e = succs[0];
    if (preds[0]->nx[0].compare_exchange_strong(e, nd, std::memory_order_acq_rel)) break;
    K5Node* x = preds[0];  // somebody linked here first: walk on from the same predecessor
    K5Node* n = x->nx[0].load(std::memory_order_acquire);
    while (n && k5_before(S, kind, n, key, id)) { x = n; n = x->nx[0].load(std::memory_order_acquire); }
    preds[0] = x; succs[0] = n;
  }
  hd.n.fetch_add(1, std::memory_order_seq_cst);
  for (int l = 1; l < ht; l++) {
    for (;;) {
      nd->nx[l].store(succs[l], std::memory_order_relaxed);
      K5Node* e = succs[l];
      if (preds[l]->nx[l].compare_exchange_strong(e, nd, std::memory_order_acq_rel)) break;
      K5Node* x = preds[l];
      K5Node* n = x->nx[l].load(std::memory_order_acquire);
      while (n && k5_before(S, kind, n, key, id)) { x = n; n = x->nx[l].load(std::memory_order_acquire); }
      preds[l] = x; succs[l] = n;
    }
  }
}
const K5Head* k5_head(const LFMap<K5Head>& M, uint64_t h) { const auto* n = M.find(h ? h : 1); return n ? &n->val : nullptr; }
// kind 0: the first node with θₑ < b. kind 1 / 2: the first node with key > b.
K5Node* k5_sk_from(const K5Head* hd, int kind, uint64_t b) {
  if (!hd) return nullptr;
  K5Node* x = hd->sent.load(std::memory_order_acquire);
  if (!x) return nullptr;
  for (int l = K5L - 1; l >= 0; l--) {
    K5Node* n = x->nx[l].load(std::memory_order_acquire);
    while (n && (kind == 0 ? n->key >= b : n->key <= b)) { x = n; n = x->nx[l].load(std::memory_order_acquire); }
  }
  return x->nx[0].load(std::memory_order_acquire);
}
bool k5_slice(const K5Base* B, bool q, uint64_t h, const uint32_t*& a, const uint32_t*& e, const K5Part** part = nullptr) {
  a = e = nullptr;
  if (!B) return false;
  const K5Part& p = (q ? B->qp : B->kp)[h >> 58];
  const auto it = std::lower_bound(p.h.begin(), p.h.end(), h);
  if (it == p.h.end() || *it != h) return false;
  const size_t i = size_t(it - p.h.begin());
  a = p.id.data() + p.off[i]; e = p.id.data() + p.off[i + 1];
  if (part) *part = &p;
  return true;
}
// below(h, b, lim): the turns of L(h) with θₑ < b, from the largest κ, at most lim of them (b = UINT64_MAX: top); refs: the synapse slot of each
// (a base entry's cell, or an overlay node | 1, whose cell is made on the first add; k5_ref_cell)
size_t k5_below(const Store& S, uint64_t h, uint64_t b, size_t lim, std::vector<uint32_t>& out, std::vector<uintptr_t>* cs = nullptr) {
  const K5Part* P = nullptr;
  const uint32_t *a, *e; k5_slice(S.k5->base.load(std::memory_order_acquire), false, h, a, e, &P);
  if (a) a = std::partition_point(a, e, [&](uint32_t id) { return S.TT[id]->the >= b; });
  K5Node* n = k5_sk_from(k5_head(S.k5->ok, h), 0, b);
  size_t got = 0;
  while (got < lim) {
    const bool hb = a && a < e, ho = n != nullptr;
    if (!hb && !ho) break;
    if (hb && (!ho || k5_kgt(S, *a, n->id))) { if (cs) cs->push_back(uintptr_t(&P->syn[size_t(a - P->id.data())])); out.push_back(*a++); }
    else { if (cs) cs->push_back(uintptr_t(n) | 1); out.push_back(n->id); n = n->nx[0].load(std::memory_order_acquire); }
    got++;
  }
  return got;
}
// the cell of a slot reference: a base entry's cell, or the cell of an overlay node made by CAS when make
K5Syn* k5_ref_cell(Store& S, uintptr_t r, bool make) {
  if (!(r & 1)) return reinterpret_cast<K5Syn*>(r);
  K5Node* n = reinterpret_cast<K5Node*>(r & ~uintptr_t(1));
  K5Syn* c = n->syn.load(std::memory_order_acquire);
  if (c || !make) return c;
  K5Syn* nc = new K5Syn;
  if (n->syn.compare_exchange_strong(c, nc, std::memory_order_acq_rel)) { S.k5->syn_bytes.fetch_add(sizeof(K5Syn), std::memory_order_relaxed); return nc; }
  delete nc;
  return c;
}
// 4판 §17 B-s2: the synapse slot of σ = (h, y) for a turn y of L(h). A base turn: its entry of the base array of h. An overlay turn: the cell of its
// node of L(h), made by CAS when make (the first add). Null when y is not in L(h) (never for a pair of a unit: y ∈ P(h) ⊆ L(h)).
K5Syn* k5_syn(Store& S, uint64_t h, uint32_t y, bool make) {
  const TurnRec& Y = *S.TT[y];
  if (Y.k5in.load(std::memory_order_acquire) == 1) {
    const K5Part* P = nullptr; const uint32_t *a, *e;
    if (!k5_slice(S.k5->base.load(std::memory_order_acquire), false, h, a, e, &P)) return nullptr;
    const uint32_t* at = std::partition_point(a, e, [&](uint32_t id) { return id != y && k5_kgt(S, id, y); });
    if (at == e || *at != y) return nullptr;
    return &P->syn[size_t(at - P->id.data())];
  }
  const K5Head* hd = k5_head(S.k5->ok, h);
  K5Node* x = hd ? hd->sent.load(std::memory_order_acquire) : nullptr;
  if (!x) return nullptr;
  for (int l = K5L - 1; l >= 0; l--) {
    K5Node* n = x->nx[l].load(std::memory_order_acquire);
    while (n && k5_before(S, 0, n, Y.the, y)) { x = n; n = x->nx[l].load(std::memory_order_acquire); }
  }
  K5Node* n = x->nx[0].load(std::memory_order_acquire);
  if (!n || n->id != y) return nullptr;
  return k5_ref_cell(S, uintptr_t(n) | 1, make);
}
uint64_t k5_len(const Store& S, uint64_t h) {
  const uint32_t *a, *e; k5_slice(S.k5->base.load(std::memory_order_acquire), false, h, a, e);
  const K5Head* hd = k5_head(S.k5->ok, h);
  return uint64_t(a ? e - a : 0) + (hd ? hd->n.load(std::memory_order_acquire) : 0);
}
// the turns x of L(h) with κ(x) > κ(y) and θₑ(x) < b, counted up to cap
uint32_t k5_between(const Store& S, uint64_t h, uint32_t y, uint64_t b, uint32_t cap) {
  uint32_t c = 0;
  const uint32_t *a, *e; k5_slice(S.k5->base.load(std::memory_order_acquire), false, h, a, e);
  if (a) {
    const uint32_t* lo = std::partition_point(a, e, [&](uint32_t id) { return S.TT[id]->the >= b; });
    const uint32_t* hi = std::partition_point(a, e, [&](uint32_t id) { return id != y && k5_kgt(S, id, y); });
    if (hi > lo) c += uint32_t(std::min<ptrdiff_t>(hi - lo, cap));
  }
  for (K5Node* n = k5_sk_from(k5_head(S.k5->ok, h), 0, b); n && c < cap; n = n->nx[0].load(std::memory_order_acquire)) {
    if (n->id == y || !k5_kgt(S, n->id, y)) break;
    c++;
  }
  return std::min(c, cap);
}
// the turns w of Lq(h) with θ₀(w) > b, from the smallest θ₀; fn returns false to stop
template <class Fn> void k5_qrange(const Store& S, uint64_t h, uint64_t b, Fn&& fn) {
  const uint32_t *a, *e; k5_slice(S.k5->base.load(std::memory_order_acquire), true, h, a, e);
  if (a) a = std::partition_point(a, e, [&](uint32_t id) { return S.TT[id]->th0 <= b; });
  K5Node* n = k5_sk_from(k5_head(S.k5->oq, h), 1, b);
  for (;;) {
    const bool hb = a && a < e, ho = n != nullptr;
    if (!hb && !ho) return;
    uint32_t w;
    if (hb && (!ho || std::make_pair(S.TT[*a]->th0, *a) < std::make_pair(n->key, n->id))) w = *a++;
    else { w = n->id; n = n->nx[0].load(std::memory_order_acquire); }
    if (!fn(w)) return;
  }
}

// ---------------- J with a hash set of C(x) (the same value as lib7 direct) ----------------
struct K5Set {
  std::vector<uint64_t> t; uint64_t mask = 0; bool zero = false;
  explicit K5Set(const std::vector<uint64_t>& v) {
    size_t cap = 16; while (cap < v.size() * 2 + 2) cap <<= 1;
    t.assign(cap, 0); mask = cap - 1;
    for (uint64_t x : v) {
      if (!x) { zero = true; continue; }
      size_t i = size_t(ci_mix(x)) & mask;
      while (t[i] && t[i] != x) i = (i + 1) & mask;
      t[i] = x;
    }
  }
  bool has(uint64_t x) const {
    if (!x) return zero;
    for (size_t i = size_t(ci_mix(x)) & mask;; i = (i + 1) & mask) { if (t[i] == x) return true; if (!t[i]) return false; }
  }
};
int k5_J(const Store& S, const K5Set& Cx, uint32_t y) {  // lib7 direct(S, x, y) with C(x) as a set: -1 abstain before the core, 0 A, 1 R, 2 U
  const ObsY* o = obs_of(S, y);
  if (!o->rules) return -1;
  const TurnRec& Y = *S.TT[y];
  uint32_t s = 0; const uint64_t* d = Y.D.data();
  for (uint32_t i = 0; i < Y.n; ++i) {
    const uint32_t L = Y.len[i];
    for (uint32_t k = 0; k < L; ++k) if (Cx.has(d[k])) { ++s; break; }
    d += L;
  }
  kernel::EvidenceTally t;
  t.axis_support[0] = double(s); t.axis_refute[0] = double(o->n - s);
  t.axis_source_diversity[0] = o->src; t.source_diversity = o->src; t.context_diversity = o->n; t.revision = 1;
  const auto st = kernel::judge_evidence(*o->rules, t).status();
  return st == kernel::EvidenceStatus::accept ? 0 : st == kernel::EvidenceStatus::reject ? 1 : 2;
}
// the window fingerprint of y, kept per turn while 𝔅 has not moved (two words; a torn pair fails its check: phi ^ chk names the version)
uint64_t k5_wphi(const Store& S, uint32_t y) {
  TurnRec& t = *S.TT[y];
  const uint64_t v0 = S.B.started.load(std::memory_order_seq_cst);
  const bool still = v0 == S.B.finished.load(std::memory_order_seq_cst);
  if (still) {
    const uint64_t p = t.k5wphi.load(std::memory_order_acquire), c = t.k5wchk.load(std::memory_order_acquire);
    if ((p ^ ci_mix(v0 + 0x5157)) == c) return p;
  }
  const uint64_t p = tl_pass ? win_of(S, y).phi : window_of(S, t).phi;
  if (still && S.B.started.load(std::memory_order_seq_cst) == v0 && S.B.finished.load(std::memory_order_seq_cst) == v0) {
    t.k5wphi.store(p, std::memory_order_release); t.k5wchk.store(p ^ ci_mix(v0 + 0x5157), std::memory_order_release);
  }
  return p;
}
// the lineage root of a file: its topmost ancestor (lib7 count_rank: the first piece of the lineage)
uint32_t k5_root(const Store& S, uint32_t sg) {
  uint32_t cur = sg;
  for (int d = 0; d < 17; d++) {
    const Sess& c = S.sx(cur);
    if (!c.meta_pub.load(std::memory_order_acquire) || c.fpar.empty()) break;
    const uint32_t p = find_parent(S, c.fpar);
    if (!p || p == cur) break;
    cur = p;
  }
  return S.sx(cur).root32;
}

// ---------------- B-r3: B(s) and U(s) ----------------
// N₈(s): the closed turns of the file of s in 𝕄_s (θₑ < θ₀(s)) whose start key (θ₀, ord0) is right below that of s, 8 of them. The descending start
// list is entered right below s; the walk passes only the turns that end at or after θ₀(s) (turns that overlap the start of s).
void k5_n8(const Store& S, uint32_t s, std::vector<uint32_t>& out) {
  const TurnRec& T = *S.TT[s];
  const K5Head* hd = k5_head(S.k5->nd, uint64_t(T.sg) + 1);
  K5Node* x = hd ? hd->sent.load(std::memory_order_acquire) : nullptr;
  if (!x) return;
  for (int l = K5L - 1; l >= 0; l--) {
    K5Node* n = x->nx[l].load(std::memory_order_acquire);
    while (n && !k5_start_less(*S.TT[n->id], T)) { x = n; n = x->nx[l].load(std::memory_order_acquire); }
  }
  uint32_t got = 0;
  for (K5Node* n = x->nx[0].load(std::memory_order_acquire); n && got < K5N8; n = n->nx[0].load(std::memory_order_acquire))
    if (S.TT[n->id]->the < T.th0) { out.push_back(n->id); got++; }
}
void k5_bset(const Store& S, uint32_t s, std::vector<uint32_t>& B) {
  const TurnRec& T = *S.TT[s];
  B.clear();
  for (uint64_t h : T.Q) k5_below(S, h, T.th0, K5F, B);
  k5_n8(S, s, B);
  std::sort(B.begin(), B.end()); B.erase(std::unique(B.begin(), B.end()), B.end());
}
uint64_t k5u_phi(const Store& S, const std::vector<uint32_t>& B) {
  uint64_t phi = 0;
  for (uint32_t y : B) phi += k5_iota(*S.TT[y]) + k5_wphi(S, y);
  return phi;
}
K5U* k5u_build(const Store& S, uint32_t s) {
  for (;;) {
    PassScope ps(S);
    std::vector<uint32_t> B; k5_bset(S, s, B);
    auto* u = new K5U; u->phi = k5u_phi(S, B);  // the fingerprint first: whatever changes after it shows in the check after the install
    const K5Set Cs(S.TT[s]->C);
    for (uint32_t y : B) if (k5_J(S, Cs, y) == 0) u->U.push_back(y);
    if (!ps.p.bad) return u;
    delete u;  // 𝔅 moved under a reference read: again
    S.count_retries.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---------------- B-r4: D_x, keys, the cell adds of x ----------------
struct K5Dx {
  std::vector<uint64_t> d;                 // d(h) for h in Q(x), capped at F + 1
  std::vector<uint32_t> D;                 // D_x, sorted by id
  std::vector<uint8_t> c;                  // c(x, y) = f * 3 + g for y in D (same index)
  struct Ex { uint32_t first; uint16_t second; uintptr_t ref; bool operator<(const Ex& o) const { return first != o.first ? first < o.first : second < o.second; } };
  std::vector<Ex> ex;  // 4판: the exposures (y, i), y ∈ P_{𝕄_x}(Q(x)[i]), sorted by (y, i); 5판 B-t3: ref = the slot of σ = (Q(x)[i], y), read on the walk
};
// grade of y for a query with cue set Q (sorted) and d(h): (m, f * 3 + g)
std::pair<uint32_t, uint8_t> k5_grade(const std::vector<uint64_t>& Q, const std::vector<uint64_t>& d, const std::vector<uint64_t>& K) {
  uint32_t m = 0, g = 0;
  size_t i = 0, j = 0;
  while (i < Q.size() && j < K.size()) {
    if (Q[i] < K[j]) i++;
    else if (K[j] < Q[i]) j++;
    else { m++; if (d[i] <= K5F) g++; i++; j++; }
  }
  const uint32_t q = uint32_t(Q.size());
  const uint32_t f = q ? std::min<uint32_t>(4 * m / q, 4) : 0;
  return {m, uint8_t(f * 3 + std::min<uint32_t>(g, 2))};
}
void k5_dx(const Store& S, uint32_t x, K5Dx& R, bool grades = true) {  // 5판 B-t6: grades only to make a unit (no fingerprint holds them)
  const TurnRec& X = *S.TT[x];
  R.d.assign(X.Q.size(), 0); R.D.clear(); R.c.clear(); R.ex.clear();
  std::vector<uint32_t> tmp; std::vector<uintptr_t> refs;
  for (size_t i = 0; i < X.Q.size(); i++) {
    tmp.clear(); refs.clear();
    R.d[i] = k5_below(S, X.Q[i], X.th0, K5F + 1, tmp, &refs);
    for (size_t k = 0; k < tmp.size() && k < K5F; k++) { R.D.push_back(tmp[k]); R.ex.push_back({tmp[k], uint16_t(i), refs[k]}); }
  }
  std::sort(R.D.begin(), R.D.end()); R.D.erase(std::unique(R.D.begin(), R.D.end()), R.D.end());
  std::sort(R.ex.begin(), R.ex.end());
  if (!grades) return;
  R.c.resize(R.D.size());
  for (size_t k = 0; k < R.D.size(); k++) R.c[k] = k5_grade(X.Q, R.d, S.TT[R.D[k]]->k5set).second;
}
void k5_conv_u(Store& S, uint32_t s, const struct K5Dx* pre = nullptr);
const K5U* k5_u_of(Store& S, uint32_t s) {  // the installed unit of s; made when there is none
  const K5U* u = S.TT[s]->k5u.load(std::memory_order_acquire);
  if (u) return u;
  k5_conv_u(S, s);
  return S.TT[s]->k5u.load(std::memory_order_acquire);
}
// 4판 §17 B-s5: φ_C(x) = mix(root) + Σ_i mix(Q(x)[i], d(h_i)) + Σ_{(y, i) exposed} mix(i, ι(y)) + Σ_{y ∈ D_x} (wphi(y) + mix(φ_U(y))). ind: the
// indirect keys (15 + c(x, s), z) for z ∈ U(s), s ∈ D_x
uint64_t k5c_phi(Store& S, uint32_t x, const K5Dx& R, uint32_t root, std::vector<std::pair<uint8_t, uint32_t>>* ind) {
  const TurnRec& X = *S.TT[x];
  uint64_t phi = ci_mix(uint64_t(root) ^ 0x7007);
  for (size_t i = 0; i < X.Q.size(); i++) phi += ci_mix(X.Q[i] ^ (R.d[i] * 0x9E3779B97F4A7C15ull));
  uint64_t io = 0; uint32_t yo = 0;  // ι(y) once per y (the exposures are sorted by y)
  for (const auto& e : R.ex) {
    if (e.first != yo || !io) { yo = e.first; io = k5_iota(*S.TT[yo]); }
    phi += ci_mix(io ^ ((uint64_t(e.second) + 1) * 0xC2B2AE3D27D4EB4Full));
  }
  for (size_t k = 0; k < R.D.size(); k++) {
    const uint32_t y = R.D[k];
    phi += k5_wphi(S, y);
    const K5U* u = k5_u_of(S, y);
    phi += ci_mix(u->phi ^ 0x55AA);
    if (ind) for (uint32_t z : u->U) ind->push_back({uint8_t(15 + R.c[k]), z});  // ind: R holds the grades (k5c_build)
  }
  return phi;
}
K5C* k5c_build(Store& S, uint32_t x, K5Dx* Rout = nullptr) {  // Rout: the walk the unit was made of (5판 B-t3: its slots)
  for (;;) {
    PassScope ps(S);
    K5Dx R; k5_dx(S, x, R);
    const uint32_t root = k5_root(S, S.TT[x]->sg);
    std::vector<std::pair<uint8_t, uint32_t>> ind;
    auto* c = new K5C; c->root = root; c->phi = k5c_phi(S, x, R, root, &ind);
    std::sort(ind.begin(), ind.end()); ind.erase(std::unique(ind.begin(), ind.end()), ind.end());  // a pair counts once in a cell
    const K5Set Cx(S.TT[x]->C);
    std::unordered_map<uint32_t, int> pv;  // y -> -1 not in Π (abstains before the core, or no reference turn), else α | β << 1
    auto judge = [&](uint32_t y) {
      auto it = pv.find(y);
      if (it != pv.end()) return it->second;
      int v = -1;
      if (obs_of(S, y)->rules) {
        const uint32_t r = ref_turn(S, x, y);
        if (r != UINT32_MAX) v = (k5_J(S, Cx, y) == 0 ? 1 : 0) | (direct(S, r, y) == 0 ? 2 : 0);
      }
      pv.emplace(y, v);
      return v;
    };
    size_t j = 0;
    for (size_t k = 0; k < R.D.size(); k++) {  // the direct items, by y, with hs
      const uint32_t y = R.D[k];
      const size_t j0 = j;
      while (j < R.ex.size() && R.ex[j].first == y) j++;
      const int v = judge(y);
      if (v < 0) continue;
      c->e.push_back(K5CE{y, R.c[k], uint8_t(v), uint16_t(j - j0), uint32_t(c->hs.size())});
      for (size_t t = j0; t < j; t++) c->hs.push_back(R.ex[t].second);
    }
    uint32_t sm[15][4] = {};  // the indirect cells: a′, b′, a, n
    for (const auto& [k, z] : ind) {
      const int v = judge(z);
      if (v < 0) continue;
      const bool a = (v & 1) != 0, b = (v & 2) != 0;
      uint32_t* q = sm[k - 15];
      if (a && !b) q[0]++;
      if (b && !a) q[1]++;
      if (a) q[2]++;
      q[3]++;
    }
    for (int k = 0; k < 15; k++) if (sm[k][3]) c->ci.push_back(K5CI{uint8_t(15 + k), sm[k][0], sm[k][1], sm[k][2], sm[k][3]});
    if (!ps.p.bad) { if (Rout) *Rout = std::move(R); return c; }
    delete c;
    S.count_retries.fetch_add(1, std::memory_order_relaxed);
  }
}
// a synapse takes one exposure's share (an = a·2³² + n, ab = a′·2³² + b′) and its (synapse, lineage root) count (lin1 = roots with a positive count)
uint64_t k5_synroot(const K5Syn* c, uint32_t root) {
  const uint64_t k = ci_mix((uint64_t(uintptr_t(c)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(root) + 0x51A5E0000ull));
  return k > 1 ? k : 2;
}
void k5_syn_add(Store& S, K5Syn& cl, int sign, uint8_t fl, uint32_t root) {
  const bool a = (fl & 1) != 0, b = (fl & 2) != 0;
  K5_RACE(0x44);
  cl.an.fetch_add((uint64_t)(int64_t(sign) * (a ? ((int64_t(1) << 32) + 1) : 1)));
  if (a != b) cl.ab.fetch_add((uint64_t)(int64_t(sign) * (a ? (int64_t(1) << 32) : 1)));
  std::atomic<int32_t>* cnt = nullptr;
  const uint32_t r = root + 1;
  for (int k = 0; k < 2 && !cnt; k++) {
    uint32_t v = cl.r[k].load(std::memory_order_acquire);
    if (!v && cl.r[k].compare_exchange_strong(v, r, std::memory_order_acq_rel)) v = r;  // on failure v is the root that holds the place
    if (v == r) cnt = &cl.c[k];
  }
  const int64_t prev = cnt ? int64_t(cnt->fetch_add(sign)) : S.rootcnt.get(k5_synroot(&cl, root))->val.fetch_add(sign), now = prev + sign;
  if (prev <= 0 && now > 0) cl.lin.fetch_add(1); else if (prev > 0 && now <= 0) cl.lin.fetch_sub(1);
}
PJ k5_syn_judge(const K5Syn& s) {  // the cell_judge of D-s1 (rule″) on a synapse slot
  Cell t; t.an.store(s.an.load()); t.ab.store(s.ab.load()); t.lin1.store(s.lin.load());
  return cell_judge(t);
}
void k5_syn_miss(Store& S, uint32_t x, uint32_t y) {
  S.k5->syn_miss.fetch_add(1);
  std::fprintf(stderr, "K5_SYN_MISS x=%u y=%u\n", x, y);
}
// the synapses of x: σ = (Q(x)[i], y) for each direct item and i ∈ hs. The shares of the old unit go out and those of the new go in; a synapse that has
// the same share in both (same y, α, β, i and root) is left alone. 5판 B-t3: the new unit's slots come from the walk Rn it was checked or made with.
void k5_apply_syn(Store& S, uint32_t x, const K5C* nw, const K5C* old, const K5Dx* Rn) {
  const std::vector<uint64_t>& Q = S.TT[x]->Q;
  auto one = [&](const K5C& c, const K5CE& e, uint16_t i, int sign) {
    K5Syn* cl = nullptr;
    if (Rn && &c == nw) {
      const auto it = std::lower_bound(Rn->ex.begin(), Rn->ex.end(), K5Dx::Ex{e.y, i, 0});
      if (it != Rn->ex.end() && it->first == e.y && it->second == i) cl = k5_ref_cell(S, it->ref, true);
    }
    if (!cl) cl = k5_syn(S, Q[i], e.y, true);
    if (!cl) { k5_syn_miss(S, x, e.y); return; }
    k5_syn_add(S, *cl, sign, e.fl, c.root);
  };
  auto all = [&](const K5C& c, const K5CE& e, int sign) { for (uint32_t t = 0; t < e.nh; t++) one(c, e, c.hs[e.h0 + t], sign); };
  static const K5C none;
  const K5C& A = nw ? *nw : none; const K5C& B = old ? *old : none;
  const bool same_root = nw && old && nw->root == old->root;
  size_t p = 0, q = 0;
  while (p < A.e.size() || q < B.e.size()) {
    if (q >= B.e.size() || (p < A.e.size() && A.e[p].y < B.e[q].y)) { all(A, A.e[p++], +1); continue; }
    if (p >= A.e.size() || B.e[q].y < A.e[p].y) { all(B, B.e[q++], -1); continue; }
    const K5CE& ea = A.e[p++]; const K5CE& eb = B.e[q++];
    if (!same_root || ea.fl != eb.fl) { all(A, ea, +1); all(B, eb, -1); continue; }
    uint32_t s = 0, t = 0;
    while (s < ea.nh || t < eb.nh) {
      const uint16_t ia = s < ea.nh ? A.hs[ea.h0 + s] : UINT16_MAX, ib = t < eb.nh ? B.hs[eb.h0 + t] : UINT16_MAX;
      if (t >= eb.nh || (s < ea.nh && ia < ib)) { one(A, ea, ia, +1); s++; }
      else if (s >= ea.nh || ib < ia) { one(B, eb, ib, -1); t++; }
      else { s++; t++; }
    }
  }
}
// D11 for the K5 cells: the new unit goes in for the old one; a cell takes the difference (an = a·2³² + n, ab = a′·2³² + b′) and the counts per
// (cell, lineage root) give s (lin1 = roots with a positive count). 4판: the direct cells from the direct items, the indirect cells from the sums.
void k5_apply(Store& S, uint32_t x, const K5C* nw, const K5C* old, const K5Dx* Rn) {
  { K5P(4); k5_apply_syn(S, x, nw, old, Rn); }
  K5P(5);
  int64_t da[K5NC] = {}, db[K5NC] = {}, dn[K5NC][2] = {};
  uint32_t roots[2] = {nw ? nw->root : 0, old ? old->root : 0};
  auto add = [&](const K5C* c, int sign, int ri) {
    if (!c) return;
    for (const K5CE& e : c->e) {
      const bool a = (e.fl & 1) != 0, b = (e.fl & 2) != 0;
      da[e.c] += sign * (a ? ((int64_t(1) << 32) + 1) : 1);
      if (a != b) db[e.c] += sign * (a ? (int64_t(1) << 32) : 1);
      dn[e.c][ri] += sign;
    }
    for (const K5CI& s : c->ci) {
      da[s.k] += sign * ((int64_t(s.a) << 32) + int64_t(s.n));
      db[s.k] += sign * ((int64_t(s.a2) << 32) + int64_t(s.b2));
      dn[s.k][ri] += sign * int64_t(s.n);
    }
  };
  add(nw, +1, 0); add(old, -1, 1);
  for (int k = 0; k < K5NC; k++) {
    Cell& cl = S.k5->cells[k];
    K5_RACE(0x33 + uint64_t(k));
    if (da[k]) cl.an.fetch_add((uint64_t)da[k]);
    if (db[k]) cl.ab.fetch_add((uint64_t)db[k]);
    int64_t per[2] = {dn[k][0], dn[k][1]};
    if (roots[0] == roots[1]) { per[0] += per[1]; per[1] = 0; }
    for (int ri = 0; ri < 2; ri++) {
      if (!per[ri]) continue;
      auto* nd = S.rootcnt.get(((uint64_t(0x10000 + k) << 32) | roots[ri]) + 1);
      const int64_t prev = nd->val.fetch_add(per[ri]), now = prev + per[ri];
      if (prev <= 0 && now > 0) cl.lin1.fetch_add(1); else if (prev > 0 && now <= 0) cl.lin1.fetch_sub(1);
    }
  }
}

// ---------------- persistence (B-r7): R_K5U = o0 | phi | n | n x seed; R_K5C = o0 | phi | root | n | n x (κ u8, fl u8, seed u64) ----------------
void k5_emit_u(Store& S, uint32_t s, const K5U& u) {
  const TurnRec& T = *S.TT[s]; Sess& ss = S.sx(T.sg);
  if (ss.memmode) return;
  Wr w; w.u64(T.o0); w.u64(u.phi); w.u32(u.U.size());
  for (uint32_t y : u.U) w.u64(S.TT[y]->seed);
  std::string b; frame(b, R_K5U, w.b); shard_write(S, ss, b);
}
void k5_emit_c(Store& S, uint32_t x, const K5C& c) {
  const TurnRec& T = *S.TT[x]; Sess& ss = S.sx(T.sg);
  if (ss.memmode) return;
  Wr w; w.u64(T.o0); w.u64(c.phi); w.u32(c.root); w.u32(c.e.size());
  for (const K5CE& e : c.e) {
    w.u8(e.c); w.u8(e.fl); w.u16(e.nh);
    for (uint32_t t = 0; t < e.nh; t++) w.u16(c.hs[e.h0 + t]);
    w.u64(S.TT[e.y]->seed);
  }
  w.u32(c.ci.size());
  for (const K5CI& s : c.ci) { w.u8(s.k); w.u32(s.a2); w.u32(s.b2); w.u32(s.a); w.u32(s.n); }
  std::string b; frame(b, R_K5C2, w.b); shard_write(S, ss, b);
}
void k5_attach(TurnRec& t, const std::vector<std::pair<uint8_t, std::string>>& recs) {  // materialize: the records of a restored turn become candidates
  K5P(6);
  for (const auto& [ty, pl] : recs) {
    Rd r{(const uint8_t*)pl.data(), (const uint8_t*)pl.data() + pl.size()};
    r.u64();
    if (ty == R_K5U) {
      auto* c = new K5UCand; c->phi = r.u64(); const uint64_t n = r.u32();
      if (!r.ok || n > size_t(r.e - r.p) / 8) { delete c; continue; }
      c->seeds.resize(n); for (auto& v : c->seeds) v = r.u64();
      if (!r.ok) { delete c; continue; }
      c->next = t.k5ucand.load(std::memory_order_relaxed); t.k5ucand.store(c, std::memory_order_relaxed);
    } else if (ty == R_K5C2) {  // 4판 (§17 B-s4); a 3판 R_K5C is not a 4판 unit and is never a candidate
      auto* c = new K5CCand; c->phi = r.u64(); c->root = (uint32_t)r.u32(); const uint64_t n = r.u32();
      bool ok = r.ok && n <= size_t(r.e - r.p) / 12;
      for (uint64_t k = 0; ok && k < n; k++) {
        K5CE e{0, uint8_t(r.u8()), uint8_t(r.u8()), uint16_t(r.u16()), uint32_t(c->c.hs.size())};
        if (!r.ok || e.c >= 15 || e.nh > size_t(r.e - r.p) / 2) { ok = false; break; }
        for (uint32_t t2 = 0; t2 < e.nh; t2++) c->c.hs.push_back(uint16_t(r.u16()));
        c->seeds.push_back(r.u64());
        c->c.e.push_back(e);
      }
      const uint64_t ni = ok ? r.u32() : 0;
      ok = ok && r.ok && ni <= 15;
      for (uint64_t k = 0; ok && k < ni; k++) {
        K5CI s{uint8_t(r.u8()), 0, 0, 0, 0}; s.a2 = (uint32_t)r.u32(); s.b2 = (uint32_t)r.u32(); s.a = (uint32_t)r.u32(); s.n = (uint32_t)r.u32();
        if (s.k < 15 || s.k >= K5NC) ok = false;
        c->c.ci.push_back(s);
      }
      if (!ok || !r.ok) { delete c; continue; }
      c->next = t.k5ccand.load(std::memory_order_relaxed); t.k5ccand.store(c, std::memory_order_relaxed);
    }
  }
}
uint32_t k5_by_seed(const Store& S, uint64_t seed) { const auto* n = S.k5->seedmap.find(seed ? seed : 1); return n ? n->val.load(std::memory_order_acquire) : 0; }

// ---------------- B-r6: what a change makes to be looked at again ----------------
struct K5Rev { std::vector<std::pair<uint8_t, uint32_t>> v; };  // (0 = K5U, 1 = K5C, turn)
void k5_push_rev(Store& S, std::vector<std::pair<uint8_t, uint32_t>>& v);
// the units whose lists below θ₀ hold y among the first F + 1: the w with h in Q(w) ∩ K(V(y)), θ₀(w) > θₑ(y), and at most F turns of L(h) between
void k5_dependents(const Store& S, uint32_t y, uint8_t which, std::vector<std::pair<uint8_t, uint32_t>>& out) {
  const TurnRec& Y = *S.TT[y];
  for (uint64_t h : Y.k5set)
    k5_qrange(S, h, Y.the, [&](uint32_t w) {
      if (k5_between(S, h, y, S.TT[w]->th0, K5F + 1) > K5F) return false;  // θ₀ only grows along Lq(h): the count only grows
      if (which & 1) out.push_back({0, w});
      if (which & 2) out.push_back({1, w});
      return true;
    });
}
// the turns s of the file of z whose N₈ can hold z: above z in start order, until 8 members below them are found
void k5_n8_dependents(const Store& S, uint32_t z, std::vector<std::pair<uint8_t, uint32_t>>& out) {
  const TurnRec& Z = *S.TT[z];
  const K5Head* hd = k5_head(S.k5->ns, uint64_t(Z.sg) + 1);
  K5Node* n = k5_sk_from(hd, 2, Z.th0 ? Z.th0 - 1 : 0);
  std::vector<uint64_t> seen;  // θₑ of the members passed between z and s
  for (; n; n = n->nx[0].load(std::memory_order_acquire)) {
    const TurnRec& W = *S.TT[n->id];
    if (!k5_start_less(Z, W)) continue;  // z itself, or a turn that starts with it and before it
    uint32_t below = 0; for (uint64_t e : seen) if (e < W.th0) below++;
    if (below >= K5N8) break;
    if (Z.the < W.th0) out.push_back({0, n->id});
    seen.push_back(W.the);
  }
}
// rev(y): the turns s whose U(s) held y (insert-only; a stale entry is harmless)
void k5_rev_add(Store& S, uint32_t y, uint32_t s) {
  TurnRec& t = *S.TT[y];
  for (;;) {
    DepChunk* h = t.k5rev.load(std::memory_order_seq_cst);
    if (h) { const uint32_t k = h->n.fetch_add(1, std::memory_order_seq_cst); if (k < 14) { h->id[k].store(s, std::memory_order_seq_cst); return; } }
    DepChunk* nc = new DepChunk; nc->n.store(1); nc->id[0].store(s); nc->next.store(h);
    if (t.k5rev.compare_exchange_strong(h, nc, std::memory_order_seq_cst)) return;
    delete nc;
  }
}
// the turns of 𝔅 whose window can have changed when z came in (lib7 on_turn_in: Y_z)
void k5_yz(const Store& S, uint32_t z, std::vector<uint32_t>& Yz) {
  const TurnRec& tz = *S.TT[z];
  const uint64_t p0 = S.B.rank(tz.the + 1);
  for (uint64_t p = p0, tot = S.B.total.load(std::memory_order_acquire); p < tot; p++) {
    const uint32_t y = S.B.select(p);
    if (!y) break;
    if (!(S.TT[y]->the > tz.the)) continue;
    if ((int64_t)S.B.rank(S.TT[y]->the) - (int64_t)S.B.rank(tz.the + 1) >= (int64_t)OBS_W) break;
    Yz.push_back(y);
  }
}
void k5_window_changed(const Store& S, uint32_t y, std::vector<std::pair<uint8_t, uint32_t>>& out) {
  k5_dependents(S, y, 3, out);         // B(s) / D_x hold y
  k5_n8_dependents(S, y, out);         // N₈(s) holds y
  for (const DepChunk* c = S.TT[y]->k5rev.load(std::memory_order_seq_cst); c; c = c->next.load(std::memory_order_acquire)) {  // y in U(s): the x with s in D_x
    const uint32_t n = std::min<uint32_t>(14, c->n.load(std::memory_order_seq_cst));
    for (uint32_t i = 0; i < n; i++) if (const uint32_t s = c->id[i].load(std::memory_order_seq_cst)) k5_dependents(S, s, 2, out);
  }
}

// ---------------- converge: install a unit until the unit installed is the unit of the state now (lib7 converge) ----------------
void k5_done_one(Store& S);
bool k5u_same(const K5U& a, const K5U& b) { return a.phi == b.phi && a.U == b.U; }
bool k5c_same(const K5C& a, const K5C& b) {
  if (a.phi != b.phi || a.root != b.root || a.e.size() != b.e.size() || a.hs != b.hs || a.ci.size() != b.ci.size()) return false;
  for (size_t i = 0; i < a.e.size(); i++)
    if (a.e[i].y != b.e[i].y || a.e[i].c != b.e[i].c || a.e[i].fl != b.e[i].fl || a.e[i].nh != b.e[i].nh || a.e[i].h0 != b.e[i].h0) return false;
  for (size_t i = 0; i < a.ci.size(); i++)
    if (a.ci[i].k != b.ci[i].k || a.ci[i].a2 != b.ci[i].a2 || a.ci[i].b2 != b.ci[i].b2 || a.ci[i].a != b.ci[i].a || a.ci[i].n != b.ci[i].n) return false;
  return true;
}
void k5_shadow_fail(Store& S, const char* what, uint32_t t) {
  S.k5->shadow_bad.fetch_add(1);
  std::fprintf(stderr, "K5_SHADOW_FAIL %s turn=%u o0=%llu\n", what, t, (unsigned long long)S.TT[t]->o0);
}
// 5판: pre = the walk of s made for both of its units (B-t1: B(s) = D_s ∪ N₈(s)); used for the first look only
void k5_conv_u(Store& S, uint32_t s, const K5Dx* pre) {
  EbrGuard guard(S);
  TurnRec& t = *S.TT[s];
  for (;;) {
    K5U* c = t.k5u.load(std::memory_order_acquire);
    uint64_t now = 0;
    {
      PassScope ps(S); std::vector<uint32_t> B;
      if (pre) { B = pre->D; k5_n8(S, s, B); std::sort(B.begin(), B.end()); B.erase(std::unique(B.begin(), B.end()), B.end()); pre = nullptr; }
      else k5_bset(S, s, B);
      now = k5u_phi(S, B);
    }
    S.k5->units_checked.fetch_add(1, std::memory_order_relaxed);
    if (c && c->phi == now) return;
    K5U* nw = nullptr; bool restored = false;
    if (!c) if (K5UCand* cl = t.k5ucand.exchange(nullptr, std::memory_order_acq_rel)) {  // restored units: the one made over the state now, if any
      for (K5UCand* q = cl; q && !nw; q = q->next) {
        if (q->phi != now) continue;
        auto* u = new K5U; u->phi = q->phi; bool ok = true;
        for (uint64_t sd : q->seeds) { const uint32_t y = k5_by_seed(S, sd); if (!y) { ok = false; break; } u->U.push_back(y); }
        if (ok) { std::sort(u->U.begin(), u->U.end()); nw = u; restored = true; } else delete u;
      }
      for (K5UCand* q = cl; q;) { K5UCand* nx = q->next; delete q; q = nx; }
    }
    if (!nw) nw = k5u_build(S, s);
    K5_RACE(0x22);
    K5U* ex = c;
    if (t.k5u.compare_exchange_strong(ex, nw, std::memory_order_acq_rel)) {
      for (uint32_t y : nw->U) k5_rev_add(S, y, s);
      S.k5->unit_bytes.fetch_add(sizeof(K5U) + nw->U.size() * 4, std::memory_order_relaxed);
      if (restored) S.k5->units_restored.fetch_add(1, std::memory_order_relaxed);
      else { S.k5->units_built.fetch_add(1, std::memory_order_relaxed); k5_emit_u(S, s, *nw); }
#ifdef VRS_K5_RACE
      { K5U* sh = k5u_build(S, s); S.k5->shadow_n.fetch_add(1); if (sh->phi == nw->phi && !k5u_same(*sh, *nw)) k5_shadow_fail(S, "K5U", s); delete sh; }
#endif
      if (c) {
        S.k5->unit_bytes.fetch_sub(sizeof(K5U) + c->U.size() * 4, std::memory_order_relaxed);
        retire_obj(S, c, 5, sizeof(K5U) + c->U.size() * 4);
        std::vector<std::pair<uint8_t, uint32_t>> dep; k5_dependents(S, s, 2, dep);  // the units of x with s in D_x hold Φ_U(s)
        if (!dep.empty()) k5_push_rev(S, dep);
      } else k5_done_one(S);
      return;  // 5판 B-t2: a change after the fingerprint has its own look again (B-r6 2)
    }
    delete nw;
  }
}
void k5_conv_c(Store& S, uint32_t x, const K5Dx* pre = nullptr) {  // 5판: pre as in k5_conv_u
  EbrGuard guard(S);
  TurnRec& t = *S.TT[x];
  for (;;) {
    K5C* c = t.k5c.load(std::memory_order_acquire);
    uint64_t now = 0; K5Dx Rown; uint32_t root = 0;
    const K5Dx* Ru = pre ? pre : &Rown; pre = nullptr;
    { K5P(2); PassScope ps(S); if (Ru == &Rown) k5_dx(S, x, Rown, false); root = k5_root(S, t.sg); now = k5c_phi(S, x, *Ru, root, nullptr); }
    S.k5->units_checked.fetch_add(1, std::memory_order_relaxed);
    if (c && c->phi == now) return;
    K5C* nw = nullptr; bool restored = false;
    if (!c) if (K5CCand* cl = t.k5ccand.exchange(nullptr, std::memory_order_acq_rel)) {
      K5P(3);
      for (K5CCand* q = cl; q && !nw; q = q->next) {
        if (q->phi != now || q->root != root) continue;
        std::vector<std::pair<uint32_t, size_t>> ys; bool ok = true;  // the items by the turn ids of this run (ids are not kept across runs)
        for (size_t i = 0; i < q->seeds.size(); i++) {
          const uint32_t y = k5_by_seed(S, q->seeds[i]);
          if (!y) { ok = false; break; }
          ys.push_back({y, i});
        }
        for (uint16_t i : q->c.hs) if (i >= t.Q.size()) ok = false;
        if (ok) {
          std::sort(ys.begin(), ys.end());
          auto* u = new K5C; u->phi = q->phi; u->root = q->root; u->ci = q->c.ci;
          for (const auto& [y, i] : ys) {
            K5CE e = q->c.e[i]; const uint32_t h0 = e.h0;
            e.y = y; e.h0 = uint32_t(u->hs.size());
            u->hs.insert(u->hs.end(), q->c.hs.begin() + h0, q->c.hs.begin() + h0 + e.nh);
            u->e.push_back(e);
          }
          nw = u; restored = true;
        }
      }
      for (K5CCand* q = cl; q;) { K5CCand* nx = q->next; delete q; q = nx; }
    }
    K5Dx Rb;
    if (!nw) { K5P(7); nw = k5c_build(S, x, &Rb); Ru = &Rb; }
    K5_RACE(0x23);
    K5C* ex = c;
    if (t.k5c.compare_exchange_strong(ex, nw, std::memory_order_acq_rel)) {
      k5_apply(S, x, nw, c, Ru);
      S.k5->unit_bytes.fetch_add(k5c_bytes(*nw), std::memory_order_relaxed);
      if (restored) S.k5->units_restored.fetch_add(1, std::memory_order_relaxed);
      else { S.k5->units_built.fetch_add(1, std::memory_order_relaxed); k5_emit_c(S, x, *nw); }
#ifdef VRS_K5_RACE
      { K5C* sh = k5c_build(S, x); S.k5->shadow_n.fetch_add(1); if (sh->phi == nw->phi && !k5c_same(*sh, *nw)) k5_shadow_fail(S, "K5C", x); delete sh; }
#endif
      if (c) {
        S.k5->unit_bytes.fetch_sub(k5c_bytes(*c), std::memory_order_relaxed);
        retire_obj(S, c, 6, k5c_bytes(*c));
      } else k5_done_one(S);
      return;
    }
    delete nw;
  }
}

// ---------------- tasks ----------------
struct K5Task { uint8_t kind = 0; uint32_t a = 0, e = 0; std::vector<std::pair<uint8_t, uint32_t>> v; };  // 0 base claim, 1 base part a, 2 units of claimed[a, e), 3 revisits v
void k5_run(Store& S, K5Task* t);
void k5_push(Store& S, K5Task* t) { if (!S.work.push(task_of(T_K5, (uint64_t)(uintptr_t)t))) k5_run(S, t); }
void k5_push_rev(Store& S, std::vector<std::pair<uint8_t, uint32_t>>& v) {
  std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
  for (size_t i = 0; i < v.size(); i += 32) {
    auto* t = new K5Task; t->kind = 3; t->v.assign(v.begin() + i, v.begin() + std::min(v.size(), i + 32));
    S.k5->pend.fetch_add(1, std::memory_order_seq_cst);
    S.k5->rev_tasks.fetch_add(1, std::memory_order_relaxed);
    k5_push(S, t);
  }
}
void k5_maybe_ready(Store& S) {
  if (S.k5->base_pub.load(std::memory_order_seq_cst) && S.k5->pend.load(std::memory_order_seq_cst) <= 0 && S.ready.load(std::memory_order_seq_cst) == 1) {
    int64_t m = -1;
    S.k5->ready_us.compare_exchange_strong(m, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - S.t_open).count());
    int z = 0;
    if (S.k5->ready.compare_exchange_strong(z, 1, std::memory_order_seq_cst)) bump_progress(S);
  }
}
void k5_done_one(Store& S) { if (S.k5->pend.fetch_sub(1, std::memory_order_seq_cst) <= 1) k5_maybe_ready(S); }
// a turn becomes a K5 memory: its cue set, its identity, its place in the lists (the overlay, or the base the claim task builds)
void k5_prepare(Store& S, TurnRec& t, uint32_t id) {
  t.k5set = t.D; std::sort(t.k5set.begin(), t.k5set.end()); t.k5set.erase(std::unique(t.k5set.begin(), t.k5set.end()), t.k5set.end());
  S.k5->seedmap.get(t.seed ? t.seed : 1)->val.store(id, std::memory_order_release);
}
void k5_overlay(Store& S, uint32_t z) {
  TurnRec& t = *S.TT[z];
  for (uint64_t h : t.k5set) k5_sk_insert(S, S.k5->ok, 0, h, t.the, z);
  for (uint64_t h : t.Q) k5_sk_insert(S, S.k5->oq, 1, h, t.th0, z);
  S.k5->list_entries.fetch_add(t.k5set.size() + t.Q.size(), std::memory_order_relaxed);
  S.k5->ins.fetch_add(1, std::memory_order_seq_cst);
}
void k5_schedule_new(Store& S, uint32_t z) {  // B-r6 for a turn that came in after the base: its own units, and every unit it can change
  std::vector<std::pair<uint8_t, uint32_t>> v{{0, z}, {1, z}};
  k5_dependents(S, z, 3, v);
  k5_n8_dependents(S, z, v);
  std::vector<uint32_t> Yz; k5_yz(S, z, Yz);
  for (uint32_t y : Yz) k5_window_changed(S, y, v);
  S.k5->revisits.fetch_add(v.size(), std::memory_order_relaxed);
  k5_push_rev(S, v);
}
// lib7 hook: the turn z is in 𝔅 (finalize_turn after the insert into 𝔅, or the restore)
void k5_turn_in(Store& S, uint32_t z) {
  TurnRec& t = *S.TT[z];
  t.k5b.store(1, std::memory_order_seq_cst);
  if (!S.k5->gate.load(std::memory_order_seq_cst)) return;  // the claim task will see it (it reads the turns after it opens the gate)
  uint8_t e = 0;
  if (!t.k5in.compare_exchange_strong(e, 2, std::memory_order_seq_cst)) return;
  k5_prepare(S, t, z);
  k5_sk_insert(S, S.k5->ns, 2, uint64_t(t.sg) + 1, t.th0, z);
  k5_sk_insert(S, S.k5->nd, 3, uint64_t(t.sg) + 1, t.th0, z);
  k5_overlay(S, z);
  S.k5->pend.fetch_add(2, std::memory_order_seq_cst);
  t.k5in.store(3, std::memory_order_seq_cst);  // in the lists: from here on the publisher may schedule it too
  if (S.k5->base_pub.load(std::memory_order_seq_cst)) k5_schedule_new(S, z);  // otherwise the publisher schedules it
}
// lib7 hook: after the load, once
void k5_start(Store& S) {
  int z = 0;
  if (!S.k5->started.compare_exchange_strong(z, 1, std::memory_order_seq_cst)) return;
  auto* t = new K5Task; t->kind = 0;
  k5_push(S, t);
}
void k5_part(Store& S, uint32_t p) {
  const auto& C = S.k5->claimed;
  std::vector<std::pair<uint64_t, uint32_t>> ek, eq;
  for (uint32_t id : C) {
    const TurnRec& t = *S.TT[id];
    auto range = [&](const std::vector<uint64_t>& v, std::vector<std::pair<uint64_t, uint32_t>>& out) {
      const auto lo = std::lower_bound(v.begin(), v.end(), uint64_t(p) << 58);
      const auto hi = p == 63 ? v.end() : std::lower_bound(v.begin(), v.end(), uint64_t(p + 1) << 58);
      for (auto it = lo; it != hi; ++it) out.push_back({*it, id});
    };
    range(t.k5set, ek); range(t.Q, eq);
  }
  std::sort(ek.begin(), ek.end(), [&](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : k5_kgt(S, a.second, b.second); });
  std::sort(eq.begin(), eq.end(), [&](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    const uint64_t x = S.TT[a.second]->th0, y = S.TT[b.second]->th0;
    return x != y ? x < y : a.second < b.second;
  });
  K5Base* pending = S.k5->building;  // not published yet: the part task is the only writer of its part
  auto fill = [](K5Part& P, const std::vector<std::pair<uint64_t, uint32_t>>& e) {
    P.id.reserve(e.size());
    for (size_t i = 0; i < e.size(); i++) {
      if (i == 0 || e[i].first != e[i - 1].first) { P.h.push_back(e[i].first); P.off.push_back(uint32_t(i)); }
      P.id.push_back(e[i].second);
    }
    P.off.push_back(uint32_t(e.size()));
  };
  fill(pending->kp[p], ek); fill(pending->qp[p], eq);
  pending->kp[p].syn.reset(new K5Syn[ek.size() ? ek.size() : 1]);  // 4판: a synapse slot per entry of L(h)
  S.k5->syn_bytes.fetch_add(ek.size() * sizeof(K5Syn), std::memory_order_relaxed);
  S.k5->list_entries.fetch_add(ek.size() + eq.size(), std::memory_order_relaxed);
  if (S.k5->parts_done.fetch_add(1, std::memory_order_seq_cst) + 1 == 64) {  // the last part publishes the base, then every claimed turn is counted
    uint64_t bytes = 0;
    for (int i = 0; i < 64; i++) for (const K5Part* P : {&pending->kp[i], &pending->qp[i]}) bytes += P->id.size() * 4 + P->h.size() * 8 + P->off.size() * 4;
    pending->bytes = bytes;
    S.k5->base.store(pending, std::memory_order_seq_cst);
    S.k5->base_pub.store(1, std::memory_order_seq_cst);
    for (uint32_t a = 0; a < C.size(); a += 8) { auto* t = new K5Task; t->kind = 2; t->a = a; t->e = std::min<uint32_t>(uint32_t(C.size()), a + 8); k5_push(S, t); }
    for (uint32_t id = 1, n = S.nturn.load(std::memory_order_seq_cst); id <= n; id++) {  // the turns that went to the overlay before the base was out
      TurnRec* t = tt_find(S, id);
      if (t && t->k5in.load(std::memory_order_seq_cst) == 3) k5_schedule_new(S, id);
    }
    k5_maybe_ready(S);
  }
}
void k5_claim(Store& S) {
  S.k5->gate.store(1, std::memory_order_seq_cst);
  auto& C = S.k5->claimed;
  for (uint32_t id = 1, n = S.nturn.load(std::memory_order_seq_cst); id <= n; id++) {
    TurnRec* t = tt_find(S, id);
    if (!t || !t->k5b.load(std::memory_order_seq_cst)) continue;  // not in 𝔅 yet: its hook sees the gate open
    uint8_t e = 0;
    if (!t->k5in.compare_exchange_strong(e, 1, std::memory_order_seq_cst)) continue;
    k5_prepare(S, *t, id);
    C.push_back(id);
  }
  for (uint32_t id : C) { k5_sk_insert(S, S.k5->ns, 2, uint64_t(S.TT[id]->sg) + 1, S.TT[id]->th0, id); k5_sk_insert(S, S.k5->nd, 3, uint64_t(S.TT[id]->sg) + 1, S.TT[id]->th0, id); }
  S.k5->pend.fetch_add(2 * int64_t(C.size()), std::memory_order_seq_cst);
  S.k5->building = new K5Base;
  for (uint32_t p = 0; p < 64; p++) { auto* t = new K5Task; t->kind = 1; t->a = p; k5_push(S, t); }
}
void k5_run(Store& S, K5Task* t) {
  switch (t->kind) {
    case 0: k5_claim(S); break;
    case 1: k5_part(S, t->a); break;
    case 2:  // 5판 B-t1: one walk per turn for both units
      for (uint32_t i = t->a; i < t->e; i++) {
        const uint32_t id = S.k5->claimed[i];
        K5Dx R; { K5P(0); PassScope ps(S); k5_dx(S, id, R, false); }
        { K5P(1); k5_conv_u(S, id, &R); }
        k5_conv_c(S, id, &R);
      }
      break;
    case 3:
      for (const auto& [k, id] : t->v) { if (k == 0) k5_conv_u(S, id); else k5_conv_c(S, id); }
      k5_done_one(S);
      break;
  }
  delete t;
}

// ---------------- B-r5: a query ----------------
// A member of 𝕄(t): a closed turn (id) or an open turn of W (OPEN | index into the query's open turns).
constexpr uint32_t K5OPEN = 0x80000000u;
struct K5Open {  // D-r1 (b): the request side of the open turn of a file of W, from the lines on disk
  uint32_t sg = 0, last_item = 0; uint64_t o0 = 0, th0 = 0, the = 0, o1 = 0;
  std::vector<uint64_t> K; std::vector<uint32_t> req;
};
bool k5_open_of(Store& S, Sess& ss, K5Open& o) {
  const PubView v = pub_view(S, ss);
  if (!v.open) return false;
  const TurnSlot* sl = ss.slots.find(v.sa);
  if (!sl) return false;
  TurnAgg g; g.o0 = sl->o0.load(); g.th0 = sl->th0.load(); g.ord0 = sl->ord0.load(); g.pos0 = sl->pos0.load(); g.ordl = g.ord0;
  for (uint32_t id = sl->lst.load(std::memory_order_seq_cst); id; id = S.lr[id].nextl) {
    const LineRec& L = S.lr[id];
    if (L.off >= g.o0 && L.off < v.P) agg_add(S, g, L, L.td.load(std::memory_order_acquire));
  }
  TurnParse p; agg_parse(g, p);
  const std::string uq = clip_chars(req_text(p), 1500);
  std::unordered_set<uint64_t> seen;
  for (size_t b = 0;;) {
    const size_t e = uq.find('\n', b);
    for (uint64_t h : cue_set(uq.substr(b, e == std::string::npos ? std::string::npos : e - b))) if (seen.insert(h).second) o.K.push_back(h);
    if (e == std::string::npos) break;
    b = e + 1;
  }
  std::sort(o.K.begin(), o.K.end());
  o.sg = ss.sg; o.o0 = g.o0; o.th0 = g.th0; o.the = v.lastth; o.o1 = v.P; o.req = p.req;
  o.last_item = v.ia > g.pos0 ? ss.sess[v.ia - 1] : 0;
  return true;
}
struct K5Q {  // Row::sh: σ*(y) as the index of its cue in Q(q), -1 none
  struct Row { uint32_t y; double w; int sh; uint32_t m; int pq = 2, pe = 2, re = 0; uint32_t sq = 0, n = 0; double tau = 0; std::vector<uint32_t> hits; };
  std::vector<uint64_t> Q, d; std::vector<std::string> qtext;  // cues, d(h), the text each cue was made of
  uint64_t reads = 0; uint32_t rare = 0, D = 0;
  std::vector<K5Open> open;
  std::vector<Row> R; size_t rho = 0; uint32_t re = 0;
};
uint64_t k5m_the(const Store& S, const K5Q& q, uint32_t m) { return m & K5OPEN ? q.open[m & ~K5OPEN].the : S.TT[m]->the; }
uint32_t k5m_sg(const Store& S, const K5Q& q, uint32_t m) { return m & K5OPEN ? q.open[m & ~K5OPEN].sg : S.TT[m]->sg; }
uint64_t k5m_o0(const Store& S, const K5Q& q, uint32_t m) { return m & K5OPEN ? q.open[m & ~K5OPEN].o0 : S.TT[m]->o0; }
const std::vector<uint64_t>& k5m_K(const Store& S, const K5Q& q, uint32_t m) { return m & K5OPEN ? q.open[m & ~K5OPEN].K : S.TT[m]->k5set; }
bool k5m_kgt(const Store& S, const K5Q& q, uint32_t a, uint32_t b) {
  const uint64_t ta = k5m_the(S, q, a), tb = k5m_the(S, q, b);
  if (ta != tb) return ta > tb;
  const uint32_t sa = k5m_sg(S, q, a), sb = k5m_sg(S, q, b);
  if (sa != sb) return k5_nu_cmp(S, sa, sb) > 0;
  return k5m_o0(S, q, a) > k5m_o0(S, q, b);
}
// the cue texts of a query: tokenize with the strings it hashes (the same emits as tokenize)
void k5_cue_texts(const std::string& s, std::vector<std::pair<uint64_t, std::string>>& out) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data()); const size_t n = s.size();
  std::vector<uint32_t> run; std::string w, t;
  auto flush_run = [&] {
    if (run.size() == 1) { t.clear(); put_utf8(run[0], t); out.push_back({tok_hash('K', (const uint8_t*)t.data(), t.size()), t}); }
    for (size_t i = 0; i + 1 < run.size(); ++i) { t.clear(); put_utf8(run[i], t); put_utf8(run[i + 1], t); out.push_back({tok_hash('K', (const uint8_t*)t.data(), t.size()), t}); }
    run.clear();
  };
  size_t b64_end = 0;
  for (size_t i = 0; i < n;) {
    const uint8_t c = p[i];
    if (c == '\\' && i + 1 < n) {
      if (p[i + 1] == 'u' && i + 5 < n) {
        int v = 0; bool ok = true;
        for (int k = 2; k < 6; ++k) { const int h = hexv(p[i + k]); if (h < 0) { ok = false; break; } v = v * 16 + h; }
        if (ok) { i += 6; if (cjk(uint32_t(v))) run.push_back(uint32_t(v)); else flush_run(); continue; }
      }
      flush_run(); i += 2; continue;
    }
    if (c < 0x80) {
      flush_run();
      const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      if (!word) { ++i; continue; }
      if (i >= b64_end) { size_t j = i; while (j < n && b64c(p[j])) ++j; if (j - i >= 256) { i = j; continue; } b64_end = j; }
      size_t j = i; w.clear();
      while (j < n && ((p[j] >= 'a' && p[j] <= 'z') || (p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= '0' && p[j] <= '9') || p[j] == '_')) { w += char(p[j] >= 'A' && p[j] <= 'Z' ? p[j] + 32 : p[j]); ++j; }
      if (w.size() >= 2 && w.size() <= 40) out.push_back({tok_hash('W', (const uint8_t*)w.data(), w.size()), w});
      i = j; continue;
    }
    uint32_t cp = 0; size_t len = 1;
    if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 31u) << 6) | (p[i + 1] & 63u); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 15u) << 12) | ((p[i + 1] & 63u) << 6) | (p[i + 2] & 63u); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 7u) << 18) | ((p[i + 1] & 63u) << 12) | ((p[i + 2] & 63u) << 6) | (p[i + 3] & 63u); len = 4; }
    if (cjk(cp)) run.push_back(cp); else flush_run();
    i += len;
  }
  flush_run();
}
// D-r5: J_q(q, y) over the observer of y (its window and its 99 draws), with the reference turns measured by their request cues
int k5_Jq(const Store& S, const std::vector<uint64_t>& Q, uint32_t y, std::vector<uint32_t>& hit_lines, uint32_t& sq, uint32_t& nn, double& tau_out) {
  const ObsY* o = obs_of(S, y);
  if (!o->rules) return 2;  // abstains before the core (D-r3): U
  const TurnRec& Y = *S.TT[y];
  const Win w = win_of(S, y);
  auto hits = [&](const std::vector<uint64_t>& cues, std::vector<uint32_t>* at) {
    uint32_t s = 0; const uint64_t* d = Y.D.data();
    for (uint32_t i = 0; i < Y.n; ++i) {
      const uint32_t L = Y.len[i];
      for (uint32_t k = 0; k < L; ++k) if (std::binary_search(cues.begin(), cues.end(), d[k])) { ++s; if (at) at->push_back(i); break; }
      d += L;
    }
    return s;
  };
  std::vector<uint32_t> Sv;
  for (uint32_t r : o->R) { const uint32_t t = S.B[w.lo + r]; if (!t) return 2; Sv.push_back(hits(S.TT[t]->Q, nullptr)); }
  if (Sv.size() != OBS_M) return 2;
  const uint32_t mx = *std::max_element(Sv.begin(), Sv.end());
  std::nth_element(Sv.begin(), Sv.begin() + 49, Sv.end());
  const uint32_t med = Sv[49], n = Y.n;
  const double z = obs_z(), z2 = z * z;
  const auto U = [&](uint32_t m) { return kernel::wilson_interval(double(m), double(n - m), z, z2).upper; };
  const double Lmax = kernel::wilson_interval(double(mx), double(n - mx), z, z2).lower;
  uint32_t lo = med, hi = med <= n - 1 ? n - 1 : n;
  while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (Lmax <= U(mid)) hi = mid; else lo = mid + 1; }
  if (!(Lmax <= U(lo))) return 2;  // lemma E: no rule
  const double tau = U(lo);
  EvidencePolicy pol; pol.confidence_level = OBS_CONF; pol.axis_count = 1;
  if (tau < 1) { pol.chance_rate = tau; pol.accept_margin = 0; } else { pol.chance_rate = 0; pol.accept_margin = 1; }
  const auto rules = make_evidence_rules(pol);
  hit_lines.clear();
  const uint32_t s = hits(Q, &hit_lines);
  sq = s; nn = n; tau_out = tau;
  kernel::EvidenceTally t;
  t.axis_support[0] = double(s); t.axis_refute[0] = double(n - s);
  t.axis_source_diversity[0] = o->src; t.source_diversity = o->src; t.context_diversity = n; t.revision = 1;
  const auto st = kernel::judge_evidence(rules, t).status();
  return st == kernel::EvidenceStatus::accept ? 0 : st == kernel::EvidenceStatus::reject ? 1 : 2;
}
void k5_query(Store& S, const std::string& text, const std::vector<std::string>& W, K5Q& q) {
  const std::string qc = clip_chars(text, 1500);
  q.Q = cue_set(qc);
  {
    std::vector<std::pair<uint64_t, std::string>> ct; k5_cue_texts(qc, ct);
    q.qtext.assign(q.Q.size(), std::string());
    for (const auto& [h, s] : ct) { const auto it = std::lower_bound(q.Q.begin(), q.Q.end(), h); if (it != q.Q.end() && *it == h && q.qtext[it - q.Q.begin()].empty()) q.qtext[it - q.Q.begin()] = s; }
  }
  for (const std::string& p : W) {  // 3: the open turns of W
    Sess* ss = get_sess(S, canon_path(S, p.c_str()), false);
    if (!ss || ss->memmode) continue;
    K5Open o;
    if (k5_open_of(S, *ss, o)) {
      bool dup = false; for (const auto& x : q.open) if (x.sg == o.sg && x.o0 == o.o0) dup = true;
      if (!dup) q.open.push_back(std::move(o));
    }
  }
  // 4–5: Déjà vu P_t(h), d_t(h); D_t(q)
  q.d.assign(q.Q.size(), 0);
  std::vector<uint32_t> Dm, tmp, oh;
  std::vector<uintptr_t> cs;
  std::unordered_map<uint32_t, std::pair<double, int>> best;  // 4판 D-r7′: y -> (the largest w_σ over the approved σ = (h, y) read, the index of its h)
  for (size_t i = 0; i < q.Q.size(); i++) {
    const uint64_t h = q.Q[i];
    tmp.clear(); cs.clear(); k5_below(S, h, UINT64_MAX, K5F, tmp, &cs);
    oh.clear();
    for (size_t j = 0; j < q.open.size(); j++) if (std::binary_search(q.open[j].K.begin(), q.open[j].K.end(), h)) oh.push_back(K5OPEN | uint32_t(j));
    std::sort(oh.begin(), oh.end(), [&](uint32_t a, uint32_t b) { return k5m_kgt(S, q, a, b); });
    size_t a = 0, b = 0, got = 0;
    while (got < K5F && (a < tmp.size() || b < oh.size())) {
      if (a < tmp.size() && (b >= oh.size() || k5m_kgt(S, q, tmp[a], oh[b]))) {
        const uint32_t y = tmp[a]; const K5Syn* cl = k5_ref_cell(S, cs[a++], false);
        Dm.push_back(y);
        if (cl) {  // B-s6: the synapse (h, y) of this element; Q is ascending, so a later h wins only with a larger w
          const PJ p = k5_syn_judge(*cl);
          if (p.st == 0) { auto it = best.find(y); if (it == best.end()) best.emplace(y, std::make_pair(p.lower, int(i))); else if (p.lower > it->second.first) it->second = {p.lower, int(i)}; }
        }
      } else Dm.push_back(oh[b++]);
      got++;
    }
    q.reads += got;
    q.d[i] = std::min<uint64_t>(k5_len(S, h) + oh.size(), K5F + 1);
    if (q.d[i] <= K5F) q.rare++;
  }
  std::sort(Dm.begin(), Dm.end()); Dm.erase(std::unique(Dm.begin(), Dm.end()), Dm.end());
  q.D = uint32_t(Dm.size());
  // 5–6: grades, direct keys, the keys through the edges U(s)
  std::unordered_map<uint32_t, std::vector<uint8_t>> keys;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint8_t>> grade;
  for (uint32_t y : Dm) { const auto g = k5_grade(q.Q, q.d, k5m_K(S, q, y)); grade[y] = g; keys[y].push_back(g.second); }
  for (uint32_t s : Dm) {
    if (s & K5OPEN) continue;  // an open turn has no answer: U = ∅
    const K5U* u = k5_u_of(S, s);
    for (uint32_t z : u->U) keys[z].push_back(uint8_t(15 + grade[s].second));
  }
  // 7 (4판 D-r7′): R(q) by the edge gate P((간, c)); strength w = the synapse σ*; the cells only for the gate and φ_e
  PJ pj[K5NC];
  for (int k = 0; k < K5NC; k++) pj[k] = cell_judge(S.k5->cells[k]);
  for (auto& [y, ks] : keys) {
    std::sort(ks.begin(), ks.end()); ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
    bool any_ind = false;
    for (uint8_t k : ks) if (k >= 15 && pj[k].st == 0) any_ind = true;
    const bool inD = std::binary_search(Dm.begin(), Dm.end(), y);
    if (!inD && !any_ind) continue;
    const std::pair<uint32_t, uint8_t> g = inD ? grade[y] : k5_grade(q.Q, q.d, k5m_K(S, q, y));
    K5Q::Row r; r.y = y; r.w = 0; r.sh = -1; r.m = g.first;
    if (const auto it = best.find(y); it != best.end()) { r.w = it->second.first; r.sh = it->second.second; }
    r.pe = r.sh >= 0 ? 0 : pj[g.second].st;  // D-r8′: P(σ*) = A, else P((직, c(q, y)))
    q.R.push_back(std::move(r));
  }
  // 8: the order of Recall, ρ
  std::sort(q.R.begin(), q.R.end(), [&](const K5Q::Row& a, const K5Q::Row& b) {
    if (a.w != b.w) return a.w > b.w;
    if (a.m != b.m) return a.m > b.m;
    const uint64_t ta = k5m_the(S, q, a.y), tb = k5m_the(S, q, b.y);
    if (ta != tb) return ta > tb;
    return k5_addr_less(S, k5m_sg(S, q, a.y), k5m_o0(S, q, a.y), k5m_sg(S, q, b.y), k5m_o0(S, q, b.y));
  });
  size_t ts = 0; while (ts < q.R.size() && q.R[ts].w == q.R[0].w) ts++;
  q.rho = std::min<size_t>(ts, 5);
  // 9: the phases of ρ, re-evidence
  for (size_t i = 0; i < q.rho; i++) {
    K5Q::Row& r = q.R[i];
    r.pq = (r.y & K5OPEN) ? 2 : k5_Jq(S, q.Q, r.y, r.hits, r.sq, r.n, r.tau);
    r.re = r.pq != r.pe ? 1 : 0;
    q.re += uint32_t(r.re);
  }
}

// ---------------- output ----------------
std::string k5_repr(double v) {  // Python repr(float): the shortest round trip, fixed for -4 < decpt <= 16
  if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
  std::string s(buf, res.ptr);
  std::string sign; if (s[0] == '-') { sign = "-"; s = s.substr(1); }
  const size_t e = s.find('e');
  std::string dig; for (size_t i = 0; i < e; i++) if (s[i] != '.') dig += s[i];
  const int ex = std::atoi(s.c_str() + e + 1), decpt = ex + 1, n = int(dig.size());
  std::string o;
  if (decpt > -4 && decpt <= 16) {
    if (decpt <= 0) o = "0." + std::string(size_t(-decpt), '0') + dig;
    else if (decpt >= n) o = dig + std::string(size_t(decpt - n), '0') + ".0";
    else o = dig.substr(0, size_t(decpt)) + "." + dig.substr(size_t(decpt));
  } else {
    o = dig.substr(0, 1); if (n > 1) o += "." + dig.substr(1);
    const int x = decpt - 1; char eb[16]; std::snprintf(eb, sizeof eb, "e%c%02d", x < 0 ? '-' : '+', x < 0 ? -x : x); o += eb;
  }
  return sign + o;
}
std::string k5_js(const std::string& s) {  // json.dumps(ensure_ascii=False)
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
    else if (c == '\b') o += "\\b"; else if (c == '\f') o += "\\f";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o + "\"";
}
std::string k5_addr(const Store& S, uint32_t sg, uint64_t o0) { return "[" + k5_js(S.sx(sg).nu) + "," + std::to_string(o0) + "]"; }
std::string k5_hx(uint64_t h) { char b[20]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b; }
std::string k5_kh(const std::vector<uint64_t>& v) {
  sa::Sha256 h;
  for (uint64_t x : v) { unsigned char b[8]; for (int i = 0; i < 8; i++) b[i] = (unsigned char)(x >> (8 * i)); h.update(std::string_view(reinterpret_cast<const char*>(b), 8)); }
  const auto d = h.finish();
  static const char hx[] = "0123456789abcdef";
  std::string o; for (int i = 0; i < 8; i++) { const unsigned c = (unsigned)d[size_t(i)]; o += hx[c >> 4]; o += hx[c & 15]; }
  return o;
}
// T3-1: the turns, the 30 cells and the queries, one JSON per line, sorted (K5R_DUMP_SPEC.md)
int k5_dump(Store& S, const std::vector<std::pair<std::string, std::string>>& qs, const char* file) {
  EbrGuard guard(S);
  std::vector<std::string> L;
  auto addr_less = [&](uint32_t a, uint32_t b) { return k5_addr_less(S, S.TT[a]->sg, S.TT[a]->o0, S.TT[b]->sg, S.TT[b]->o0); };
  for (uint32_t id = 1, n = S.nturn.load(); id <= n; id++) {
    TurnRec* t = tt_find(S, id);
    if (!t || t->k5in.load(std::memory_order_acquire) == 0) continue;
    std::string l = "{\"t\":\"turn\",\"a\":" + k5_addr(S, t->sg, t->o0) + ",\"the\":" + std::to_string(t->the) + ",\"th0\":" + std::to_string(t->th0) +
                    ",\"ks\":" + std::to_string(t->k5set.size()) + ",\"kh\":\"" + k5_kh(t->k5set) + "\",\"U\":[";
    const K5U* u = t->k5u.load(std::memory_order_acquire);
    std::vector<uint32_t> U = u ? u->U : std::vector<uint32_t>();
    std::sort(U.begin(), U.end(), addr_less);
    for (size_t i = 0; i < U.size(); i++) { if (i) l += ","; l += k5_addr(S, S.TT[U[i]]->sg, S.TT[U[i]]->o0); }
    l += "],\"C\":[";
    const K5C* c = t->k5c.load(std::memory_order_acquire);
    std::vector<K5CE> E = c ? c->e : std::vector<K5CE>();
    std::sort(E.begin(), E.end(), [&](const K5CE& a, const K5CE& b) { return addr_less(a.y, b.y); });
    for (size_t i = 0; i < E.size(); i++) {  // 4판: [c, y, α, β, hs as h values]
      if (i) l += ",";
      l += "[" + std::to_string(E[i].c) + "," + k5_addr(S, S.TT[E[i].y]->sg, S.TT[E[i].y]->o0) + "," + std::to_string(E[i].fl & 1) + "," + std::to_string((E[i].fl >> 1) & 1) + ",[";
      for (uint32_t k = 0; k < E[i].nh; k++) { if (k) l += ","; l += "\"" + k5_hx(t->Q[c->hs[E[i].h0 + k]]) + "\""; }
      l += "]]";
    }
    l += "],\"Ci\":[";
    if (c) for (size_t i = 0; i < c->ci.size(); i++) {
      const K5CI& s = c->ci[i];
      if (i) l += ",";
      l += "[" + std::to_string(s.k) + "," + std::to_string(s.a2) + "," + std::to_string(s.b2) + "," + std::to_string(s.a) + "," + std::to_string(s.n) + "]";
    }
    l += "]}";
    L.push_back(std::move(l));
  }
  auto syn_line = [&](uint64_t h, uint32_t y, const K5Syn& cl) {  // 4판: every σ with n ≥ 1
    const PJ p = k5_syn_judge(cl);
    if (!p.n) return;
    L.push_back("{\"t\":\"syn\",\"h\":\"" + k5_hx(h) + "\",\"y\":" + k5_addr(S, S.TT[y]->sg, S.TT[y]->o0) + ",\"a1\":" + std::to_string(p.a2) + ",\"b1\":" +
                std::to_string(p.b2) + ",\"a\":" + std::to_string(p.a) + ",\"n\":" + std::to_string(p.n) + ",\"s\":" + std::to_string(p.s) + ",\"st\":" +
                std::to_string(p.st) + ",\"w\":\"" + k5_repr(p.lower) + "\"}");
  };
  if (const K5Base* B = S.k5->base.load())
    for (int pi = 0; pi < 64; pi++) {
      const K5Part& P = B->kp[pi];
      for (size_t i = 0; i + 1 < P.off.size() && i < P.h.size(); i++)
        for (uint32_t j = P.off[i]; j < P.off[i + 1]; j++) syn_line(P.h[i], P.id[j], P.syn[j]);
    }
  for (size_t d = 0; d < LFMap<K5Head>::ND; d++) {
    auto* ch = S.k5->ok.dir[d].load(std::memory_order_acquire);
    if (!ch) continue;
    for (size_t i = 0; i < LFMap<K5Head>::CHN; i++)
      for (auto* n = ch[i].load(std::memory_order_acquire); n; n = n->next.load(std::memory_order_acquire)) {
        const K5Node* s = n->val.sent.load(std::memory_order_acquire);
        for (const K5Node* x = s ? s->nx[0].load(std::memory_order_acquire) : nullptr; x; x = x->nx[0].load(std::memory_order_acquire))
          if (const K5Syn* cl = x->syn.load(std::memory_order_acquire)) syn_line(n->key, x->id, *cl);
      }
  }
  for (int k = 0; k < K5NC; k++) {
    const PJ p = cell_judge(S.k5->cells[k]);
    L.push_back("{\"t\":\"cell\",\"k\":" + std::to_string(k) + ",\"a1\":" + std::to_string(p.a2) + ",\"b1\":" + std::to_string(p.b2) + ",\"a\":" + std::to_string(p.a) +
                ",\"n\":" + std::to_string(p.n) + ",\"s\":" + std::to_string(p.s) + ",\"st\":" + std::to_string(p.st) + ",\"w\":\"" + k5_repr(p.lower) + "\"}");
  }
  for (const auto& [id, text] : qs) {
    K5Q q; k5_query(S, text, {}, q);
    std::string l = "{\"t\":\"q\",\"id\":" + k5_js(id) + ",\"Q\":" + std::to_string(q.Q.size()) + ",\"reads\":" + std::to_string(q.reads) + ",\"R\":[";
    for (size_t i = 0; i < q.R.size(); i++) {
      const auto& r = q.R[i];
      if (i) l += ",";
      l += "[" + k5_js(S.sx(k5m_sg(S, q, r.y)).nu) + "," + std::to_string(k5m_o0(S, q, r.y)) + ",\"" + k5_repr(r.w) + "\"," + std::to_string(r.m) + "," +
           std::to_string(k5m_the(S, q, r.y)) + ",\"" + (r.sh >= 0 ? k5_hx(q.Q[size_t(r.sh)]) : std::string()) + "\"]";
    }
    l += "],\"rho\":[";
    for (size_t i = 0; i < q.rho; i++) {
      const auto& r = q.R[i];
      if (i) l += ",";
      l += "[" + k5_js(S.sx(k5m_sg(S, q, r.y)).nu) + "," + std::to_string(k5m_o0(S, q, r.y)) + "," + std::to_string(r.pq) + "," + std::to_string(r.pe) + "," + std::to_string(r.re) + "]";
    }
    l += "]}";
    L.push_back(std::move(l));
  }
  std::sort(L.begin(), L.end());
  FILE* f = vp_fopen(file, "wb");
  if (!f) return -3;
  for (const auto& l : L) { std::fwrite(l.data(), 1, l.size(), f); std::fputc('\n', f); }
  std::fclose(f);
  return 0;
}
// vrs_search: the ordered rows of D-r9 with the places a renderer needs (the daemon maps them to the originals and makes the pages)
std::string k5_row_json(Store& S, const K5Q& q, const K5Q::Row& r, bool replay) {
  const uint32_t sg = k5m_sg(S, q, r.y); const Sess& ss = S.sx(sg);
  uint64_t o1 = 0; uint32_t last = 0; std::vector<uint32_t> vi;
  if (r.y & K5OPEN) {
    const K5Open& o = q.open[r.y & ~K5OPEN];
    o1 = o.o1; last = o.last_item; vi = o.req;
  } else {
    const TurnRec& t = *S.TT[r.y];
    o1 = t.o1;
    last = t.pos1 > t.pos0 ? ss.sess[t.pos1 - 1] : 0;
    for (size_t i = 0; i < t.vi.size(); i++) {
      const Item& m = S.it[t.vi[i]];
      if (m.kind == K_USER || (i + 1 == t.vi.size() && m.kind == K_ASSIST)) vi.push_back(t.vi[i]);
    }
  }
  std::string j = "{\"sec\":" + std::string(replay ? "0" : "1") + ",\"f\":" + loc_quote(ss.path) + ",\"nu\":" + loc_quote(ss.nu) + ",\"o0\":" + std::to_string(k5m_o0(S, q, r.y)) +
                  ",\"o1\":" + std::to_string(o1) + ",\"open\":" + ((r.y & K5OPEN) ? "true" : "false") + ",\"the\":" + std::to_string(k5m_the(S, q, r.y)) +
                  ",\"w\":\"" + k5_repr(r.w) + "\",\"sh\":" + std::to_string(r.sh) + ",\"m\":" + std::to_string(r.m);
  if (last) j += ",\"lo\":" + std::to_string(S.it[last].off) + ",\"ln\":" + std::to_string(S.it[last].len);
  if (replay) {
    j += ",\"pq\":" + std::to_string(r.pq) + ",\"pe\":" + std::to_string(r.pe) + ",\"re\":" + std::to_string(r.re);
    if (r.re) {
      j += ",\"sq\":" + std::to_string(r.sq) + ",\"n\":" + std::to_string(r.n) + ",\"tau\":\"" + k5_repr(r.tau) + "\",\"hits\":[";
      for (size_t i = 0; i < r.hits.size(); i++) { if (i) j += ","; j += std::to_string(r.hits[i]); }
      j += "]";
    }
  }
  j += ",\"vi\":[";  // the view items of D-r9 (a Recall row names them, a Replay row shows them)
  for (size_t i = 0; i < vi.size(); i++) {
    const Item& m = S.it[vi[i]];
    if (i) j += ",";
    j += "[" + std::to_string(m.off) + "," + std::to_string(m.len) + "," + std::to_string(int(m.kind)) + "]";
  }
  j += "]";
  return j + "}";
}
std::string k5_search_json(Store& S, const std::string& text, const std::vector<std::string>& W) {
  const auto t0 = std::chrono::steady_clock::now();
  EbrGuard guard(S);
  K5Q q; k5_query(S, text, W, q);
  std::string o = "{\"ok\":true,\"Q\":" + std::to_string(q.Q.size()) + ",\"rare\":" + std::to_string(q.rare) + ",\"common\":" + std::to_string(q.Q.size() - q.rare) +
                  ",\"D\":" + std::to_string(q.D) + ",\"R\":" + std::to_string(q.R.size()) + ",\"rho\":" + std::to_string(q.rho) + ",\"re\":" + std::to_string(q.re) +
                  ",\"reads\":" + std::to_string(q.reads) + ",\"open\":" + std::to_string(q.open.size()) + ",\"cues\":[";
  for (size_t i = 0; i < q.Q.size(); i++) { if (i) o += ","; o += "[" + loc_quote(q.qtext[i]) + "," + std::to_string(q.d[i]) + "]"; }
  o += "],\"rows\":[";
  bool first = true;
  for (size_t i = 0; i < q.rho; i++) { if (!first) o += ","; first = false; o += k5_row_json(S, q, q.R[i], true); }
  for (size_t i = 0; i < q.R.size(); i++) { if (!first) o += ","; first = false; o += k5_row_json(S, q, q.R[i], false); }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  o += "],\"us\":" + std::to_string((long long)us) + "}";
  return o;
}
// K6 E-s1′ (VRS_K6_auto_recall_v1.md §6): ρ″ — R in ≺ order without the caller's file, the tier of the first w, at most 5 rows
std::string k5_recall_json(Store& S, const std::string& text, const std::vector<std::string>& W, const std::string& caller) {
  const auto t0 = std::chrono::steady_clock::now();
  EbrGuard guard(S);
  K5Q q; k5_query(S, text, W, q);
  const auto t1 = std::chrono::steady_clock::now();
  std::string o = "{\"ok\":true,\"Q\":" + std::to_string(q.Q.size()) + ",\"rare\":" + std::to_string(q.rare) + ",\"R\":" + std::to_string(q.R.size()) + ",\"cues\":[";
  for (size_t i = 0; i < q.Q.size(); i++) { if (i) o += ","; o += "[" + loc_quote(q.qtext[i]) + "," + std::to_string(q.d[i]) + "]"; }
  o += "],\"rows\":[";
  const Sess* own = caller.empty() ? nullptr : get_sess(S, canon_path(S, caller.c_str()), false);
  // D-s1′: the tier of w* among the rows not of the caller's file, by (dmin ↑, then ≺); dmin = the rarest d of a cue of Q in K(y), cut at 65
  std::vector<std::pair<uint64_t, const K5Q::Row*>> tier;
  for (const auto& r : q.R) {
    if (own && &S.sx(k5m_sg(S, q, r.y)) == own) continue;
    if (!tier.empty() && r.w != tier[0].second->w) break;
    const std::vector<uint64_t>& K = k5m_K(S, q, r.y);
    uint64_t dmin = K5F + 1;
    for (size_t i = 0; i < q.Q.size(); i++) if (q.d[i] < dmin && std::binary_search(K.begin(), K.end(), q.Q[i])) dmin = q.d[i];
    tier.push_back({dmin, &r});
  }
  std::stable_sort(tier.begin(), tier.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t n = 0; n < tier.size() && n < 5; n++) {
    if (n) o += ",";
    std::string row = k5_row_json(S, q, *tier[n].second, false);
    row.insert(row.size() - 1, ",\"dmin\":" + std::to_string(tier[n].first));
    o += row;
  }
  const auto t2 = std::chrono::steady_clock::now();
  o += "],\"qus\":" + std::to_string((long long)std::chrono::duration<double, std::micro>(t1 - t0).count()) +
       ",\"us\":" + std::to_string((long long)std::chrono::duration<double, std::micro>(t2 - t0).count()) + "}";
  return o;
}
void k5_free_obj(void* p, uint8_t kind) { if (kind == 5) delete static_cast<K5U*>(p); else delete static_cast<K5C*>(p); }
K5State* k5_new() { return new K5State; }
void k5_free(Store& S) {
  K5State* k = S.k5;
  if (!k) return;
#ifdef VRS_K5_PROF
  for (int i = 0; i < 8; i++) std::fprintf(stderr, "K5PROF %s ms=%.1f n=%llu\n", k5p_name[i], k5p_ns[i].load() / 1e6, (unsigned long long)k5p_n[i].load());
#endif
  for (uint32_t i = 1, e = S.nturn.load(); i <= e; i++) {
    TurnRec* t = tt_find(S, i);
    if (!t) continue;
    delete t->k5u.load(); delete t->k5c.load();
    for (K5UCand* c = t->k5ucand.load(); c;) { K5UCand* nx = c->next; delete c; c = nx; }
    for (K5CCand* c = t->k5ccand.load(); c;) { K5CCand* nx = c->next; delete c; c = nx; }
    for (DepChunk* c = t->k5rev.load(); c;) { DepChunk* nx = c->next.load(); delete c; c = nx; }
  }
  for (LFMap<K5Head>* M : {&k->ok, &k->oq, &k->ns, &k->nd})
    for (size_t d = 0; d < LFMap<K5Head>::ND; d++) {
      auto* c = M->dir[d].load(std::memory_order_relaxed);
      if (!c) continue;
      for (size_t i = 0; i < LFMap<K5Head>::CHN; i++)
        for (auto* n = c[i].load(std::memory_order_relaxed); n; n = n->next.load(std::memory_order_relaxed))
          for (K5Node* x = n->val.sent.load(); x;) { K5Node* nx = x->nx[0].load(); delete x->syn.load(); ::operator delete(x); x = nx; }
    }
  K5Base* b = k->base.load();
  if (k->building != b) delete k->building;
  delete b;
  delete k;
  S.k5 = nullptr;
}
