// ============================================================
// chess_engine.h - 中国象棋引擎（C++ 实现，服务端权威规则）
// 对应前端 xiangqi.html 中已验证的走法逻辑
// 棋盘坐标：b[y][x]，x=0..8（列），y=0..9（行）
//   y=0 顶部为黑方，y=9 底部为红方，红先走
// ============================================================
#pragma once
#include <vector>
#include <cstring>
#include <algorithm>

namespace xq {

enum Side { RED = 0, BLACK = 1 };
enum PType { KING, ADVISOR, ELEPHANT, HORSE, ROOK, CANNON, PAWN, NONE };

struct Piece {
    Side side;
    PType type;
};

inline Piece none_piece() { return { RED, NONE }; }

struct Move {
    int fx, fy, tx, ty;
    bool operator==(const Move& o) const {
        return fx == o.fx && fy == o.fy && tx == o.tx && ty == o.ty;
    }
};

class Engine {
public:
    Piece b[10][9];
    Side turn = RED;
    bool game_over = false;

    void reset() {
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 9; x++) b[y][x] = none_piece();
        const PType back[9] = { ROOK, HORSE, ELEPHANT, ADVISOR, KING,
                                ADVISOR, ELEPHANT, HORSE, ROOK };
        for (int x = 0; x < 9; x++) {
            b[0][x] = { BLACK, back[x] };
            b[9][x] = { RED, back[x] };
        }
        b[2][1] = { BLACK, CANNON }; b[2][7] = { BLACK, CANNON };
        b[7][1] = { RED, CANNON };   b[7][7] = { RED, CANNON };
        const int px[5] = { 0, 2, 4, 6, 8 };
        for (int i = 0; i < 5; i++) {
            b[3][px[i]] = { BLACK, PAWN };
            b[6][px[i]] = { RED, PAWN };
        }
        turn = RED;
        game_over = false;
    }

    bool inBoard(int x, int y) const { return x >= 0 && x < 9 && y >= 0 && y < 10; }

    // ---- 攻击判定 ----
    static bool attackCellOn(const Piece bd[10][9], int x, int y, int tx, int ty) {
        const Piece& p = bd[y][x];
        Side side = p.side;
        int dx = tx - x, dy = ty - y;
        switch (p.type) {
            case ROOK: {
                if (x == tx) {
                    int d = ty > y ? 1 : -1;
                    for (int yy = y + d; yy != ty; yy += d) if (bd[yy][x].type != NONE) return false;
                    return true;
                }
                if (y == ty) {
                    int d = tx > x ? 1 : -1;
                    for (int xx = x + d; xx != tx; xx += d) if (bd[y][xx].type != NONE) return false;
                    return true;
                }
                return false;
            }
            case CANNON: {
                if (x == tx) {
                    int cnt = 0, d = ty > y ? 1 : -1;
                    for (int yy = y + d; yy != ty; yy += d) if (bd[yy][x].type != NONE) cnt++;
                    return cnt == 1;
                }
                if (y == ty) {
                    int cnt = 0, d = tx > x ? 1 : -1;
                    for (int xx = x + d; xx != tx; xx += d) if (bd[y][xx].type != NONE) cnt++;
                    return cnt == 1;
                }
                return false;
            }
            case HORSE: {
                if (dx * dx + dy * dy == 5) {
                    // 横跳看纵向腿
                    if (dx * dx == 4) return bd[y][x + (dx > 0 ? 1 : -1)].type == NONE;
                    // 纵跳看横向腿
                    if (dy * dy == 4) return bd[y + (dy > 0 ? 1 : -1)][x].type == NONE;
                }
                return false;
            }
            case ELEPHANT: {
                if (dx * dx == 4 && dy * dy == 4 &&
                    bd[y + (dy > 0 ? 1 : -1)][x + (dx > 0 ? 1 : -1)].type == NONE) {
                    if (side == RED && ty >= 5) return true;
                    if (side == BLACK && ty <= 4) return true;
                }
                return false;
            }
            case ADVISOR: {
                if (dx * dx == 1 && dy * dy == 1 && tx >= 3 && tx <= 5) {
                    if (side == RED && ty >= 7 && ty <= 9) return true;
                    if (side == BLACK && ty >= 0 && ty <= 2) return true;
                }
                return false;
            }
            case KING: {
                if (dx * dx + dy * dy == 1 && tx >= 3 && tx <= 5) {
                    if (side == RED && ty >= 7 && ty <= 9) return true;
                    if (side == BLACK && ty >= 0 && ty <= 2) return true;
                }
                // 飞将：双方将帅同列无遮挡直接相对
                if (x == tx && ty != y) {
                    bool blocked = false;
                    int d = ty > y ? 1 : -1;
                    for (int yy = y + d; yy != ty; yy += d)
                        if (bd[yy][x].type != NONE) { blocked = true; break; }
                    if (!blocked) {
                        const Piece& t = bd[ty][tx];
                        if (t.type == KING && t.side != side) return true;
                    }
                }
                return false;
            }
            case PAWN: {
                if (side == RED) {
                    if (ty == y - 1 && tx == x) return true;
                    if (y <= 4 && ty == y && (tx == x - 1 || tx == x + 1)) return true;
                } else {
                    if (ty == y + 1 && tx == x) return true;
                    if (y >= 5 && ty == y && (tx == x - 1 || tx == x + 1)) return true;
                }
                return false;
            }
            default: return false;
        }
    }

    static bool isAttackedOn(const Piece bd[10][9], Side s, int tx, int ty) {
        Side enemy = s == RED ? BLACK : RED;
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 9; x++) {
                const Piece& p = bd[y][x];
                if (p.type == NONE || p.side != enemy) continue;
                if (attackCellOn(bd, x, y, tx, ty)) return true;
            }
        return false;
    }

    bool isAttacked(int tx, int ty, Side s) const { return isAttackedOn(b, s, tx, ty); }

    static bool inCheckOn(const Piece bd[10][9], Side s) {
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 9; x++) {
                const Piece& p = bd[y][x];
                if (p.type == KING && p.side == s) return isAttackedOn(bd, s, x, y);
            }
        return false;
    }

    bool inCheck(Side s) const { return inCheckOn(b, s); }

    // ---- 走法生成（原始，不含将军过滤） ----
    std::vector<Move> genRaw(int x, int y) const {
        std::vector<Move> mv;
        const Piece& p = b[y][x];
        if (p.type == NONE) return mv;
        Side side = p.side;
        auto add = [&](int nx, int ny) {
            if (inBoard(nx, ny)) mv.push_back({ x, y, nx, ny });
        };
        switch (p.type) {
            case ROOK:
                for (int d = -1; d <= 1; d += 2) {
                    for (int s = 1; s < 9; s++) {
                        int xx = x + d * s;
                        if (!inBoard(xx, y)) break;
                        if (b[y][xx].type != NONE) { if (b[y][xx].side != side) add(xx, y); break; }
                        add(xx, y);
                    }
                    for (int s = 1; s < 9; s++) {
                        int yy = y + d * s;
                        if (!inBoard(x, yy)) break;
                        if (b[yy][x].type != NONE) { if (b[yy][x].side != side) add(x, yy); break; }
                        add(x, yy);
                    }
                }
                break;
            case CANNON:
                for (int d = -1; d <= 1; d += 2) {
                    for (int s = 1; s < 9; s++) {
                        int xx = x + d * s;
                        if (!inBoard(xx, y)) break;
                        if (b[y][xx].type != NONE) {
                            for (int s2 = s + 1; s2 < 9; s2++) {
                                int xx2 = x + d * s2;
                                if (!inBoard(xx2, y)) break;
                                if (b[y][xx2].type != NONE) {
                                    if (b[y][xx2].side != side) add(xx2, y);
                                    break;
                                }
                            }
                            break;
                        }
                        add(xx, y);
                    }
                    for (int s = 1; s < 9; s++) {
                        int yy = y + d * s;
                        if (!inBoard(x, yy)) break;
                        if (b[yy][x].type != NONE) {
                            for (int s2 = s + 1; s2 < 9; s2++) {
                                int yy2 = y + d * s2;
                                if (!inBoard(x, yy2)) break;
                                if (b[yy2][x].type != NONE) {
                                    if (b[yy2][x].side != side) add(x, yy2);
                                    break;
                                }
                            }
                            break;
                        }
                        add(x, yy);
                    }
                }
                break;
            case HORSE: {
                const int hdx[8] = { -2, -2, -1, -1, 1, 1, 2, 2 };
                const int hdy[8] = { -1, 1, -2, 2, -2, 2, -1, 1 };
                for (int i = 0; i < 8; i++) {
                    int nx = x + hdx[i], ny = y + hdy[i];
                    if (!inBoard(nx, ny)) continue;
                    if (hdx[i] * hdx[i] == 4) {
                        if (b[y][x + (hdx[i] > 0 ? 1 : -1)].type != NONE) continue;
                    } else {
                        if (b[y + (hdy[i] > 0 ? 1 : -1)][x].type != NONE) continue;
                    }
                    if (b[ny][nx].type == NONE || b[ny][nx].side != side) add(nx, ny);
                }
                break;
            }
            case ELEPHANT: {
                const int edx[4] = { -2, -2, 2, 2 };
                const int edy[4] = { -2, 2, -2, 2 };
                for (int i = 0; i < 4; i++) {
                    int nx = x + edx[i], ny = y + edy[i];
                    if (!inBoard(nx, ny)) continue;
                    if (b[y + (edy[i] > 0 ? 1 : -1)][x + (edx[i] > 0 ? 1 : -1)].type != NONE) continue;
                    if (side == RED && ny < 5) continue;
                    if (side == BLACK && ny > 4) continue;
                    if (b[ny][nx].type == NONE || b[ny][nx].side != side) add(nx, ny);
                }
                break;
            }
            case ADVISOR: {
                const int adx[4] = { -1, -1, 1, 1 };
                const int ady[4] = { -1, 1, -1, 1 };
                for (int i = 0; i < 4; i++) {
                    int nx = x + adx[i], ny = y + ady[i];
                    if (nx < 3 || nx > 5) continue;
                    if (side == RED && (ny < 7 || ny > 9)) continue;
                    if (side == BLACK && (ny < 0 || ny > 2)) continue;
                    if (b[ny][nx].type == NONE || b[ny][nx].side != side) add(nx, ny);
                }
                break;
            }
            case KING: {
                const int kdx[4] = { -1, 1, 0, 0 };
                const int kdy[4] = { 0, 0, -1, 1 };
                for (int i = 0; i < 4; i++) {
                    int nx = x + kdx[i], ny = y + kdy[i];
                    if (nx < 3 || nx > 5) continue;
                    if (side == RED && (ny < 7 || ny > 9)) continue;
                    if (side == BLACK && (ny < 0 || ny > 2)) continue;
                    if (b[ny][nx].type == NONE || b[ny][nx].side != side) add(nx, ny);
                }
                break;
            }
            case PAWN:
                if (side == RED) {
                    if (y - 1 >= 0 && (b[y - 1][x].type == NONE || b[y - 1][x].side != side)) add(x, y - 1);
                    if (y <= 4) {
                        if (x - 1 >= 0 && (b[y][x - 1].type == NONE || b[y][x - 1].side != side)) add(x - 1, y);
                        if (x + 1 < 9 && (b[y][x + 1].type == NONE || b[y][x + 1].side != side)) add(x + 1, y);
                    }
                } else {
                    if (y + 1 < 10 && (b[y + 1][x].type == NONE || b[y + 1][x].side != side)) add(x, y + 1);
                    if (y >= 5) {
                        if (x - 1 >= 0 && (b[y][x - 1].type == NONE || b[y][x - 1].side != side)) add(x - 1, y);
                        if (x + 1 < 9 && (b[y][x + 1].type == NONE || b[y][x + 1].side != side)) add(x + 1, y);
                    }
                }
                break;
            default: break;
        }
        return mv;
    }

    // ---- 合法走法（过滤走后自己被将军） ----
    std::vector<Move> genLegal(int x, int y) const {
        std::vector<Move> res;
        const Piece& p = b[y][x];
        if (p.type == NONE) return res;
        Side s = p.side;
        auto raw = genRaw(x, y);
        Piece tmp[10][9];
        for (auto& m : raw) {
            std::memcpy(tmp, b, sizeof(b));
            tmp[m.ty][m.tx] = tmp[m.fy][m.fx];
            tmp[m.fy][m.fx] = none_piece();
            if (!inCheckOn(tmp, s)) res.push_back(m);
        }
        return res;
    }

    bool hasLegalMove(Side s) const {
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 9; x++) {
                const Piece& p = b[y][x];
                if (p.type != NONE && p.side == s && !genLegal(x, y).empty()) return true;
            }
        return false;
    }

    bool isLegalMove(int fx, int fy, int tx, int ty) const {
        if (!inBoard(fx, fy) || !inBoard(tx, ty)) return false;
        auto mv = genLegal(fx, fy);
        return std::find(mv.begin(), mv.end(), Move{ fx, fy, tx, ty }) != mv.end();
    }

    // 直接执行走子（调用前应 isLegalMove）
    void makeMove(int fx, int fy, int tx, int ty) {
        b[ty][tx] = b[fy][fx];
        b[fy][fx] = none_piece();
        // 换边并检查对局是否结束
        turn = turn == RED ? BLACK : RED;
        if (!hasLegalMove(turn)) game_over = true;
    }
};

} // namespace xq
