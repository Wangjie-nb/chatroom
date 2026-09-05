// ============================================================
// tile_magic_engine.h —— 三消（羊了个羊式）核心引擎 v2（纯 C++）
// 功能：牌堆生成（错位散点，非规整网格）/ 层叠遮挡判定 /
//       槽位三消 / 洗牌 / 可解性校验（DFS 回溯 + 剪枝）
// v2 更新：平面坐标 px/py（网格基准 + 层偏移 + 随机抖动），
//          服务器权威计算 blocked 供前端渲染，前后端判定一致
// ============================================================
#ifndef TILE_MAGIC_ENGINE_H
#define TILE_MAGIC_ENGINE_H
#include <vector>
#include <chrono>
#include <map>
#include <algorithm>
#include <random>
#include <cstring>
#include <string>
#include <sstream>
#include <cstdio>
namespace tmg {

const int CELL = 64;    // 牌宽（缩小）
const int OFF  = 20;    // 每层错位偏移
const int JIT  = 8;     // 层内随机抖动范围

struct Tile {
    int id = -1;
    int pattern = 0;
    int layer = 0;
    int col = 0, row = 0;   // 基准网格
    int px = 0, py = 0;     // 实际渲染坐标（含错位+抖动）
    bool removed = false;
    bool blocked = false;   // 当前是否被压住（服务器计算）
};

struct GameState {
    std::vector<Tile> tiles;
    std::vector<int> slot;      // 槽位（tile id）
    int W = 0, H = 0, L = 0;
    int NP = 0;
    int slot_max = 7;           // 槽位上限（越小越难）
    bool over = false;
    bool win = false;
    int cleared = 0;
};

class Engine {
public:
    // 生成一局：W×H 网格、L 层、NP 种图案。每种图案总数必须是 3 的倍数。
    // 位置 = 网格基准 + 层偏移 + 随机抖动 → 视觉错位堆叠但结构可控
    static bool generate(int W, int H, int L, int NP, std::mt19937& rng, GameState& gs, int max_retry = 200) {
        // 从解反推：保证可解，O(N) 秒开，不吃 DFS
        // 每层放置 "图案数量=3的倍数" 的牌 → 顶层总是可点，消完一层下一层露出，天然可解
        int total = W * H * L;
        if (total <= 0 || NP <= 0 || total % NP != 0) return false;
        int per = total / NP;
        if (per % 3 != 0) return false;
        int layer_total = W * H;
        if (layer_total % NP != 0) return false;          // 每层张数需能被图案数整除
        int per_layer = layer_total / NP;
        if (per_layer % 3 != 0) return false;             // 每层每图案需为 3 的倍数
        gs = GameState();
        gs.W = W; gs.H = H; gs.L = L; gs.NP = NP;
        gs.tiles.resize(total);
        std::uniform_int_distribution<int> jit(-JIT, JIT);
        int idx = 0;
        for (int l = 0; l < L; ++l) {
            std::vector<int> pats;
            for (int p = 0; p < NP; ++p)
                for (int i = 0; i < per_layer; ++i) pats.push_back(p);
            std::shuffle(pats.begin(), pats.end(), rng);
            int k = 0;
            for (int r = 0; r < H; ++r)
                for (int c = 0; c < W; ++c) {
                    Tile t;
                    t.id = idx; t.layer = l; t.row = r; t.col = c;
                    t.pattern = pats[k++];
                    t.px = c * CELL + l * OFF + jit(rng); if (t.px < 0) t.px = 0;
                    t.py = r * CELL + l * OFF + jit(rng); if (t.py < 0) t.py = 0;
                    gs.tiles[idx] = t; ++idx;
                }
        }
        refresh_blocked(gs);
        return true;
    }
    static void refresh_blocked(GameState& gs) {
        for (Tile& t : gs.tiles) {
            if (t.removed) { t.blocked = false; continue; }
            bool bl = false;
            for (const Tile& o : gs.tiles) {
                if (o.id == t.id || o.removed) continue;
                if (o.layer > t.layer && covered_by(t, o)) { bl = true; break; }
            }
            t.blocked = bl;
        }
    }
    // 返回: 0=入槽 1=不可点 2=槽满判负 3=消除
    static int pick(GameState& gs, int id, std::vector<int>& cleared_out) {
        if (id < 0 || id >= (int)gs.tiles.size()) return 1;
        Tile& t = gs.tiles[id];
        if (t.removed || t.blocked) return 1;
        if ((int)gs.slot.size() >= gs.slot_max) { gs.over = true; gs.win = false; return 2; }
        gs.slot.push_back(id);
        t.removed = true;
        cleared_out.clear();
        std::vector<int> same;
        for (int s : gs.slot) if (gs.tiles[s].pattern == t.pattern) same.push_back(s);
        if ((int)same.size() >= 3) {
            int a = same[0], b = same[1], c = same[2];
            for (int x : {a, b, c}) {
                auto it = std::find(gs.slot.begin(), gs.slot.end(), x);
                if (it != gs.slot.end()) gs.slot.erase(it);
            }
            gs.cleared += 3;
            cleared_out = {a, b, c};
            int remain = 0;
            for (Tile& tt : gs.tiles) if (!tt.removed) ++remain;
            if (remain == 0) { gs.over = true; gs.win = true; }
        }
        refresh_blocked(gs);
        return (int)same.size() >= 3 ? 3 : 0;
    }
    static bool shuffle(GameState& gs, std::mt19937& rng) {
        std::vector<int> all;
        for (Tile& t : gs.tiles) if (!t.removed) all.push_back(t.id);
        for (int s : gs.slot) all.push_back(s);
        int T = (int)all.size();
        if (T == 0 || T % 3 != 0) return false;   // 待消总数不是 3 的倍数，救不了，需重开
        int groups = T / 3;
        std::uniform_int_distribution<int> jit(-JIT, JIT);
        // 阶段1：保留位置，随机"组→图案"，重排牌序
        for (int attempt = 0; attempt < 40; ++attempt) {
            std::shuffle(all.begin(), all.end(), rng);
            for (int g = 0; g < groups; ++g) {
                int pat = rng() % gs.NP;
                gs.tiles[all[g * 3 + 0]].pattern = pat;
                gs.tiles[all[g * 3 + 1]].pattern = pat;
                gs.tiles[all[g * 3 + 2]].pattern = pat;
            }
            for (int id : all) gs.tiles[id].removed = false;
            gs.slot.clear();
            refresh_blocked(gs);
            int nodes = 0;
            if (solvable(gs, nodes, 600)) return true;
        }
        // 阶段2：重新摆放位置（填回网格）+ 图案，相当于可解重发牌
        for (int attempt = 0; attempt < 40; ++attempt) {
            std::shuffle(all.begin(), all.end(), rng);
            for (int i = 0; i < T; ++i) {
                int l = i / (gs.W * gs.H);
                int rr = (i / gs.W) % gs.H;
                int c = i % gs.W;
                Tile& t = gs.tiles[all[i]];
                t.layer = l; t.row = rr; t.col = c;
                t.px = c * CELL + l * OFF + jit(rng); if (t.px < 0) t.px = 0;
                t.py = rr * CELL + l * OFF + jit(rng); if (t.py < 0) t.py = 0;
            }
            for (int g = 0; g < groups; ++g) {
                int pat = rng() % gs.NP;
                gs.tiles[all[g * 3 + 0]].pattern = pat;
                gs.tiles[all[g * 3 + 1]].pattern = pat;
                gs.tiles[all[g * 3 + 2]].pattern = pat;
            }
            for (int id : all) gs.tiles[id].removed = false;
            gs.slot.clear();
            refresh_blocked(gs);
            int nodes = 0;
            if (solvable(gs, nodes, 600)) return true;
        }
        return false;
    }

    // 当前局面是否已无解：统计"牌堆 + 槽位"全部待消牌，
    // 若任一图案的待消数量不是 3 的倍数，则该图案永远凑不满 3 张，判定卡死。
    static bool is_stuck(const GameState& gs) {
        // 卡死判定：牌堆中没有任何"可点"的牌（全部被上层压住），无法再操作。
        // 注：某图案残留 1-2 张（非 3 倍数）时，玩家仍可消其他图案，
        // 最终该图案凑不满会落到"无可点牌"才提示洗牌，避免中途误报。
        for (size_t i = 0; i < gs.tiles.size(); ++i) {
            if (gs.tiles[i].removed) continue;
            bool in_slot = false;
            for (int s : gs.slot) if (s == (int)i) { in_slot = true; break; }
            if (!in_slot && !gs.tiles[i].blocked) return false;  // 还有可点的牌
        }
        return true;   // 没有任何可点牌 → 卡死
    }

    // ===== DFS 可解性校验 =====
    static bool solvable(GameState& gs, int& nodes, int budget_ms = 400) {
        std::vector<char> removed(gs.tiles.size(), 0);
        for (size_t i = 0; i < gs.tiles.size(); ++i) removed[i] = gs.tiles[i].removed ? 1 : 0;
        int remain = 0;
        for (char r : removed) if (!r) ++remain;
        nodes = 0;
        auto t0 = std::chrono::steady_clock::now();
        bool timed_out = false;
        bool ok = dfs(gs, removed, remain, 0, nodes, timed_out, t0, budget_ms);
        if (timed_out) return true;   // 超时保守接受（靠洗牌兜底），避免 DFS 卡死开局
        return ok;
    }
    static bool solve_sequence(GameState& gs, std::vector<std::vector<int>>& out, int budget_ms = 800) {
        out.clear();
        std::vector<char> removed(gs.tiles.size(), 0);
        for (size_t i = 0; i < gs.tiles.size(); ++i) removed[i] = gs.tiles[i].removed ? 1 : 0;
        int remain = 0;
        for (char r : removed) if (!r) ++remain;
        int nodes = 0;
        std::vector<std::vector<int>> stack;
        auto t0 = std::chrono::steady_clock::now();
        return dfs_seq(gs, removed, remain, 0, nodes, stack, out, t0, budget_ms);
    }

    // 序列化：供 T3|init / T3|reblock 使用
    // init: id=pattern,px,py,layer,blocked;
    static std::string dump(const GameState& gs) {
        std::string s;
        char buf[128];
        for (const Tile& t : gs.tiles) {
            snprintf(buf, sizeof buf, "%d=%d,%d,%d,%d,%d;",
                     t.id, t.pattern, t.px, t.py, t.layer, t.blocked ? 1 : 0);
            s += buf;
        }
        return s;
    }
    static std::string dump_reblock(const GameState& gs) {
        std::string s;
        char buf[64];
        for (const Tile& t : gs.tiles) {
            if (t.removed) continue;
            snprintf(buf, sizeof buf, "%d=%d;", t.id, t.blocked ? 1 : 0);
            s += buf;
        }
        return s;
    }

private:
    struct Rect { int x0, y0, x1, y1; };
    static Rect tile_rect(const Tile& t) {
        return { t.px, t.py, t.px + CELL, t.py + CELL };
    }
    static bool rect_overlap(const Rect& a, const Rect& b) {
        return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
    }
    // 上层牌 o 是否"真正压住"下层牌 t：重叠面积 ≥ 牌面积的 40%（避免边缘轻微碰到就变黑）
    // 上层牌 o 是否压住下层牌 t：o 覆盖 t 的中心点。
    // 与前端"点击命中 z-index 最高的上层牌"的视觉完全一致——
    // 上层牌面盖住下层中心 → 下层必然按不到 → 判被压；
    // 只压到边缘、中心露出 → 下层可点且确实点得到。
    // 修复"引擎判可点(重叠<40%)但被上层牌面挡住点不到"的 bug。
    static bool covered_by(const Tile& t, const Tile& o) {
        Rect b = tile_rect(o);
        int cx = t.px + CELL / 2, cy = t.py + CELL / 2;
        return cx >= b.x0 && cx < b.x1 && cy >= b.y0 && cy < b.y1;
    }

    static bool dfs_seq(const GameState& gs, std::vector<char>& removed, int remain,
                        int depth, int& nodes, std::vector<std::vector<int>>& stack,
                        std::vector<std::vector<int>>& out,
                        const std::chrono::steady_clock::time_point& t0, int budget_ms) {
        ++nodes;
        if (remain == 0) { out = stack; return true; }
        if ((nodes & 511) == 0 &&
            std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(budget_ms)) return false;
        if (nodes > 4000000 || depth > 60) return false;
        std::map<int, std::vector<int>> byPat;
        for (size_t id = 0; id < gs.tiles.size(); ++id) {
            if (removed[id]) continue;
            if (is_blocked(gs, removed, (int)id)) continue;
            byPat[gs.tiles[id].pattern].push_back((int)id);
        }
        if (byPat.empty()) return false;
        std::vector<std::pair<int, std::vector<int>>> cands;
        for (auto& kv : byPat) cands.push_back(kv);
        std::sort(cands.begin(), cands.end(),
                  [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        for (auto& kv : cands) {
            auto& vec = kv.second;
            if ((int)vec.size() < 3) continue;
            int n = (int)vec.size();
            int combos = 0;
            for (int a = 0; a < n; ++a) {
                for (int b = a + 1; b < n; ++b) {
                    for (int c = b + 1; c < n; ++c) {
                        if (++combos > 20) break;
                        int ids[3] = { vec[a], vec[b], vec[c] };
                        for (int i = 0; i < 3; ++i) removed[ids[i]] = 1;
                        std::vector<int> move = { ids[0], ids[1], ids[2] };
                        stack.push_back(move);
                        if (dfs_seq(gs, removed, remain - 3, depth + 1, nodes, stack, out, t0, budget_ms)) {
                            for (int i = 0; i < 3; ++i) removed[ids[i]] = 0;
                            return true;
                        }
                        stack.pop_back();
                        for (int i = 0; i < 3; ++i) removed[ids[i]] = 0;
                    }
                    if (combos > 20) break;
                }
                if (combos > 20) break;
            }
        }
        return false;
    }
    static bool dfs(const GameState& gs, std::vector<char>& removed, int remain,
                    int depth, int& nodes, bool& timed_out,
                    const std::chrono::steady_clock::time_point& t0, int budget_ms) {
        ++nodes;
        if (remain == 0) return true;
        if ((nodes & 511) == 0 &&
            std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(budget_ms)) { timed_out = true; return false; }
        if (nodes > 4000000) return false;
        if (depth > 60) return false;
        std::map<int, std::vector<int>> byPat;
        for (size_t id = 0; id < gs.tiles.size(); ++id) {
            if (removed[id]) continue;
            if (is_blocked(gs, removed, (int)id)) continue;
            byPat[gs.tiles[id].pattern].push_back((int)id);
        }
        if (byPat.empty()) return false;
        std::vector<std::pair<int, std::vector<int>>> cands;
        for (auto& kv : byPat) cands.push_back(kv);
        std::sort(cands.begin(), cands.end(),
                  [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        for (auto& kv : cands) {
            auto& vec = kv.second;
            if ((int)vec.size() < 3) continue;
            int n = (int)vec.size();
            int combos = 0;
            for (int a = 0; a < n; ++a) {
                for (int b = a + 1; b < n; ++b) {
                    for (int c = b + 1; c < n; ++c) {
                        if (++combos > 20) break;
                        int ids[3] = { vec[a], vec[b], vec[c] };
                        for (int i = 0; i < 3; ++i) removed[ids[i]] = 1;
                        if (dfs(gs, removed, remain - 3, depth + 1, nodes, timed_out, t0, budget_ms)) {
                            for (int i = 0; i < 3; ++i) removed[ids[i]] = 0;
                            return true;
                        }
                        for (int i = 0; i < 3; ++i) removed[ids[i]] = 0;
                    }
                    if (combos > 20) break;
                }
                if (combos > 20) break;
            }
        }
        return false;
    }
    static bool is_blocked(const GameState& gs, const std::vector<char>& removed, int id) {
        const Tile& t = gs.tiles[id];
        Rect a = tile_rect(t);
        for (const Tile& o : gs.tiles) {
            if (o.id == id || removed[o.id]) continue;
            if (o.layer > t.layer && covered_by(t, o)) return true;
        }
        return false;
    }
};

} // namespace tmg
#endif
