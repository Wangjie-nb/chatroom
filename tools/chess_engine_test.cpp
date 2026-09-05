// chess_engine_test.cpp - C++ 象棋引擎测试（对照前端 JS 已通过的 17 个用例）
#include "chess_engine.h"
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace xq;

static int pass = 0, fail = 0;

static std::string sortMoves(std::vector<Move> mv) {
    std::sort(mv.begin(), mv.end(), [](const Move& a, const Move& b) {
        if (a.tx != b.tx) return a.tx < b.tx;
        return a.ty < b.ty;
    });
    std::string s;
    for (auto& m : mv) {
        if (!s.empty()) s += ",";
        s += "(" + std::to_string(m.tx) + "," + std::to_string(m.ty) + ")";
    }
    return s;
}

static bool hasMove(const std::vector<Move>& mv, int tx, int ty) {
    for (auto& m : mv) if (m.tx == tx && m.ty == ty) return true;
    return false;
}

static void eq(const char* name, const std::string& actual, const std::string& expected) {
    if (actual == expected) { pass++; std::printf("✅ %s\n", name); }
    else { fail++; std::printf("❌ %s\n   期望: %s\n   实际: %s\n", name, expected.c_str(), actual.c_str()); }
}
static void eqBool(const char* name, bool actual, bool expected) {
    if (actual == expected) { pass++; std::printf("✅ %s\n", name); }
    else { fail++; std::printf("❌ %s\n   期望: %s\n   实际: %s\n", name, expected ? "true" : "false", actual ? "true" : "false"); }
}

int main() {
    Engine e;
    e.reset();

    // 1. 红马(1,9)可跳(0,7)(2,7)
    eq("红马跳", sortMoves(e.genLegal(1, 9)), "(0,7),(2,7)");
    // 2. 红车(0,9)可到(0,8)(0,7)
    auto rm = e.genLegal(0, 9);
    eq("红车两步", std::to_string(rm.size()), "2");
    eqBool("红车到(0,8)", hasMove(rm, 0, 8), true);
    eqBool("红车到(0,7)", hasMove(rm, 0, 7), true);
    // 3. 红炮(1,7)可直走
    eqBool("红炮可直走", e.genLegal(1, 7).size() > 2, true);
    // 4. 红兵(0,6)前进
    eq("红兵前进", sortMoves(e.genLegal(0, 6)), "(0,5)");
    // 5. 红相(2,9)飞
    eq("红相飞", sortMoves(e.genLegal(2, 9)), "(0,7),(4,7)");
    // 6. 红帅(4,9)前进一步
    eq("红帅前进一步", sortMoves(e.genLegal(4, 9)), "(4,8)");
    // 7-8. 合法走法与将军
    eqBool("红有合法走法", e.hasLegalMove(RED), true);
    eqBool("初始红不将军", e.inCheck(RED), false);
    eqBool("初始黑不将军", e.inCheck(BLACK), false);

    // 9. 炮隔子吃：红炮(7,1)隔(5,1)吃(3,1)
    Engine e4;
    e4.reset();
    e4.b[2][1] = none_piece();
    e4.b[5][1] = { BLACK, PAWN };
    e4.b[3][1] = { BLACK, PAWN };
    auto cm = e4.genLegal(1, 7);  // x=1,y=7 红炮
    eqBool("炮隔子吃(1,3)", hasMove(cm, 1, 3), true);
    eqBool("炮不能空打(1,2)", hasMove(cm, 1, 2), false);

    // 10. 车不能越子
    eqBool("红车不能越兵(0,5)", hasMove(e.genLegal(0, 9), 0, 5), false);

    // 11. 马蹩腿
    Engine e5;
    e5.reset();
    e5.b[8][3] = { RED, HORSE };
    e5.b[8][2] = { RED, PAWN };
    auto hm = e5.genLegal(3, 8);
    eqBool("马左跳被蹩", hasMove(hm, 1, 7), false);
    eqBool("马右跳(5,7)可", hasMove(hm, 5, 7), true);

    // 12. 士斜走
    eq("红士斜走", sortMoves(e.genLegal(3, 9)), "(4,8)");

    // 13. 黑卒前进（向下）
    eq("黑卒前进", sortMoves(e.genLegal(0, 3)), "(0,4)");

    // 14. makeMove 后换边 + 对局推进
    Engine e6;
    e6.reset();
    e6.makeMove(0, 6, 0, 5);  // 红兵进
    eqBool("走子后轮到黑方", e6.turn == BLACK, true);
    eqBool("走子后黑有合法走法", e6.hasLegalMove(BLACK), true);

    // 15. isLegalMove 接口
    Engine e7;
    e7.reset();
    eqBool("isLegalMove 红兵前进合法", e7.isLegalMove(0, 6, 0, 5), true);
    eqBool("isLegalMove 红兵跳河非法", e7.isLegalMove(0, 6, 0, 4), false);

    std::printf("\n最终结果: %d 通过, %d 失败\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
