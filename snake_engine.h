#ifndef SNAKE_ENGINE_H
#define SNAKE_ENGINE_H
// 贪吃蛇核心引擎（C++17）
// 服务端权威：蛇身 / 方向 / 食物 / 计分 / 加速 / 碰撞全部在此计算，
// 前端只负责渲染服务端推送的状态 + 上报方向指令。
#include <string>
#include <deque>
#include <cstdlib>
#include <cstdint>

namespace snk {

const int GRID = 20;   // 20 x 20 网格

struct Pt { int x, y; };

class Engine {
public:
    std::deque<Pt> body;   // 蛇身，头在前
    Pt food;
    int dx = 1, dy = 0;    // 当前方向
    int ndx = 1, ndy = 0;  // 待生效方向（本 tick 内生效）
    int score = 0;
    bool running = false;
    bool paused = false;
    long speed_ms = 160;       // 当前 tick 间隔
    long long last_tick = 0;   // 上次 tick 的毫秒时间戳（外部注入）
    bool boost = false;        // 加速中（每 tick 走 2 格）
    int boost_accum = 0;       // 加速累计格数（每 7 格长度减 1）

    void init(long long now_ms) {
        body.clear();
        body.push_back({10, 10});
        body.push_back({9, 10});
        body.push_back({8, 10});
        dx = 1; dy = 0; ndx = 1; ndy = 0;
        score = 0; running = true; paused = false; speed_ms = 160;
        boost = false; boost_accum = 0;
        last_tick = now_ms;
        place_food();
    }

    void set_dir(int x, int y) {
        // 禁止原地掉头；相同方向忽略
        if (x == -dx && y == -dy) return;
        if (x == dx && y == dy) return;
        ndx = x; ndy = y;
    }

    bool has_body(int x, int y) const {
        for (const Pt& p : body) if (p.x == x && p.y == y) return true;
        return false;
    }

    void place_food() {
        do {
            food = { std::rand() % GRID, std::rand() % GRID };
        } while (has_body(food.x, food.y));
    }

    // 推进一帧。返回 0 = 存活，1 = 撞墙/撞自身 → 结束
    int tick() {
        if (!running || paused) return 0;
        dx = ndx; dy = ndy;
        Pt h = { body.front().x + dx, body.front().y + dy };
        // 撞墙
        if (h.x < 0 || h.x >= GRID || h.y < 0 || h.y >= GRID) { running = false; return 1; }
        // 撞自身（尾巴即将移开，不计）
        for (size_t i = 0; i + 1 < body.size(); ++i)
            if (body[i].x == h.x && body[i].y == h.y) { running = false; return 1; }
        body.push_front(h);
        if (h.x == food.x && h.y == food.y) {
            score++;
            place_food();
            // 吃豆加速
            if (speed_ms > 70) { speed_ms -= 6; if (speed_ms < 70) speed_ms = 70; }
        } else {
            body.pop_back();
        }
        // ===== 加速步进：每 tick 额外走 1 格（代价累计 7 格长度减 1）=====
        if (boost && running && !paused) {
            Pt b = { body.front().x + dx, body.front().y + dy };
            if (b.x < 0 || b.x >= GRID || b.y < 0 || b.y >= GRID) { running = false; return 1; }
            bool bhit = false;
            for (size_t i = 0; i + 1 < body.size(); ++i)
                if (body[i].x == b.x && body[i].y == b.y) { bhit = true; break; }
            if (bhit) { running = false; return 1; }
            body.push_front(b);
            if (b.x == food.x && b.y == food.y) {
                score++; place_food();
                if (speed_ms > 70) { speed_ms -= 6; if (speed_ms < 70) speed_ms = 70; }
            } else {
                body.pop_back();
            }
            boost_accum += 2;   // 加速状态下每 tick 走 2 格，全部计入代价（累计结转）
            if (boost_accum >= 20) { boost_accum -= 20; if (body.size() > 1) body.pop_back(); }
        }
        return 0;
    }

    // 蛇身序列化：x,y;x,y;...（头在前）
    std::string dump() const {
        std::string s;
        for (size_t i = 0; i < body.size(); ++i) {
            if (i) s += ";";
            s += std::to_string(body[i].x) + "," + std::to_string(body[i].y);
        }
        return s;
    }
};

// ==================== 双人联机贪吃蛇（PVP） ====================
// 规则（服务器权威判定）：
//   1. 两条蛇同场争抢同一个食物
//   2. 蛇头撞对方身体 → 撞的一方死
//   3. 两蛇头相撞（同格或相向互换）→ 长度大的不死，小的死；等长 → 平局
class PVPEngine {
public:
    std::deque<Pt> ba, bb;            // A / B 蛇身（头在前）
    std::deque<Pt> foods;             // 多个食物（创建房间可设置数量）
    int food_count=1;
    int dxa=1,dya=0, ndxa=1,ndya=0;   // A 初始向右
    int dxb=-1,dyb=0, ndxb=-1,ndyb=0; // B 初始向左
    int scoreA=0, scoreB=0;
    bool running=false, paused=false;
    int result=0;                     // 0=进行中 1=A胜 2=B胜 3=平局
    long speed_ms=160;
    long long last_tick=0;
    bool boostA=false, boostB=false;   // 各自加速中（每 tick 多走 1 格）
    int accumA=0, accumB=0;            // 各自加速累计格数（每 7 格长度减 1）

    void init(long long now, int nfood=1, long spd=160) {
        ba.clear(); bb.clear(); foods.clear();
        ba.push_back({10,10}); ba.push_back({9,10}); ba.push_back({8,10});
        bb.push_back({9,9});  bb.push_back({10,9}); bb.push_back({11,9});
        dxa=1;dya=0;ndxa=1;ndya=0;
        dxb=-1;dyb=0;ndxb=-1;ndyb=0;
        scoreA=scoreB=0; running=true; paused=false; result=0;
        boostA=false; boostB=false; accumA=0; accumB=0;
        food_count=nfood<1?1:nfood;
        speed_ms=spd<70?70:(spd>500?500:spd);
        last_tick=now;
        for(int i=0;i<food_count;i++)place_food();
    }
    void set_dirA(int x,int y){ if(x==-dxa&&y==-dya)return; if(x==dxa&&y==dya)return; ndxa=x;ndya=y; }
    void set_dirB(int x,int y){ if(x==-dxb&&y==-dyb)return; if(x==dxb&&y==dyb)return; ndxb=x;ndyb=y; }
    bool in_body(const std::deque<Pt>& b,int x,int y) const {
        for(const Pt&p:b) if(p.x==x&&p.y==y) return true; return false;
    }
    // 是否在任意蛇身上（生成食物避开）
    bool on_any_body(int x,int y) const {
        return in_body(ba,x,y)||in_body(bb,x,y);
    }
    bool on_any_food(int x,int y) const {
        for(const Pt&f:foods) if(f.x==x&&f.y==y) return true; return false;
    }
    void place_food(){
        Pt f;
        int guard=0;
        do { f={std::rand()%GRID,std::rand()%GRID}; guard++; if(guard>300)break; }
        while(on_any_body(f.x,f.y)||on_any_food(f.x,f.y));
        foods.push_back(f);
    }
    // p 是否撞 b 身体（排除即将移开的尾巴）
    static bool hit_body(const std::deque<Pt>& b,const Pt&p){
        for(size_t i=0;i+1<b.size();++i)
            if(b[i].x==p.x&&b[i].y==p.y) return true;
        return false;
    }
    // 返回 1=本局结束
    int tick(){
        if(!running||paused) return 0;
        dxa=ndxa; dya=ndya; dxb=ndxb; dyb=ndyb;
        Pt a2={ba.front().x+dxa, ba.front().y+dya};
        Pt b2={bb.front().x+dxb, bb.front().y+dyb};
        bool a_dead=false, b_dead=false;
        // 撞墙
        if(a2.x<0||a2.x>=GRID||a2.y<0||a2.y>=GRID) a_dead=true;
        if(b2.x<0||b2.x>=GRID||b2.y<0||b2.y>=GRID) b_dead=true;
        // 撞自己
        if(!a_dead&&hit_body(ba,a2)) a_dead=true;
        if(!b_dead&&hit_body(bb,b2)) b_dead=true;
        // 头对头（同格 或 相向互换）
        bool hh=false;
        if(!a_dead&&!b_dead){
            bool same = (a2.x==b2.x&&a2.y==b2.y);
            bool swap = (a2.x==bb.front().x&&a2.y==bb.front().y
                      && b2.x==ba.front().x&&b2.y==ba.front().y);
            hh = same || swap;
        }
        if(hh){
            if(ba.size()>bb.size()) b_dead=true;      // A 长，B 死
            else if(bb.size()>ba.size()) a_dead=true; // B 长，A 死
            else { a_dead=true; b_dead=true; result=3; } // 等长平局
        }
        // 撞对方身体（A 撞 B 身体 / B 撞 A 身体，排除尾巴）
        if(!a_dead&&!b_dead){
            if(hit_body(bb,a2)) a_dead=true;
            if(hit_body(ba,b2)) b_dead=true;
        }
        // 移动（先都进一格）
        ba.push_front(a2);
        bb.push_front(b2);
        // 吃豆（检查多个食物）
        bool a_eat=false, b_eat=false;
        for(auto it=foods.begin(); it!=foods.end(); ){
            if(a2.x==it->x&&a2.y==it->y){ a_eat=true; scoreA++; it=foods.erase(it); }
            else if(b2.x==it->x&&b2.y==it->y){ b_eat=true; scoreB++; it=foods.erase(it); }
            else ++it;
        }
        if(a_eat||b_eat){ if(speed_ms>70)speed_ms-=3; place_food(); }
        if(!a_eat) ba.pop_back();   // 尾巴移动（吃到食物时尾巴保留）
        if(!b_eat) bb.pop_back();
        // 结算
        if(a_dead&&b_dead){ running=false; if(result!=3)result=3; return 1; }
        if(a_dead){ running=false; result=2; return 1; }
        if(b_dead){ running=false; result=1; return 1; }
        // ===== 加速步进：加速方每 tick 额外走 1 格（代价累计 7 格长度减 1）=====
        if(boostA && running && !paused){
            Pt a3={ba.front().x+dxa, ba.front().y+dya};
            if(a3.x<0||a3.x>=GRID||a3.y<0||a3.y>=GRID){ running=false; result=2; return 1; }
            if(hit_body(ba,a3)||hit_body(bb,a3)){ running=false; result=2; return 1; }
            ba.push_front(a3);
            bool aAte=false;
            for(auto it=foods.begin(); it!=foods.end(); ){
                if(a3.x==it->x&&a3.y==it->y){ scoreA++; aAte=true; it=foods.erase(it); }
                else ++it;
            }
            if(aAte){ if(speed_ms>70)speed_ms-=3; place_food(); } else ba.pop_back();
            accumA += 2;   // 加速状态下每 tick 走 2 格，全部计入代价（累计结转）
            if(accumA>=20){ accumA-=20; if(ba.size()>1)ba.pop_back(); }
        }
        if(boostB && running && !paused){
            Pt b3={bb.front().x+dxb, bb.front().y+dyb};
            if(b3.x<0||b3.x>=GRID||b3.y<0||b3.y>=GRID){ running=false; result=1; return 1; }
            if(hit_body(bb,b3)||hit_body(ba,b3)){ running=false; result=1; return 1; }
            bb.push_front(b3);
            bool bAte=false;
            for(auto it=foods.begin(); it!=foods.end(); ){
                if(b3.x==it->x&&b3.y==it->y){ scoreB++; bAte=true; it=foods.erase(it); }
                else ++it;
            }
            if(bAte){ if(speed_ms>70)speed_ms-=3; place_food(); } else bb.pop_back();
            accumB += 2;   // 加速状态下每 tick 走 2 格，全部计入代价（累计结转）
            if(accumB>=20){ accumB-=20; if(bb.size()>1)bb.pop_back(); }
        }
        return 0;
    }
    std::string dumpA() const { return dump(ba); }
    std::string dumpB() const { return dump(bb); }
    std::string dumpFoods() const {
        std::string s;
        for(size_t i=0;i<foods.size();++i){ if(i)s+=";"; s+=std::to_string(foods[i].x)+","+std::to_string(foods[i].y); }
        return s;
    }
    static std::string dump(const std::deque<Pt>& b){
        std::string s;
        for(size_t i=0;i<b.size();++i){ if(i)s+=";"; s+=std::to_string(b[i].x)+","+std::to_string(b[i].y); }
        return s;
    }
};

} // namespace snk
#endif

