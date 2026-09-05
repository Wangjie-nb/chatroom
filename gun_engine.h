// ============================================================
// gun_engine.h — 枪战肉鸽引擎（服务器权威，单机/联机共用）
// 玩法：怪从顶部进入，炮台在底部区域移动 + 自由角度瞄准射击
// 打怪得经验 → 升级三选一随机加成（伤害/攻速/穿透/减速/点燃/中毒/散射/护盾）
// 漏怪达到上限即失败；坚持打完所有波次即胜利
// 联机：双炮台共守同一基地（turrets[0]/[1]）
// ============================================================
#ifndef GUN_ENGINE_H
#define GUN_ENGINE_H
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace gun {

const double TICK = 0.04;          // 每 tick 秒数（40ms）
const double W = 16.0;             // 场地逻辑宽（格）
const double H = 20.0;             // 场地逻辑高（格）
const double LEAK_Y = H - 0.5;     // 漏怪线：怪 y 超过此线即漏怪
const double MOVE_MIN_Y = H * 0.70;// 炮台活动区上界
const double MOVE_MAX_Y = H - 1.0; // 炮台活动区下界
const int MAX_TURRETS = 2;

// 难度
enum Diff { DIFF_EASY = 0, DIFF_MED = 1, DIFF_HARD = 2 };

// buff 类型（升级三选一技能池）
enum BuffType {
    B_DMG = 0,      // 子弹伤害 +
    B_ATKSPD = 1,   // 攻速 +
    B_PIERCE = 2,   // 子弹穿透 +
    B_SLOW = 3,     // 命中减速（怪减速）
    B_BURN = 4,     // 点燃（持续灼烧）
    B_POISON = 5,   // 中毒（持续毒伤，可叠）
    B_SPREAD = 6,   // 散射 +
    B_HEAL = 7,     // 回血/护盾
    B_MAG = 8       // 弹夹容量 +
};
const char* BUFF_NAME[9] = { "伤害+", "攻速+", "穿透+", "减速", "点燃", "中毒", "散射+", "护盾", "弹夹+" };

// 怪
struct Monster {
    double x, y;
    double hp, maxhp;
    int type;            // 0 小怪 1 中怪 2 精英 3 头目
    int exp;             // 击杀经验
    double baseSpeed;    // 基础速度（格/tick）
    bool alive;
    double slowT;        // 减速剩余 tick
    double slowMul;      // 减速倍率（<1）
    double burnT, burnDps;     // 点燃剩余 tick + 每 tick 灼烧
    double poisonT, poisonDps; // 中毒剩余 tick + 每 tick 毒伤
    int hitBy[2];        // 已被谁的穿透弹命中标记（防同一子弹重复扣血）
    long long id;        // 唯一 ID（前端按 ID 插值，防止击杀后索引错位）
};

// 子弹
struct Bullet {
    double x, y, vx, vy;
    int owner;           // 0=A 1=B
    double dmg;
    int pierce;          // 剩余穿透次数
    double slowMul, slowT;      // 命中施加的减速
    double burnDps, burnT;      // 命中施加的点燃
    double poisonDps, poisonT;  // 命中施加的中毒
    bool alive;
};

// 炮台
struct Turret {
    double x, y;
    double angle;        // 瞄准角度（弧度）
    bool alive;
    int level;
    long long exp, expNeed;
    double dmgMul;       // 伤害倍率
    double atkSpd;       // 攻速（发/秒）
    double fireCd;       // 射击冷却（tick）
    int pierce;          // 穿透次数（0 表示不穿透）
    double slowMul;      // 减速强度（命中减速倍率）
    double burnDps, burnT;   // 点燃强度
    double poisonDps, poisonT;
    int spread;          // 散射数量
    int mag, magSize;    // 弹夹当前/容量
    double reloadT;      // 换弹剩余 tick（>0 表示换弹中）
    double reloadTime;   // 换弹耗时（tick）
    int hp, maxhp;       // 炮台血量（被怪贴身碰到扣血）
    bool upgradePending; // 是否等待三选一
    int choice[3];       // 三个随机加成
    int upgradeTimer;    // 升级等待倒计时（tick），超时自动随机选（防永久冻结卡死）
    int fireMode;        // 0 自动开火 1 手动瞄准
    int mvx;             // 移动方向：-1 左 0 停 1 右（tick 统一推进）
    int firing;          // 手动模式开火标志：1 按住中（tick 自动射）
};
const char* FIRE_MODE_NAME[2] = { "自动开火", "瞄准开火" };

struct WaveState {
    int wave;            // 当前波（从 1 开始）
    int totalWaves;
    int toSpawn;         // 本波怪总数
    int spawned;         // 已生成
    double spawnTimer;   // 生成计时（tick）
    double spawnGap;     // 生成间隔（tick）
    int leaked;          // 累计漏怪
    int maxLeak;         // 漏怪上限
    int phase;           // 0 波间休息 1 生成中 2 波结束等下一波
    double restTimer;
    bool over;
};

class Engine {
public:
    Turret turrets[MAX_TURRETS];
    std::vector<Monster> monsters;
    long long nextMId = 1;   // 怪唯一 ID 计数器
    std::vector<Bullet> bullets;
    WaveState wave;
    int diff;
    int nPlayers;        // 1 单机 2 联机
    bool over;
    int result;          // 0 进行中 1 胜利 2 失败
    unsigned seed;
    long long last_tick; // 服务端 tick 节流时间戳
    bool paused;         // 暂停

    void init(int diff_, int players, unsigned sd) {
        diff = diff_;
        nPlayers = players;
        seed = sd;
        std::srand(seed);
        over = false; result = 0;
        last_tick = 0;
        paused = false;
        bullets.clear(); monsters.clear();
        // 炮台
        for (int i = 0; i < nPlayers; i++) {
            Turret &t = turrets[i];
            t.x = W * (i == 0 ? 0.38 : 0.62);
            t.y = H - 2.2;
            t.angle = -M_PI / 2;   // 默认朝上
            t.alive = true;
            t.level = 1;
            t.exp = 0;
            t.expNeed = 35;
            t.upgradeTimer = 0;
            t.dmgMul = 1.0;
            t.atkSpd = 2.8;
            t.fireCd = 0;
            t.pierce = 0;
            t.slowMul = 1.0;       // 1.0 = 不减速
            t.burnDps = 0; t.burnT = 0;
            t.poisonDps = 0; t.poisonT = 0;
            t.spread = 1;
            t.magSize = 12;
            t.mag = 12;
            t.reloadT = 0;
            t.reloadTime = 100;    // 4 秒换弹
            t.hp = t.maxhp = 3;
            t.upgradePending = false;
            for (int c = 0; c < 3; c++) t.choice[c] = 0;
            t.fireMode = 0;   // 默认自动开火
            t.mvx = 0;
            t.firing = 0;
        }
        // 波次参数（按难度）
        static const int wavesByDiff[3] = { 8, 12, 16 };
        static const int maxLeakByDiff[3] = { 28, 18, 12 };
        wave.wave = 0;
        wave.totalWaves = wavesByDiff[diff];
        wave.leaked = 0;
        wave.maxLeak = maxLeakByDiff[diff];
        wave.phase = 0;
        wave.restTimer = 120;      // 开局 4.8 秒准备
        wave.over = false;
        begin_wave();
    }

    // 开始下一波
    void begin_wave() {
        if (wave.over) return;
        wave.wave++;
        if (wave.wave > wave.totalWaves) {
            wave.over = true;
            if (!over) { over = true; result = 1; }   // 打完所有波 → 胜利
            return;
        }
        wave.spawned = 0;
        wave.spawnTimer = 0;
        wave.toSpawn = wave_count();
        wave.spawnGap = std::max(10, 36 - wave.wave * 2);   // 生成间隔随波次变密
        wave.phase = 1;
    }

    // 本波怪数（按难度 + 波次递增）
    int wave_count() const {
        double base;
        switch (diff) {
            case DIFF_EASY: base = 5 + wave.wave * 1.2; break;
            case DIFF_MED:  base = 6 + wave.wave * 1.5; break;
            default:        base = 8 + wave.wave * 2.0; break;
        }
        return (int)base;
    }

    // 生成一只怪：类型按波次/难度分布
    void spawn_monster() {
        if ((int)monsters.size() >= 80) return;   // 场上怪上限，防止堆积拖慢服务器

        Monster m;
        int w = wave.wave;
        // 类型概率：越后面越高等级（逐波明显提升）
        int r = std::rand() % 100;
        int type = 0;
        int eliteP = 5 + w * 3;      // 精英概率（每波 +3%）
        int bossP = (w >= 3) ? (w - 2) * 3 : 0;   // 头目概率（第 3 波起，每波 +3%）
        int midP = (w >= 2) ? (16 + (w - 2) * 2) : 0;   // 中怪概率（逐波增加）
        if (r < bossP) type = 3;
        else if (r < bossP + eliteP) type = 2;
        else if (r < bossP + eliteP + midP) type = 1;
        else type = 0;
        // 数值（难度修正：血量/速度）
        double hpMul = 1.0, spdMul = 1.0;
        switch (diff) {
            case DIFF_EASY: hpMul = 0.62; spdMul = 0.82; break;
            case DIFF_MED:  hpMul = 0.85; spdMul = 1.0; break;
            default:        hpMul = 1.18; spdMul = 1.18; break;
        }
        static const double baseHp[4] = { 16, 45, 115, 300 };
        static const double baseSpd[4] = { 0.040, 0.034, 0.027, 0.023 };
        static const int baseExp[4] = { 8, 20, 50, 120 };
        m.type = type;
        // 强度随波次复合增长：血量每波 +8% 叠加、速度每波 +3% 叠加、经验每波 +6%
        m.maxhp = baseHp[type] * hpMul * std::pow(1.08, w - 1);
        m.hp = m.maxhp;
        m.baseSpeed = baseSpd[type] * spdMul * std::pow(1.03, w - 1);
        m.exp = (int)(baseExp[type] * std::pow(1.06, w - 1));
        m.x = 0.8 + (std::rand() % 1000) / 1000.0 * (W - 1.6);
        m.y = -0.6;
        m.alive = true;
        m.slowT = 0; m.slowMul = 1.0;
        m.burnT = 0; m.burnDps = 0;
        m.poisonT = 0; m.poisonDps = 0;
        m.hitBy[0] = m.hitBy[1] = 0;
        m.id = nextMId++;
        monsters.push_back(m);
    }

    // 炮台移动（底部活动区）
    void set_move(int pi, double dx, double dy) {
        if (pi < 0 || pi >= nPlayers) return;
        Turret &t = turrets[pi];
        if (!t.alive || t.upgradePending) return;
        (void)dy;   // 炮台只在底部左右移动
        t.mvx = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);   // 记录方向，tick 统一移动
    }

    void set_angle(int pi, double ang) {
        if (pi < 0 || pi >= nPlayers) return;
        Turret &t = turrets[pi];
        if (!t.alive) return;
        t.angle = ang;
    }
    // 切换开火模式：0 自动开火 1 手动瞄准
    void set_fire_mode(int pi, int m) {
        if (pi < 0 || pi >= nPlayers) return;
        turrets[pi].fireMode = (m == 1) ? 1 : 0;
    }
    void set_paused(bool p) { paused = p; }
    // 自动模式：炮口自动转向最接近防线的怪
    void aim_nearest(int pi) {
        Turret &t = turrets[pi];
        if (!t.alive) return;
        double bestY = -1e9; double tx = 0, ty = 0; bool found = false;
        for (auto &m : monsters) {
            if (!m.alive) continue;
            if (m.y > bestY) { bestY = m.y; tx = m.x; ty = m.y; found = true; }
        }
        if (found) t.angle = std::atan2(ty - t.y, tx - t.x);
    }
    // 手动模式：按下发射（tick 内按攻速持续射）
    void fire_manual(int pi) {
        if (pi < 0 || pi >= nPlayers) return;
        Turret &t = turrets[pi];
        if (!t.alive || t.upgradePending || t.fireMode != 1) return;
        t.firing = 1;
    }
    // 手动模式：松开发射
    void fire_manual_off(int pi) {
        if (pi < 0 || pi >= nPlayers) return;
        turrets[pi].firing = 0;
    }

    // 炮台受伤
    void hurt_turret(int pi) {
        if (pi < 0 || pi >= nPlayers) return;
        Turret &t = turrets[pi];
        if (!t.alive) return;
        t.hp--;
        if (t.hp <= 0) { t.alive = false; }
    }

    // 漏怪计数
    void leak() {
        wave.leaked++;
        if (wave.leaked >= wave.maxLeak && !over) { over = true; result = 2; }
    }

    // 单 tick 推进，返回是否结束
    bool tick() {
        if (over) return true;
        if (paused) return false;
        // 升级待选：自己暂时不能移动/开火（炮台循环里已跳过该玩家），游戏世界不暂停，其他玩家与怪继续
        // 超时未选择 → 自动随机选一个，避免永久冻结卡死
        for (int i = 0; i < nPlayers; i++) {
            if (turrets[i].alive && turrets[i].upgradePending) {
                if (--turrets[i].upgradeTimer <= 0) apply_choice(i, std::rand() % 3);
            }
        }
        // ---- 波次推进 ----
        if (wave.phase == 0) {   // 波间休息
            wave.restTimer--;
            if (wave.restTimer <= 0) { begin_wave(); }
        } else if (wave.phase == 1) {
            wave.spawnTimer++;
            if (wave.spawnTimer >= wave.spawnGap && wave.spawned < wave.toSpawn) {
                wave.spawnTimer = 0;
                spawn_monster();
                wave.spawned++;
            }
            if (wave.spawned >= wave.toSpawn && monsters.empty()) {
                wave.phase = 0;
                wave.restTimer = 100;   // 4 秒休息
                if (wave.wave >= wave.totalWaves) begin_wave();   // 触发胜利判定
            }
        }
        // ---- 炮台（移动 + 射击，全部 tick 驱动，和状态推送同步） ----
        for (int i = 0; i < nPlayers; i++) {
            Turret &t = turrets[i];
            if (!t.alive || t.upgradePending) continue;
            // 移动：方向由 set_move 记录，这里统一推进
            if (t.mvx != 0) {
                double nx = t.x + t.mvx * 0.13;
                t.x = std::max(0.7, std::min(W - 0.7, nx));
            }
            // 换弹计时
            if (t.reloadT > 0) {
                t.reloadT--;
                if (t.reloadT <= 0) t.mag = t.magSize;   // 换弹完成回满
                continue;
            }
            if (t.fireMode == 0) {          // 自动开火：玩家自己瞄准角度，自动连射
                if (t.fireCd > 0) t.fireCd--;
                else if (t.mag > 0) {
                    fire(t, i);
                    t.fireCd = (int)std::round(1.0 / t.atkSpd / TICK);
                } else {
                    t.reloadT = t.reloadTime;   // 没子弹立刻换弹
                }
            } else {                        // 手动瞄准：按住发射（firing）才射
                if (t.fireCd > 0) t.fireCd--;
                if (t.firing && t.mag > 0 && t.fireCd <= 0) {
                    fire(t, i);
                    t.fireCd = (int)std::round(1.0 / t.atkSpd / TICK);
                }
                if (t.mag <= 0) t.reloadT = t.reloadTime;
            }
        }
        // ---- 子弹移动 + 碰撞 ----
        for (auto &b : bullets) if (b.alive) {
            b.x += b.vx; b.y += b.vy;
            if (b.x < 0 || b.x > W || b.y < -1 || b.y > H + 1) { b.alive = false; continue; }
            for (auto &m : monsters) if (m.alive) {
                double dx = m.x - b.x, dy = m.y - b.y;
                if (dx * dx + dy * dy < 0.42 * 0.42) {
                    // 穿透：同一子弹不重复命中同一怪
                    if (b.pierce > 0 && (m.hitBy[b.owner] & (1 << (b.pierce & 31)))) continue;
                    m.hp -= b.dmg;
                    // 命中特效（减速/点燃/中毒）
                    if (b.slowMul < 1.0) {
                        m.slowMul = std::min(m.slowMul, b.slowMul);
                        m.slowT = std::max(m.slowT, b.slowT);
                    }
                    if (b.burnDps > 0) { m.burnDps = b.burnDps; m.burnT = std::max(m.burnT, b.burnT); }
                    if (b.poisonDps > 0) { m.poisonDps += b.poisonDps; m.poisonT = std::max(m.poisonT, b.poisonT); }
                    if (b.pierce > 0) {
                        m.hitBy[b.owner] |= (1 << (b.pierce & 31));
                        b.pierce--;
                        if (b.pierce <= 0) { b.alive = false; }
                        // 穿透子弹仍留在场上
                        if (b.pierce > 0) b.dmg *= 0.8;
                        break;
                    } else {
                        b.alive = false;
                        break;
                    }
                }
            }
        }
        // ---- 怪移动 + DOT + 漏怪/碰撞炮台 ----
        for (size_t i = 0; i < monsters.size();) {
            Monster &m = monsters[i];
            if (!m.alive) { i++; continue; }
            // DOT
            if (m.burnT > 0) { m.hp -= m.burnDps; m.burnT--; }
            if (m.poisonT > 0) { m.hp -= m.poisonDps; m.poisonT--; }
            double spd = m.baseSpeed;
            if (m.slowT > 0) { spd *= m.slowMul; m.slowT--; }
            m.y += spd;
            // 漏怪
            if (m.y >= LEAK_Y) {
                m.alive = false;
                leak();
                i++;
                continue;
            }
            // 碰到炮台（贴身）→ 炮台受伤，怪消失
            bool hitTurret = false;
            for (int p = 0; p < nPlayers && !hitTurret; p++) {
                if (!turrets[p].alive) continue;
                double dx = m.x - turrets[p].x, dy = m.y - turrets[p].y;
                if (dx * dx + dy * dy < 0.7 * 0.7) {
                    hurt_turret(p);
                    m.alive = false;
                    hitTurret = true;
                }
            }
            if (hitTurret) { i++; continue; }
            // 死亡 → 经验（给最近的存活炮台）
            if (m.hp <= 0) {
                m.alive = false;
                int bp = 0; double best = 1e9;
                for (int p = 0; p < nPlayers; p++) {
                    if (!turrets[p].alive) continue;
                    double d = std::hypot(turrets[p].x - m.x, turrets[p].y - m.y);
                    if (d < best) { best = d; bp = p; }
                }
                gain_exp(&turrets[bp], m.exp);
                i++;
                continue;
            }
            i++;
        }
        // 移除死亡怪
        monsters.erase(std::remove_if(monsters.begin(), monsters.end(),
            [](const Monster &m) { return !m.alive; }), monsters.end());
        // 移除死亡子弹
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
            [](const Bullet &b) { return !b.alive; }), bullets.end());
        // 全部炮台阵亡 → 失败
        bool anyAlive = false;
        for (int i = 0; i < nPlayers; i++) if (turrets[i].alive) anyAlive = true;
        if (!anyAlive && !over) { over = true; result = 2; }
        return over;
    }

    // 怪死亡归属：由最近存活炮台获得经验（简化，联机时也公平）

    // 发射子弹
    void fire(Turret &t, int pi) {
        if (t.mag <= 0 || t.reloadT > 0) return;
        double spd = 0.55;
        for (int s = 0; s < t.spread; s++) {
            Bullet b;
            double ang = t.angle;
            if (t.spread > 1) {
                // 扇形散射：主角度 ± 均匀分布
                double off = (s - (t.spread - 1) / 2.0) * 0.10;
                ang += off;
            }
            b.x = t.x + std::cos(ang) * 0.4;
            b.y = t.y + std::sin(ang) * 0.4;
            b.vx = std::cos(ang) * spd;
            b.vy = std::sin(ang) * spd;
            b.owner = pi;
            b.dmg = 20.0 * t.dmgMul;
            b.pierce = t.pierce;
            b.slowMul = t.slowMul;
            b.slowT = t.slowMul < 1.0 ? 25 : 0;
            b.burnDps = t.burnDps; b.burnT = t.burnT;
            b.poisonDps = t.poisonDps; b.poisonT = t.poisonT;
            b.alive = true;
            bullets.push_back(b);
        }
        t.mag--;
        if (t.mag <= 0) t.reloadT = t.reloadTime;   // 打空自动换弹
    }

    // 加经验 → 升级
    void gain_exp(Turret *t, int e) {
        if (!t || !t->alive) return;
        t->exp += e;
        while (t->exp >= t->expNeed && !t->upgradePending) {
            t->exp -= t->expNeed;
            t->level++;
            t->expNeed = (long long)(t->expNeed * 1.35 + 8);
            t->upgradePending = true;
            t->upgradeTimer = 750;   // 30 秒内未选择 → 自动随机选（给足时间，避免"还没选就开始动"）
            roll_choices(*t);
        }
    }

    // 随机生成 3 个加成
    void roll_choices(Turret &t) {
        int pool[9] = { B_DMG, B_ATKSPD, B_PIERCE, B_SLOW, B_BURN, B_POISON, B_SPREAD, B_HEAL, B_MAG };
        // 洗牌取前 3（简单 Fisher-Yates）
        for (int i = 8; i > 0; i--) {
            int j = std::rand() % (i + 1);
            std::swap(pool[i], pool[j]);
        }
        t.choice[0] = pool[0];
        t.choice[1] = pool[1];
        t.choice[2] = pool[2];
    }

    // 应用三选一加成
    void apply_choice(int pi, int buffIdx) {
        if (pi < 0 || pi >= nPlayers) return;
        Turret &t = turrets[pi];
        if (!t.upgradePending) return;
        if (buffIdx < 0 || buffIdx > 2) { t.upgradePending = false; return; }
        int b = t.choice[buffIdx];
        switch (b) {
            case B_DMG:    t.dmgMul *= 1.5; break;
            case B_ATKSPD: t.atkSpd *= 1.4; break;
            case B_PIERCE: t.pierce++; break;
            case B_SLOW:   t.slowMul = (t.slowMul < 1.0 ? t.slowMul : 1.0) * 0.55; break;
            case B_BURN:   t.burnDps += 0.9; t.burnT = 50; break;
            case B_POISON: t.poisonDps += 0.7; t.poisonT = 75; break;
            case B_SPREAD: t.spread++; break;
            case B_HEAL:   t.hp = std::min(t.maxhp, t.hp + 2); break;
            case B_MAG:    t.magSize += 6; t.mag += 6; break;   // 弹夹容量 +6 并补满
        }
        t.upgradePending = false;
    }

    // ---- 序列化 ----
    std::string dump_state() const {
        std::string s;
        char buf[128];
        // 炮台
        for (int i = 0; i < nPlayers; i++) {
            const Turret &t = turrets[i];
            snprintf(buf, sizeof(buf), "%s%.2f,%.2f,%.3f,%d,%d,%lld,%lld,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%.0f",
                i ? ";" : "", t.x, t.y, t.angle, t.alive ? 1 : 0, t.level,
                (long long)t.exp, (long long)t.expNeed, t.dmgMul, t.atkSpd,
                t.pierce, t.hp, t.maxhp, t.upgradePending ? 1 : 0, t.spread,
                t.mag, t.magSize, t.reloadT);
            s += buf;
        }
        s += "|";
        // 波次
        snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d", wave.wave, wave.totalWaves, wave.leaked, wave.maxLeak, wave.toSpawn);
        s += buf;
        s += "|";
        // 怪
        for (size_t i = 0; i < monsters.size(); i++) {
            const Monster &m = monsters[i];
            snprintf(buf, sizeof(buf), "%s%.2f,%.2f,%.1f,%.1f,%d,%d,%.3f,%.3f,%.1f,%.1f,%.3f,%lld",
                i ? ";" : "", m.x, m.y, m.hp, m.maxhp, m.type, m.exp,
                m.slowT, m.slowMul, m.burnT, m.poisonT, m.baseSpeed, m.id);
            s += buf;
        }
        s += "|";
        // 子弹
        for (size_t i = 0; i < bullets.size(); i++) {
            const Bullet &b = bullets[i];
            snprintf(buf, sizeof(buf), "%s%.2f,%.2f,%.3f,%.3f,%d,%.1f,%d",
                i ? ";" : "", b.x, b.y, b.vx, b.vy, b.owner, b.dmg, b.pierce);
            s += buf;
        }
        s += "|";
        // 升级选择
        for (int i = 0; i < nPlayers; i++) {
            const Turret &t = turrets[i];
            snprintf(buf, sizeof(buf), "%s%d,%d,%d,%d", i ? ";" : "",
                t.upgradePending ? 1 : 0, t.choice[0], t.choice[1], t.choice[2]);
            s += buf;
        }
        return s;
    }
};

} // namespace gun
#endif
