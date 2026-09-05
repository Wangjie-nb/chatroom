#ifndef TANK_ENGINE_H
#define TANK_ENGINE_H
// 坦克大战核心引擎（C++17）—— 服务端权威
// 单机：玩家 vs 3 辆 AI 敌方坦克，砖墙可破坏、钢墙不可破坏，消灭全部过关
// 双人：两名玩家对抗，各 N 条命，命中对方扣 1 命，命尽判负
// 弹药：弹夹 CLIP 发，打完需换弹 RELOAD_TIME（tick），换弹期间不能射击
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
namespace tn {
const int GRID = 24;          // 单机固定尺寸
const int MAXG = 32;          // 双人地图最大尺寸（可动态设 16~32）
const double TANK_SPEED = 0.30;    // 坦克每 tick 移动格数
const double BULLET_SPEED = 1.8;   // 子弹每 tick 移动格数（适中，弹道多几帧可见）
const int FIRE_CD = 15;            // 射击冷却（tick 数）
const int CLIP = 5;                // 弹夹容量
const int RELOAD_TIME = 50;        // 换弹耗时（tick，40ms/tick → 2 秒）
const int DIRS[8][2] = {{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}};  // 上 右上 右 右下 下 左下 左 左上
struct Pt { int x, y; };
struct Tank {
    double x = 0, y = 0;
    int dir = 1;          // 0上 1右上 2右 3右下 4下 5左下 6左 7左上
    int hp = 1;
    bool alive = true;
    bool mv = false;      // 是否在移动
    int fire = 0;         // 射击冷却剩余
    int ai = 0;           // AI 计时
    int stuck = 0;        // AI 连续卡住计数（用于让路）
    int ammo = CLIP;      // 弹夹剩余弹药
    int reload = 0;       // 换弹剩余 tick，>0 表示正在换弹
};
struct Bullet {
    double x = 0, y = 0;
    int dx = 0, dy = 0;
    int owner = 0;        // 1=A/玩家, 2=B/敌方
    bool alive = true;
};
static int dir_of(int dx, int dy) {
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    for (int i = 0; i < 8; i++)
        if (DIRS[i][0] == sx && DIRS[i][1] == sy) return i;
    return 2;
}
// 地图生成：0 空地、1 砖墙（可破坏）、2 钢墙（不可破坏）；fillPct 为 2×2 大格填充概率(%)
// 墙块按 2×2 网格对齐，空隙至少 2 格，1 格宽窄道大幅减少
// 空地连通性检查：从第一个空位 BFS，覆盖 85% 以上空地视为连通（避免墙把路堵死）
static bool map_connected(int m[MAXG][MAXG], int G) {
    static int qx[MAXG * MAXG], qy[MAXG * MAXG];
    static bool vis[MAXG][MAXG];
    int sx = -1, sy = -1;
    for (int y = 0; y < G && sx < 0; y++) for (int x = 0; x < G; x++)
        if (m[x][y] == 0) { sx = x; sy = y; break; }
    if (sx < 0) return false;
    std::memset(vis, 0, sizeof(bool) * MAXG * MAXG);
    int h = 0, t = 0; qx[t] = sx; qy[t] = sy; t++; vis[sx][sy] = true;
    int cnt = 1;
    const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    while (h < t) {
        int x = qx[h], y = qy[h]; h++;
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx >= 1 && nx < G - 1 && ny >= 0 && ny < G - 1 && !vis[nx][ny] && m[nx][ny] == 0) {
                vis[nx][ny] = true; qx[t] = nx; qy[t] = ny; t++;
                cnt++;
            }
        }
    }
    int total = 0;
    for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) if (m[x][y] == 0) total++;
    return total > 0 && cnt >= (int)(total * 0.85f);
}

// 确定性连通兜底：若空地不连通，把封闭区域的墙打穿成一扇门（优先砖墙，对称地图镜像同步开洞）
static void ensure_connected(int m[MAXG][MAXG], int G, bool sym) {
    static bool vis[MAXG][MAXG];
    static int qx[MAXG * MAXG], qy[MAXG * MAXG];
    const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    for (int iter = 0; iter < 128; iter++) {
        // BFS 主簇（从第一个空地）
        std::memset(vis, 0, sizeof(bool) * MAXG * MAXG);
        int sx = -1, sy = -1;
        for (int y = 0; y < G && sx < 0; y++) for (int x = 0; x < G; x++)
            if (m[x][y] == 0) { sx = x; sy = y; break; }
        if (sx < 0) return;
        int h = 0, t = 0; qx[t] = sx; qy[t] = sy; t++; vis[sx][sy] = true;
        while (h < t) {
            int x = qx[h], y = qy[h]; h++;
            for (int k = 0; k < 4; k++) {
                int nx = x + dx[k], ny = y + dy[k];
                if (nx >= 1 && nx < G - 1 && ny >= 0 && ny < G - 1 && !vis[nx][ny] && m[nx][ny] == 0) {
                    vis[nx][ny] = true; qx[t] = nx; qy[t] = ny; t++;
                }
            }
        }
        // 找封闭簇：未访问空地格，向 4 方向穿墙，墙另一侧是主簇则打通
        bool opened = false;
        for (int y = 0; y < G && !opened; y++) for (int x = 0; x < G && !opened; x++) {
            if (m[x][y] != 0 || vis[x][y]) continue;
            for (int k = 0; k < 4 && !opened; k++) {
                int wx = x + dx[k], wy = y + dy[k];
                if (wx < 1 || wx >= G - 1 || wy < 0 || wy >= G - 1 || m[wx][wy] == 0) continue;
                // 沿方向穿墙（墙带最多 6 格）
                int px = wx, py = wy, steps = 0, brickIdx = -1;
                while (px >= 1 && px < G - 1 && py >= 0 && py < G - 1 && m[px][py] != 0 && steps < 6) {
                    if (m[px][py] == 1 && brickIdx < 0) brickIdx = steps;
                    px += dx[k]; py += dy[k]; steps++;
                }
                if (px >= 1 && px < G - 1 && py >= 0 && py < G - 1 && m[px][py] == 0 && vis[px][py]) {
                    // 打通路径中的墙：优先砖墙，否则钢墙
                    int ox = wx + dx[k] * (brickIdx >= 0 ? brickIdx : 0);
                    int oy = wy + dy[k] * (brickIdx >= 0 ? brickIdx : 0);
                    m[ox][oy] = 0;
                    if (sym) m[G - 1 - ox][oy] = 0;   // 镜像同步开洞，保持对称
                    opened = true;
                }
            }
        }
        if (!opened) break;   // 已全部连通（或无法打开）
    }
}

// 统计空地连通图的环路数（冗余路径数）：环路 = 边数 - 节点数 + 连通分量数
// 环路 >= 2 表示有多条并行通路，避免"只有一条路"的独木桥地图
static int count_loops(int m[MAXG][MAXG], int G) {
    static bool vis[MAXG][MAXG];
    static int qx[MAXG * MAXG], qy[MAXG * MAXG];
    const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    int V = 0, E = 0, C = 0;
    for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) if (m[x][y] == 0) {
        V++;
        if (x + 1 < G && m[x + 1][y] == 0) E++;   // 只数右、下邻居避免重复
        if (y + 1 < G && m[x][y + 1] == 0) E++;
    }
    std::memset(vis, 0, sizeof(bool) * MAXG * MAXG);
    for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) {
        if (m[x][y] != 0 || vis[x][y]) continue;
        C++;
        int h = 0, t = 0; qx[t] = x; qy[t] = y; t++; vis[x][y] = true;
        while (h < t) {
            int cx = qx[h], cy = qy[h]; h++;
            for (int k = 0; k < 4; k++) {
                int nx = cx + dx[k], ny = cy + dy[k];
                if (nx >= 1 && nx < G - 1 && ny >= 0 && ny < G - 1 && !vis[nx][ny] && m[nx][ny] == 0) {
                    vis[nx][ny] = true; qx[t] = nx; qy[t] = ny; t++;
                }
            }
        }
    }
    return E - V + C;
}

// 地图生成：0 空地、1 砖墙（可破坏）、2 钢墙（不可破坏）；fillPct 为 2×2 大格填充概率(%)
// 保证：① 空地 100% 连通（ensure_connected 打通封闭墙洞）；② 至少 2 条并行通路（环路>=2），否则自动降密重试
static void gen_map(int m[MAXG][MAXG], int G, bool sym, int fillPct) {
    for (int attempt = 0; attempt < 12; attempt++) {
        int p = fillPct - attempt * 2;   // 每重试一次稀疏 2%
        if (p < 16) p = 16;
        std::memset(m, 0, sizeof(int) * MAXG * MAXG);
        for (int y = 0; y < G; y++) { m[0][y] = 2; m[G - 1][y] = 2; }
        for (int x = 0; x < G; x++) { m[x][G - 1] = 2; }
        int lim = sym ? G / 2 : G;
        int nx = (lim - 2) / 2, ny = (G - 2) / 2;   // 2×2 大格数
        for (int by = 0; by < ny; by++) for (int bx = 0; bx < nx; bx++) {
            int cx = 2 + bx * 2, cy = 2 + by * 2;
            if (std::rand() % 100 < p) {
                int w = 2, h = 2;
                int r = std::rand() % 100;
                if (r < 10) w = 3; else if (r < 20) h = 3;   // 少量 3 格变异，打破规整
                for (int a = 0; a < w; a++) for (int b = 0; b < h; b++) {
                    int xx = cx + a, yy = cy + b;
                    if (xx >= 1 && xx < lim - 1 && yy > 0 && yy < G - 1 && m[xx][yy] == 0)
                        m[xx][yy] = (std::rand() % 5 == 0) ? 2 : 1;
                }
            }
        }
        if (sym) {
            for (int y = 0; y < G; y++) for (int x = G / 2; x < G; x++)
                m[x][y] = m[G - 1 - x][y];
        }
        ensure_connected(m, G, sym);                 // ① 保证连通
        if (count_loops(m, G) >= 2) return;          // ② 保证至少 2 条并行通路
    }
}
// 清空以 (cx,cy) 为中心 5x5 区域，保证出生畅通
static void clear_spawn_area(int m[MAXG][MAXG], int G, int cx, int cy) {
    for (int x = cx - 2; x <= cx + 2; x++) for (int y = cy - 2; y <= cy + 2; y++)
        if (x >= 0 && x < G && y >= 0 && y < G) m[x][y] = 0;
}
// 在 [x0,x1]x[2,G-3] 内随机找 3x3 全空的出生点；ax,ay<0 表示不避让
static bool find_spawn(int m[MAXG][MAXG], int G, int x0, int x1, double &sx, double &sy,
                       double ax = -1, double ay = -1, double min_d = 0) {
    for (int attempt = 0; attempt < 300; attempt++) {
        int x = x0 + std::rand() % (x1 - x0 + 1);
        int y = 2 + std::rand() % (G - 4);
        bool clear = true;
        for (int a = -1; a <= 1 && clear; a++) for (int b = -1; b <= 1; b++) {
            int gx = x + a, gy = y + b;
            if (gx < 0 || gx >= G || gy < 0 || gy >= G || m[gx][gy] != 0) { clear = false; break; }
        }
        if (!clear) continue;
        if (ax >= 0 && std::abs((x + 0.5) - ax) + std::abs((y + 0.5) - ay) < min_d) continue;
        sx = x + 0.5; sy = y + 0.5; return true;
    }
    return false;
}
class Engine {
public:
    int map[MAXG][MAXG];      // 只用前 GRID 行/列
    Tank player;
    std::vector<Tank> enemies;
    std::vector<Bullet> bullets;
    std::vector<Pt> broken;
    int score = 0, lives = 3;
    bool running = false, win = false;
    long long last_tick = 0;
    void init(long long now) {
        gen_map(map, GRID, false, 26);
        // 玩家随机出生
        double px, py;
        if (!find_spawn(map, GRID, 1, GRID - 2, px, py)) { px = 2.5; py = (double)(GRID - 2.5); }
        clear_spawn_area(map, GRID, (int)px, (int)py);
        player = Tank();
        player.x = px; player.y = py; player.dir = 2; player.hp = 1; player.ammo = CLIP; player.reload = 0;
        // 3 个敌方随机出生，彼此及与玩家保持距离
        enemies.clear();
        double exs[3] = {0}, eys[3] = {0};
        int placed = 0, guard = 0;
        while (placed < 3 && guard++ < 300) {
            double ex, ey;
            if (!find_spawn(map, GRID, 1, GRID - 2, ex, ey, px, py, 7)) break;
            bool farEnough = true;
            for (int i = 0; i < placed; i++)
                if (std::abs(ex - exs[i]) + std::abs(ey - eys[i]) < 7) { farEnough = false; break; }
            if (!farEnough) continue;
            clear_spawn_area(map, GRID, (int)ex, (int)ey);
            enemies.push_back({ ex, ey, 2, 1, true, false, 0, 0, CLIP, 0 });
            exs[placed] = ex; eys[placed] = ey; placed++;
        }
        while (placed < 3) {  // 兜底
            enemies.push_back({ 3.5 + placed * 4, 2.5, 2, 1, true, false, 0, 0, CLIP, 0 });
            placed++;
        }
        bullets.clear(); broken.clear();
        score = 0; lives = 3; running = true; win = false;
        last_tick = now;
    }
    void set_move(int dx, int dy) {
        if (dx == 0 && dy == 0) { player.mv = false; return; }
        player.dir = dir_of(dx, dy); player.mv = true;
    }
    void stop() { player.mv = false; }
    void shoot() {
        if (player.fire <= 0 && player.ammo > 0 && player.reload <= 0) {
            double sx = player.x + DIRS[player.dir][0] * 0.6, sy = player.y + DIRS[player.dir][1] * 0.6;
            for (int k = 0; k < 10; k++) {   // 起点落墙则回退到空地（贴墙发射仍可见弹道）
                int gx = (int)sx, gy = (int)sy;
                if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID || map[gx][gy] == 0) break;
                sx -= DIRS[player.dir][0] * 0.1; sy -= DIRS[player.dir][1] * 0.1;
            }
            bullets.push_back({ sx, sy, DIRS[player.dir][0], DIRS[player.dir][1], 1, true });
            player.fire = FIRE_CD;
            player.ammo--;
            if (player.ammo <= 0) player.reload = RELOAD_TIME;
            if (player.ammo <= 0) player.reload = RELOAD_TIME;  // 打空弹夹开始换弹
        }
    }
    bool wall(int x, int y) const {
        if (x < 0 || x >= GRID || y < 0 || y >= GRID) return true;
        return map[x][y] > 0;
    }
    // 圆形碰撞：坦克视为半径为 R 的圆，墙视为格子矩形，圆与墙矩形相交才算撞
    bool collide_cell(double nx, double ny, int gx, int gy, double R) const {
        if (!wall(gx, gy)) return false;
        double cx = std::max((double)gx, std::min(nx, (double)gx + 1));
        double cy = std::max((double)gy, std::min(ny, (double)gy + 1));
        double dx = nx - cx, dy = ny - cy;
        return dx * dx + dy * dy < R * R - 1e-6;
    }
    bool canMove(const Tank &t, int d, double step = TANK_SPEED) const {
        double nx = t.x + DIRS[d][0] * step, ny = t.y + DIRS[d][1] * step;
        const double R = 0.35;
        int x0 = (int)std::floor(nx - R), x1 = (int)std::floor(nx + R);
        int y0 = (int)std::floor(ny - R), y1 = (int)std::floor(ny + R);
        for (int gx = x0; gx <= x1; gx++) for (int gy = y0; gy <= y1; gy++)
            if (collide_cell(nx, ny, gx, gy, R)) return false;
        // 坦克互撞：朝对方靠近则挡，已接触后朝远离方向移动则允许（分离，避免粘死）
        const bool selfIsPlayer = (&t == &player);
        if (!selfIsPlayer && player.alive) {
            double dx = player.x - nx, dy = player.y - ny;
            double d2 = dx * dx + dy * dy;
            if (d2 < (R + R) * (R + R) - 1e-6) {
                double cx = player.x - t.x, cy = player.y - t.y;
                if (d2 <= cx * cx + cy * cy - 1e-9) return false;
            }
        }
        for (const auto &e : enemies) if (e.alive) {
            if (&e == &t) continue;
            double dx = e.x - nx, dy = e.y - ny;
            double d2 = dx * dx + dy * dy;
            if (d2 < (R + R) * (R + R) - 1e-6) {
                double cx = e.x - t.x, cy = e.y - t.y;
                if (d2 <= cx * cx + cy * cy - 1e-9) return false;
            }
        }
        return true;
    }
    void moveTank(Tank &t, int d) {
        // 拆成 6 个小步，撞墙时能精确停在贴墙点；斜向(1,3,5,7)等速
        const int STEPS = 6;
        const double diag = (d == 1 || d == 3 || d == 5 || d == 7) ? 0.70710678 : 1.0;
        const double step = TANK_SPEED / STEPS * diag;
        for (int i = 0; i < STEPS; i++) {
            if (!canMove(t, d, step)) break;
            t.x += DIRS[d][0] * step;
            t.y += DIRS[d][1] * step;
        }
        t.dir = d;
    }
    void bulletMove(Bullet &b) {
        double rem = BULLET_SPEED;
        while (rem > 0.15 && b.alive) {
            double st = 0.15; rem -= st;
            b.x += b.dx * st; b.y += b.dy * st;
            int gx = (int)b.x, gy = (int)b.y;
            if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID) { b.alive = false; break; }
            if (map[gx][gy] == 1) { b.alive = false; map[gx][gy] = 0; broken.push_back({ gx, gy }); break; }
            if (map[gx][gy] == 2) { b.alive = false; break; }
            if (b.owner == 1) {
                for (auto &e : enemies) if (e.alive)
                    if (std::abs(e.x - b.x) < 0.7 && std::abs(e.y - b.y) < 0.7) {
                        e.alive = false; b.alive = false; score += 100; break;
                    }
            } else {
                if (player.alive && std::abs(player.x - b.x) < 0.7 && std::abs(player.y - b.y) < 0.7) {
                    player.hp--; b.alive = false;
                    if (player.hp <= 0) {
                        player.alive = false; lives--;
                        if (lives > 0) {
                            double rx, ry;
                            if (!find_spawn(map, GRID, 1, GRID - 2, rx, ry)) { rx = 2.5; ry = (double)(GRID - 2.5); }
                            clear_spawn_area(map, GRID, (int)rx, (int)ry);
                            player.x = rx; player.y = ry;
                            player.dir = 2; player.hp = 1; player.alive = true; player.fire = 0; player.mv = false;
                            player.ammo = CLIP; player.reload = 0;
                        }
                    }
                }
            }
        }
    }
    int tick() {
        if (!running) return 1;
        broken.clear();
        if (player.fire > 0) player.fire--;
        if (player.reload > 0) { player.reload--; if (player.reload <= 0) player.ammo = CLIP; }
        if (player.mv) moveTank(player, player.dir);
        for (auto &e : enemies) {
            if (!e.alive) continue;
            if (e.fire > 0) e.fire--;
            if (e.reload > 0) { e.reload--; if (e.reload <= 0) e.ammo = CLIP; }
            e.ai++;
            // 简单 AI：倾向朝玩家方向移动 + 随机变向 + 随机射击
            if (e.ai % 8 == 0 && std::rand() % 100 < 45) {
                if (std::abs(e.x - player.x) > std::abs(e.y - player.y)) e.dir = player.x > e.x ? 2 : 6;
                else e.dir = player.y > e.y ? 4 : 0;
            }
            if (e.ai % 12 == 0 && std::rand() % 100 < 40) e.dir = 2 * (std::rand() % 4);
            double ox = e.x, oy = e.y;
            moveTank(e, e.dir);
            // 卡住自动让路：连续无法移动则朝远离玩家的方向退让几步，再换向
            if (e.x == ox && e.y == oy) {
                e.stuck++;
                if (e.stuck >= 6) {
                    int back = 0; double best = -1;
                    for (int d2 = 0; d2 < 4; d2++) {
                        int dir2 = d2 * 2;
                        double nx2 = e.x + DIRS[dir2][0] * 0.05, ny2 = e.y + DIRS[dir2][1] * 0.05;
                        double dist = (nx2 - player.x) * (nx2 - player.x) + (ny2 - player.y) * (ny2 - player.y);
                        if (dist > best) { best = dist; back = d2; }
                    }
                    for (int k = 0; k < 8; k++) {
                        if (canMove(e, back, 0.05)) { e.x += DIRS[back][0] * 0.05; e.y += DIRS[back][1] * 0.05; }
                        else break;
                    }
                    e.dir = 2 * (std::rand() % 4); e.stuck = 0;
                }
            } else e.stuck = 0;
            if (e.ai % 4 == 0 && e.fire <= 0 && e.ammo > 0 && e.reload <= 0) {
                double sx = e.x + DIRS[e.dir][0] * 0.6, sy = e.y + DIRS[e.dir][1] * 0.6;
                for (int k = 0; k < 10; k++) {   // 起点落墙则回退到空地
                    int gx = (int)sx, gy = (int)sy;
                    if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID || map[gx][gy] == 0) break;
                    sx -= DIRS[e.dir][0] * 0.1; sy -= DIRS[e.dir][1] * 0.1;
                }
                bullets.push_back({ sx, sy, DIRS[e.dir][0], DIRS[e.dir][1], 2, true });
                e.fire = FIRE_CD;
                e.ammo--;
                if (e.ammo <= 0) e.reload = RELOAD_TIME;
            }
        }
        for (auto &b : bullets) if (b.alive) bulletMove(b);
        std::vector<Bullet> nb;
        for (auto &b : bullets) if (b.alive) nb.push_back(b);
        bullets = nb;
        bool allDead = true;
        for (auto &e : enemies) if (e.alive) { allDead = false; break; }
        if (allDead) { running = false; win = true; return 1; }
        if (lives <= 0) { running = false; win = false; return 1; }
        return 0;
    }
    std::string dump_map() const {
        std::string s; s.reserve(GRID * GRID);
        for (int y = 0; y < GRID; y++) for (int x = 0; x < GRID; x++) s.push_back('0' + map[x][y]);
        return s;
    }
    std::string dump_state() const {
        char buf[64];
        std::string t;
        snprintf(buf, sizeof(buf), "%.1f,%.1f,%d,%d,%d,%d,%d", player.x, player.y, player.dir, player.hp, player.alive ? 1 : 0, player.ammo, player.reload);
        t = buf;
        for (auto &e : enemies) {
            snprintf(buf, sizeof(buf), ";%.1f,%.1f,%d,%d,%d,%d,%d", e.x, e.y, e.dir, e.hp, e.alive ? 1 : 0, e.ammo, e.reload);
            t += buf;
        }
        std::string b;
        for (auto &bl : bullets) {
            snprintf(buf, sizeof(buf), "%s%.1f,%.1f,%d,%d,%d", b.empty() ? "" : ";", bl.x, bl.y, bl.dx, bl.dy, bl.owner);
            b += buf;
        }
        std::string w;
        for (auto &p : broken) {
            snprintf(buf, sizeof(buf), "%s%d,%d", w.empty() ? "" : ";", p.x, p.y);
            w += buf;
        }
        return t + "|" + b + "|" + std::to_string(score) + "|" + std::to_string(lives) + "|" + w;
    }
};
class PVPEngine {
public:
    int map[MAXG][MAXG];
    int G = 24;              // 当前地图尺寸（可设 16~32）
    Tank ta, tb;
    std::vector<Bullet> bullets;
    std::vector<Pt> broken;
    int livesA = 3, livesB = 3;
    bool running = false;
    int result = 0;   // 1 A胜 2 B胜
    long long last_tick = 0;
    void init(long long now, int gsize = 24, int lives = 3) {
        G = gsize; if (G < 16) G = 16; if (G > MAXG) G = MAXG;
        if (lives < 1) lives = 1; if (lives > 9) lives = 9;
        gen_map(map, G, true, 26);   // 2×2 大格 26% 填充 + 多通路保证，路更多条
        // A 随机出生（左半区）；B 出生在 A 的左右镜像格（x 镜像、y 相同），保持地图左右对称
        // gen_map 为左右镜像（格 g ↔ 格 G-1-g），B 清空区与 A 清空区互为镜像
        double ax, ay, bx, by;
        if (!find_spawn(map, G, 1, G / 2 - 3, ax, ay)) { ax = 2.5; ay = (double)(G - 2.5); }
        int acx = (int)ax, acy = (int)ay;
        int bcx = G - 1 - acx, bcy = acy;
        clear_spawn_area(map, G, acx, acy);
        bx = bcx + 0.5; by = bcy + 0.5;
        clear_spawn_area(map, G, bcx, bcy);
        ta = Tank(); ta.x = ax; ta.y = ay; ta.dir = 2; ta.hp = 1; ta.ammo = CLIP; ta.reload = 0;
        tb = Tank(); tb.x = bx; tb.y = by; tb.dir = 6; tb.hp = 1; tb.ammo = CLIP; tb.reload = 0;
        bullets.clear(); broken.clear();
        livesA = lives; livesB = lives; running = true; result = 0;
        last_tick = now;
    }
    void set_moveA(int dx, int dy) { if (dx == 0 && dy == 0) { ta.mv = false; return; } ta.dir = dir_of(dx, dy); ta.mv = true; }
    void set_moveB(int dx, int dy) { if (dx == 0 && dy == 0) { tb.mv = false; return; } tb.dir = dir_of(dx, dy); tb.mv = true; }
    void stopA() { ta.mv = false; }
    void stopB() { tb.mv = false; }
    void shootA() {
        if (ta.fire <= 0 && ta.ammo > 0 && ta.reload <= 0) {
            double sx = ta.x + DIRS[ta.dir][0] * 0.6, sy = ta.y + DIRS[ta.dir][1] * 0.6;
            for (int k = 0; k < 10; k++) {   // 起点落墙则回退到空地（贴墙发射仍可见弹道）
                int gx = (int)sx, gy = (int)sy;
                if (gx < 0 || gx >= G || gy < 0 || gy >= G || map[gx][gy] == 0) break;
                sx -= DIRS[ta.dir][0] * 0.1; sy -= DIRS[ta.dir][1] * 0.1;
            }
            bullets.push_back({ sx, sy, DIRS[ta.dir][0], DIRS[ta.dir][1], 1, true });
            ta.fire = FIRE_CD; ta.ammo--;
            if (ta.ammo <= 0) ta.reload = RELOAD_TIME;
        }
    }
    void shootB() {
        if (tb.fire <= 0 && tb.ammo > 0 && tb.reload <= 0) {
            double sx = tb.x + DIRS[tb.dir][0] * 0.6, sy = tb.y + DIRS[tb.dir][1] * 0.6;
            for (int k = 0; k < 10; k++) {   // 起点落墙则回退到空地
                int gx = (int)sx, gy = (int)sy;
                if (gx < 0 || gx >= G || gy < 0 || gy >= G || map[gx][gy] == 0) break;
                sx -= DIRS[tb.dir][0] * 0.1; sy -= DIRS[tb.dir][1] * 0.1;
            }
            bullets.push_back({ sx, sy, DIRS[tb.dir][0], DIRS[tb.dir][1], 2, true });
            tb.fire = FIRE_CD; tb.ammo--;
            if (tb.ammo <= 0) tb.reload = RELOAD_TIME;
        }
    }
    bool wall(int x, int y) const {
        if (x < 0 || x >= G || y < 0 || y >= G) return true;
        return map[x][y] > 0;
    }
    bool collide_cell(double nx, double ny, int gx, int gy, double R) const {
        if (!wall(gx, gy)) return false;
        double cx = std::max((double)gx, std::min(nx, (double)gx + 1));
        double cy = std::max((double)gy, std::min(ny, (double)gy + 1));
        double dx = nx - cx, dy = ny - cy;
        return dx * dx + dy * dy < R * R - 1e-6;
    }
    bool canMove(const Tank &t, int d, double step = TANK_SPEED) const {
        double nx = t.x + DIRS[d][0] * step, ny = t.y + DIRS[d][1] * step;
        const double R = 0.35;
        int x0 = (int)std::floor(nx - R), x1 = (int)std::floor(nx + R);
        int y0 = (int)std::floor(ny - R), y1 = (int)std::floor(ny + R);
        for (int gx = x0; gx <= x1; gx++) for (int gy = y0; gy <= y1; gy++)
            if (collide_cell(nx, ny, gx, gy, R)) return false;
        // 坦克互撞：朝对方靠近则挡，已接触后朝远离方向移动则允许（分离，避免粘死）
        const bool selfIsA = (&t == &ta);
        const bool selfIsB = (&t == &tb);
        if (!selfIsA && ta.alive) {
            double dx = ta.x - nx, dy = ta.y - ny;
            double d2 = dx * dx + dy * dy;
            if (d2 < (R + R) * (R + R) - 1e-6) {
                double cx = ta.x - t.x, cy = ta.y - t.y;
                if (d2 <= cx * cx + cy * cy - 1e-9) return false;
            }
        }
        if (!selfIsB && tb.alive) {
            double dx = tb.x - nx, dy = tb.y - ny;
            double d2 = dx * dx + dy * dy;
            if (d2 < (R + R) * (R + R) - 1e-6) {
                double cx = tb.x - t.x, cy = tb.y - t.y;
                if (d2 <= cx * cx + cy * cy - 1e-9) return false;
            }
        }
        return true;
    }
    void moveTank(Tank &t, int d) {
        // 拆成 6 个小步，撞墙时能精确停在贴墙点；斜向(1,3,5,7)等速
        const int STEPS = 6;
        const double diag = (d == 1 || d == 3 || d == 5 || d == 7) ? 0.70710678 : 1.0;
        const double step = TANK_SPEED / STEPS * diag;
        for (int i = 0; i < STEPS; i++) {
            if (!canMove(t, d, step)) break;
            t.x += DIRS[d][0] * step;
            t.y += DIRS[d][1] * step;
        }
        t.dir = d;
    }
    void bulletMove(Bullet &b) {
        double rem = BULLET_SPEED;
        while (rem > 0.15 && b.alive) {
            double st = 0.15; rem -= st;
            b.x += b.dx * st; b.y += b.dy * st;
            int gx = (int)b.x, gy = (int)b.y;
            if (gx < 0 || gx >= G || gy < 0 || gy >= G) { b.alive = false; break; }
            if (map[gx][gy] == 1) { b.alive = false; map[gx][gy] = 0; broken.push_back({ gx, gy }); break; }
            if (map[gx][gy] == 2) { b.alive = false; break; }
            if (b.owner == 1) {
                if (tb.alive && std::abs(tb.x - b.x) < 0.7 && std::abs(tb.y - b.y) < 0.7) {
                    b.alive = false; tb.alive = false; livesB--;
                    if (livesB > 0) {
                        double rx, ry;
                        if (!find_spawn(map, G, G / 2, G - 2, rx, ry)) { rx = (double)(G - 2.5); ry = 2.5; }
                        clear_spawn_area(map, G, (int)rx, (int)ry);
                        tb.x = rx; tb.y = ry; tb.dir = 6; tb.hp = 1; tb.alive = true; tb.fire = 0; tb.mv = false;
                        tb.ammo = CLIP; tb.reload = 0;
                    }
                }
            } else {
                if (ta.alive && std::abs(ta.x - b.x) < 0.7 && std::abs(ta.y - b.y) < 0.7) {
                    b.alive = false; ta.alive = false; livesA--;
                    if (livesA > 0) {
                        double rx, ry;
                        if (!find_spawn(map, G, 1, G / 2 - 1, rx, ry)) { rx = 2.5; ry = (double)(G - 2.5); }
                        clear_spawn_area(map, G, (int)rx, (int)ry);
                        ta.x = rx; ta.y = ry; ta.dir = 2; ta.hp = 1; ta.alive = true; ta.fire = 0; ta.mv = false;
                        ta.ammo = CLIP; ta.reload = 0;
                    }
                }
            }
        }
    }
    int tick() {
        if (!running) return 1;
        broken.clear();
        if (ta.fire > 0) ta.fire--;
        if (tb.fire > 0) tb.fire--;
        if (ta.reload > 0) { ta.reload--; if (ta.reload <= 0) ta.ammo = CLIP; }
        if (tb.reload > 0) { tb.reload--; if (tb.reload <= 0) tb.ammo = CLIP; }
        if (ta.mv) moveTank(ta, ta.dir);
        if (tb.mv) moveTank(tb, tb.dir);
        for (auto &b : bullets) if (b.alive) bulletMove(b);
        std::vector<Bullet> nb;
        for (auto &b : bullets) if (b.alive) nb.push_back(b);
        bullets = nb;
        if (livesA <= 0 && livesB <= 0) { running = false; result = 0; return 1; }  // 同归于尽 = 平局
        if (livesA <= 0) { running = false; result = 2; return 1; }
        if (livesB <= 0) { running = false; result = 1; return 1; }
        return 0;
    }
    std::string dump_map() const {
        std::string s; s.reserve(G * G);
        for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) s.push_back('0' + map[x][y]);
        return s;
    }
    std::string dump_state() const {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f,%.1f,%d,%d,%d,%d,%d", ta.x, ta.y, ta.dir, ta.hp, ta.alive ? 1 : 0, ta.ammo, ta.reload);
        std::string t = buf;
        snprintf(buf, sizeof(buf), "%.1f,%.1f,%d,%d,%d,%d,%d", tb.x, tb.y, tb.dir, tb.hp, tb.alive ? 1 : 0, tb.ammo, tb.reload);
        t += "|" + std::string(buf);
        std::string b;
        for (auto &bl : bullets) {
            snprintf(buf, sizeof(buf), "%s%.1f,%.1f,%d,%d,%d", b.empty() ? "" : ";", bl.x, bl.y, bl.dx, bl.dy, bl.owner);
            b += buf;
        }
        std::string w;
        for (auto &p : broken) {
            snprintf(buf, sizeof(buf), "%s%d,%d", w.empty() ? "" : ";", p.x, p.y);
            w += buf;
        }
        return t + "|" + b + "|" + std::to_string(livesA) + "|" + std::to_string(livesB) + "|" + w;
    }
};
}
#endif
